#include "jsonrpc.hpp"
#include "verbatim.hpp"
#include "wire.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <limits>
#include <mcp/channel.hpp>
#include <mcp/client.hpp>
#include <mcp/context.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace mcp {

namespace {

using jsonrpc::error_message;
using jsonrpc::join;
using jsonrpc::notification_message;
using jsonrpc::request_message;
using jsonrpc::result_message;

using scrydantic::Json;
using scrydantic::JsonKind;
using scrydantic::JsonView;

using Clock = std::chrono::steady_clock;

// The most pages a list is followed through, so that a server whose cursors
// never end cannot hold a call for ever.
constexpr std::size_t max_pages = 1000;

// The error of an HTTP request whose headers do not match its body.
constexpr int header_mismatch = -32020;

// The time `span` from now, or the end of time for a span that reaches past
// it.
[[nodiscard]] Clock::time_point from_now(const std::chrono::milliseconds span) {
  const Clock::time_point now = Clock::now();
  if (span >= std::chrono::duration_cast<std::chrono::milliseconds>(
                  Clock::time_point::max() - now)) {
    return Clock::time_point::max();
  }
  return now + span;
}

[[nodiscard]] ClientError fault(const Fault kind, std::string message) {
  return {
      .fault = kind, .code = 0, .message = std::move(message), .data = std::nullopt};
}

[[nodiscard]] bool is_object(const std::optional<JsonView>& value) noexcept {
  return value.has_value() && value->kind() == JsonKind::object;
}

// Whether only a server of 2026-07-28 or later sends this error: a header that
// does not match, a capability the client lacks, or a protocol version the
// server does not support.
[[nodiscard]] bool is_modern_error(const int code) noexcept {
  return code == header_mismatch || code == wire::missing_capability ||
         code == wire::unsupported_protocol_version;
}

// The revisions this client speaks with a handshake, as the static text of
// legacy_protocol_versions, or nothing.
[[nodiscard]] std::optional<std::string_view>
known_legacy(const std::string_view version) noexcept {
  for (const std::string_view known : legacy_protocol_versions) {
    if (known == version) {
      return known;
    }
  }
  return std::nullopt;
}

// A string member, or nothing when it is absent or not a string.
[[nodiscard]] std::optional<std::string_view> text_of(const JsonView& object,
                                                      const std::string_view key) {
  const std::optional<JsonView> value = object.find(key);
  return value.has_value() ? value->string() : std::nullopt;
}

// What a list of versions, such as `supportedVersions`, holds of the client's
// own: its modern revision, and the newest handshake revision it speaks.
struct Versions {
  std::optional<std::string_view> modern;
  std::optional<std::string_view> legacy;
};

[[nodiscard]] Versions versions_in(const std::optional<JsonView>& list) {
  Versions found;
  if (!list.has_value() || list->kind() != JsonKind::array) {
    return found;
  }
  for (std::size_t index = 0; index < list->size(); ++index) {
    const std::optional<std::string_view> version = list->at(index)->string();
    if (!version.has_value()) {
      continue;
    }
    if (*version == modern_protocol_version) {
      found.modern = modern_protocol_version;
    } else if (const std::optional<std::string_view> legacy = known_legacy(*version);
               legacy.has_value() && (!found.legacy.has_value() ||
                                      // legacy_protocol_versions is newest first.
                                      legacy->data() < found.legacy->data())) {
      found.legacy = legacy;
    }
  }
  return found;
}

// What a request got back.
struct Response {
  // The result object as the server wrote it, and a view of it.
  std::string text;
  JsonView result;
};

// A request that was sent and has not finished.
struct Pending {
  std::uint64_t id{};
  // Once the response came: its result, or the error it carried.
  bool done{};
  std::optional<Response> response;
  std::optional<ClientError> error;
  // Receives the request's progress reports, or none.
  const std::function<void(const ProgressReport&)>* progress{};
  // How many of its progress reports are being delivered now.
  int reporting{};
  // The time limit, zero for none, and when it runs out. A progress report
  // moves the deadline.
  std::chrono::milliseconds limit{};
  Clock::time_point deadline{};
};

} // namespace

struct Client::State final : Incoming {
  State(std::unique_ptr<Channel> channel_, ClientOptions options_)
      : channel{std::move(channel_)}, options{std::move(options_)} {}

  State(const State&) = delete;
  State& operator=(const State&) = delete;

  ~State() override { shutdown(); }

  void message(std::string_view text) override;
  void failed(std::string_view id, ClientError why) override;
  void closed(ClientError why) override;

  // Agrees on an era with the server, as ClientOptions::era says.
  Reply<void> handshake();

  // Sends a request and waits for its result.
  template <typename Params>
  Reply<Response> request(std::string_view method, Params params, std::string_view name,
                          const CallOptions& call);

  // Ends the client: every waiting call fails, and the channel closes.
  void shutdown();

  std::unique_ptr<Channel> channel;
  ClientOptions options;
  ServerInfo info;
  // Whether the channel opened, so that it is closed.
  bool opened{};

private:
  // How a request is sent: under the modern revision, with the protocol fields
  // in its `_meta`, and under which version. Empty before a handshake.
  struct Revision {
    bool modern{};
    std::string_view version{};
  };

