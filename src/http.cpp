#include "jsonrpc.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mcp/http.hpp>
#include <mcp/server.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <scrydantic/scrydantic.hpp>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcp {

namespace {

using jsonrpc::error_message;
using jsonrpc::join;
using scrydantic::JsonKind;
using scrydantic::JsonView;
using Clock = std::chrono::steady_clock;

constexpr std::string_view protocol_version_key =
    "io.modelcontextprotocol/protocolVersion";
constexpr std::string_view client_capabilities_key =
    "io.modelcontextprotocol/clientCapabilities";

// The codes this transport adds to JSON-RPC's own.
constexpr int parse_error = -32700;
constexpr int invalid_request = -32600;
constexpr int method_not_found = -32601;
constexpr int invalid_params = -32602;
constexpr int header_mismatch = -32020;
constexpr int missing_capability = -32021;
constexpr int unsupported_protocol_version = -32022;
constexpr int request_cancelled = -32800;

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

// Whether an Accept header takes a media type, itself or through a wildcard.
// A request without one takes anything, as HTTP has it.
[[nodiscard]] bool accepts(const std::optional<std::string_view>& accept,
                           const std::string_view type) {
  if (!accept.has_value()) {
    return true;
  }
  const std::string_view major = type.substr(0, type.find('/'));
  std::string_view rest = *accept;
  while (!rest.empty()) {
    const std::size_t comma = rest.find(',');
    std::string_view item = rest.substr(0, comma);
    rest =
        comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    item = trim(item.substr(0, item.find(';')));
    if (iequals(item, type) || item == "*/*" ||
        (item.size() == major.size() + 2 && istarts_with(item, major) &&
         item.substr(major.size()) == "/*")) {
      return true;
    }
  }
  return false;
}

// A host name without its port: `[::1]:8080` is `[::1]`.
[[nodiscard]] std::string_view without_port(const std::string_view host) noexcept {
  if (host.starts_with('[')) {
    return host.substr(0, host.find(']') + 1);
  }
  return host.substr(0, host.find(':'));
}

// The host of an origin such as `http://localhost:3000`.
[[nodiscard]] std::string_view origin_host(const std::string_view origin) noexcept {
  const std::size_t scheme = origin.find("://");
  if (scheme == std::string_view::npos) {
    return {};
  }
  const std::string_view rest = origin.substr(scheme + 3);
  return without_port(rest.substr(0, rest.find('/')));
}

[[nodiscard]] bool listed(const std::vector<std::string>& list,
                          const std::string_view value) {
  return std::ranges::any_of(
      list, [value](const std::string& item) { return iequals(item, value); });
}

[[nodiscard]] std::optional<std::string> base64_decode(const std::string_view text) {
  std::string bytes;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char c : text) {
    std::uint32_t value = 0;
    if (c >= 'A' && c <= 'Z') {
      value = static_cast<std::uint32_t>(c - 'A');
    } else if (c >= 'a' && c <= 'z') {
      value = static_cast<std::uint32_t>(c - 'a' + 26);
    } else if (c >= '0' && c <= '9') {
      value = static_cast<std::uint32_t>(c - '0' + 52);
    } else if (c == '+') {
      value = 62;
    } else if (c == '/') {
      value = 63;
    } else if (c == '=') {
      break;
    } else {
      return std::nullopt;
    }
    buffer = (buffer << 6U) | value;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes += static_cast<char>((buffer >> static_cast<unsigned>(bits)) & 0xFFU);
    }
  }
  return bytes;
}

// The value a routing header carries: itself, or the UTF-8 text that a
// `=?base64?...?=` value encodes, for text a header cannot hold.
[[nodiscard]] std::optional<std::string> header_text(const std::string_view value) {
  constexpr std::string_view open = "=?base64?";
  constexpr std::string_view close = "?=";
  if (value.size() >= open.size() + close.size() && istarts_with(value, open) &&
      value.ends_with(close)) {
    return base64_decode(
        value.substr(open.size(), value.size() - open.size() - close.size()));
  }
  return std::string{value};
}

[[nodiscard]] std::string new_session_id() {
  std::random_device device;
  constexpr std::string_view digits = "0123456789abcdef";
  std::string id;
  for (int word = 0; word < 4; ++word) {
    std::uint32_t value = device();
    for (int digit = 0; digit < 8; ++digit) {
      id += digits[value & 0xFU];
      value >>= 4U;
    }
  }
  return id;
}

