// A client and a server in one process, through the loopback channel: the
// client's view of the server's tools, in both eras.
#include "support.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mcp/client.hpp>
#include <mcp/mcp.hpp>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

namespace zoo {

struct Forecast {
  std::string summary;
  double high{};
};

[[= mcp::tool{"Get the forecast for a city"}]] inline Forecast
    forecast(std::string city,
             [[= mcp::range{.min = 1, .max = 7}]] std::int32_t days = 1) {
  return {.summary = city + " for " + std::to_string(days) + " days", .high = 21.5};
}

[[= mcp::tool{"List the cities"}]] inline std::vector<std::string> cities() {
  return {"Lima", "Oslo"};
}

[[= mcp::tool{"Describe a picture"}]] inline mcp::Content picture() {
  constexpr std::array<std::byte, 3> pixels{std::byte{1}, std::byte{2}, std::byte{3}};
  return {mcp::TextContent{.text = "a cat"}, mcp::image(pixels, "image/png")};
}

// Released by wait_for_cancel when it starts, and when it sees its stop.
inline std::counting_semaphore<> started{0};
inline std::counting_semaphore<> stopped{0};

[[= mcp::tool{"Wait until the call is cancelled"}]] inline std::string
    wait_for_cancel(std::stop_token stop) {
  started.release();
  std::mutex mutex;
  std::condition_variable_any never;
  std::unique_lock lock{mutex};
  never.wait_for(lock, stop, std::chrono::seconds{10}, [] { return false; });
  if (stop.stop_requested()) {
    stopped.release();
    return "cancelled";
  }
  return "not cancelled";
}

// How many calls of meet have come, and how many each waits for.
inline std::mutex meeting;
inline std::condition_variable met;
inline int arrived{0};
inline int expected{0};

[[= mcp::tool{"Wait until every caller has come"}]] inline std::string meet() {
  std::unique_lock lock{meeting};
  ++arrived;
  met.notify_all();
  const bool together =
      met.wait_for(lock, std::chrono::seconds{10}, [] { return arrived >= expected; });
  return together ? "together" : "alone";
}

} // namespace zoo

namespace {

constexpr auto patience = 10s;

struct Query {
  std::string city;
  std::int32_t days{};
};

mcp::Server zoo_server(std::vector<std::string>& log) {
  mcp::Server server = support::sample_server(log);
  server.add<^^zoo>();
  return server;
}

mcp::ClientOptions options(const mcp::Era era) {
  return {.name = "zoo", .version = "0.3.0", .era = era};
}

std::string text_of(const mcp::Content& content, const std::size_t index = 0) {
  REQUIRE(content.size() > index);
  const auto* const text = std::get_if<mcp::TextContent>(&content[index]);
  REQUIRE(text != nullptr);
  return text->text;
}

constexpr std::array<mcp::Era, 2> eras{mcp::Era::modern, mcp::Era::legacy};

} // namespace

TEST_CASE("a loopback client opens in the era it asks for") {
  std::vector<std::string> log;
  const mcp::Server server = zoo_server(log);
  struct Case {
    mcp::Era asked;
    mcp::Era got;
    std::string_view version;
  };
  for (const Case& each : {Case{mcp::Era::automatic, mcp::Era::modern, "2026-07-28"},
                           Case{mcp::Era::modern, mcp::Era::modern, "2026-07-28"},
                           Case{mcp::Era::legacy, mcp::Era::legacy, "2025-11-25"}}) {
    const mcp::Reply<mcp::Client> client = mcp::connect(server, options(each.asked));
    REQUIRE(client.has_value());
    const mcp::ServerInfo& info = client->server();
    CHECK(info.era == each.got);
    CHECK(info.protocol_version == each.version);
    CHECK(info.name == "sample");
    CHECK(info.version == "0.0.1");
    CHECK(info.capabilities.text == R"({"tools":{}})");
  }
}

TEST_CASE("a server that cannot be served does not open") {
  mcp::Server server{"broken", "1"};
  server.request_state_key("short");
  const mcp::Reply<mcp::Client> client = mcp::connect(server);
  REQUIRE_FALSE(client.has_value());
  CHECK(client.error().fault == mcp::Fault::transport);
}

