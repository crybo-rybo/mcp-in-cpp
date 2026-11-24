#include "inquiry.hpp"
#include "jsonrpc.hpp"
#include "sha256.hpp"
#include "wire.hpp"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <mcp/ask.hpp>
#include <mcp/server.hpp>
#include <mutex>
#include <optional>
#include <random>
#include <scrydantic/scrydantic.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

namespace {

using jsonrpc::error_message;
using jsonrpc::join;
using jsonrpc::notification_message;
using jsonrpc::result_message;

using scrydantic::JsonKind;
using scrydantic::JsonView;

constexpr std::string_view protocol_version_key =
    "io.modelcontextprotocol/protocolVersion";
constexpr std::string_view client_capabilities_key =
    "io.modelcontextprotocol/clientCapabilities";
constexpr std::string_view log_level_key = "io.modelcontextprotocol/logLevel";

[[nodiscard]] bool is_object(const std::optional<JsonView>& value) noexcept;

// The lifetime of a requestState: how long a client may take to answer.
constexpr std::chrono::seconds state_lifetime{600};

[[nodiscard]] std::string base64url(const std::string_view bytes) {
  std::string text = base64(bytes);
  while (!text.empty() && text.back() == '=') {
    text.pop_back();
  }
  for (char& c : text) {
    c = c == '+' ? '-' : c == '/' ? '_' : c;
  }
  return text;
}

[[nodiscard]] std::optional<std::string> from_base64url(const std::string_view text) {
  std::string bytes;
  std::uint32_t buffer = 0;
  unsigned bits = 0;
  for (const char c : text) {
    std::uint32_t value = 0;
    if (c >= 'A' && c <= 'Z') {
      value = static_cast<std::uint32_t>(c - 'A');
    } else if (c >= 'a' && c <= 'z') {
      value = static_cast<std::uint32_t>(c - 'a' + 26);
    } else if (c >= '0' && c <= '9') {
      value = static_cast<std::uint32_t>(c - '0' + 52);
    } else if (c == '-') {
      value = 62;
    } else if (c == '_') {
      value = 63;
    } else {
      return std::nullopt;
    }
    buffer = (buffer << 6U) | value;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes += static_cast<char>((buffer >> bits) & 0xFFU);
    }
  }
  return bytes;
}

[[nodiscard]] std::string digest_text(const crypto::Digest& digest) {
  return std::string{reinterpret_cast<const char*>(digest.data()), digest.size()};
}

// What a requestState is bound to: the method and the request it answers.
[[nodiscard]] std::string state_binding(const std::string_view method,
                                        const std::string_view target,
                                        const std::string_view arguments) {
  return base64url(
      digest_text(crypto::sha256(join({method, "\n", target, "\n", arguments}))));
}

[[nodiscard]] std::int64_t unix_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// The requestState that carries a call's answers to its next round: the
// answers, what they are bound to, and when they expire, signed with the
// server's key.
[[nodiscard]] std::string
seal_state(const std::string_view key,
           const std::unordered_map<std::string, std::string>& answers,
           const std::string_view binding) {
  std::string payload{R"({"answers":{)"};
  bool first = true;
  for (const auto& [name, answer] : answers) {
    if (!first) {
      payload += ',';
    }
    first = false;
    payload += scrydantic::escape_json_string(name);
    payload += ':';
    payload += answer;
  }
  payload += R"(},"binding":")";
  payload += binding;
  payload += R"(","expires":)";
  payload += std::to_string(unix_seconds() + state_lifetime.count());
  payload += '}';
  return base64url(payload) + "." +
         base64url(digest_text(crypto::hmac_sha256(key, payload)));
}

// The answers a requestState carries, or nothing when it is not one this
// server signed for this request, or it has expired.
[[nodiscard]] std::optional<std::unordered_map<std::string, std::string>>
open_state(const std::string_view key, const std::string_view state,
           const std::string_view binding) {
  const std::size_t dot = state.find('.');
  if (dot == std::string_view::npos) {
    return std::nullopt;
  }
  const std::optional<std::string> payload = from_base64url(state.substr(0, dot));
  const std::optional<std::string> mac = from_base64url(state.substr(dot + 1));
  if (!payload.has_value() || !mac.has_value() || mac->size() != 32) {
    return std::nullopt;
  }
  crypto::Digest given{};
  std::copy(mac->begin(), mac->end(), reinterpret_cast<char*>(given.data()));
  if (!crypto::equal(given, crypto::hmac_sha256(key, *payload))) {
    return std::nullopt;
  }
  const scrydantic::Result<JsonView> document = JsonView::parse(*payload);
  if (!document) {
    return std::nullopt;
  }
  const std::optional<JsonView> bound = document->find("binding");
  const std::optional<JsonView> expires = document->find("expires");
  const std::optional<JsonView> answers = document->find("answers");
  if (!bound.has_value() || bound->string() != binding || !expires.has_value() ||
      expires->number().value_or(0) < static_cast<double>(unix_seconds()) ||
      !is_object(answers)) {
    return std::nullopt;
  }
  std::unordered_map<std::string, std::string> found;
  for (std::size_t index = 0; index < answers->size(); ++index) {
    const std::string_view name = answers->key_at(index).value_or("");
    found.emplace(name, answers->find(name)->to_json().text);
  }
  return found;
}

// Whether a client's capabilities let it be asked each kind of question.
struct ClientAbilities {
  bool elicitation{};
  bool sampling{};
  bool roots{};
};

[[nodiscard]] ClientAbilities
abilities_in(const std::optional<JsonView>& capabilities) {
  if (!is_object(capabilities)) {
    return {};
  }
  const std::optional<JsonView> elicitation = capabilities->find("elicitation");
  // An elicitation capability without `form` still has it, unless it names
  // only `url`.
  const bool form = is_object(elicitation) && (elicitation->find("form").has_value() ||
                                               !elicitation->find("url").has_value());
  return {.elicitation = form,
          .sampling = capabilities->find("sampling").has_value(),
          .roots = capabilities->find("roots").has_value()};
}

// How many resources one session may subscribe to.
constexpr std::size_t most_subscriptions = 1024;

// The resource a URI names: one of that URI, or else the first template it
// matches, whose variables then go to `variables`.
[[nodiscard]] const BoundResource*
find_resource(const std::span<const BoundResource> resources,
              const std::string_view uri, std::string* const variables = nullptr) {
  for (const BoundResource& resource : resources) {
    if (!resource.is_template && resource.uri == uri) {
      return &resource;
    }
  }
  for (const BoundResource& resource : resources) {
    if (!resource.is_template) {
      continue;
    }
    if (std::optional<std::string> matched = match_uri_template(resource.uri, uri)) {
      if (variables != nullptr) {
        *variables = std::move(*matched);
      }
      return &resource;
    }
  }
  return nullptr;
}

