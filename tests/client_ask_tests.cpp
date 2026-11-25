// What a server asks of the client, against a scripted channel: rounds of
// input_required under 2026-07-28, and the requests of a handshake server.
#include "scripted.hpp"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mcp/client.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace scripted;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view every_capability =
    R"({"elicitation":{"form":{}},"roots":{},"sampling":{}})";

// The metadata of a modern request of a client named zoo with these
// capabilities.
std::string meta_with(const std::string_view capabilities) {
  return std::string{R"("_meta":{"io.modelcontextprotocol/clientCapabilities":)"} +
         std::string{capabilities} +
         R"(,"io.modelcontextprotocol/clientInfo":{"name":"zoo","version":"0.3.0"},)"
         R"("io.modelcontextprotocol/protocolVersion":"2026-07-28"})";
}

// The params of tools/call of `book` for Lima, with the answers and the state
// of a round, as JSON text, when they are given.
std::string call_params(const std::string_view capabilities,
                        const std::string_view responses = {},
                        const std::string_view state = {}) {
  std::string params =
      "{" + meta_with(capabilities) + R"(,"arguments":{"place":"Lima"})";
  if (!responses.empty()) {
    params += R"(,"inputResponses":)" + std::string{responses};
  }
  params += R"(,"name":"book")";
  if (!state.empty()) {
    params += R"(,"requestState":)" + std::string{state};
  }
  return params + "}";
}

// A result of input_required with these members of inputRequests and this
// requestState, each JSON text, when they are given.
std::string asking(const std::string_view requests, const std::string_view state = {}) {
  std::string result{"{"};
  if (!requests.empty()) {
    result += R"("inputRequests":{)" + std::string{requests} + "},";
  }
  if (!state.empty()) {
    result += R"("requestState":)" + std::string{state} + ",";
  }
  return result + R"("resultType":"input_required"})";
}

// A question of each kind, as an entry of inputRequests.
std::string form_question(const std::string_view key,
                          const std::string_view message = "Who?") {
  return "\"" + std::string{key} +
         R"(":{"method":"elicitation/create","params":{"mode":"form","message":")" +
         std::string{message} +
         R"(","requestedSchema":{"type":"object","properties":{"name":{"type":"string"}}}}})";
}

std::string sampling_question(const std::string_view key) {
  return "\"" + std::string{key} +
         R"(":{"method":"sampling/createMessage","params":{"messages":[{"role":"user",)"
         R"("content":{"type":"text","text":"A word?"}}],"maxTokens":10}})";
}

std::string roots_question(const std::string_view key) {
  return "\"" + std::string{key} + R"(":{"method":"roots/list"})";
}

constexpr std::string_view booked = R"({"content":[{"type":"text","text":"booked"}]})";

// Answers the calls of tools/call, one after another, with these results,
// while they last.
std::function<void(Script&, const Sent&)> rounds(std::vector<std::string> results) {
  auto next = std::make_shared<std::size_t>(0);
  return [results = std::move(results), next](Script& script, const Sent& sent) {
    if (sent.method == "tools/call" && *next < results.size()) {
      script.deliver(support::result(sent.id, results[(*next)++]));
    }
  };
}

mcp::Answer<mcp::Json> accept(std::string content) {
  return {.action = mcp::Action::accept, .content = mcp::Json{std::move(content)}};
}

mcp::SamplingResult said(std::string text) {
  return {.role = mcp::Role::assistant,
          .content = {mcp::TextContent{.text = std::move(text)}},
          .model = "m1",
          .stop_reason = "endTurn"};
}

// Options with a handler of each kind that answers at once.
mcp::ClientOptions answering_all() {
  mcp::ClientOptions with = options();
  with.elicitation = [](const mcp::FormRequest&) {
    return accept(R"({"name":"Ada"})");
  };
  with.sampling = [](const mcp::SamplingRequest&) -> mcp::Result<mcp::SamplingResult> {
    return said("hello");
  };
  with.roots = [] { return std::vector<mcp::Root>{{.uri = "file:///work"}}; };
  return with;
}

mcp::Reply<mcp::ToolResult> book(mcp::Client& client,
                                 const mcp::CallOptions& call = {}) {
  return client.call_tool("book", mcp::Json{R"({"place":"Lima"})"}, call);
}

std::size_t calls_sent(const Script& script) {
  std::size_t count = 0;
  for (const Sent& sent : script.sent()) {
    count += sent.method == "tools/call" ? 1U : 0U;
  }
  return count;
}

// The client's response to the server's request `id`, once it is sent.
std::optional<std::string> response_to(const Script& script,
                                       const std::string_view id) {
  const std::string marker = R"("id":)" + std::string{id} + ",";
  const std::optional<Sent> sent = script.await_if([&marker](const Sent& each) {
    return each.method.empty() && each.message.find(marker) != std::string::npos;
  });
  return sent.has_value() ? std::optional{sent->message} : std::nullopt;
}

std::string server_request(const std::string_view id, const std::string_view method,
                           const std::string_view params) {
  return R"({"jsonrpc":"2.0","id":)" + std::string{id} + R"(,"method":")" +
         std::string{method} + R"(","params":)" + std::string{params} + "}";
}

