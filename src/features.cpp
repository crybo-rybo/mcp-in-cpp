#include "features.hpp"

#include "jsonrpc.hpp"
#include "verbatim.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <mcp/binding.hpp>
#include <mcp/channel.hpp>
#include <mcp/client.hpp>
#include <mcp/content.hpp>
#include <mcp/context.hpp>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

namespace {

using jsonrpc::join;

using scrydantic::Json;
using scrydantic::JsonKind;
using scrydantic::JsonView;

[[nodiscard]] ClientError fault(const Fault kind, std::string message) {
  return {
      .fault = kind, .code = 0, .message = std::move(message), .data = std::nullopt};
}

[[nodiscard]] std::unexpected<ClientError> malformed(std::string message) {
  return std::unexpected(fault(Fault::protocol, std::move(message)));
}

[[nodiscard]] bool is_object(const std::optional<JsonView>& value) noexcept {
  return value.has_value() && value->kind() == JsonKind::object;
}

[[nodiscard]] bool is_null(const std::optional<JsonView>& value) noexcept {
  return !value.has_value() || value->kind() == JsonKind::null;
}

// A string member, or nothing when it is absent or not a string.
[[nodiscard]] std::optional<std::string_view> text_of(const JsonView& object,
                                                      const std::string_view key) {
  const std::optional<JsonView> value = object.find(key);
  return value.has_value() ? value->string() : std::nullopt;
}

// A string member for display, which is empty when it is absent or not a
// string, as a tool's title is.
[[nodiscard]] std::string shown(const JsonView& object, const std::string_view key) {
  return std::string{text_of(object, key).value_or("")};
}

// A member that the client does not interpret, as the server wrote it, unless
// it is absent or null.
[[nodiscard]] std::optional<Json>
kept(const JsonView& entry, const std::string_view text, const std::string_view key) {
  if (is_null(entry.find(key))) {
    return std::nullopt;
  }
  return Json{.text = std::string{verbatim::member(text, key).value_or("null")}};
}

// Reads each entry of the array `key` of a list's result with `read`, which
// takes the entry, its text, and its position. Every entry is an object.
template <typename Read>
[[nodiscard]] Reply<void> each_entry(const std::string_view text,
                                     const JsonView& result, const std::string_view key,
                                     const std::string_view what, const Read& read) {
  const std::optional<JsonView> list = result.find(key);
  const std::optional<std::string_view> raw = verbatim::member(text, key);
  const std::optional<std::vector<std::string_view>> entries =
      raw.has_value() ? verbatim::elements(*raw) : std::nullopt;
  if (!list.has_value() || list->kind() != JsonKind::array || !entries.has_value() ||
      entries->size() != list->size()) {
    return malformed(join({"the server's ", what, " list is not an array"}));
  }
  for (std::size_t index = 0; index < list->size(); ++index) {
    const JsonView entry = *list->at(index);
    if (entry.kind() != JsonKind::object) {
      return malformed(join({what, " ", std::to_string(index),
                             " of the server's list is not an object"}));
    }
    if (Reply<void> done = read(entry, (*entries)[index], index); !done) {
      return done;
    }
  }
  return {};
}

[[nodiscard]] std::unexpected<ClientError> lacks(const std::string_view what,
                                                 const std::size_t index,
                                                 const std::string_view needs) {
  return malformed(
      join({what, " ", std::to_string(index), " of the server's list needs ", needs}));
}

[[nodiscard]] std::optional<LogLevel> level_named(const std::string_view name) {
  constexpr std::array<std::string_view, 8> names{
      "debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"};
  for (std::size_t level = 0; level < names.size(); ++level) {
    if (names[level] == name) {
      return static_cast<LogLevel>(level);
    }
  }
  return std::nullopt;
}

} // namespace

