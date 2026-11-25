// The client's Streamable HTTP transport, against a scripted HTTP client: the
// test plays the server, and sees each request exactly as it was sent.
#include "fake_http.hpp"
#include "http_channel.hpp"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <future>
#include <map>
#include <mcp/client.hpp>
#include <mcp/http_client.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

using fake::Exchange;
using fake::Request;

namespace {

const std::vector<std::string> modern_base{
    "Content-Type: application/json", "Accept: application/json, text/event-stream",
    "MCP-Protocol-Version: 2026-07-28"};

std::vector<std::string> with(std::vector<std::string> lines,
                              const std::vector<std::string>& more) {
  lines.insert(lines.end(), more.begin(), more.end());
  return lines;
}

std::string tool_call(const std::string_view id, const std::string_view members) {
  return fake::request(id, "tools/call",
                       "{" + std::string{fake::modern_meta} + "," +
                           std::string{members} + "}");
}

std::string text_of(const mcp::Reply<mcp::ToolResult>& result) {
  REQUIRE(result.has_value());
  REQUIRE(result->content.size() == 1);
  const auto* const text = std::get_if<mcp::TextContent>(&result->content.front());
  REQUIRE(text != nullptr);
  return text->text;
}

// What a channel tells its client, kept for a test that drives the channel
// on its own.
class Heard final : public mcp::Incoming {
public:
  void message(const std::string_view text) override {
    note("message " + std::string{text});
  }
  void failed(const std::string_view id, mcp::ClientError why) override {
    note("failed " + std::string{id} + " " + why.message);
  }
  void closed(mcp::ClientError why) override { note("closed " + why.message); }

  [[nodiscard]] std::vector<std::string> said() const {
    const std::scoped_lock lock{mutex_};
    return said_;
  }

  // Waits until the channel has said `count` things, and returns them.
  [[nodiscard]] std::vector<std::string> await(const std::size_t count) const {
    std::unique_lock lock{mutex_};
    changed_.wait_for(lock, fake::patience, [&] { return said_.size() >= count; });
    return said_;
  }

private:
  void note(std::string line) {
    {
      const std::scoped_lock lock{mutex_};
      said_.push_back(std::move(line));
    }
    changed_.notify_all();
  }

  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::vector<std::string> said_;
};

mcp::Outgoing outgoing(const std::string_view message, const std::string_view id,
                       const std::string_view method, const std::string_view version) {
  return {.message = message,
          .id = id,
          .method = method,
          .name = {},
          .protocol_version = version,
          .headers = {}};
}

} // namespace

TEST_CASE("a modern client posts each request with the headers that route it") {
  fake::Http http;
  http.answer(fake::modern_server({{"tools/call", std::string{fake::tool_done}}}));
  mcp::Reply<mcp::Client> client =
      mcp::connect(fake::url, http, fake::options(mcp::Era::modern));
  REQUIRE(client.has_value());
  CHECK(client->server().era == mcp::Era::modern);
  CHECK(client->server().name == "fake");

  CHECK(text_of(client->call_tool("forecast", mcp::Json{R"({"city":"Oslo"})"})) ==
        "done");
  const std::vector<Request> sent = http.requests();
  REQUIRE(sent.size() == 2);
  CHECK(sent[0].method == "POST");
  CHECK(sent[0].url == fake::url);
  CHECK(sent[0].lines() == with(modern_base, {"Mcp-Method: server/discover"}));
  CHECK(sent[0].body == fake::request("1", "server/discover",
                                      "{" + std::string{fake::modern_meta} + "}"));
  CHECK(sent[1].lines() ==
        with(modern_base, {"Mcp-Method: tools/call", "Mcp-Name: forecast"}));
  CHECK(sent[1].body ==
        tool_call("2", R"("arguments":{"city":"Oslo"},"name":"forecast")"));
}

TEST_CASE("a name that a header cannot hold is sent in Base64") {
  fake::Http http;
  http.answer(fake::modern_server({{"tools/call", std::string{fake::tool_done}}}));
  mcp::Reply<mcp::Client> client =
      mcp::connect(fake::url, http, fake::options(mcp::Era::modern));
  REQUIRE(client.has_value());
  REQUIRE(client->call_tool("météo").has_value());
  CHECK(http.requests().back().header("Mcp-Name") == "=?base64?bcOpdMOpbw==?=");
}

