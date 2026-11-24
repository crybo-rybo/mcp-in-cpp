// Asking the client while a call runs: elicitation, sampling and roots, as
// rounds of input_required under 2026-07-28, and as requests of the server's
// in a handshake session.
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <mcp/mcp.hpp>
#include <mutex>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using support::called;
using support::error;
using support::legacy;
using support::text_content;

namespace asking {

struct Guest {
  [[= mcp::description{"Who the table is for"}]] std::string name;
  std::int32_t seats{1};
};

[[= mcp::tool{"Book a table"}]] inline mcp::Result<std::string>
    book(std::string place, mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<Guest>> guest = ask.form<Guest>("Who is it for?");
  if (!guest) {
    return std::unexpected(guest.error());
  }
  if (guest->action != mcp::Action::accept) {
    return std::string{"not booked"};
  }
  return "booked " + std::to_string(guest->content->seats) + " at " + place + " for " +
         guest->content->name;
}

struct Name {
  std::string name;
};

struct Color {
  std::string color;
};

[[= mcp::tool{
    "Ask two questions, one after the other"}]] inline mcp::Result<std::string>
    two_steps(mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<Name>> name = ask.form<Name>("Your name?", "step1");
  if (!name) {
    return std::unexpected(name.error());
  }
  mcp::Result<mcp::Answer<Color>> color = ask.form<Color>("Your color?", "step2");
  if (!color) {
    return std::unexpected(color.error());
  }
  return name->content->name + " likes " + color->content->color;
}

[[= mcp::tool{"Ask the model for a word"}]] inline mcp::Result<std::string>
    word(mcp::Sampling sampling) {
  mcp::Result<mcp::SamplingResult> answer = sampling.create(
      {.messages = {{.role = mcp::Role::user, .content = mcp::TextContent{"A word?"}}},
       .max_tokens = 10});
  if (!answer) {
    return std::unexpected(answer.error());
  }
  return std::get<mcp::TextContent>(answer->content.at(0)).text + " from " +
         answer->model;
}

[[= mcp::tool{"Count the client's roots"}]] inline mcp::Result<std::string>
    count_roots(mcp::Roots roots) {
  mcp::Result<std::vector<mcp::Root>> listed = roots.list();
  if (!listed) {
    return std::unexpected(listed.error());
  }
  return std::to_string(listed->size()) + " roots";
}

} // namespace asking

namespace {

mcp::Server asking_server() {
  mcp::Server server{"sample", "0.0.1"};
  server.add<^^asking>();
  return server;
}

// A tools/call of 2026-07-28 from a client with `capabilities`, and with
// `rest` further members of its params.
std::string call(const std::string_view id, const std::string_view name,
                 const std::string_view capabilities,
                 const std::string_view rest = {}) {
  std::string text{R"({"jsonrpc":"2.0","id":)"};
  text += id;
  text += R"(,"method":"tools/call","params":{"name":")";
  text += name;
  text += R"(","_meta":{"io.modelcontextprotocol/clientCapabilities":)";
  text += capabilities;
  text += R"(,"io.modelcontextprotocol/protocolVersion":"2026-07-28"})";
  if (!rest.empty()) {
    text += ',';
    text += rest;
  }
  text += "}}";
  return text;
}

// A member of a response's result, as JSON text.
std::string result_member(const std::optional<std::string>& response,
                          const std::string_view key) {
  REQUIRE(response.has_value());
  const scrydantic::Result<scrydantic::JsonView> document =
      scrydantic::JsonView::parse(*response);
  REQUIRE(document);
  const std::optional<scrydantic::JsonView> result = document->find("result");
  REQUIRE(result.has_value());
  const std::optional<scrydantic::JsonView> member = result->find(key);
  return member.has_value() ? member->to_json().text : std::string{};
}

constexpr std::string_view everything =
    R"({"elicitation":{},"roots":{},"sampling":{}})";

} // namespace

