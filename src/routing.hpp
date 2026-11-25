// What Streamable HTTP under 2026-07-28 mirrors from a request's body into
// its headers, so that a proxy can route the request without reading it: a
// value in the form a header can hold, and the arguments that a tool's input
// schema marks with `x-mcp-header`.
#pragma once

#include <expected>
#include <mcp/http.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::routing {

// A value as a header carries it: itself when it is printable ASCII with no
// white space at either end, and otherwise `=?base64?...?=` around the Base64
// of its UTF-8. A value that looks like that form is encoded too, so that it
// is never read as one.
[[nodiscard]] std::string header_value(std::string_view value);

// One argument that a tool call mirrors into a header: the name that follows
// `Mcp-Param-`, and the chain of `properties` keys that leads to it.
struct Param {
  std::string header;
  std::vector<std::string> path;
};

// The arguments that a tool's input schema marks with `x-mcp-header`, or why
// a mark breaks the rules of 2026-07-28, which makes the tool unusable: a
// name that is empty, is not an HTTP token, or repeats another without regard
// to case; a mark on a property that is not a string, an integer or a
// boolean; or one anywhere but on a property reached through `properties`
// alone.
[[nodiscard]] std::expected<std::vector<Param>, std::string>
params_of(std::string_view schema);

// The headers of a call with these arguments, a JSON object: one for each
// marked argument that has a value other than null.
[[nodiscard]] std::vector<HttpHeader> headers_for(const std::vector<Param>& params,
                                                  std::string_view arguments);

} // namespace mcp::routing