[[nodiscard]] bool is_object(const std::optional<JsonView>& value) noexcept {
  return value.has_value() && value->kind() == JsonKind::object;
}

[[nodiscard]] bool is_legacy_version(const std::string_view version) noexcept {
  return std::ranges::find(legacy_protocol_versions, version) !=
         std::end(legacy_protocol_versions);
}

// The code of the error a response carries, or nothing for a result.
[[nodiscard]] std::optional<std::int64_t> error_code(const std::string_view response) {
  const scrydantic::Result<JsonView> document = JsonView::parse(response);
  if (!document) {
    return std::nullopt;
  }
  const std::optional<JsonView> error = document->find("error");
  if (!error.has_value()) {
    return std::nullopt;
  }
  const std::optional<JsonView> code = error->find("code");
  return code.has_value() ? code->signed_integer() : std::nullopt;
}

// The status a response goes out with. Under 2026-07-28 the errors of the
// envelope and of routing are the transport's as well as JSON-RPC's.
[[nodiscard]] int status_of(const std::string_view response, const bool modern) {
  const std::optional<std::int64_t> code = error_code(response);
  if (!code.has_value()) {
    return 200;
  }
  if (*code == parse_error || *code == invalid_request) {
    return 400;
  }
  if (modern) {
    if (*code == method_not_found) {
      return 404;
    }
    if (*code == header_mismatch || *code == missing_capability ||
        *code == unsupported_protocol_version) {
      return 400;
    }
  }
  return 200;
}

const HttpHeader json_type{.name = "Content-Type", .value = "application/json"};
const HttpHeader text_type{.name = "Content-Type",
                           .value = "text/plain; charset=utf-8"};

// Writes the response to one request: a JSON body, or an event stream from the
// first notification on.
class Reply {
public:
  Reply(HttpResponse& out, const bool streams, std::stop_source& gone) noexcept
      : out_{&out}, streams_{streams}, gone_{&gone} {}

  // Sends one message of the stream, opening it first. A client that takes no
  // stream gets no notifications.
  void event(const std::string_view message) {
    const std::scoped_lock lock{mutex_};
    if (!streams_ || lost_) {
      return;
    }
    open_stream();
    send(join({"event: message\r\ndata: ", message, "\r\n\r\n"}));
  }

  // Sends a comment on an open stream, which a client ignores.
  bool comment() {
    const std::scoped_lock lock{mutex_};
    if (lost_) {
      return false;
    }
    open_stream();
    send(": keepalive\r\n\r\n");
    return !lost_;
  }

  // Sends the response: the last message of an open stream, or the whole body.
  void finish(const int status, const std::string_view message,
              std::vector<HttpHeader> headers = {}) {
    const std::scoped_lock lock{mutex_};
    if (lost_) {
      return;
    }
    if (started_) {
      send(join({"event: message\r\ndata: ", message, "\r\n\r\n"}));
      return;
    }
    headers.push_back(json_type);
    started_ = true;
    out_->start(status, headers);
    send(message);
  }

  [[nodiscard]] bool started() const {
    const std::scoped_lock lock{mutex_};
    return started_;
  }

private:
  void open_stream() {
    if (started_) {
      return;
    }
    started_ = true;
    const std::array<HttpHeader, 3> headers{
        HttpHeader{.name = "Content-Type", .value = "text/event-stream"},
        HttpHeader{.name = "Cache-Control", .value = "no-cache, no-transform"},
        HttpHeader{.name = "X-Accel-Buffering", .value = "no"},
    };
    out_->start(200, headers);
  }

  void send(const std::string_view data) {
    if (!out_->write(data)) {
      lost_ = true;
      gone_->request_stop();
    }
  }

  mutable std::mutex mutex_;
  HttpResponse* out_;
  bool streams_;
  std::stop_source* gone_;
  bool started_{};
  bool lost_{};
};

void send_plain(HttpResponse& out, const int status, const std::string_view text,
                std::vector<HttpHeader> headers = {}) {
  headers.push_back(text_type);
  out.start(status, headers);
  out.write(text);
}

void send_json(HttpResponse& out, const int status, const std::string_view body,
               std::vector<HttpHeader> headers = {}) {
  headers.push_back(json_type);
  out.start(status, headers);
  out.write(body);
}

void send_empty(HttpResponse& out, const int status,
                const std::vector<HttpHeader>& headers = {}) {
  out.start(status, headers);
}

// A handshake session: the connection that remembers its revision, and the
// requests of it in flight, which the client cancels by id.
struct Session {
  explicit Session(const Server& server) : connection{server} {}

