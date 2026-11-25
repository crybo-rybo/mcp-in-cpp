#include "answer.hpp"
#include "features.hpp"
#include "jsonrpc.hpp"
#include "routing.hpp"
#include "verbatim.hpp"
#include "wire.hpp"

#include <algorithm>
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
#include <list>
#include <map>
#include <mcp/ask.hpp>
#include <mcp/channel.hpp>
#include <mcp/client.hpp>
#include <mcp/context.hpp>
#include <mcp/http.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
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

// The time `span` after `start`, or the end of time for a span that reaches
// past it.
template <typename Span>
[[nodiscard]] Clock::time_point later(const Clock::time_point start, const Span span) {
  if (start == Clock::time_point::max() ||
      span >= std::chrono::duration_cast<Span>(Clock::time_point::max() - start)) {
    return Clock::time_point::max();
  }
  return start + span;
}

[[nodiscard]] Clock::time_point from_now(const std::chrono::milliseconds span) {
  return later(Clock::now(), span);
}

// The most requests of a server's that the client answers at once, each on a
// thread of its own. A server that asks more is told the client is busy.
constexpr std::size_t most_answering = 32;

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
  // moves the deadline, and a handler that runs moves it on by as long as it
  // ran.
  std::chrono::milliseconds limit{};
  Clock::time_point deadline{};
};

// The threads on which a client answers the requests of a handshake server,
// one for each request, so that a handler that takes minutes holds up neither
// the channel nor the other requests.
class Helpers {
public:
  Helpers() = default;
  Helpers(const Helpers&) = delete;
  Helpers& operator=(const Helpers&) = delete;
  ~Helpers() { stop(); }

  // Runs `work` on a thread of its own. Returns false, without running it,
  // once stop() has begun, when `most_answering` threads run already, or when
  // no thread can be started.
  bool start(std::function<void()> work) {
    const std::scoped_lock lock{mutex_};
    // A thread that has ended is joined before another starts, so that the
    // threads of a long session do not pile up.
    for (const auto ended : ended_) {
      ended->join();
      running_.erase(ended);
    }
    ended_.clear();
    if (stopping_ || running_.size() >= most_answering) {
      return false;
    }
    const auto slot = running_.emplace(running_.end());
    try {
      *slot = std::thread{[this, slot, work = std::move(work)] {
        work();
        const std::scoped_lock done{mutex_};
        if (!stopping_) {
          ended_.push_back(slot);
        }
      }};
    } catch (const std::system_error&) {
      running_.erase(slot);
      return false;
    }
    return true;
  }