[[nodiscard]] std::optional<LogLevel> log_level_named(const std::string_view name) {
  constexpr std::string_view names[] = {"debug", "info",     "notice", "warning",
                                        "error", "critical", "alert",  "emergency"};
  for (std::size_t level = 0; level < std::size(names); ++level) {
    if (names[level] == name) {
      return static_cast<LogLevel>(level);
    }
  }
  return std::nullopt;
}

// What a client is told when a call fails for a reason it cannot act on. The
// reason itself goes to the server's log.
constexpr std::string_view tool_failed = "the tool failed";

// A message's result object as JSON text, or nothing when the value holds a
// string that is not UTF-8, which only a server's or a tool's own text can be.
template <typename Value>
[[nodiscard]] std::optional<std::string> encoded(const Value& value) {
  scrydantic::Result<Json> json = scrydantic::encode_verbatim(value);
  if (!json) {
    return std::nullopt;
  }
  return std::move(json->text);
}

[[nodiscard]] bool is_request_id(const JsonView& id) noexcept {
  const JsonKind kind = id.kind();
  return kind == JsonKind::string || kind == JsonKind::signed_integer ||
         kind == JsonKind::unsigned_integer;
}

[[nodiscard]] bool is_object(const std::optional<JsonView>& value) noexcept {
  return value.has_value() && value->kind() == JsonKind::object;
}

[[nodiscard]] bool describes_an_object(const Json& schema) {
  const scrydantic::Result<JsonView> view = JsonView::parse(schema);
  if (!view || view->kind() != JsonKind::object) {
    return false;
  }
  const std::optional<JsonView> type = view->find("type");
  return type.has_value() && type->string() == "object";
}

[[nodiscard]] const std::string_view*
legacy_version_named(const std::string_view version) noexcept {
  for (const std::string_view& known : legacy_protocol_versions) {
    if (known == version) {
      return &known;
    }
  }
  return nullptr;
}

[[nodiscard]] std::vector<std::string> supported_versions() {
  std::vector<std::string> versions{std::string{modern_protocol_version}};
  for (const std::string_view version : legacy_protocol_versions) {
    versions.emplace_back(version);
  }
  return versions;
}

// What a tool's text content says: a string as it is, and any other value as
// its JSON.
[[nodiscard]] std::string content_text(const Json& result) {
  if (result.text.starts_with('"')) {
    const scrydantic::Result<JsonView> view = JsonView::parse(result);
    if (view) {
      if (const std::optional<std::string_view> text = view->string()) {
        return std::string{*text};
      }
    }
  }
  return result.text;
}

} // namespace

struct Progress::State {
  const Notify* notify{};
  std::string token{};
  // Reports may come from several threads of one call.
  std::mutex mutex;
  bool reported{};
  double last{};
};

void Progress::report(const double progress, std::optional<double> total,
                      const std::string_view message) const {
  if (state_ == nullptr) {
    return;
  }
  const std::scoped_lock lock{state_->mutex};
  if (!std::isfinite(progress) || (state_->reported && progress <= state_->last)) {
    return;
  }
  if (total.has_value() && !std::isfinite(*total)) {
    total.reset();
  }
  wire::ProgressParams params{
      .progress_token = Json{.text = state_->token},
      .progress = progress,
      .total = total,
      .message = std::nullopt,
  };
  if (!message.empty()) {
    params.message = std::string{message};
  }
  std::optional<std::string> text = encoded(params);
  if (!text && params.message.has_value()) {
    // The note is not UTF-8. The report still counts without it.
    params.message.reset();
    text = encoded(params);
  }
  if (!text) {
    return;
  }
  state_->reported = true;
  state_->last = progress;
  (*state_->notify)(notification_message("notifications/progress", *text));
}

struct Logger::State {
  const Notify* notify{};
  std::optional<LogLevel> threshold{};
};

bool Logger::wants(const LogLevel level) const noexcept {
  return state_ != nullptr &&
         (!state_->threshold.has_value() || level >= *state_->threshold);
}

void Logger::log(const LogLevel level, const std::string_view message) const {
  if (!wants(level)) {
    return;
  }
  const std::string data = scrydantic::escape_json_string(message);
  log(level, Json{.text = data});
}

void Logger::log(const LogLevel level, const Json& data) const {
  if (!wants(level)) {
    return;
  }
  const std::optional<std::string> params =
      encoded(wire::LogParams{.data = data, .level = level});
  if (params.has_value()) {
    (*state_->notify)(notification_message("notifications/message", *params));
  }
}

struct Connection::Call {
  Progress::State progress{};
  Logger::State logger{};
  detail::Inquiry inquiry{};
};

// A request of the server's to the client, waiting for its response.
struct Connection::Waiting {
  std::mutex mutex;
  std::condition_variable_any done;
  // The response, once it came.
  std::optional<std::string> response;
};

namespace detail {

struct Registry {
  std::mutex mutex;
  std::vector<Connection*> connections;
};

} // namespace detail

Server::Server(std::string name, std::string version)
    : name_{std::move(name)}, version_{std::move(version)},
      log_{[](const std::string_view line) { std::cerr << line << '\n'; }},
      registry_{std::make_shared<detail::Registry>()} {
  std::random_device device;
  for (int word = 0; word < 8; ++word) {
    const std::uint32_t value = device();
    for (unsigned shift = 0; shift < 32; shift += 8) {
      state_key_ += static_cast<char>((value >> shift) & 0xFFU);
    }
  }
}

void Server::resource_updated(const std::string_view uri) const {
  const std::scoped_lock lock{registry_->mutex};
  for (const Connection* const connection : registry_->connections) {
    connection->resource_updated(uri);
  }
}

Connection::Connection(const Server& server) : server_{&server} {
  const std::scoped_lock lock{server.registry_->mutex};
  server.registry_->connections.push_back(this);
}

Connection::~Connection() {
  const std::scoped_lock lock{server_->registry_->mutex};
  std::erase(server_->registry_->connections, this);
}

void Connection::client_gone() { gone_.request_stop(); }

void Connection::push_to(Notify sink) {
  const std::scoped_lock lock{push_mutex_};
  push_ = std::move(sink);
}

void Connection::resource_updated(const std::string_view uri) const {
  const std::scoped_lock lock{push_mutex_};
  if (!push_ || !subscriptions_.contains(std::string{uri})) {
    return;
  }
  push_(notification_message(
      "notifications/resources/updated",
      join({R"({"uri":)", scrydantic::escape_json_string(uri), "}"})));
}

Server& Server::request_state_key(std::string key) {
  if (key.size() < 32 && !problem_.has_value()) {
    problem_ = scrydantic::Error{};
    problem_->message = "a request state key must be at least 32 bytes";
  }
  state_key_ = std::move(key);
  return *this;
}

Server& Server::instructions(std::string text) {
  instructions_ = std::move(text);
  return *this;
}

Server& Server::list_ttl(const std::chrono::milliseconds ttl) {
  list_ttl_ = ttl.count() < 0 ? std::chrono::milliseconds{0} : ttl;
  return *this;
}

