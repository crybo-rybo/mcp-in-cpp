// The client's protocol core, against a scripted channel: the test plays the
// server, and sees every message the client sends.
#include "scripted.hpp"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <map>
#include <mcp/client.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

using namespace scripted;

TEST_CASE("a server that answers discovery opens in the modern era") {
  const auto script = std::make_shared<Script>();
  script->respond = answering({{"server/discover", std::string{discovered}}});

  const mcp::Reply<mcp::Client> client = open(script);
  REQUIRE(client.has_value());
  const mcp::ServerInfo& server = client->server();
  CHECK(server.name == "script");
  CHECK(server.version == "1.0");
  CHECK(server.instructions == "Be kind.");
  // As the server wrote it.
  CHECK(server.capabilities.text == R"({ "tools" : {} })");
  CHECK(server.has("tools"));
  CHECK_FALSE(server.has("prompts"));
  CHECK(server.era == mcp::Era::modern);
  CHECK(server.protocol_version == "2026-07-28");

  const std::vector<Sent> sent = script->sent();
  REQUIRE(sent.size() == 1);
  CHECK(sent[0].message ==
        request("1", "server/discover", "{" + std::string{modern_meta} + "}"));
  CHECK(sent[0].id == "1");
  CHECK(sent[0].method == "server/discover");
  CHECK(sent[0].protocol_version == "2026-07-28");
}

TEST_CASE("a server that leaves its identity out of discovery still opens") {
  const auto script = std::make_shared<Script>();
  script->respond =
      answering({{"server/discover",
                  R"({"capabilities":{},"supportedVersions":["2026-07-28"]})"}});

  const mcp::Reply<mcp::Client> client = open(script);
  REQUIRE(client.has_value());
  CHECK(client->server().name.empty());
  CHECK_FALSE(client->server().instructions.has_value());
  CHECK(client->server().era == mcp::Era::modern);
}

TEST_CASE("a version the server lacks is asked again, as a new request, once") {
  const auto script = std::make_shared<Script>();
  const std::string unsupported =
      R"({"requested":"2026-07-28","supported":["2026-07-28"]})";
  bool refused = false;
  script->respond = [&](Script& server, const Sent& sent) {
    if (sent.method != "server/discover") {
      return;
    }
    if (!std::exchange(refused, true)) {
      server.deliver(
          error_with(sent.id, -32022, "Unsupported protocol version", unsupported));
    } else {
      server.deliver(support::result(sent.id, discovered));
    }
  };

  const mcp::Reply<mcp::Client> client = open(script);
  REQUIRE(client.has_value());
  CHECK(client->server().era == mcp::Era::modern);
  const std::vector<Sent> sent = script->sent();
  REQUIRE(sent.size() == 2);
  CHECK(sent[0].id == "1");
  CHECK(sent[1].message ==
        request("2", "server/discover", "{" + std::string{modern_meta} + "}"));
}

TEST_CASE("a version refused twice fails the opening") {
  const auto script = std::make_shared<Script>();
  const std::string unsupported =
      R"({"requested":"2026-07-28","supported":["2026-07-28"]})";
  script->respond = [&](Script& server, const Sent& sent) {
    server.deliver(
        error_with(sent.id, -32022, "Unsupported protocol version", unsupported));
  };

  const mcp::Reply<mcp::Client> client = open(script);
  REQUIRE_FALSE(client.has_value());
  CHECK(client.error().fault == mcp::Fault::rejected);
  CHECK(client.error().code == -32022);
  CHECK(script->sent().size() == 2);
}

TEST_CASE("a modern error with no version to retry fails, and never falls back") {
  struct Case {
    int code;
    std::string data;
  };
  for (const Case& refusal :
       {Case{-32022, R"({"requested":"2026-07-28","supported":["2099-01-01"]})"},
        Case{-32021, R"({"requiredCapabilities":{"sampling":{}}})"},
        Case{-32020, R"({"header":"Mcp-Method"})"}}) {
    INFO(refusal.code);
    for (const mcp::Era era : {mcp::Era::automatic, mcp::Era::modern}) {
      const auto script = std::make_shared<Script>();
      script->respond = [&](Script& server, const Sent& sent) {
        server.deliver(error_with(sent.id, refusal.code, "No", refusal.data));
      };
      const mcp::Reply<mcp::Client> client = open(script, options(era));
      REQUIRE_FALSE(client.has_value());
      CHECK(client.error().fault == mcp::Fault::rejected);
      CHECK(client.error().code == refusal.code);
      CHECK(client.error().message == "No");
      REQUIRE(client.error().data.has_value());
      CHECK(client.error().data->text == refusal.data);
      CHECK(script->sent().size() == 1);
      CHECK(script->events().back() == "close");
    }
  }
}