  [[nodiscard]] Revision revision() const;
  [[nodiscard]] std::chrono::milliseconds limit_of(const CallOptions& call) const;

  // Sends a request without waiting for it.
  template <typename Params>
  Reply<std::shared_ptr<Pending>> start(std::string_view method, Params params,
                                        std::string_view name, const CallOptions& call,
                                        Revision revision);
  // How a wait ended.
  enum class Waited : std::uint8_t { done, until, cancelled, timeout };
  // Waits until the request has finished, or until `until` if it is given, or
  // until the caller stops it or its time runs out.
  Waited wait(Pending& pending, const std::stop_token& stop,
              std::optional<Clock::time_point> until);
  // Waits until one of two requests has finished, or both have run out of
  // time. Returns the one that finished, the first if both did, or none.
  const Pending* wait_either(const Pending& first, const Pending& second);
  // Waits for the request as wait() does, then takes it out of the client:
  // returns its outcome, or cancels it when the caller stopped it or its time
  // ran out.
  Reply<Response> finish(const std::shared_ptr<Pending>& pending,
                         std::string_view method, const std::stop_token& stop);
  // Takes a request out of the client, for good. Returns its outcome if it
  // finished.
  std::optional<Reply<Response>> take(Pending& pending);
  // Gives up on a request without waiting for it more: the channel abandons
  // it, and the server is told unless the channel told it or the method may
  // not be cancelled.
  void give_up(const Pending& pending, std::string_view method,
               std::string_view reason);

  // The parts of handshake().
  Reply<void> discover(bool automatic);
  Reply<void> initialize();
  Reply<void> accept_initialize(Reply<Response> answer);
  Reply<std::shared_ptr<Pending>> start_discover(std::string_view version);
  Reply<std::shared_ptr<Pending>> start_initialize();
  // What a `server/discover` answer says of the server.
  enum class Verdict : std::uint8_t { modern, legacy, retry, failed };
  struct Judgment {
    Verdict verdict{Verdict::failed};
    ClientError error{};
    std::string_view retry_version{};
  };
  Judgment judge_discover(const Reply<Response>& answer, bool automatic, bool retried);
  // Waits for a probe and a handshake sent together, and opens in the era of
  // the first that succeeds. Sets `judgment` to what the probe said, if it was
  // judged; a retry verdict there leaves the probe to be sent again.
  Reply<void> race_handshake(const std::shared_ptr<Pending>& probe,
                             const std::shared_ptr<Pending>& handshake,
                             Judgment& judgment);
  // Settles on the modern revision `version`.
  Reply<void> adopt_modern(std::string_view version);

  void answered(std::string_view text, const JsonView& document);
  void notified(const JsonView& document, std::string_view method);
  void requested(std::string_view method, const JsonView& id);
  // Sends one notification or response, unless the client is closed.
  void post(std::string_view message, std::string_view method);

  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Pending>> pending_;
  // Why the client can make no more calls, once it cannot.
  std::optional<ClientError> gone_;
  Revision revision_{};
  std::atomic<std::uint64_t> next_id_{0};
};

namespace {

// The JSON-RPC error of a response, or why it is not one.
[[nodiscard]] ClientError rejection(const std::string_view text,
                                    const JsonView& error) {
  if (error.kind() != JsonKind::object) {
    return fault(Fault::protocol, "the server's error is not an object");
  }
  const std::optional<JsonView> code = error.find("code");
  const std::optional<std::string_view> message = text_of(error, "message");
  const bool integer = code.has_value() && (code->kind() == JsonKind::signed_integer ||
                                            code->kind() == JsonKind::unsigned_integer);
  const std::optional<double> number = integer ? code->number() : std::nullopt;
  if (!number.has_value() || *number < std::numeric_limits<int>::min() ||
      *number > std::numeric_limits<int>::max() || !message.has_value()) {
    return fault(Fault::protocol,
                 "the server's error needs an integer code and a string message");
  }
  ClientError rejected{.fault = Fault::rejected,
                       .code = static_cast<int>(*number),
                       .message = std::string{*message},
                       .data = std::nullopt};
  const std::optional<std::string_view> raw = verbatim::member(text, "error");
  if (const std::optional<std::string_view> data =
          raw.has_value() ? verbatim::member(*raw, "data") : std::nullopt) {
    rejected.data = Json{.text = std::string{*data}};
  }
  return rejected;
}

// The result of a response, or why it has none.
[[nodiscard]] Reply<Response> outcome(const std::string_view text,
                                      const JsonView& document) {
  if (text_of(document, "jsonrpc") != "2.0") {
    return std::unexpected(fault(
        Fault::protocol, R"(the server's response does not say "jsonrpc":"2.0")"));
  }
  const std::optional<JsonView> result = document.find("result");
  const std::optional<JsonView> error = document.find("error");
  if (result.has_value() == error.has_value()) {
    return std::unexpected(fault(
        Fault::protocol, "the server's response needs a result or an error, not both"));
  }
  if (error.has_value()) {
    return std::unexpected(rejection(text, *error));
  }
  if (result->kind() != JsonKind::object) {
    return std::unexpected(
        fault(Fault::protocol, "the server's result is not an object"));
  }
  return Response{.text = std::string{verbatim::member(text, "result").value_or("{}")},
                  .result = *result};
}

// Whether a modern result is complete: one without `resultType` is, as an
// earlier revision writes it.
[[nodiscard]] Reply<void> complete(const Response& response) {
  const std::optional<JsonView> type = response.result.find("resultType");
  if (!type.has_value() || type->string() == "complete") {
    return {};
  }
  if (type->string() == "input_required") {
    return std::unexpected(fault(
        Fault::protocol, "the server asks for input, which this client cannot give"));
  }
  return std::unexpected(
      fault(Fault::protocol,
            join({"the server's result has a resultType this client does not know: ",
                  type->to_json().text})));
}

[[nodiscard]] std::optional<std::uint64_t> id_number(const std::string_view text) {
  std::uint64_t id = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return id;
}

} // namespace

