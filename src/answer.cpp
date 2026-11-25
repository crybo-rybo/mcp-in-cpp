#include "answer.hpp"

#include "jsonrpc.hpp"
#include "verbatim.hpp"
#include "wire.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <mcp/ask.hpp>
#include <mcp/binding.hpp>
#include <mcp/channel.hpp>
#include <mcp/client.hpp>
#include <mcp/content.hpp>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mcp::answer {

namespace {

using jsonrpc::join;
using scrydantic::Json;
using scrydantic::JsonKind;
using scrydantic::JsonView;

[[nodiscard]] bool is_object(const std::optional<JsonView>& value) noexcept {
  return value.has_value() && value->kind() == JsonKind::object;
}

[[nodiscard]] bool is_number(const JsonKind kind) noexcept {
  return kind == JsonKind::signed_integer || kind == JsonKind::unsigned_integer ||
         kind == JsonKind::number;
}

// A question that is not valid: its params are not what its method takes.
[[nodiscard]] std::unexpected<Refusal> invalid(const std::string_view method,
                                               const std::string_view why) {
  return std::unexpected(
      Refusal{.code = wire::invalid_params,
              .message = join({"Invalid params: ", why}),
              .fault = Fault::protocol,
              .reason = join({"the server's ", method, " is not valid: ", why})});
}

// An answer of a handler's that cannot go on the wire.
[[nodiscard]] std::unexpected<Refusal> unsendable(const std::string_view handler,
                                                  const std::string_view why) {
  return std::unexpected(Refusal{.code = wire::internal_error,
                                 .message = "Internal error",
                                 .fault = Fault::protocol,
                                 .reason = join({"the answer of the ", handler,
                                                 " handler cannot be sent: ", why})});
}

// Whether a value may be a field of a form's content: a string, a number, a
// boolean, or the list of strings of a multi-select enumeration.
[[nodiscard]] bool is_field(const JsonView& value) {
  const JsonKind kind = value.kind();
  if (kind == JsonKind::string || kind == JsonKind::boolean || is_number(kind)) {
    return true;
  }
  if (kind != JsonKind::array) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value.at(index)->kind() != JsonKind::string) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<std::int64_t> integer_of(const JsonView& value) {
  if (const std::optional<std::uint64_t> positive = value.unsigned_integer()) {
    if (*positive <=
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return static_cast<std::int64_t>(*positive);
    }
    return std::nullopt;
  }
  return value.signed_integer();
}

// Reads one message of a sampling request into `messages`: one for each block
// of its content.
[[nodiscard]] Answered<void> read_message(const JsonView& message,
                                          const std::size_t index,
                                          std::vector<PromptMessage>& messages) {
  constexpr std::string_view method = "sampling/createMessage";
  const std::string which = join({"message ", std::to_string(index)});
  const std::optional<JsonView> role =
      message.kind() == JsonKind::object ? message.find("role") : std::nullopt;
  const std::optional<std::string_view> spelled =
      role.has_value() ? role->string() : std::nullopt;
  if (spelled != "user" && spelled != "assistant") {
    return invalid(method, join({which, " needs a role of user or assistant"}));
  }
  const Role speaker = spelled == "user" ? Role::user : Role::assistant;
  const std::optional<JsonView> content = message.find("content");
  if (!content.has_value() ||
      (content->kind() != JsonKind::object && content->kind() != JsonKind::array)) {
    return invalid(method, join({which, " needs content"}));
  }
  const std::size_t blocks = content->kind() == JsonKind::array ? content->size() : 1;
  for (std::size_t block = 0; block < blocks; ++block) {
    const JsonView part =
        content->kind() == JsonKind::array ? *content->at(block) : *content;
    scrydantic::Result<ContentBlock> decoded = scrydantic::decode<ContentBlock>(part);
    if (!decoded) {
      return invalid(method, join({which, " has content this client cannot read: ",
                                   decoded.error().message}));
    }
    messages.push_back(PromptMessage{.role = speaker, .content = std::move(*decoded)});
  }
  return {};
}

} // namespace

Refusal not_declared(const std::string_view method) {
  return {.code = wire::method_not_found,
          .message = join({"Method not found: ", method}),
          .fault = Fault::protocol,
          .reason = join(
              {"the server asks for ", method, ", which this client did not declare"})};
}