constexpr std::string_view who_params =
    R"({"mode":"form","message":"Who?","requestedSchema":{"type":"object",)"
    R"("properties":{"name":{"type":"string"}}}})";

} // namespace

// Capabilities.

TEST_CASE("each handler declares its capability, as each era declares it") {
  struct Case {
    bool elicitation;
    bool sampling;
    bool roots;
    std::string_view declared;
  };
  for (const Case& each : {Case{false, false, false, "{}"},
                           Case{true, false, false, R"({"elicitation":{"form":{}}})"},
                           Case{false, true, false, R"({"sampling":{}})"},
                           Case{false, false, true, R"({"roots":{}})"},
                           Case{true, true, true, every_capability}}) {
    INFO(each.declared);
    mcp::ClientOptions with = answering_all();
    if (!each.elicitation) {
      with.elicitation = {};
    }
    if (!each.sampling) {
      with.sampling = {};
    }
    if (!each.roots) {
      with.roots = {};
    }

    // Under 2026-07-28, in the _meta of every request.
    Opened modern = open_in(mcp::Era::modern, with);
    REQUIRE(modern.client.has_value());
    modern.script->respond = answering({{"tools/call", std::string{booked}}});
    REQUIRE(book(*modern.client).has_value());
    const std::vector<Sent> sent = modern.script->sent();
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].message ==
          request("1", "server/discover", "{" + meta_with(each.declared) + "}"));
    CHECK(sent[1].message == request("2", "tools/call", call_params(each.declared)));

    // In a handshake, in initialize, whichever revision the server answers
    // with: one initialize asks for 2025-11-25, whose shape a 2025-06-18
    // server reads as it reads its own.
    for (const std::string_view version : {"2025-11-25", "2025-06-18"}) {
      INFO(version);
      const auto script = std::make_shared<Script>();
      script->respond = answering({{"initialize", initialized(version)}});
      mcp::ClientOptions legacy = with;
      legacy.era = mcp::Era::legacy;
      const mcp::Reply<mcp::Client> client = open(script, legacy);
      REQUIRE(client.has_value());
      CHECK(script->sent().front().message ==
            request("1", "initialize",
                    R"({"capabilities":)" + std::string{each.declared} +
                        R"(,"clientInfo":{"name":"zoo","version":"0.3.0"},)"
                        R"("protocolVersion":"2025-11-25"})"));
    }
  }
}

// Rounds of input_required.

TEST_CASE("a call that needs input is sent again with the answers and the state") {
  mcp::ClientOptions with = options();
  std::vector<mcp::FormRequest> forms;
  with.elicitation = [&forms](const mcp::FormRequest& form) {
    forms.push_back(form);
    // Spread over lines, as a person's tool may write it.
    return accept("{\n  \"name\" : \"Ada\"\n}");
  };
  Opened opened = open_in(mcp::Era::modern, with);
  REQUIRE(opened.client.has_value());
  const std::string schema =
      R"({ "type": "object", "properties": { "name": {"type": "string"} } })";
  // A state spelled with escapes, which goes back as it was written.
  const std::string state = R"("s1\/x+==")";
  opened.script->respond =
      rounds({asking(R"("who":{"method":"elicitation/create","params":{"mode":"form",)"
                     R"("message":"Who?","requestedSchema":)" +
                         schema + "}}",
                     state),
              std::string{booked}});

  const mcp::Reply<mcp::ToolResult> called = book(*opened.client);
  REQUIRE(called.has_value());
  CHECK(text_of(*called) == "booked");
  REQUIRE(forms.size() == 1);
  CHECK(forms[0].message == "Who?");
  CHECK(forms[0].schema.text == schema);

  const std::string declared = R"({"elicitation":{"form":{}}})";
  const std::vector<Sent> sent = opened.script->sent();
  REQUIRE(sent.size() == 3);
  CHECK(sent[1].message == request("2", "tools/call", call_params(declared)));
  // A new request, with a new id, the answer under the server's key, and the
  // state byte for byte.
  CHECK(sent[2].message ==
        request("3", "tools/call",
                call_params(declared,
                            R"({"who":{"action":"accept","content":{"name":"Ada"}}})",
                            state)));
  CHECK(sent[2].id == "3");
  CHECK(sent[2].method == "tools/call");
  CHECK(sent[2].name == "book");
}

