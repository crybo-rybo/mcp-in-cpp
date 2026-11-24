// The server that the official MCP conformance suite tests: the tools,
// resources and prompts its scenarios call by name, each answering as the
// suite's reference server does. It serves Streamable HTTP at /mcp.
//
//   mcp_conformance_fixture 3000
//   npx @modelcontextprotocol/conformance server --url http://localhost:3000/mcp

#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <httplib.h>
#include <iostream>
#include <mcp/httplib.hpp>
#include <mcp/mcp.hpp>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace conformance {

// A 1x1 PNG and a short WAV, as the reference server has them.
constexpr std::string_view png =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==";
constexpr std::string_view wav = "UklGRiYAAABXQVZFZm10IBAAAAABAAEAQB8AAAB9AAACABAAZGF0YQIAAAA=";

mcp::ImageContent image() {
  return {.data = std::string{png}, .mime_type = "image/png"};
}

[[= mcp::tool{"Tests simple text content response"}]] std::string test_simple_text() {
  return "This is a simple text response for testing.";
}

[[= mcp::tool{"Tests image content response"}]] mcp::Content test_image_content() {
  return {image()};
}

[[= mcp::tool{"Tests audio content response"}]] mcp::Content test_audio_content() {
  return {mcp::AudioContent{.data = std::string{wav}, .mime_type = "audio/wav"}};
}

[[= mcp::tool{"Tests embedded resource content response"}]] mcp::Content
    test_embedded_resource() {
  return {mcp::EmbeddedResource{
      mcp::text_resource("test://embedded-resource", "This is an embedded resource content.")}};
}

[[= mcp::tool{"Tests response with multiple content types (text, image, resource)"}]]
mcp::Content test_multiple_content_types() {
  return {mcp::TextContent{"Multiple content types test:"}, image(),
          mcp::EmbeddedResource{mcp::text_resource("test://mixed-content-resource",
                                                   R"({"test":"data","value":123})",
                                                   "application/json")}};
}