Refusal threw(const std::string_view handler, const std::string_view what) {
  // The server is not told what the client's own code said.
  return {.code = wire::internal_error,
          .message = "Internal error",
          .fault = Fault::unanswered,
          .reason = join({"the ", handler, " handler threw: ", what})};
}

Refusal refused(const std::string_view reason) {
  // The error's message is the handler's own words, when they can be sent.
  const bool sendable = !reason.empty() && scrydantic::is_valid_json(
                                               scrydantic::escape_json_string(reason));
  return {.code = -1,
          .message = sendable ? std::string{reason}
                              : std::string{"User rejected sampling request"},
          .fault = Fault::unanswered,
          .reason = join({"the sampling handler refused: ", reason})};
}

Answered<FormRequest> read_form(const std::string_view params) {
  constexpr std::string_view method = "elicitation/create";
  const scrydantic::Result<JsonView> parsed = JsonView::parse(params);
  if (!parsed || parsed->kind() != JsonKind::object) {
    return invalid(method, "params must be an object");
  }
  // A request without a mode is a form, as 2025-06-18 writes every one.
  if (const std::optional<JsonView> mode = parsed->find("mode");
      mode.has_value() && mode->string() != "form") {
    return invalid(method, mode->string() == "url"
                               ? "this client declared elicitation in form mode only"
                               : "the mode is not form or url");
  }
  const std::optional<JsonView> message = parsed->find("message");
  const std::optional<std::string_view> text =
      message.has_value() ? message->string() : std::nullopt;
  if (!text.has_value() || !is_object(parsed->find("requestedSchema"))) {
    return invalid(method, "a form needs a message and a requestedSchema object");
  }
  return FormRequest{.message = std::string{*text},
                     .schema = Json{.text = std::string{
                                        *verbatim::member(params, "requestedSchema")}}};
}

std::string with_defaults(std::string content, const std::string_view schema) {
  const scrydantic::Result<JsonView> form = JsonView::parse(schema);
  const scrydantic::Result<JsonView> given = JsonView::parse(content);
  const std::optional<JsonView> properties =
      form ? form->find("properties") : std::nullopt;
  const std::optional<std::string_view> written =
      form ? verbatim::member(schema, "properties") : std::nullopt;
  if (!given || !is_object(properties) || !written.has_value()) {
    return content;
  }
  std::string added;
  for (std::size_t index = 0; index < properties->size(); ++index) {
    const std::string_view key = properties->key_at(index).value_or("");
    const std::optional<JsonView> property = properties->find(key);
    if (given->find(key).has_value() || !is_object(property) ||
        !property->find("default").has_value()) {
      continue;
    }
    // The default goes as the server wrote it.
    const std::optional<std::string_view> raw = verbatim::member(*written, key);
    const std::optional<std::string_view> value =
        raw.has_value() ? verbatim::member(*raw, "default") : std::nullopt;
    if (!value.has_value()) {
      continue;
    }
    added += ',';
    added += scrydantic::escape_json_string(key);
    added += ':';
    added += verbatim::compact(*value);
  }
  if (added.empty()) {
    return content;
  }
  // `content` is an object on one line, so it ends with its closing brace.
  content.pop_back();
  if (content == "{") {
    added.erase(0, 1);
  }
  content += added;
  content += '}';
  return content;
}

Answered<std::string> write_form(const Answer<Json>& answer,
                                 const std::string_view schema) {
  constexpr std::string_view handler = "elicitation";
  wire::ElicitResult result{.action = answer.action, .content = std::nullopt};
  if (answer.action == Action::accept) {
    const std::string text =
        verbatim::compact(answer.content.has_value() ? answer.content->text : "{}");
    const scrydantic::Result<JsonView> content = JsonView::parse(text);
    if (!content || content->kind() != JsonKind::object) {
      return unsendable(handler, "accepted content must be a JSON object");
    }
    for (std::size_t index = 0; index < content->size(); ++index) {
      const std::string_view key = content->key_at(index).value_or("");
      if (!is_field(*content->find(key))) {
        return unsendable(handler, join({"field ", key,
                                         " is not a string, a number, a boolean, "
                                         "or a list of strings"}));
      }
    }
    result.content = Json{.text = with_defaults(text, schema)};
  }
  scrydantic::Result<Json> encoded = scrydantic::encode_verbatim(result);
  if (!encoded) {
    return unsendable(handler, encoded.error().message);
  }
  return std::move(encoded->text);
}

