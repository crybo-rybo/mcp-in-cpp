#include <cstddef>
#include <mcp/binding.hpp>
#include <mcp/content.hpp>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

namespace {

[[nodiscard]] int hex_value(const char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// The text that percent-encoded `value` spells, or nothing when it spells no
// valid escape.
[[nodiscard]] std::optional<std::string> percent_decoded(const std::string_view value) {
  std::string text;
  text.reserve(value.size());
  for (std::size_t at = 0; at < value.size(); ++at) {
    if (value[at] != '%') {
      text += value[at];
      continue;
    }
    if (at + 2 >= value.size()) {
      return std::nullopt;
    }
    const int high = hex_value(value[at + 1]);
    const int low = hex_value(value[at + 2]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    text += static_cast<char>((high << 4) | low);
    at += 2;
  }
  return text;
}

} // namespace

std::optional<std::string> match_uri_template(const std::string_view uri_template,
                                              const std::string_view uri) {
  std::vector<std::pair<std::string_view, std::string>> variables;
  std::size_t pattern = 0;
  std::size_t at = 0;
  while (pattern < uri_template.size()) {
    const std::size_t open = uri_template.find('{', pattern);
    const std::string_view literal = uri_template.substr(pattern, open - pattern);
    if (uri.substr(at, literal.size()) != literal) {
      return std::nullopt;
    }
    at += literal.size();
    if (open == std::string_view::npos) {
      break;
    }
    const std::size_t close = uri_template.find('}', open);
    std::string_view name = uri_template.substr(open + 1, close - open - 1);
    const bool reserved = name.starts_with('+');
    if (reserved) {
      name.remove_prefix(1);
    }
    pattern = close + 1;
    // A variable runs to the literal text that follows it, or to the end. A
    // simple one stays within a path segment.
    const std::size_t next_open = uri_template.find('{', pattern);
    const std::string_view next = uri_template.substr(pattern, next_open - pattern);
    std::size_t end = next.empty() ? uri.size() : uri.find(next, at);
    if (end == std::string_view::npos) {
      return std::nullopt;
    }
    std::string_view value = uri.substr(at, end - at);
    if (!reserved && value.find_first_of("/?#") != std::string_view::npos) {
      return std::nullopt;
    }
    if (value.empty()) {
      return std::nullopt;
    }
    std::optional<std::string> decoded =
        reserved ? std::optional<std::string>{std::string{value}} : percent_decoded(value);
    if (!decoded.has_value()) {
      return std::nullopt;
    }
    variables.emplace_back(name, std::move(*decoded));
    at = end;
  }
  if (at != uri.size()) {
    return std::nullopt;
  }
  std::string object{"{"};
  for (const auto& [name, value] : variables) {
    if (object.size() > 1) {
      object += ',';
    }
    object += scrydantic::escape_json_string(name);
    object += ':';
    object += scrydantic::escape_json_string(value);
  }
  object += '}';
  return object;
}

namespace detail {

std::vector<ResourceContents> contents_from(const std::string_view uri,
                                            const std::string_view mime_type, std::string text) {
  return {text_resource(std::string{uri}, std::move(text),
                        std::string{mime_type.empty() ? "text/plain" : mime_type})};
}

std::vector<ResourceContents> contents_from(const std::string_view uri,
                                            const std::string_view mime_type,
                                            const std::vector<std::byte>& bytes) {
  return {blob_resource(std::string{uri}, bytes,
                        std::string{mime_type.empty() ? "application/octet-stream" : mime_type})};
}

std::vector<ResourceContents> contents_from(const std::string_view uri,
                                            std::vector<ResourceContents> contents) {
  for (ResourceContents& item : contents) {
    if (item.uri.empty()) {
      item.uri = uri;
    }
  }
  return contents;
}

} // namespace detail

} // namespace mcp
