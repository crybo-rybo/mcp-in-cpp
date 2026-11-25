// Reads parts of a JSON text as they were written. A JsonView gives the
// canonical form of a value, with its keys sorted and its numbers respelled;
// what the client hands on from a server, such as a tool's schema, keeps the
// server's own text instead. Every function here reads text that
// JsonView::parse() has accepted, so it skips values without checking them.
#pragma once

#include <cstddef>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::verbatim {

namespace detail {

[[nodiscard]] constexpr bool is_space(const char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

[[nodiscard]] constexpr std::size_t skip_space(const std::string_view text,
                                               std::size_t at) noexcept {
  while (at < text.size() && is_space(text[at])) {
    ++at;
  }
  return at;
}

// Where the string whose opening quote is at `at` ends, past its closing quote.
[[nodiscard]] constexpr std::size_t string_end(const std::string_view text,
                                               std::size_t at) noexcept {
  for (++at; at < text.size() && text[at] != '"'; ++at) {
    if (text[at] == '\\') {
      ++at;
    }
  }
  return at < text.size() ? at + 1 : text.size();
}

// Where the value that starts at `at` ends.
[[nodiscard]] constexpr std::size_t value_end(const std::string_view text,
                                              std::size_t at) noexcept {
  if (at >= text.size()) {
    return text.size();
  }
  if (text[at] == '"') {
    return string_end(text, at);
  }
  if (text[at] == '{' || text[at] == '[') {
    std::size_t depth = 0;
    while (at < text.size()) {
      const char c = text[at];
      if (c == '"') {
        at = string_end(text, at);
        continue;
      }
      ++at;
      if (c == '{' || c == '[') {
        ++depth;
      } else if ((c == '}' || c == ']') && --depth == 0) {
        break;
      }
    }
    return at;
  }
  // A number, or true, false or null.
  while (at < text.size() && !is_space(text[at]) && text[at] != ',' &&
         text[at] != '}' && text[at] != ']') {
    ++at;
  }
  return at;
}

} // namespace detail

// The text of the value that `object` holds under `key`, or nothing when it is
// not an object or has no such member. Of several members with one key, the
// last counts, as JsonView has it.
[[nodiscard]] inline std::optional<std::string_view>
member(const std::string_view object, const std::string_view key) {
  std::size_t at = detail::skip_space(object, 0);
  if (at >= object.size() || object[at] != '{') {
    return std::nullopt;
  }
  std::optional<std::string_view> found;
  ++at;
  for (;;) {
    at = detail::skip_space(object, at);
    if (at >= object.size() || object[at] != '"') {
      return found;
    }
    const std::size_t key_end = detail::string_end(object, at);
    const std::string_view literal = object.substr(at, key_end - at);
    at = detail::skip_space(object, key_end) + 1; // Past the colon.
    at = detail::skip_space(object, at);
    const std::size_t end = detail::value_end(object, at);
    bool matches = false;
    if (literal.find('\\') == std::string_view::npos) {
      matches = literal.size() >= 2 && literal.substr(1, literal.size() - 2) == key;
    } else {
      // A key that spells a character with an escape.
      const scrydantic::Result<scrydantic::JsonView> spelled =
          scrydantic::JsonView::parse(literal);
      matches = spelled && spelled->string() == key;
    }
    if (matches) {
      found = object.substr(at, end - at);
    }
    at = detail::skip_space(object, end);
    if (at >= object.size() || object[at] != ',') {
      return found;
    }
    ++at;
  }
}

// The text of each element of `array`, or nothing when it is not an array.
[[nodiscard]] inline std::optional<std::vector<std::string_view>>
elements(const std::string_view array) {
  std::size_t at = detail::skip_space(array, 0);
  if (at >= array.size() || array[at] != '[') {
    return std::nullopt;
  }
  std::vector<std::string_view> found;
  at = detail::skip_space(array, at + 1);
  if (at < array.size() && array[at] == ']') {
    return found;
  }
  while (at < array.size()) {
    at = detail::skip_space(array, at);
    const std::size_t end = detail::value_end(array, at);
    found.push_back(array.substr(at, end - at));
    at = detail::skip_space(array, end);
    if (at >= array.size() || array[at] != ',') {
      break;
    }
    ++at;
  }
  return found;
}

// The text without the white space between its tokens, so that it fits on one
// line. Strings and numbers keep their spelling.
[[nodiscard]] inline std::string compact(const std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t at = 0;
  while (at < text.size()) {
    if (text[at] == '"') {
      const std::size_t end = detail::string_end(text, at);
      out += text.substr(at, end - at);
      at = end;
    } else {
      if (!detail::is_space(text[at])) {
        out += text[at];
      }
      ++at;
    }
  }
  return out;
}

} // namespace mcp::verbatim