  Connection connection;
  std::mutex mutex;
  std::unordered_map<std::string, std::shared_ptr<std::stop_source>> running;
  // How many streams of messages of no request the session has had: the last
  // one is the one its connection writes to.
  std::uint64_t streams{};

  void cancel(const std::string& id) {
    const std::scoped_lock lock{mutex};
    if (const auto found = running.find(id); found != running.end() && found->second) {
      found->second->request_stop();
    }
  }

  void cancel_all() {
    const std::scoped_lock lock{mutex};
    for (auto& [id, stop] : running) {
      if (stop) {
        stop->request_stop();
      }
    }
  }

  // Ends the session: its calls, the questions they wait on, and its stream.
  void close() {
    closed.request_stop();
    connection.client_gone();
    cancel_all();
  }

  // Stops when the session ends.
  std::stop_source closed;
};

} // namespace

std::optional<std::string_view> HttpRequest::header(const std::string_view name) const {
  for (const HttpHeader& header : headers) {
    if (iequals(header.name, name)) {
      return header.value;
    }
  }
  return std::nullopt;
}

std::size_t HttpRequest::header_count(const std::string_view name) const {
  return static_cast<std::size_t>(
      std::ranges::count_if(headers, [name](const HttpHeader& header) {
        return iequals(header.name, name);
      }));
}

struct HttpEndpoint::State {
  State(const Server& served, HttpOptions chosen)
      : server{&served}, options{std::move(chosen)}, modern{served} {}

  const Server* server;
  HttpOptions options;
  // Serves every request of 2026-07-28, which carries all it needs.
  Connection modern;
  std::mutex mutex;
  // A session, and when a request last named it.
  struct Kept {
    std::shared_ptr<Session> session;
    Clock::time_point used;
  };
  std::unordered_map<std::string, Kept> sessions;

  static void cancel(const std::shared_ptr<Session>& session) {
    if (session != nullptr) {
      session->close();
    }
  }

  [[nodiscard]] std::shared_ptr<Session> find(const std::string& id) {
    // A session that ends here is destroyed once the lock is released.
    std::shared_ptr<Session> ended;
    const std::scoped_lock lock{mutex};
    const auto found = sessions.find(id);
    if (found == sessions.end()) {
      return nullptr;
    }
    const Clock::time_point now = Clock::now();
    if (now - found->second.used > options.session_timeout) {
      ended = std::move(found->second.session);
      cancel(ended);
      sessions.erase(found);
      return nullptr;
    }
    found->second.used = now;
    return found->second.session;
  }

  std::string keep(std::shared_ptr<Session> session) {
    // The sessions that end here are destroyed once the lock is released.
    std::vector<std::shared_ptr<Session>> ended;
    const std::scoped_lock lock{mutex};
    const Clock::time_point now = Clock::now();
    std::erase_if(sessions, [&](const auto& entry) {
      const bool expired = now - entry.second.used > options.session_timeout;
      if (expired) {
        cancel(entry.second.session);
        ended.push_back(entry.second.session);
      }
      return expired;
    });
    const std::size_t most = std::max<std::size_t>(options.max_sessions, 1);
    while (sessions.size() >= most) {
      const auto oldest = std::ranges::min_element(
          sessions, {}, [](const auto& entry) { return entry.second.used; });
      if (oldest == sessions.end()) {
        break;
      }
      cancel(oldest->second.session);
      ended.push_back(std::move(oldest->second.session));
      sessions.erase(oldest);
    }
    std::string id = new_session_id();
    sessions.emplace(id, Kept{.session = std::move(session), .used = now});
    return id;
  }

  bool end(const std::string& id) {
    std::shared_ptr<Session> session;
    {
      const std::scoped_lock lock{mutex};
      const auto found = sessions.find(id);
      if (found == sessions.end()) {
        return false;
      }
      session = std::move(found->second.session);
      sessions.erase(found);
    }
    cancel(session);
    return true;
  }

  void serve_legacy(const HttpRequest& request, HttpResponse& out,
                    const JsonView& message, const std::string& id,
                    std::string_view method);
  void stream_session(const HttpRequest& request, HttpResponse& out,
                      const std::string& id, std::stop_token stop);
  void serve_modern(const HttpRequest& request, HttpResponse& out,
                    const JsonView& message, const std::string& id,
                    std::string_view method, std::string_view version,
                    std::stop_token stop);
};

HttpEndpoint::HttpEndpoint(const Server& server, HttpOptions options)
    : state_{std::make_unique<State>(server, std::move(options))} {}