TEST_CASE("a handshake client keeps its session and ends it when it closes") {
  fake::Http http;
  http.answer(fake::legacy_server({{"tools/list", R"({"tools":[]})"}}));
  {
    mcp::Reply<mcp::Client> client =
        mcp::connect(fake::url, http, fake::options(mcp::Era::legacy));
    REQUIRE(client.has_value());
    CHECK(client->server().era == mcp::Era::legacy);
    REQUIRE(client->list_tools().has_value());
  }
  const std::vector<Request> sent = http.requests();
  REQUIRE(sent.size() == 4);
  const std::vector<std::string> base{"Content-Type: application/json",
                                      "Accept: application/json, text/event-stream"};
  // initialize names no revision and no session.
  CHECK(sent[0].lines() == base);
  CHECK(sent[0].body ==
        fake::request(
            "1", "initialize",
            R"({"capabilities":{},"clientInfo":{"name":"zoo","version":"0.3.0"},)"
            R"("protocolVersion":"2025-11-25"})"));
  const std::vector<std::string> session =
      with(base, {"MCP-Protocol-Version: 2025-11-25", "Mcp-Session-Id: s1"});
  CHECK(sent[1].lines() == session);
  CHECK(sent[1].body == R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  CHECK(sent[2].lines() == session);
  CHECK(sent[2].body == fake::request("2", "tools/list", "{}"));
  CHECK(sent[3].method == "DELETE");
  CHECK(sent[3].url == fake::url);
  CHECK(sent[3].lines() ==
        std::vector<std::string>{"Mcp-Session-Id: s1",
                                 "MCP-Protocol-Version: 2025-11-25"});
  CHECK(sent[3].body.empty());
}

TEST_CASE("a server that does not end sessions does not hold up closing") {
  fake::Http http;
  const fake::Respond legacy = fake::legacy_server();
  http.answer([&legacy](const Request& request, Exchange& exchange) {
    if (request.method == "DELETE") {
      exchange.start(405, {{.name = "Allow", .value = "POST"}});
      return mcp::Reply<void>{};
    }
    return legacy(request, exchange);
  });
  {
    mcp::Reply<mcp::Client> client =
        mcp::connect(fake::url, http, fake::options(mcp::Era::legacy));
    REQUIRE(client.has_value());
  }
  CHECK(http.methods() ==
        std::vector<std::string>{"initialize", "notifications/initialized", "DELETE"});
}

TEST_CASE("a modern client keeps no session, even one a server names") {
  fake::Http http;
  http.answer([](const Request& request, Exchange& exchange) {
    const std::string result = request.rpc() == "server/discover"
                                   ? std::string{fake::discovered}
                                   : std::string{R"({"tools":[]})"};
    exchange.json(support::result(request.id(), result), 200,
                  {{.name = "Mcp-Session-Id", .value = "nope"}});
    return mcp::Reply<void>{};
  });
  {
    mcp::Reply<mcp::Client> client =
        mcp::connect(fake::url, http, fake::options(mcp::Era::modern));
    REQUIRE(client.has_value());
    REQUIRE(client->list_tools().has_value());
  }
  const std::vector<Request> sent = http.requests();
  REQUIRE(sent.size() == 2);
  CHECK_FALSE(sent[1].header("Mcp-Session-Id").has_value());
}

TEST_CASE("an event stream's notifications and response reach the caller") {
  fake::Http http;
  const fake::Respond modern = fake::modern_server();
  http.answer([&modern](const Request& request, Exchange& exchange) {
    if (request.rpc() != "tools/call") {
      return modern(request, exchange);
    }
    exchange.events({{.name = "Cache-Control", .value = "no-cache"}});
    const std::string progress =
        R"({"jsonrpc":"2.0","method":"notifications/progress","params":)"
        R"({"progressToken":2,"progress":1,"total":2,"message":"half"}})";
    // A comment, then a notification split inside its data and inside a CRLF.
    exchange.write(": keepalive\r\n\r\n");
    exchange.write("event: message\r\ndata: " + progress.substr(0, 20));
    exchange.write(progress.substr(20) + "\r");
    exchange.write("\n\r\n");
    exchange.event(support::result(request.id(), fake::tool_done));
    return mcp::Reply<void>{};
  });
  mcp::Reply<mcp::Client> client =
      mcp::connect(fake::url, http, fake::options(mcp::Era::modern));
  REQUIRE(client.has_value());
  std::vector<mcp::ProgressReport> reports;
  const mcp::Reply<mcp::ToolResult> result =
      client->call_tool("slow", mcp::Json{"{}"},
                        {.progress = [&reports](const mcp::ProgressReport& report) {
                          reports.push_back(report);
                        }});
  CHECK(text_of(result) == "done");
  REQUIRE(reports.size() == 1);
  CHECK(reports[0].progress == 1.0);
  CHECK(reports[0].total == 2.0);
  CHECK(reports[0].message == "half");
}

TEST_CASE("a stream that ends without the response fails the call") {
  fake::Http http;
  const fake::Respond modern = fake::modern_server();
  http.answer([&modern](const Request& request, Exchange& exchange) {
    if (request.rpc() != "tools/call") {
      return modern(request, exchange);
    }
    exchange.events();
    // A priming event of the kind a handshake server sends, then the end.
    exchange.write("id: 1\nretry: 500\ndata: \n\n");
    return mcp::Reply<void>{};
  });
  mcp::Reply<mcp::Client> client =
      mcp::connect(fake::url, http, fake::options(mcp::Era::modern));
  REQUIRE(client.has_value());
  const mcp::Reply<mcp::ToolResult> result = client->call_tool("slow");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().fault == mcp::Fault::transport);
  CHECK(result.error().message == "the server ended the stream before it answered");
}