TEST_CASE("any other answer to discovery falls back to the handshake") {
  const std::string unsupported_but_legacy =
      R"({"requested":"2026-07-28","supported":["2025-11-25"]})";
  for (const std::string& answer :
       {support::error("1", -32601, "Method not found"),
        support::error("1", -32602, "Invalid params"),
        support::error("1", -32600, "Invalid Request"),
        support::error("1", -32603, "Internal error"),
        support::error("1", -32000, "Not initialized"),
        error_with("1", -32022, "Unsupported protocol version", unsupported_but_legacy),
        // A result that is not a discovery result.
        support::result("1", "{}"),
        support::result("1",
                        R"({"capabilities":{},"supportedVersions":["2025-11-25"]})"),
        support::result("1", "[]"), std::string{R"({"id":1,"result":{}})"}}) {
    INFO(answer);
    const auto script = std::make_shared<Script>();
    script->respond = [&](Script& server, const Sent& sent) {
      if (sent.method == "server/discover") {
        server.deliver(answer);
      } else if (sent.method == "initialize") {
        server.deliver(support::result(sent.id, initialized("2025-11-25")));
      }
    };

    const mcp::Reply<mcp::Client> client = open(script);
    REQUIRE(client.has_value());
    const mcp::ServerInfo& server = client->server();
    CHECK(server.era == mcp::Era::legacy);
    CHECK(server.protocol_version == "2025-11-25");
    CHECK(server.name == "script");
    CHECK(server.title == "Script");
    CHECK(server.version == "1.0");
    CHECK(server.capabilities.text == R"({"tools":{}})");

    const std::vector<Sent> sent = script->sent();
    REQUIRE(sent.size() == 3);
    CHECK(sent[1].message ==
          request("2", "initialize",
                  R"({"capabilities":{},"clientInfo":{"name":"zoo","version":"0.3.0"},)"
                  R"("protocolVersion":"2025-11-25"})"));
    CHECK(sent[1].protocol_version.empty());
    CHECK(sent[2].message ==
          R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    CHECK(sent[2].id.empty());
    CHECK(sent[2].protocol_version == "2025-11-25");
  }
}

TEST_CASE("the handshake accepts 2025-06-18 and nothing it does not speak") {
  for (const mcp::Era era : {mcp::Era::automatic, mcp::Era::legacy}) {
    const auto script = std::make_shared<Script>();
    std::string offered = "2025-06-18";
    script->respond = [&](Script& server, const Sent& sent) {
      if (sent.method == "server/discover") {
        server.deliver(support::error(sent.id, -32601, "Method not found"));
      } else if (sent.method == "initialize") {
        server.deliver(support::result(sent.id, initialized(offered)));
      }
    };
    const mcp::Reply<mcp::Client> client = open(script, options(era));
    REQUIRE(client.has_value());
    CHECK(client->server().protocol_version == "2025-06-18");

    offered = "2024-11-05";
    const mcp::Reply<mcp::Client> refused = open(script, options(era));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().fault == mcp::Fault::protocol);
  }
}

TEST_CASE("a handshake result without the server's identity fails the opening") {
  const auto script = std::make_shared<Script>();
  script->respond = answering(
      {{"initialize", R"({"capabilities":{},"protocolVersion":"2025-11-25"})"}});
  const mcp::Reply<mcp::Client> client = open(script, options(mcp::Era::legacy));
  REQUIRE_FALSE(client.has_value());
  CHECK(client.error().fault == mcp::Fault::protocol);
}

TEST_CASE("Era::legacy starts with the handshake and Era::modern never sends it") {
  {
    const auto script = std::make_shared<Script>();
    script->respond = answering({{"initialize", initialized("2025-11-25")}});
    const mcp::Reply<mcp::Client> client = open(script, options(mcp::Era::legacy));
    REQUIRE(client.has_value());
    CHECK(script->sent()[0].method == "initialize");
    CHECK(script->sent()[0].id == "1");
  }
  {
    const auto script = std::make_shared<Script>();
    script->respond = [](Script& server, const Sent& sent) {
      server.deliver(support::error(sent.id, -32601, "Method not found"));
    };
    const mcp::Reply<mcp::Client> client = open(script, options(mcp::Era::modern));
    REQUIRE_FALSE(client.has_value());
    CHECK(client.error().fault == mcp::Fault::rejected);
    CHECK(client.error().code == -32601);
    CHECK(script->sent().size() == 1);
  }
}

