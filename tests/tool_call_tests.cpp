// tools/call: what a tool's return value, its refusal, and its failure each
// become, and which mistakes in a request are the protocol's to report.
#include "support.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <span>
#include <mcp/mcp.hpp>
#include <optional>
#include <string>
#include <vector>

using support::called;
using support::error;
using support::modern;
using support::text_content;

TEST_CASE("a returned value is text for the model and structured content") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(
            modern("1", "tools/call", R"("name":"add","arguments":{"a":2,"b":3})")) ==
        called("1", text_content("5", false), "5"));
  CHECK(connection.handle(modern("2", "tools/call",
                                 R"("name":"mirror","arguments":{"x":1,"y":-2})")) ==
        called("2", text_content(R"({\"x\":-1,\"y\":2})", false), R"({"x":-1,"y":2})"));
  CHECK(log.empty());
}

TEST_CASE("a returned string is the text itself, not its JSON spelling") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(modern(
            "1", "tools/call", R"("name":"greet","arguments":{"who":"\"Ada\"\n"})")) ==
        called("1", text_content(R"(hello \"Ada\"\n)", false), R"("hello \"Ada\"\n")"));
}

TEST_CASE("a call may leave its arguments out") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  // `greet` then runs with its own default argument.
  CHECK(connection.handle(modern("1", "tools/call", R"("name":"greet")")) ==
        called("1", text_content("hello world", false), R"("hello world")"));
  CHECK(connection.handle(
            modern("2", "tools/call", R"("name":"greet","arguments":{})")) ==
        called("2", text_content("hello world", false), R"("hello world")"));
}

TEST_CASE("a tool that returns nothing has no content") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(modern("1", "tools/call", R"("name":"nothing")")) ==
        called("1", R"("content":[],"isError":false)"));
}

TEST_CASE("a call may carry members the server has no use for") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  // Answers to questions the tool never asks.
  CHECK(connection.handle(modern("1", "tools/call",
                                 R"("name":"add","arguments":{"a":2,"b":3},"task":{},)"
                                 R"("inputResponses":{"x":{"action":"accept"}})")) ==
        called("1", text_content("5", false), "5"));
}

TEST_CASE("a requestState that the server did not sign is invalid params") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(modern("1", "tools/call",
                                 R"("name":"add","arguments":{"a":2,"b":3},"requestState":"abc")")) ==
        R"({"error":{"code":-32602,"data":{"reason":"invalid_request_state"},)"
        R"("message":"Invalid or expired requestState"},"id":1,"jsonrpc":"2.0"})");
}

TEST_CASE("a malformed call is a protocol error") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(modern("1", "tools/call")) ==
        error("1", -32602, "Invalid params: name must be a string"));
  CHECK(connection.handle(modern("2", "tools/call", R"("name":7)")) ==
        error("2", -32602, "Invalid params: name must be a string"));
  CHECK(connection.handle(
            modern("3", "tools/call", R"("name":"add","arguments":[2,3])")) ==
        error("3", -32602, "Invalid params: arguments must be an object"));
  CHECK(connection.handle(modern("4", "tools/call", R"("name":"fly")")) ==
        error("4", -32602, "Unknown tool: fly"));
  CHECK(log.empty());
}

TEST_CASE("arguments the tool cannot take are a tool error the model can read") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(
            modern("1", "tools/call", R"("name":"add","arguments":{"a":"2","c":1})")) ==
        called("1", text_content(R"($ contains unknown member \"c\"\n)"
                                 R"($.a must be an integer\n)"
                                 "$.b is a required member",
                                 true)));
  CHECK(log.empty());
}

TEST_CASE("a tool's refusal is a tool error with the tool's words for the model") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  // The host's text, "the door is locked", is not sent.
  CHECK(connection.handle(modern("1", "tools/call", R"("name":"refuse")")) ==
        called("1", text_content("not today", true)));
  CHECK(log.empty());
}

TEST_CASE("a tool that throws fails the call and keeps its reason on the server") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(modern("1", "tools/call", R"("name":"explode")")) ==
        called("1", text_content("the tool failed", true)));
  CHECK(log == std::vector<std::string>{"tool explode threw: a secret detail"});

  // The connection goes on serving.
  CHECK(connection.handle(
            modern("2", "tools/call", R"("name":"add","arguments":{"a":2,"b":3})")) ==
        called("2", text_content("5", false), "5"));
}

TEST_CASE("a result that breaks its own output schema is not sent") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(modern("1", "tools/call", R"("name":"overflow")")) ==
        called("1", text_content("the tool failed", true)));
  CHECK(log == std::vector<std::string>{
                   "tool overflow failed: result $.value must be at most 100"});
}

