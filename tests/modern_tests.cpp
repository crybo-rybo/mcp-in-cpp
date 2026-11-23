// The 2026-07-28 revision: every request names its protocol version, and
// nothing is remembered between two requests.
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <mcp/mcp.hpp>
#include <optional>
#include <string>
#include <vector>

using support::error;
using support::modern;
using support::result;

namespace {

std::string with_server_info(const std::string_view members) {
  std::string text{"{"};
  text += support::server_info;
  text += ",";
  text += members;
  text += "}";
  return text;
}

} // namespace

TEST_CASE("server/discover names the versions, the capabilities, and the server") {
  mcp::Server server{"sample", "0.0.1"};
  server.add<^^one_tool>();
  mcp::Connection connection{server};

  CHECK(connection.handle(modern(R"("d")", "server/discover")) ==
        result(R"("d")",
               with_server_info(
                   R"("cacheScope":"public","capabilities":{"tools":{}},)"
                   R"("resultType":"complete",)"
                   R"("supportedVersions":["2026-07-28","2025-11-25","2025-06-18"],)"
                   R"("ttlMs":0)")));

  server.instructions("Add things.").list_ttl(std::chrono::minutes{5});
  CHECK(
      connection.handle(modern("2", "server/discover")) ==
      result("2", with_server_info(
                      R"("cacheScope":"public","capabilities":{"tools":{}},)"
                      R"("instructions":"Add things.","resultType":"complete",)"
                      R"("supportedVersions":["2026-07-28","2025-11-25","2025-06-18"],)"
                      R"("ttlMs":300000)")));
}

TEST_CASE("tools/list gives each tool's schemas and the caching hints") {
  mcp::Server server{"sample", "0.0.1"};
  server.add<^^one_tool>();
  mcp::Connection connection{server};

  std::string tool{R"({"description":"Add two integers","inputSchema":)"};
  tool += R"({"additionalProperties":false,"properties":{"a":)";
  tool += support::int32_schema;
  tool += R"(,"b":)";
  tool += support::int32_schema;
  tool += R"(},"required":["a","b"],"type":"object"},"name":"add","outputSchema":)";
  tool += support::int32_schema;
  tool += "}";

  CHECK(connection.handle(modern("1", "tools/list")) ==
        result("1", with_server_info(R"("cacheScope":"public","resultType":"complete",)"
                                     R"("tools":[)" +
                                     tool + R"(],"ttlMs":0)")));
}

TEST_CASE("tools are listed in the order they were added") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);

  std::vector<std::string> names;
  for (const auto& tool : server.tools()) {
    names.push_back(tool.name);
  }
  CHECK(names == std::vector<std::string>{"add", "greet", "mirror", "nothing", "refuse",
                                          "explode", "overflow", "count", "revision"});
}

TEST_CASE("the whole tool list is one page, so no cursor is known") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(modern("1", "tools/list", R"("cursor":"abc")")) ==
        error("1", -32602, "Invalid params: unknown cursor"));
}

TEST_CASE("a request must name its protocol version and its client's capabilities") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(support::legacy("1", "tools/list")) ==
        error("1", -32602,
              "Invalid params: the request names no protocol version. Send "
              R"(_meta[\"io.modelcontextprotocol/protocolVersion\"] as 2026-07-28 on )"
              "each request, or send initialize first for 2025-11-25"));
  CHECK(connection.handle(support::legacy(
            "1", "tools/list",
            R"({"_meta":{"io.modelcontextprotocol/protocolVersion":20260728}})")) ==
        error("1", -32602,
              R"(Invalid params: _meta[\"io.modelcontextprotocol/protocolVersion\"] )"
              "must be a string"));
  CHECK(
      connection.handle(support::legacy(
          "1", "tools/list",
          R"({"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}})")) ==
      error("1", -32602,
            R"(Invalid params: _meta[\"io.modelcontextprotocol/clientCapabilities\"] )"
            "is required and must be an object"));
}

TEST_CASE("a version the server lacks is answered with the ones it has") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(
            R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{)"
            R"("io.modelcontextprotocol/protocolVersion":"2030-01-01",)"
            R"("io.modelcontextprotocol/clientCapabilities":{}}}})") ==
        R"({"error":{"code":-32022,"data":{"requested":"2030-01-01",)"
        R"("supported":["2026-07-28","2025-11-25","2025-06-18"]},)"
        R"("message":"Unsupported protocol version"},"id":1,"jsonrpc":"2.0"})");
}

TEST_CASE("a handshake version cannot be named per request") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(support::legacy(
            "1", "tools/list",
            R"({"_meta":{"io.modelcontextprotocol/protocolVersion":"2025-11-25",)"
            R"("io.modelcontextprotocol/clientCapabilities":{}}})")) ==
        error("1", -32602,
              "Invalid params: protocol version 2025-11-25 is agreed with an "
              "initialize request, not per request"));
}

TEST_CASE("the modern revision has no ping and no method the server lacks") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(modern("1", "ping")) ==
        error("1", -32601, "Method not found: ping"));
  CHECK(connection.handle(modern("1", "resources/list")) ==
        error("1", -32601, "Method not found: resources/list"));
}

TEST_CASE("a subscription is acknowledged and stays open") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  std::vector<std::string> sent;
  const mcp::Notify notify = [&sent](const std::string_view message) {
    sent.emplace_back(message);
  };
  // The tools of a server do not change while it is served, so the server
  // agrees to none of the notification types the client asked for.
  CHECK(connection.handle(modern(R"("listen-1")", "subscriptions/listen",
                                 R"("notifications":{"toolsListChanged":true})"),
                          notify) == std::nullopt);
  CHECK(
      sent ==
      std::vector<std::string>{
          R"({"jsonrpc":"2.0","method":"notifications/subscriptions/acknowledged",)"
          R"("params":{"_meta":{"io.modelcontextprotocol/subscriptionId":"listen-1"},)"
          R"("notifications":{}}})"});

  CHECK(connection.handle(modern("2", "subscriptions/listen"), notify) ==
        error("2", -32602, "Invalid params: notifications must be an object"));
}

TEST_CASE("a modern request is served as one on a connection that has a session") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};
  REQUIRE(connection.handle(support::initialize("2025-11-25")).has_value());

  CHECK(connection.handle(modern("1", "tools/call", R"("name":"revision")")) ==
        support::called("1", support::text_content("2026-07-28", false),
                        R"("2026-07-28")"));
}