TEST_CASE("a slow server that answers the handshake first opens in the legacy era") {
  const auto script = std::make_shared<Script>();
  mcp::ClientOptions with = options();
  with.probe_timeout = 1ms;
  auto opening = std::async(std::launch::async, [&] { return open(script, with); });

  const std::optional<Sent> handshake = script->await("initialize");
  REQUIRE(handshake.has_value());
  script->deliver(support::result(handshake->id, initialized("2025-11-25")));
  REQUIRE(opening.wait_for(patience) == std::future_status::ready);
  const mcp::Reply<mcp::Client> client = opening.get();
  REQUIRE(client.has_value());
  CHECK(client->server().era == mcp::Era::legacy);

  // The probe's answer, when it comes at last, concerns no call.
  script->deliver(support::result("1", discovered));
  CHECK(script->events() ==
        std::vector<std::string>{"send server/discover", "send initialize",
                                 "send notifications/initialized", "abandon 1"});
}

TEST_CASE("a slow server that answers discovery first opens in the modern era") {
  const auto script = std::make_shared<Script>();
  mcp::ClientOptions with = options();
  with.probe_timeout = 1ms;
  auto opening = std::async(std::launch::async, [&] { return open(script, with); });

  const std::optional<Sent> handshake = script->await("initialize");
  REQUIRE(handshake.has_value());
  CHECK(handshake->id == "2");
  script->deliver(support::result("1", discovered));
  REQUIRE(opening.wait_for(patience) == std::future_status::ready);
  const mcp::Reply<mcp::Client> client = opening.get();
  REQUIRE(client.has_value());
  CHECK(client->server().era == mcp::Era::modern);

  script->deliver(support::result("2", initialized("2025-11-25")));
  // `initialize` may not be cancelled, so it is only abandoned.
  CHECK(script->events() == std::vector<std::string>{"send server/discover",
                                                     "send initialize", "abandon 2"});
}

TEST_CASE("a slow server whose handshake fails may still answer discovery") {
  const auto script = std::make_shared<Script>();
  mcp::ClientOptions with = options();
  with.probe_timeout = 1ms;
  auto opening = std::async(std::launch::async, [&] { return open(script, with); });

  const std::optional<Sent> handshake = script->await("initialize");
  REQUIRE(handshake.has_value());
  script->deliver(support::error(handshake->id, -32601, "Method not found"));
  script->deliver(support::result("1", discovered));
  REQUIRE(opening.wait_for(patience) == std::future_status::ready);
  const mcp::Reply<mcp::Client> client = opening.get();
  REQUIRE(client.has_value());
  CHECK(client->server().era == mcp::Era::modern);
}

TEST_CASE("Era::modern waits for a slow server without sending the handshake") {
  const auto script = std::make_shared<Script>();
  mcp::ClientOptions with = options(mcp::Era::modern);
  with.probe_timeout = 1ms;
  auto opening = std::async(std::launch::async, [&] { return open(script, with); });

  REQUIRE(script->await("server/discover").has_value());
  // Long past the probe's time.
  std::this_thread::sleep_for(50ms);
  script->deliver(support::result("1", discovered));
  REQUIRE(opening.wait_for(patience) == std::future_status::ready);
  REQUIRE(opening.get().has_value());
  CHECK(script->sent().size() == 1);
}

TEST_CASE("a server that answers neither request fails the opening in time") {
  const auto script = std::make_shared<Script>();
  mcp::ClientOptions with = options();
  with.probe_timeout = 1ms;
  with.timeout = 300ms;
  const mcp::Reply<mcp::Client> client = open(script, with);
  REQUIRE_FALSE(client.has_value());
  CHECK(client.error().fault == mcp::Fault::timeout);
  // Neither may be cancelled.
  CHECK(script->events() == std::vector<std::string>{"send server/discover",
                                                     "send initialize", "abandon 1",
                                                     "abandon 2", "close"});
}

