// Log messages: which a call sends, by the level each era lets a client ask
// for.
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <mcp/mcp.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using support::error;
using support::legacy;
using support::modern;

namespace chatty {

[[= mcp::tool{"Say something at two levels"}]] inline std::string talk(mcp::Logger logger) {
  logger.info("thinking");
  logger.error("oh no");
  return "done";
}

} // namespace chatty

namespace {

mcp::Server chatty_server() {
  mcp::Server server{"sample", "0.0.1"};
  server.add<^^chatty>();
  return server;
}

std::string message(const std::string_view level, const std::string_view text) {
  std::string line{R"({"jsonrpc":"2.0","method":"notifications/message","params":{"data":")"};
  line += text;
  line += R"(","level":")";
  line += level;
  line += R"("}})";
  return line;
}

// The notifications one request sends.
std::vector<std::string> sent_by(mcp::Connection& connection, const std::string& request) {
  std::vector<std::string> sent;
  const mcp::Notify notify = [&sent](const std::string_view text) { sent.emplace_back(text); };
  REQUIRE(connection.handle(request, notify).has_value());
  return sent;
}

std::string call_with_level(const std::string_view level) {
  std::string text{R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"talk",)"
                   R"("_meta":{"io.modelcontextprotocol/clientCapabilities":{},)"
                   R"("io.modelcontextprotocol/logLevel":")"};
  text += level;
  text += R"(","io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})";
  return text;
}

} // namespace

TEST_CASE("under 2026-07-28 a call logs only when its request asks, and at that level") {
  const mcp::Server server = chatty_server();
  mcp::Connection connection{server};

  CHECK(sent_by(connection, modern("1", "tools/call", R"("name":"talk")")).empty());
  CHECK(sent_by(connection, call_with_level("debug")) ==
        std::vector<std::string>{message("info", "thinking"), message("error", "oh no")});
  CHECK(sent_by(connection, call_with_level("warning")) ==
        std::vector<std::string>{message("error", "oh no")});
  CHECK(connection.handle(call_with_level("loud")) ==
        error("1", -32602,
              R"(Invalid params: _meta[\"io.modelcontextprotocol/logLevel\"] must name a log )"
              R"(level, such as \"info\")"));
  // The revision has no logging/setLevel.
  CHECK(connection.handle(modern("2", "logging/setLevel", R"("level":"info")")) ==
        error("2", -32601, "Method not found: logging/setLevel"));
}

TEST_CASE("a session logs everything until the client sets a level") {
  const mcp::Server server = chatty_server();
  mcp::Connection connection{server};
  const std::optional<std::string> initialized =
      connection.handle(support::initialize("2025-11-25"));
  REQUIRE(initialized.has_value());
  CHECK(initialized->find(R"("logging":{})") != std::string::npos);

  const std::string call = legacy("1", "tools/call", R"({"name":"talk"})");
  CHECK(sent_by(connection, call).size() == 2);
  CHECK(connection.handle(legacy("2", "logging/setLevel", R"({"level":"error"})")) ==
        support::result("2", "{}"));
  CHECK(sent_by(connection, call) == std::vector<std::string>{message("error", "oh no")});
  CHECK(connection.handle(legacy("3", "logging/setLevel", R"({"level":"loud"})")) ==
        error("3", -32602, R"(Invalid params: level must name a log level, such as \"info\")"));
}

TEST_CASE("a server whose functions take no logger has no logging") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};
  const std::optional<std::string> initialized =
      connection.handle(support::initialize("2025-11-25"));
  REQUIRE(initialized.has_value());
  CHECK(initialized->find("logging") == std::string::npos);
  CHECK(connection.handle(legacy("1", "logging/setLevel", R"({"level":"info"})")) ==
        error("1", -32601, "Method not found: logging/setLevel"));
}