HttpEndpoint::~HttpEndpoint() = default;

void HttpEndpoint::handle(const HttpRequest& request, HttpResponse& out,
                          std::stop_token stop) {
  State& state = *state_;
  const HttpOptions& options = state.options;

  if (!options.allowed_hosts.empty()) {
    const std::optional<std::string_view> host = request.header("Host");
    if (!host.has_value() ||
        !listed(options.allowed_hosts, without_port(trim(*host)))) {
      send_plain(out, 421, "Invalid Host header");
      return;
    }
  }
  if (const std::optional<std::string_view> origin = request.header("Origin")) {
    const bool allowed = listed(options.allowed_origins, *origin) ||
                         options.allowed_hosts.empty() ||
                         listed(options.allowed_hosts, origin_host(*origin));
    if (!allowed) {
      send_plain(out, 403, "Invalid Origin header");
      return;
    }
  }

  const std::optional<std::string_view> session_header =
      request.header("Mcp-Session-Id");
  if (iequals(request.method, "DELETE") && session_header.has_value()) {
    if (state.end(std::string{*session_header})) {
      send_empty(out, 204);
    } else {
      send_plain(out, 404, "Session not found");
    }
    return;
  }
  if (iequals(request.method, "GET") && session_header.has_value()) {
    state.stream_session(request, out, std::string{*session_header}, std::move(stop));
    return;
  }
  if (!iequals(request.method, "POST")) {
    send_empty(out, 405, {{.name = "Allow", .value = "POST"}});
    return;
  }
  const std::optional<std::string_view> type = request.header("Content-Type");
  if (!type.has_value() || !istarts_with(trim(*type), "application/json")) {
    send_plain(out, 415, "Content-Type must be application/json");
    return;
  }
  if (!accepts(request.header("Accept"), "application/json")) {
    send_plain(out, 406, "Accept must include application/json");
    return;
  }

  const scrydantic::Result<JsonView> document = JsonView::parse(request.body);
  if (!document) {
    send_json(out, 400,
              error_message({}, parse_error,
                            join({"Parse error: ", document.error().message})));
    return;
  }
  if (document->kind() != JsonKind::object) {
    send_json(out, 400,
              error_message({}, invalid_request,
                            document->kind() == JsonKind::array
                                ? "Invalid Request: batches are not supported"
                                : "Invalid Request: a message must be an object"));
    return;
  }
  const std::optional<JsonView> id_value = document->find("id");
  const std::string id =
      id_value.has_value() ? id_value->to_json().text : std::string{};
  const std::optional<JsonView> method_value = document->find("method");
  const std::optional<std::string_view> method =
      method_value.has_value() ? method_value->string() : std::nullopt;

  if (request.header_count("MCP-Protocol-Version") > 1) {
    send_json(out, 400,
              error_message(id, header_mismatch,
                            "Header mismatch: MCP-Protocol-Version is given twice"));
    return;
  }
  const std::optional<std::string_view> version =
      request.header("MCP-Protocol-Version");
  // A client of an earlier revision sends no version with `initialize`, and
  // its own after it. Under 2026-07-28 a session id means nothing.
  const bool legacy = !version.has_value() || is_legacy_version(*version);
  if (legacy) {
    state.serve_legacy(request, out, *document, id,
                       method.value_or(std::string_view{}));
  } else {
    state.serve_modern(request, out, *document, id, method.value_or(std::string_view{}),
                       *version, std::move(stop));
  }
}

