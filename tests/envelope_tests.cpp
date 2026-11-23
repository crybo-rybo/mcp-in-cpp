// What a connection makes of a message before it knows the method: JSON-RPC's
// own rules, as MCP 2026-07-28 narrows them.
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <mcp/mcp.hpp>
#include <optional>
#include <string>
#include <vector>

using support::error;
using support::modern;

TEST_CASE("text that is not JSON is a parse error with no id") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(
      connection.handle(R"({"jsonrpc" "2.0"})") ==
      error("", -32700,
            "Parse error: JSON text is not valid: expected ':' at line 1, column 12"));
}

TEST_CASE("a message that is not an object is an invalid request") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  // Batches left the protocol in 2025-06-18.
  CHECK(connection.handle(R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])") ==
        error("", -32600, "Invalid Request: batches are not supported"));
  CHECK(connection.handle("7") ==
        error("", -32600, "Invalid Request: a message must be an object"));
}

TEST_CASE("an id is a string or an integer") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  for (const std::string_view id : {"null", "1.5", "true", "[1]", "{}"}) {
    INFO(id);
    CHECK(connection.handle(support::legacy(id, "ping")) ==
          error("", -32600, "Invalid Request: id must be a string or an integer"));
  }
}

TEST_CASE("a response repeats the request's id exactly") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  for (const std::string_view id :
       {"0", "-7", "18446744073709551615", R"("abc")", R"("a\"b\\c")", R"("")"}) {
    INFO(id);
    CHECK(connection.handle(support::legacy(id, "ping")) == support::result(id, "{}"));
  }
}

TEST_CASE("jsonrpc must be 2.0") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(R"({"id":1,"method":"ping"})") ==
        error("1", -32600, R"(Invalid Request: jsonrpc must be \"2.0\")"));
  CHECK(connection.handle(R"({"jsonrpc":"1.0","id":1,"method":"ping"})") ==
        error("1", -32600, R"(Invalid Request: jsonrpc must be \"2.0\")"));
  CHECK(connection.handle(R"({"jsonrpc":2.0,"id":1,"method":"ping"})") ==
        error("1", -32600, R"(Invalid Request: jsonrpc must be \"2.0\")"));
}

TEST_CASE("a message needs a method, unless it is a response") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(R"({"jsonrpc":"2.0","id":1})") ==
        error("1", -32600, "Invalid Request: a message needs a method"));
  CHECK(connection.handle(R"({"jsonrpc":"2.0","id":1,"method":7})") ==
        error("1", -32600, "Invalid Request: method must be a string"));
  // The server sends no requests, so a response answers nothing and gets none.
  CHECK(connection.handle(R"({"jsonrpc":"2.0","id":1,"result":{}})") == std::nullopt);
  CHECK(connection.handle(
            R"({"jsonrpc":"2.0","id":1,"error":{"code":-1,"message":"no"}})") ==
        std::nullopt);
}

TEST_CASE("a notification gets no response") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(
      connection.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})") ==
      std::nullopt);
  CHECK(
      connection.handle(
          R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":1}})") ==
      std::nullopt);
  // Not even one the server does not know, or one that is malformed.
  CHECK(connection.handle(R"({"jsonrpc":"2.0","method":"notifications/made_up"})") ==
        std::nullopt);
  CHECK(connection.handle(R"({"jsonrpc":"2.0","method":"tools/list","params":[1]})") ==
        std::nullopt);
}

TEST_CASE("params are an object") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(support::legacy("1", "tools/list", "[1,2]")) ==
        error("1", -32602, "Invalid params: params must be an object"));
}

TEST_CASE("a server with two tools of one name answers with an internal error") {
  std::vector<std::string> log;
  mcp::Server server = support::sample_server(log);
  server.add<^^one_tool>(); // A second `add`.

  REQUIRE_FALSE(server.status().has_value());
  CHECK(server.status().error().message == R"(two tools are named "add")");

  mcp::Connection connection{server};
  CHECK(connection.handle(modern("1", "tools/list")) ==
        error("1", -32603, "Internal error"));
}
