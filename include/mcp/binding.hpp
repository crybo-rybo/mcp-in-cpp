/// @file
/// Resources and prompts bound from annotated functions: what a server lists
/// for each, and the handler that reads or renders it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <mcp/annotations.hpp>
#include <mcp/content.hpp>
#include <mcp/context.hpp>
#include <meta>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mcp {

/// Who a prompt's message is from.
enum class Role : std::uint8_t { user, assistant };

/// One message of a prompt.
struct PromptMessage {
  Role role{Role::user};
  ContentBlock content;
};

/// A resource or a resource template, ready to list and read.
struct BoundResource {
  /// The resource's URI, or the template when `is_template`.
  std::string uri{};
  /// Whether `uri` is a template with variables.
  bool is_template{};
  std::string name{};
  /// Empty for none.
  std::string title{};
  /// Empty for none.
  std::string description{};
  /// Empty for none.
  std::string mime_type{};
  /// Reads the resource at `uri`, which matched. `variables` is an object that
  /// holds the text of each of the template's variables, or is empty.
  std::function<Result<std::vector<ResourceContents>>(
      const Context& context, std::string_view uri,
      const scrydantic::JsonView& variables)>
      read{};
};

/// One argument of a prompt.
struct PromptArgument {
  std::string name{};
  /// Empty for none.
  std::string description{};
  bool required{};
};

/// A prompt, ready to list and render.
struct BoundPrompt {
  std::string name{};
  /// Empty for none.
  std::string title{};
  std::string description{};
  std::vector<PromptArgument> arguments{};
  /// Renders the prompt from `arguments`, an object of strings.
  std::function<Result<std::vector<PromptMessage>>(
      const Context& context, const scrydantic::JsonView& arguments)>
      get{};
};

/// Matches a URI against a template of mcp::resource. On a match, returns the
/// variables as a JSON object of their decoded text.
[[nodiscard]] std::optional<std::string>
match_uri_template(std::string_view uri_template, std::string_view uri);