namespace features {

Reply<void> read_resources(const std::string_view text, const JsonView& result,
                           std::vector<Resource>& resources) {
  return each_entry(
      text, result, "resources", "resource",
      [&resources](const JsonView& entry, const std::string_view written,
                   const std::size_t index) -> Reply<void> {
        const std::optional<std::string_view> uri = text_of(entry, "uri");
        const std::optional<std::string_view> name = text_of(entry, "name");
        if (!uri.has_value() || !name.has_value()) {
          return lacks("resource", index, "a uri and a name");
        }
        Resource resource{.uri = std::string{*uri},
                          .name = std::string{*name},
                          .title = shown(entry, "title"),
                          .description = shown(entry, "description"),
                          .mime_type = shown(entry, "mimeType"),
                          .size = std::nullopt,
                          .annotations = kept(entry, written, "annotations"),
                          .icons = kept(entry, written, "icons"),
                          .meta = kept(entry, written, "_meta")};
        // A size that is not a count of bytes says nothing, and is passed over.
        if (const std::optional<JsonView> size = entry.find("size")) {
          const std::optional<std::uint64_t> bytes = size->unsigned_integer();
          if (bytes.has_value() &&
              *bytes <= static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max())) {
            resource.size = static_cast<std::int64_t>(*bytes);
          }
        }
        resources.push_back(std::move(resource));
        return {};
      });
}

Reply<void> read_templates(const std::string_view text, const JsonView& result,
                           std::vector<ResourceTemplate>& templates) {
  return each_entry(
      text, result, "resourceTemplates", "resource template",
      [&templates](const JsonView& entry, const std::string_view written,
                   const std::size_t index) -> Reply<void> {
        const std::optional<std::string_view> uri = text_of(entry, "uriTemplate");
        const std::optional<std::string_view> name = text_of(entry, "name");
        if (!uri.has_value() || !name.has_value()) {
          return lacks("resource template", index, "a uriTemplate and a name");
        }
        templates.push_back({.uri_template = std::string{*uri},
                             .name = std::string{*name},
                             .title = shown(entry, "title"),
                             .description = shown(entry, "description"),
                             .mime_type = shown(entry, "mimeType"),
                             .annotations = kept(entry, written, "annotations"),
                             .icons = kept(entry, written, "icons"),
                             .meta = kept(entry, written, "_meta")});
        return {};
      });
}

Reply<void> read_prompts(const std::string_view text, const JsonView& result,
                         std::vector<Prompt>& prompts) {
  return each_entry(
      text, result, "prompts", "prompt",
      [&prompts](const JsonView& entry, const std::string_view written,
                 const std::size_t index) -> Reply<void> {
        const std::optional<std::string_view> name = text_of(entry, "name");
        const std::optional<JsonView> arguments = entry.find("arguments");
        if (!name.has_value() ||
            (!is_null(arguments) && arguments->kind() != JsonKind::array)) {
          return lacks("prompt", index, "a name, and arguments that are an array");
        }
        Prompt prompt{.name = std::string{*name},
                      .title = shown(entry, "title"),
                      .description = shown(entry, "description"),
                      .arguments = {},
                      .icons = kept(entry, written, "icons"),
                      .meta = kept(entry, written, "_meta")};
        for (std::size_t at = 0; !is_null(arguments) && at < arguments->size(); ++at) {
          const JsonView argument = *arguments->at(at);
          const std::optional<std::string_view> argument_name =
              argument.kind() == JsonKind::object ? text_of(argument, "name")
                                                  : std::nullopt;
          if (!argument_name.has_value()) {
            return lacks("prompt", index, "arguments that each have a name");
          }
          const std::optional<JsonView> required = argument.find("required");
          prompt.arguments.push_back(
              {.name = std::string{*argument_name},
               .title = shown(argument, "title"),
               .description = shown(argument, "description"),
               .required = required.has_value() && required->boolean() == true});
        }
        prompts.push_back(std::move(prompt));
        return {};
      });
}

