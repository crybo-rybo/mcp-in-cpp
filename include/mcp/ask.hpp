/// @file
/// What a call may ask of the client while it runs: input from the person
/// (elicitation), a message from the client's model (sampling), and the
/// client's roots.
///
/// A tool, a resource, or a prompt takes an mcp::Elicitation, mcp::Sampling, or
/// mcp::Roots as a parameter in any position, and the server fills it in,
/// outside the input schema. Each works in both eras, and the function is
/// written the same way for both:
///
///     [[= mcp::tool{"Book a table"}]]
///     mcp::Result<std::string> book(std::string place, mcp::Elicitation ask) {
///       mcp::Result<mcp::Answer<Guest>> guest = ask.form<Guest>("Who is it for?");
///       if (!guest) {
///         return std::unexpected(guest.error());
///       }
///       if (guest->action != mcp::Action::accept) {
///         return "not booked";
///       }
///       return "booked for " + guest->content->name;
///     }
///
/// In a handshake session the question is a request to the client, and the
/// call waits for its answer. Under 2026-07-28 a call that asks something the
/// request has no answer for fails with an error that the function returns as
/// it is, and the server answers the request with `input_required`, naming
/// every question asked; the client asks the person, and sends the request
/// again with the answers, which the function then finds. The function runs
/// once per round, so what it does before it asks it does again: it asks
/// first, and acts once it has every answer. To ask several questions in one
/// round, it asks them all before it looks at any answer. The answers of
/// earlier rounds travel in a `requestState` that the server signs and binds to
/// the request, so a client cannot change an answer it gave; it can only answer
/// a question the call has still to ask, as any client may answer what it is
/// asked.
///
/// An Elicitation, a Sampling, or a Roots is valid only while the call that it
/// was given to runs; a function must not keep one.
///
/// Each question has a key, which names its answer across rounds. It is
/// `input-1`, `input-2`, and so on, in the order the call asks, unless the
/// call names one.
///
/// A client that did not declare the capability a question needs is not
/// asked: the call fails with an error that the function returns, and the
/// request fails with `-32021` and the capability it needs.
#pragma once

#include <cstdint>
#include <expected>
#include <mcp/binding.hpp>
#include <mcp/content.hpp>
#include <mcp/context.hpp>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mcp {

namespace detail {

// What the client was asked for.
enum class Need : std::uint8_t { elicitation, sampling, roots };

// Asks the client one question of the call that `context` serves, and returns
// its answer as JSON text: the result of the client's request.
[[nodiscard]] Result<scrydantic::Json> ask(const Context& context, Need need,
                                           std::string_view key, std::string_view method,
                                           std::string params);

// Whether the client of the call that `context` serves can be asked this.
[[nodiscard]] bool can_ask(const Context& context, Need need) noexcept;

// The JSON Schema of a form: a flat object of the fields of `Form`.
template <typename Form> std::string form_schema() {
  std::string properties;
  std::string required;
  for (const scrydantic::FieldDescriptor& field : scrydantic::fields_v<Form>) {
    if (!properties.empty()) {
      properties += ',';
    }
    properties += scrydantic::escape_json_string(field.key);
    properties += ':';
    properties += field.schema;
    if (field.required) {
      if (!required.empty()) {
        required += ',';
      }
      required += scrydantic::escape_json_string(field.key);
    }
  }
  return R"({"properties":{)" + properties + R"(},"required":[)" + required +
         R"(],"type":"object"})";
}

} // namespace detail

/// What the person did with a form.
enum class Action : std::uint8_t {
  /// Filled it in and sent it.
  accept,
  /// Said no.
  decline,
  /// Closed it without choosing.
  cancel,
};

namespace detail {

// The result of elicitation/create, before its content is read as the form.
struct [[ = scrydantic::ignore_unknown, = scrydantic::skip_null ]] ElicitReply {
  Action action{};
  std::optional<scrydantic::Json> content;
};

} // namespace detail

/// The person's answer to a form.
template <typename Form> struct Answer {
  Action action{Action::cancel};
  /// What the person entered, when the action is Action::accept.
  std::optional<Form> content{};
};

/// Asks the person using the client to fill in a form.
class Elicitation {
public:
  /// An Elicitation that can ask no one.
  Elicitation() = default;
  explicit Elicitation(const Context& context) noexcept : context_{&context} {}

  /// Whether the client can show a form.
  [[nodiscard]] explicit operator bool() const noexcept {
    return context_ != nullptr && detail::can_ask(*context_, detail::Need::elicitation);
  }