TEST_CASE("a question without an answer ends the round with input_required") {
  const mcp::Server server = asking_server();
  mcp::Connection connection{server};

  const std::optional<std::string> first = connection.handle(
      call("1", "book", everything, R"("arguments":{"place":"Lima"})"));
  CHECK(result_member(first, "resultType") == R"("input_required")");
  CHECK(
      result_member(first, "inputRequests") ==
      R"({"input-1":{"method":"elicitation/create","params":{"message":"Who is it for?",)"
      R"("mode":"form","requestedSchema":{"properties":{"name":{"description":)"
      R"("Who the table is for","type":"string"},"seats":{"default":1,"maximum":2147483647,)"
      R"("minimum":-2147483648,"type":"integer"}},"required":["name"],"type":"object"}}}})");
  const std::string state = result_member(first, "requestState");
  REQUIRE(state.size() > 2);

  // The client asks the person and sends the call again with the answer.
  std::string retry{
      R"("arguments":{"place":"Lima"},"inputResponses":{"input-1":)"
      R"({"action":"accept","content":{"name":"Ada","seats":2}}},"requestState":)"};
  retry += state;
  CHECK(connection.handle(call("2", "book", everything, retry)) ==
        called("2", text_content("booked 2 at Lima for Ada", false),
               R"("booked 2 at Lima for Ada")"));

  // A form the person declined.
  CHECK(connection.handle(call("3", "book", everything,
                               R"("arguments":{"place":"Lima"},"inputResponses":)"
                               R"({"input-1":{"action":"decline"}})")) ==
        called("3", text_content("not booked", false), R"("not booked")"));
}

TEST_CASE("each round's answers travel to the next in the signed state") {
  const mcp::Server server = asking_server();
  mcp::Connection connection{server};

  const std::optional<std::string> first =
      connection.handle(call("1", "two_steps", everything));
  CHECK(result_member(first, "inputRequests").starts_with(R"({"step1":)"));

  std::string second_round{R"("inputResponses":{"step1":{"action":"accept","content":)"
                           R"({"name":"Ada"}}},"requestState":)"};
  second_round += result_member(first, "requestState");
  const std::optional<std::string> second =
      connection.handle(call("2", "two_steps", everything, second_round));
  CHECK(result_member(second, "inputRequests").starts_with(R"({"step2":)"));

  // The last round brings only the last answer; the first is in the state.
  std::string third_round{R"("inputResponses":{"step2":{"action":"accept","content":)"
                          R"({"color":"blue"}}},"requestState":)"};
  third_round += result_member(second, "requestState");
  CHECK(connection.handle(call("3", "two_steps", everything, third_round)) ==
        called("3", text_content("Ada likes blue", false), R"("Ada likes blue")"));
}

TEST_CASE("a state that was changed, or that belongs to another call, is refused") {
  const mcp::Server server = asking_server();
  mcp::Connection connection{server};
  const std::optional<std::string> first = connection.handle(
      call("1", "book", everything, R"("arguments":{"place":"Lima"})"));
  std::string state = result_member(first, "requestState");
  const std::string refused =
      R"({"error":{"code":-32602,"data":{"reason":"invalid_request_state"},)"
      R"("message":"Invalid or expired requestState"},"id":2,"jsonrpc":"2.0"})";

  // The same state for other arguments.
  CHECK(connection.handle(call("2", "book", everything,
                               R"("arguments":{"place":"Oslo"},"requestState":)" +
                                   state)) == refused);

  // One character of the state changed.
  state[state.size() / 2] = state[state.size() / 2] == 'A' ? 'B' : 'A';
  CHECK(connection.handle(call("2", "book", everything,
                               R"("arguments":{"place":"Lima"},"requestState":)" +
                                   state)) == refused);

  // A state from another server, whose key differs.
  const mcp::Server other = asking_server();
  mcp::Connection elsewhere{other};
  const std::string foreign =
      result_member(elsewhere.handle(call("1", "book", everything,
                                          R"("arguments":{"place":"Lima"})")),
                    "requestState");
  CHECK(connection.handle(call("2", "book", everything,
                               R"("arguments":{"place":"Lima"},"requestState":)" +
                                   foreign)) == refused);
}