Reply<std::vector<ResourceContents>> read_contents(const JsonView& result) {
  const std::optional<JsonView> contents = result.find("contents");
  if (!contents.has_value() || contents->kind() != JsonKind::array) {
    return malformed("the server's resource has no contents array");
  }
  std::vector<ResourceContents> items;
  for (std::size_t index = 0; index < contents->size(); ++index) {
    scrydantic::Result<ResourceContents> item =
        scrydantic::decode<ResourceContents>(*contents->at(index));
    if (!item) {
      return malformed(join({"the server's resource has contents this client cannot "
                             "read: ",
                             item.error().message}));
    }
    if (item->text.has_value() == item->blob.has_value()) {
      return malformed("each item of a resource's contents needs text or a blob, not "
                       "both");
    }
    items.push_back(std::move(*item));
  }
  return items;
}

Reply<PromptResult> read_prompt(const JsonView& result) {
  const std::optional<JsonView> messages = result.find("messages");
  if (!messages.has_value() || messages->kind() != JsonKind::array) {
    return malformed("the server's prompt has no messages array");
  }
  PromptResult prompt{.description = shown(result, "description"), .messages = {}};
  for (std::size_t index = 0; index < messages->size(); ++index) {
    const JsonView message = *messages->at(index);
    const std::optional<JsonView> role =
        message.kind() == JsonKind::object ? message.find("role") : std::nullopt;
    const std::optional<JsonView> content =
        message.kind() == JsonKind::object ? message.find("content") : std::nullopt;
    scrydantic::Result<Role> speaker = role.has_value()
                                           ? scrydantic::decode<Role>(*role)
                                           : std::unexpected(scrydantic::Error{});
    scrydantic::Result<ContentBlock> block =
        content.has_value() ? scrydantic::decode<ContentBlock>(*content)
                            : std::unexpected(scrydantic::Error{});
    if (!speaker || !block) {
      return malformed(join({"message ", std::to_string(index),
                             " of the server's prompt needs a role, user or assistant, "
                             "and content this client can read"}));
    }
    prompt.messages.push_back({.role = *speaker, .content = std::move(*block)});
  }
  return prompt;
}

Reply<Completion> read_completion(const JsonView& result) {
  const std::optional<JsonView> completion = result.find("completion");
  const std::optional<JsonView> values =
      is_object(completion) ? completion->find("values") : std::nullopt;
  if (!values.has_value() || values->kind() != JsonKind::array) {
    return malformed("the server's completion has no values array");
  }
  Completion found{.values = {}, .total = std::nullopt, .has_more = false};
  for (std::size_t index = 0; index < values->size(); ++index) {
    const std::optional<std::string_view> value = values->at(index)->string();
    if (!value.has_value()) {
      return malformed("the server's completion has a value that is not a string");
    }
    found.values.emplace_back(*value);
  }
  if (const std::optional<JsonView> total = completion->find("total");
      !is_null(total)) {
    const std::optional<std::uint64_t> count = total->unsigned_integer();
    if (!count.has_value() ||
        *count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return malformed("the server's completion has a total that is not a count");
    }
    found.total = static_cast<std::int64_t>(*count);
  }
  if (const std::optional<JsonView> more = completion->find("hasMore");
      !is_null(more)) {
    const std::optional<bool> flag = more->boolean();
    if (!flag.has_value()) {
      return malformed("the server's completion has a hasMore that is not a boolean");
    }
    found.has_more = *flag;
  }
  return found;
}

std::optional<LogMessage> read_log(const std::string_view text,
                                   const JsonView& params) {
  const std::optional<std::string_view> level =
      params.kind() == JsonKind::object ? text_of(params, "level") : std::nullopt;
  const std::optional<LogLevel> severity =
      level.has_value() ? level_named(*level) : std::nullopt;
  const std::optional<std::string_view> data = verbatim::member(text, "data");
  if (!severity.has_value() || !params.find("data").has_value() || !data.has_value()) {
    return std::nullopt;
  }
  return LogMessage{.level = *severity,
                    .logger = shown(params, "logger"),
                    .data = Json{.text = std::string{*data}}};
}

} // namespace features