Client::State::Revision Client::State::revision() const {
  const std::scoped_lock lock{mutex_};
  return revision_;
}

std::chrono::milliseconds Client::State::limit_of(const CallOptions& call) const {
  const std::chrono::milliseconds limit = call.timeout.value_or(options.timeout);
  return limit.count() < 0 ? std::chrono::milliseconds{0} : limit;
}

template <typename Params>
Reply<std::shared_ptr<Pending>>
Client::State::start(const std::string_view method, Params params,
                     const std::string_view name, const CallOptions& call,
                     const Revision revision) {
  const std::uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed) + 1;
  const std::string id_text = std::to_string(id);
  wire::RequestMeta meta{};
  if (revision.modern) {
    meta.client_capabilities = wire::ClientCapabilities{};
    meta.client_info =
        wire::Implementation{.name = options.name, .version = options.version};
    meta.protocol_version = std::string{revision.version};
  }
  if (call.progress) {
    // The request's id is unique for the life of the client, so it serves as
    // the progress token too.
    meta.progress_token = id;
  }
  if (revision.modern || call.progress) {
    params.meta = std::move(meta);
  }
  const scrydantic::Result<Json> encoded = scrydantic::encode_verbatim(params);
  if (!encoded) {
    return std::unexpected(fault(Fault::protocol, join({"the request cannot be sent: ",
                                                        encoded.error().message})));
  }
  const std::string message = request_message(id_text, method, encoded->text);

  auto pending = std::make_shared<Pending>();
  pending->id = id;
  pending->progress = call.progress ? &call.progress : nullptr;
  pending->limit = limit_of(call);
  pending->deadline = from_now(pending->limit);
  {
    const std::scoped_lock lock{mutex_};
    if (gone_.has_value()) {
      return std::unexpected(*gone_);
    }
    pending_.emplace(id, pending);
  }
  const Reply<void> sent = channel->send(Outgoing{.message = message,
                                                  .id = id_text,
                                                  .method = method,
                                                  .name = name,
                                                  .protocol_version = revision.version,
                                                  .headers = {}});
  if (!sent) {
    take(*pending);
    return std::unexpected(sent.error());
  }
  return pending;
}

Client::State::Waited
Client::State::wait(Pending& pending, const std::stop_token& stop,
                    const std::optional<Clock::time_point> until) {
  std::unique_lock lock{mutex_};
  for (;;) {
    if (pending.done) {
      return Waited::done;
    }
    const Clock::time_point now = Clock::now();
    if (stop.stop_requested()) {
      return Waited::cancelled;
    }
    if (pending.limit.count() > 0 && now >= pending.deadline) {
      return Waited::timeout;
    }
    if (until.has_value() && now >= *until) {
      return Waited::until;
    }
    std::optional<Clock::time_point> wake = until;
    if (pending.limit.count() > 0 && (!wake.has_value() || pending.deadline < *wake)) {
      wake = pending.deadline;
    }
    if (wake.has_value() && *wake != Clock::time_point::max()) {
      changed_.wait_until(lock, stop, *wake, [&pending] { return pending.done; });
    } else {
      changed_.wait(lock, stop, [&pending] { return pending.done; });
    }
  }
}

const Pending* Client::State::wait_either(const Pending& first, const Pending& second) {
  const auto finished = [&first, &second] { return first.done || second.done; };
  std::unique_lock lock{mutex_};
  for (;;) {
    if (finished()) {
      return first.done ? &first : &second;
    }
    const Clock::time_point now = Clock::now();
    // The earliest deadline of the two that is still to come, or none when
    // one of them waits for ever.
    std::optional<Clock::time_point> wake;
    bool forever = false;
    for (const Pending* const pending : {&first, &second}) {
      if (pending->limit.count() == 0) {
        forever = true;
      } else if (pending->deadline > now &&
                 (!wake.has_value() || pending->deadline < *wake)) {
        wake = pending->deadline;
      }
    }
    if (wake.has_value() && *wake != Clock::time_point::max()) {
      changed_.wait_until(lock, *wake, finished);
    } else if (forever || wake.has_value()) {
      changed_.wait(lock, finished);
    } else {
      return nullptr;
    }
  }
}

std::optional<Reply<Response>> Client::State::take(Pending& pending) {
  std::unique_lock lock{mutex_};
  pending_.erase(pending.id);
  // A progress report being delivered finishes before the call returns, so
  // that it never outlives what the caller gave it.
  changed_.wait(lock, [&pending] { return pending.reporting == 0; });
  if (!pending.done) {
    return std::nullopt;
  }
  if (pending.error.has_value()) {
    return std::unexpected(std::move(*pending.error));
  }
  return std::move(*pending.response);
}

