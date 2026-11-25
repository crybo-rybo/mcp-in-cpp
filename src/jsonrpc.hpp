// The JSON-RPC envelopes, written by hand: the connection and the HTTP endpoint
// both answer with them, and the client sends them.
#pragma once

#include <initializer_list>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <string_view>

namespace mcp::jsonrpc {

[[nodiscard]] inline std::string
join(const std::initializer_list<std::string_view> parts) {
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
[[nodiscard]] inline std::string result_message(const std::string_view id,
                                                const std::string_view result) {
  return join({R"({"id":)", id, R"(,"jsonrpc":"2.0","result":)", result, "}"});
}

// `id` is empty when the request's own could not be read. `message` must be
// valid UTF-8: the server's own words, or text that came out of a parsed
// message. `data` is JSON text, or empty for none.
[[nodiscard]] inline std::string error_message(const std::string_view id,
                                               const int code,
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

// `params` is JSON text, or empty for a notification without them.
[[nodiscard]] inline std::string notification_message(const std::string_view method,
                                                      const std::string_view params) {
  if (params.empty()) {
    return join({R"({"jsonrpc":"2.0","method":")", method, R"("})"});
  }
  return join(
      {R"({"jsonrpc":"2.0","method":")", method, R"(","params":)", params, "}"});
}

// A request. `id` and `params` are JSON text, and `method` needs no escaping.
[[nodiscard]] inline std::string request_message(const std::string_view id,
                                                 const std::string_view method,
                                                 const std::string_view params) {
  return join({R"({"id":)", id, R"(,"jsonrpc":"2.0","method":")", method,
               R"(","params":)", params, "}"});
}

} // namespace mcp::jsonrpc