TEST_CASE("a modern request carries the protocol fields, and a legacy one does not") {
  for (const mcp::Era era : {mcp::Era::modern, mcp::Era::legacy}) {
    Opened opened = open_in(era);
    REQUIRE(opened.client.has_value());
    const bool modern = era == mcp::Era::modern;
    opened.script->respond = answering({{"tools/list", R"({"tools":[]})"}});

    REQUIRE(opened.client->list_tools().has_value());
    const Sent sent = opened.script->sent().back();
    CHECK(sent.message ==
          request("2", "tools/list",
                  modern ? "{" + std::string{modern_meta} + "}" : "{}"));
    CHECK(sent.protocol_version == (modern ? "2026-07-28" : "2025-11-25"));

    // A call asks for progress with its own id, in either era.
    opened.script->respond = answering({{"tools/call", R"({"content":[]})"}});
    const mcp::Reply<mcp::ToolResult> called =
        opened.client->call_tool("forecast", mcp::Json{R"( { "city" : "Oslo",
                                    "days": 3.0 } )"},
                                 {.progress = [](const mcp::ProgressReport&) {}});
    REQUIRE(called.has_value());
    const Sent call = opened.script->sent().back();
    const std::string next = "3";
    const std::string meta =
        modern ? std::string{modern_meta}.insert(modern_meta.size() - 1,
                                                 R"(,"progressToken":)" + next)
               : R"("_meta":{"progressToken":)" + next + "}";
    // The arguments as the caller wrote them, on one line.
    CHECK(call.message ==
          request(next, "tools/call",
                  "{" + meta +
                      R"(,"arguments":{"city":"Oslo","days":3.0},"name":"forecast"})"));
    CHECK(call.id == next);
    CHECK(call.name == "forecast");
  }
}

TEST_CASE("arguments that are not a JSON object are not sent") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  for (const std::string_view arguments : {"[1]", "3", "{", "", R"({"a":1} x)"}) {
    INFO(arguments);
    const mcp::Reply<mcp::ToolResult> called =
        opened.client->call_tool("forecast", mcp::Json{std::string{arguments}});
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().fault == mcp::Fault::protocol);
  }
  CHECK(opened.script->sent().size() == 1);
}

TEST_CASE("a JSON-RPC error is a rejection that keeps its code, message and data") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  opened.script->respond = [](Script& server, const Sent& sent) {
    server.deliver(error_with(sent.id, -32602, "Unknown tool: fly", R"({"x": 1.50})"));
  };
  const mcp::Reply<mcp::ToolResult> called = opened.client->call_tool("fly");
  REQUIRE_FALSE(called.has_value());
  CHECK(called.error().fault == mcp::Fault::rejected);
  CHECK(called.error().code == -32602);
  CHECK(called.error().message == "Unknown tool: fly");
  REQUIRE(called.error().data.has_value());
  CHECK(called.error().data->text == R"({"x": 1.50})");
}

TEST_CASE("an answer the protocol does not allow fails its call with Fault::protocol") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  for (
      const std::string_view answer : {
          R"({"id":2,"result":{"content":[]}})",
          R"({"id":2,"jsonrpc":"1.0","result":{"content":[]}})",
          R"({"id":2,"jsonrpc":"2.0","result":{"content":[]},"error":{"code":1,"message":"x"}})",
          R"({"id":2,"jsonrpc":"2.0"})",
          R"({"id":2,"jsonrpc":"2.0","result":[]})",
          R"({"id":2,"jsonrpc":"2.0","error":"no"})",
          R"({"id":2,"jsonrpc":"2.0","error":{"code":"1","message":"x"}})",
          R"({"id":2,"jsonrpc":"2.0","error":{"code":1.5,"message":"x"}})",
          R"({"id":2,"jsonrpc":"2.0","error":{"code":1}})",
          R"({"id":2,"jsonrpc":"2.0","result":{}})",
          R"({"id":2,"jsonrpc":"2.0","result":{"content":{}}})",
          R"({"id":2,"jsonrpc":"2.0","result":{"content":[{"type":"video","url":"x"}]}})",
          R"({"id":2,"jsonrpc":"2.0","result":{"content":[{"type":"text"}]}})",
          R"({"id":2,"jsonrpc":"2.0","result":{"content":[],"isError":"yes"}})",
          R"({"id":2,"jsonrpc":"2.0","result":{"content":[],"resultType":"input_required",)"
          R"("inputRequests":{}}})",
          R"({"id":2,"jsonrpc":"2.0","result":{"content":[],"resultType":"later"}})",
      }) {
    INFO(answer);
    opened.script->respond = [answer](Script& server, const Sent& sent) {
      // Every call is answered as if it were the second request.
      std::string text{answer};
      text.replace(text.find(R"("id":2)"), 6, R"("id":)" + sent.id);
      server.deliver(text);
    };
    const mcp::Reply<mcp::ToolResult> called = opened.client->call_tool("add");
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().fault == mcp::Fault::protocol);
  }
}

