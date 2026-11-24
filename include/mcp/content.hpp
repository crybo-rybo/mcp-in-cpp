/// @file
/// Content for a model: the blocks of a tool's result or a prompt's message.
/// A block read from a client keeps what this library knows of it and passes
/// over the rest, such as `annotations`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mcp {

/// Text for the model.
struct [[ = scrydantic::tag{"text"}, = scrydantic::ignore_unknown ]] TextContent {
  std::string text;
};

/// An image, as base64 of its bytes. image() makes one from the bytes.
struct [[ = scrydantic::tag{"image"}, = scrydantic::rename_all{scrydantic::camel_case},
          = scrydantic::ignore_unknown ]]
ImageContent {
  /// The image's bytes in base64.
  std::string data;
  /// Such as `image/png`.
  std::string mime_type;
};

/// A sound, as base64 of its bytes. audio() makes one from the bytes.
struct [[ = scrydantic::tag{"audio"}, = scrydantic::rename_all{scrydantic::camel_case},
          = scrydantic::ignore_unknown ]]
AudioContent {
  /// The sound's bytes in base64.
  std::string data;
  /// Such as `audio/wav`.
  std::string mime_type;
};

/// What a resource holds: text, or bytes in base64 as `blob`. Exactly one of
/// the two is set.
struct [[ = scrydantic::rename_all{scrydantic::camel_case}, = scrydantic::skip_null,
          = scrydantic::ignore_unknown ]]
ResourceContents {
  /// The resource's URI.
  std::string uri;
  /// Such as `text/plain` or `image/png`.
  std::optional<std::string> mime_type{};
  /// The resource's text.
  std::optional<std::string> text{};
  /// The resource's bytes in base64.
  std::optional<std::string> blob{};
};

/// A resource whose contents come with the result.
struct [[ = scrydantic::tag{"resource"}, = scrydantic::ignore_unknown ]] EmbeddedResource {
  ResourceContents resource;
};

/// A resource that the client can read for itself.
struct [[ = scrydantic::tag{"resource_link"},
          = scrydantic::rename_all{scrydantic::camel_case}, = scrydantic::skip_null,
          = scrydantic::ignore_unknown ]]
ResourceLink {
  std::string uri;
  std::string name;
  std::optional<std::string> title{};
  std::optional<std::string> description{};
  std::optional<std::string> mime_type{};
  /// The resource's size in bytes.
  std::optional<std::int64_t> size{};
};

/// One block of content.
using ContentBlock =
    std::variant<TextContent, ImageContent, AudioContent, ResourceLink, EmbeddedResource>;

/// What a tool returns when its result is content for the model rather than a
/// value: text, images, sounds, and resources, in order.
///
/// A tool that returns Content, or a Result of it, sends these blocks as its
/// result's content as they are. It has no output schema and no structured
/// content.
using Content = std::vector<ContentBlock>;

/// The base64 spelling of some bytes, as content carries them.
[[nodiscard]] std::string base64(std::span<const std::byte> bytes);

/// The base64 spelling of the bytes a string holds.
[[nodiscard]] std::string base64(std::string_view bytes);

/// An image block from the image's bytes.
[[nodiscard]] ImageContent image(std::span<const std::byte> bytes, std::string mime_type);

/// A sound block from the sound's bytes.
[[nodiscard]] AudioContent audio(std::span<const std::byte> bytes, std::string mime_type);

/// A resource's text.
[[nodiscard]] ResourceContents text_resource(std::string uri, std::string text,
                                             std::string mime_type = "text/plain");

/// A resource's bytes.
[[nodiscard]] ResourceContents blob_resource(std::string uri,
                                             std::span<const std::byte> bytes,
                                             std::string mime_type);

} // namespace mcp