void Client::State::give_up(const Pending& pending, const std::string_view method,
                            const std::string_view reason) {
  {
    const std::scoped_lock lock{mutex_};
    if (gone_.has_value()) {
      return;
    }
  }
  const std::string id = std::to_string(pending.id);
  const bool told = channel->abandon(id);
  // The protocol forbids cancelling `initialize`, and `server/discover` is part
  // of opening as `initialize` is.
  if (told || method == "initialize" || method == "server/discover") {
    return;
  }
  const scrydantic::Result<Json> params = scrydantic::encode(
      wire::CancelledParams{.reason = std::string{reason}, .request_id = pending.id});
  if (params) {
    post(notification_message("notifications/cancelled", params->text),
         "notifications/cancelled");
  }
}

Reply<Response> Client::State::finish(const std::shared_ptr<Pending>& pending,
                                      const std::string_view method,
                                      const std::stop_token& stop) {
  const Waited ended = wait(*pending, stop, std::nullopt);
  if (std::optional<Reply<Response>> outcome = take(*pending)) {
    return std::move(*outcome);
  }
  const bool cancelled = ended == Waited::cancelled;
  give_up(*pending, method,
          cancelled ? "the caller cancelled the request" : "the request timed out");
  return std::unexpected(
      fault(cancelled ? Fault::cancelled : Fault::timeout,
            join({method, cancelled ? " was cancelled" : " timed out"})));
}

template <typename Params>
Reply<Response> Client::State::request(const std::string_view method, Params params,
                                       const std::string_view name,
                                       const CallOptions& call) {
  if (call.stop.stop_requested()) {
    return std::unexpected(fault(Fault::cancelled, join({method, " was cancelled"})));
  }
  const Revision sent_as = revision();
  Reply<std::shared_ptr<Pending>> pending =
      start(method, std::move(params), name, call, sent_as);
  if (!pending) {
    return std::unexpected(std::move(pending).error());
  }
  Reply<Response> response = finish(*pending, method, call.stop);
  if (response && sent_as.modern) {
    if (Reply<void> done = complete(*response); !done) {
      return std::unexpected(std::move(done).error());
    }
  }
  return response;
}

void Client::State::post(const std::string_view message,
                         const std::string_view method) {
  const Revision current = revision();
  {
    const std::scoped_lock lock{mutex_};
    if (gone_.has_value()) {
      return;
    }
  }
  // A notification that cannot be sent has nothing to report to: the request
  // it concerns has already ended.
  static_cast<void>(channel->send(Outgoing{.message = message,
                                           .id = {},
                                           .method = method,
                                           .name = {},
                                           .protocol_version = current.version,
                                           .headers = {}}));
}

void Client::State::shutdown() {
  {
    const std::scoped_lock lock{mutex_};
    if (!gone_.has_value()) {
      gone_ = fault(Fault::transport, "the client is closed");
    }
    for (auto& [id, pending] : pending_) {
      pending->error = *gone_;
      pending->done = true;
    }
    pending_.clear();
  }
  changed_.notify_all();
  if (opened) {
    opened = false;
    channel->close();
  }
}

void Client::State::message(const std::string_view text) {
  const scrydantic::Result<JsonView> document = JsonView::parse(text);
  if (!document || document->kind() != JsonKind::object) {
    // Nothing in it says which call it concerns, so none fails for it.
    return;
  }
  const std::optional<JsonView> method = document->find("method");
  if (!method.has_value()) {
    answered(text, *document);
    return;
  }
  const std::optional<std::string_view> name = method->string();
  if (!name.has_value()) {
    return;
  }
  if (const std::optional<JsonView> id = document->find("id")) {
    requested(*name, *id);
  } else {
    notified(*document, *name);
  }
}

void Client::State::answered(const std::string_view text, const JsonView& document) {
  const std::optional<JsonView> id = document.find("id");
  // Every id the client sends is a positive integer. Another one, or a null id
  // of a request the server could not read, belongs to no call.
  const std::optional<std::uint64_t> number =
      id.has_value() ? id->unsigned_integer() : std::nullopt;
  if (!number.has_value()) {
    return;
  }
  std::shared_ptr<Pending> pending;
  {
    const std::scoped_lock lock{mutex_};
    const auto found = pending_.find(*number);
    if (found == pending_.end()) {
      // A call that gave up already, or an id the client never sent.
      return;
    }
    pending = found->second;
    pending_.erase(found);
  }
  Reply<Response> result = outcome(text, document);
  {
    const std::scoped_lock lock{mutex_};
    if (result) {
      pending->response = std::move(*result);
    } else {
      pending->error = std::move(result).error();
    }
    pending->done = true;
  }
  changed_.notify_all();
}