TEST_CASE("every question of a round is answered, in the order of the keys") {
  mcp::ClientOptions with = answering_all();
  std::vector<std::string> order;
  std::optional<mcp::SamplingRequest> sampled;
  with.elicitation = [&order](const mcp::FormRequest&) {
    order.emplace_back("elicitation");
    return accept(R"({"name":"Ada"})");
  };
  with.sampling =
      [&](const mcp::SamplingRequest& request) -> mcp::Result<mcp::SamplingResult> {
    order.emplace_back("sampling");
    sampled = request;
    return said("hello");
  };
  with.roots = [&order] {
    order.emplace_back("roots");
    return std::vector<mcp::Root>{{.uri = "file:///work"},
                                  {.uri = "file:///home", .name = "home"}};
  };
  Opened opened = open_in(mcp::Era::modern, with);
  REQUIRE(opened.client.has_value());
  opened.script->respond =
      rounds({asking(form_question("b") + "," + sampling_question("a") + "," +
                     roots_question("c")),
              std::string{booked}});

  REQUIRE(book(*opened.client).has_value());
  CHECK(order == std::vector<std::string>{"sampling", "elicitation", "roots"});
  REQUIRE(sampled.has_value());
  CHECK(sampled->max_tokens == 10);
  REQUIRE(sampled->messages.size() == 1);
  CHECK(std::get<mcp::TextContent>(sampled->messages[0].content).text == "A word?");

  // No state was given, so none goes back.
  CHECK(
      opened.script->sent().back().message ==
      request("3", "tools/call",
              call_params(every_capability,
                          R"({"a":{"content":{"text":"hello","type":"text"},)"
                          R"("model":"m1","role":"assistant","stopReason":"endTurn"},)"
                          R"("b":{"action":"accept","content":{"name":"Ada"}},)"
                          R"("c":{"roots":[{"uri":"file:///work"},)"
                          R"({"name":"home","uri":"file:///home"}]}})")));
}

TEST_CASE("a round that only gives a state is sent again with the state alone") {
  Opened opened = open_in(mcp::Era::modern, answering_all());
  REQUIRE(opened.client.has_value());
  opened.script->respond = rounds({asking({}, R"("later")"), std::string{booked}});
  REQUIRE(book(*opened.client).has_value());
  CHECK(opened.script->sent().back().message ==
        request("3", "tools/call", call_params(every_capability, {}, R"("later")")));
}

TEST_CASE("each round is a request of its own, which carries the latest state") {
  Opened opened = open_in(mcp::Era::modern, answering_all());
  REQUIRE(opened.client.has_value());
  opened.script->respond = rounds({asking(form_question("q1"), R"("one")"),
                                   asking(roots_question("q2"), R"("two")"),
                                   asking(form_question("q3")), std::string{booked}});
  REQUIRE(book(*opened.client).has_value());

  const std::vector<Sent> sent = opened.script->sent();
  REQUIRE(sent.size() == 5);
  CHECK(sent[1].id == "2");
  CHECK(sent[2].message ==
        request("3", "tools/call",
                call_params(every_capability,
                            R"({"q1":{"action":"accept","content":{"name":"Ada"}}})",
                            R"("one")")));
  CHECK(
      sent[3].message ==
      request("4", "tools/call",
              call_params(every_capability,
                          R"({"q2":{"roots":[{"uri":"file:///work"}]}})", R"("two")")));
  // The last round gave no state, so this one sends none.
  CHECK(sent[4].message ==
        request("5", "tools/call",
                call_params(every_capability,
                            R"({"q3":{"action":"accept","content":{"name":"Ada"}}})")));
}

TEST_CASE("a server that asks round after round fails the call within the bound") {
  for (const std::size_t bound : {std::size_t{0}, std::size_t{3}}) {
    INFO(bound);
    mcp::ClientOptions with = answering_all();
    with.input_rounds = bound;
    Opened opened = open_in(mcp::Era::modern, with);
    REQUIRE(opened.client.has_value());
    opened.script->respond = [](Script& server, const Sent& sent) {
      server.deliver(support::result(sent.id, asking(form_question("again"))));
    };
    const mcp::Reply<mcp::ToolResult> called = book(*opened.client);
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().fault == mcp::Fault::protocol);
    CHECK(calls_sent(*opened.script) == bound + 1);
  }
}

TEST_CASE("a call stopped while its round is out is cancelled on the wire") {
  Opened opened = open_in(mcp::Era::modern, answering_all());
  REQUIRE(opened.client.has_value());
  // The second round is never answered.
  opened.script->respond = rounds({asking(form_question("who"))});
  std::stop_source stop;
  mcp::Client& client = *opened.client;
  auto calling = std::async(std::launch::async, [&] {
    return book(client, {.progress = {}, .stop = stop.get_token(), .timeout = {}});
  });
  const std::optional<Sent> again = opened.script->await("tools/call", 2);
  REQUIRE(again.has_value());
  const std::size_t before = opened.script->events().size();
  stop.request_stop();
  REQUIRE(calling.wait_for(patience) == std::future_status::ready);
  const mcp::Reply<mcp::ToolResult> called = calling.get();
  REQUIRE_FALSE(called.has_value());
  CHECK(called.error().fault == mcp::Fault::cancelled);
  std::vector<std::string> events = opened.script->events();
  events.erase(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(before));
  CHECK(events == std::vector<std::string>{"abandon " + again->id,
                                           "send notifications/cancelled"});
  CHECK(opened.script->sent().back().message.ends_with(R"("requestId":)" + again->id +
                                                       "}}"));
}

TEST_CASE("a call stopped while a handler runs is not sent again") {
  std::stop_source stop;
  mcp::ClientOptions with = answering_all();
  bool second = false;
  with.sampling =
      [&stop](const mcp::SamplingRequest&) -> mcp::Result<mcp::SamplingResult> {
    // The person stops the call while the model writes.
    stop.request_stop();
    return said("hello");
  };
  with.elicitation = [&second](const mcp::FormRequest&) {
    second = true;
    return accept("{}");
  };
  Opened opened = open_in(mcp::Era::modern, with);
  REQUIRE(opened.client.has_value());
  opened.script->respond = rounds(
      {asking(sampling_question("a") + "," + form_question("b")), std::string{booked}});
  const std::size_t before = opened.script->events().size();
  const mcp::Reply<mcp::ToolResult> called =
      book(*opened.client, {.progress = {}, .stop = stop.get_token(), .timeout = {}});
  REQUIRE_FALSE(called.has_value());
  CHECK(called.error().fault == mcp::Fault::cancelled);
  // The question after the stop is not asked, and nothing is left to cancel.
  CHECK_FALSE(second);
  CHECK(opened.script->events().size() == before + 1);
}

TEST_CASE("each round has the call's time limit, which a handler does not use up") {
  SECTION("a round that is not answered in time is cancelled") {
    mcp::ClientOptions with = answering_all();
    with.timeout = 100ms;
    Opened opened = open_in(mcp::Era::modern, with);
    REQUIRE(opened.client.has_value());
    opened.script->respond = rounds({asking(form_question("who"))});
    const mcp::Reply<mcp::ToolResult> called = book(*opened.client);
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().fault == mcp::Fault::timeout);
    const std::vector<std::string> events = opened.script->events();
    CHECK(events.back() == "send notifications/cancelled");
    CHECK(events[events.size() - 2] == "abandon 3");
  }
  SECTION("a person may take longer than the limit") {
    mcp::ClientOptions with = answering_all();
    with.timeout = 100ms;
    with.elicitation = [](const mcp::FormRequest&) {
      // The person takes their time, well past the call's limit.
      std::this_thread::sleep_for(400ms);
      return accept(R"({"name":"Ada"})");
    };
    Opened opened = open_in(mcp::Era::modern, with);
    REQUIRE(opened.client.has_value());
    opened.script->respond =
        rounds({asking(form_question("who")), std::string{booked}});
    const mcp::Reply<mcp::ToolResult> called = book(*opened.client);
    INFO((called ? std::string{} : called.error().message));
    CHECK(called.has_value());
  }
}

TEST_CASE("while a handler runs, the time of the other waiting calls stands still") {
  mcp::ClientOptions with = answering_all();
  with.timeout = 400ms;
  with.elicitation = [](const mcp::FormRequest&) {
    std::this_thread::sleep_for(1200ms);
    return accept(R"({"name":"Ada"})");
  };
  Opened opened = open_in(mcp::Era::modern, with);
  REQUIRE(opened.client.has_value());
  // `slow` is answered only once `book` has its answers.
  std::mutex mutex;
  std::string slow_id;
  bool asked = false;
  opened.script->respond = [&](Script& server, const Sent& sent) {
    if (sent.method != "tools/call") {
      return;
    }
    const std::scoped_lock lock{mutex};
    if (sent.name == "slow") {
      slow_id = sent.id;
    } else if (!std::exchange(asked, true)) {
      server.deliver(support::result(sent.id, asking(form_question("who"))));
    } else {
      server.deliver(support::result(sent.id, booked));
      server.deliver(support::result(slow_id, booked));
    }
  };
  mcp::Client& client = *opened.client;
  auto slow =
      std::async(std::launch::async, [&client] { return client.call_tool("slow"); });
  REQUIRE(opened.script->await("tools/call").has_value());
  CHECK(book(client).has_value());
  REQUIRE(slow.wait_for(patience) == std::future_status::ready);
  const mcp::Reply<mcp::ToolResult> waited = slow.get();
  INFO((waited ? std::string{} : waited.error().message));
  CHECK(waited.has_value());
}

TEST_CASE("under 2026-07-28 a question the client does not answer fails the call") {
  struct Case {
    std::string_view what;
    std::function<void(mcp::ClientOptions&)> setup;
    std::string question;
    mcp::Fault fault;
  };
  const std::vector<Case> cases{
      {"an elicitation handler that throws",
       [](mcp::ClientOptions& with) {
         with.elicitation = [](const mcp::FormRequest&) -> mcp::Answer<mcp::Json> {
           throw std::runtime_error{"the window would not open"};
         };
       },
       form_question("q"), mcp::Fault::unanswered},
      {"a roots handler that throws what is not an exception",
       [](mcp::ClientOptions& with) {
         with.roots = []() -> std::vector<mcp::Root> { throw 42; };
       },
       roots_question("q"), mcp::Fault::unanswered},
      {"a sampling handler that refuses",
       [](mcp::ClientOptions& with) {
         with.sampling =
             [](const mcp::SamplingRequest&) -> mcp::Result<mcp::SamplingResult> {
           return std::unexpected(mcp::tool_error("not with my model"));
         };
       },
       sampling_question("q"), mcp::Fault::unanswered},
      {"accepted content that is not an object",
       [](mcp::ClientOptions& with) {
         with.elicitation = [](const mcp::FormRequest&) { return accept("[1]"); };
       },
       form_question("q"), mcp::Fault::protocol},
      {"accepted content that is not JSON",
       [](mcp::ClientOptions& with) {
         with.elicitation = [](const mcp::FormRequest&) { return accept("{"); };
       },
       form_question("q"), mcp::Fault::protocol},
      {"accepted content with a field that is an object",
       [](mcp::ClientOptions& with) {
         with.elicitation = [](const mcp::FormRequest&) {
           return accept(R"({"name":{"first":"Ada"}})");
         };
       },
       form_question("q"), mcp::Fault::protocol},
      {"a sampled message without content",
       [](mcp::ClientOptions& with) {
         with.sampling =
             [](const mcp::SamplingRequest&) -> mcp::Result<mcp::SamplingResult> {
           return mcp::SamplingResult{.role = mcp::Role::assistant,
                                      .content = {},
                                      .model = "m1",
                                      .stop_reason = {}};
         };
       },
       sampling_question("q"), mcp::Fault::protocol},
      {"a sampled message that links a resource",
       [](mcp::ClientOptions& with) {
         with.sampling =
             [](const mcp::SamplingRequest&) -> mcp::Result<mcp::SamplingResult> {
           return mcp::SamplingResult{
               .role = mcp::Role::assistant,
               .content = {mcp::ResourceLink{.uri = "file:///a", .name = "a"}},
               .model = "m1",
               .stop_reason = {}};
         };
       },
       sampling_question("q"), mcp::Fault::protocol},
      {"a root that is not a file URI",
       [](mcp::ClientOptions& with) {
         with.roots = [] { return std::vector<mcp::Root>{{.uri = "/work"}}; };
       },
       roots_question("q"), mcp::Fault::protocol},
      {"a question the client declared no capability for",
       [](mcp::ClientOptions& with) { with.roots = {}; }, roots_question("q"),
       mcp::Fault::protocol},
      {"a method the client does not know", [](mcp::ClientOptions&) {},
       R"("q":{"method":"tasks/get","params":{}})", mcp::Fault::protocol},
      {"a form in url mode", [](mcp::ClientOptions&) {},
       R"("q":{"method":"elicitation/create","params":{"mode":"url",)"
       R"("message":"Sign in","url":"https://example.com"}})",
       mcp::Fault::protocol},
      {"a form without a message", [](mcp::ClientOptions&) {},
       R"("q":{"method":"elicitation/create","params":{"requestedSchema":{}}})",
       mcp::Fault::protocol},
      {"sampling with tools", [](mcp::ClientOptions&) {},
       R"("q":{"method":"sampling/createMessage","params":{"messages":[],)"
       R"("maxTokens":5,"tools":[]}})",
       mcp::Fault::protocol},
  };
  for (const Case& each : cases) {
    INFO(each.what);
    mcp::ClientOptions with = answering_all();
    each.setup(with);
    Opened opened = open_in(mcp::Era::modern, with);
    REQUIRE(opened.client.has_value());
    opened.script->respond = rounds({asking(each.question), std::string{booked}});
    const mcp::Reply<mcp::ToolResult> called = book(*opened.client);
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().fault == each.fault);
    INFO(called.error().message);
    CHECK_FALSE(called.error().message.empty());
    // Nothing more is sent.
    CHECK(calls_sent(*opened.script) == 1);
  }
}

TEST_CASE("a handler's failure says what it was") {
  mcp::ClientOptions with = answering_all();
  with.elicitation = [](const mcp::FormRequest&) -> mcp::Answer<mcp::Json> {
    throw std::runtime_error{"the window would not open"};
  };
  with.sampling = [](const mcp::SamplingRequest&) -> mcp::Result<mcp::SamplingResult> {
    return std::unexpected(mcp::tool_error("not with my model"));
  };
  Opened opened = open_in(mcp::Era::modern, with);
  REQUIRE(opened.client.has_value());
  opened.script->respond =
      rounds({asking(form_question("q")), asking(sampling_question("q"))});
  const mcp::Reply<mcp::ToolResult> threw = book(*opened.client);
  REQUIRE_FALSE(threw.has_value());
  CHECK(threw.error().message ==
        "the elicitation handler threw: the window would not open");
  const mcp::Reply<mcp::ToolResult> refused = book(*opened.client);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().message == "the sampling handler refused: not with my model");
}

TEST_CASE("an input_required result that is not valid fails with Fault::protocol") {
  Opened opened = open_in(mcp::Era::modern, answering_all());
  REQUIRE(opened.client.has_value());
  for (const std::string_view result :
       {R"({"resultType":"input_required"})",
        R"({"inputRequests":{},"resultType":"input_required"})",
        R"({"inputRequests":[],"resultType":"input_required"})",
        R"({"inputRequests":{"q":1},"resultType":"input_required"})",
        R"({"inputRequests":{"q":{"params":{}}},"resultType":"input_required"})",
        R"({"inputRequests":{"q":{"method":"roots/list","params":[]}},)"
        R"("resultType":"input_required"})",
        R"({"requestState":5,"resultType":"input_required"})"}) {
    INFO(result);
    opened.script->respond = rounds({std::string{result}, std::string{booked}});
    const mcp::Reply<mcp::ToolResult> called = book(*opened.client);
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().fault == mcp::Fault::protocol);
  }
  // A request that cannot take input.
  opened.script->respond = [](Script& server, const Sent& sent) {
    server.deliver(support::result(sent.id, asking(form_question("q"))));
  };
  const mcp::Reply<std::vector<mcp::Tool>> tools = opened.client->list_tools();
  REQUIRE_FALSE(tools.has_value());
  CHECK(tools.error().fault == mcp::Fault::protocol);
}

// Defaults.

TEST_CASE("an accepted form gets the defaults of the fields it leaves out") {
  const std::string schema =
      R"({"type":"object","properties":{"name":{"type":"string","default":"Ada"},)"
      R"("seats":{"type":"integer","default": 2},"vip":{"type":"boolean","default":false},)"
      R"("color":{"type":"string","enum":["red","blue"],"default":"blue"},)"
      R"("tags":{"type":"array","items":{"type":"string","enum":["a","b"]},)"
      R"("default":[ "a" ]},"note":{"type":"string"}}})";
  struct Case {
    mcp::Answer<mcp::Json> given;
    std::string_view sent;
  };
  const std::vector<Case> cases{
      {accept(R"({"name":"Bo"})"),
       R"({"action":"accept","content":{"name":"Bo","color":"blue","seats":2,)"
       R"("tags":["a"],"vip":false}})"},
      // What the person entered stays as it is, falsy or not.
      {accept(R"({"name":"","seats":0,"vip":true,"color":"red","tags":[]})"),
       R"({"action":"accept","content":{"name":"","seats":0,"vip":true,)"
       R"("color":"red","tags":[]}})"},
      {{.action = mcp::Action::accept, .content = std::nullopt},
       R"({"action":"accept","content":{"color":"blue","name":"Ada","seats":2,)"
       R"("tags":["a"],"vip":false}})"},
      // A form declined or cancelled sends no content, and so no defaults.
      {{.action = mcp::Action::decline, .content = mcp::Json{R"({"name":"Bo"})"}},
       R"({"action":"decline"})"},
      {{.action = mcp::Action::cancel, .content = std::nullopt},
       R"({"action":"cancel"})"},
  };
  for (const Case& each : cases) {
    INFO(each.sent);
    mcp::ClientOptions with = options();
    with.elicitation = [&each](const mcp::FormRequest&) { return each.given; };

    // Under 2026-07-28, in the answers of the next round.
    Opened modern = open_in(mcp::Era::modern, with);
    REQUIRE(modern.client.has_value());
    modern.script->respond = rounds(
        {asking(R"("f":{"method":"elicitation/create","params":{"message":"Book",)"
                R"("requestedSchema":)" +
                schema + "}}"),
         std::string{booked}});
    REQUIRE(book(*modern.client).has_value());
    CHECK(modern.script->sent().back().message.find(R"("inputResponses":{"f":)" +
                                                    std::string{each.sent} + "}") !=
          std::string::npos);

    // In a session, as the result of the server's request.
    Opened legacy = open_in(mcp::Era::legacy, with);
    REQUIRE(legacy.client.has_value());
    legacy.script->deliver(
        server_request("7", "elicitation/create",
                       R"({"message":"Book","requestedSchema":)" + schema + "}"));
    CHECK(response_to(*legacy.script, "7") ==
          R"({"id":7,"jsonrpc":"2.0","result":)" + std::string{each.sent} + "}");
  }
}

// Requests of a handshake server.

TEST_CASE("a session's requests are answered by the handlers") {
  mcp::ClientOptions with = answering_all();
  std::optional<mcp::SamplingRequest> sampled;
  with.sampling =
      [&sampled](
          const mcp::SamplingRequest& request) -> mcp::Result<mcp::SamplingResult> {
    sampled = request;
    return mcp::SamplingResult{
        .role = mcp::Role::assistant,
        .content = {mcp::TextContent{.text = "a"}, mcp::TextContent{.text = "b"}},
        .model = "m1",
        .stop_reason = {}};
  };
  Opened opened = open_in(mcp::Era::legacy, with);
  REQUIRE(opened.client.has_value());

  opened.script->deliver(server_request("\"e\"", "elicitation/create", who_params));
  CHECK(
      response_to(*opened.script, "\"e\"") ==
      R"({"id":"e","jsonrpc":"2.0","result":{"action":"accept","content":{"name":"Ada"}}})");

  // What a request has beyond what SamplingRequest holds is passed over, and
  // a message of several blocks is one message per block.
  opened.script->deliver(server_request(
      "2", "sampling/createMessage",
      R"({"messages":[{"role":"user","content":[{"type":"text","text":"one"},)"
      R"({"type":"text","text":"two"}]},{"role":"assistant","content":{"type":"text",)"
      R"("text":"three"},"_meta":{}}],"maxTokens":10,"systemPrompt":"Be brief.",)"
      R"("temperature":0.5,"stopSequences":["."],"modelPreferences":{"hints":[]},)"
      R"("includeContext":"none","metadata":{}})"));
  // 2025-11-25 takes several blocks.
  CHECK(response_to(*opened.script, "2") ==
        R"({"id":2,"jsonrpc":"2.0","result":{"content":[{"text":"a","type":"text"},)"
        R"({"text":"b","type":"text"}],"model":"m1","role":"assistant"}})");
  REQUIRE(sampled.has_value());
  REQUIRE(sampled->messages.size() == 3);
  CHECK(sampled->messages[1].role == mcp::Role::user);
  CHECK(std::get<mcp::TextContent>(sampled->messages[1].content).text == "two");
  CHECK(sampled->messages[2].role == mcp::Role::assistant);
  CHECK(sampled->max_tokens == 10);
  CHECK(sampled->system_prompt == "Be brief.");
  CHECK(sampled->temperature == 0.5);
  CHECK(sampled->stop_sequences == std::vector<std::string>{"."});

  opened.script->deliver(server_request("3", "roots/list", "{}"));
  CHECK(response_to(*opened.script, "3") ==
        R"({"id":3,"jsonrpc":"2.0","result":{"roots":[{"uri":"file:///work"}]}})");
}

TEST_CASE("a 2025-06-18 session's forms have no mode, and its messages one block") {
  mcp::ClientOptions with = answering_all();
  with.era = mcp::Era::legacy;
  bool two = false;
  with.sampling =
      [&two](const mcp::SamplingRequest&) -> mcp::Result<mcp::SamplingResult> {
    mcp::SamplingResult result = said("a");
    if (two) {
      result.content.emplace_back(mcp::TextContent{.text = "b"});
    }
    return result;
  };
  const auto script = std::make_shared<Script>();
  script->respond = answering({{"initialize", initialized("2025-06-18")}});
  const mcp::Reply<mcp::Client> client = open(script, with);
  REQUIRE(client.has_value());
  script->respond = {};

  script->deliver(server_request(
      "1", "elicitation/create",
      R"({"message":"Who?","requestedSchema":{"type":"object","properties":{}}})"));
  CHECK(
      response_to(*script, "1") ==
      R"({"id":1,"jsonrpc":"2.0","result":{"action":"accept","content":{"name":"Ada"}}})");
  const std::string sampling =
      R"({"messages":[{"role":"user","content":{"type":"text","text":"?"}}],"maxTokens":5})";
  script->deliver(server_request("2", "sampling/createMessage", sampling));
  CHECK(response_to(*script, "2") ==
        R"({"id":2,"jsonrpc":"2.0","result":{"content":{"text":"a","type":"text"},)"
        R"("model":"m1","role":"assistant","stopReason":"endTurn"}})");
  two = true;
  script->deliver(server_request("3", "sampling/createMessage", sampling));
  CHECK(
      response_to(*script, "3") ==
      R"({"error":{"code":-32603,"message":"Internal error"},"id":3,"jsonrpc":"2.0"})");
}

TEST_CASE("a session's request the client does not answer gets a JSON-RPC error") {
  mcp::ClientOptions with = answering_all();
  with.elicitation = [](const mcp::FormRequest& form) -> mcp::Answer<mcp::Json> {
    if (form.message == "throw") {
      throw std::runtime_error{"a detail the server is not told"};
    }
    return accept("[1]");
  };
  with.sampling = [](const mcp::SamplingRequest&) -> mcp::Result<mcp::SamplingResult> {
    return std::unexpected(mcp::tool_error("not with my model"));
  };
  with.roots = {};
  Opened opened = open_in(mcp::Era::legacy, with);
  REQUIRE(opened.client.has_value());
  const auto refused = [](const std::string_view id, const int code,
                          const std::string_view message) {
    return R"({"error":{"code":)" + std::to_string(code) + R"(,"message":")" +
           std::string{message} + R"("},"id":)" + std::string{id} +
           R"(,"jsonrpc":"2.0"})";
  };
  struct Case {
    std::string id;
    std::string method;
    std::string params;
    std::string answer;
  };
  const std::vector<Case> cases{
      {"1", "elicitation/create",
       R"({"message":"throw","requestedSchema":{"type":"object"}})",
       refused("1", -32603, "Internal error")},
      {"2", "elicitation/create", std::string{who_params},
       refused("2", -32603, "Internal error")},
      {"3", "elicitation/create",
       R"({"mode":"url","message":"Sign in","url":"https://example.com","elicitationId":"x"})",
       refused("3", -32602,
               "Invalid params: this client declared elicitation in form mode only")},
      {"4", "elicitation/create", R"({"requestedSchema":{}})",
       refused("4", -32602,
               "Invalid params: a form needs a message and a requestedSchema object")},
      {"5", "sampling/createMessage",
       R"({"messages":[{"role":"user","content":{"type":"text","text":"?"}}],"maxTokens":5})",
       refused("5", -1, "not with my model")},
      {"6", "sampling/createMessage", R"({"messages":[],"maxTokens":5,"tools":[]})",
       refused("6", -32602,
               "Invalid params: this client did not declare sampling with tools")},
      {"7", "roots/list", "{}", refused("7", -32601, "Method not found: roots/list")},
      {"8", "tasks/get", "{}", refused("8", -32601, "Method not found: tasks/get")},
      {"9", "elicitation/create", "[]",
       refused("9", -32602, "Invalid params: params must be an object")},
  };
  for (const Case& each : cases) {
    INFO(each.id);
    opened.script->deliver(server_request(each.id, each.method, each.params));
    CHECK(response_to(*opened.script, each.id) == each.answer);
  }
  opened.script->deliver(R"({"jsonrpc":"2.0","id":10,"method":"ping"})");
  CHECK(response_to(*opened.script, "10") ==
        R"({"id":10,"jsonrpc":"2.0","result":{}})");
}