TEST_CASE("servers that share a key accept each other's state") {
  mcp::Server one = asking_server();
  mcp::Server two = asking_server();
  one.request_state_key("a shared secret of enough length..");
  two.request_state_key("a shared secret of enough length..");
  mcp::Connection first{one};
  mcp::Connection second{two};

  const std::string state = result_member(
      first.handle(call("1", "book", everything, R"("arguments":{"place":"Lima"})")),
      "requestState");
  CHECK(second.handle(call(
            "2", "book", everything,
            R"("arguments":{"place":"Lima"},"inputResponses":{"input-1":)"
            R"({"action":"accept","content":{"name":"Bo"}}},"requestState":)" +
                state)) == called("2", text_content("booked 1 at Lima for Bo", false),
                                  R"("booked 1 at Lima for Bo")"));
}

TEST_CASE("a client that did not declare a capability is not asked") {
  const mcp::Server server = asking_server();
  mcp::Connection connection{server};

  CHECK(
      connection.handle(call("1", "book", "{}", R"("arguments":{"place":"Lima"})")) ==
      R"({"error":{"code":-32021,"data":{"requiredCapabilities":{"elicitation":{"form":{}}}},)"
      R"("message":"Missing required client capability"},"id":1,"jsonrpc":"2.0"})");
  CHECK(connection.handle(call("2", "word", R"({"elicitation":{}})")) ==
        R"({"error":{"code":-32021,"data":{"requiredCapabilities":{"sampling":{}}},)"
        R"("message":"Missing required client capability"},"id":2,"jsonrpc":"2.0"})");
  // A client that can open only URLs cannot show a form.
  CHECK(connection
            .handle(call("3", "book", R"({"elicitation":{"url":{}}})",
                         R"("arguments":{"place":"Lima"})"))
            ->starts_with(R"({"error":{"code":-32021)"));
}