Server& Server::log(Log sink) {
  log_ = std::move(sink);
  return *this;
}

Server& Server::add(std::vector<scrydantic::BoundTool<Context>> tools) {
  return add_bound(std::move(tools), {});
}

Server& Server::add_bound(std::vector<scrydantic::BoundTool<Context>> tools,
                          const std::span<const bool> content) {
  if (problem_.has_value()) {
    return *this;
  }
  if (!content.empty() && content.size() != tools.size()) {
    problem_ = scrydantic::Error{};
    problem_->message = "the tools bound and the tools found differ";
    return *this;
  }
  for (std::size_t position = 0; position < tools.size(); ++position) {
    const bool blocks = !content.empty() && content[position];
    if (blocks) {
      // Content is the result itself, which has no schema to publish.
      tools[position].output_schema.reset();
    }
    content_.push_back(blocks);
  }
  std::vector<scrydantic::BoundTool<Context>> all = tools_.release();
  all.insert(all.end(), std::make_move_iterator(tools.begin()),
             std::make_move_iterator(tools.end()));
  scrydantic::Result<scrydantic::ToolSet<Context>> set =
      scrydantic::ToolSet<Context>::make(std::move(all));
  if (!set) {
    problem_ = set.error();
    write_tool_lists();
    return *this;
  }
  tools_ = std::move(*set);
  write_tool_lists();
  return *this;
}

Server& Server::add(std::vector<BoundResource> resources) {
  if (problem_.has_value()) {
    return *this;
  }
  for (BoundResource& resource : resources) {
    const bool taken = std::ranges::any_of(resources_, [&](const BoundResource& other) {
      return other.uri == resource.uri;
    });
    if (taken || !resource.read) {
      problem_ = scrydantic::Error{};
      problem_->message =
          taken ? join({"two resources have the URI \"", resource.uri, "\""})
                : join({"resource \"", resource.uri, "\" has no handler"});
      return *this;
    }
    resources_.push_back(std::move(resource));
  }
  write_resource_lists();
  return *this;
}

Server& Server::add(std::vector<BoundPrompt> prompts) {
  if (problem_.has_value()) {
    return *this;
  }
  for (BoundPrompt& prompt : prompts) {
    const bool taken =
        std::ranges::find(prompts_, prompt.name, &BoundPrompt::name) != prompts_.end();
    if (taken || !prompt.get) {
      problem_ = scrydantic::Error{};
      problem_->message = taken ? join({"two prompts are named \"", prompt.name, "\""})
                                : join({"prompt \"", prompt.name, "\" has no handler"});
      return *this;
    }
    prompts_.push_back(std::move(prompt));
  }
  write_prompt_list();
  return *this;
}

void Server::write_resource_lists() {
  std::vector<wire::Resource> fixed;
  std::vector<wire::Resource> templates;
  const auto optional_text = [](const std::string& text) {
    return text.empty() ? std::nullopt : std::optional<std::string>{text};
  };
  for (const BoundResource& resource : resources_) {
    wire::Resource entry{.uri = std::nullopt,
                         .uri_template = std::nullopt,
                         .name = resource.name,
                         .title = optional_text(resource.title),
                         .description = optional_text(resource.description),
                         .mime_type = optional_text(resource.mime_type)};
    if (resource.is_template) {
      entry.uri_template = resource.uri;
      templates.push_back(std::move(entry));
    } else {
      entry.uri = resource.uri;
      fixed.push_back(std::move(entry));
    }
  }
  scrydantic::Result<Json> fixed_text = scrydantic::encode_verbatim(fixed);
  scrydantic::Result<Json> template_text = scrydantic::encode_verbatim(templates);
  if (!fixed_text || !template_text) {
    problem_ = fixed_text ? template_text.error() : fixed_text.error();
    return;
  }
  resource_list_ = std::move(fixed_text->text);
  template_list_ = std::move(template_text->text);
}

void Server::write_prompt_list() {
  std::vector<wire::Prompt> list;
  for (const BoundPrompt& prompt : prompts_) {
    wire::Prompt entry{.name = prompt.name,
                       .title = std::nullopt,
                       .description = std::nullopt,
                       .arguments = {}};
    if (!prompt.title.empty()) {
      entry.title = prompt.title;
    }
    if (!prompt.description.empty()) {
      entry.description = prompt.description;
    }
    for (const PromptArgument& argument : prompt.arguments) {
      entry.arguments.push_back(
          {.name = argument.name,
           .description = argument.description.empty()
                              ? std::nullopt
                              : std::optional<std::string>{argument.description},
           .required = argument.required});
    }
    list.push_back(std::move(entry));
  }
  scrydantic::Result<Json> text = scrydantic::encode_verbatim(list);
  if (!text) {
    problem_ = text.error();
    return;
  }
  prompt_list_ = std::move(text->text);
}

bool Server::completes() const noexcept {
  return !prompts_.empty() ||
         std::ranges::any_of(resources_, &BoundResource::is_template);
}

wire::ServerCapabilities Server::capabilities(const bool handshake) const {
  wire::ServerCapabilities capabilities{};
  if (completes()) {
    capabilities.completions = wire::CompletionsCapability{};
  }
  if (!prompts_.empty()) {
    capabilities.prompts = wire::PromptsCapability{};
  }
  if (logs_) {
    capabilities.logging = wire::LoggingCapability{};
  }
  if (!resources_.empty()) {
    // A session can subscribe to a resource; 2026-07-28 would deliver updates
    // on subscriptions/listen, which does not carry them yet.
    capabilities.resources = wire::ResourcesCapability{
        .subscribe = handshake ? std::optional<bool>{true} : std::nullopt};
  }
  // A server with nothing else still says that it has tools, if none yet.
  if (!tools_.tools().empty() || (resources_.empty() && prompts_.empty())) {
    capabilities.tools = wire::ToolsCapability{};
  }
  return capabilities;
}

wire::ResultMeta Server::result_meta() const {
  return {.server_info = {.name = name_, .version = version_}};
}

Status Server::status() const {
  if (problem_.has_value()) {
    return std::unexpected(*problem_);
  }
  return {};
}

void Server::write_tool_lists() {
  std::vector<wire::Tool> modern;
  std::vector<wire::Tool> legacy;
  legacy_structured_.clear();
  for (const scrydantic::BoundTool<Context>& tool : tools_.tools()) {
    modern.push_back({.name = tool.name,
                      .description = tool.description,
                      .input_schema = tool.input_schema,
                      .output_schema = tool.output_schema});
    // Before 2026-07-28 an output schema had to describe an object, and
    // structured content had to be one.
    const bool structured =
        tool.output_schema.has_value() && describes_an_object(*tool.output_schema);
    legacy_structured_.push_back(structured);
    legacy.push_back(modern.back());
    if (!structured) {
      legacy.back().output_schema.reset();
    }
  }
  scrydantic::Result<Json> modern_text = scrydantic::encode_verbatim(modern);
  scrydantic::Result<Json> legacy_text = scrydantic::encode_verbatim(legacy);
  if (!modern_text || !legacy_text) {
    // A name, a description, or a schema written by hand is not valid.
    if (!problem_.has_value()) {
      problem_ = modern_text ? legacy_text.error() : modern_text.error();
    }
    modern_tool_list_ = "[]";
    legacy_tool_list_ = "[]";
    return;
  }
  modern_tool_list_ = std::move(modern_text->text);
  legacy_tool_list_ = std::move(legacy_text->text);
}