TEST_CASE("a modern server's request is not a question, whatever handlers are set") {
  Opened opened = open_in(mcp::Era::modern, answering_all());
  REQUIRE(opened.client.has_value());
  opened.script->deliver(server_request("1", "elicitation/create", who_params));
  CHECK(response_to(*opened.script, "1") ==
        R"({"error":{"code":-32601,"message":"Method not found: elicitation/create"},)"
        R"("id":1,"jsonrpc":"2.0"})");
}

TEST_CASE("a handler runs on a thread of its own, and may call the client") {
  mcp::Client* self = nullptr;
  std::thread::id ran_on;
  mcp::ClientOptions with = answering_all();
  with.elicitation = [&](const mcp::FormRequest&) {
    ran_on = std::this_thread::get_id();
    const mcp::Reply<std::vector<mcp::Tool>> tools = self->list_tools();
    return accept(R"({"tools":)" + std::to_string(tools ? tools->size() : 99) + "}");
  };
  Opened opened = open_in(mcp::Era::legacy, with);
  REQUIRE(opened.client.has_value());
  self = &*opened.client;
  opened.script->respond =
      answering({{"tools/list", R"({"tools":[{"name":"a","inputSchema":{}}]})"}});

  opened.script->deliver(server_request("1", "elicitation/create", who_params));
  CHECK(
      response_to(*opened.script, "1") ==
      R"({"id":1,"jsonrpc":"2.0","result":{"action":"accept","content":{"tools":1}}})");
  CHECK(ran_on != std::this_thread::get_id());
}