namespace {

// Whether a character of a URI stands for itself in any part of it: RFC 3986's
// unreserved characters.
[[nodiscard]] bool is_unreserved(const char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '.' || c == '_' || c == '~';
}

// How much of a `{name}` value is percent-encoded: what must be, then also the
// characters of the template's own text, then every byte. A later way is
// tried only when the URI of an earlier one would read back otherwise.
enum class Escaping : std::uint8_t { needed, literals, all };

[[nodiscard]] std::string percent_encoded(const std::string_view value,
                                          const Escaping escaping,
                                          const std::string_view literals) {
  constexpr std::string_view digits = "0123456789ABCDEF";
  std::string text;
  for (const char c : value) {
    const bool kept =
        is_unreserved(c) &&
        (escaping == Escaping::needed || (escaping == Escaping::literals &&
                                          literals.find(c) == std::string_view::npos));
    if (kept) {
      text += c;
      continue;
    }
    const auto byte = static_cast<unsigned char>(c);
    text += '%';
    text += digits[byte >> 4U];
    text += digits[byte & 0x0FU];
  }
  return text;
}

// One part of a URI template: text, or a variable.
struct Part {
  std::string_view text;
  std::string_view variable;
  bool reserved{};
  std::string value;
};

// The value of an argument as text, as a URI or a prompt carries it: a string
// as it is, and a number or a boolean as JSON spells it.
[[nodiscard]] std::optional<std::string> as_text(const JsonView& value) {
  switch (value.kind()) {
  case JsonKind::string:
    return std::string{*value.string()};
  case JsonKind::boolean:
  case JsonKind::signed_integer:
  case JsonKind::unsigned_integer:
  case JsonKind::number:
    return value.to_json().text;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] Reply<JsonView> object_of(const Json& arguments) {
  scrydantic::Result<JsonView> parsed = JsonView::parse(arguments);
  if (!parsed || parsed->kind() != JsonKind::object) {
    return std::unexpected(
        fault(Fault::protocol, "the arguments must make a JSON object"));
  }
  return std::move(*parsed);
}

// The bytes of base64 text, or nothing when it is not base64.
[[nodiscard]] std::optional<std::vector<std::byte>> from_base64(std::string_view text) {
  while (!text.empty() && text.back() == '=') {
    text.remove_suffix(1);
  }
  std::vector<std::byte> bytes;
  bytes.reserve(text.size() * 3 / 4);
  std::uint32_t group = 0;
  int bits = 0;
  for (const char c : text) {
    std::uint32_t digit = 0;
    if (c >= 'A' && c <= 'Z') {
      digit = static_cast<std::uint32_t>(c - 'A');
    } else if (c >= 'a' && c <= 'z') {
      digit = static_cast<std::uint32_t>(c - 'a' + 26);
    } else if (c >= '0' && c <= '9') {
      digit = static_cast<std::uint32_t>(c - '0' + 52);
    } else if (c == '+') {
      digit = 62;
    } else if (c == '/') {
      digit = 63;
    } else {
      return std::nullopt;
    }
    group = (group << 6U) | digit;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(
          static_cast<std::byte>((group >> static_cast<unsigned>(bits)) & 0xFFU));
    }
  }
  // Six bits left over cannot end base64 text.
  if (bits >= 6) {
    return std::nullopt;
  }
  return bytes;
}

[[nodiscard]] std::unexpected<ClientError> mismatch(std::string message) {
  return std::unexpected(fault(Fault::mismatch, std::move(message)));
}

[[nodiscard]] Reply<ResourceContents>
only_item(std::vector<ResourceContents> contents) {
  if (contents.size() != 1) {
    return mismatch(join({"the resource has ", std::to_string(contents.size()),
                          " items of contents, not one"}));
  }
  return std::move(contents.front());
}

} // namespace

