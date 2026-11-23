// The revisions with an `initialize` handshake, which a connection serves
// beside the modern one.
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <mcp/mcp.hpp>
#include <optional>
#include <string>
#include <vector>

using support::error;
using support::legacy;
using support::result;

namespace {

std::string initialized(const std::string_view version) {
  std::string text{R"({"capabilities":{"tools":{}},"protocolVersion":")"};
  text += version;
  text += R"(","serverInfo":{"name":"sample","version":"0.0.1"}})";
  return result("0", text);
}

} // namespace

TEST_CASE("initialize settles on the version the client asked for") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);

  for (const std::string_view version : {"2025-11-25", "2025-06-18"}) {
    INFO(version);
    mcp::Connection connection{server};
    CHECK(connection.handle(support::initialize(version)) == initialized(version));
    CHECK(connection.handle(legacy("1", "tools/call", R"({"name":"revision"})")) ==
          result("1", std::string{R"({"content":[{"text":")"} + std::string{version} +
                          R"(","type":"text"}],"isError":false})"));
  }
}

TEST_CASE("initialize offers its newest handshake version in place of one it lacks") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);

  for (const std::string_view version : {"2024-11-05", "2025-03-26", "2026-07-28"}) {
    INFO(version);
    mcp::Connection connection{server};
    CHECK(connection.handle(support::initialize(version)) == initialized("2025-11-25"));
  }
}

TEST_CASE("initialize gives the server's instructions") {
  mcp::Server server{"sample", "0.0.1"};
  server.instructions("Add things.");
  mcp::Connection connection{server};

  CHECK(connection.handle(support::initialize("2025-11-25")) ==
        result("0", R"({"capabilities":{"tools":{}},"instructions":"Add things.",)"
                    R"("protocolVersion":"2025-11-25",)"
                    R"("serverInfo":{"name":"sample","version":"0.0.1"}})"));
}

TEST_CASE("initialize needs a protocol version") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(legacy("1", "initialize", R"({"capabilities":{}})")) ==
        error("1", -32602, "Invalid params: protocolVersion must be a string"));
  CHECK(connection.handle(legacy("1", "initialize")) ==
        error("1", -32602, "Invalid params: protocolVersion must be a string"));
  // A failed handshake starts no session.
  CHECK(connection.handle(legacy("2", "tools/list"))
            .value()
            .starts_with(R"({"error":{"code":-32602)"));
}

TEST_CASE("ping is answered before and after the handshake") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(legacy("1", "ping")) == result("1", "{}"));
  REQUIRE(connection.handle(support::initialize("2025-11-25")).has_value());
  CHECK(connection.handle(legacy("2", "ping")) == result("2", "{}"));
}

TEST_CASE("a legacy tool list has no modern members") {
  mcp::Server server{"sample", "0.0.1"};
  server.add<^^one_tool>();
  mcp::Connection connection{server};
  REQUIRE(connection.handle(support::initialize("2025-11-25")).has_value());

  // An output schema had to describe an object before 2026-07-28, so the
  // integer that `add` returns has none here.
  std::string tool{R"({"description":"Add two integers","inputSchema":)"};
  tool += R"({"additionalProperties":false,"properties":{"a":)";
  tool += support::int32_schema;
  tool += R"(,"b":)";
  tool += support::int32_schema;
  tool += R"(},"required":["a","b"],"type":"object"},"name":"add"})";

  CHECK(connection.handle(legacy("1", "tools/list")) ==
        result("1", R"({"tools":[)" + tool + "]}"));
  CHECK(connection.handle(legacy("2", "tools/list", "{}")) ==
        result("2", R"({"tools":[)" + tool + "]}"));
}

TEST_CASE("a legacy call sends structured content only for an object") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};
  REQUIRE(connection.handle(support::initialize("2025-06-18")).has_value());

  CHECK(connection.handle(
            legacy("1", "tools/call", R"({"name":"add","arguments":{"a":2,"b":3}})")) ==
        result("1", R"({"content":[{"text":"5","type":"text"}],"isError":false})"));
  CHECK(connection.handle(legacy("2", "tools/call",
                                 R"({"name":"mirror","arguments":{"x":1,"y":-2}})")) ==
        result("2", R"({"content":[{"text":"{\"x\":-1,\"y\":2}","type":"text"}],)"
                    R"("isError":false,"structuredContent":{"x":-1,"y":2}})"));
}

TEST_CASE("a legacy session has only the methods of a tool server") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};
  REQUIRE(connection.handle(support::initialize("2025-11-25")).has_value());

  CHECK(connection.handle(legacy("1", "resources/list")) ==
        error("1", -32601, "Method not found: resources/list"));
  CHECK(connection.handle(legacy("2", "server/discover")) ==
        error("2", -32601, "Method not found: server/discover"));
}