void Server::log_line(const std::string_view line) const {
  if (log_) {
    log_(line);
  }
}

// One request, as handle() read it from its envelope.
struct Connection::Request {
  // The request's id, as JSON text.
  std::string id{};
  std::string_view method{};
  // The request's `params`, when it has them. They are an object.
  std::optional<JsonView> params{};
  // Whether the request is served under the modern revision.
  bool modern{};
  // Stops when the client cancels the request.
  std::stop_token stop{};
  // Whether the client takes log messages about the request, and the least
  // severe it takes, or none for every one.
  bool logs{};
  std::optional<LogLevel> log_level{};

  [[nodiscard]] std::string result(const std::string_view json) const {
    return result_message(id, json);
  }

  [[nodiscard]] std::string error(const int code, const std::string_view message,
                                  const std::string_view data = {}) const {
    return error_message(id, code, message, data);
  }

  // The answer when a result could not be written as JSON.
  [[nodiscard]] std::string internal_error() const {
    return error(wire::internal_error, "Internal error");
  }

  [[nodiscard]] std::optional<JsonView> param(const std::string_view key) const {
    return params.has_value() ? params->find(key) : std::nullopt;
  }
};

std::optional<std::string> Connection::handle(const std::string_view message,
                                              const Notify& notify,
                                              std::stop_token stop) {
  const scrydantic::Result<JsonView> document = JsonView::parse(message);
  if (!document) {
    return error_message({}, wire::parse_error,
                         join({"Parse error: ", document.error().message}));
  }
  if (document->kind() != JsonKind::object) {
    return error_message({}, wire::invalid_request,
                         document->kind() == JsonKind::array
                             ? "Invalid Request: batches are not supported"
                             : "Invalid Request: a message must be an object");
  }

  const std::optional<JsonView> id = document->find("id");
  if (id.has_value() && !is_request_id(*id)) {
    return error_message({}, wire::invalid_request,
                         "Invalid Request: id must be a string or an integer");
  }
  Request request{};
  if (id.has_value()) {
    request.id = id->to_json().text;
  }

  const std::optional<JsonView> jsonrpc = document->find("jsonrpc");
  if (!jsonrpc.has_value() || jsonrpc->string() != "2.0") {
    return error_message(request.id, wire::invalid_request,
                         R"(Invalid Request: jsonrpc must be "2.0")");
  }

  const std::optional<JsonView> method = document->find("method");
  if (!method.has_value()) {
    if (document->find("result").has_value() || document->find("error").has_value()) {
      // A response to a request of the server's: a call waits for it.
      answered(request.id, *document);
      return std::nullopt;
    }
    return error_message(request.id, wire::invalid_request,
                         "Invalid Request: a message needs a method");
  }
  if (!method->string().has_value()) {
    return error_message(request.id, wire::invalid_request,
                         "Invalid Request: method must be a string");
  }
  request.method = *method->string();

  if (!id.has_value()) {
    // A notification. `notifications/initialized` needs no action, and
    // `notifications/cancelled` is the transport's: it stops the token it
    // passed with the request it names.
    return std::nullopt;
  }

  request.stop = std::move(stop);
  request.params = document->find("params");
  if (request.params.has_value() && request.params->kind() != JsonKind::object) {
    return request.error(wire::invalid_params,
                         "Invalid params: params must be an object");
  }

  if (server_->problem_.has_value()) {
    return request.internal_error();
  }
  try {
    return dispatch(request, notify);
  } catch (const std::exception& thrown) {
    server_->log_line(join({"request failed: ", thrown.what()}));
  } catch (...) {
    server_->log_line("request failed: an unknown exception");
  }
  return request.internal_error();
}

std::optional<std::string> Connection::dispatch(Request& request,
                                                const Notify& notify) {
  if (request.method == "initialize") {
    return initialize(request);
  }

  const std::optional<JsonView> meta = request.param("_meta");
  const std::optional<JsonView> version =
      is_object(meta) ? meta->find(protocol_version_key) : std::nullopt;
  if (version.has_value()) {
    const std::optional<std::string_view> requested = version->string();
    if (!requested.has_value()) {
      return request.error(wire::invalid_params,
                           join({"Invalid params: _meta[\"", protocol_version_key,
                                 "\"] must be a string"}));
    }
    if (*requested != modern_protocol_version) {
      if (legacy_version_named(*requested) != nullptr) {
        return request.error(wire::invalid_params,
                             join({"Invalid params: protocol version ", *requested,
                                   " is agreed with an initialize request, not per "
                                   "request"}));
      }
      const std::optional<std::string> data = encoded(wire::UnsupportedVersionData{
          .supported = supported_versions(), .requested = std::string{*requested}});
      if (!data.has_value()) {
        return request.internal_error();
      }
      return request.error(wire::unsupported_protocol_version,
                           "Unsupported protocol version", *data);
    }
    if (!is_object(meta->find(client_capabilities_key))) {
      return request.error(wire::invalid_params,
                           join({"Invalid params: _meta[\"", client_capabilities_key,
                                 "\"] is required and must be an object"}));
    }
    request.modern = true;
    if (const std::optional<JsonView> level = meta->find(log_level_key)) {
      const std::optional<LogLevel> parsed = level->string().and_then(log_level_named);
      if (!parsed.has_value()) {
        return request.error(wire::invalid_params,
                             join({"Invalid params: _meta[\"", log_level_key,
                                   "\"] must name a log level, such as \"info\""}));
      }
      request.logs = true;
      request.log_level = parsed;
    }
    if (request.method == "server/discover") {
      return discover(request);
    }
    if (request.method == "tools/list") {
      return list_tools(request);
    }
    if (request.method == "tools/call") {
      return call_tool(request, notify);
    }
    if (request.method == "subscriptions/listen") {
      return listen(request, notify);
    }
    if (const std::optional<std::string> served = serve_feature(request, notify)) {
      return served;
    }
    return request.error(wire::method_not_found,
                         join({"Method not found: ", request.method}));
  }

  if (request.method == "ping") {
    return request.result("{}");
  }
  if (legacy_version().empty()) {
    return request.error(
        wire::invalid_params,
        join({"Invalid params: the request names no protocol version. Send _meta[\"",
              protocol_version_key, "\"] as ", modern_protocol_version,
              " on each request, or send initialize first for ",
              legacy_protocol_versions[0]}));
  }
  // Until the client sets a level, every message is sent.
  request.logs = true;
  if (const int level = log_level_.load(std::memory_order_acquire); level >= 0) {
    request.log_level = static_cast<LogLevel>(level);
  }
  if (request.method == "logging/setLevel" && server_->logs_) {
    return set_log_level(request);
  }
  if ((request.method == "resources/subscribe" ||
       request.method == "resources/unsubscribe") &&
      !server_->resources_.empty()) {
    return subscribe(request, request.method == "resources/subscribe");
  }
  if (request.method == "tools/list") {
    return list_tools(request);
  }
  if (request.method == "tools/call") {
    return call_tool(request, notify);
  }
  if (const std::optional<std::string> served = serve_feature(request, notify)) {
    return served;
  }
  return request.error(wire::method_not_found,
                       join({"Method not found: ", request.method}));
}