namespace detail {

// The variables of a URI template, or the reason it is not one this library
// matches.
struct template_variables {
  std::vector<std::string_view> names;
  std::string_view problem;
};

// It reads the text a character at a time: under -fsanitize=undefined, GCC
// cannot evaluate string_view's searches of an annotation's static text at
// compile time.
consteval template_variables variables_of(const std::string_view uri) {
  template_variables found;
  std::size_t at = 0;
  while (at < uri.size()) {
    if (uri[at] != '{') {
      ++at;
      continue;
    }
    std::size_t end = at + 1;
    while (end < uri.size() && uri[end] != '}') {
      ++end;
    }
    if (end == uri.size()) {
      found.problem = "a '{' in the URI template has no '}'";
      return found;
    }
    std::size_t first = at + 1;
    if (first < end && uri[first] == '+') {
      ++first;
    }
    bool plain = first < end;
    for (std::size_t c = first; c < end; ++c) {
      const char letter = uri[c];
      plain = plain &&
              ((letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
               (letter >= '0' && letter <= '9') || letter == '_');
    }
    if (!plain) {
      found.problem = "a URI template variable must be {name} or {+name}";
      return found;
    }
    if (end + 1 < uri.size() && uri[end + 1] == '{') {
      found.problem = "two URI template variables must have text between them";
      return found;
    }
    found.names.push_back(uri.substr(first, end - first));
    at = end + 1;
  }
  return found;
}

consteval bool is_uri_template(const std::string_view uri) {
  return !variables_of(uri).names.empty();
}

// Why a resource function's parameters do not fit its URI template, or empty.
template <typename Arguments>
consteval std::string_view resource_problem(std::string_view uri) {
  const template_variables variables = variables_of(uri);
  if (!variables.problem.empty()) {
    return variables.problem;
  }
  for (const std::string_view name : variables.names) {
    if (std::ranges::find(scrydantic::keys_v<Arguments>, name) ==
        std::ranges::end(scrydantic::keys_v<Arguments>)) {
      return "a URI template variable names no parameter of the function";
    }
  }
  for (const scrydantic::FieldDescriptor& field : scrydantic::fields_v<Arguments>) {
    if (field.required &&
        std::ranges::find(variables.names, field.key) == variables.names.end()) {
      return "a parameter of the function is not a variable of its URI template";
    }
  }
  return {};
}

template <typename Type> struct is_result : std::false_type {};
template <typename Type>
struct is_result<std::expected<Type, scrydantic::Error>> : std::true_type {};

[[nodiscard]] std::vector<ResourceContents>
contents_from(std::string_view uri, std::string_view mime_type, std::string text);
[[nodiscard]] std::vector<ResourceContents>
contents_from(std::string_view uri, std::string_view mime_type,
              const std::vector<std::byte>& bytes);
[[nodiscard]] std::vector<ResourceContents>
contents_from(std::string_view uri, std::vector<ResourceContents> contents);

// What a resource holds, from what its function returned.
template <typename Value>
Result<std::vector<ResourceContents>>
resource_contents(const std::string_view uri, const std::string_view mime_type,
                  Value&& value) {
  using Type = std::remove_cvref_t<Value>;
  if constexpr (is_result<Type>::value) {
    if (!value) {
      return std::unexpected(std::move(value).error());
    }
    return resource_contents(uri, mime_type, std::move(*value));
  } else if constexpr (std::is_same_v<Type, std::string>) {
    return contents_from(uri, mime_type, std::string{std::forward<Value>(value)});
  } else if constexpr (std::is_same_v<Type, std::vector<std::byte>>) {
    return contents_from(uri, mime_type, value);
  } else if constexpr (std::is_same_v<Type, ResourceContents>) {
    return contents_from(uri,
                         std::vector<ResourceContents>{std::forward<Value>(value)});
  } else if constexpr (std::is_same_v<Type, std::vector<ResourceContents>>) {
    return contents_from(uri, std::forward<Value>(value));
  } else {
    Result<scrydantic::Json> json = scrydantic::encode(value);
    if (!json) {
      return std::unexpected(std::move(json).error());
    }
    return contents_from(uri, mime_type.empty() ? "application/json" : mime_type,
                         std::move(json->text));
  }
}

// A prompt's messages, from what its function returned.
template <typename Value>
Result<std::vector<PromptMessage>> prompt_messages(Value&& value) {
  using Type = std::remove_cvref_t<Value>;
  if constexpr (is_result<Type>::value) {
    if (!value) {
      return std::unexpected(std::move(value).error());
    }
    return prompt_messages(std::move(*value));
  } else if constexpr (std::is_same_v<Type, std::string>) {
    return std::vector<PromptMessage>{
        {.role = Role::user, .content = TextContent{std::forward<Value>(value)}}};
  } else if constexpr (std::is_same_v<Type, PromptMessage>) {
    return std::vector<PromptMessage>{std::forward<Value>(value)};
  } else if constexpr (std::is_same_v<Type, std::vector<PromptMessage>>) {
    return std::forward<Value>(value);
  } else if constexpr (std::is_same_v<Type, Content>) {
    std::vector<PromptMessage> messages;
    for (ContentBlock& block : value) {
      messages.push_back({.role = Role::user, .content = std::move(block)});
    }
    return messages;
  } else {
    static_assert(
        std::is_constructible_v<ContentBlock, Type>,
        "a prompt returns a std::string, an mcp::ContentBlock, mcp::Content, an "
        "mcp::PromptMessage, a vector of them, or a Result of one of these");
    return std::vector<PromptMessage>{
        {.role = Role::user, .content = ContentBlock{std::forward<Value>(value)}}};
  }
}

consteval std::string_view name_or(const std::string_view given,
                                   const std::meta::info function) {
  return given.empty() ? std::meta::identifier_of(function) : given;
}

template <typename Annotation>
consteval Annotation annotation_of(const std::meta::info function) {
  return std::meta::extract<Annotation>(
      std::meta::annotations_of_with_type(function, ^^Annotation)[0]);
}

// The decode options for text that arrives where a value is wanted: a URI's
// variables and a prompt's arguments are all strings.
inline constexpr scrydantic::DecodeOptions from_text{.coerce_scalars = true,
                                                     .coerce_structures = false};

template <std::meta::info Function, typename Binding> BoundResource bind_resource() {
  static constexpr resource annotation = annotation_of<resource>(Function);
  using Arguments = scrydantic::arguments_t<Function, Context, Binding>;
  static_assert(resource_problem<Arguments>(annotation.uri.view()).empty(),
                std::string_view{resource_problem<Arguments>(annotation.uri.view())});
  return {
      .uri = std::string{annotation.uri.view()},
      .is_template = is_uri_template(annotation.uri.view()),
      .name = std::string{name_or(annotation.name.view(), Function)},
      .title = std::string{annotation.title.view()},
      .description = std::string{annotation.description.view()},
      .mime_type = std::string{annotation.mime_type.view()},
      .read = [](const Context& context, const std::string_view uri,
                 const scrydantic::JsonView& variables)
          -> Result<std::vector<ResourceContents>> {
        Result<Arguments> arguments =
            scrydantic::decode<Arguments>(variables, from_text);
        if (!arguments) {
          return std::unexpected(std::move(arguments).error());
        }
        auto called = scrydantic::try_apply<Function, Context, Binding>(
            context, std::move(*arguments));
        if (!called) {
          return std::unexpected(std::move(called).error());
        }
        return resource_contents(uri, annotation.mime_type.view(), std::move(*called));
      },
  };
}

// The positions in fields_v<Arguments> of the fields, in the order their
// members are declared: a function's parameters, in their order.
template <typename Arguments> consteval std::vector<std::size_t> declaration_order() {
  std::vector<std::size_t> order;
  for (const std::meta::info member : std::meta::nonstatic_data_members_of(
           ^^Arguments, std::meta::access_context::unchecked())) {
    std::size_t field = 0;
    for (const std::meta::info candidate : scrydantic::field_members_v<Arguments>) {
      if (candidate == member) {
        order.push_back(field);
      }
      ++field;
    }
  }
  return order;
}

template <std::meta::info Function, typename Binding> BoundPrompt bind_prompt() {
  static constexpr prompt annotation = annotation_of<prompt>(Function);
  using Arguments = scrydantic::arguments_t<Function, Context, Binding>;
  BoundPrompt bound{
      .name = std::string{name_or(annotation.name.view(), Function)},
      .title = std::string{annotation.title.view()},
      .description = std::string{annotation.description.view()},
      .arguments = {},
      .get = [](const Context& context, const scrydantic::JsonView& arguments)
          -> Result<std::vector<PromptMessage>> {
        Result<Arguments> decoded = scrydantic::decode<Arguments>(arguments, from_text);
        if (!decoded) {
          return std::unexpected(std::move(decoded).error());
        }
        auto called = scrydantic::try_apply<Function, Context, Binding>(
            context, std::move(*decoded));
        if (!called) {
          return std::unexpected(std::move(called).error());
        }
        return prompt_messages(std::move(*called));
      },
  };
  static constexpr std::span<const std::size_t> order =
      std::define_static_array(declaration_order<Arguments>());
  for (const std::size_t position : order) {
    const scrydantic::FieldDescriptor& field =
        scrydantic::fields_v<Arguments>[position];
    bound.arguments.push_back({.name = std::string{field.key},
                               .description = std::string{field.description},
                               .required = field.required});
  }
  return bound;
}

// One annotated function and how its parameters read where it was bound.
template <std::meta::info Function, typename Binding> struct entry {};
template <typename... Entries> struct entries {};

// The functions of `entity` that carry `annotation`: those of a namespace, in
// declaration order, or the one function, if it carries it.
consteval std::vector<std::meta::info> marked(const std::meta::info entity,
                                              const std::meta::info annotation) {
  if (std::meta::is_namespace(entity)) {
    return scrydantic::annotated_functions_of(entity, annotation);
  }
  if (std::meta::is_function(entity) &&
      !std::meta::annotations_of_with_type(entity, annotation).empty()) {
    return {entity};
  }
  return {};
}

// Reflects the entries for the functions of `entity` that carry `annotation`,
// with their parameters as the declarations before this point give them.
consteval std::meta::info entries_of(const std::meta::info entity,
                                     const std::meta::info annotation) {
  std::vector<std::meta::info> found;
  for (const std::meta::info function : marked(entity, annotation)) {
    found.push_back(std::meta::substitute(
        ^^entry, {
                     std::meta::reflect_constant(function),
                     scrydantic::binding_of(function, ^^Context)}));
  }
  return std::meta::substitute(^^entries, found);
}

template <std::meta::info... Functions, typename... Bindings>
std::vector<BoundResource>
bind_resources(entries<entry<Functions, Bindings>...> /*unused*/) {
  return {bind_resource<Functions, Bindings>()...};
}

template <std::meta::info... Functions, typename... Bindings>
std::vector<BoundPrompt>
bind_prompts(entries<entry<Functions, Bindings>...> /*unused*/) {
  return {bind_prompt<Functions, Bindings>()...};
}

} // namespace detail

} // namespace mcp