TEST_CASE("each HTTP status fails a call as the transport has it") {
  struct Case {
    std::string name;
    fake::Respond respond;
    mcp::Fault fault;
    int code;
    std::string message;
  };
  const auto status = [](const int code, std::string body, std::string type,
                         std::vector<mcp::HttpHeader> headers = {}) {
    return [=](const Request&, Exchange& exchange) {
      std::vector<mcp::HttpHeader> sent = headers;
      if (!type.empty()) {
        sent.push_back({.name = "Content-Type", .value = type});
      }
      exchange.start(code, sent);
      exchange.write(body);
      return mcp::Reply<void>{};
    };
  };
  const std::vector<Case> cases{
      {"400 with text", status(400, "Bad Request: no", "text/plain"),
       mcp::Fault::protocol, 0, "HTTP 400: Bad Request: no"},
      {"400 with the request's error",
       status(400, support::error("2", -32602, "Invalid params"), "application/json"),
       mcp::Fault::rejected, -32602, "Invalid params"},
      {"404 with the request's error",
       status(404, support::error("2", -32601, "Method not found"), "application/json"),
       mcp::Fault::rejected, -32601, "Method not found"},
      {"400 with an error of no request",
       status(400, support::error("", -32700, "Parse error"), "application/json"),
       mcp::Fault::protocol, 0, "HTTP 400: Parse error"},
      {"405", status(405, "", ""), mcp::Fault::protocol, 0, "HTTP 405"},
      {"500", status(500, "boom", "text/plain"), mcp::Fault::transport, 0,
       "HTTP 500: boom"},
      {"503 with the request's error",
       status(503, support::error("2", -32603, "Overloaded"), "application/json"),
       mcp::Fault::transport, 0, "HTTP 503: Overloaded"},
      {"202 for a request", status(202, "", ""), mcp::Fault::protocol, 0,
       "the server's HTTP 202 response does not hold the request's answer"},
      {"200 with another's answer",
       status(200, support::result("9", fake::tool_done), "application/json"),
       mcp::Fault::protocol, 0,
       "the server's HTTP 200 response does not hold the request's answer"},
      {"401",
       status(401, "", "",
              {{.name = "WWW-Authenticate", .value = R"(Bearer realm="mcp")"}}),
       mcp::Fault::unauthorized, 0, "the server requires authorization (HTTP 401)"},
      {"a broken connection",
       [](const Request&, Exchange&) -> mcp::Reply<void> {
         return std::unexpected(mcp::ClientError{.fault = mcp::Fault::transport,
                                                 .code = 0,
                                                 .message = "connection refused",
                                                 .data = std::nullopt});
       },
       mcp::Fault::transport, 0, "connection refused"},
  };
  for (const Case& each : cases) {
    INFO(each.name);
    fake::Http http;
    http.answer(fake::modern_server());
    mcp::Reply<mcp::Client> client =
        mcp::connect(fake::url, http, fake::options(mcp::Era::modern));
    REQUIRE(client.has_value());
    http.answer(each.respond);
    const mcp::Reply<mcp::ToolResult> result = client->call_tool("add");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().fault == each.fault);
    CHECK(result.error().code == each.code);
    CHECK(result.error().message == each.message);
    if (each.fault == mcp::Fault::unauthorized) {
      REQUIRE(result.error().data.has_value());
      CHECK(result.error().data->text == R"("Bearer realm=\"mcp\"")");
    }
  }
}

TEST_CASE("a modern answer to discovery keeps the client modern") {
  for (const mcp::Era era : {mcp::Era::automatic, mcp::Era::modern}) {
    fake::Http http;
    http.answer(fake::modern_server());
    mcp::Reply<mcp::Client> client = mcp::connect(fake::url, http, fake::options(era));
    REQUIRE(client.has_value());
    CHECK(client->server().era == mcp::Era::modern);
    CHECK(http.methods() == std::vector<std::string>{"server/discover"});
  }
}

TEST_CASE("a 400 that names the versions a server supports is asked again") {
  fake::Http http;
  int refused = 0;
  const fake::Respond modern = fake::modern_server();
  http.answer([&](const Request& request, Exchange& exchange) {
    if (request.rpc() == "server/discover" && refused++ == 0) {
      exchange.json(
          std::string{R"({"error":{"code":-32022,"data":{"requested":"2026-07-28",)"} +
              R"("supported":["2026-07-28"]},"message":"Unsupported protocol version"},)"
              R"("id":)" +
              request.id() + R"(,"jsonrpc":"2.0"})",
          400);
      return mcp::Reply<void>{};
    }
    return modern(request, exchange);
  });
  mcp::Reply<mcp::Client> client = mcp::connect(fake::url, http, fake::options());
  REQUIRE(client.has_value());
  CHECK(client->server().era == mcp::Era::modern);
  const std::vector<Request> sent = http.requests();
  REQUIRE(sent.size() == 2);
  CHECK(sent[1].id() == "2");
  CHECK(sent[1].header("MCP-Protocol-Version") == "2026-07-28");
}