TEST_CASE("messages that concern no call are passed over") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  opened.script->respond = [](Script& server, const Sent& sent) {
    for (
        const std::string_view noise : {
            "nonsense",
            "",
            "[]",
            "42",
            R"({"jsonrpc":"2.0","id":99,"result":{"content":[]}})",
            R"({"jsonrpc":"2.0","id":"2","result":{"content":[]}})",
            R"({"jsonrpc":"2.0","id":-2,"result":{"content":[]}})",
            R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"Parse error"}})",
            R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","data":1}})",
            R"({"jsonrpc":"2.0","method":"notifications/tools/list_changed"})",
            R"({"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":2,"progress":1}})",
            R"({"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":77,"progress":1}})",
            R"({"jsonrpc":"2.0","method":"notifications/progress","params":"x"})",
            R"({"jsonrpc":"2.0","method":7})",
            R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":2}})",
        }) {
      server.deliver(noise);
    }
    server.deliver(
        support::result(sent.id, R"({"content":[{"type":"text","text":"5"}]})"));
  };
  const mcp::Reply<mcp::ToolResult> called = opened.client->call_tool("add");
  REQUIRE(called.has_value());
  CHECK(text_of(*called) == "5");
}

TEST_CASE("a request of the server's is answered as a client with no capabilities") {
  Opened opened = open_in(mcp::Era::legacy);
  REQUIRE(opened.client.has_value());
  opened.script->deliver(
      R"({"jsonrpc":"2.0","id":"s1","method":"sampling/createMessage","params":{}})");
  opened.script->deliver(R"({"jsonrpc":"2.0","id":7,"method":"ping"})");
  opened.script->deliver(R"({"jsonrpc":"2.0","id":[1],"method":"ping"})");

  const std::vector<Sent> sent = opened.script->sent();
  REQUIRE(sent.size() == 4);
  CHECK(sent[2].message == R"({"error":{"code":-32601,"message":"Method not found: )"
                           R"(sampling/createMessage"},"id":"s1","jsonrpc":"2.0"})");
  CHECK(sent[2].id.empty());
  CHECK(sent[2].method.empty());
  CHECK(sent[2].protocol_version == "2025-11-25");
  CHECK(sent[3].message == R"({"id":7,"jsonrpc":"2.0","result":{}})");
}

TEST_CASE("ping asks for a ping, or for discovery where there is no ping") {
  for (const mcp::Era era : {mcp::Era::modern, mcp::Era::legacy}) {
    Opened opened = open_in(era);
    REQUIRE(opened.client.has_value());
    opened.script->respond =
        answering({{"ping", "{}"}, {"server/discover", std::string{discovered}}});
    REQUIRE(opened.client->ping().has_value());
    CHECK(opened.script->sent().back().method ==
          (era == mcp::Era::modern ? "server/discover" : "ping"));
  }
}

TEST_CASE("a call that is not answered in time is cancelled") {
  for (const mcp::Era era : {mcp::Era::modern, mcp::Era::legacy}) {
    mcp::ClientOptions with = options();
    with.timeout = 50ms;
    Opened opened = open_in(era, with);
    REQUIRE(opened.client.has_value());
    const std::size_t before = opened.script->events().size();

    const mcp::Reply<mcp::ToolResult> called = opened.client->call_tool("add");
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().fault == mcp::Fault::timeout);
    const std::string id = "2";
    std::vector<std::string> events = opened.script->events();
    events.erase(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(before));
    // The channel gives up on it first, then the server is told.
    CHECK(events == std::vector<std::string>{"send tools/call", "abandon " + id,
                                             "send notifications/cancelled"});
    CHECK(opened.script->sent().back().message ==
          R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"reason":)"
          R"("the request timed out","requestId":)" +
              id + "}}");
    CHECK(opened.script->sent().back().protocol_version ==
          (era == mcp::Era::modern ? "2026-07-28" : "2025-11-25"));

    // An answer that comes too late concerns no call.
    opened.script->deliver(support::result(id, R"({"content":[]})"));
  }
}

TEST_CASE("a call's own time limit replaces the client's, and zero has none") {
  mcp::ClientOptions with = options();
  with.timeout = 10ms;
  Opened opened = open_in(mcp::Era::modern, with);
  REQUIRE(opened.client.has_value());
  mcp::Client& client = *opened.client;
  auto calling = std::async(std::launch::async, [&client] {
    return client.call_tool("add", mcp::Json{"{}"}, {.timeout = 0ms});
  });
  const std::optional<Sent> sent = opened.script->await("tools/call");
  REQUIRE(sent.has_value());
  // Long past the client's own limit.
  std::this_thread::sleep_for(100ms);
  opened.script->deliver(support::result(sent->id, R"({"content":[]})"));
  REQUIRE(calling.wait_for(patience) == std::future_status::ready);
  CHECK(calling.get().has_value());
}

