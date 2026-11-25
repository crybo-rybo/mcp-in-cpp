// The protocol's own messages, as the plain structs Scrydantic encodes. Each is
// named as the MCP schema names it. They are written, never read: a message is
// read through a JsonView, because its envelope decides what the rest must be.
#pragma once

#include <cstdint>
#include <mcp/binding.hpp>
#include <mcp/content.hpp>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::wire {

using scrydantic::camel_case;
using scrydantic::Json;
using scrydantic::name;
using scrydantic::rename_all;
using scrydantic::skip_null;

// JSON-RPC's own error codes, and the ones MCP 2026-07-28 allocates.
inline constexpr int parse_error = -32700;
inline constexpr int invalid_request = -32600;
inline constexpr int method_not_found = -32601;
inline constexpr int invalid_params = -32602;
inline constexpr int internal_error = -32603;
inline constexpr int unsupported_protocol_version = -32022;
inline constexpr int missing_capability = -32021;
// The code of a resource that is not there, before 2026-07-28.
inline constexpr int resource_not_found = -32002;

struct Implementation {
  std::string name;
  std::string version;
};

struct ResultMeta {
  [[= name{"io.modelcontextprotocol/serverInfo"}]] Implementation server_info;
};

enum class CacheScope : std::uint8_t {
  public_[[= name{"public"}]],
  private_[[= name{"private"}]],
};

struct[[ = rename_all{camel_case}, = skip_null ]] Tool {
  std::string name;
  std::string description;
  Json input_schema;
  std::optional<Json> output_schema;
};

struct ToolsCapability {};
struct[[= skip_null]] ResourcesCapability {
  std::optional<bool> subscribe;
};
struct PromptsCapability {};
struct CompletionsCapability {};
struct LoggingCapability {};

struct[[= skip_null]] ServerCapabilities {
  std::optional<CompletionsCapability> completions;
  std::optional<LoggingCapability> logging;
  std::optional<PromptsCapability> prompts;
  std::optional<ResourcesCapability> resources;
  std::optional<ToolsCapability> tools;
};

// One entry of resources/list, or of resources/templates/list, where `uri` is
// written `uriTemplate`.
struct[[ = rename_all{camel_case}, = skip_null ]] Resource {
  std::optional<std::string> uri;
  std::optional<std::string> uri_template;
  std::string name;
  std::optional<std::string> title;
  std::optional<std::string> description;
  std::optional<std::string> mime_type;
};

struct[[= skip_null]] PromptArgument {
  std::string name;
  std::optional<std::string> description;
  bool required{};
};

struct[[= skip_null]] Prompt {
  std::string name;
  std::optional<std::string> title;
  std::optional<std::string> description;
  std::vector<PromptArgument> arguments;
};

// The results of the lists of resources, templates and prompts, and of
// resources/read, in either era. Under the modern revision each carries the
// envelope and the caching hints; a legacy one has neither. Each list is JSON
// text, written when the server's resources or prompts changed.
struct[[ = rename_all{camel_case}, = skip_null ]] ListResourcesResult {
  [[= name{"_meta"}]] std::optional<ResultMeta> meta;
  std::optional<std::string> result_type;
  Json resources;
  std::optional<std::int64_t> ttl_ms;
  std::optional<CacheScope> cache_scope;
};

struct[[ = rename_all{camel_case}, = skip_null ]] ListResourceTemplatesResult {
  [[= name{"_meta"}]] std::optional<ResultMeta> meta;
  std::optional<std::string> result_type;
  Json resource_templates;
  std::optional<std::int64_t> ttl_ms;
  std::optional<CacheScope> cache_scope;
};

struct[[ = rename_all{camel_case}, = skip_null ]] ListPromptsResult {
  [[= name{"_meta"}]] std::optional<ResultMeta> meta;
  std::optional<std::string> result_type;
  Json prompts;
  std::optional<std::int64_t> ttl_ms;
  std::optional<CacheScope> cache_scope;
};

struct[[ = rename_all{camel_case}, = skip_null ]] ReadResourceResult {
  [[= name{"_meta"}]] std::optional<ResultMeta> meta;
  std::optional<std::string> result_type;
  std::vector<mcp::ResourceContents> contents;
  std::optional<std::int64_t> ttl_ms;
  std::optional<CacheScope> cache_scope;
};

struct[[ = rename_all{camel_case}, = skip_null ]] GetPromptResult {
  [[= name{"_meta"}]] std::optional<ResultMeta> meta;
  std::optional<std::string> result_type;
  std::optional<std::string> description;
  std::vector<mcp::PromptMessage> messages;
};

