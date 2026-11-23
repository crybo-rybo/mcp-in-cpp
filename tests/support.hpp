// The tools and the helpers that the tests share.
#pragma once

#include <cstdint>
#include <expected>
#include <mcp/mcp.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sample {

struct Percent {
  [[= mcp::range{.min = 0, .max = 100}]] std::int32_t value{};
};

struct Point {
  double x{};
  double y{};
};

[[= mcp::tool{"Add two integers"}]] inline std::int32_t add(std::int32_t a,
                                                            std::int32_t b) {
  return a + b;
}

[[= mcp::tool{"Greet someone"}]] inline std::string greet(std::string who = "world") {
  return "hello " + who;
}

[[= mcp::tool{"Mirror a point"}]] inline Point mirror(Point point) {
  return {.x = -point.x, .y = -point.y};
}

[[= mcp::tool{"Do nothing"}]] inline void nothing() {}

[[= mcp::tool{"Refuse every call"}]] inline mcp::Status refuse() {
  return std::unexpected(mcp::tool_error("not today", "the door is locked"));
}

[[= mcp::tool{"Throw"}]] inline std::int32_t explode() {
  throw std::runtime_error{"a secret detail"};
}

[[= mcp::tool{"Return a value outside its own range"}]] inline Percent overflow() {
  return {.value = 150};
}

[[= mcp::tool{"Count to a number"}]] inline std::int32_t count(std::int32_t steps,
                                                               mcp::Progress progress) {
  for (std::int32_t step = 1; step <= steps; ++step) {
    progress.report(step, steps);
  }
  progress.report(0); // Not greater than the last report, so it is dropped.
  return steps;
}

[[= mcp::tool{"Name the revision that serves the call"}]] inline std::string
    revision(const mcp::Context& context) {
  return std::string{context.protocol_version};
}

} // namespace sample

namespace one_tool {

[[= mcp::tool{"Add two integers"}]] inline std::int32_t add(std::int32_t a,
                                                            std::int32_t b) {
  return a + b;
}

} // namespace one_tool

namespace support {

// The metadata that every request of the modern revision carries.
inline constexpr std::string_view meta =
    R"("_meta":{"io.modelcontextprotocol/clientCapabilities":{},)"
    R"("io.modelcontextprotocol/protocolVersion":"2026-07-28"})";

// What every result of the modern revision starts with.
inline constexpr std::string_view server_info =
    R"("_meta":{"io.modelcontextprotocol/serverInfo":{"name":"sample","version":"0.0.1"}})";

inline constexpr std::string_view int32_schema =
    R"({"maximum":2147483647,"minimum":-2147483648,"type":"integer"})";

// A server named `sample` with every tool of that namespace, which keeps its
// log lines in `log`.
inline mcp::Server sample_server(std::vector<std::string>& log) {
  mcp::Server server{"sample", "0.0.1"};
  server.log([&log](const std::string_view line) { log.emplace_back(line); });
  server.add<^^sample>();
  return server;
}

// A request of the modern revision. `members` are further members of its
// params, such as `"name":"add"`, or empty for none.
inline std::string modern(const std::string_view id, const std::string_view method,
                          const std::string_view members = {}) {
  std::string text{R"({"jsonrpc":"2.0","id":)"};
  text += id;
  text += R"(,"method":")";
  text += method;
  text += R"(","params":{)";
  text += meta;
  if (!members.empty()) {
    text += ",";
    text += members;
  }
  text += "}}";
  return text;
}

// A request without the modern metadata, as a client of an earlier revision
// sends it. `params` is the whole params object, or empty for none.
inline std::string legacy(const std::string_view id, const std::string_view method,
                          const std::string_view params = {}) {
  std::string text{R"({"jsonrpc":"2.0","id":)"};
  text += id;
  text += R"(,"method":")";
  text += method;
  text += '"';
  if (!params.empty()) {
    text += R"(,"params":)";
    text += params;
  }
  text += "}";
  return text;
}

// The response that carries `result`, which is the result object's JSON text.
inline std::string result(const std::string_view id, const std::string_view result) {
  std::string text{R"({"id":)"};
  text += id;
  text += R"(,"jsonrpc":"2.0","result":)";
  text += result;
  text += "}";
  return text;
}

// The response that carries an error with no data. An empty `id` is a response
// to a message whose id could not be read.
inline std::string error(const std::string_view id, const int code,
                         const std::string_view message) {
  std::string text{R"({"error":{"code":)"};
  text += std::to_string(code);
  text += R"(,"message":")";
  text += message;
  text += R"("})";
  if (!id.empty()) {
    text += R"(,"id":)";
    text += id;
  }
  text += R"(,"jsonrpc":"2.0"})";
  return text;
}

// The result of a tool call under the modern revision. `body` holds the
// members between `_meta` and `resultType`: `content` and `isError`.
inline std::string called(const std::string_view id, const std::string_view body,
                          const std::string_view structured = {}) {
  std::string text{"{"};
  text += server_info;
  text += ",";
  text += body;
  text += R"(,"resultType":"complete")";
  if (!structured.empty()) {
    text += R"(,"structuredContent":)";
    text += structured;
  }
  text += "}";
  return result(id, text);
}

// One text block and the error flag, as `called` takes them.
inline std::string text_content(const std::string_view escaped, const bool is_error) {
  std::string text{R"("content":[{"text":")"};
  text += escaped;
  text += R"(","type":"text"}],"isError":)";
  text += is_error ? "true" : "false";
  return text;
}

inline std::string initialize(const std::string_view version) {
  std::string params{R"({"protocolVersion":")"};
  params += version;
  params += R"(","capabilities":{},"clientInfo":{"name":"test","version":"1"}})";
  return legacy("0", "initialize", params);
}

} // namespace support