void HttpEndpoint::State::serve_legacy(const HttpRequest& request, HttpResponse& out,
                                       const JsonView& message, const std::string& id,
                                       const std::string_view method) {
  if (method == "initialize" && !id.empty()) {
    auto session = std::make_shared<Session>(*server);
    const std::optional<std::string> response =
        session->connection.handle(request.body);
    if (!response.has_value()) {
      send_empty(out, 202);
      return;
    }
    if (error_code(*response).has_value()) {
      send_json(out, status_of(*response, false), *response);
      return;
    }
    const std::string session_id = keep(std::move(session));
    send_json(out, 200, *response, {{.name = "Mcp-Session-Id", .value = session_id}});
    return;
  }

  const std::optional<std::string_view> session_id = request.header("Mcp-Session-Id");
  if (!session_id.has_value()) {
    send_json(
        out, 400,
        error_message(id, invalid_request,
                      "Bad Request: the request names no protocol version. Send "
                      "MCP-Protocol-Version and _meta for 2026-07-28, or initialize "
                      "first and then Mcp-Session-Id"));
    return;
  }
  const std::shared_ptr<Session> session = find(std::string{*session_id});
  if (session == nullptr) {
    send_json(out, 404, error_message(id, invalid_request, "Session not found"));
    return;
  }

  if (id.empty() || method.empty()) {
    // A notification, or the client's response to a request of the server's.
    if (method == "notifications/cancelled") {
      const std::optional<JsonView> params = message.find("params");
      const std::optional<JsonView> target =
          is_object(params) ? params->find("requestId") : std::nullopt;
      if (target.has_value()) {
        session->cancel(target->to_json().text);
      }
    }
    const std::optional<std::string> response =
        session->connection.handle(request.body);
    if (response.has_value()) {
      send_json(out, status_of(*response, false), *response);
    } else {
      send_empty(out, 202);
    }
    return;
  }

  // A request. Under these revisions a client that drops the connection has
  // not cancelled: it sends notifications/cancelled for that.
  auto stop = std::make_shared<std::stop_source>();
  {
    const std::scoped_lock lock{session->mutex};
    session->running[id] = stop;
  }
  // Nor can the client answer a question of the server's once the stream that
  // carries it is lost, so that stops the call. A client that takes no stream
  // is asked nothing.
  std::stop_source gone;
  const std::stop_callback lost{gone.get_token(), [&stop] { stop->request_stop(); }};
  const bool streams = accepts(request.header("Accept"), "text/event-stream");
  Reply reply{out, streams, gone};
  Notify notify;
  if (streams) {
    notify = [&reply](const std::string_view text) { reply.event(text); };
  }
  const std::optional<std::string> response =
      session->connection.handle(request.body, notify, stop->get_token());
  {
    const std::scoped_lock lock{session->mutex};
    if (const auto found = session->running.find(id);
        found != session->running.end() && found->second == stop) {
      session->running.erase(found);
    }
  }
  if (stop->stop_requested()) {
    // The stream must still end with a response to the request it answers.
    reply.finish(200, error_message(id, request_cancelled, "Request cancelled"));
  } else if (response.has_value()) {
    reply.finish(status_of(*response, false), *response);
  } else if (!reply.started()) {
    send_empty(out, 202);
  }
}

// The stream of a session's messages of no request: the updates of the
// resources it subscribed to. It lasts until the client closes it, and a
// newer one replaces it.
void HttpEndpoint::State::stream_session(const HttpRequest& request, HttpResponse& out,
                                         const std::string& id, std::stop_token stop) {
  const std::shared_ptr<Session> session = find(id);
  if (session == nullptr) {
    send_plain(out, 404, "Session not found");
    return;
  }
  if (!accepts(request.header("Accept"), "text/event-stream")) {
    send_plain(out, 406, "Accept must include text/event-stream");
    return;
  }
  // The stream ends when the client goes, or the session does.
  std::stop_source gone;
  const std::stop_callback forward{stop, [&gone] { gone.request_stop(); }};
  const std::stop_callback ended{session->closed.get_token(),
                                 [&gone] { gone.request_stop(); }};
  Reply reply{out, true, gone};

  // Updates are queued here by whoever announces them, and written by this
  // thread, which the host's response belongs to.
  constexpr std::size_t most_queued = 1024;
  std::mutex queue_mutex;
  std::condition_variable_any wake;
  std::deque<std::string> queued;
  std::uint64_t mine = 0;
  {
    const std::scoped_lock lock{session->mutex};
    mine = ++session->streams;
    session->connection.push_to([&](const std::string_view text) {
      {
        const std::scoped_lock queue_lock{queue_mutex};
        if (queued.size() < most_queued) {
          queued.emplace_back(text);
        }
      }
      wake.notify_all();
    });
  }
  const std::stop_token closed = gone.get_token();
  // The first comment opens the stream.
  bool open = reply.comment();
  while (open && !closed.stop_requested()) {
    std::deque<std::string> batch;
    {
      std::unique_lock lock{queue_mutex};
      wake.wait_for(lock, closed, options.keepalive, [&] { return !queued.empty(); });
      batch.swap(queued);
    }
    if (closed.stop_requested()) {
      break;
    }
    if (batch.empty()) {
      open = reply.comment();
    }
    for (const std::string& message : batch) {
      reply.event(message);
    }
    open = open && !gone.stop_requested();
  }
  const std::scoped_lock last{session->mutex};
  if (session->streams == mine) {
    session->connection.push_to({});
  }
}