  // Waits for every thread to end. None starts after.
  void stop() {
    std::list<std::thread> running;
    {
      const std::scoped_lock lock{mutex_};
      stopping_ = true;
      running = std::move(running_);
      ended_.clear();
    }
    for (std::thread& thread : running) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

private:
  std::mutex mutex_;
  bool stopping_{};
  std::list<std::thread> running_;
  std::vector<std::list<std::thread>::iterator> ended_;
};

// What a result of `input_required` asks: each question with its key, as the
// method and the params the server wrote, and the server's requestState as
// the JSON text it wrote, if it sent one.
struct Question {
  std::string key;
  std::string method;
  std::string params;
};

struct Inputs {
  std::vector<Question> questions;
  std::optional<std::string> state;
};

// Whether a request's params can carry the answers of `input_required`: those
// of tools/call, prompts/get and resources/read.
template <typename Params>
concept takes_input = requires(Params params) {
  params.input_responses = std::optional<Json>{};
  params.request_state = std::optional<Json>{};
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

  // Sends a request and waits for its result. `headers` go to a transport
  // that has them.
  template <typename Params>
  Reply<Response> request(std::string_view method, Params params, std::string_view name,
                          const CallOptions& call,
                          std::span<const HttpHeader> headers = {});

  // Under 2026-07-28, keeps what a tool list says of the arguments that each
  // tool mirrors into headers, and leaves out the tools whose marks break the
  // rules.
  void route(std::vector<Tool>& tools);
  // The headers of a call of this tool, as the last tool list marked its
  // arguments, or nothing when that list left the tool out.
  [[nodiscard]] std::optional<std::vector<HttpHeader>>
  routed(std::string_view tool, std::string_view arguments) const;

  // Ends the client: every waiting call fails, the handlers that run return,
  // and the channel closes.
  void shutdown();

  // In a handshake session, asks for the log messages of ClientOptions, once
  // the client has opened.
  Reply<void> set_log_level();

  // Follows the pages of a list, handing each result to `page`, until the
  // server gives no cursor, or fails with Fault::protocol after `max_pages`.
  // `what` names the list in that error.
  Reply<void> pages(std::string_view method,
                    const std::function<Reply<void>(const Response&)>& page,
                    std::string_view what, const CallOptions& call);

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
  // What the client declares: a capability for each handler it has.
  [[nodiscard]] wire::ClientCapabilities capabilities() const;

  // Starts the time of a request again from now, or while a handler runs
  // from when the first of them began, which resume() moves on. Takes
  // mutex_.
  void restart_clock(Pending& pending) const;
  // While a handler runs, no waiting call's time runs.
  void pause();
  void resume();
  class Paused;

  // Answers one question of the server's with the handler its method names.
  answer::Answered<std::string>
  answer_question(std::string_view method, std::string_view params, Revision revision);
  // Answers the questions of one round of input_required, as the JSON text
  // of its inputResponses.
  Reply<Json> answer_round(const Inputs& inputs, std::string_view method,
                           const std::stop_token& stop, Revision revision);
  // Waits a little before a round that answers nothing, unless the caller
  // stops the call. Returns whether it did.
  bool rest(std::chrono::milliseconds span, const std::stop_token& stop);

  // Sends a request without waiting for it.
  template <typename Params>
  Reply<std::shared_ptr<Pending>> start(std::string_view method, Params params,
                                        std::string_view name, const CallOptions& call,
                                        Revision revision,
                                        std::span<const HttpHeader> headers = {});
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
  // Hands a log message to ClientOptions::log.
  void logged(std::string_view text, const JsonView& document);
  void requested(std::string_view text, const JsonView& document,
                 std::string_view method, const JsonView& id);
  // Sends one notification or response, unless the client is closed.
  void post(std::string_view message, std::string_view method);

  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Pending>> pending_;
  // Why the client can make no more calls, once it cannot.
  std::optional<ClientError> gone_;
  Revision revision_{};
  std::atomic<std::uint64_t> next_id_{0};
  // How many handlers run, and since when the first of them has run.
  std::size_t pauses_{};
  Clock::time_point paused_since_{};
  Helpers helpers_;
  // What the last tool list marked of each tool's arguments for headers, and
  // the tools it left out for marks that break the rules.
  std::unordered_map<std::string, std::vector<routing::Param>> routes_;
  std::unordered_set<std::string> unroutable_;
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
        Fault::protocol, "the server asks for input on a request that takes none"));
  }
  return std::unexpected(
      fault(Fault::protocol,
            join({"the server's result has a resultType this client does not know: ",
                  type->to_json().text})));
}

[[nodiscard]] ClientError malformed_input(const std::string_view why) {
  return fault(Fault::protocol,
               join({"the server's input_required result is not valid: ", why}));
}