TEST_CASE("progress reaches a client that asked for it") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  std::vector<std::string> sent;
  const mcp::Notify notify = [&sent](const std::string_view message) {
    sent.emplace_back(message);
  };

  SECTION("with the client's own token, and only while each report is greater") {
    CHECK(
        connection.handle(
            R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"count",)"
            R"("arguments":{"steps":2},"_meta":{"progressToken":"t-1",)"
            R"("io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
            R"("io.modelcontextprotocol/clientCapabilities":{}}}})",
            notify) == called("1", text_content("2", false), "2"));
    CHECK(sent == std::vector<std::string>{
                      R"({"jsonrpc":"2.0","method":"notifications/progress",)"
                      R"("params":{"progress":1,"progressToken":"t-1","total":2}})",
                      R"({"jsonrpc":"2.0","method":"notifications/progress",)"
                      R"("params":{"progress":2,"progressToken":"t-1","total":2}})"});
  }

  SECTION("with an integer token, in a legacy session too") {
    REQUIRE(connection.handle(support::initialize("2025-11-25")).has_value());
    CHECK(connection
              .handle(support::legacy("1", "tools/call",
                                      R"({"name":"count","arguments":{"steps":1},)"
                                      R"("_meta":{"progressToken":7}})"),
                      notify)
              .has_value());
    CHECK(sent == std::vector<std::string>{
                      R"({"jsonrpc":"2.0","method":"notifications/progress",)"
                      R"("params":{"progress":1,"progressToken":7,"total":1}})"});
  }

  SECTION("and no one else") {
    CHECK(connection.handle(
              modern("1", "tools/call", R"("name":"count","arguments":{"steps":2})"),
              notify) == called("1", text_content("2", false), "2"));
    CHECK(sent.empty());
  }
}

TEST_CASE("a parameter the server supplies is not in the tool's input schema") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);

  for (const auto& tool : server.tools()) {
    if (tool.name == "count") {
      CHECK(tool.input_schema.text ==
            std::string{R"({"additionalProperties":false,"properties":{"steps":)"} +
                std::string{support::int32_schema} +
                R"(},"required":["steps"],"type":"object"})");
    }
    if (tool.name == "revision") {
      CHECK(
          tool.input_schema.text ==
          R"({"additionalProperties":false,"properties":{},"required":[],"type":"object"})");
    }
  }
}

namespace media {

[[= mcp::tool{"Show a picture and say what it is"}]] inline mcp::Content picture() {
  const std::array<std::byte, 3> png{std::byte{0x89}, std::byte{'P'}, std::byte{'N'}};
  return {mcp::TextContent{"a picture:"}, mcp::image(png, "image/png"),
          mcp::EmbeddedResource{mcp::text_resource("test://note", "a note")},
          mcp::ResourceLink{.uri = "test://more", .name = "more"}};
}

[[= mcp::tool{"Play a sound, or refuse"}]] inline mcp::Result<mcp::Content>
    sound(const bool play) {
  if (!play) {
    return std::unexpected(mcp::tool_error("not now"));
  }
  return mcp::Content{mcp::audio(std::as_bytes(std::span{"hi", 2}), "audio/wav")};
}

[[= mcp::tool{"Add two integers"}]] inline std::int32_t add(std::int32_t a,
                                                            std::int32_t b) {
  return a + b;
}

struct Gallery {
  [[= mcp::tool{"Describe the gallery"}]] mcp::Content describe() const {
    return {mcp::TextContent{"empty"}};
  }
};

} // namespace media

TEST_CASE("a tool that returns content sends its blocks, with no output schema") {
  mcp::Server server{"sample", "0.0.1"};
  server.add<^^media>();
  mcp::Connection connection{server};

  const std::optional<std::string> listed = connection.handle(modern("1", "tools/list"));
  REQUIRE(listed.has_value());
  // `add` keeps its output schema; the content tools have none.
  CHECK(listed->find(R"("name":"picture","outputSchema")") == std::string::npos);
  CHECK(listed->find(R"("name":"sound","outputSchema")") == std::string::npos);
  CHECK(listed->find(R"("name":"add","outputSchema")") != std::string::npos);

  CHECK(connection.handle(modern("2", "tools/call", R"("name":"picture")")) ==
        called("2", R"("content":[{"text":"a picture:","type":"text"},)"
                    R"({"data":"iVBO","mimeType":"image/png","type":"image"},)"
                    R"({"resource":{"mimeType":"text/plain","text":"a note","uri":"test://note"},)"
                    R"("type":"resource"},{"name":"more","type":"resource_link","uri":"test://more"}],)"
                    R"("isError":false)"));
  CHECK(connection.handle(modern("3", "tools/call", R"("name":"sound","arguments":{"play":true})")) ==
        called("3", R"("content":[{"data":"aGk=","mimeType":"audio/wav","type":"audio"}],"isError":false)"));
  CHECK(connection.handle(modern("4", "tools/call", R"("name":"sound","arguments":{"play":false})")) ==
        called("4", text_content("not now", true)));
}

TEST_CASE("a toolbox's tool may return content too") {
  mcp::Server server{"sample", "0.0.1"};
  server.add(std::make_shared<const media::Gallery>());
  mcp::Connection connection{server};
  CHECK(connection.handle(modern("1", "tools/call", R"("name":"describe")")) ==
        called("1", text_content("empty", false)));
}

TEST_CASE("base64 spells every length of input") {
  CHECK(mcp::base64("") == "");
  CHECK(mcp::base64("f") == "Zg==");
  CHECK(mcp::base64("fo") == "Zm8=");
  CHECK(mcp::base64("foo") == "Zm9v");
  CHECK(mcp::base64("foobar") == "Zm9vYmFy");
}
