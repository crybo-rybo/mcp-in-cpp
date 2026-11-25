#include "http_channel.hpp"

#include "event_stream.hpp"
#include "jsonrpc.hpp"
#include "routing.hpp"
#include "verbatim.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <list>
#include <mcp/channel.hpp>
#include <mcp/client.hpp>
#include <mcp/context.hpp>
#include <mcp/http.hpp>
#include <mcp/http_client.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace mcp {

namespace {

using jsonrpc::join;
using scrydantic::JsonKind;
using scrydantic::JsonView;

// The most a response's body, or one event of a stream, may hold.
constexpr std::size_t most_body = std::size_t{64} << 20U;
// The most requests in flight at once, each with a thread of its own.
constexpr std::size_t most_in_flight = 256;
// How long closing waits for the server to end its session.
constexpr std::chrono::seconds ending_patience{5};
// How much of an error's body a fault's message quotes.
constexpr std::size_t most_quoted = 200;

[[nodiscard]] ClientError fault(const Fault kind, std::string message) {
  return {
      .fault = kind, .code = 0, .message = std::move(message), .data = std::nullopt};
}

[[nodiscard]] char lower(const char c) noexcept {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool iequals(const std::string_view a,
                           const std::string_view b) noexcept {
  return std::ranges::equal(
      a, b, [](const char x, const char y) { return lower(x) == lower(y); });
}

[[nodiscard]] bool istarts_with(const std::string_view text,
                                const std::string_view prefix) noexcept {
  return text.size() >= prefix.size() && iequals(text.substr(0, prefix.size()), prefix);
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

// Whether a header can carry the value without it ending the header early.
[[nodiscard]] bool header_safe(const std::string_view value) noexcept {
  return value.find_first_of(std::string_view{"\r\n\0", 3}) == std::string_view::npos;
}

[[nodiscard]] bool is_token(const std::string_view name) noexcept {
  constexpr std::string_view marks = "!#$%&'*+-.^_`|~";
  return !name.empty() && std::ranges::all_of(name, [marks](const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           marks.find(c) != std::string_view::npos;
  });
}

// Whether a session id is one the protocol allows: visible ASCII only.
[[nodiscard]] bool visible(const std::string_view text) noexcept {
  return !text.empty() &&
         std::ranges::all_of(text, [](const char c) { return c >= 0x21 && c <= 0x7E; });
}

[[nodiscard]] std::optional<std::string_view>
find_header(const std::span<const HttpHeader> headers, const std::string_view name) {
  for (const HttpHeader& header : headers) {
    if (iequals(header.name, name)) {
      return trim(header.value);
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool is_legacy(const std::string_view version) noexcept {
  return std::ranges::find(legacy_protocol_versions, version) !=
         std::end(legacy_protocol_versions);
}

// Whether the transport sets this header itself, so that one of the program's
// is left out.
[[nodiscard]] bool reserved(const std::string_view name, const bool bearer) noexcept {
  return iequals(name, "Content-Type") || iequals(name, "Accept") ||
         istarts_with(name, "Mcp-") || (bearer && iequals(name, "Authorization"));
}

// Whether a message is the response to the request with this id.
[[nodiscard]] bool answers(const std::string_view text, const std::string_view id) {
  if (id.empty() || !scrydantic::is_valid_json(text) ||
      verbatim::member(text, "method").has_value()) {
    return false;
  }
  const std::optional<std::string_view> found = verbatim::member(text, "id");
  return found == id;
}

// What a fault's message says of a body: the message of the JSON-RPC error it
// holds, or its text when that is short printable ASCII.
[[nodiscard]] std::string quoted(const std::string_view body) {
  if (const scrydantic::Result<JsonView> document = JsonView::parse(body);
      document && document->kind() == JsonKind::object) {
    const std::optional<JsonView> error = document->find("error");
    const std::optional<JsonView> message =
        error.has_value() ? error->find("message") : std::nullopt;
    if (message.has_value() && message->string().has_value()) {
      return join({": ", *message->string()});
    }
    return {};
  }
  const std::string_view text = trim(body);
  if (text.empty() || text.size() > most_quoted ||
      !std::ranges::all_of(text, [](const char c) { return c >= 0x20 && c <= 0x7E; })) {
    return {};
  }
  return join({": ", text});
}

// One message in flight: what its POST carries, and what stops it.
struct Exchange {
  // The request's id as JSON text, or empty for a notification or a response.
  std::string id;
  std::string method;
  // Whether it is sent under 2026-07-28.
  bool modern{};
  // Its place among the channel's messages. A notification or a response is
  // read by the server before any message sent after it.
  std::uint64_t order{};
  bool note{};
  // Whether it carried the session's id.
  bool in_session{};
  std::vector<HttpHeader> headers;
  std::string body;
  std::stop_source stop;
};

class HttpChannel;

// What the HTTP client hands over of one exchange's response.
class Receiver final : public HttpClientResponse {
public:
  Receiver(HttpChannel& channel, Exchange& exchange, Incoming& incoming)
      : channel_{&channel}, exchange_{&exchange}, incoming_{&incoming} {}

  void start(int status, std::span<const HttpHeader> headers) override;
  bool write(std::string_view data) override;

  // Hands one message to the client.
  void deliver(std::string_view text);

  int status{};
  bool stream{};
  bool answered{};
  bool overflowed{};
  std::string body;
  std::optional<std::string> challenge;

private:
  HttpChannel* channel_;
  Exchange* exchange_;
  Incoming* incoming_;
  EventStream events_{most_body};
};

// Ignores what it is given, for a DELETE whose answer does not matter.
class Discard final : public HttpClientResponse {
public:
  void start(int /*status*/, std::span<const HttpHeader> /*headers*/) override {}
  bool write(std::string_view /*data*/) override { return false; }
};

class HttpChannel final : public Channel {
public:
  HttpChannel(std::string url, HttpClient& http, HttpConnectOptions options)
      : url_{std::move(url)}, http_{&http}, options_{std::move(options)} {}

  HttpChannel(const HttpChannel&) = delete;
  HttpChannel& operator=(const HttpChannel&) = delete;

  ~HttpChannel() override { close(); }

  Reply<void> open(Incoming& incoming) override {
    incoming_ = &incoming;
    return {};
  }

  Reply<void> send(const Outgoing& message) override;
  bool abandon(std::string_view id) override;
  void close() override;

  // A status has come for this exchange, so the messages after it may go.
  void heard(const Exchange& exchange) {
    {
      const std::scoped_lock lock{mutex_};
      notes_.erase(exchange.order);
    }
    changed_.notify_all();
  }

  // The server's answer to `initialize` named the session.
  void adopt_session(const std::string_view id) {
    const std::scoped_lock lock{mutex_};
    session_ = id;
  }

private:
  struct Running {
    std::shared_ptr<Exchange> exchange;
    std::thread thread;
  };

  void run(Exchange& exchange);
  void settle(Exchange& exchange, Receiver& receiver, const Reply<void>& sent);
  void finished(std::list<Running>::iterator slot);
  // The headers every request has from the program, and its bearer token.
  // Fails when the token cannot be sent.
  Reply<void> add_own(std::vector<HttpHeader>& headers) const;
  void end_session(const std::string& session, const std::string& version);

  std::string url_;
  HttpClient* http_;
  HttpConnectOptions options_;
  Incoming* incoming_{};

  std::mutex mutex_;
  std::condition_variable_any changed_;
  bool closing_{};
  // The session of a handshake, and the revision it agreed, once known.
  std::string session_;
  std::string legacy_version_;
  // Whether the last message that named a revision named 2026-07-28.
  bool modern_{};
  std::uint64_t next_order_{};
  // The notifications and responses whose status has not come.
  std::set<std::uint64_t> notes_;
  std::list<Running> running_;
  std::vector<std::list<Running>::iterator> ended_;
};

void Receiver::start(const int code, const std::span<const HttpHeader> headers) {
  status = code;
  const bool success = code >= 200 && code < 300;
  const std::optional<std::string_view> type = find_header(headers, "Content-Type");
  stream = success && type.has_value() && istarts_with(*type, "text/event-stream");
  if (const std::optional<std::string_view> said =
          find_header(headers, "WWW-Authenticate")) {
    challenge = std::string{*said};
  }
  if (exchange_->method == "initialize" && success && !exchange_->modern) {
    // The session must be known before the answer reaches the client, which
    // sends its next message as soon as it has it.
    if (const std::optional<std::string_view> session =
            find_header(headers, "Mcp-Session-Id");
        session.has_value() && visible(*session)) {
      channel_->adopt_session(*session);
    }
  }
  channel_->heard(*exchange_);
}

bool Receiver::write(const std::string_view data) {
  if (answered || exchange_->stop.stop_requested()) {
    return false;
  }
  if (stream) {
    const bool read = events_.feed(data, [this](const EventStream::Event& event) {
      if (event.type == "message") {
        deliver(event.data);
      }
    });
    overflowed = !read;
    // Once the response has come, the rest of the stream is not needed.
    return read && !answered;
  }
  if (body.size() + data.size() > most_body) {
    overflowed = true;
    return false;
  }
  body += data;
  return true;
}

void Receiver::deliver(const std::string_view text) {
  if (trim(text).empty() || answered) {
    return;
  }
  answered = answers(text, exchange_->id);
  incoming_->message(text);
}

Reply<void> HttpChannel::send(const Outgoing& message) {
  auto exchange = std::make_shared<Exchange>();
  exchange->id = message.id;
  exchange->method = message.method;
  exchange->note = message.id.empty();
  exchange->modern =
      !message.protocol_version.empty() && !is_legacy(message.protocol_version);
  exchange->body = message.message;
  std::vector<HttpHeader>& headers = exchange->headers;
  headers.push_back({.name = "Content-Type", .value = "application/json"});
  headers.push_back({.name = "Accept", .value = "application/json, text/event-stream"});
  if (!message.protocol_version.empty()) {
    headers.push_back({.name = "MCP-Protocol-Version",
                       .value = std::string{message.protocol_version}});
  }
  if (exchange->modern && !message.method.empty()) {
    headers.push_back({.name = "Mcp-Method", .value = std::string{message.method}});
    if (!message.name.empty()) {
      headers.push_back(
          {.name = "Mcp-Name", .value = routing::header_value(message.name)});
    }
  }
  headers.insert(headers.end(), message.headers.begin(), message.headers.end());

  const std::scoped_lock lock{mutex_};
  if (closing_ || incoming_ == nullptr) {
    return std::unexpected(fault(Fault::transport, "the HTTP channel is closed"));
  }
  // A thread that has ended is joined before another starts, so that the
  // threads of a long session do not pile up.
  for (const auto ended : ended_) {
    ended->thread.join();
    running_.erase(ended);
  }
  ended_.clear();
  if (running_.size() >= most_in_flight) {
    return std::unexpected(
        fault(Fault::transport, join({"the client has ", std::to_string(most_in_flight),
                                      " HTTP requests in flight already"})));
  }
  exchange->order = next_order_++;
  if (exchange->note) {
    notes_.insert(exchange->order);
  }
  if (!message.protocol_version.empty()) {
    modern_ = exchange->modern;
    if (!exchange->modern) {
      legacy_version_ = message.protocol_version;
    }
  }
  const auto slot = running_.emplace(running_.end());
  slot->exchange = std::move(exchange);
  try {
    slot->thread = std::thread{[this, slot] {
      run(*slot->exchange);
      finished(slot);
    }};
  } catch (const std::system_error& error) {
    notes_.erase(slot->exchange->order);
    running_.erase(slot);
    return std::unexpected(fault(
        Fault::transport, join({"no thread can be started to send: ", error.what()})));
  }
  return {};
}

void HttpChannel::run(Exchange& exchange) {
  const std::stop_token stop = exchange.stop.get_token();
  {
    std::unique_lock lock{mutex_};
    // The notifications and responses sent before this message reach the
    // server first, so that `notifications/initialized` precedes the requests
    // that follow it.
    changed_.wait(lock, stop, [this, &exchange] {
      return notes_.empty() || *notes_.begin() >= exchange.order;
    });
    if (stop.stop_requested()) {
      return;
    }
    if (!exchange.modern && !session_.empty()) {
      exchange.headers.push_back({.name = "Mcp-Session-Id", .value = session_});
      exchange.in_session = true;
    }
  }
  if (Reply<void> own = add_own(exchange.headers); !own) {
    if (!exchange.id.empty()) {
      incoming_->failed(exchange.id, std::move(own).error());
    }
    return;
  }
  Receiver receiver{*this, exchange, *incoming_};
  const Reply<void> sent = http_->send(HttpClientRequest{.method = "POST",
                                                         .url = url_,
                                                         .headers = exchange.headers,
                                                         .body = exchange.body},
                                       receiver, stop);
  settle(exchange, receiver, sent);
}

Reply<void> HttpChannel::add_own(std::vector<HttpHeader>& headers) const {
  const bool bearer = static_cast<bool>(options_.bearer);
  for (const HttpHeader& header : options_.headers) {
    if (!reserved(header.name, bearer)) {
      headers.push_back(header);
    }
  }
  if (bearer) {
    std::string token;
    try {
      token = options_.bearer();
    } catch (...) {
      return std::unexpected(
          fault(Fault::unauthorized, "the bearer token function threw"));
    }
    if (!header_safe(token)) {
      return std::unexpected(fault(Fault::protocol,
                                   "the bearer token has a line break, so it cannot be "
                                   "sent in a header"));
    }
    if (!token.empty()) {
      headers.push_back({.name = "Authorization", .value = join({"Bearer ", token})});
    }
  }
  return {};
}

void HttpChannel::settle(Exchange& exchange, Receiver& receiver,
                         const Reply<void>& sent) {
  if (exchange.stop.stop_requested()) {
    // The client gave up on it, or is closing.
    return;
  }
  const std::string& id = exchange.id;
  const int status = receiver.status;
  if (status == 0) {
    if (!id.empty()) {
      incoming_->failed(id, sent ? fault(Fault::transport,
                                         "the HTTP client returned without a response")
                                 : sent.error());
    }
    return;
  }
  if (status == 404 && exchange.in_session) {
    {
      // There is nothing left to DELETE.
      const std::scoped_lock lock{mutex_};
      session_.clear();
    }
    incoming_->closed(
        fault(Fault::transport, "the server no longer knows the session (HTTP 404)"));
    return;
  }
  if (status == 401) {
    if (!id.empty()) {
      ClientError why =
          fault(Fault::unauthorized, "the server requires authorization (HTTP 401)");
      if (receiver.challenge.has_value()) {
        why.data = scrydantic::Json{
            .text = scrydantic::escape_json_string(*receiver.challenge)};
      }
      incoming_->failed(id, std::move(why));
    }
    return;
  }
  const bool success = status >= 200 && status < 300;
  const bool client_error = status >= 400 && status < 500;
  // A JSON body is the response; that of an error status may be the request's
  // JSON-RPC error, such as one that names the versions a server supports.
  if (sent && !receiver.stream && !receiver.overflowed && (success || client_error) &&
      scrydantic::is_valid_json(receiver.body)) {
    receiver.deliver(receiver.body);
  }
  if (id.empty() || receiver.answered) {
    // Nothing waits for a notification or a response.
    return;
  }
  const std::string code = std::to_string(status);
  ClientError why;
  if (receiver.overflowed) {
    why = fault(Fault::protocol, join({"the server's response is larger than ",
                                       std::to_string(most_body >> 20U),
                                       " MiB, or does not read as an event stream"}));
  } else if (!sent) {
    why = sent.error();
  } else if (success) {
    why =
        receiver.stream
            ? fault(Fault::transport, "the server ended the stream before it answered")
            : fault(Fault::protocol,
                    join({"the server's HTTP ", code,
                          " response does not hold the request's answer"}));
  } else if (client_error) {
    why = fault(Fault::protocol, join({"HTTP ", code, quoted(receiver.body)}));
  } else {
    why = fault(Fault::transport, join({"HTTP ", code, quoted(receiver.body)}));
  }
  incoming_->failed(id, std::move(why));
}

void HttpChannel::finished(const std::list<Running>::iterator slot) {
  {
    const std::scoped_lock lock{mutex_};
    notes_.erase(slot->exchange->order);
    if (!closing_) {
      ended_.push_back(slot);
    }
  }
  changed_.notify_all();
}

bool HttpChannel::abandon(const std::string_view id) {
  std::shared_ptr<Exchange> found;
  bool modern = false;
  {
    const std::scoped_lock lock{mutex_};
    modern = modern_;
    for (const Running& running : running_) {
      if (running.exchange->id == id) {
        found = running.exchange;
        modern = found->modern;
        break;
      }
    }
  }
  if (found != nullptr) {
    // Closes the stream. The HTTP client may wait for the exchange to end, so
    // nothing of the channel's is held meanwhile.
    found->stop.request_stop();
  }
  // Under 2026-07-28 closing the stream is the cancellation. A handshake
  // server takes a dropped connection for one that may come back, so it is
  // told with `notifications/cancelled`.
  return modern;
}

void HttpChannel::close() {
  std::list<Running> running;
  std::string session;
  std::string version;
  {
    std::unique_lock lock{mutex_};
    if (closing_) {
      return;
    }
    closing_ = true;
    // A notification or a response that was handed over still goes, such as
    // `notifications/cancelled` for a call that just gave up; the server
    // takes one at once. Requests are not waited for.
    changed_.wait_for(lock, ending_patience, [this] { return notes_.empty(); });
    running = std::move(running_);
    ended_.clear();
    session = std::exchange(session_, {});
    version = legacy_version_;
  }
  for (Running& each : running) {
    each.exchange->stop.request_stop();
  }
  changed_.notify_all();
  for (Running& each : running) {
    if (each.thread.joinable()) {
      each.thread.join();
    }
  }
  if (!session.empty()) {
    end_session(session, version);
  }
}

void HttpChannel::end_session(const std::string& session, const std::string& version) {
  std::vector<HttpHeader> headers{{.name = "Mcp-Session-Id", .value = session}};
  if (!version.empty()) {
    headers.push_back({.name = "MCP-Protocol-Version", .value = version});
  }
  if (!add_own(headers)) {
    return;
  }
  // The server may not end sessions (405), or may be gone; either way the
  // client is done. Closing waits for it only so long.
  std::stop_source stop;
  std::mutex mutex;
  std::condition_variable ended;
  bool done = false;
  std::thread deleting;
  try {
    deleting = std::thread{[&] {
      Discard ignored;
      static_cast<void>(http_->send(
          HttpClientRequest{
              .method = "DELETE", .url = url_, .headers = headers, .body = {}},
          ignored, stop.get_token()));
      const std::scoped_lock lock{mutex};
      done = true;
      ended.notify_all();
    }};
  } catch (const std::system_error&) {
    return;
  }
  {
    std::unique_lock lock{mutex};
    ended.wait_for(lock, ending_patience, [&done] { return done; });
  }
  stop.request_stop();
  deleting.join();
}

// Whether a URL is absolute, with the scheme `http` or `https` and a host.
[[nodiscard]] bool reachable_url(const std::string_view url) {
  for (const std::string_view scheme : {"http://", "https://"}) {
    if (istarts_with(url, scheme)) {
      const std::string_view rest = url.substr(scheme.size());
      return !rest.empty() && rest.front() != '/' && header_safe(url) &&
             url.find(' ') == std::string_view::npos;
    }
  }
  return false;
}

} // namespace

std::unique_ptr<Channel> http_channel(std::string url, HttpClient& http,
                                      HttpConnectOptions options) {
  return std::make_unique<HttpChannel>(std::move(url), http, std::move(options));
}

Reply<Client> connect(const std::string_view url, HttpClient& http,
                      ClientOptions options, HttpConnectOptions transport) {
  if (!reachable_url(url)) {
    return std::unexpected(
        fault(Fault::transport,
              join({"the URL must be absolute, with http:// or https://: ", url})));
  }
  for (const HttpHeader& header : transport.headers) {
    if (!is_token(header.name) || !header_safe(header.value)) {
      return std::unexpected(
          fault(Fault::protocol,
                join({"the header ", scrydantic::escape_json_string(header.name),
                      " cannot be sent: its name is not an HTTP token, or "
                      "its value has a line break"})));
    }
  }
  return Client::open(http_channel(std::string{url}, http, std::move(transport)),
                      std::move(options));
}

} // namespace mcp
