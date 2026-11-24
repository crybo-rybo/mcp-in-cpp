/// @file
/// The annotations that make a function a resource or a prompt. A tool is
/// marked with mcp::tool, which is Scrydantic's own.
#pragma once

#include <meta>
#include <string_view>

namespace mcp {

/// Text that an annotation holds: a string literal, kept in static storage so
/// that the annotation is a constant.
struct static_text {
  consteval static_text(const char* text) // NOLINT(google-explicit-constructor)
      : value{std::define_static_string(std::string_view{text})} {}

  [[nodiscard]] constexpr std::string_view view() const noexcept { return value; }

  const char* value;
};

/// Marks a function as a resource, which a client reads by its URI.
///
///     [[= mcp::resource{.uri = "weather://{city}/alerts",
///                       .description = "Storm alerts for a city",
///                       .mime_type = "application/json"}]]
///     std::vector<Alert> alerts(std::string city);
///
/// A URI with variables in braces is a template, and each variable is a
/// parameter of the same name: `{city}` matches one path segment, and
/// `{+path}` any text, slashes included. The values arrive as text and are
/// read as the parameter's type, so `{id}` may be an integer. A URI without
/// variables is one resource, listed by `resources/list`; a template is listed
/// by `resources/templates/list`.
///
/// What the function returns is what the resource holds: a `std::string` is
/// its text, a `std::vector<std::byte>` its bytes, an mcp::ResourceContents or
/// a vector of them is given as it is, and any other value is its JSON, as
/// `application/json`. A Result that fails says the resource cannot be read,
/// and its model text is the error's message. Parameters that the server
/// supplies, such as `const mcp::Context&`, mcp::Progress and
/// `std::stop_token`, may be taken as a tool takes them.
struct resource {
  /// The resource's URI, or a URI template.
  static_text uri;
  /// What the resource is, for a client and its model.
  static_text description{""};
  /// The media type of its contents. Empty means `text/plain` for text,
  /// `application/octet-stream` for bytes, and `application/json` for a value.
  static_text mime_type{""};
  /// The name a client shows. Empty means the function's own name.
  static_text name{""};
  /// A title for people. Empty means none.
  static_text title{""};
};

/// Marks a function as a prompt, a message template that a person picks.
///
///     [[= mcp::prompt{"Plan a trip around the weather"}]]
///     std::string plan_trip(std::string city, std::int32_t days = 3);
///
/// Its parameters are the prompt's arguments, which arrive as text and are
/// read as the parameter's type; one with a default argument is optional, and
/// a parameter's mcp::description describes it. A `std::string` result is one
/// message from the user; an mcp::ContentBlock is too, mcp::Content is one user
/// message per block, and an mcp::PromptMessage or a vector of them is given as
/// it is. A Result that fails is an invalid request, with the error's model
/// text as its message.
struct prompt {
  /// What the prompt does.
  static_text description;
  /// The prompt's name. Empty means the function's own name.
  static_text name{""};
  /// A title for people. Empty means none.
  static_text title{""};
};

} // namespace mcp
