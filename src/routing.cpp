#include "routing.hpp"

#include "jsonrpc.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mcp/http.hpp>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::routing {

namespace {

using jsonrpc::join;
using scrydantic::JsonKind;
using scrydantic::JsonView;

constexpr std::string_view sentinel_open = "=?base64?";
constexpr std::string_view sentinel_close = "?=";

[[nodiscard]] std::string base64(const std::string_view bytes) {
  constexpr std::string_view digits =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string text;
  text.reserve((bytes.size() + 2) / 3 * 4);
  std::size_t at = 0;
  for (; at + 3 <= bytes.size(); at += 3) {
    const std::uint32_t group =
        (std::uint32_t{static_cast<unsigned char>(bytes[at])} << 16U) |
        (std::uint32_t{static_cast<unsigned char>(bytes[at + 1])} << 8U) |
        std::uint32_t{static_cast<unsigned char>(bytes[at + 2])};
    text += digits[(group >> 18U) & 0x3FU];
    text += digits[(group >> 12U) & 0x3FU];
    text += digits[(group >> 6U) & 0x3FU];
    text += digits[group & 0x3FU];
  }
  if (const std::size_t left = bytes.size() - at; left > 0) {
    std::uint32_t group = std::uint32_t{static_cast<unsigned char>(bytes[at])} << 16U;
    if (left == 2) {
      group |= std::uint32_t{static_cast<unsigned char>(bytes[at + 1])} << 8U;
    }
    text += digits[(group >> 18U) & 0x3FU];
    text += digits[(group >> 12U) & 0x3FU];
    text += left == 2 ? digits[(group >> 6U) & 0x3FU] : '=';
    text += '=';
  }
  return text;
}

// Whether a header can carry the value as it is: visible ASCII and inner
// spaces only, as RFC 9110 has a field value, without the encoded form's
// look.
[[nodiscard]] bool plain(const std::string_view value) {
  if (!value.empty() && (value.front() == ' ' || value.back() == ' ')) {
    return false;
  }
  if (value.starts_with(sentinel_open) && value.ends_with(sentinel_close)) {
    return false;
  }
  return std::ranges::all_of(value,
                             [](const char c) { return c >= 0x20 && c <= 0x7E; });
}

// Whether a character may be in an HTTP token (RFC 9110, `tchar`).
[[nodiscard]] bool is_tchar(const char c) {
  constexpr std::string_view marks = "!#$%&'*+-.^_`|~";
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         marks.find(c) != std::string_view::npos;
}

[[nodiscard]] char lower(const char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool iequals(const std::string_view a, const std::string_view b) {
  return std::ranges::equal(
      a, b, [](const char x, const char y) { return lower(x) == lower(y); });
}

// Where in the schema a value lies: the root, a property reached through
// `properties` alone, or anywhere else.
enum class Place : std::uint8_t { root, property, other };

// The members whose value maps names to schemas rather than being one.
[[nodiscard]] bool names_schemas(const std::string_view key) {
  constexpr std::array<std::string_view, 5> maps{
      "properties", "patternProperties", "$defs", "definitions", "dependentSchemas"};
  return std::ranges::find(maps, key) != maps.end();
}

// The members whose value is an instance of the schema, not a schema, so that
// what it holds is data.
[[nodiscard]] bool holds_data(const std::string_view key) {
  constexpr std::array<std::string_view, 4> data{"const", "default", "enum",
                                                 "examples"};
  return std::ranges::find(data, key) != data.end();
}

class Walk {
public:
  void visit(const JsonView& value, const Place place) {
    if (problem.has_value()) {
      return;
    }
    if (value.kind() == JsonKind::array) {
      for (std::size_t index = 0; index < value.size(); ++index) {
        visit(*value.at(index), Place::other);
      }
      return;
    }
    if (value.kind() != JsonKind::object) {
      return;
    }
    if (const std::optional<JsonView> mark = value.find("x-mcp-header")) {
      mark_property(value, *mark, place);
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
      const std::string_view key = *value.key_at(index);
      const JsonView member = *value.find(key);
      if (key == "x-mcp-header" || holds_data(key)) {
        continue;
      }
      if (names_schemas(key) && member.kind() == JsonKind::object) {
        const Place inner = key == "properties" && place != Place::other
                                ? Place::property
                                : Place::other;
        for (std::size_t name = 0; name < member.size(); ++name) {
          const std::string_view property = *member.key_at(name);
          path_.emplace_back(property);
          visit(*member.find(property), inner);
          path_.pop_back();
        }
        continue;
      }
      visit(member, Place::other);
    }
  }

  std::vector<Param> params;
  std::optional<std::string> problem;

private:
  void mark_property(const JsonView& schema, const JsonView& mark, const Place place) {
    if (place != Place::property) {
      problem = std::string{"an x-mcp-header is on a schema that is not a property "
                            "reached through properties alone"};
      return;
    }
    const std::string where = join({"property ", dotted()});
    const std::optional<std::string_view> name = mark.string();
    if (!name.has_value() || name->empty() || !std::ranges::all_of(*name, is_tchar)) {
      problem = join({"x-mcp-header of ", where, " is not an HTTP token"});
      return;
    }
    const std::optional<JsonView> type = schema.find("type");
    const std::optional<std::string_view> kind =
        type.has_value() ? type->string() : std::nullopt;
    if (kind != "string" && kind != "integer" && kind != "boolean") {
      problem = join({"x-mcp-header of ", where,
                      " is on a type other than string, integer, or boolean"});
      return;
    }
    for (const Param& seen : params) {
      if (iequals(seen.header, *name)) {
        problem = join({"x-mcp-header ", *name, " is given twice"});
        return;
      }
    }
    params.push_back({.header = std::string{*name}, .path = path_});
  }

  [[nodiscard]] std::string dotted() const {
    std::string text;
    for (const std::string& key : path_) {
      if (!text.empty()) {
        text += '.';
      }
      text += key;
    }
    return text;
  }

  std::vector<std::string> path_;
};

} // namespace

std::string header_value(const std::string_view value) {
  if (plain(value)) {
    return std::string{value};
  }
  return join({sentinel_open, base64(value), sentinel_close});
}

std::expected<std::vector<Param>, std::string>
params_of(const std::string_view schema) {
  const scrydantic::Result<JsonView> parsed = JsonView::parse(schema);
  if (!parsed) {
    return std::unexpected(std::string{"the input schema is not JSON"});
  }
  Walk walk;
  walk.visit(*parsed, Place::root);
  if (walk.problem.has_value()) {
    return std::unexpected(std::move(*walk.problem));
  }
  return std::move(walk.params);
}

std::vector<HttpHeader> headers_for(const std::vector<Param>& params,
                                    const std::string_view arguments) {
  std::vector<HttpHeader> headers;
  if (params.empty()) {
    return headers;
  }
  const scrydantic::Result<JsonView> parsed = JsonView::parse(arguments);
  if (!parsed) {
    return headers;
  }
  for (const Param& param : params) {
    std::optional<JsonView> value = *parsed;
    for (const std::string& key : param.path) {
      value = value->find(key);
      if (!value.has_value()) {
        break;
      }
    }
    if (!value.has_value()) {
      continue;
    }
    std::string text;
    switch (value->kind()) {
    case JsonKind::string:
      text = header_value(*value->string());
      break;
    case JsonKind::boolean:
      text = *value->boolean() ? "true" : "false";
      break;
    case JsonKind::signed_integer:
    case JsonKind::unsigned_integer:
    case JsonKind::number:
      text = value->to_json().text;
      break;
    case JsonKind::null:
    case JsonKind::array:
    case JsonKind::object:
      // Null has no header, and neither has a value that is not one of the
      // types the mark allows: a server that wants one turns the call away.
      continue;
    }
    headers.push_back(
        {.name = join({"Mcp-Param-", param.header}), .value = std::move(text)});
  }
  return headers;
}

} // namespace mcp::routing
