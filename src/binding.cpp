#include <cstddef>
#include <cstdint>
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

namespace {

// Whether text is UTF-8, as a JSON string must be.
[[nodiscard]] bool is_utf8(const std::string_view text) noexcept {
  std::size_t at = 0;
  while (at < text.size()) {
    const auto lead = static_cast<unsigned char>(text[at]);
    std::size_t length = 0;
    std::uint32_t point = 0;
    if (lead < 0x80) {
      ++at;
      continue;
    }
    if ((lead & 0xE0U) == 0xC0) {
      length = 2;
      point = lead & 0x1FU;
    } else if ((lead & 0xF0U) == 0xE0) {
      length = 3;
      point = lead & 0x0FU;
    } else if ((lead & 0xF8U) == 0xF0) {
      length = 4;
      point = lead & 0x07U;
    } else {
      return false;
    }
    if (at + length > text.size()) {
      return false;
    }
    for (std::size_t next = 1; next < length; ++next) {
      const auto byte = static_cast<unsigned char>(text[at + next]);
      if ((byte & 0xC0U) != 0x80) {
        return false;
      }
      point = (point << 6U) | (byte & 0x3FU);
    }
    constexpr std::uint32_t least[] = {0, 0, 0x80, 0x800, 0x10000};
    if (point < least[length] || point > 0x10FFFF ||
        (point >= 0xD800 && point <= 0xDFFF)) {
      return false;
    }
    at += length;
  }
  return true;
}

using Variables = std::vector<std::pair<std::string_view, std::string>>;

// Matches the rest of a template, from `pattern`, against the rest of a URI,
// from `at`. A variable takes the longest text that lets the rest match.
[[nodiscard]] bool match_from(const std::string_view uri_template,
                              const std::size_t pattern, const std::string_view uri,
                              const std::size_t at, Variables& variables) {
  if (pattern == uri_template.size()) {
    return at == uri.size();
  }
  if (uri_template[pattern] != '{') {
    return at < uri.size() && uri[at] == uri_template[pattern] &&
           match_from(uri_template, pattern + 1, uri, at + 1, variables);
  }
  const std::size_t close = uri_template.find('}', pattern);
  std::string_view name = uri_template.substr(pattern + 1, close - pattern - 1);
  const bool reserved = name.starts_with('+');
  if (reserved) {
    name.remove_prefix(1);
  }
  for (std::size_t end = uri.size(); end > at; --end) {
    const std::string_view value = uri.substr(at, end - at);
    // A simple variable stays within a path segment.
    if (!reserved && value.find_first_of("/?#") != std::string_view::npos) {
      continue;
    }
    std::optional<std::string> decoded =
        reserved ? std::optional<std::string>{std::string{value}}
                 : percent_decoded(value);
    if (!decoded.has_value() || !is_utf8(*decoded)) {
      continue;
    }
    variables.emplace_back(name, std::move(*decoded));
    if (match_from(uri_template, close + 1, uri, end, variables)) {
      return true;
    }
    variables.pop_back();
  }
  return false;
}

} // namespace

std::optional<std::string> match_uri_template(const std::string_view uri_template,
                                              const std::string_view uri) {
  // Longer URIs are not resources a client can be expected to read.
  constexpr std::size_t longest = 8192;
  Variables variables;
  if (uri.size() > longest || !match_from(uri_template, 0, uri, 0, variables)) {
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
                                            const std::string_view mime_type,
                                            std::string text) {
  return {text_resource(std::string{uri}, std::move(text),
                        std::string{mime_type.empty() ? "text/plain" : mime_type})};
}

std::vector<ResourceContents> contents_from(const std::string_view uri,
                                            const std::string_view mime_type,
                                            const std::vector<std::byte>& bytes) {
  return {blob_resource(
      std::string{uri}, bytes,
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
