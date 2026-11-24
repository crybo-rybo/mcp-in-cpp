#include <cstddef>
#include <cstdint>
#include <mcp/content.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

std::string base64(const std::span<const std::byte> bytes) {
  constexpr std::string_view digits =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string text;
  text.reserve((bytes.size() + 2) / 3 * 4);
  std::size_t at = 0;
  for (; at + 3 <= bytes.size(); at += 3) {
    const auto group = (std::to_integer<std::uint32_t>(bytes[at]) << 16U) |
                       (std::to_integer<std::uint32_t>(bytes[at + 1]) << 8U) |
                       std::to_integer<std::uint32_t>(bytes[at + 2]);
    text += digits[(group >> 18U) & 0x3FU];
    text += digits[(group >> 12U) & 0x3FU];
    text += digits[(group >> 6U) & 0x3FU];
    text += digits[group & 0x3FU];
  }
  if (const std::size_t left = bytes.size() - at; left > 0) {
    std::uint32_t group = std::to_integer<std::uint32_t>(bytes[at]) << 16U;
    if (left == 2) {
      group |= std::to_integer<std::uint32_t>(bytes[at + 1]) << 8U;
    }
    text += digits[(group >> 18U) & 0x3FU];
    text += digits[(group >> 12U) & 0x3FU];
    text += left == 2 ? digits[(group >> 6U) & 0x3FU] : '=';
    text += '=';
  }
  return text;
}

std::string base64(const std::string_view bytes) {
  return base64(std::as_bytes(std::span{bytes.data(), bytes.size()}));
}

ImageContent image(const std::span<const std::byte> bytes, std::string mime_type) {
  return {.data = base64(bytes), .mime_type = std::move(mime_type)};
}

AudioContent audio(const std::span<const std::byte> bytes, std::string mime_type) {
  return {.data = base64(bytes), .mime_type = std::move(mime_type)};
}

ResourceContents text_resource(std::string uri, std::string text, std::string mime_type) {
  return {.uri = std::move(uri),
          .mime_type = std::move(mime_type),
          .text = std::move(text),
          .blob = std::nullopt};
}

ResourceContents blob_resource(std::string uri, const std::span<const std::byte> bytes,
                               std::string mime_type) {
  return {.uri = std::move(uri),
          .mime_type = std::move(mime_type),
          .text = std::nullopt,
          .blob = base64(bytes)};
}

} // namespace mcp