TEST_CASE("a 400 with another modern error fails the opening, and never falls back") {
  for (const mcp::Era era : {mcp::Era::automatic, mcp::Era::modern}) {
    fake::Http http;
    http.answer([](const Request& request, Exchange& exchange) {
      exchange.json(support::error(request.id(), -32020, "Header mismatch: Mcp-Method"),
                    400);
      return mcp::Reply<void>{};
    });
    const mcp::Reply<mcp::Client> client =
        mcp::connect(fake::url, http, fake::options(era));
    REQUIRE_FALSE(client.has_value());
    CHECK(client.error().fault == mcp::Fault::rejected);
    CHECK(client.error().code == -32020);
    CHECK(http.methods() == std::vector<std::string>{"server/discover"});
  }
}

TEST_CASE("any other 4xx to discovery falls back to the handshake") {
  struct Case {
    int status;
    std::string type;
    std::string body;
  };
  const std::vector<Case> cases{
      {400, "text/plain", "Bad Request: No valid session ID provided"},
      {400, "application/json",
       R"({"jsonrpc":"2.0","id":"server-error","error":{"code":-32000,"message":"No session"}})"},
      {400, "application/json", support::error("", -32600, "Invalid Request")},
      {400, "application/json", support::error("1", -32600, "Invalid Request")},
      {404, "text/html", "<html>Not Found</html>"},
      {404, "application/json", support::error("1", -32601, "Method not found")},
      {405, "", ""},
      {406, "text/plain", "Not Acceptable"},
      {415, "", ""},
  };
  for (const Case& each : cases) {
    INFO(each.status << " " << each.body);
    fake::Http http;
    const fake::Respond legacy = fake::legacy_server();
    http.answer([&](const Request& request, Exchange& exchange) {
      if (request.rpc() == "server/discover") {
        std::vector<mcp::HttpHeader> headers;
        if (!each.type.empty()) {
          headers.push_back({.name = "Content-Type", .value = each.type});
        }
        exchange.start(each.status, headers);
        exchange.write(each.body);
        return mcp::Reply<void>{};
      }
      return legacy(request, exchange);
    });
    mcp::Reply<mcp::Client> client = mcp::connect(fake::url, http, fake::options());
    REQUIRE(client.has_value());
    CHECK(client->server().era == mcp::Era::legacy);
    CHECK(client->server().protocol_version == "2025-11-25");
    REQUIRE(http.await_rpc("notifications/initialized").has_value());
    const std::vector<std::string> methods = http.methods();
    REQUIRE(methods.size() >= 3);
    CHECK(methods[0] == "server/discover");
    CHECK(methods[1] == "initialize");
    CHECK(methods[2] == "notifications/initialized");
  }
}

