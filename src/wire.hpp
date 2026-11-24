// The protocol's own messages, as the plain structs Scrydantic encodes. Each is
// named as the MCP schema names it. They are written, never read: a request is
// read through a JsonView, because its envelope decides what the rest must be.
#pragma once

#include <cstdint>
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

struct[[= skip_null]] ServerCapabilities {
  std::optional<ToolsCapability> tools;
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

} // namespace mcp::wire