namespace detail {

Reply<std::string> expand_uri(const std::string_view uri_template,
                              const Json& arguments) {
  const Reply<JsonView> values = object_of(arguments);
  if (!values) {
    return std::unexpected(values.error());
  }
  std::vector<Part> parts;
  std::string literals;
  for (std::size_t at = 0; at < uri_template.size();) {
    const std::size_t open = uri_template.find('{', at);
    if (open != at) {
      const std::string_view text = uri_template.substr(at, open - at);
      literals += text;
      parts.push_back({.text = text, .variable = {}, .reserved = false, .value = {}});
      at = open == std::string_view::npos ? uri_template.size() : open;
      continue;
    }
    // The template is one that variables_of() accepted.
    const std::size_t close = uri_template.find('}', open);
    std::string_view name = uri_template.substr(open + 1, close - open - 1);
    const bool reserved = name.starts_with('+');
    if (reserved) {
      name.remove_prefix(1);
    }
    const std::optional<JsonView> value = values->find(name);
    std::optional<std::string> text =
        value.has_value() ? as_text(*value) : std::nullopt;
    if (!text.has_value() || text->empty()) {
      return std::unexpected(
          fault(Fault::protocol,
                join({"the URI template's variable ", name,
                      " needs a value of text, a number or a boolean that is "
                      "not empty"})));
    }
    parts.push_back({.text = {},
                     .variable = name,
                     .reserved = reserved,
                     .value = std::move(*text)});
    at = close + 1;
  }

  // What the server's matcher must read back.
  std::string expected{"{"};
  for (const Part& part : parts) {
    if (!part.variable.empty()) {
      if (expected.size() > 1) {
        expected += ',';
      }
      expected += scrydantic::escape_json_string(part.variable);
      expected += ':';
      expected += scrydantic::escape_json_string(part.value);
    }
  }
  expected += '}';

  for (const Escaping escaping :
       {Escaping::needed, Escaping::literals, Escaping::all}) {
    std::string uri;
    for (const Part& part : parts) {
      if (part.variable.empty()) {
        uri += part.text;
      } else {
        uri += part.reserved ? part.value
                             : percent_encoded(part.value, escaping, literals);
      }
    }
    if (match_uri_template(uri_template, uri) == expected) {
      return uri;
    }
  }
  return std::unexpected(
      fault(Fault::protocol, join({"the arguments make no URI of ", uri_template,
                                   " that reads back as the same values"})));
}

Reply<std::map<std::string, std::string>> argument_texts(const Json& arguments) {
  const Reply<JsonView> values = object_of(arguments);
  if (!values) {
    return std::unexpected(values.error());
  }
  std::map<std::string, std::string> texts;
  for (std::size_t index = 0; index < values->size(); ++index) {
    const std::string_view key = values->key_at(index).value_or("");
    const JsonView value = *values->find(key);
    if (value.kind() == JsonKind::null) {
      continue;
    }
    std::optional<std::string> text = as_text(value);
    if (!text.has_value()) {
      return std::unexpected(fault(
          Fault::protocol, join({"the argument ", key,
                                 " is not text, a number or a boolean, which a prompt "
                                 "takes"})));
    }
    texts.emplace(key, std::move(*text));
  }
  return texts;
}

Reply<std::string> resource_text(std::vector<ResourceContents> contents) {
  Reply<ResourceContents> item = only_item(std::move(contents));
  if (!item) {
    return std::unexpected(std::move(item).error());
  }
  if (!item->text.has_value()) {
    return mismatch("the resource holds bytes, not text");
  }
  return std::move(*item->text);
}

Reply<std::vector<std::byte>>
resource_bytes(const std::vector<ResourceContents>& contents) {
  if (contents.size() != 1) {
    return mismatch(join({"the resource has ", std::to_string(contents.size()),
                          " items of contents, not one"}));
  }
  if (!contents.front().blob.has_value()) {
    return mismatch("the resource holds text, not bytes");
  }
  std::optional<std::vector<std::byte>> bytes = from_base64(*contents.front().blob);
  if (!bytes.has_value()) {
    return mismatch("the resource's blob is not base64");
  }
  return std::move(*bytes);
}

Reply<ResourceContents> resource_item(std::vector<ResourceContents> contents) {
  return only_item(std::move(contents));
}

} // namespace detail

} // namespace mcp