TEST_CASE("a session's requests are answered side by side") {
  std::mutex mutex;
  std::condition_variable met;
  int arrived = 0;
  mcp::ClientOptions with = answering_all();
  with.elicitation = [&](const mcp::FormRequest&) {
    std::unique_lock lock{mutex};
    ++arrived;
    met.notify_all();
    // Each waits for the other, which only a second thread lets come.
    const bool together = met.wait_for(lock, patience, [&] { return arrived == 2; });
    return accept(together ? R"({"name":"together"})" : R"({"name":"alone"})");
  };
  Opened opened = open_in(mcp::Era::legacy, with);
  REQUIRE(opened.client.has_value());
  opened.script->deliver(server_request("1", "elicitation/create", who_params));
  opened.script->deliver(server_request("2", "elicitation/create", who_params));
  for (const std::string_view id : {"1", "2"}) {
    CHECK(response_to(*opened.script, id) ==
          R"({"id":)" + std::string{id} +
              R"(,"jsonrpc":"2.0","result":{"action":"accept","content":)"
              R"({"name":"together"}}})");
  }
}

TEST_CASE("in a session, a waiting call's time stands still while a handler runs") {
  mcp::ClientOptions with = answering_all();
  with.timeout = 400ms;
  with.elicitation = [](const mcp::FormRequest&) {
    // The person takes their time, well past the call's limit.
    std::this_thread::sleep_for(1200ms);
    return accept(R"({"name":"Ada"})");
  };
  Opened opened = open_in(mcp::Era::legacy, with);
  REQUIRE(opened.client.has_value());
  std::mutex mutex;
  std::string call_id;
  opened.script->respond = [&](Script& server, const Sent& sent) {
    const std::scoped_lock lock{mutex};
    if (sent.method == "tools/call") {
      // The tool asks the person before it answers.
      call_id = sent.id;
      server.deliver(server_request("\"ask\"", "elicitation/create", who_params));
    } else if (sent.method.empty() &&
               sent.message.find(R"("id":"ask")") != std::string::npos) {
      server.deliver(support::result(call_id, booked));
    }
  };
  const mcp::Reply<mcp::ToolResult> called = book(*opened.client);
  INFO((called ? std::string{} : called.error().message));
  CHECK(called.has_value());
}

TEST_CASE("destroying a client waits for the handlers that run") {
  std::binary_semaphore started{0};
  std::binary_semaphore release{0};
  mcp::ClientOptions with = answering_all();
  with.elicitation = [&](const mcp::FormRequest&) {
    started.release();
    static_cast<void>(release.try_acquire_for(patience));
    return accept(R"({"name":"late"})");
  };
  Opened opened = open_in(mcp::Era::legacy, with);
  REQUIRE(opened.client.has_value());
  opened.script->deliver(server_request("1", "elicitation/create", who_params));
  REQUIRE(started.try_acquire_for(patience));

  auto closing = std::async(std::launch::async, [&opened] {
    opened.client = std::unexpected(mcp::ClientError{});
  });
  // The destructor does not return while the handler runs.
  CHECK(closing.wait_for(100ms) == std::future_status::timeout);
  release.release();
  REQUIRE(closing.wait_for(patience) == std::future_status::ready);
  // The client was closed by the time the person answered, so the answer is
  // not sent, and the channel closed last.
  for (const Sent& sent : opened.script->sent()) {
    CHECK(sent.message.find("late") == std::string::npos);
  }
  CHECK(opened.script->events().back() == "close");
}