std::string Connection::subscribe(const Request& request, const bool subscribing) {
  const std::optional<JsonView> uri_value = request.param("uri");
  const std::optional<std::string_view> uri =
      uri_value.has_value() ? uri_value->string() : std::nullopt;
  if (!uri.has_value()) {
    return request.error(wire::invalid_params, "Invalid params: uri must be a string");
  }
  if (subscribing && find_resource(server_->resources_, *uri) == nullptr) {
    const std::optional<std::string> data =
        encoded(wire::ResourceNotFoundData{.uri = std::string{*uri}});
    return request.error(wire::resource_not_found, "Resource not found",
                         data.value_or(std::string{}));
  }
  const std::scoped_lock lock{push_mutex_};
  if (!subscribing) {
    subscriptions_.erase(std::string{*uri});
  } else if (subscriptions_.size() >= most_subscriptions) {
    return request.error(
        wire::invalid_params,
        "Invalid params: a session subscribes to at most 1024 resources");
  } else {
    subscriptions_.emplace(*uri);
  }
  return request.result("{}");
}

std::string Connection::set_log_level(const Request& request) {
  const std::optional<JsonView> level = request.param("level");
  const std::optional<LogLevel> parsed =
      level.has_value() ? level->string().and_then(log_level_named) : std::nullopt;
  if (!parsed.has_value()) {
    return request.error(
        wire::invalid_params,
        "Invalid params: level must name a log level, such as \"info\"");
  }
  log_level_.store(static_cast<int>(*parsed), std::memory_order_release);
  return request.result("{}");
}

std::optional<std::string> Connection::serve_feature(const Request& request,
                                                     const Notify& notify) const {
  // A method of a capability the server lacks is not found.
  const bool resources = !server_->resources_.empty();
  const bool prompts = !server_->prompts_.empty();
  if (!resources && request.method.starts_with("resources/")) {
    return std::nullopt;
  }
  if (!prompts && request.method.starts_with("prompts/")) {
    return std::nullopt;
  }
  if (request.method == "resources/list") {
    return list_resources(request, false);
  }
  if (request.method == "resources/templates/list") {
    return list_resources(request, true);
  }
  if (request.method == "resources/read") {
    return read_resource(request, notify);
  }
  if (request.method == "prompts/list") {
    return list_prompts(request);
  }
  if (request.method == "prompts/get") {
    return get_prompt(request, notify);
  }
  if (request.method == "completion/complete" && server_->completes()) {
    return complete(request);
  }
  return std::nullopt;
}

std::string Connection::initialize(const Request& request) {
  const std::optional<JsonView> version = request.param("protocolVersion");
  const std::optional<std::string_view> requested =
      version.has_value() ? version->string() : std::nullopt;
  if (!requested.has_value()) {
    return request.error(wire::invalid_params,
                         "Invalid params: protocolVersion must be a string");
  }
  // A client that asks for a revision the server lacks is offered the newest
  // one with a handshake, and decides for itself whether to go on.
  const std::string_view* const known = legacy_version_named(*requested);
  const std::string_view chosen =
      known != nullptr ? *known : legacy_protocol_versions[0];
  const std::optional<std::string> result = encoded(wire::InitializeResult{
      .protocol_version = std::string{chosen},
      .capabilities = server_->capabilities(true),
      .server_info = {.name = server_->name_, .version = server_->version_},
      .instructions = server_->instructions_,
  });
  if (!result.has_value()) {
    return request.internal_error();
  }
  const ClientAbilities abilities = abilities_in(request.param("capabilities"));
  legacy_abilities_.store((abilities.elicitation ? 1U : 0U) |
                              (abilities.sampling ? 2U : 0U) |
                              (abilities.roots ? 4U : 0U),
                          std::memory_order_release);
  legacy_version_.store(
      static_cast<int>(known != nullptr ? known - legacy_protocol_versions : 0),
      std::memory_order_release);
  return request.result(*result);
}

std::string_view Connection::legacy_version() const noexcept {
  const int position = legacy_version_.load(std::memory_order_acquire);
  return position == no_legacy_version
             ? std::string_view{}
             : legacy_protocol_versions[static_cast<std::size_t>(position)];
}

std::string Connection::discover(const Request& request) const {
  const std::optional<std::string> result = encoded(wire::DiscoverResult{
      .meta = {.server_info = {.name = server_->name_, .version = server_->version_}},
      .result_type = "complete",
      .supported_versions = supported_versions(),
      .capabilities = server_->capabilities(false),
      .instructions = server_->instructions_,
      .ttl_ms = server_->list_ttl_.count(),
      .cache_scope = wire::CacheScope::public_,
  });
  return result.has_value() ? request.result(*result) : request.internal_error();
}

std::string Connection::list_tools(const Request& request) const {
  if (request.param("cursor").has_value()) {
    // The whole list is one page, so the server has handed out no cursor.
    return request.error(wire::invalid_params, "Invalid params: unknown cursor");
  }
  const std::optional<std::string> result =
      request.modern ? encoded(wire::ListToolsResult{
                           .meta = {.server_info = {.name = server_->name_,
                                                    .version = server_->version_}},
                           .result_type = "complete",
                           .tools = Json{.text = server_->modern_tool_list_},
                           .ttl_ms = server_->list_ttl_.count(),
                           .cache_scope = wire::CacheScope::public_,
                       })
                     : encoded(wire::LegacyListToolsResult{
                           .tools = Json{.text = server_->legacy_tool_list_}});
  return result.has_value() ? request.result(*result) : request.internal_error();
}