TEST_CASE("a time limit too long to count is no limit") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  opened.script->respond = answering({{"tools/call", R"({"content":[]})"}});
  CHECK(opened.client
            ->call_tool("add", mcp::Json{"{}"},
                        {.timeout = std::chrono::milliseconds::max()})
            .has_value());
}

TEST_CASE("a progress report starts the time again, and reaches the caller") {
  mcp::ClientOptions with = options();
  with.timeout = 1500ms;
  Opened opened = open_in(mcp::Era::modern, with);
  REQUIRE(opened.client.has_value());
  mcp::Client& client = *opened.client;

  std::mutex mutex;
  std::vector<mcp::ProgressReport> reports;
  const mcp::CallOptions call{.progress =
                                  [&](const mcp::ProgressReport& report) {
                                    const std::scoped_lock lock{mutex};
                                    reports.push_back(report);
                                  },
                              .stop = {},
                              .timeout = std::nullopt};
  auto calling = std::async(std::launch::async, [&] {
    return client.call_tool("add", mcp::Json{"{}"}, call);
  });
  const std::optional<Sent> sent = opened.script->await("tools/call");
  REQUIRE(sent.has_value());
  // A report comes at once and then one every 600 ms, well within the limit,
  // and the answer comes after 1800 ms, well past it.
  for (int step = 1; step <= 3; ++step) {
    if (step > 1) {
      std::this_thread::sleep_for(600ms);
    }
    opened.script->deliver(
        R"({"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":)" +
        sent->id + R"(,"progress":)" + std::to_string(step) +
        R"(,"total":3,"message":"step"}})");
  }
  std::this_thread::sleep_for(600ms);
  opened.script->deliver(support::result(sent->id, R"({"content":[]})"));
  REQUIRE(calling.wait_for(patience) == std::future_status::ready);
  const mcp::Reply<mcp::ToolResult> called = calling.get();
  INFO((called ? std::string{} : called.error().message));
  REQUIRE(called.has_value());

  const std::scoped_lock lock{mutex};
  REQUIRE(reports.size() == 3);
  for (std::size_t index = 0; index < reports.size(); ++index) {
    CHECK(reports[index].progress == static_cast<double>(index + 1));
    CHECK(reports[index].total == 3.0);
    CHECK(reports[index].message == "step");
  }
}

TEST_CASE("a stopped call is cancelled at once, on the wire as its era says") {
  struct Case {
    mcp::Era era;
    bool abandon_tells;
  };
  for (const Case& each : {Case{mcp::Era::modern, false}, Case{mcp::Era::legacy, false},
                           Case{mcp::Era::modern, true}}) {
    Opened opened = open_in(each.era);
    REQUIRE(opened.client.has_value());
    opened.script->abandon_tells = each.abandon_tells;
    mcp::Client& client = *opened.client;
    std::stop_source stop;
    auto calling = std::async(std::launch::async, [&] {
      return client.call_tool(
          "add", mcp::Json{"{}"},
          {.progress = {}, .stop = stop.get_token(), .timeout = {}});
    });
    const std::optional<Sent> sent = opened.script->await("tools/call");
    REQUIRE(sent.has_value());
    const std::size_t before = opened.script->events().size();
    stop.request_stop();
    REQUIRE(calling.wait_for(patience) == std::future_status::ready);
    const mcp::Reply<mcp::ToolResult> called = calling.get();
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().fault == mcp::Fault::cancelled);

    std::vector<std::string> events = opened.script->events();
    events.erase(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(before));
    if (each.abandon_tells) {
      // Closing the request's stream told the server already.
      CHECK(events == std::vector<std::string>{"abandon " + sent->id});
    } else {
      CHECK(events == std::vector<std::string>{"abandon " + sent->id,
                                               "send notifications/cancelled"});
      CHECK(opened.script->sent().back().message ==
            R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"reason":)"
            R"("the caller cancelled the request","requestId":)" +
                sent->id + "}}");
    }
  }
}

TEST_CASE("a call whose stop was requested already is not sent") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  std::stop_source stop;
  stop.request_stop();
  const mcp::Reply<mcp::ToolResult> called = opened.client->call_tool(
      "add", mcp::Json{"{}"},
      {.progress = {}, .stop = stop.get_token(), .timeout = {}});
  REQUIRE_FALSE(called.has_value());
  CHECK(called.error().fault == mcp::Fault::cancelled);
  CHECK(opened.script->sent().size() == 1);
}