// What a modern result asks of the client: nothing when it is complete, or
// the questions and the state of `input_required`.
[[nodiscard]] Reply<std::optional<Inputs>> inputs_of(const Response& response) {
  const std::optional<JsonView> type = response.result.find("resultType");
  if (!type.has_value() || type->string() != "input_required") {
    if (Reply<void> done = complete(response); !done) {
      return std::unexpected(std::move(done).error());
    }
    return std::nullopt;
  }
  Inputs inputs;
  const std::optional<JsonView> requests = response.result.find("inputRequests");
  const std::optional<std::string_view> written =
      verbatim::member(response.text, "inputRequests");
  if (requests.has_value()) {
    if (requests->kind() != JsonKind::object || !written.has_value()) {
      return std::unexpected(malformed_input("inputRequests is not an object"));
    }
    for (std::size_t index = 0; index < requests->size(); ++index) {
      const std::string_view key = requests->key_at(index).value_or("");
      const JsonView request = *requests->find(key);
      const std::optional<std::string_view> method =
          is_object(request) ? text_of(request, "method") : std::nullopt;
      const std::optional<JsonView> params =
          method.has_value() ? request.find("params") : std::nullopt;
      if (!method.has_value() || (params.has_value() && !is_object(params))) {
        return std::unexpected(
            malformed_input(join({"inputRequests[", scrydantic::escape_json_string(key),
                                  "] needs a method and params that are an object"})));
      }
      const std::optional<std::string_view> raw = verbatim::member(*written, key);
      const std::optional<std::string_view> raw_params =
          raw.has_value() ? verbatim::member(*raw, "params") : std::nullopt;
      inputs.questions.push_back(
          Question{.key = std::string{key},
                   .method = std::string{*method},
                   .params = std::string{raw_params.value_or("{}")}});
    }
  }
  if (const std::optional<JsonView> state = response.result.find("requestState")) {
    if (state->kind() != JsonKind::string) {
      return std::unexpected(malformed_input("requestState is not a string"));
    }
    // The state goes back exactly as the server wrote it.
    inputs.state = std::string{*verbatim::member(response.text, "requestState")};
  }
  if (inputs.questions.empty() && !inputs.state.has_value()) {
    return std::unexpected(malformed_input("it asks nothing and has no requestState"));
  }
  return inputs;
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

wire::ClientCapabilities Client::State::capabilities() const {
  wire::ClientCapabilities declared{};
  if (options.elicitation) {
    // Form mode: the client does not open URLs.
    declared.elicitation = wire::ElicitationCapability{.form = wire::FormCapability{}};
  }
  if (options.roots) {
    // The client never says its roots changed, so it does not declare
    // listChanged.
    declared.roots = wire::RootsCapability{};
  }
  if (options.sampling) {
    declared.sampling = wire::SamplingCapability{};
  }
  return declared;
}

void Client::State::restart_clock(Pending& pending) const {
  pending.deadline = later(pauses_ > 0 ? paused_since_ : Clock::now(), pending.limit);
}

void Client::State::pause() {
  const std::scoped_lock lock{mutex_};
  if (pauses_++ == 0) {
    paused_since_ = Clock::now();
  }
}

void Client::State::resume() {
  {
    const std::scoped_lock lock{mutex_};
    if (--pauses_ == 0) {
      // Every waiting call gets back the time it waited while handlers ran.
      const Clock::duration paused = Clock::now() - paused_since_;
      for (auto& [id, pending] : pending_) {
        pending->deadline = later(pending->deadline, paused);
      }
    }
  }
  changed_.notify_all();
}

// Pauses the time of waiting calls while it lives.
class Client::State::Paused {
public:
  explicit Paused(State& state) : state_{&state} { state_->pause(); }
  Paused(const Paused&) = delete;
  Paused& operator=(const Paused&) = delete;
  ~Paused() { state_->resume(); }

private:
  State* state_;
};

template <typename Params>
Reply<std::shared_ptr<Pending>>
Client::State::start(const std::string_view method, Params params,
                     const std::string_view name, const CallOptions& call,
                     const Revision revision,
                     const std::span<const HttpHeader> headers) {
  const std::uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed) + 1;
  const std::string id_text = std::to_string(id);
  wire::RequestMeta meta{};
  if (revision.modern) {
    meta.client_capabilities = capabilities();
    meta.client_info =
        wire::Implementation{.name = options.name, .version = options.version};
    meta.protocol_version = std::string{revision.version};
    // A server of 2026-07-28 logs only for a request that asks.
    if (options.log) {
      meta.log_level = call.log_level.value_or(options.log_level);
    }
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
  {
    const std::scoped_lock lock{mutex_};
    if (gone_.has_value()) {
      return std::unexpected(*gone_);
    }
    restart_clock(*pending);
    pending_.emplace(id, pending);
  }
  const Reply<void> sent = channel->send(Outgoing{.message = message,
                                                  .id = id_text,
                                                  .method = method,
                                                  .name = name,
                                                  .protocol_version = revision.version,
                                                  .headers = headers});
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
    // While a handler runs, the time does not.
    const bool paused = pauses_ > 0;
    const bool limited = pending.limit.count() > 0 && !paused;
    if (limited && now >= pending.deadline) {
      return Waited::timeout;
    }
    if (until.has_value() && now >= *until) {
      return Waited::until;
    }
    std::optional<Clock::time_point> wake = until;
    if (limited && (!wake.has_value() || pending.deadline < *wake)) {
      wake = pending.deadline;
    }
    // A handler that begins or ends changes how long to wait.
    const auto woken = [this, &pending, paused] {
      return pending.done || (pauses_ > 0) != paused;
    };
    if (wake.has_value() && *wake != Clock::time_point::max()) {
      changed_.wait_until(lock, stop, *wake, woken);
    } else {
      changed_.wait(lock, stop, woken);
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
                                       const CallOptions& call,
                                       const std::span<const HttpHeader> headers) {
  const auto cancelled = [method] {
    return std::unexpected(fault(Fault::cancelled, join({method, " was cancelled"})));
  };
  if (call.stop.stop_requested()) {
    return cancelled();
  }
  const Revision sent_as = revision();
  // How long to wait before a round that answers nothing, which a server
  // sends to have the request again later.
  constexpr std::chrono::milliseconds first_rest{50};
  constexpr std::chrono::milliseconds longest_rest{250};
  std::chrono::milliseconds rest_span = first_rest;
  for (std::size_t round = 0;; ++round) {
    // Each round is a request of its own, with an id of its own.
    Reply<std::shared_ptr<Pending>> pending =
        start(method, params, name, call, sent_as, headers);
    if (!pending) {
      return std::unexpected(std::move(pending).error());
    }
    Reply<Response> response = finish(*pending, method, call.stop);
    if (!response || !sent_as.modern) {
      return response;
    }
    Reply<std::optional<Inputs>> asked = inputs_of(*response);
    if (!asked) {
      return std::unexpected(std::move(asked).error());
    }
    if (!asked->has_value()) {
      return response;
    }
    if constexpr (!takes_input<Params>) {
      return std::unexpected(
          fault(Fault::protocol, join({"the server asks for input on ", method,
                                       ", which the protocol does not allow"})));
    } else {
      if (round >= options.input_rounds) {
        return std::unexpected(
            fault(Fault::protocol, join({"the server still asks for input after ",
                                         std::to_string(round), " rounds"})));
      }
      const Inputs& inputs = **asked;
      if (inputs.questions.empty()) {
        if (!rest(rest_span, call.stop)) {
          return cancelled();
        }
        rest_span = std::min(rest_span * 2, longest_rest);
      } else {
        rest_span = first_rest;
      }
      Reply<Json> answers = answer_round(inputs, method, call.stop, sent_as);
      if (!answers) {
        return std::unexpected(std::move(answers).error());
      }
      if (call.stop.stop_requested()) {
        return cancelled();
      }
      params.input_responses =
          inputs.questions.empty() ? std::nullopt : std::optional{std::move(*answers)};
      // Echoed exactly, and left out when the server sent none.
      params.request_state = inputs.state.has_value()
                                 ? std::optional{Json{.text = *inputs.state}}
                                 : std::nullopt;
    }
  }
}

bool Client::State::rest(const std::chrono::milliseconds span,
                         const std::stop_token& stop) {
  std::unique_lock lock{mutex_};
  // Nothing wakes it but the caller's stop, or the client's end.
  changed_.wait_for(lock, stop, span, [this] { return gone_.has_value(); });
  return !stop.stop_requested();
}

Reply<Json> Client::State::answer_round(const Inputs& inputs,
                                        const std::string_view method,
                                        const std::stop_token& stop,
                                        const Revision revision) {
  std::string responses{"{"};
  for (const Question& question : inputs.questions) {
    if (stop.stop_requested()) {
      return std::unexpected(fault(Fault::cancelled, join({method, " was cancelled"})));
    }
    answer::Answered<std::string> answered =
        answer_question(question.method, question.params, revision);
    if (!answered) {
      return std::unexpected(fault(answered.error().fault, answered.error().reason));
    }
    if (responses.size() > 1) {
      responses += ',';
    }
    responses += scrydantic::escape_json_string(question.key);
    responses += ':';
    responses += *answered;
  }
  responses += '}';
  return Json{.text = std::move(responses)};
}

answer::Answered<std::string>
Client::State::answer_question(const std::string_view method,
                               const std::string_view params, const Revision revision) {
  // Runs a handler while the time of waiting calls stands still. What it
  // throws is the client's own failure, which the server is not told of.
  const auto run = [this](const std::string_view handler, auto&& function)
      -> answer::Answered<std::invoke_result_t<decltype(function)>> {
    try {
      const Paused paused{*this};
      return answer::Answered<std::invoke_result_t<decltype(function)>>{std::in_place,
                                                                        function()};
    } catch (const std::exception& error) {
      return std::unexpected(answer::threw(handler, error.what()));
    } catch (...) {
      return std::unexpected(
          answer::threw(handler, "an exception that is not a std::exception"));
    }
  };
  if (method == "elicitation/create" && options.elicitation) {
    answer::Answered<FormRequest> form = answer::read_form(params);
    if (!form) {
      return std::unexpected(std::move(form).error());
    }
    answer::Answered<Answer<Json>> given =
        run("elicitation", [this, &form] { return options.elicitation(*form); });
    if (!given) {
      return std::unexpected(std::move(given).error());
    }
    return answer::write_form(*given, form->schema.text);
  }
  if (method == "sampling/createMessage" && options.sampling) {
    answer::Answered<SamplingRequest> request = answer::read_sampling(params);
    if (!request) {
      return std::unexpected(std::move(request).error());
    }
    answer::Answered<Result<SamplingResult>> sampled =
        run("sampling", [this, &request] { return options.sampling(*request); });
    if (!sampled) {
      return std::unexpected(std::move(sampled).error());
    }
    if (!*sampled) {
      return std::unexpected(answer::refused(sampled->error().message));
    }
    return answer::write_sampling(**sampled, revision.version != "2025-06-18");
  }
  if (method == "roots/list" && options.roots) {
    answer::Answered<std::vector<Root>> roots =
        run("roots", [this] { return options.roots(); });
    if (!roots) {
      return std::unexpected(std::move(roots).error());
    }
    return answer::write_roots(*roots);
  }
  return std::unexpected(answer::not_declared(method));
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
  // A handler that runs returns before the channel closes, so that nothing it
  // sends meets a closed channel; whatever it calls fails at once now.
  helpers_.stop();
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
    requested(text, *document, *name, *id);
  } else if (*name == "notifications/message") {
    logged(text, *document);
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
    restart_clock(*pending);
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

void Client::State::requested(const std::string_view text, const JsonView& document,
                              const std::string_view method, const JsonView& id) {
  const JsonKind kind = id.kind();
  if (kind != JsonKind::string && kind != JsonKind::signed_integer &&
      kind != JsonKind::unsigned_integer) {
    return;
  }
  const std::string id_text = id.to_json().text;
  // A legacy server may ping its client.
  if (method == "ping") {
    post(result_message(id_text, "{}"), {});
    return;
  }
  // Only a handshake session asks with requests of the server's own, once the
  // handshake has agreed a revision. A question no handler is set for is a
  // method the client does not have.
  const Revision current = revision();
  const bool handled = (method == "elicitation/create" && options.elicitation) ||
                       (method == "sampling/createMessage" && options.sampling) ||
                       (method == "roots/list" && options.roots);
  if (current.modern || current.version.empty() || !handled) {
    const answer::Refusal refusal = answer::not_declared(method);
    post(error_message(id_text, refusal.code, refusal.message), {});
    return;
  }
  const std::optional<JsonView> params = document.find("params");
  if (params.has_value() && params->kind() != JsonKind::object) {
    post(error_message(id_text, wire::invalid_params,
                       "Invalid params: params must be an object"),
         {});
    return;
  }
  // The handler may take minutes and may call the client, so it runs on a
  // thread of its own, never on the channel's.
  const bool started = helpers_.start(
      [this, id_text, method = std::string{method},
       params = std::string{verbatim::member(text, "params").value_or("{}")}, current] {
        const answer::Answered<std::string> answered =
            answer_question(method, params, current);
        post(answered ? result_message(id_text, *answered)
                      : error_message(id_text, answered.error().code,
                                      answered.error().message),
             {});
      });
  if (!started) {
    post(error_message(id_text, wire::internal_error,
                       "Internal error: the client answers too many requests"),
         {});
  }
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
                   .capabilities = capabilities(),
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
  // Qualified, since Client::complete hides it here.
  if (Reply<void> done = mcp::complete(*answer); !done) {
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

// The headers of tool calls.

void Client::State::route(std::vector<Tool>& tools) {
  if (!revision().modern) {
    // Only 2026-07-28 mirrors arguments into headers.
    return;
  }
  std::unordered_map<std::string, std::vector<routing::Param>> routes;
  std::unordered_set<std::string> unroutable;
  // A tool whose marks break the rules cannot be called as the protocol
  // requires, so it is left out; the others stay.
  std::erase_if(tools, [&](const Tool& tool) {
    std::expected<std::vector<routing::Param>, std::string> params =
        routing::params_of(tool.input_schema.text);
    if (!params) {
      unroutable.insert(tool.name);
      return true;
    }
    if (!params->empty()) {
      routes.try_emplace(tool.name, std::move(*params));
    }
    return false;
  });
  const std::scoped_lock lock{mutex_};
  routes_ = std::move(routes);
  unroutable_ = std::move(unroutable);
}

std::optional<std::vector<HttpHeader>>
Client::State::routed(const std::string_view tool,
                      const std::string_view arguments) const {
  std::vector<routing::Param> params;
  {
    const std::scoped_lock lock{mutex_};
    const std::string name{tool};
    if (unroutable_.contains(name)) {
      return std::nullopt;
    }
    if (const auto found = routes_.find(name); found != routes_.end()) {
      params = found->second;
    }
  }
  return routing::headers_for(params, arguments);
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
  if (Reply<void> asked = state->set_log_level(); !asked) {
    return std::unexpected(std::move(asked).error());
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
      state_->route(tools);
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
  const std::string compacted = verbatim::compact(arguments.text);
  for (bool retried = false;; retried = true) {
    const std::optional<std::vector<HttpHeader>> headers =
        state_->routed(name, compacted);
    if (!headers.has_value()) {
      return std::unexpected(
          fault(Fault::protocol, join({"the tool ", name,
                                       " has x-mcp-header marks that break the "
                                       "protocol's rules, so it cannot be called"})));
    }
    Reply<Response> response =
        state_->request("tools/call",
                        wire::CallToolParams{.meta = std::nullopt,
                                             .arguments = {.text = compacted},
                                             .input_responses = std::nullopt,
                                             .name = std::string{name},
                                             .request_state = std::nullopt},
                        name, options, *headers);
    if (!response && !retried && response.error().fault == Fault::rejected &&
        response.error().code == header_mismatch && state_->info.era == Era::modern) {
      // The tool's schema may have changed since the client listed it: its
      // marks are read again, and the call is sent once more.
      const Reply<std::vector<Tool>> listed = list_tools(
          {.progress = {}, .stop = options.stop, .timeout = options.timeout});
      if (listed) {
        continue;
      }
    }
    if (!response) {
      return std::unexpected(std::move(response).error());
    }
    return read_tool_result(*response);
  }
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

std::optional<Json> wrapped_result(const ToolResult& result) {
  if (!result.structured_content.has_value()) {
    return std::nullopt;
  }
  const std::string& text = result.structured_content->text;
  const scrydantic::Result<JsonView> wrapper = JsonView::parse(text);
  if (!wrapper || wrapper->kind() != JsonKind::object || wrapper->size() != 1) {
    return std::nullopt;
  }
  const std::optional<std::string_view> value = verbatim::member(text, "result");
  if (!value.has_value()) {
    return std::nullopt;
  }
  return Json{.text = std::string{*value}};
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

// Resources, prompts, completion and log messages.

Reply<void> Client::State::set_log_level() {
  const Revision current = revision();
  if (current.modern || !options.log || !info.has("logging")) {
    return {};
  }
  Reply<Response> set =
      request("logging/setLevel",
              wire::SetLevelParams{.meta = {}, .level = options.log_level}, {}, {});
  // Only a server that is gone or does not answer fails the opening. One that
  // refuses the level logs as it likes, and the client still passes over what
  // is less severe than it asked for.
  if (!set &&
      (set.error().fault == Fault::transport || set.error().fault == Fault::timeout)) {
    return std::unexpected(std::move(set).error());
  }
  return {};
}

void Client::State::logged(const std::string_view text, const JsonView& document) {
  if (!options.log) {
    return;
  }
  const std::optional<JsonView> params = document.find("params");
  const std::optional<std::string_view> written = verbatim::member(text, "params");
  if (!is_object(params) || !written.has_value()) {
    return;
  }
  const std::optional<LogMessage> message = features::read_log(*written, *params);
  // Under 2026-07-28 each request asks for its own level, which the server
  // keeps to; a handshake session has the one level the client set.
  if (!message.has_value() ||
      (!revision().modern && message->level < options.log_level)) {
    return;
  }
  try {
    options.log(*message);
  } catch (...) {
    // The caller's own function failed. It is not the server's doing.
    static_cast<void>(std::current_exception());
  }
}

Reply<void>
Client::State::pages(const std::string_view method,
                     const std::function<Reply<void>(const Response&)>& page,
                     const std::string_view what, const CallOptions& call) {
  std::optional<std::string> cursor;
  for (std::size_t count = 0; count < max_pages; ++count) {
    Reply<Response> response = request(
        method, wire::ListParams{.meta = std::nullopt, .cursor = std::move(cursor)}, {},
        call);
    if (!response) {
      return std::unexpected(std::move(response).error());
    }
    if (Reply<void> read = page(*response); !read) {
      return read;
    }
    const std::optional<JsonView> next = response->result.find("nextCursor");
    if (!next.has_value() || next->kind() == JsonKind::null) {
      return {};
    }
    if (!next->string().has_value()) {
      return std::unexpected(
          fault(Fault::protocol, "the server's nextCursor is not a string"));
    }
    cursor = std::string{*next->string()};
  }
  return std::unexpected(
      fault(Fault::protocol, join({"the server's ", what, " list did not end within ",
                                   std::to_string(max_pages), " pages"})));
}

Reply<std::vector<Resource>> Client::list_resources(const CallOptions& options) {
  if (state_ == nullptr) {
    return std::unexpected(moved_from());
  }
  std::vector<Resource> resources;
  Reply<void> listed = state_->pages(
      "resources/list",
      [&resources](const Response& response) {
        return features::read_resources(response.text, response.result, resources);
      },
      "resource", options);
  if (!listed) {
    return std::unexpected(std::move(listed).error());
  }
  return resources;
}

Reply<std::vector<ResourceTemplate>>
Client::list_resource_templates(const CallOptions& options) {
  if (state_ == nullptr) {
    return std::unexpected(moved_from());
  }
  std::vector<ResourceTemplate> templates;
  Reply<void> listed = state_->pages(
      "resources/templates/list",
      [&templates](const Response& response) {
        return features::read_templates(response.text, response.result, templates);
      },
      "resource template", options);
  if (!listed) {
    return std::unexpected(std::move(listed).error());
  }
  return templates;
}

Reply<std::vector<ResourceContents>> Client::read_resource(const std::string_view uri,
                                                           const CallOptions& options) {
  if (state_ == nullptr) {
    return std::unexpected(moved_from());
  }
  Reply<Response> response =
      state_->request("resources/read",
                      wire::ReadResourceParams{.meta = std::nullopt,
                                               .input_responses = std::nullopt,
                                               .request_state = std::nullopt,
                                               .uri = std::string{uri}},
                      uri, options);
  if (!response) {
    return std::unexpected(std::move(response).error());
  }
  return features::read_contents(response->result);
}

Reply<std::vector<Prompt>> Client::list_prompts(const CallOptions& options) {
  if (state_ == nullptr) {
    return std::unexpected(moved_from());
  }
  std::vector<Prompt> prompts;
  Reply<void> listed = state_->pages(
      "prompts/list",
      [&prompts](const Response& response) {
        return features::read_prompts(response.text, response.result, prompts);
      },
      "prompt", options);
  if (!listed) {
    return std::unexpected(std::move(listed).error());
  }
  return prompts;
}

Reply<PromptResult>
Client::get_prompt(const std::string_view name,
                   const std::map<std::string, std::string>& arguments,
                   const CallOptions& options) {
  if (state_ == nullptr) {
    return std::unexpected(moved_from());
  }
  Reply<Response> response = state_->request(
      "prompts/get",
      wire::GetPromptParams{.meta = std::nullopt,
                            .arguments = arguments.empty() ? std::nullopt
                                                           : std::optional{arguments},
                            .input_responses = std::nullopt,
                            .name = std::string{name},
                            .request_state = std::nullopt},
      name, options);
  if (!response) {
    return std::unexpected(std::move(response).error());
  }
  return features::read_prompt(response->result);
}

namespace {

[[nodiscard]] wire::CompleteParams
complete_params(wire::CompletionReference reference, const std::string_view argument,
                const std::string_view value,
                const std::map<std::string, std::string>& context) {
  return {.meta = std::nullopt,
          .argument = {.name = std::string{argument}, .value = std::string{value}},
          .context = context.empty()
                         ? std::nullopt
                         : std::optional{wire::CompletionContext{.arguments = context}},
          .ref = std::move(reference)};
}

} // namespace

Reply<Completion> Client::complete(const PromptReference& prompt,
                                   const std::string_view argument,
                                   const std::string_view value,
                                   const std::map<std::string, std::string>& context,
                                   const CallOptions& options) {
  if (state_ == nullptr) {
    return std::unexpected(moved_from());
  }
  Reply<Response> response = state_->request(
      "completion/complete",
      complete_params({.name = prompt.name, .type = "ref/prompt", .uri = std::nullopt},
                      argument, value, context),
      {}, options);
  if (!response) {
    return std::unexpected(std::move(response).error());
  }
  return features::read_completion(response->result);
}

Reply<Completion> Client::complete(const ResourceTemplateReference& resource,
                                   const std::string_view variable,
                                   const std::string_view value,
                                   const std::map<std::string, std::string>& context,
                                   const CallOptions& options) {
  if (state_ == nullptr) {
    return std::unexpected(moved_from());
  }
  Reply<Response> response = state_->request(
      "completion/complete",
      complete_params(
          {.name = std::nullopt, .type = "ref/resource", .uri = resource.uri}, variable,
          value, context),
      {}, options);
  if (!response) {
    return std::unexpected(std::move(response).error());
  }
  return features::read_completion(response->result);
}

} // namespace mcp
