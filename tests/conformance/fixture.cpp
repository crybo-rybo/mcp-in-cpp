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
#include <string>
#include <string_view>
#include <thread>
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