void Client::State::notified(const JsonView& document, const std::string_view method) {
  if (method != "notifications/progress") {
    // Nothing else is asked for yet.
    return;
  }
  const std::optional<JsonView> params = document.find("params");
  if (!is_object(params)) {
    return;
  }
  const std::optional<JsonView> token = params->find("progressToken");
  const std::optional<std::uint64_t> id =
      token.has_value() ? token->unsigned_integer() : std::nullopt;
  const std::optional<JsonView> progress = params->find("progress");
  const std::optional<double> value =
      progress.has_value() ? progress->number() : std::nullopt;
  if (!id.has_value() || !value.has_value()) {
    return;
  }
  ProgressReport report{.progress = *value, .total = std::nullopt, .message = {}};
  if (const std::optional<JsonView> total = params->find("total")) {
    report.total = total->number();
  }
  report.message = std::string{text_of(*params, "message").value_or("")};

  const std::function<void(const ProgressReport&)>* receiver = nullptr;
  std::shared_ptr<Pending> pending;
  {
    const std::scoped_lock lock{mutex_};
    const auto found = pending_.find(*id);
    if (found == pending_.end() || found->second->progress == nullptr) {
      return;
    }
    pending = found->second;
    receiver = pending->progress;
    // Work is happening, so the time starts again.
    pending->deadline = from_now(pending->limit);
    ++pending->reporting;
  }
  try {
    (*receiver)(report);
  } catch (...) {
    // The caller's own function failed. It is not the server's doing, and the
    // call goes on.
    static_cast<void>(std::current_exception());
  }
  {
    const std::scoped_lock lock{mutex_};
    --pending->reporting;
  }
  changed_.notify_all();
}

void Client::State::requested(const std::string_view method, const JsonView& id) {
  const JsonKind kind = id.kind();
  if (kind != JsonKind::string && kind != JsonKind::signed_integer &&
      kind != JsonKind::unsigned_integer) {
    return;
  }
  const std::string id_text = id.to_json().text;
  // A legacy server may ping its client. The client declares no capability
  // yet, so every other request is a method it does not have.
  post(method == "ping" ? result_message(id_text, "{}")
                        : error_message(id_text, wire::method_not_found,
                                        join({"Method not found: ", method})),
       {});
}

void Client::State::failed(const std::string_view id, ClientError why) {
  const std::optional<std::uint64_t> number = id_number(id);
  if (!number.has_value()) {
    return;
  }
  {
    const std::scoped_lock lock{mutex_};
    const auto found = pending_.find(*number);
    if (found == pending_.end()) {
      return;
    }
    found->second->error = std::move(why);
    found->second->done = true;
    pending_.erase(found);
  }
  changed_.notify_all();
}

void Client::State::closed(ClientError why) {
  {
    const std::scoped_lock lock{mutex_};
    if (!gone_.has_value()) {
      gone_ = std::move(why);
    }
    for (auto& [id, pending] : pending_) {
      pending->error = *gone_;
      pending->done = true;
    }
    pending_.clear();
  }
  changed_.notify_all();
}

// Opening.

Reply<std::shared_ptr<Pending>>
Client::State::start_discover(const std::string_view version) {
  return start("server/discover", wire::RequestParams{}, {}, {},
               Revision{.modern = true, .version = version});
}

Reply<std::shared_ptr<Pending>> Client::State::start_initialize() {
  return start("initialize",
               wire::InitializeParams{
                   .meta = std::nullopt,
                   .capabilities = {},
                   .client_info = {.name = options.name, .version = options.version},
                   .protocol_version = std::string{legacy_protocol_versions[0]}},
               {}, {}, Revision{});
}

Reply<void> Client::State::handshake() {
  switch (options.era) {
  case Era::legacy:
    return initialize();
  case Era::modern:
    return discover(false);
  case Era::automatic:
    break;
  }
  return discover(true);
}

Reply<void> Client::State::initialize() {
  Reply<std::shared_ptr<Pending>> pending = start_initialize();
  if (!pending) {
    return std::unexpected(std::move(pending).error());
  }
  return accept_initialize(finish(*pending, "initialize", {}));
}

Reply<void> Client::State::accept_initialize(Reply<Response> answer) {
  if (!answer) {
    return std::unexpected(std::move(answer).error());
  }
  const JsonView& result = answer->result;
  const std::optional<std::string_view> asked = text_of(result, "protocolVersion");
  const std::optional<std::string_view> version =
      asked.has_value() ? known_legacy(*asked) : std::nullopt;
  if (!version.has_value()) {
    return std::unexpected(fault(
        Fault::protocol,
        asked.has_value()
            ? join({"the server offers protocol version ", *asked,
                    ", which this client does not speak"})
            : std::string{"the server's initialize result names no protocolVersion"}));
  }
  const std::optional<JsonView> server = result.find("serverInfo");
  const std::optional<std::string_view> name =
      is_object(server) ? text_of(*server, "name") : std::nullopt;
  const std::optional<std::string_view> server_version =
      is_object(server) ? text_of(*server, "version") : std::nullopt;
  if (!is_object(result.find("capabilities")) || !name.has_value() ||
      !server_version.has_value()) {
    return std::unexpected(
        fault(Fault::protocol, "the server's initialize result needs capabilities and "
                               "serverInfo with a name and a version"));
  }
  info.name = std::string{*name};
  info.title = std::string{text_of(*server, "title").value_or("")};
  info.version = std::string{*server_version};
  if (const std::optional<std::string_view> instructions =
          text_of(result, "instructions")) {
    info.instructions = std::string{*instructions};
  }
  info.capabilities.text =
      std::string{verbatim::member(answer->text, "capabilities").value_or("{}")};
  info.protocol_version = std::string{*version};
  info.era = Era::legacy;
  {
    const std::scoped_lock lock{mutex_};
    revision_ = Revision{.modern = false, .version = *version};
  }
  post(notification_message("notifications/initialized", {}),
       "notifications/initialized");
  return {};
}