std::string Connection::call_tool(const Request& request, const Notify& notify) const {
  const std::optional<JsonView> name_value = request.param("name");
  const std::optional<std::string_view> name =
      name_value.has_value() ? name_value->string() : std::nullopt;
  if (!name.has_value()) {
    return request.error(wire::invalid_params, "Invalid params: name must be a string");
  }
  const scrydantic::BoundTool<Context>* const tool = server_->tools_.find(*name);
  if (tool == nullptr) {
    return request.error(wire::invalid_params, join({"Unknown tool: ", *name}));
  }

  const std::size_t position =
      static_cast<std::size_t>(tool - server_->tools_.tools().data());
  // A call may leave `arguments` out, which is the empty object.
  static const JsonView no_arguments = JsonView::parse("{}").value();
  std::optional<JsonView> arguments = request.param("arguments");
  if (!arguments.has_value()) {
    arguments = no_arguments;
  } else if (arguments->kind() != JsonKind::object) {
    return request.error(wire::invalid_params,
                         "Invalid params: arguments must be an object");
  }

  const std::string argument_text = arguments->to_json().text;
  Call call{};
  if (std::optional<std::string> refused =
          prepare_inquiry(request, call, notify, tool->name, argument_text)) {
    return std::move(*refused);
  }
  const Context context = context_of(request, call, notify);

  wire::CallToolResult reply{};
  if (request.modern) {
    reply.meta = wire::ResultMeta{
        .server_info = {.name = server_->name_, .version = server_->version_}};
    reply.result_type = "complete";
  }
  // A text block that is not UTF-8 is the tool's own fault, and a failure.
  bool written = true;
  const auto say = [&reply, &written](const std::string_view text) {
    std::optional<std::string> blocks = encoded(
        std::vector<wire::TextContent>{{.text = std::string{text}, .type = "text"}});
    written = blocks.has_value();
    reply.content = Json{.text = blocks.has_value() ? std::move(*blocks) : "[]"};
  };
  const auto failed = [&reply, &say](const std::string_view text) {
    say(text);
    reply.structured_content.reset();
    reply.is_error = true;
  };

  scrydantic::Result<Json> result;
  bool threw = true;
  try {
    result = tool->invoke(context, *arguments);
    threw = false;
  } catch (const std::exception& thrown) {
    server_->log_line(join({"tool ", tool->name, " threw: ", thrown.what()}));
  } catch (...) {
    server_->log_line(join({"tool ", tool->name, " threw an unknown exception"}));
  }
  if (std::optional<std::string> asked =
          inquiry_outcome(request, call, tool->name, argument_text)) {
    return std::move(*asked);
  }

  if (threw) {
    failed(tool_failed);
  } else if (result && server_->content_[position]) {
    // The tool's content blocks, as they are.
    reply.content = std::move(*result);
  } else if (result) {
    const bool structured = tool->output_schema.has_value() &&
                            (request.modern || server_->legacy_structured_[position]);
    // A tool that returns nothing answers `{}`, which says nothing to a model.
    if (tool->output_schema.has_value() || result->text != "{}") {
      say(content_text(*result));
    }
    if (structured) {
      reply.structured_content = std::move(*result);
    }
  } else if (!result.error().model_message.empty()) {
    // The arguments did not decode, or the tool refused the call. Either way
    // the text is the model's to read and act on.
    failed(result.error().model_message);
  } else {
    // The result broke its own type's constraints or could not be encoded.
    server_->log_line(join({"tool ", tool->name, " failed: ", result.error().message}));
    failed(tool_failed);
  }

  std::optional<std::string> text = written ? encoded(reply) : std::nullopt;
  if (!text.has_value()) {
    // The tool's own message is not UTF-8.
    server_->log_line(
        join({"tool ", tool->name, " answered with text that is not UTF-8"}));
    failed(tool_failed);
    text = encoded(reply);
  }
  return text.has_value() ? request.result(*text) : request.internal_error();
}

Context Connection::context_of(const Request& request, Call& call,
                               const Notify& notify) const {
  Progress::State& progress = call.progress;
  Context context{.protocol_version =
                      request.modern ? modern_protocol_version : legacy_version(),
                  .progress = {},
                  .logger = {},
                  .stop = request.stop,
                  .inquiry = &call.inquiry};
  if (notify && request.logs) {
    call.logger.notify = &notify;
    call.logger.threshold = request.log_level;
    context.logger = Logger{&call.logger};
  }
  const std::optional<JsonView> meta = request.param("_meta");
  const std::optional<JsonView> token =
      is_object(meta) ? meta->find("progressToken") : std::nullopt;
  if (notify && token.has_value() && is_request_id(*token)) {
    progress.notify = &notify;
    progress.token = token->to_json().text;
    context.progress = Progress{&progress};
  }
  return context;
}

std::optional<std::string>
Connection::prepare_inquiry(const Request& request, Call& call, const Notify& notify,
                            const std::string_view target,
                            const std::string_view arguments) const {
  detail::Inquiry& inquiry = call.inquiry;
  inquiry.modern = request.modern;
  if (!request.modern) {
    const unsigned abilities = legacy_abilities_.load(std::memory_order_acquire);
    inquiry.elicitation = (abilities & 1U) != 0;
    inquiry.sampling = (abilities & 2U) != 0;
    inquiry.roots = (abilities & 4U) != 0;
    inquiry.form_mode = legacy_version() != "2025-06-18";
    inquiry.round_trip = [this, &notify,
                          stop = request.stop](const std::string_view method,
                                               const std::string_view params) {
      return ask_client(method, params, notify, stop);
    };
    return std::nullopt;
  }

  const std::optional<JsonView> meta = request.param("_meta");
  const ClientAbilities abilities = abilities_in(meta->find(client_capabilities_key));
  inquiry.elicitation = abilities.elicitation;
  inquiry.sampling = abilities.sampling;
  inquiry.roots = abilities.roots;
  if (const std::optional<JsonView> state = request.param("requestState")) {
    const std::optional<std::string_view> text = state->string();
    std::optional<std::unordered_map<std::string, std::string>> answers =
        text.has_value() ? open_state(server_->state_key_, *text,
                                      state_binding(request.method, target, arguments))
                         : std::nullopt;
    if (!answers.has_value()) {
      return request.error(wire::invalid_params, "Invalid or expired requestState",
                           R"({"reason":"invalid_request_state"})");
    }
    inquiry.answers = std::move(*answers);
  }
  if (const std::optional<JsonView> responses = request.param("inputResponses")) {
    if (responses->kind() != JsonKind::object) {
      return request.error(wire::invalid_params,
                           "Invalid params: inputResponses must be an object");
    }
    for (std::size_t index = 0; index < responses->size(); ++index) {
      const std::string_view name = responses->key_at(index).value_or("");
      // An answer the state carries was given in an earlier round, and
      // stays as it was.
      inquiry.answers.emplace(std::string{name}, responses->find(name)->to_json().text);
    }
  }
  return std::nullopt;
}