TEST_CASE("sampling and roots are asked in rounds too") {
  const mcp::Server server = asking_server();
  mcp::Connection connection{server};

  const std::optional<std::string> asked =
      connection.handle(call("1", "word", everything));
  CHECK(
      result_member(asked, "inputRequests") ==
      R"({"input-1":{"method":"sampling/createMessage","params":{"maxTokens":10,)"
      R"("messages":[{"content":{"text":"A word?","type":"text"},"role":"user"}]}}})");
  CHECK(
      connection.handle(call("2", "word", everything,
                             R"("inputResponses":{"input-1":{"role":"assistant",)"
                             R"("content":{"type":"text","text":"hello"},"model":"m1",)"
                             R"("stopReason":"endTurn"}})")) ==
      called("2", text_content("hello from m1", false), R"("hello from m1")"));

  CHECK(result_member(connection.handle(call("3", "count_roots", everything)),
                      "inputRequests") ==
        R"({"input-1":{"method":"roots/list","params":{}}})");
  CHECK(connection.handle(
            call("4", "count_roots", everything,
                 R"("inputResponses":{"input-1":{"roots":[{"uri":"file:///a"},)"
                 R"({"uri":"file:///b","name":"b"}]}})")) ==
        called("4", text_content("2 roots", false), R"("2 roots")"));
}

TEST_CASE("in a session the server asks the client and the call waits for its answer") {
  const mcp::Server server = asking_server();
  mcp::Connection connection{server};
  REQUIRE(connection
              .handle(legacy("0", "initialize",
                             R"({"protocolVersion":"2025-11-25","capabilities":)"
                             R"({"elicitation":{},"sampling":{}},"clientInfo":)"
                             R"({"name":"test","version":"1"}})"))
              .has_value());

  std::mutex mutex;
  std::vector<std::string> sent;
  std::counting_semaphore<> asked{0};
  const mcp::Notify notify = [&](const std::string_view message) {
    {
      const std::scoped_lock lock{mutex};
      sent.emplace_back(message);
    }
    asked.release();
  };

  std::optional<std::string> response;
  std::jthread calling{[&] {
    response = connection.handle(
        legacy("1", "tools/call", R"({"name":"book","arguments":{"place":"Lima"}})"),
        notify);
  }};
  REQUIRE(asked.try_acquire_for(std::chrono::seconds{10}));
  {
    const std::scoped_lock lock{mutex};
    REQUIRE(sent.size() == 1);
    CHECK(
        sent[0].starts_with(R"({"id":1,"jsonrpc":"2.0","method":"elicitation/create",)"
                            R"("params":{"mode":"form","message":"Who is it for?",)"));
  }
  // The client's response comes in as a message of its own.
  CHECK_FALSE(connection
                  .handle(R"({"jsonrpc":"2.0","id":1,"result":{"action":"accept",)"
                          R"("content":{"name":"Cy","seats":4}}})")
                  .has_value());
  calling.join();
  CHECK(response == support::result("1",
                                    R"({"content":[{"text":"booked 4 at Lima for Cy",)"
                                    R"("type":"text"}],"isError":false})"));

  // A client without roots is not asked for them.
  CHECK(
      connection.handle(legacy("2", "tools/call", R"({"name":"count_roots"})"), notify)
          ->starts_with(
              R"({"error":{"code":-32021,"data":{"requiredCapabilities":{"roots":{}}})"));
}

TEST_CASE("a session's question ends with the call when the client cancels it") {
  const mcp::Server server = asking_server();
  mcp::Connection connection{server};
  REQUIRE(connection
              .handle(legacy(
                  "0", "initialize",
                  R"({"protocolVersion":"2025-06-18","capabilities":)"
                  R"({"elicitation":{}},"clientInfo":{"name":"t","version":"1"}})"))
              .has_value());
  std::counting_semaphore<> asked{0};
  std::string question;
  const mcp::Notify notify = [&](const std::string_view message) {
    question = message;
    asked.release();
  };
  std::stop_source stop;
  std::optional<std::string> response;
  std::jthread calling{[&] {
    response = connection.handle(
        legacy("1", "tools/call", R"({"name":"book","arguments":{"place":"Lima"}})"),
        notify, stop.get_token());
  }};
  REQUIRE(asked.try_acquire_for(std::chrono::seconds{10}));
  // 2025-06-18 has no form mode.
  CHECK(question.find(R"("mode")") == std::string::npos);
  stop.request_stop();
  calling.join();
  CHECK(response == support::result("1",
                                    R"({"content":[{"text":"the call was cancelled",)"
                                    R"("type":"text"}],"isError":true})"));
}

TEST_CASE("an answer that is not a response to its question is invalid params") {
  const mcp::Server server = asking_server();
  mcp::Connection connection{server};
  CHECK(connection.handle(
            call("1", "book", everything,
                 R"("arguments":{"place":"Lima"},"inputResponses":{"input-1":42})")) ==
        error("1", -32602,
              R"(Invalid params: inputResponses[\"input-1\"] must be an object)"));
  CHECK(
      connection
          .handle(call("2", "book", everything,
                       R"("arguments":{"place":"Lima"},"inputResponses":)"
                       R"({"input-1":{"action":"maybe"}})"))
          ->starts_with(
              R"({"error":{"code":-32602,"message":"Invalid params: the answer to a form)"));
}

TEST_CASE("an answer the state carries cannot be changed by a later round") {
  const mcp::Server server = asking_server();
  mcp::Connection connection{server};
  const std::optional<std::string> first =
      connection.handle(call("1", "two_steps", everything));
  std::string second_round{R"("inputResponses":{"step1":{"action":"accept","content":)"
                           R"({"name":"Ada"}}},"requestState":)"};
  second_round += result_member(first, "requestState");
  const std::optional<std::string> second =
      connection.handle(call("2", "two_steps", everything, second_round));

  // The last round tries to give step1 another answer.
  std::string third_round{R"("inputResponses":{"step1":{"action":"accept","content":)"
                          R"({"name":"Eve"}},"step2":{"action":"accept","content":)"
                          R"({"color":"red"}}},"requestState":)"};
  third_round += result_member(second, "requestState");
  CHECK(connection.handle(call("3", "two_steps", everything, third_round)) ==
        called("3", text_content("Ada likes red", false), R"("Ada likes red")"));
}