  /// Asks the person to fill in a form whose fields are the members of
  /// `Form`: strings, numbers, booleans, and enumerations, with their
  /// descriptions and constraints. A form must not ask for a secret, such as a
  /// password or a key.
  /// @param message What the form is for.
  /// @param key Names the question across rounds, or empty for the next of
  /// `input-1`, `input-2`, ...
  /// @return The answer, or an error that the function returns as it is: the
  /// answer is still to come, the client cannot show a form, or what the
  /// person entered does not fit the form.
  template <typename Form>
  [[nodiscard]] Result<Answer<Form>> form(const std::string_view message,
                                          const std::string_view key = {}) const {
    Result<Answer<scrydantic::Json>> answer =
        form(message, scrydantic::Json{.text = detail::form_schema<Form>()}, key);
    if (!answer) {
      return std::unexpected(std::move(answer).error());
    }
    Answer<Form> result{.action = answer->action, .content = std::nullopt};
    if (answer->content.has_value()) {
      Result<Form> content = scrydantic::decode<Form>(
          *answer->content,
          scrydantic::DecodeOptions{.coerce_scalars = true, .coerce_structures = false});
      if (!content) {
        return std::unexpected(std::move(content).error());
      }
      result.content = std::move(*content);
    }
    return result;
  }

  /// Asks the person to fill in a form whose schema is given as it is, for a
  /// form that no type describes, such as one built while the server runs. The
  /// schema must be the flat object of primitive fields that elicitation
  /// allows; the answer's content is the JSON the client sent.
  [[nodiscard]] Result<Answer<scrydantic::Json>>
  form(std::string_view message, const scrydantic::Json& requested_schema,
       std::string_view key = {}) const;

private:
  const Context* context_{};
};

/// A request to the client's model.
struct [[ = scrydantic::rename_all{scrydantic::camel_case}, = scrydantic::skip_null ]]
SamplingRequest {
  /// The conversation so far.
  std::vector<PromptMessage> messages;
  /// The most tokens the model may write.
  std::int64_t max_tokens{256};
  std::optional<std::string> system_prompt{};
  std::optional<double> temperature{};
  std::optional<std::vector<std::string>> stop_sequences{};
};

/// What the client's model wrote.
struct SamplingResult {
  Role role{Role::assistant};
  /// The message, in blocks.
  Content content;
  /// The model that wrote it.
  std::string model;
  /// Why it stopped, such as `endTurn` or `maxTokens`, or empty.
  std::string stop_reason;
};

/// Asks the client's model to write a message.
class Sampling {
public:
  /// A Sampling that can ask no one.
  Sampling() = default;
  explicit Sampling(const Context& context) noexcept : context_{&context} {}

  /// Whether the client lets the server use its model.
  [[nodiscard]] explicit operator bool() const noexcept {
    return context_ != nullptr && detail::can_ask(*context_, detail::Need::sampling);
  }

  /// Asks the model for a message.
  /// @param request The conversation and the limits.
  /// @param key Names the question across rounds, or empty for the next of
  /// `input-1`, `input-2`, ...
  /// @return The message, or an error that the function returns as it is.
  [[nodiscard]] Result<SamplingResult> create(const SamplingRequest& request,
                                              std::string_view key = {}) const;

private:
  const Context* context_{};
};

/// A directory or file that the client lets the server work in.
struct [[ = scrydantic::skip_null, = scrydantic::ignore_unknown ]] Root {
  /// A `file://` URI.
  std::string uri;
  std::optional<std::string> name{};
};

/// Asks the client for its roots.
class Roots {
public:
  /// A Roots that can ask no one.
  Roots() = default;
  explicit Roots(const Context& context) noexcept : context_{&context} {}

  /// Whether the client has roots to give.
  [[nodiscard]] explicit operator bool() const noexcept {
    return context_ != nullptr && detail::can_ask(*context_, detail::Need::roots);
  }

  /// Asks for the roots.
  /// @param key Names the question across rounds, or empty for the next of
  /// `input-1`, `input-2`, ...
  /// @return The roots, or an error that the function returns as it is.
  [[nodiscard]] Result<std::vector<Root>> list(std::string_view key = {}) const;

private:
  const Context* context_{};
};

} // namespace mcp

/// Lets a function take an mcp::Elicitation parameter in any position.
template <> struct scrydantic::supplier<mcp::Elicitation, mcp::Context> {
  static mcp::Elicitation supply(const mcp::Context& context) noexcept {
    return mcp::Elicitation{context};
  }
};

/// Lets a function take an mcp::Sampling parameter in any position.
template <> struct scrydantic::supplier<mcp::Sampling, mcp::Context> {
  static mcp::Sampling supply(const mcp::Context& context) noexcept {
    return mcp::Sampling{context};
  }
};

/// Lets a function take an mcp::Roots parameter in any position.
template <> struct scrydantic::supplier<mcp::Roots, mcp::Context> {
  static mcp::Roots supply(const mcp::Context& context) noexcept { return mcp::Roots{context}; }
};