TEST_CASE("list_tools follows the cursors and keeps each tool as the server wrote it") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  const std::string first_page =
      R"({"nextCursor":"p2","tools":[{"name":"a","title":"A","description":"First",)"
      R"("inputSchema":{"type": "object", "properties": {"z": {}, "a": {"maximum": 1e2}}},)"
      R"("outputSchema":{"type":"integer"},"annotations":{"readOnlyHint":true},)"
      R"("_meta":{"x":1},"icons":[]}]})";
  const std::string second_page = R"({"tools":[{"name":"b","inputSchema":{}}]})";
  opened.script->respond = [&](Script& server, const Sent& sent) {
    server.deliver(support::result(sent.id, sent.message.find(R"("cursor":"p2")") ==
                                                    std::string::npos
                                                ? first_page
                                                : second_page));
  };

  const mcp::Reply<std::vector<mcp::Tool>> tools = opened.client->list_tools();
  REQUIRE(tools.has_value());
  REQUIRE(tools->size() == 2);
  const mcp::Tool& a = (*tools)[0];
  CHECK(a.name == "a");
  CHECK(a.title == "A");
  CHECK(a.description == "First");
  CHECK(a.input_schema.text ==
        R"({"type": "object", "properties": {"z": {}, "a": {"maximum": 1e2}}})");
  REQUIRE(a.output_schema.has_value());
  CHECK(a.output_schema->text == R"({"type":"integer"})");
  REQUIRE(a.annotations.has_value());
  CHECK(a.annotations->text == R"({"readOnlyHint":true})");
  const mcp::Tool& b = (*tools)[1];
  CHECK(b.name == "b");
  CHECK(b.title.empty());
  CHECK(b.input_schema.text == "{}");
  CHECK_FALSE(b.output_schema.has_value());

  CHECK(opened.script->sent().back().message ==
        request("3", "tools/list",
                "{" + std::string{modern_meta} + R"(,"cursor":"p2"})"));
}

TEST_CASE("a tool list that never ends fails within its bound of pages") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  opened.script->respond = [](Script& server, const Sent& sent) {
    server.deliver(support::result(sent.id, R"({"nextCursor":"again","tools":[]})"));
  };
  const mcp::Reply<std::vector<mcp::Tool>> tools = opened.client->list_tools();
  REQUIRE_FALSE(tools.has_value());
  CHECK(tools.error().fault == mcp::Fault::protocol);
  CHECK(opened.script->sent().size() == 1 + 1000);
}

TEST_CASE("a tool list the protocol does not allow fails with Fault::protocol") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  for (const std::string_view result :
       {R"({})", R"({"tools":{}})", R"({"tools":[{"inputSchema":{}}]})",
        R"({"tools":[{"name":"a"}]})", R"({"tools":[{"name":"a","inputSchema":true}]})",
        R"({"tools":["a"]})", R"({"tools":[],"nextCursor":3})"}) {
    INFO(result);
    opened.script->respond = [result](Script& server, const Sent& sent) {
      server.deliver(support::result(sent.id, result));
    };
    const mcp::Reply<std::vector<mcp::Tool>> tools = opened.client->list_tools();
    REQUIRE_FALSE(tools.has_value());
    CHECK(tools.error().fault == mcp::Fault::protocol);
  }
}

TEST_CASE("a channel that closes fails every waiting call, and every later one") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  mcp::Client& client = *opened.client;
  auto calling =
      std::async(std::launch::async, [&] { return client.call_tool("add"); });
  REQUIRE(opened.script->await("tools/call").has_value());
  opened.script->close({.fault = mcp::Fault::transport,
                        .code = 0,
                        .message = "the server exited",
                        .data = std::nullopt});
  REQUIRE(calling.wait_for(patience) == std::future_status::ready);
  const mcp::Reply<mcp::ToolResult> called = calling.get();
  REQUIRE_FALSE(called.has_value());
  CHECK(called.error().fault == mcp::Fault::transport);
  CHECK(called.error().message == "the server exited");

  const std::size_t sent = opened.script->sent().size();
  const mcp::Reply<std::vector<mcp::Tool>> later = client.list_tools();
  REQUIRE_FALSE(later.has_value());
  CHECK(later.error().message == "the server exited");
  CHECK(opened.script->sent().size() == sent);
}