std::optional<std::string>
Connection::inquiry_outcome(const Request& request, Call& call,
                            const std::string_view target,
                            const std::string_view arguments) const {
  detail::Inquiry& inquiry = call.inquiry;
  const std::scoped_lock lock{inquiry.mutex};
  if (inquiry.missing.has_value()) {
    return request.error(wire::missing_capability, "Missing required client capability",
                         join({R"({"requiredCapabilities":)", *inquiry.missing, "}"}));
  }
  if (inquiry.invalid.has_value()) {
    return request.error(wire::invalid_params,
                         join({"Invalid params: ", *inquiry.invalid}));
  }
  if (!request.modern || inquiry.unanswered.empty()) {
    return std::nullopt;
  }
  // The round ends here. The client answers every question, and sends the
  // request again with the answers and the state that holds the earlier ones.
  const std::optional<std::string> meta = encoded(server_->result_meta());
  if (!meta.has_value()) {
    return request.internal_error();
  }
  std::string result{R"({"_meta":)"};
  result += *meta;
  result += R"(,"inputRequests":{)";
  bool first = true;
  for (const auto& [name, question] : inquiry.unanswered) {
    if (!first) {
      result += ',';
    }
    first = false;
    result += scrydantic::escape_json_string(name);
    result += ':';
    result += question;
  }
  result += R"(},"requestState":)";
  result += scrydantic::escape_json_string(
      seal_state(server_->state_key_, inquiry.answers,
                 state_binding(request.method, target, arguments)));
  result += R"(,"resultType":"input_required"})";
  return request.result(result);
}

Result<std::string> Connection::ask_client(const std::string_view method,
                                           const std::string_view params,
                                           const Notify& notify,
                                           const std::stop_token& stop) const {
  if (!notify || gone_.stop_requested()) {
    return std::unexpected(
        scrydantic::tool_error("the client cannot be reached from this call"));
  }
  const std::string id =
      std::to_string(next_request_id_.fetch_add(1, std::memory_order_relaxed) + 1);
  auto waiting = std::make_shared<Waiting>();
  {
    const std::scoped_lock lock{waiting_mutex_};
    waiting_.insert_or_assign(id, waiting);
  }
  notify(join({R"({"id":)", id, R"(,"jsonrpc":"2.0","method":)",
               scrydantic::escape_json_string(method), R"(,"params":)", params, "}"}));
  std::optional<std::string> response;
  {
    // The call waits until the answer comes, the call is cancelled, or the
    // client is gone.
    std::stop_source either;
    const std::stop_callback cancelled{stop, [&either] { either.request_stop(); }};
    const std::stop_callback left{gone_.get_token(),
                                  [&either] { either.request_stop(); }};
    std::unique_lock lock{waiting->mutex};
    waiting->done.wait(lock, either.get_token(),
                       [&] { return waiting->response.has_value(); });
    response = std::move(waiting->response);
  }
  {
    const std::scoped_lock lock{waiting_mutex_};
    waiting_.erase(id);
  }
  if (!response.has_value()) {
    return std::unexpected(scrydantic::tool_error("the call was cancelled"));
  }
  const scrydantic::Result<JsonView> document = JsonView::parse(*response);
  if (!document) {
    return std::unexpected(scrydantic::tool_error("the client's answer is not JSON"));
  }
  if (const std::optional<JsonView> result = document->find("result")) {
    return result->to_json().text;
  }
  const std::optional<JsonView> error = document->find("error");
  const std::optional<JsonView> text =
      error.has_value() ? error->find("message") : std::nullopt;
  return std::unexpected(scrydantic::tool_error(
      join({"the client answered with an error: ",
            text.has_value() ? text->string().value_or("") : std::string_view{}})));
}

void Connection::answered(const std::string& id, const JsonView& response) const {
  std::shared_ptr<Waiting> waiting;
  {
    const std::scoped_lock lock{waiting_mutex_};
    const auto found = waiting_.find(id);
    if (found == waiting_.end()) {
      // No call waits for it: one that was cancelled, or an id the server
      // never sent.
      return;
    }
    waiting = found->second;
  }
  if (waiting == nullptr) {
    return;
  }
  {
    const std::scoped_lock lock{waiting->mutex};
    waiting->response = response.to_json().text;
  }
  waiting->done.notify_all();
}

std::string Connection::list_resources(const Request& request,
                                       const bool templates) const {
  if (request.param("cursor").has_value()) {
    return request.error(wire::invalid_params, "Invalid params: unknown cursor");
  }
  std::optional<wire::ResultMeta> meta;
  std::optional<std::string> result_type;
  std::optional<std::int64_t> ttl;
  std::optional<wire::CacheScope> scope;
  if (request.modern) {
    meta = server_->result_meta();
    result_type = "complete";
    ttl = server_->list_ttl_.count();
    scope = wire::CacheScope::public_;
  }
  const std::optional<std::string> result =
      templates ? encoded(wire::ListResourceTemplatesResult{
                      .meta = std::move(meta),
                      .result_type = std::move(result_type),
                      .resource_templates = Json{.text = server_->template_list_},
                      .ttl_ms = ttl,
                      .cache_scope = scope,
                  })
                : encoded(wire::ListResourcesResult{
                      .meta = std::move(meta),
                      .result_type = std::move(result_type),
                      .resources = Json{.text = server_->resource_list_},
                      .ttl_ms = ttl,
                      .cache_scope = scope,
                  });
  return result.has_value() ? request.result(*result) : request.internal_error();
}

std::string Connection::read_resource(const Request& request,
                                      const Notify& notify) const {
  const std::optional<JsonView> uri_value = request.param("uri");
  const std::optional<std::string_view> uri =
      uri_value.has_value() ? uri_value->string() : std::nullopt;
  if (!uri.has_value()) {
    return request.error(wire::invalid_params, "Invalid params: uri must be a string");
  }
  // Under 2026-07-28 a resource that is not there is invalid params; before
  // it, the revisions gave it a code of its own.
  const auto not_found = [&](const std::string_view message) {
    const std::optional<std::string> data =
        encoded(wire::ResourceNotFoundData{.uri = std::string{*uri}});
    return request.error(request.modern ? wire::invalid_params
                                        : wire::resource_not_found,
                         message, data.value_or(std::string{}));
  };

  std::string variables{"{}"};
  const BoundResource* const found =
      find_resource(server_->resources_, *uri, &variables);
  if (found == nullptr) {
    return not_found("Resource not found");
  }

  const scrydantic::Result<JsonView> parsed = JsonView::parse(variables);
  if (!parsed) {
    return request.internal_error();
  }
  Call call{};
  if (std::optional<std::string> refused =
          prepare_inquiry(request, call, notify, *uri, "{}")) {
    return std::move(*refused);
  }
  const Context context = context_of(request, call, notify);
  scrydantic::Result<std::vector<ResourceContents>> contents;
  try {
    contents = found->read(context, *uri, *parsed);
  } catch (const std::exception& thrown) {
    server_->log_line(join({"resource ", found->uri, " threw: ", thrown.what()}));
    return request.internal_error();
  } catch (...) {
    server_->log_line(join({"resource ", found->uri, " threw an unknown exception"}));
    return request.internal_error();
  }
  if (std::optional<std::string> asked = inquiry_outcome(request, call, *uri, "{}")) {
    return std::move(*asked);
  }
  if (!contents) {
    if (!contents.error().model_message.empty()) {
      // The variables do not fit the parameters, or the function refused.
      return not_found(contents.error().model_message);
    }
    server_->log_line(
        join({"resource ", found->uri, " failed: ", contents.error().message}));
    return request.internal_error();
  }

  wire::ReadResourceResult reply{.meta = std::nullopt,
                                 .result_type = std::nullopt,
                                 .contents = std::move(*contents),
                                 .ttl_ms = std::nullopt,
                                 .cache_scope = std::nullopt};
  if (request.modern) {
    reply.meta = server_->result_meta();
    reply.result_type = "complete";
    reply.ttl_ms = server_->list_ttl_.count();
    reply.cache_scope = wire::CacheScope::public_;
  }
  const std::optional<std::string> result = encoded(reply);
  if (!result.has_value()) {
    server_->log_line(join({"resource ", found->uri, " holds text that is not UTF-8"}));
    return request.internal_error();
  }
  return request.result(*result);
}