void HttpEndpoint::State::serve_modern(const HttpRequest& request, HttpResponse& out,
                                       const JsonView& message, const std::string& id,
                                       const std::string_view method,
                                       const std::string_view version,
                                       std::stop_token stop) {
  if (method.empty()) {
    send_json(out, 400,
              error_message(id, invalid_request,
                            id.empty()
                                ? "Invalid Request: a message needs a method"
                                : "Invalid Request: this server sends no requests, "
                                  "so it takes no responses"));
    return;
  }
  if (id.empty()) {
    // This revision defines no notification from a client over HTTP: a client
    // cancels a call by closing its stream.
    send_empty(out, 202);
    return;
  }
  if (method == "initialize") {
    send_json(
        out, 404,
        error_message(id, method_not_found,
                      "Method not found: initialize. 2026-07-28 has no handshake"));
    return;
  }

  // The envelope comes first. A request without it is the connection's to
  // answer, and the answer is a bad request.
  const std::optional<JsonView> params = message.find("params");
  const std::optional<JsonView> meta =
      is_object(params) ? params->find("_meta") : std::nullopt;
  const std::optional<JsonView> body_version =
      is_object(meta) ? meta->find(protocol_version_key) : std::nullopt;
  const bool enveloped = body_version.has_value() &&
                         body_version->string().has_value() &&
                         is_object(meta->find(client_capabilities_key));
  if (!enveloped) {
    std::optional<std::string> response = modern.handle(request.body);
    if (!response.has_value() || !error_code(*response).has_value()) {
      // Such as ping, which an earlier revision has: here it has no envelope.
      response = error_message(
          id, invalid_params,
          join({"Invalid params: _meta[\"", protocol_version_key, "\"] and _meta[\"",
                client_capabilities_key, "\"] are required"}));
    }
    send_json(out, 400, *response);
    return;
  }

  const auto mismatch = [&](const std::string_view what) {
    send_json(out, 400,
              error_message(id, header_mismatch, join({"Header mismatch: ", what})));
  };
  if (*body_version->string() != version) {
    mismatch(join({"MCP-Protocol-Version is ", version, " but _meta names ",
                   *body_version->string()}));
    return;
  }
  const std::optional<std::string_view> method_header = request.header("Mcp-Method");
  if (request.header_count("Mcp-Method") != 1 || method_header != method) {
    mismatch(join({"Mcp-Method must be given once, as ", method}));
    return;
  }
  const std::string_view name_key = method == "tools/call" || method == "prompts/get"
                                        ? "name"
                                    : method == "resources/read" ? "uri"
                                                                 : "";
  if (!name_key.empty()) {
    const std::optional<JsonView> name = params->find(name_key);
    const std::optional<std::string_view> body_name =
        name.has_value() ? name->string() : std::nullopt;
    if (body_name.has_value()) {
      const std::optional<std::string_view> name_header = request.header("Mcp-Name");
      const std::optional<std::string> header_name =
          name_header.has_value() ? header_text(*name_header) : std::nullopt;
      if (request.header_count("Mcp-Name") != 1 || header_name != *body_name) {
        mismatch(join({"Mcp-Name must be given once, as params.", name_key}));
        return;
      }
    }
  }
  if (method == "subscriptions/listen" &&
      !accepts(request.header("Accept"), "text/event-stream")) {
    send_plain(out, 406,
               "Accept must include text/event-stream for subscriptions/listen");
    return;
  }

  // A client that closes the stream, or that the host finds has gone, cancels.
  std::stop_source gone;
  const std::stop_callback forward{stop, [&gone] { gone.request_stop(); }};
  Reply reply{out, accepts(request.header("Accept"), "text/event-stream"), gone};
  const Notify notify = [&reply](const std::string_view text) { reply.event(text); };
  const std::stop_token cancelled = gone.get_token();
  const std::optional<std::string> response =
      modern.handle(request.body, notify, cancelled);
  if (response.has_value()) {
    if (!cancelled.stop_requested()) {
      reply.finish(status_of(*response, true), *response);
    }
    return;
  }

  // A subscription, which stays open until the client closes it.
  std::mutex quiet;
  std::condition_variable_any idle;
  std::unique_lock lock{quiet};
  while (!cancelled.stop_requested()) {
    idle.wait_for(lock, cancelled, options.keepalive, [] { return false; });
    if (cancelled.stop_requested() || !reply.comment()) {
      break;
    }
  }
}

} // namespace mcp