TEST_CASE("a request the channel gives up on, or cannot send, fails alone") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  opened.script->respond = [](Script& server, const Sent& sent) {
    server.fail(sent.id, {.fault = mcp::Fault::transport,
                          .code = 0,
                          .message = "the stream ended",
                          .data = std::nullopt});
  };
  const mcp::Reply<mcp::ToolResult> failed = opened.client->call_tool("add");
  REQUIRE_FALSE(failed.has_value());
  CHECK(failed.error().message == "the stream ended");

  opened.script->respond = {};
  opened.script->refuse = mcp::ClientError{
      .fault = mcp::Fault::transport, .code = 0, .message = "broken pipe", .data = {}};
  const mcp::Reply<mcp::ToolResult> unsent = opened.client->call_tool("add");
  REQUIRE_FALSE(unsent.has_value());
  CHECK(unsent.error().message == "broken pipe");

  opened.script->refuse.reset();
  opened.script->respond = answering({{"tools/call", R"({"content":[]})"}});
  CHECK(opened.client->call_tool("add").has_value());
}

TEST_CASE("request ids are unique across threads") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  opened.script->respond = answering({{"tools/call", R"({"content":[]})"}});
  mcp::Client& client = *opened.client;
  constexpr int threads = 8;
  constexpr int calls = 25;
  std::vector<std::future<int>> workers;
  for (int thread = 0; thread < threads; ++thread) {
    workers.push_back(std::async(std::launch::async, [&client] {
      int succeeded = 0;
      for (int call = 0; call < calls; ++call) {
        succeeded += client.call_tool("add").has_value() ? 1 : 0;
      }
      return succeeded;
    }));
  }
  for (std::future<int>& worker : workers) {
    REQUIRE(worker.wait_for(patience) == std::future_status::ready);
    CHECK(worker.get() == calls);
  }
  std::set<std::string> ids;
  for (const Sent& sent : opened.script->sent()) {
    ids.insert(sent.id);
  }
  CHECK(ids.size() == opened.script->sent().size());
  CHECK(ids.size() == 1 + threads * calls);
}

namespace {

struct Arguments {
  std::int32_t a{};
  std::int32_t b{};
};

} // namespace

TEST_CASE("a typed call decodes the structured content, or else the text") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  std::string answer;
  opened.script->respond = [&answer](Script& server, const Sent& sent) {
    server.deliver(support::result(sent.id, answer));
  };
  mcp::Client& client = *opened.client;

  answer = R"({"content":[{"type":"text","text":"ignored"}],"structuredContent":5})";
  CHECK(client.call<std::int32_t>("add", Arguments{.a = 2, .b = 3}) == 5);
  CHECK(opened.script->sent().back().message.find(R"("arguments":{"a":2,"b":3})") !=
        std::string::npos);
  answer = R"({"content":[{"type":"text","text":"[1,2]"}]})";
  CHECK(client.call<std::vector<std::int32_t>>("add", Arguments{}) ==
        std::vector<std::int32_t>{1, 2});
  answer = R"({"content":[{"type":"text","text":"plain words"}]})";
  CHECK(client.call<std::string>("add", Arguments{}) == "plain words");
  answer = R"({"content":[{"type":"text","text":"\"spelled\""}]})";
  CHECK(client.call<std::string>("add", Arguments{}) == "spelled");
  answer = R"({"content":[{"type":"text","text":"42"}]})";
  CHECK(client.call<std::string>("add", Arguments{}) == "42");

  for (const std::string_view mismatch :
       {R"({"content":[{"type":"text","text":"plain words"}]})",
        R"({"content":[],"structuredContent":"five"})", R"({"content":[]})",
        R"({"content":[{"type":"text","text":"1"},{"type":"text","text":"2"}]})"}) {
    INFO(mismatch);
    answer = mismatch;
    const mcp::Reply<std::int32_t> value =
        client.call<std::int32_t>("add", Arguments{});
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error().fault == mcp::Fault::mismatch);
  }

  answer = R"({"content":[{"type":"text","text":"not today"}],"isError":true})";
  const mcp::Reply<std::int32_t> refused =
      client.call<std::int32_t>("add", Arguments{});
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().fault == mcp::Fault::tool);
  CHECK(refused.error().message == "not today");
  // A tool's failure is a result when it is called by name.
  const mcp::Reply<mcp::ToolResult> by_name = client.call_tool("add");
  REQUIRE(by_name.has_value());
  CHECK(by_name->is_error);
}

TEST_CASE("a moved-from client answers every call with Fault::transport") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  mcp::Client moved = std::move(*opened.client);
  const mcp::Reply<std::vector<mcp::Tool>> tools = opened.client->list_tools();
  REQUIRE_FALSE(tools.has_value());
  CHECK(tools.error().fault == mcp::Fault::transport);
  CHECK(opened.client->server().name.empty());
  CHECK(moved.server().name == "script");
}

TEST_CASE("destroying a client closes its channel") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  opened.client = std::unexpected(mcp::ClientError{});
  CHECK(opened.script->events().back() == "close");
}
