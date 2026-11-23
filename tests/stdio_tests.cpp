// The stdio transport: one message per line, each way.
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <mcp/mcp.hpp>
#include <sstream>
#include <string>
#include <vector>

using support::called;
using support::modern;
using support::text_content;

TEST_CASE("serve_stdio answers each line with one line and ends with the input") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);

  // A blank line and a carriage return before the line feed are tolerated,
  // and a notification gets no line back.
  std::istringstream in{
      modern("1", "tools/call", R"("name":"add","arguments":{"a":2,"b":3})") + "\n\n" +
      R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":1}})" +
      "\n" + support::legacy("2", "ping") + "\r\n" + "nonsense\n" +
      modern("3", "tools/call", R"("name":"greet")")};
  std::ostringstream out;

  CHECK(mcp::serve_stdio(server, in, out) == 0);
  CHECK(out.str() ==
        called("1", text_content("5", false), "5") + "\n" + support::result("2", "{}") +
            "\n" +
            support::error("", -32700,
                           "Parse error: JSON text is not valid: expected a value at "
                           "line 1, column 1") +
            "\n" + called("3", text_content("hello world", false), R"("hello world")") +
            "\n");
}

TEST_CASE("serve_stdio writes a call's progress before its response") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);

  std::istringstream in{
      R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"count",)"
      R"("arguments":{"steps":1},"_meta":{"progressToken":"t",)"
      R"("io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
      R"("io.modelcontextprotocol/clientCapabilities":{}}}})"
      "\n"};
  std::ostringstream out;

  CHECK(mcp::serve_stdio(server, in, out) == 0);
  CHECK(out.str() ==
        std::string{R"({"jsonrpc":"2.0","method":"notifications/progress",)"
                    R"("params":{"progress":1,"progressToken":"t","total":1}})"} +
            "\n" + called("1", text_content("1", false), "1") + "\n");
}

TEST_CASE("serve_stdio writes no line that holds a line break") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);

  std::istringstream in{
      modern("1", "tools/call", R"("name":"greet","arguments":{"who":"a\nb\r\nc"})") +
      "\n"};
  std::ostringstream out;

  CHECK(mcp::serve_stdio(server, in, out) == 0);
  const std::string written = out.str();
  REQUIRE_FALSE(written.empty());
  CHECK(written.find('\n') == written.size() - 1);
  CHECK(written.find('\r') == std::string::npos);
}

TEST_CASE("serve_stdio does not serve a server that cannot be served") {
  std::vector<std::string> log;
  mcp::Server server = support::sample_server(log);
  server.add<^^one_tool>(); // A second `add`.

  std::istringstream in{modern("1", "tools/list") + "\n"};
  std::ostringstream out;

  CHECK(mcp::serve_stdio(server, in, out) == 1);
  CHECK(out.str().empty());
  CHECK(log == std::vector<std::string>{R"(two tools are named "add")"});
}
