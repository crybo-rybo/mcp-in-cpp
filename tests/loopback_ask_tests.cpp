// A client that answers what a server in the same process asks while a call
// runs, through the loopback channel: forms, the model, and the roots, in
// both eras.
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <map>
#include <mcp/client.hpp>
#include <mcp/mcp.hpp>
#include <mutex>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace desk {

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
    return std::string{guest->action == mcp::Action::decline ? "declined"
                                                             : "cancelled"};
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

// Asks both questions before it looks at either, so that under 2026-07-28
// both come in one round.
[[= mcp::tool{"Ask two questions at once"}]] inline mcp::Result<std::string>
    both(mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<Name>> name = ask.form<Name>("Your name?", "name");
  mcp::Result<mcp::Answer<Color>> color = ask.form<Color>("Your color?", "color");
  if (!name) {
    return std::unexpected(name.error());
  }
  if (!color) {
    return std::unexpected(color.error());
  }
  return name->content->name + " likes " + color->content->color;
}

// Asks the second question only once it has the first answer, so that under
// 2026-07-28 they come in two rounds.
[[= mcp::tool{"Ask one question after another"}]] inline mcp::Result<std::string>
    in_turn(mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<Name>> name = ask.form<Name>("Your name?", "name");
  if (!name) {
    return std::unexpected(name.error());
  }
  mcp::Result<mcp::Answer<Color>> color = ask.form<Color>("Your color?", "color");
  if (!color) {
    return std::unexpected(color.error());
  }
  return name->content->name + " likes " + color->content->color;
}

// Returns the content the client sent, as it reached the server.
[[= mcp::tool{"Fill in a form with defaults"}]] inline mcp::Result<std::string>
    table(mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<scrydantic::Json>> answer = ask.form(
      "Your table?",
      scrydantic::Json{R"({"type":"object","properties":{"name":{"type":"string"},)"
                       R"("seats":{"type":"integer","default":4},)"
                       R"("window":{"type":"boolean","default":true}},)"
                       R"("required":["name"]})"});
  if (!answer) {
    return std::unexpected(answer.error());
  }
  return answer->content.has_value() ? answer->content->text : std::string{"none"};
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

[[= mcp::tool{"Say what the client can be asked"}]] inline std::string
    abilities(mcp::Elicitation ask, mcp::Sampling sampling, mcp::Roots roots) {
  return std::string{ask ? "elicitation" : "-"} + " " + (sampling ? "sampling" : "-") +
         " " + (roots ? "roots" : "-");
}

} // namespace desk

namespace {

constexpr std::array<mcp::Era, 3> eras{mcp::Era::modern, mcp::Era::legacy,
                                       mcp::Era::automatic};

mcp::Server desk_server() {
  mcp::Server server{"desk", "0.0.1"};
  server.add<^^desk>();
  return server;
}

// A person who answers each form as its message says, and counts them.
struct Person {
  std::mutex mutex;
  std::map<std::string, int, std::less<>> asked;
  mcp::Action action{mcp::Action::accept};

  mcp::Answer<scrydantic::Json> answer(const mcp::FormRequest& form) {
    const std::scoped_lock lock{mutex};
    ++asked[form.message];
    const std::map<std::string_view, std::string_view> contents{
        {"Who is it for?", R"({"name":"Ada","seats":2})"},
        {"Your name?", R"({"name":"Ada"})"},
        {"Your color?", R"({"color":"blue"})"},
        {"Your table?", R"({"name":"Ada"})"}};
    const auto found = contents.find(form.message);
    if (action != mcp::Action::accept || found == contents.end()) {
      return {.action = action == mcp::Action::accept ? mcp::Action::cancel : action,
              .content = std::nullopt};
    }
    return {.action = mcp::Action::accept,
            .content = scrydantic::Json{std::string{found->second}}};
  }

  int count(const std::string_view message) {
    const std::scoped_lock lock{mutex};
    const auto found = asked.find(message);
    return found == asked.end() ? 0 : found->second;
  }
};

mcp::ClientOptions answering(Person& person, const mcp::Era era) {
  return {.name = "zoo",
          .version = "0.3.0",
          .era = era,
          .elicitation =
              [&person](const mcp::FormRequest& form) { return person.answer(form); },
          .sampling = [](const mcp::SamplingRequest& request)
              -> mcp::Result<mcp::SamplingResult> {
            if (request.messages.size() != 1 || request.max_tokens != 10) {
              return std::unexpected(mcp::tool_error("an unexpected request"));
            }
            return mcp::SamplingResult{.role = mcp::Role::assistant,
                                       .content = {mcp::TextContent{.text = "hello"}},
                                       .model = "m1",
                                       .stop_reason = "endTurn"};
          },
          .roots =
              [] {
                return std::vector<mcp::Root>{{.uri = "file:///work"},
                                              {.uri = "file:///home", .name = "home"}};
              }};
}

std::string text_of(const mcp::Reply<mcp::ToolResult>& result) {
  REQUIRE(result.has_value());
  REQUIRE(result->content.size() == 1);
  const auto* const text = std::get_if<mcp::TextContent>(&result->content.front());
  REQUIRE(text != nullptr);
  return text->text;
}

} // namespace