std::string Connection::list_prompts(const Request& request) const {
  if (request.param("cursor").has_value()) {
    return request.error(wire::invalid_params, "Invalid params: unknown cursor");
  }
  wire::ListPromptsResult reply{.meta = std::nullopt,
                                .result_type = std::nullopt,
                                .prompts = Json{.text = server_->prompt_list_},
                                .ttl_ms = std::nullopt,
                                .cache_scope = std::nullopt};
  if (request.modern) {
    reply.meta = server_->result_meta();
    reply.result_type = "complete";
    reply.ttl_ms = server_->list_ttl_.count();
    reply.cache_scope = wire::CacheScope::public_;
  }
  const std::optional<std::string> result = encoded(reply);
  return result.has_value() ? request.result(*result) : request.internal_error();
}

std::string Connection::get_prompt(const Request& request, const Notify& notify) const {
  const std::optional<JsonView> name_value = request.param("name");
  const std::optional<std::string_view> name =
      name_value.has_value() ? name_value->string() : std::nullopt;
  if (!name.has_value()) {
    return request.error(wire::invalid_params, "Invalid params: name must be a string");
  }
  const auto found = std::ranges::find(server_->prompts_, *name, &BoundPrompt::name);
  if (found == server_->prompts_.end()) {
    return request.error(wire::invalid_params, join({"Unknown prompt: ", *name}));
  }
  static const JsonView no_arguments = JsonView::parse("{}").value();
  std::optional<JsonView> arguments = request.param("arguments");
  if (!arguments.has_value() || arguments->kind() == JsonKind::null) {
    arguments = no_arguments;
  } else if (arguments->kind() != JsonKind::object) {
    return request.error(wire::invalid_params,
                         "Invalid params: arguments must be an object");
  }

  const std::string argument_text = arguments->to_json().text;
  Call call{};
  if (std::optional<std::string> refused =
          prepare_inquiry(request, call, notify, found->name, argument_text)) {
    return std::move(*refused);
  }
  const Context context = context_of(request, call, notify);
  scrydantic::Result<std::vector<PromptMessage>> messages;
  try {
    messages = found->get(context, *arguments);
  } catch (const std::exception& thrown) {
    server_->log_line(join({"prompt ", found->name, " threw: ", thrown.what()}));
    return request.internal_error();
  } catch (...) {
    server_->log_line(join({"prompt ", found->name, " threw an unknown exception"}));
    return request.internal_error();
  }
  if (std::optional<std::string> asked =
          inquiry_outcome(request, call, found->name, argument_text)) {
    return std::move(*asked);
  }
  if (!messages) {
    if (!messages.error().model_message.empty()) {
      return request.error(wire::invalid_params,
                           join({"Invalid params: ", messages.error().model_message}));
    }
    server_->log_line(
        join({"prompt ", found->name, " failed: ", messages.error().message}));
    return request.internal_error();
  }

  wire::GetPromptResult reply{.meta = std::nullopt,
                              .result_type = std::nullopt,
                              .description = std::nullopt,
                              .messages = std::move(*messages)};
  if (!found->description.empty()) {
    reply.description = found->description;
  }
  if (request.modern) {
    reply.meta = server_->result_meta();
    reply.result_type = "complete";
  }
  const std::optional<std::string> result = encoded(reply);
  if (!result.has_value()) {
    server_->log_line(join({"prompt ", found->name, " wrote text that is not UTF-8"}));
    return request.internal_error();
  }
  return request.result(*result);
}

std::string Connection::complete(const Request& request) const {
  const std::optional<JsonView> ref = request.param("ref");
  const std::optional<JsonView> kind =
      is_object(ref) ? ref->find("type") : std::nullopt;
  const std::optional<std::string_view> type =
      kind.has_value() ? kind->string() : std::nullopt;
  bool known = false;
  if (type == "ref/prompt") {
    const std::optional<JsonView> name = ref->find("name");
    const std::optional<std::string_view> text =
        name.has_value() ? name->string() : std::nullopt;
    known = text.has_value() &&
            std::ranges::find(server_->prompts_, *text, &BoundPrompt::name) !=
                server_->prompts_.end();
  } else if (type == "ref/resource") {
    const std::optional<JsonView> uri = ref->find("uri");
    const std::optional<std::string_view> text =
        uri.has_value() ? uri->string() : std::nullopt;
    known = text.has_value() && std::ranges::any_of(server_->resources_,
                                                    [&](const BoundResource& resource) {
                                                      return resource.uri == *text;
                                                    });
  } else {
    return request.error(wire::invalid_params,
                         "Invalid params: ref must be a ref/prompt or a ref/resource");
  }
  if (!known) {
    return request.error(
        wire::invalid_params,
        "Invalid params: ref names no prompt or resource of this server");
  }
  // The server suggests no values of its own.
  wire::CompleteResult reply{
      .meta = std::nullopt,
      .result_type = std::nullopt,
      .completion = {.values = {}, .total = 0, .has_more = false}};
  if (request.modern) {
    reply.meta = server_->result_meta();
    reply.result_type = "complete";
  }
  const std::optional<std::string> result = encoded(reply);
  return result.has_value() ? request.result(*result) : request.internal_error();
}

std::optional<std::string> Connection::listen(const Request& request,
                                              const Notify& notify) const {
  if (!is_object(request.param("notifications"))) {
    return request.error(wire::invalid_params,
                         "Invalid params: notifications must be an object");
  }
  // The tool list of a server does not change while it is served, so there is
  // nothing to notify a client of. The stream opens, agrees to no notification
  // type, and stays open until the client cancels it or the transport closes.
  const std::optional<std::string> params = encoded(wire::SubscriptionAcknowledged{
      .meta = {.subscription_id = Json{.text = request.id}}, .notifications = {}});
  if (!params.has_value()) {
    return request.internal_error();
  }
  if (notify) {
    notify(notification_message("notifications/subscriptions/acknowledged", *params));
  }
  return std::nullopt;
}

} // namespace mcp