TEST_CASE("the tools are listed with the server's own schemas") {
  std::vector<std::string> log;
  const mcp::Server server = zoo_server(log);
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());
    REQUIRE(client->ping().has_value());
    const mcp::Reply<std::vector<mcp::Tool>> tools = client->list_tools();
    REQUIRE(tools.has_value());
    REQUIRE(tools->size() == server.tools().size());
    for (std::size_t index = 0; index < tools->size(); ++index) {
      const mcp::Tool& tool = (*tools)[index];
      const scrydantic::BoundTool<mcp::Context>& bound = server.tools()[index];
      INFO(tool.name);
      CHECK(tool.name == bound.name);
      CHECK(tool.description == bound.description);
      CHECK(tool.input_schema.text == bound.input_schema.text);
      if (era == mcp::Era::modern && bound.output_schema.has_value()) {
        REQUIRE(tool.output_schema.has_value());
        CHECK(tool.output_schema->text == bound.output_schema->text);
      }
    }
  }
}

TEST_CASE("a tool is called by name, and its failure is a result") {
  std::vector<std::string> log;
  const mcp::Server server = zoo_server(log);
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());

    const mcp::Reply<mcp::ToolResult> added =
        client->call_tool("add", mcp::Json{R"({"a":2,"b":3})"});
    REQUIRE(added.has_value());
    CHECK_FALSE(added->is_error);
    CHECK(text_of(added->content) == "5");
    // Before 2026-07-28, structured content had to be an object.
    CHECK(added->structured_content.has_value() == (era == mcp::Era::modern));

    const mcp::Reply<mcp::ToolResult> refused = client->call_tool("refuse");
    REQUIRE(refused.has_value());
    CHECK(refused->is_error);
    CHECK(text_of(refused->content) == "not today");

    const mcp::Reply<mcp::ToolResult> unknown = client->call_tool("fly");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().fault == mcp::Fault::rejected);
    CHECK(unknown.error().code == -32602);
    CHECK(unknown.error().message == "Unknown tool: fly");
  }
}

TEST_CASE("a tool is called with types, by its declaration or by name") {
  std::vector<std::string> log;
  const mcp::Server server = zoo_server(log);
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());

    const mcp::Reply<zoo::Forecast> forecast =
        client->call<^^zoo::forecast>({.city = "Oslo", .days = 3});
    REQUIRE(forecast.has_value());
    CHECK(forecast->summary == "Oslo for 3 days");
    CHECK(forecast->high == 21.5);
    // A default argument the call leaves out is the server's to fill in.
    CHECK(client->call<^^zoo::forecast>({.city = "Lima", .days = std::nullopt})
              ->summary == "Lima for 1 days");

    const mcp::Reply<zoo::Forecast> by_name =
        client->call<zoo::Forecast>("forecast", Query{.city = "Oslo", .days = 2});
    REQUIRE(by_name.has_value());
    CHECK(by_name->summary == "Oslo for 2 days");

    CHECK(client->call<^^sample::add>({.a = 2, .b = 3}) == 5);
    const mcp::Reply<void> nothing = client->call<^^sample::nothing>({});
    CHECK(nothing.has_value());

    // Strings and arrays reach a legacy client as text alone.
    CHECK(client->call<^^sample::greet>({.who = "Ada"}) == "hello Ada");
    CHECK(client->call<std::string>("greet", mcp::Json{"{}"}) == "hello world");
    CHECK(client->call<^^zoo::cities>({}) == std::vector<std::string>{"Lima", "Oslo"});
    const mcp::Reply<mcp::ToolResult> listed = client->call_tool("cities");
    REQUIRE(listed.has_value());
    CHECK(listed->structured_content.has_value() == (era == mcp::Era::modern));

    const mcp::Reply<void> refused = client->call<^^sample::refuse>({});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().fault == mcp::Fault::tool);
    CHECK(refused.error().message == "not today");

    const mcp::Reply<std::int32_t> mismatched =
        client->call<std::int32_t>("greet", mcp::Json{"{}"});
    REQUIRE_FALSE(mismatched.has_value());
    CHECK(mismatched.error().fault == mcp::Fault::mismatch);

    // Arguments the tool's constraints reject are the tool's failure.
    const mcp::Reply<zoo::Forecast> too_long =
        client->call<^^zoo::forecast>({.city = "Oslo", .days = 9});
    REQUIRE_FALSE(too_long.has_value());
    CHECK(too_long.error().fault == mcp::Fault::tool);
  }
}