TEST_CASE("a 401, a 5xx, or no server fails the opening without a handshake") {
  struct Case {
    fake::Respond respond;
    mcp::Fault fault;
  };
  const std::vector<Case> cases{
      {[](const Request&, Exchange& exchange) {
         exchange.start(401, {{.name = "WWW-Authenticate",
                               .value = R"(Bearer resource_metadata="https://a/b")"}});
         return mcp::Reply<void>{};
       },
       mcp::Fault::unauthorized},
      {[](const Request&, Exchange& exchange) {
         exchange.start(502);
         return mcp::Reply<void>{};
       },
       mcp::Fault::transport},
      {[](const Request&, Exchange&) -> mcp::Reply<void> {
         return std::unexpected(mcp::ClientError{.fault = mcp::Fault::transport,
                                                 .code = 0,
                                                 .message = "connection refused",
                                                 .data = std::nullopt});
       },
       mcp::Fault::transport},
  };
  for (const Case& each : cases) {
    fake::Http http;
    http.answer(each.respond);
    const mcp::Reply<mcp::Client> client =
        mcp::connect(fake::url, http, fake::options());
    REQUIRE_FALSE(client.has_value());
    CHECK(client.error().fault == each.fault);
    CHECK(http.methods() == std::vector<std::string>{"server/discover"});
    if (each.fault == mcp::Fault::unauthorized) {
      REQUIRE(client.error().data.has_value());
      CHECK(client.error().data->text ==
            R"("Bearer resource_metadata=\"https://a/b\"")");
    }
  }
}

TEST_CASE("a session the server no longer knows ends the client") {
  fake::Http http;
  http.answer(fake::legacy_server());
  mcp::Reply<mcp::Client> client =
      mcp::connect(fake::url, http, fake::options(mcp::Era::legacy));
  REQUIRE(client.has_value());
  // The handshake is over once the server has its notification.
  REQUIRE(http.await_rpc("notifications/initialized").has_value());
  http.answer([](const Request& request, Exchange& exchange) {
    exchange.json(support::error(request.id(), -32600, "Session not found"), 404);
    return mcp::Reply<void>{};
  });
  const mcp::Reply<std::vector<mcp::Tool>> listed = client->list_tools();
  REQUIRE_FALSE(listed.has_value());
  CHECK(listed.error().fault == mcp::Fault::transport);
  CHECK(listed.error().message == "the server no longer knows the session (HTTP 404)");
  // Every later call fails at once, unsent.
  const std::size_t before = http.requests().size();
  CHECK(client->ping().error().fault == mcp::Fault::transport);
  CHECK(http.requests().size() == before);
  // Nor is there a session to DELETE.
  client = std::unexpected(mcp::ClientError{});
  CHECK(http.methods() == std::vector<std::string>{
                              "initialize", "notifications/initialized", "tools/list"});
}

TEST_CASE("cancelling a modern call closes its stream and posts nothing") {
  for (const bool timeout : {false, true}) {
    fake::Http http;
    const fake::Respond modern = fake::modern_server();
    std::promise<bool> closed;
    http.answer([&](const Request& request, Exchange& exchange) {
      if (request.rpc() != "tools/call") {
        return modern(request, exchange);
      }
      // A tool that runs without a word until the client closes the stream.
      closed.set_value(exchange.closed());
      return mcp::Reply<void>{};
    });
    mcp::Reply<mcp::Client> client =
        mcp::connect(fake::url, http, fake::options(mcp::Era::modern));
    REQUIRE(client.has_value());
    std::stop_source stop;
    mcp::CallOptions call{.progress = {}, .stop = stop.get_token(), .timeout = {}};
    if (timeout) {
      call.timeout = 100ms;
    }
    mcp::Client& open = *client;
    auto calling = std::async(std::launch::async, [&] {
      return open.call_tool("slow", {.text = "{}"}, call);
    });
    REQUIRE(http.await_rpc("tools/call").has_value());
    if (!timeout) {
      stop.request_stop();
    }
    REQUIRE(calling.wait_for(fake::patience) == std::future_status::ready);
    const mcp::Reply<mcp::ToolResult> result = calling.get();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().fault ==
          (timeout ? mcp::Fault::timeout : mcp::Fault::cancelled));
    std::future<bool> seen = closed.get_future();
    REQUIRE(seen.wait_for(fake::patience) == std::future_status::ready);
    CHECK(seen.get());
    CHECK(http.methods() == std::vector<std::string>{"server/discover", "tools/call"});
  }
}

TEST_CASE("cancelling a handshake call posts notifications/cancelled") {
  fake::Http http;
  const fake::Respond legacy = fake::legacy_server();
  std::promise<bool> closed;
  http.answer([&](const Request& request, Exchange& exchange) {
    if (request.rpc() != "tools/call") {
      return legacy(request, exchange);
    }
    exchange.events();
    closed.set_value(exchange.closed());
    return mcp::Reply<void>{};
  });
  mcp::Reply<mcp::Client> client =
      mcp::connect(fake::url, http, fake::options(mcp::Era::legacy));
  REQUIRE(client.has_value());
  std::stop_source stop;
  mcp::Client& open = *client;
  auto calling = std::async(std::launch::async, [&] {
    return open.call_tool("slow", {.text = "{}"},
                          {.progress = {}, .stop = stop.get_token(), .timeout = {}});
  });
  REQUIRE(http.await_rpc("tools/call").has_value());
  stop.request_stop();
  REQUIRE(calling.wait_for(fake::patience) == std::future_status::ready);
  CHECK(calling.get().error().fault == mcp::Fault::cancelled);
  const std::optional<Request> cancel = http.await_rpc("notifications/cancelled");
  REQUIRE(cancel.has_value());
  CHECK(cancel->body ==
        R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"reason":)"
        R"("the caller cancelled the request","requestId":2}})");
  CHECK(cancel->header("Mcp-Session-Id") == "s1");
  CHECK(cancel->header("MCP-Protocol-Version") == "2025-11-25");
  // The stream is closed too: the client reads no more of it.
  std::future<bool> seen = closed.get_future();
  REQUIRE(seen.wait_for(fake::patience) == std::future_status::ready);
  CHECK(seen.get());
}

TEST_CASE("a handshake server's request on a stream is answered with a POST") {
  fake::Http http;
  const fake::Respond legacy = fake::legacy_server();
  http.answer([&](const Request& request, Exchange& exchange) {
    if (request.rpc() != "tools/call") {
      return legacy(request, exchange);
    }
    exchange.events();
    exchange.event(
        R"({"jsonrpc":"2.0","id":"e1","method":"elicitation/create","params":)"
        R"({"message":"Name?","requestedSchema":{"type":"object","properties":)"
        R"({"name":{"type":"string"}}}}})");
    const std::optional<Request> answer =
        http.await([](const Request& sent) { return sent.id() == R"("e1")"; });
    exchange.event(support::result(request.id(), answer.has_value()
                                                     ? std::string{fake::tool_done}
                                                     : R"({"content":[]})"));
    return mcp::Reply<void>{};
  });
  mcp::ClientOptions options = fake::options(mcp::Era::legacy);
  options.elicitation = [](const mcp::FormRequest&) {
    return mcp::Answer<mcp::Json>{.action = mcp::Action::accept,
                                  .content = mcp::Json{R"({"name":"Ada"})"}};
  };
  mcp::Reply<mcp::Client> client = mcp::connect(fake::url, http, options);
  REQUIRE(client.has_value());
  const mcp::Reply<mcp::ToolResult> booked = client->call_tool("book");
  INFO((booked ? std::string{} : booked.error().message));
  CHECK(text_of(booked) == "done");
  const std::optional<Request> answer =
      http.await([](const Request& sent) { return sent.id() == R"("e1")"; });
  REQUIRE(answer.has_value());
  CHECK(
      answer->body ==
      R"({"id":"e1","jsonrpc":"2.0","result":{"action":"accept","content":{"name":"Ada"}}})");
  CHECK(answer->header("Mcp-Session-Id") == "s1");
}

TEST_CASE("the program's headers and bearer token go with every request") {
  fake::Http http;
  http.answer(fake::modern_server({{"tools/list", R"({"tools":[]})"}}));
  std::mutex mutex;
  int tokens = 0;
  mcp::Reply<mcp::Client> client =
      mcp::connect(fake::url, http, fake::options(mcp::Era::modern),
                   {.headers = {{.name = "X-Team", .value = "zoo"},
                                // The transport's own headers are left out.
                                {.name = "content-type", .value = "text/plain"},
                                {.name = "Mcp-Method", .value = "spoof"},
                                {.name = "Authorization", .value = "Basic abc"}},
                    .bearer = [&] {
                      const std::scoped_lock lock{mutex};
                      return "t" + std::to_string(++tokens);
                    }});
  REQUIRE(client.has_value());
  REQUIRE(client->list_tools().has_value());
  const std::vector<Request> sent = http.requests();
  REQUIRE(sent.size() == 2);
  CHECK(sent[0].lines() ==
        with(modern_base, {"Mcp-Method: server/discover", "X-Team: zoo",
                           "Authorization: Bearer t1"}));
  CHECK(sent[1].lines() == with(modern_base, {"Mcp-Method: tools/list", "X-Team: zoo",
                                              "Authorization: Bearer t2"}));
}

TEST_CASE("a token or a header that cannot be sent is refused") {
  fake::Http http;
  http.answer(fake::modern_server());
  const mcp::Reply<mcp::Client> broken = mcp::connect(
      fake::url, http, fake::options(mcp::Era::modern),
      {.headers = {}, .bearer = [] { return std::string{"a\r\nX-Evil: 1"}; }});
  REQUIRE_FALSE(broken.has_value());
  CHECK(broken.error().fault == mcp::Fault::protocol);
  CHECK(http.requests().empty());

  const mcp::Reply<mcp::Client> header =
      mcp::connect(fake::url, http, fake::options(),
                   {.headers = {{.name = "X-A", .value = "1\n2"}}});
  REQUIRE_FALSE(header.has_value());
  CHECK(header.error().fault == mcp::Fault::protocol);
  const mcp::Reply<mcp::Client> name = mcp::connect(
      fake::url, http, fake::options(), {.headers = {{.name = "X A", .value = "1"}}});
  REQUIRE_FALSE(name.has_value());
  for (const std::string_view url :
       {"localhost:8080/mcp", "ftp://host/mcp", "http://", "http:///mcp"}) {
    INFO(url);
    const mcp::Reply<mcp::Client> wrong = mcp::connect(url, http, fake::options());
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().fault == mcp::Fault::transport);
  }
  CHECK(http.requests().empty());
}

