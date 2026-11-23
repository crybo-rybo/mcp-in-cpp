#include "wire.hpp"

#include <cmath>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <mcp/server.hpp>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

namespace {

using scrydantic::JsonKind;
using scrydantic::JsonView;

constexpr std::string_view protocol_version_key =
    "io.modelcontextprotocol/protocolVersion";
constexpr std::string_view client_capabilities_key =
    "io.modelcontextprotocol/clientCapabilities";

// What a client is told when a call fails for a reason it cannot act on. The
// reason itself goes to the server's log.
constexpr std::string_view tool_failed = "the tool failed";

[[nodiscard]] std::string join(const std::initializer_list<std::string_view> parts) {
  std::size_t size = 0;
  for (const std::string_view part : parts) {
    size += part.size();
  }
  std::string text;
  text.reserve(size);
  for (const std::string_view part : parts) {
    text += part;
  }
  return text;
}

// The envelopes are written by hand, with their members in sorted order as
// Scrydantic writes an object's. `id` and `result` are JSON text already.
[[nodiscard]] std::string result_message(const std::string_view id,
                                         const std::string_view result) {
  return join({R"({"id":)", id, R"(,"jsonrpc":"2.0","result":)", result, "}"});
}

// `id` is empty when the request's own could not be read. `message` must be
// valid UTF-8: the server's own words, or text that came out of a parsed
// message. `data` is JSON text, or empty for none.
[[nodiscard]] std::string error_message(const std::string_view id, const int code,
                                        const std::string_view message,
                                        const std::string_view data = {}) {
  std::string text = join({R"({"error":{"code":)", std::to_string(code)});
  if (!data.empty()) {
    text += join({R"(,"data":)", data});
  }
  text += join({R"(,"message":)", scrydantic::escape_json_string(message), "}"});
  if (!id.empty()) {
    text += join({R"(,"id":)", id});
  }
  text += R"(,"jsonrpc":"2.0"})";
  return text;
}

[[nodiscard]] std::string notification_message(const std::string_view method,
                                               const std::string_view params) {
  return join(
      {R"({"jsonrpc":"2.0","method":")", method, R"(","params":)", params, "}"});
}

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
  bool reported{};
  double last{};
};

void Progress::report(const double progress, std::optional<double> total,
                      const std::string_view message) const {
  if (state_ == nullptr || !std::isfinite(progress) ||
      (state_->reported && progress <= state_->last)) {
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

Server::Server(std::string name, std::string version)
    : name_{std::move(name)}, version_{std::move(version)},
      log_{[](const std::string_view line) { std::cerr << line << '\n'; }} {}

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
  if (problem_.has_value()) {
    return *this;
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
                                              const Notify& notify) {
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
      // A response. The server sends no requests, so it answers none of its own.
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
    // `notifications/cancelled` names a request that has already been answered:
    // nothing outlives handle().
    return std::nullopt;
  }

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
    return request.error(wire::method_not_found,
                         join({"Method not found: ", request.method}));
  }

  if (request.method == "ping") {
    return request.result("{}");
  }
  if (legacy_version_.empty()) {
    return request.error(
        wire::invalid_params,
        join({"Invalid params: the request names no protocol version. Send _meta[\"",
              protocol_version_key, "\"] as ", modern_protocol_version,
              " on each request, or send initialize first for ",
              legacy_protocol_versions[0]}));
  }
  if (request.method == "tools/list") {
    return list_tools(request);
  }
  if (request.method == "tools/call") {
    return call_tool(request, notify);
  }
  return request.error(wire::method_not_found,
                       join({"Method not found: ", request.method}));
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
      .capabilities = {.tools = wire::ToolsCapability{}},
      .server_info = {.name = server_->name_, .version = server_->version_},
      .instructions = server_->instructions_,
  });
  if (!result.has_value()) {
    return request.internal_error();
  }
  legacy_version_ = chosen;
  return request.result(*result);
}

std::string Connection::discover(const Request& request) const {
  const std::optional<std::string> result = encoded(wire::DiscoverResult{
      .meta = {.server_info = {.name = server_->name_, .version = server_->version_}},
      .result_type = "complete",
      .supported_versions = supported_versions(),
      .capabilities = {.tools = wire::ToolsCapability{}},
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

  // A call may leave `arguments` out, which is the empty object.
  static const JsonView no_arguments = JsonView::parse("{}").value();
  std::optional<JsonView> arguments = request.param("arguments");
  if (!arguments.has_value()) {
    arguments = no_arguments;
  } else if (arguments->kind() != JsonKind::object) {
    return request.error(wire::invalid_params,
                         "Invalid params: arguments must be an object");
  }

  Progress::State progress{};
  Context context{.protocol_version =
                      request.modern ? modern_protocol_version : legacy_version_};
  const std::optional<JsonView> meta = request.param("_meta");
  const std::optional<JsonView> token =
      is_object(meta) ? meta->find("progressToken") : std::nullopt;
  if (notify && token.has_value() && is_request_id(*token)) {
    progress.notify = &notify;
    progress.token = token->to_json().text;
    context.progress = Progress{&progress};
  }

  wire::CallToolResult reply{};
  if (request.modern) {
    reply.meta = wire::ResultMeta{
        .server_info = {.name = server_->name_, .version = server_->version_}};
    reply.result_type = "complete";
  }
  const auto failed = [&reply](const std::string_view text) {
    reply.content = {{.text = std::string{text}, .type = "text"}};
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

  if (threw) {
    failed(tool_failed);
  } else if (result) {
    const std::size_t position =
        static_cast<std::size_t>(tool - server_->tools_.tools().data());
    const bool structured = tool->output_schema.has_value() &&
                            (request.modern || server_->legacy_structured_[position]);
    // A tool that returns nothing answers `{}`, which says nothing to a model.
    if (tool->output_schema.has_value() || result->text != "{}") {
      reply.content = {{.text = content_text(*result), .type = "text"}};
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

  std::optional<std::string> text = encoded(reply);
  if (!text.has_value()) {
    // The tool's own message is not UTF-8.
    server_->log_line(
        join({"tool ", tool->name, " answered with text that is not UTF-8"}));
    failed(tool_failed);
    text = encoded(reply);
  }
  return text.has_value() ? request.result(*text) : request.internal_error();
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