Client::State::Judgment Client::State::judge_discover(const Reply<Response>& answer,
                                                      const bool automatic,
                                                      const bool retried) {
  // What a server that is not modern gets: the handshake, or under
  // Era::modern the error.
  const auto not_modern = [automatic](ClientError error) {
    return Judgment{.verdict = automatic ? Verdict::legacy : Verdict::failed,
                    .error = std::move(error),
                    .retry_version = {}};
  };
  if (!answer) {
    const ClientError& error = answer.error();
    if (error.fault == Fault::protocol) {
      // What answered is not a modern server.
      return not_modern(error);
    }
    if (error.fault != Fault::rejected) {
      return {.verdict = Verdict::failed, .error = error, .retry_version = {}};
    }
    if (!is_modern_error(error.code)) {
      return not_modern(error);
    }
    // A modern server. Only a version it lacks is worth another try.
    if (error.code == wire::unsupported_protocol_version && error.data.has_value()) {
      const scrydantic::Result<JsonView> data = JsonView::parse(*error.data);
      const Versions supported =
          data && is_object(*data) ? versions_in(data->find("supported")) : Versions{};
      if (supported.modern.has_value() && !retried) {
        return {.verdict = Verdict::retry,
                .error = error,
                .retry_version = *supported.modern};
      }
      if (!supported.modern.has_value() && supported.legacy.has_value() && automatic) {
        // It speaks a revision of the client's, which a handshake agrees.
        return {.verdict = Verdict::legacy, .error = error, .retry_version = {}};
      }
    }
    return {.verdict = Verdict::failed, .error = error, .retry_version = {}};
  }

  const JsonView& result = answer->result;
  if (Reply<void> done = complete(*answer); !done) {
    return not_modern(done.error());
  }
  const Versions supported = versions_in(result.find("supportedVersions"));
  if (!is_object(result.find("capabilities")) ||
      !result.find("supportedVersions").has_value() ||
      result.find("supportedVersions")->kind() != JsonKind::array) {
    return not_modern(fault(Fault::protocol,
                            "the server's discover result needs supportedVersions "
                            "and capabilities"));
  }
  if (!supported.modern.has_value()) {
    // A server that answers discover but supports no modern revision of the
    // client's speaks the handshake.
    return not_modern(
        fault(Fault::protocol,
              "the server supports none of this client's protocol versions"));
  }
  info = ServerInfo{};
  const std::optional<JsonView> meta = result.find("_meta");
  const std::optional<JsonView> server =
      is_object(meta) ? meta->find("io.modelcontextprotocol/serverInfo") : std::nullopt;
  if (is_object(server)) {
    // The server's identity is for display, so a malformed one is left out.
    info.name = std::string{text_of(*server, "name").value_or("")};
    info.title = std::string{text_of(*server, "title").value_or("")};
    info.version = std::string{text_of(*server, "version").value_or("")};
  }
  if (const std::optional<std::string_view> instructions =
          text_of(result, "instructions")) {
    info.instructions = std::string{*instructions};
  }
  info.capabilities.text =
      std::string{verbatim::member(answer->text, "capabilities").value_or("{}")};
  info.era = Era::modern;
  return {.verdict = Verdict::modern, .error = {}, .retry_version = {}};
}

Reply<void> Client::State::discover(const bool automatic) {
  std::string_view version = modern_protocol_version;
  bool retried = false;
  bool race = automatic;
  for (;;) {
    Reply<std::shared_ptr<Pending>> started = start_discover(version);
    if (!started) {
      return std::unexpected(std::move(started).error());
    }
    const std::shared_ptr<Pending> probe = std::move(*started);
    Judgment judgment;
    if (race && wait(*probe, {}, from_now(options.probe_timeout)) == Waited::until) {
      // A server that is slow to start must still open in its own era: the
      // probe has waited long enough, so `initialize` goes too, and the first
      // of the two to succeed wins.
      Reply<std::shared_ptr<Pending>> handshake = start_initialize();
      if (!handshake) {
        take(*probe);
        return std::unexpected(std::move(handshake).error());
      }
      Reply<void> raced = race_handshake(probe, *handshake, judgment);
      if (judgment.verdict != Verdict::retry) {
        return raced;
      }
    } else {
      judgment =
          judge_discover(finish(probe, "server/discover", {}), automatic, retried);
    }
    race = false;
    switch (judgment.verdict) {
    case Verdict::modern:
      return adopt_modern(version);
    case Verdict::retry:
      version = judgment.retry_version;
      retried = true;
      continue;
    case Verdict::legacy:
      return initialize();
    case Verdict::failed:
      break;
    }
    return std::unexpected(std::move(judgment.error));
  }
}

Reply<void> Client::State::adopt_modern(const std::string_view version) {
  info.protocol_version = std::string{version};
  const std::scoped_lock lock{mutex_};
  revision_ = Revision{.modern = true, .version = version};
  return {};
}