namespace {

const std::string marked_tools =
    R"({"tools":[{"name":"sql","inputSchema":{"type":"object","properties":{)"
    R"("region":{"type":"string","x-mcp-header":"Region"},)"
    R"("priority":{"type":"integer","x-mcp-header":"Priority"},)"
    R"("verbose":{"type":"boolean","x-mcp-header":"Verbose"},)"
    R"("query":{"type":"string"}}}},)"
    R"({"name":"broken","inputSchema":{"type":"object","properties":{)"
    R"("score":{"type":"number","x-mcp-header":"Score"}}}},)"
    R"({"name":"plain","inputSchema":{"type":"object"}}]})";

std::vector<std::string> names_of(const std::vector<mcp::Tool>& tools) {
  std::vector<std::string> names;
  for (const mcp::Tool& tool : tools) {
    names.push_back(tool.name);
  }
  return names;
}

} // namespace

TEST_CASE("a call mirrors the arguments its tool marks into headers") {
  fake::Http http;
  http.answer(fake::modern_server(
      {{"tools/list", marked_tools}, {"tools/call", std::string{fake::tool_done}}}));
  mcp::Reply<mcp::Client> client =
      mcp::connect(fake::url, http, fake::options(mcp::Era::modern));
  REQUIRE(client.has_value());

  // Before the tools are listed, nothing is known of their marks.
  REQUIRE(client->call_tool("sql", mcp::Json{R"({"region":"eu"})"}).has_value());
  CHECK_FALSE(http.requests().back().header("Mcp-Param-Region").has_value());

  // The tool whose marks break the rules is left out, and the others stay.
  const mcp::Reply<std::vector<mcp::Tool>> tools = client->list_tools();
  REQUIRE(tools.has_value());
  CHECK(names_of(*tools) == std::vector<std::string>{"sql", "plain"});

  REQUIRE(client
              ->call_tool("sql", mcp::Json{R"({"region":"Zürich","priority":42,)"
                                           R"("verbose":null,"query":"SELECT 1"})"})
              .has_value());
  CHECK(http.requests().back().lines() ==
        with(modern_base,
             {"Mcp-Method: tools/call", "Mcp-Name: sql", "Mcp-Param-Priority: 42",
              "Mcp-Param-Region: =?base64?WsO8cmljaA==?="}));

  // The tool that was left out is not called.
  const std::size_t before = http.requests().size();
  const mcp::Reply<mcp::ToolResult> broken =
      client->call_tool("broken", mcp::Json{R"({"score":1.5})"});
  REQUIRE_FALSE(broken.has_value());
  CHECK(broken.error().fault == mcp::Fault::protocol);
  CHECK(http.requests().size() == before);
}