Answered<SamplingRequest> read_sampling(const std::string_view params) {
  constexpr std::string_view method = "sampling/createMessage";
  const scrydantic::Result<JsonView> parsed = JsonView::parse(params);
  if (!parsed || parsed->kind() != JsonKind::object) {
    return invalid(method, "params must be an object");
  }
  if (parsed->find("tools").has_value() || parsed->find("toolChoice").has_value()) {
    return invalid(method, "this client did not declare sampling with tools");
  }
  const std::optional<JsonView> messages = parsed->find("messages");
  const std::optional<JsonView> tokens = parsed->find("maxTokens");
  const std::optional<std::int64_t> most =
      tokens.has_value() ? integer_of(*tokens) : std::nullopt;
  if (!messages.has_value() || messages->kind() != JsonKind::array ||
      !most.has_value()) {
    return invalid(method, "a request needs messages and an integer maxTokens");
  }
  SamplingRequest request{.messages = {},
                          .max_tokens = *most,
                          .system_prompt = std::nullopt,
                          .temperature = std::nullopt,
                          .stop_sequences = std::nullopt};
  for (std::size_t index = 0; index < messages->size(); ++index) {
    if (Answered<void> read =
            read_message(*messages->at(index), index, request.messages);
        !read) {
      return std::unexpected(std::move(read).error());
    }
  }
  if (const std::optional<JsonView> prompt = parsed->find("systemPrompt")) {
    if (!prompt->string().has_value()) {
      return invalid(method, "systemPrompt must be a string");
    }
    request.system_prompt = std::string{*prompt->string()};
  }
  if (const std::optional<JsonView> temperature = parsed->find("temperature")) {
    if (!is_number(temperature->kind())) {
      return invalid(method, "temperature must be a number");
    }
    request.temperature = temperature->number();
  }
  if (const std::optional<JsonView> stops = parsed->find("stopSequences")) {
    std::vector<std::string> sequences;
    for (std::size_t index = 0; index < stops->size(); ++index) {
      const std::optional<std::string_view> stop = stops->at(index)->string();
      if (!stop.has_value()) {
        break;
      }
      sequences.emplace_back(*stop);
    }
    if (stops->kind() != JsonKind::array || sequences.size() != stops->size()) {
      return invalid(method, "stopSequences must be a list of strings");
    }
    request.stop_sequences = std::move(sequences);
  }
  return request;
}

Answered<std::string> write_sampling(const SamplingResult& result, const bool several) {
  constexpr std::string_view handler = "sampling";
  if (result.content.empty()) {
    return unsendable(handler, "the message has no content");
  }
  for (const ContentBlock& block : result.content) {
    if (!std::holds_alternative<TextContent>(block) &&
        !std::holds_alternative<ImageContent>(block) &&
        !std::holds_alternative<AudioContent>(block)) {
      return unsendable(handler,
                        "a sampled message holds text, images and sounds only");
    }
  }
  if (result.content.size() > 1 && !several) {
    return unsendable(handler, "2025-06-18 takes a message of one block");
  }
  scrydantic::Result<Json> content = result.content.size() == 1
                                         ? scrydantic::encode(result.content.front())
                                         : scrydantic::encode(result.content);
  if (!content) {
    return unsendable(handler, content.error().message);
  }
  scrydantic::Result<Json> encoded =
      scrydantic::encode_verbatim(wire::CreateMessageResult{
          .content = std::move(*content),
          .model = result.model,
          .role = result.role,
          .stop_reason = result.stop_reason.empty()
                             ? std::nullopt
                             : std::optional{result.stop_reason}});
  if (!encoded) {
    return unsendable(handler, encoded.error().message);
  }
  return std::move(encoded->text);
}

Answered<std::string> write_roots(const std::vector<Root>& roots) {
  for (const Root& root : roots) {
    if (!root.uri.starts_with("file://")) {
      return unsendable("roots", join({"root ", root.uri, " is not a file:// URI"}));
    }
  }
  scrydantic::Result<Json> encoded = scrydantic::encode(wire::ListRootsResult{roots});
  if (!encoded) {
    return unsendable("roots", encoded.error().message);
  }
  return std::move(encoded->text);
}

} // namespace mcp::answer