Reply<void> Client::State::race_handshake(const std::shared_ptr<Pending>& probe,
                                          const std::shared_ptr<Pending>& handshake,
                                          Judgment& judgment) {
  // The one that lost is abandoned. Neither may be cancelled, so the server is
  // not told.
  const auto drop = [this](const std::shared_ptr<Pending>& loser,
                           const std::string_view method) {
    if (!take(*loser).has_value()) {
      give_up(*loser, method, {});
    }
  };
  const Pending* const first = wait_either(*probe, *handshake);
  if (first == probe.get()) {
    judgment = judge_discover(*take(*probe), true, false);
    switch (judgment.verdict) {
    case Verdict::modern:
      drop(handshake, "initialize");
      return adopt_modern(modern_protocol_version);
    case Verdict::retry:
      // A modern server, which wants another version.
      drop(handshake, "initialize");
      return {};
    case Verdict::legacy:
      return accept_initialize(finish(handshake, "initialize", {}));
    case Verdict::failed:
      break;
    }
    drop(handshake, "initialize");
    return std::unexpected(judgment.error);
  }
  if (first == handshake.get()) {
    Reply<void> accepted = accept_initialize(*take(*handshake));
    if (accepted) {
      drop(probe, "server/discover");
      return accepted;
    }
    // The handshake failed, so the probe may yet show a modern server.
    judgment = judge_discover(finish(probe, "server/discover", {}), true, false);
    switch (judgment.verdict) {
    case Verdict::modern:
      return adopt_modern(modern_protocol_version);
    case Verdict::retry:
      return {};
    case Verdict::legacy:
      return accepted;
    case Verdict::failed:
      break;
    }
    return std::unexpected(judgment.error);
  }
  drop(probe, "server/discover");
  drop(handshake, "initialize");
  return std::unexpected(fault(
      Fault::timeout, "the server answered neither server/discover nor initialize"));
}

// The client.

namespace {

[[nodiscard]] ClientError moved_from() {
  return fault(Fault::transport, "the client has been moved from");
}

// Reads the tools of one page of tools/list into `tools`.
[[nodiscard]] Reply<void> read_tools(const Response& response,
                                     std::vector<Tool>& tools) {
  const std::optional<JsonView> list = response.result.find("tools");
  const std::optional<std::string_view> raw = verbatim::member(response.text, "tools");
  const std::optional<std::vector<std::string_view>> entries =
      raw.has_value() ? verbatim::elements(*raw) : std::nullopt;
  if (!list.has_value() || list->kind() != JsonKind::array || !entries.has_value() ||
      entries->size() != list->size()) {
    return std::unexpected(
        fault(Fault::protocol, "the server's tool list is not an array"));
  }
  for (std::size_t index = 0; index < list->size(); ++index) {
    const JsonView entry = *list->at(index);
    const std::string_view text = (*entries)[index];
    const std::optional<std::string_view> name =
        entry.kind() == JsonKind::object ? text_of(entry, "name") : std::nullopt;
    if (!name.has_value() || !is_object(entry.find("inputSchema"))) {
      return std::unexpected(
          fault(Fault::protocol,
                join({"tool ", std::to_string(index),
                      " of the server's list needs a name and an inputSchema"})));
    }
    Tool tool{
        .name = std::string{*name},
        .title = std::string{text_of(entry, "title").value_or("")},
        .description = std::string{text_of(entry, "description").value_or("")},
        .input_schema = {.text = std::string{*verbatim::member(text, "inputSchema")}},
        .output_schema = std::nullopt,
        .annotations = std::nullopt};
    for (const auto& [key, member] : {std::pair{"outputSchema", &tool.output_schema},
                                      std::pair{"annotations", &tool.annotations}}) {
      const std::optional<JsonView> value = entry.find(key);
      if (value.has_value() && value->kind() != JsonKind::null) {
        *member = Json{.text = std::string{*verbatim::member(text, key)}};
      }
    }
    tools.push_back(std::move(tool));
  }
  return {};
}

[[nodiscard]] Reply<ToolResult> read_tool_result(const Response& response) {
  const std::optional<JsonView> content = response.result.find("content");
  if (!content.has_value() || content->kind() != JsonKind::array) {
    return std::unexpected(
        fault(Fault::protocol, "the server's tool result has no content array"));
  }
  scrydantic::Result<Content> blocks = scrydantic::decode<Content>(*content);
  if (!blocks) {
    return std::unexpected(
        fault(Fault::protocol,
              join({"the server's tool result has content this client cannot read: ",
                    blocks.error().message})));
  }
  ToolResult result{.content = std::move(*blocks),
                    .structured_content = std::nullopt,
                    .is_error = false};
  if (const std::optional<JsonView> structured =
          response.result.find("structuredContent");
      structured.has_value() && structured->kind() != JsonKind::null) {
    result.structured_content = Json{
        .text = std::string{*verbatim::member(response.text, "structuredContent")}};
  }
  if (const std::optional<JsonView> is_error = response.result.find("isError")) {
    const std::optional<bool> flag = is_error->boolean();
    if (!flag.has_value()) {
      return std::unexpected(
          fault(Fault::protocol, "the server's tool result has an isError that is not "
                                 "a boolean"));
    }
    result.is_error = *flag;
  }
  return result;
}

} // namespace

bool ServerInfo::has(const std::string_view capability) const {
  const scrydantic::Result<JsonView> parsed = JsonView::parse(capabilities);
  if (!parsed) {
    return false;
  }
  const std::optional<JsonView> value = parsed->find(capability);
  return value.has_value() && value->kind() != JsonKind::null;
}