TEST_CASE("a handshake client reads no marks") {
  fake::Http http;
  http.answer(fake::legacy_server(
      {{"tools/list", marked_tools}, {"tools/call", std::string{fake::tool_done}}}));
  mcp::Reply<mcp::Client> client =
      mcp::connect(fake::url, http, fake::options(mcp::Era::legacy));
  REQUIRE(client.has_value());
  const mcp::Reply<std::vector<mcp::Tool>> tools = client->list_tools();
  REQUIRE(tools.has_value());
  CHECK(names_of(*tools) == std::vector<std::string>{"sql", "broken", "plain"});
  REQUIRE(client->call_tool("sql", mcp::Json{R"({"region":"eu"})"}).has_value());
  CHECK_FALSE(http.requests().back().header("Mcp-Param-Region").has_value());
  CHECK_FALSE(http.requests().back().header("Mcp-Method").has_value());
}

TEST_CASE("a header mismatch lists the tools again and sends the call once more") {
  for (const bool fixed : {true, false}) {
    INFO(fixed);
    fake::Http http;
    std::mutex mutex;
    bool listed = false;
    const fake::Respond modern = fake::modern_server();
    http.answer([&](const Request& request, Exchange& exchange) {
      if (request.rpc() == "tools/list") {
        {
          const std::scoped_lock lock{mutex};
          listed = true;
        }
        exchange.json(support::result(request.id(), marked_tools));
        return mcp::Reply<void>{};
      }
      if (request.rpc() == "tools/call") {
        const bool good = fixed && request.header("Mcp-Param-Region") == "eu";
        if (good) {
          exchange.json(support::result(request.id(), fake::tool_done));
        } else {
          exchange.json(support::error(request.id(), -32020,
                                       "Header mismatch: Mcp-Param-Region is missing"),
                        400);
        }
        return mcp::Reply<void>{};
      }
      return modern(request, exchange);
    });
    mcp::Reply<mcp::Client> client =
        mcp::connect(fake::url, http, fake::options(mcp::Era::modern));
    REQUIRE(client.has_value());
    const mcp::Reply<mcp::ToolResult> result =
        client->call_tool("sql", mcp::Json{R"({"region":"eu"})"});
    CHECK(http.methods() == std::vector<std::string>{"server/discover", "tools/call",
                                                     "tools/list", "tools/call"});
    const std::vector<Request> sent = http.requests();
    CHECK(sent[3].id() == "4");
    CHECK(sent[3].header("Mcp-Param-Region") == "eu");
    if (fixed) {
      CHECK(text_of(result) == "done");
    } else {
      REQUIRE_FALSE(result.has_value());
      CHECK(result.error().fault == mcp::Fault::rejected);
      CHECK(result.error().code == -32020);
    }
  }
}