struct[[= rename_all{camel_case}]] Completion {
  std::vector<std::string> values;
  std::int64_t total{};
  bool has_more{};
};

struct[[ = rename_all{camel_case}, = skip_null ]] CompleteResult {
  [[= name{"_meta"}]] std::optional<ResultMeta> meta;
  std::optional<std::string> result_type;
  Completion completion;
};

struct ResourceNotFoundData {
  std::string uri;
};

// The result of `server/discover`.
struct[[ = rename_all{camel_case}, = skip_null ]] DiscoverResult {
  [[= name{"_meta"}]] ResultMeta meta;
  std::string result_type;
  std::vector<std::string> supported_versions;
  ServerCapabilities capabilities;
  std::optional<std::string> instructions;
  std::int64_t ttl_ms{};
  CacheScope cache_scope{CacheScope::public_};
};

// The result of `initialize`, for the revisions that have a handshake.
struct[[ = rename_all{camel_case}, = skip_null ]] InitializeResult {
  std::string protocol_version;
  ServerCapabilities capabilities;
  Implementation server_info;
  std::optional<std::string> instructions;
};

// The result of `tools/list` under the modern revision. `tools` is the array
// the server wrote when its tools were added.
struct[[= rename_all{camel_case}]] ListToolsResult {
  [[= name{"_meta"}]] ResultMeta meta;
  std::string result_type;
  Json tools;
  std::int64_t ttl_ms{};
  CacheScope cache_scope{CacheScope::public_};
};

struct LegacyListToolsResult {
  Json tools;
};

struct TextContent {
  std::string text;
  std::string type;
};

// The result of `tools/call` in either era. A legacy result has no `_meta` and
// no `resultType`.
struct[[ = rename_all{camel_case}, = skip_null ]] CallToolResult {
  [[= name{"_meta"}]] std::optional<ResultMeta> meta;
  std::optional<std::string> result_type;
  // The content blocks, as JSON text: the server's own text block, or what a
  // tool that returns mcp::Content wrote.
  Json content{.text = "[]"};
  std::optional<Json> structured_content;
  bool is_error{};
};

struct[[ = rename_all{camel_case}, = skip_null ]] ProgressParams {
  Json progress_token;
  double progress{};
  std::optional<double> total;
  std::optional<std::string> message;
};

// The parameters of `notifications/message`.
struct LogParams {
  Json data;
  mcp::LogLevel level{};
};

struct UnsupportedVersionData {
  std::vector<std::string> supported;
  std::string requested;
};

struct SubscriptionMeta {
  [[= name{"io.modelcontextprotocol/subscriptionId"}]] Json subscription_id;
};

struct NoNotifications {};

// The parameters of `notifications/subscriptions/acknowledged`. The server has
// nothing to notify a client of yet, so it agrees to an empty set.
struct SubscriptionAcknowledged {
  [[= name{"_meta"}]] SubscriptionMeta meta;
  NoNotifications notifications;
};

// What a client sends.

// The capabilities a client declares. It has none of its own yet: it answers
// no request of a server's.
struct ClientCapabilities {};

// The `_meta` of a client's request. Under 2026-07-28 it carries the protocol
// fields on every request; under a handshake revision, only a progress token.
struct[[= skip_null]] RequestMeta {
  [[= name{
      "io.modelcontextprotocol/clientCapabilities"}]] std::optional<ClientCapabilities>
      client_capabilities;
  [[= name{"io.modelcontextprotocol/clientInfo"}]] std::optional<Implementation>
      client_info;
  [[= name{"io.modelcontextprotocol/protocolVersion"}]] std::optional<std::string>
      protocol_version;
  [[= name{"progressToken"}]] std::optional<std::uint64_t> progress_token;
};

// The params of a request that has nothing but its `_meta`, such as
// `server/discover` and `ping`.
struct[[= skip_null]] RequestParams {
  [[= name{"_meta"}]] std::optional<RequestMeta> meta;
};

// The params of a list, such as `tools/list`.
struct[[= skip_null]] ListParams {
  [[= name{"_meta"}]] std::optional<RequestMeta> meta;
  std::optional<std::string> cursor;
};

struct[[= skip_null]] CallToolParams {
  [[= name{"_meta"}]] std::optional<RequestMeta> meta;
  Json arguments;
  std::string name;
};

struct[[ = rename_all{camel_case}, = skip_null ]] InitializeParams {
  [[= name{"_meta"}]] std::optional<RequestMeta> meta;
  ClientCapabilities capabilities;
  Implementation client_info;
  std::string protocol_version;
};

struct[[= rename_all{camel_case}]] CancelledParams {
  std::string reason;
  std::uint64_t request_id{};
};

} // namespace mcp::wire