TEST_CASE("a tool that returns content blocks gives them as they are") {
  std::vector<std::string> log;
  const mcp::Server server = zoo_server(log);
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());

    const mcp::Reply<mcp::Content> content = client->call<^^zoo::picture>({});
    REQUIRE(content.has_value());
    REQUIRE(content->size() == 2);
    CHECK(text_of(*content) == "a cat");
    const auto* const image = std::get_if<mcp::ImageContent>(&(*content)[1]);
    REQUIRE(image != nullptr);
    CHECK(image->data == "AQID");
    CHECK(image->mime_type == "image/png");

    const mcp::Reply<mcp::ToolResult> called = client->call_tool("picture");
    REQUIRE(called.has_value());
    CHECK(called->content.size() == 2);
    CHECK_FALSE(called->structured_content.has_value());
  }
}

TEST_CASE("a call's progress reaches the caller") {
  std::vector<std::string> log;
  const mcp::Server server = zoo_server(log);
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());
    std::vector<mcp::ProgressReport> reports;
    const mcp::Reply<std::int32_t> counted = client->call<^^sample::count>(
        {.steps = 3}, {.progress = [&reports](const mcp::ProgressReport& report) {
          reports.push_back(report);
        }});
    CHECK(counted == 3);
    REQUIRE(reports.size() == 3);
    for (std::size_t index = 0; index < reports.size(); ++index) {
      CHECK(reports[index].progress == static_cast<double>(index + 1));
      CHECK(reports[index].total == 3.0);
    }
  }
}

TEST_CASE("cancelling a call stops the tool that runs it") {
  std::vector<std::string> log;
  const mcp::Server server = zoo_server(log);
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());
    mcp::Client& open = *client;
    std::stop_source stop;
    auto calling = std::async(std::launch::async, [&] {
      return open.call<^^zoo::wait_for_cancel>(
          {}, {.progress = {}, .stop = stop.get_token(), .timeout = {}});
    });
    REQUIRE(zoo::started.try_acquire_for(patience));
    stop.request_stop();
    REQUIRE(calling.wait_for(patience) == std::future_status::ready);
    const mcp::Reply<std::string> called = calling.get();
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().fault == mcp::Fault::cancelled);
    CHECK(zoo::stopped.try_acquire_for(patience));
  }
}

TEST_CASE("a call that runs out of time stops the tool that runs it") {
  std::vector<std::string> log;
  const mcp::Server server = zoo_server(log);
  mcp::Reply<mcp::Client> client = mcp::connect(server, options(mcp::Era::modern));
  REQUIRE(client.has_value());
  const mcp::Reply<std::string> called =
      client->call<^^zoo::wait_for_cancel>({}, {.timeout = 300ms});
  REQUIRE_FALSE(called.has_value());
  CHECK(called.error().fault == mcp::Fault::timeout);
  CHECK(zoo::started.try_acquire_for(patience));
  CHECK(zoo::stopped.try_acquire_for(patience));
}

TEST_CASE("several threads call one client at once") {
  std::vector<std::string> log;
  const mcp::Server server = zoo_server(log);
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());
    constexpr int callers = 4;
    {
      const std::scoped_lock lock{zoo::meeting};
      zoo::arrived = 0;
      zoo::expected = callers;
    }
    mcp::Client& open = *client;
    std::vector<std::future<mcp::Reply<std::string>>> calls;
    for (int caller = 0; caller < callers; ++caller) {
      calls.push_back(std::async(std::launch::async,
                                 [&open] { return open.call<^^zoo::meet>({}); }));
    }
    for (std::future<mcp::Reply<std::string>>& call : calls) {
      REQUIRE(call.wait_for(patience) == std::future_status::ready);
      CHECK(call.get() == "together");
    }
  }
}