TEST_CASE("several calls run at once, each on its own POST") {
  constexpr int callers = 8;
  fake::Http http;
  const fake::Respond modern = fake::modern_server();
  std::mutex mutex;
  std::condition_variable all_here;
  int here = 0;
  http.answer([&](const Request& request, Exchange& exchange) {
    if (request.rpc() != "tools/call") {
      return modern(request, exchange);
    }
    {
      // Every call waits until all are in flight together.
      std::unique_lock lock{mutex};
      ++here;
      all_here.notify_all();
      all_here.wait_for(lock, fake::patience, [&] { return here >= callers; });
    }
    const std::string name =
        request.body.substr(request.body.rfind(R"("name":")") + 8, 2);
    exchange.json(support::result(
        request.id(), R"({"content":[{"type":"text","text":")" + name + R"("}]})"));
    return mcp::Reply<void>{};
  });
  mcp::Reply<mcp::Client> client =
      mcp::connect(fake::url, http, fake::options(mcp::Era::modern));
  REQUIRE(client.has_value());
  mcp::Client& open = *client;
  std::vector<std::future<mcp::Reply<mcp::ToolResult>>> calls;
  for (int caller = 0; caller < callers; ++caller) {
    calls.push_back(std::async(std::launch::async, [&open, caller] {
      return open.call_tool("t" + std::to_string(caller));
    }));
  }
  for (int caller = 0; caller < callers; ++caller) {
    REQUIRE(calls[static_cast<std::size_t>(caller)].wait_for(fake::patience) ==
            std::future_status::ready);
    CHECK(text_of(calls[static_cast<std::size_t>(caller)].get()) ==
          "t" + std::to_string(caller));
  }
}

TEST_CASE("closing the channel ends the requests in flight and waits for them") {
  fake::Http http;
  std::mutex mutex;
  int closed = 0;
  http.answer([&](const Request&, Exchange& exchange) {
    exchange.events();
    if (exchange.closed()) {
      const std::scoped_lock lock{mutex};
      ++closed;
    }
    return mcp::Reply<void>{};
  });
  Heard heard;
  const std::unique_ptr<mcp::Channel> channel =
      mcp::http_channel(std::string{fake::url}, http, {});
  REQUIRE(channel->open(heard).has_value());
  for (const std::string id : {"1", "2", "3"}) {
    const std::string message = fake::request(id, "tools/list", "{}");
    REQUIRE(
        channel->send(outgoing(message, id, "tools/list", "2026-07-28")).has_value());
  }
  REQUIRE(http.await_rpc("tools/list", 3).has_value());
  channel->close();
  {
    const std::scoped_lock lock{mutex};
    CHECK(closed == 3);
  }
  // Nothing was heard of the requests the channel gave up, and nothing more
  // is sent.
  CHECK(heard.said().empty());
  const std::string message = fake::request("4", "tools/list", "{}");
  CHECK_FALSE(
      channel->send(outgoing(message, "4", "tools/list", "2026-07-28")).has_value());
}

TEST_CASE("abandoning a request closes its stream in either era") {
  for (const std::string_view version : {"2026-07-28", "2025-11-25"}) {
    INFO(version);
    fake::Http http;
    std::promise<bool> closed;
    http.answer([&](const Request&, Exchange& exchange) {
      closed.set_value(exchange.closed());
      return mcp::Reply<void>{};
    });
    Heard heard;
    const std::unique_ptr<mcp::Channel> channel =
        mcp::http_channel(std::string{fake::url}, http, {});
    REQUIRE(channel->open(heard).has_value());
    const std::string message = fake::request("7", "tools/call", "{}");
    REQUIRE(channel->send(outgoing(message, "7", "tools/call", version)).has_value());
    REQUIRE(http.await_rpc("tools/call").has_value());
    // Only under 2026-07-28 does closing the stream tell the server.
    CHECK(channel->abandon("7") == (version == "2026-07-28"));
    std::future<bool> seen = closed.get_future();
    REQUIRE(seen.wait_for(fake::patience) == std::future_status::ready);
    CHECK(seen.get());
    channel->close();
    CHECK(heard.said().empty());
  }
}

TEST_CASE("a notification reaches the server before the messages sent after it") {
  fake::Http http;
  std::mutex mutex;
  std::condition_variable released;
  bool release = false;
  http.answer([&](const Request& request, Exchange& exchange) {
    if (request.id().empty()) {
      // The server takes its time to accept the notification.
      std::unique_lock lock{mutex};
      released.wait_for(lock, fake::patience, [&] { return release; });
      exchange.start(202);
      return mcp::Reply<void>{};
    }
    exchange.json(support::result(request.id(), R"({"tools":[]})"));
    return mcp::Reply<void>{};
  });
  Heard heard;
  const std::unique_ptr<mcp::Channel> channel =
      mcp::http_channel(std::string{fake::url}, http, {});
  REQUIRE(channel->open(heard).has_value());
  const std::string note = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
  REQUIRE(channel->send(outgoing(note, "", "notifications/initialized", "2025-11-25"))
              .has_value());
  const std::string list = fake::request("2", "tools/list", "{}");
  REQUIRE(channel->send(outgoing(list, "2", "tools/list", "2025-11-25")).has_value());
  // The request waits while the notification has no status. This wait bounds
  // a check that nothing happens; it does not order anything.
  CHECK_FALSE(
      http.await([](const Request& request) { return request.rpc() == "tools/list"; },
                 200ms)
          .has_value());
  {
    const std::scoped_lock lock{mutex};
    release = true;
  }
  released.notify_all();
  REQUIRE(http.await_rpc("tools/list").has_value());
  CHECK(heard.await(1) ==
        std::vector<std::string>{"message " + support::result("2", R"({"tools":[]})")});
  channel->close();
}