Client::Client(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
Client::~Client() = default;
Client::Client(Client&& other) noexcept = default;
Client& Client::operator=(Client&& other) noexcept = default;

Reply<Client> Client::open(std::unique_ptr<Channel> channel, ClientOptions options) {
  if (channel == nullptr) {
    return std::unexpected(fault(Fault::transport, "there is no channel"));
  }
  auto state = std::make_unique<State>(std::move(channel), std::move(options));
  if (Reply<void> opened = state->channel->open(*state); !opened) {
    return std::unexpected(std::move(opened).error());
  }
  state->opened = true;
  if (Reply<void> agreed = state->handshake(); !agreed) {
    // The state closes the channel as it goes.
    return std::unexpected(std::move(agreed).error());
  }
  return Client{std::move(state)};
}

const ServerInfo& Client::server() const noexcept {
  static const ServerInfo none{};
  return state_ != nullptr ? state_->info : none;
}

Reply<void> Client::ping(const CallOptions& options) {
  if (state_ == nullptr) {
    return std::unexpected(moved_from());
  }
  // 2026-07-28 has no ping. server/discover is the one request every modern
  // server answers, and asks nothing of it.
  const std::string_view method =
      state_->info.era == Era::modern ? "server/discover" : "ping";
  Reply<Response> response =
      state_->request(method, wire::RequestParams{}, {}, options);
  if (!response) {
    return std::unexpected(std::move(response).error());
  }
  return {};
}

Reply<std::vector<Tool>> Client::list_tools(const CallOptions& options) {
  if (state_ == nullptr) {
    return std::unexpected(moved_from());
  }
  std::vector<Tool> tools;
  std::optional<std::string> cursor;
  for (std::size_t page = 0; page < max_pages; ++page) {
    Reply<Response> response = state_->request(
        "tools/list",
        wire::ListParams{.meta = std::nullopt, .cursor = std::move(cursor)}, {},
        options);
    if (!response) {
      return std::unexpected(std::move(response).error());
    }
    if (Reply<void> read = read_tools(*response, tools); !read) {
      return std::unexpected(std::move(read).error());
    }
    const std::optional<JsonView> next = response->result.find("nextCursor");
    if (!next.has_value() || next->kind() == JsonKind::null) {
      return tools;
    }
    if (!next->string().has_value()) {
      return std::unexpected(
          fault(Fault::protocol, "the server's nextCursor is not a string"));
    }
    cursor = std::string{*next->string()};
  }
  return std::unexpected(
      fault(Fault::protocol, join({"the server's tool list did not end within ",
                                   std::to_string(max_pages), " pages"})));
}

Reply<ToolResult> Client::call_tool(const std::string_view name, const Json& arguments,
                                    const CallOptions& options) {
  if (state_ == nullptr) {
    return std::unexpected(moved_from());
  }
  const scrydantic::Result<JsonView> parsed = JsonView::parse(arguments);
  if (!parsed || parsed->kind() != JsonKind::object) {
    return std::unexpected(
        fault(Fault::protocol, "the arguments of a tool call must be a JSON object"));
  }
  // The arguments go as the caller wrote them, on one line.
  Reply<Response> response = state_->request(
      "tools/call",
      wire::CallToolParams{.meta = std::nullopt,
                           .arguments = {.text = verbatim::compact(arguments.text)},
                           .name = std::string{name}},
      name, options);
  if (!response) {
    return std::unexpected(std::move(response).error());
  }
  return read_tool_result(*response);
}

namespace detail {

ClientError unsendable(const scrydantic::Error& error) {
  return fault(Fault::protocol,
               join({"the arguments cannot be sent as JSON: ", error.message}));
}

ClientError tool_failed(const ToolResult& result) {
  std::string text;
  for (const ContentBlock& block : result.content) {
    if (const auto* const said = std::get_if<TextContent>(&block)) {
      if (!text.empty()) {
        text += '\n';
      }
      text += said->text;
    }
  }
  return fault(Fault::tool, text.empty() ? std::string{"the tool failed"} : text);
}

ClientError mismatched(const scrydantic::Error& error) {
  return fault(Fault::mismatch,
               join({"the result does not fit the type: ", error.message}));
}

Reply<Json> typed_source(const ToolResult& result, const bool text) {
  if (result.structured_content.has_value()) {
    return *result.structured_content;
  }
  const TextContent* const block =
      result.content.size() == 1 ? std::get_if<TextContent>(&result.content.front())
                                 : nullptr;
  if (block == nullptr) {
    return std::unexpected(
        fault(Fault::mismatch,
              "the result has no structured content and is not one text block"));
  }
  if (text) {
    // A string tool's text is the string itself, unless it is spelled as JSON.
    const scrydantic::Result<JsonView> spelled = JsonView::parse(block->text);
    return spelled && spelled->kind() == JsonKind::string
               ? Json{.text = block->text}
               : Json{.text = scrydantic::escape_json_string(block->text)};
  }
  if (!scrydantic::is_valid_json(block->text)) {
    return std::unexpected(fault(Fault::mismatch, "the result's text is not JSON"));
  }
  return Json{.text = block->text};
}

} // namespace detail

} // namespace mcp