TEST_CASE("a typed form is filled in through the client's handler") {
  const mcp::Server server = desk_server();
  for (const mcp::Era era : eras) {
    Person person;
    mcp::Reply<mcp::Client> client = mcp::connect(server, answering(person, era));
    REQUIRE(client.has_value());
    CHECK(client->call<^^desk::book>({.place = "Lima"}) == "booked 2 at Lima for Ada");
    CHECK(person.count("Who is it for?") == 1);
    CHECK(text_of(client->call_tool("abilities")) == "elicitation sampling roots");
  }
}

TEST_CASE("a declined or cancelled form reaches the tool as it is") {
  const mcp::Server server = desk_server();
  for (const mcp::Era era : eras) {
    for (const mcp::Action action : {mcp::Action::decline, mcp::Action::cancel}) {
      Person person;
      person.action = action;
      mcp::Reply<mcp::Client> client = mcp::connect(server, answering(person, era));
      REQUIRE(client.has_value());
      CHECK(client->call<^^desk::book>({.place = "Lima"}) ==
            (action == mcp::Action::decline ? "declined" : "cancelled"));
    }
  }
}

TEST_CASE("a tool may ask several questions, at once or in turn") {
  const mcp::Server server = desk_server();
  for (const mcp::Era era : eras) {
    Person person;
    mcp::Reply<mcp::Client> client = mcp::connect(server, answering(person, era));
    REQUIRE(client.has_value());
    CHECK(client->call<^^desk::both>({}) == "Ada likes blue");
    CHECK(person.count("Your name?") == 1);
    CHECK(person.count("Your color?") == 1);
    // Under 2026-07-28 the first answer travels in the server's state, so the
    // person is asked each question once.
    CHECK(client->call<^^desk::in_turn>({}) == "Ada likes blue");
    CHECK(person.count("Your name?") == 2);
    CHECK(person.count("Your color?") == 2);
  }
}

TEST_CASE("a form reaches the server with the defaults the person left as they were") {
  const mcp::Server server = desk_server();
  for (const mcp::Era era : eras) {
    Person person;
    mcp::Reply<mcp::Client> client = mcp::connect(server, answering(person, era));
    REQUIRE(client.has_value());
    const mcp::Reply<std::string> sent = client->call<^^desk::table>({});
    REQUIRE(sent.has_value());
    const scrydantic::Result<scrydantic::JsonView> content =
        scrydantic::JsonView::parse(*sent);
    REQUIRE(content);
    CHECK(content->find("name")->string() == "Ada");
    CHECK(content->find("seats")->number() == 4.0);
    CHECK(content->find("window")->boolean() == true);
  }
}

TEST_CASE("the model and the roots are asked through the client's handlers") {
  const mcp::Server server = desk_server();
  for (const mcp::Era era : eras) {
    Person person;
    mcp::Reply<mcp::Client> client = mcp::connect(server, answering(person, era));
    REQUIRE(client.has_value());
    CHECK(client->call<^^desk::word>({}) == "hello from m1");
    CHECK(client->call<^^desk::count_roots>({}) == "2 roots");
  }
}

TEST_CASE("a client without handlers is not asked, and the call is refused") {
  const mcp::Server server = desk_server();
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client =
        mcp::connect(server, {.name = "zoo", .version = "0.3.0", .era = era});
    REQUIRE(client.has_value());
    // What the tool sees.
    CHECK(text_of(client->call_tool("abilities")) == "- - -");
    // A question the client cannot answer fails the request.
    for (const auto& [tool, arguments] :
         {std::pair{"book", R"({"place":"Lima"})"}, std::pair{"word", "{}"},
          std::pair{"count_roots", "{}"}}) {
      INFO(tool);
      const mcp::Reply<mcp::ToolResult> called =
          client->call_tool(tool, scrydantic::Json{arguments});
      REQUIRE_FALSE(called.has_value());
      CHECK(called.error().fault == mcp::Fault::rejected);
      CHECK(called.error().code == -32021);
      REQUIRE(called.error().data.has_value());
      CHECK(called.error().data->text.starts_with(R"({"requiredCapabilities":)"));
    }
  }
}

TEST_CASE("a sampling handler that refuses fails the call under 2026-07-28") {
  const mcp::Server server = desk_server();
  for (const mcp::Era era : {mcp::Era::modern, mcp::Era::legacy}) {
    Person person;
    mcp::ClientOptions with = answering(person, era);
    with.sampling =
        [](const mcp::SamplingRequest&) -> mcp::Result<mcp::SamplingResult> {
      return std::unexpected(mcp::tool_error("not with my model"));
    };
    mcp::Reply<mcp::Client> client = mcp::connect(server, with);
    REQUIRE(client.has_value());
    const mcp::Reply<mcp::ToolResult> called = client->call_tool("word");
    if (era == mcp::Era::modern) {
      // The client sends nothing more.
      REQUIRE_FALSE(called.has_value());
      CHECK(called.error().fault == mcp::Fault::unanswered);
    } else {
      // The server is told, and its tool decides: this one fails.
      REQUIRE(called.has_value());
      CHECK(called->is_error);
      CHECK(text_of(called).find("not with my model") != std::string::npos);
    }
  }
}