[[= mcp::tool{"Tests tool that reports progress notifications"}]] std::string
    test_tool_with_progress(mcp::Progress progress) {
  for (const int step : {0, 50, 100}) {
    progress.report(step, 100.0, "Completed step " + std::to_string(step) + " of 100");
    if (step < 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
  }
  return "done";
}

[[= mcp::tool{"Tests tool that emits log messages during execution"}]] std::string
    test_tool_with_logging(mcp::Logger logger) {
  logger.info("Tool execution started");
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  logger.info("Tool processing data");
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  logger.info("Tool execution completed");
  return "Tool with logging executed successfully";
}

[[= mcp::tool{"Diagnostic logging validator tool"}]] std::string
    test_logging_tool(mcp::Logger logger) {
  logger.info("Diagnostic log message");
  return "Logging complete";
}

[[= mcp::tool{"Tests error response handling"}]] mcp::Status test_error_handling() {
  return std::unexpected(
      mcp::tool_error("This tool intentionally returns an error for testing"));
}

// The answer to a form, as the reference server writes it.
std::string answer_text(const std::string_view lead, const mcp::Answer<mcp::Json>& answer) {
  constexpr std::string_view actions[] = {"accept", "decline", "cancel"};
  return std::string{lead} + "action=" + std::string{actions[static_cast<int>(answer.action)]} +
         ", content=" + answer.content.value_or(mcp::Json{.text = "{}"}).text;
}

std::string first_text(const mcp::Content& content) {
  for (const mcp::ContentBlock& block : content) {
    if (const auto* text = std::get_if<mcp::TextContent>(&block)) {
      return text->text;
    }
  }
  return "no response";
}

mcp::SamplingRequest ask_model(std::string text, const std::int64_t max_tokens) {
  return {.messages = {{.role = mcp::Role::user, .content = mcp::TextContent{std::move(text)}}},
          .max_tokens = max_tokens};
}

// 2025-11-25: requests of the server's to the client.

[[= mcp::tool{"Tests server-initiated sampling (LLM completion request)"}]] std::string
    test_sampling([[= mcp::description{"The prompt to send to the LLM"}]] std::string prompt,
                  mcp::Sampling sampling) {
  mcp::Result<mcp::SamplingResult> answer = sampling.create(ask_model(std::move(prompt), 100));
  if (!answer) {
    return "Sampling not supported or error: " + answer.error().model_message;
  }
  return "LLM response: " + first_text(answer->content);
}

struct UserResponse {
  [[= mcp::description{"User's response"}]] std::string response;
};

[[= mcp::tool{"Tests server-initiated elicitation (user input request)"}]] std::string
    test_elicitation([[= mcp::description{"The message to show the user"}]] std::string message,
                     mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<mcp::Json>> answer =
      ask.form(message, mcp::Json{.text = std::string{scrydantic::schema_v<UserResponse>}});
  if (!answer) {
    return "Elicitation not supported or error: " + answer.error().model_message;
  }
  return answer_text("User response: ", *answer);
}

enum class UserStatus : std::uint8_t { active, inactive, pending };

struct Defaults {
  [[= mcp::description{"User name"}]] std::string name{"John Doe"};
  [[= mcp::description{"User age"}]] std::int32_t age{30};
  [[= mcp::description{"User score"}]] double score{95.5};
  [[= mcp::description{"User status"}]] UserStatus status{UserStatus::active};
  [[= mcp::description{"Verification status"}]] bool verified{true};
};

[[= mcp::tool{"Tests elicitation with default values per SEP-1034"}]] std::string
    test_elicitation_sep1034_defaults(mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<mcp::Json>> answer =
      ask.form("Please review and update the form fields with defaults",
               mcp::Json{.text = mcp::detail::form_schema<Defaults>()});
  if (!answer) {
    return "Elicitation not supported or error: " + answer.error().model_message;
  }
  return answer_text("Elicitation completed: ", *answer);
}

// Titled enumerations are spelled as JSON: Scrydantic writes an enumeration's
// names, not titles for them.
constexpr std::string_view enum_form = R"json({"properties":{)json"
    R"json("legacyEnum":{"description":"Select one option (legacy)","enum":["opt1","opt2","opt3"],)json"
    R"json("enumNames":["Option One","Option Two","Option Three"],"type":"string"},)json"
    R"json("titledMulti":{"description":"Select multiple options with titles","items":{"anyOf":[)json"
    R"json({"const":"value1","title":"First Choice"},{"const":"value2","title":"Second Choice"},)json"
    R"json({"const":"value3","title":"Third Choice"}]},"maxItems":3,"minItems":1,"type":"array"},)json"
    R"json("titledSingle":{"description":"Select one option with titles","oneOf":[)json"
    R"json({"const":"value1","title":"First Option"},{"const":"value2","title":"Second Option"},)json"
    R"json({"const":"value3","title":"Third Option"}],"type":"string"},)json"
    R"json("untitledMulti":{"description":"Select multiple options","items":{"enum":)json"
    R"json(["option1","option2","option3"],"type":"string"},"maxItems":3,"minItems":1,"type":"array"},)json"
    R"json("untitledSingle":{"description":"Select one option","enum":["option1","option2","option3"],)json"
    R"json("type":"string"}},"required":[],"type":"object"})json";

[[= mcp::tool{"Tests elicitation with enum schema improvements per SEP-1330"}]] std::string
    test_elicitation_sep1330_enums(mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<mcp::Json>> answer =
      ask.form("Please select options from the enum fields", mcp::Json{.text = std::string{enum_form}});
  if (!answer) {
    return "Elicitation not supported or error: " + answer.error().model_message;
  }
  return answer_text("Elicitation completed: ", *answer);
}

// 2026-07-28: rounds of input_required.

[[= mcp::tool{"Test tool requiring sampling"}]] mcp::Result<std::string>
    test_missing_capability(mcp::Sampling sampling) {
  mcp::Result<mcp::SamplingResult> answer = sampling.create(ask_model("Say hi", 10));
  if (!answer) {
    return std::unexpected(answer.error());
  }
  return std::string{"Success"};
}

struct Name {
  std::string name;
};

[[= mcp::tool{"MRTR: returns InputRequiredResult with elicitation request"}]]
mcp::Result<std::string> test_input_required_result_elicitation(mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<Name>> name = ask.form<Name>("What is your name?", "user_name");
  if (!name) {
    return std::unexpected(name.error());
  }
  return "Hello, " + (name->content ? name->content->name : std::string{"friend"}) + "!";
}

[[= mcp::tool{"MRTR: returns InputRequiredResult with sampling request"}]]
mcp::Result<std::string> test_input_required_result_sampling(mcp::Sampling sampling) {
  mcp::Result<mcp::SamplingResult> answer =
      sampling.create(ask_model("What is the capital of France?", 100), "sample_request");
  if (!answer) {
    return std::unexpected(answer.error());
  }
  return "Sampling result: " + first_text(answer->content);
}

[[= mcp::tool{"MRTR: returns InputRequiredResult with roots/list request"}]]
mcp::Result<std::string> test_input_required_result_list_roots(mcp::Roots roots) {
  mcp::Result<std::vector<mcp::Root>> listed = roots.list("roots_request");
  if (!listed) {
    return std::unexpected(listed.error());
  }
  return "Found " + std::to_string(listed->size()) + " root(s)";
}

struct Confirmation {
  bool ok{};
};

[[= mcp::tool{"MRTR: returns InputRequiredResult with requestState"}]]
mcp::Result<std::string> test_input_required_result_request_state(mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<Confirmation>> confirmed =
      ask.form<Confirmation>("Please confirm", "confirm");
  if (!confirmed) {
    return std::unexpected(confirmed.error());
  }
  return std::string{"state-ok: requestState validated"};
}

[[= mcp::tool{"MRTR: returns InputRequiredResult with multiple input requests"}]]
mcp::Result<std::string> test_input_required_result_multiple_inputs(mcp::Elicitation ask,
                                                                    mcp::Sampling sampling,
                                                                    mcp::Roots roots) {
  // Every question is asked before any answer is looked at, so that one round
  // asks them all.
  mcp::Result<mcp::Answer<Name>> name = ask.form<Name>("What is your name?", "user_name");
  mcp::Result<mcp::SamplingResult> greeting =
      sampling.create(ask_model("Generate a greeting", 50), "greeting");
  mcp::Result<std::vector<mcp::Root>> listed = roots.list("client_roots");
  if (!name) {
    return std::unexpected(name.error());
  }
  if (!greeting) {
    return std::unexpected(greeting.error());
  }
  if (!listed) {
    return std::unexpected(listed.error());
  }
  return "Name: " + (name->content ? name->content->name : std::string{"friend"}) +
         "; Greeting: " + first_text(greeting->content) +
         "; Roots: " + std::to_string(listed->size());
}

struct Color {
  std::string color;
};

[[= mcp::tool{"MRTR: multi-round InputRequiredResult workflow"}]]
mcp::Result<std::string> test_input_required_result_multi_round(mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<Name>> name = ask.form<Name>("Step 1: What is your name?", "step1");
  if (!name) {
    return std::unexpected(name.error());
  }
  mcp::Result<mcp::Answer<Color>> color =
      ask.form<Color>("Step 2: What is your favorite color?", "step2");
  if (!color) {
    return std::unexpected(color.error());
  }
  return "Multi-round complete for " +
         (name->content ? name->content->name : std::string{"friend"}) + " who likes " +
         (color->content ? color->content->color : std::string{"nothing"});
}

[[= mcp::tool{"MRTR: verifies the integrity of its requestState"}]]
mcp::Result<std::string> test_input_required_result_tampered_state(mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<Confirmation>> confirmed =
      ask.form<Confirmation>("Please confirm", "confirm");
  if (!confirmed) {
    return std::unexpected(confirmed.error());
  }
  return std::string{"integrity-ok: state verified"};
}

struct Value {
  std::string value;
};

[[= mcp::tool{"MRTR: requests only the inputs the client's capabilities allow"}]]
mcp::Result<std::string> test_input_required_result_capabilities(mcp::Elicitation ask,
                                                                 mcp::Sampling sampling) {
  std::vector<std::string> received;
  std::optional<mcp::Error> pending;
  if (ask) {
    mcp::Result<mcp::Answer<Value>> value = ask.form<Value>("Elicitation input", "elicit_input");
    if (value) {
      received.emplace_back("elicit_input");
    } else {
      pending = value.error();
    }
  }
  if (sampling) {
    mcp::Result<mcp::SamplingResult> sample =
        sampling.create(ask_model("Sample request", 50), "sample_input");
    if (sample) {
      received.emplace_back("sample_input");
    } else if (!pending) {
      pending = sample.error();
    }
  }
  if (!ask && !sampling) {
    return std::string{"No supported capabilities declared"};
  }
  if (pending) {
    return std::unexpected(*pending);
  }
  std::string keys;
  for (const std::string& key : received) {
    keys += (keys.empty() ? "" : ",") + key;
  }
  return "capabilities-ok: received " + keys;
}

[[= mcp::tool{"Diagnostic tool validating response progress streams"}]] std::string
    test_streaming_elicitation(mcp::Progress progress) {
  progress.report(50, 100.0);
  return "Streaming complete";
}

struct Context {
  std::string context;
};

[[= mcp::prompt{"MRTR: prompt that requires elicitation input"}]]
mcp::Result<std::string> test_input_required_result_prompt(mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<Context>> context =
      ask.form<Context>("What context should the prompt use?", "user_context");
  if (!context) {
    return std::unexpected(context.error());
  }
  return "Prompt with context: " +
         (context->content ? context->content->context : std::string{"none"});
}

[[= mcp::resource{.uri = "test://static-text",
                  .description = "A static text resource for testing",
                  .mime_type = "text/plain",
                  .name = "static-text",
                  .title = "Static Text Resource"}]]
std::string static_text() {
  return "This is the content of the static text resource.";
}

[[= mcp::resource{.uri = "test://static-binary",
                  .description = "A static binary resource (image) for testing",
                  .mime_type = "image/png",
                  .name = "static-binary",
                  .title = "Static Binary Resource"}]]
mcp::ResourceContents static_binary() {
  return {.uri = "test://static-binary",
          .mime_type = "image/png",
          .text = std::nullopt,
          .blob = std::string{png}};
}

struct TemplateData {
  std::string id;
  bool templateTest{};
  std::string data;
};

[[= mcp::resource{.uri = "test://template/{id}/data",
                  .description = "A resource template with parameter substitution",
                  .mime_type = "application/json",
                  .name = "template",
                  .title = "Resource Template"}]]
TemplateData template_data(std::string id) {
  return {.id = id, .templateTest = true, .data = "Data for ID: " + id};
}

[[= mcp::resource{.uri = "test://watched-resource",
                  .description = "A resource that auto-updates every 3 seconds",
                  .mime_type = "text/plain",
                  .name = "watched-resource",
                  .title = "Watched Resource"}]]
std::string watched_resource() {
  return "Watched resource content";
}

[[= mcp::prompt{.description = "A simple prompt without arguments",
                .title = "Simple Test Prompt"}]]
std::string test_simple_prompt() {
  return "This is a simple prompt for testing.";
}

[[= mcp::prompt{.description = "A prompt with required arguments",
                .title = "Prompt With Arguments"}]]
std::string test_prompt_with_arguments(
    [[= mcp::description{"First test argument"}]] std::string arg1,
    [[= mcp::description{"Second test argument"}]] std::string arg2) {
  return "Prompt with arguments: arg1='" + arg1 + "', arg2='" + arg2 + "'";
}

[[= mcp::prompt{.description = "A prompt that includes an embedded resource",
                .title = "Prompt With Embedded Resource"}]]
mcp::Content test_prompt_with_embedded_resource(
    [[= mcp::name{"resourceUri"}, = mcp::description{"URI of the resource to embed"}]]
    std::string resource_uri) {
  return {mcp::EmbeddedResource{
              mcp::text_resource(resource_uri, "Embedded resource content for testing.")},
          mcp::TextContent{"Please process the embedded resource above."}};
}

[[= mcp::prompt{.description = "A prompt that includes image content",
                .title = "Prompt With Image"}]]
mcp::Content test_prompt_with_image() {
  return {image(), mcp::TextContent{"Please analyze the image above."}};
}

} // namespace conformance

int main(const int argc, const char* const argv[]) {
  int port = 3000;
  if (argc > 1) {
    const std::string_view text{argv[1]};
    if (std::from_chars(text.data(), text.data() + text.size(), port).ec != std::errc{}) {
      std::cerr << "usage: mcp_conformance_fixture [PORT]\n";
      return 2;
    }
  }
  mcp::Server server{"mcp-in-cpp-conformance", "0.1.0"};
  server.add<^^conformance>();
  if (const mcp::Status status = server.status(); !status) {
    std::cerr << status.error().message << '\n';
    return 1;
  }
  mcp::HttpEndpoint endpoint{server};
  httplib::Server http;
  mcp::mount(http, "/mcp", endpoint);
  std::cerr << "serving on http://localhost:" << port << "/mcp\n";
  return http.listen("127.0.0.1", port) ? 0 : 1;
}
