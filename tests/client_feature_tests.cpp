// Resources, prompts, completion and log messages, against a scripted channel:
// the test plays the server, and sees every message the client sends.
#include "scripted.hpp"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <mcp/client.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace scripted;
using namespace std::chrono_literals;

// Declarations alone, as a header shared with a server gives them: nothing
// here is defined.
namespace declared {

[[= mcp::resource{.uri = "notes://{name}/at/{+rest}"}]] std::string
    note(std::string name, std::string rest);

[[= mcp::resource{.uri = "data://blob"}]] std::vector<std::byte>
    blob(mcp::Logger logger);

[[= mcp::prompt{.description = "Plan a trip", .name = "plan_trip"}]] std::string
    plan(std::string city, std::int32_t days, mcp::Logger logger, bool rainy = false);

} // namespace declared

namespace {

// The params of a modern request: the metadata, then these members, if any.
std::string modern_params(const std::string_view members = {}) {
  std::string params = "{" + std::string{modern_meta};
  if (!members.empty()) {
    params += ",";
    params += members;
  }
  return params + "}";
}

// The metadata of a modern request that asks for log messages of `level`.
std::string meta_asking(const std::string_view level) {
  return R"("_meta":{"io.modelcontextprotocol/clientCapabilities":{},)"
         R"("io.modelcontextprotocol/clientInfo":{"name":"zoo","version":"0.3.0"},)"
         R"("io.modelcontextprotocol/logLevel":")" +
         std::string{level} +
         R"(","io.modelcontextprotocol/protocolVersion":"2026-07-28"})";
}

// Answers every request with `result`, as JSON text.
std::function<void(Script&, const Sent&)> always(std::string result) {
  return [result = std::move(result)](Script& server, const Sent& sent) {
    if (!sent.id.empty()) {
      server.deliver(support::result(sent.id, result));
    }
  };
}

std::string log_message(const std::string_view params) {
  return R"({"jsonrpc":"2.0","method":"notifications/message","params":)" +
         std::string{params} + "}";
}

std::string text_of(const mcp::PromptMessage& message) {
  const auto* const text = std::get_if<mcp::TextContent>(&message.content);
  REQUIRE(text != nullptr);
  return text->text;
}

constexpr std::string_view one_text =
    R"({"contents":[{"uri":"a://b","mimeType":"text/plain","text":"hi"}]})";
constexpr std::string_view one_message =
    R"({"messages":[{"role":"user","content":{"type":"text","text":"hi"}}]})";
constexpr std::string_view no_values = R"({"completion":{"values":[]}})";

} // namespace

// What the client sends.

TEST_CASE("each request is sent as its era writes it") {
  for (const mcp::Era era : {mcp::Era::modern, mcp::Era::legacy}) {
    const bool modern = era == mcp::Era::modern;
    Opened opened = open_in(era);
    REQUIRE(opened.client.has_value());
    mcp::Client& client = *opened.client;
    // The params as each era writes them, from the members after `_meta`.
    const auto params = [modern](const std::string_view members = {}) {
      return modern ? modern_params(members)
                    : (members.empty() ? std::string{"{}"}
                                       : "{" + std::string{members} + "}");
    };
    const auto last = [&opened] { return opened.script->sent().back(); };

    opened.script->respond = always(R"({"resources":[]})");
    REQUIRE(client.list_resources().has_value());
    CHECK(last().message == request(last().id, "resources/list", params()));
    opened.script->respond = always(R"({"resourceTemplates":[]})");
    REQUIRE(client.list_resource_templates().has_value());
    CHECK(last().message == request(last().id, "resources/templates/list", params()));
    opened.script->respond = always(R"({"prompts":[]})");
    REQUIRE(client.list_prompts().has_value());
    CHECK(last().message == request(last().id, "prompts/list", params()));

    opened.script->respond = always(std::string{one_text});
    REQUIRE(client.read_resource("a://b c/é").has_value());
    CHECK(last().message ==
          request(last().id, "resources/read", params(R"("uri":"a://b c/é")")));
    CHECK(last().name == "a://b c/é");
    CHECK(last().protocol_version == (modern ? "2026-07-28" : "2025-11-25"));

    opened.script->respond = always(std::string{one_message});
    REQUIRE(client.get_prompt("plan", {{"days", "3"}, {"city", "Oslo \"Norway\""}})
                .has_value());
    CHECK(last().message ==
          request(last().id, "prompts/get",
                  params(R"("arguments":{"city":"Oslo \"Norway\"","days":"3"},)"
                         R"("name":"plan")")));
    CHECK(last().name == "plan");
    // No arguments, no member.
    REQUIRE(client.get_prompt("plan").has_value());
    CHECK(last().message ==
          request(last().id, "prompts/get", params(R"("name":"plan")")));

    opened.script->respond = always(std::string{no_values});
    REQUIRE(client.complete(mcp::PromptReference{"plan"}, "city", "Os").has_value());
    CHECK(last().message ==
          request(last().id, "completion/complete",
                  params(R"("argument":{"name":"city","value":"Os"},)"
                         R"("ref":{"name":"plan","type":"ref/prompt"})")));
    CHECK(last().name.empty());
    REQUIRE(client
                .complete(mcp::ResourceTemplateReference{"w://{city}/{day}"}, "day", "",
                          {{"city", "Oslo"}})
                .has_value());
    CHECK(last().message ==
          request(last().id, "completion/complete",
                  params(R"("argument":{"name":"day","value":""},)"
                         R"("context":{"arguments":{"city":"Oslo"}},)"
                         R"("ref":{"type":"ref/resource","uri":"w://{city}/{day}"})")));
  }
}

TEST_CASE("text that is not UTF-8 is not sent") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  const mcp::Reply<mcp::PromptResult> got =
      opened.client->get_prompt("plan", {{"city", "\xFF"}});
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().fault == mcp::Fault::protocol);
  const mcp::Reply<std::vector<mcp::ResourceContents>> read =
      opened.client->read_resource("a://\xC3");
  REQUIRE_FALSE(read.has_value());
  CHECK(read.error().fault == mcp::Fault::protocol);
  CHECK(opened.script->sent().size() == 1);
}

TEST_CASE("a typed read sends the declaration's URI, filled in") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  opened.script->respond = always(std::string{one_text});
  CHECK(opened.client->read<^^declared::note>({.name = "a b/c", .rest = "x/y z"}) ==
        "hi");
  const Sent sent = opened.script->sent().back();
  CHECK(sent.name == "notes://a%20b%2Fc/at/x/y z");
  CHECK(sent.message ==
        request(sent.id, "resources/read",
                modern_params(R"("uri":"notes://a%20b%2Fc/at/x/y z")")));

  opened.script->respond = always(
      R"({"contents":[{"uri":"data://blob","mimeType":"image/png","blob":"AQI="}]})");
  CHECK(opened.client->read<^^declared::blob>({}) ==
        std::vector<std::byte>{std::byte{1}, std::byte{2}});
  CHECK(opened.script->sent().back().name == "data://blob");
}

TEST_CASE("a typed read decodes what the function returns, or fails with mismatch") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  mcp::Client& client = *opened.client;
  std::string answer;
  opened.script->respond = [&answer](Script& server, const Sent& sent) {
    server.deliver(support::result(sent.id, answer));
  };
  const auto contents = [](const std::string_view items) {
    return R"({"contents":[)" + std::string{items} + "]}";
  };
  const std::string text = R"({"uri":"a://b","text":"[1, 2]"})";
  const std::string blob = R"({"uri":"a://b","blob":"AQI="})";

  answer = contents(text);
  CHECK(client.read<std::string>("a://b") == "[1, 2]");
  CHECK(client.read<std::vector<std::int32_t>>("a://b") ==
        std::vector<std::int32_t>{1, 2});
  answer = contents(text + "," + blob);
  const mcp::Reply<std::vector<mcp::ResourceContents>> both =
      client.read<std::vector<mcp::ResourceContents>>("a://b");
  REQUIRE(both.has_value());
  CHECK(both->size() == 2);

  struct Case {
    std::string items;
    std::function<mcp::Reply<void>()> read;
  };
  const auto failure = [](auto reply) -> mcp::Reply<void> {
    if (reply) {
      return {};
    }
    return std::unexpected(reply.error());
  };
  for (const Case& each : {
           // Two items for one value.
           Case{text + "," + text,
                [&] { return failure(client.read<std::string>("a://b")); }},
           Case{text + "," + blob,
                [&] { return failure(client.read<mcp::ResourceContents>("a://b")); }},
           // None.
           Case{"", [&] { return failure(client.read<std::string>("a://b")); }},
           // Bytes where text is wanted, and text where bytes are.
           Case{blob, [&] { return failure(client.read<std::string>("a://b")); }},
           Case{text,
                [&] { return failure(client.read<std::vector<std::byte>>("a://b")); }},
           // Text that is not the value.
           Case{text, [&] { return failure(client.read<std::int32_t>("a://b")); }},
           // A blob that is not base64.
           Case{R"({"uri":"a://b","blob":"A*=="})",
                [&] { return failure(client.read<std::vector<std::byte>>("a://b")); }},
           Case{R"({"uri":"a://b","blob":"A"})",
                [&] { return failure(client.read<std::vector<std::byte>>("a://b")); }},
       }) {
    INFO(each.items);
    answer = contents(each.items);
    const mcp::Reply<void> read = each.read();
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().fault == mcp::Fault::mismatch);
  }
}

TEST_CASE("a typed prompt sends its arguments as the text the server reads") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  opened.script->respond = always(std::string{one_message});

  const mcp::Reply<mcp::PromptResult> rendered =
      opened.client->prompt<^^declared::plan>(
          {.city = "Oslo", .days = -3, .rainy = true});
  REQUIRE(rendered.has_value());
  CHECK(text_of(rendered->messages.at(0)) == "hi");
  Sent sent = opened.script->sent().back();
  CHECK(sent.name == "plan_trip");
  CHECK(
      sent.message ==
      request(sent.id, "prompts/get",
              modern_params(R"("arguments":{"city":"Oslo","days":"-3","rainy":"true"},)"
                            R"("name":"plan_trip")")));

  // A default argument left out is not sent.
  REQUIRE(
      opened.client
          ->prompt<^^declared::plan>({.city = "Lima", .days = 1, .rainy = std::nullopt})
          .has_value());
  sent = opened.script->sent().back();
  CHECK(sent.message ==
        request(sent.id, "prompts/get",
                modern_params(R"("arguments":{"city":"Lima","days":"1"},)"
                              R"("name":"plan_trip")")));
}

// Lists.

TEST_CASE("each list follows the cursors and keeps what it does not read as written") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  mcp::Client& client = *opened.client;
  std::string first;
  std::string second;
  opened.script->respond = [&](Script& server, const Sent& sent) {
    const bool later = sent.message.find(R"("cursor":"p2")") != std::string::npos;
    server.deliver(support::result(sent.id, later ? second : first));
  };

  first =
      R"({"nextCursor":"p2","resources":[{"uri":"a://1","name":"one","title":"One",)"
      R"("description":"The first","mimeType":"text/plain","size":12,)"
      R"("annotations":{"priority": 0.50, "audience":["user"]},)"
      R"("icons":[{"src":"https://x/i.png"}],"_meta":{"k":1}}]})";
  second = R"({"resources":[{"uri":"a://2","name":"two","size":-1,"title":7}],)"
           R"("nextCursor":null})";
  const mcp::Reply<std::vector<mcp::Resource>> resources = client.list_resources();
  REQUIRE(resources.has_value());
  REQUIRE(resources->size() == 2);
  const mcp::Resource& one = (*resources)[0];
  CHECK(one.uri == "a://1");
  CHECK(one.name == "one");
  CHECK(one.title == "One");
  CHECK(one.description == "The first");
  CHECK(one.mime_type == "text/plain");
  CHECK(one.size == 12);
  REQUIRE(one.annotations.has_value());
  CHECK(one.annotations->text == R"({"priority": 0.50, "audience":["user"]})");
  REQUIRE(one.icons.has_value());
  CHECK(one.icons->text == R"([{"src":"https://x/i.png"}])");
  REQUIRE(one.meta.has_value());
  CHECK(one.meta->text == R"({"k":1})");
  // What is only for display, and is not as the protocol has it, is left out.
  const mcp::Resource& two = (*resources)[1];
  CHECK(two.title.empty());
  CHECK_FALSE(two.size.has_value());
  CHECK_FALSE(two.annotations.has_value());
  CHECK(opened.script->sent().back().message ==
        request(opened.script->sent().back().id, "resources/list",
                modern_params(R"("cursor":"p2")")));

  first = R"({"nextCursor":"p2","resourceTemplates":[{"uriTemplate":"a://{x}",)"
          R"("name":"x","title":"X","description":"Any x","mimeType":"text/csv",)"
          R"("annotations":{"lastModified":"2025-01-12T15:00:58Z"}}]})";
  second = R"({"resourceTemplates":[{"uriTemplate":"b://{+y}","name":"y"}]})";
  const mcp::Reply<std::vector<mcp::ResourceTemplate>> templates =
      client.list_resource_templates();
  REQUIRE(templates.has_value());
  REQUIRE(templates->size() == 2);
  CHECK((*templates)[0].uri_template == "a://{x}");
  CHECK((*templates)[0].title == "X");
  CHECK((*templates)[0].description == "Any x");
  CHECK((*templates)[0].mime_type == "text/csv");
  CHECK((*templates)[0].annotations->text ==
        R"({"lastModified":"2025-01-12T15:00:58Z"})");
  CHECK((*templates)[1].uri_template == "b://{+y}");
  CHECK_FALSE((*templates)[1].icons.has_value());

  first = R"({"nextCursor":"p2","prompts":[{"name":"plan","title":"Plan",)"
          R"("description":"Plan a trip","arguments":[{"name":"city","title":"City",)"
          R"("description":"Where","required":true},{"name":"days"}],"icons":[]}]})";
  second = R"({"prompts":[{"name":"bare"}]})";
  const mcp::Reply<std::vector<mcp::Prompt>> prompts = client.list_prompts();
  REQUIRE(prompts.has_value());
  REQUIRE(prompts->size() == 2);
  const mcp::Prompt& plan = (*prompts)[0];
  CHECK(plan.name == "plan");
  CHECK(plan.title == "Plan");
  CHECK(plan.description == "Plan a trip");
  REQUIRE(plan.arguments.size() == 2);
  CHECK(plan.arguments[0].name == "city");
  CHECK(plan.arguments[0].title == "City");
  CHECK(plan.arguments[0].description == "Where");
  CHECK(plan.arguments[0].required);
  CHECK(plan.arguments[1].name == "days");
  CHECK_FALSE(plan.arguments[1].required);
  CHECK(plan.icons->text == "[]");
  CHECK((*prompts)[1].arguments.empty());
}

TEST_CASE("a list that never ends fails within its bound of pages") {
  struct Case {
    std::string page;
    std::function<mcp::Reply<std::size_t>(mcp::Client&)> list;
  };
  const auto count = [](auto reply) -> mcp::Reply<std::size_t> {
    if (!reply) {
      return std::unexpected(reply.error());
    }
    return reply->size();
  };
  for (const Case& each :
       {Case{R"({"nextCursor":"again","resources":[]})",
             [&](mcp::Client& client) { return count(client.list_resources()); }},
        Case{R"({"nextCursor":"again","resourceTemplates":[]})",
             [&](mcp::Client& client) {
               return count(client.list_resource_templates());
             }},
        Case{R"({"nextCursor":"again","prompts":[]})",
             [&](mcp::Client& client) { return count(client.list_prompts()); }}}) {
    INFO(each.page);
    Opened opened = open_in(mcp::Era::modern);
    REQUIRE(opened.client.has_value());
    opened.script->respond = always(each.page);
    const mcp::Reply<std::size_t> listed = each.list(*opened.client);
    REQUIRE_FALSE(listed.has_value());
    CHECK(listed.error().fault == mcp::Fault::protocol);
    CHECK(opened.script->sent().size() == 1 + 1000);
  }
}

// What the protocol does not allow.

TEST_CASE("a list the protocol does not allow fails with Fault::protocol") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  mcp::Client& client = *opened.client;
  for (const std::string_view result :
       {R"({})", R"({"resources":{}})", R"({"resources":["a"]})",
        R"({"resources":[{"name":"a"}]})", R"({"resources":[{"uri":"a://"}]})",
        R"({"resources":[{"uri":1,"name":"a"}]})",
        R"({"resources":[],"nextCursor":3})"}) {
    INFO(result);
    opened.script->respond = always(std::string{result});
    const mcp::Reply<std::vector<mcp::Resource>> listed = client.list_resources();
    REQUIRE_FALSE(listed.has_value());
    CHECK(listed.error().fault == mcp::Fault::protocol);
  }
  for (const std::string_view result :
       {R"({"resources":[]})", R"({"resourceTemplates":[{"uri":"a://","name":"a"}]})",
        R"({"resourceTemplates":[{"uriTemplate":"a://{x}"}]})",
        R"({"resourceTemplates":[null]})"}) {
    INFO(result);
    opened.script->respond = always(std::string{result});
    const mcp::Reply<std::vector<mcp::ResourceTemplate>> listed =
        client.list_resource_templates();
    REQUIRE_FALSE(listed.has_value());
    CHECK(listed.error().fault == mcp::Fault::protocol);
  }
  for (const std::string_view result :
       {R"({"prompts":{}})", R"({"prompts":[{"title":"a"}]})",
        R"({"prompts":[{"name":"a","arguments":{}}]})",
        R"({"prompts":[{"name":"a","arguments":[{"description":"no name"}]}]})",
        R"({"prompts":[{"name":"a","arguments":["x"]}]})"}) {
    INFO(result);
    opened.script->respond = always(std::string{result});
    const mcp::Reply<std::vector<mcp::Prompt>> listed = client.list_prompts();
    REQUIRE_FALSE(listed.has_value());
    CHECK(listed.error().fault == mcp::Fault::protocol);
  }
}

TEST_CASE("a read, a prompt or a completion the protocol does not allow fails") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  mcp::Client& client = *opened.client;
  for (const std::string_view result :
       {R"({})", R"({"contents":{}})", R"({"contents":[{"text":"no uri"}]})",
        R"({"contents":[{"uri":"a://b"}]})",
        R"({"contents":[{"uri":"a://b","text":"x","blob":"AA=="}]})",
        R"({"contents":[{"uri":"a://b","text":3}]})", R"({"contents":["x"]})",
        R"({"contents":[],"resultType":"later"})"}) {
    INFO(result);
    opened.script->respond = always(std::string{result});
    const mcp::Reply<std::vector<mcp::ResourceContents>> read =
        client.read_resource("a://b");
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().fault == mcp::Fault::protocol);
  }
  for (const std::string_view result :
       {R"({})", R"({"messages":{}})", R"({"messages":["hi"]})",
        R"({"messages":[{"role":"user"}]})",
        R"({"messages":[{"content":{"type":"text","text":"hi"}}]})",
        R"({"messages":[{"role":"system","content":{"type":"text","text":"hi"}}]})",
        R"({"messages":[{"role":"user","content":{"type":"video"}}]})",
        R"({"messages":[{"role":"user","content":[]}]})"}) {
    INFO(result);
    opened.script->respond = always(std::string{result});
    const mcp::Reply<mcp::PromptResult> got = client.get_prompt("plan");
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().fault == mcp::Fault::protocol);
  }
  for (
      const std::string_view result :
      {R"({})", R"({"completion":[]})", R"({"completion":{}})",
       R"({"completion":{"values":{}}})", R"({"completion":{"values":[1]}})",
       R"({"completion":{"values":[],"total":"3"}})",
       R"({"completion":{"values":[],"total":-1}})",
       R"({"completion":{"values":[],"total":1.5}})",
       R"({"completion":{"values":[],"hasMore":"yes"}})",
       // Completion takes no input.
       R"({"inputRequests":{"a":{"method":"roots/list"}},"resultType":"input_required"})"}) {
    INFO(result);
    opened.script->respond = always(std::string{result});
    const mcp::Reply<mcp::Completion> completed =
        client.complete(mcp::PromptReference{"plan"}, "city", "");
    REQUIRE_FALSE(completed.has_value());
    CHECK(completed.error().fault == mcp::Fault::protocol);
  }
}

TEST_CASE("a read and a prompt keep what the server wrote") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  opened.script->respond = always(
      R"({"contents":[{"uri":"a://b","mimeType":"text/plain","text":"one","_meta":{}},)"
      R"({"uri":"a://b/2","blob":"AAEC"}],"ttlMs":5,"cacheScope":"private"})");
  const mcp::Reply<std::vector<mcp::ResourceContents>> read =
      opened.client->read_resource("a://b");
  REQUIRE(read.has_value());
  REQUIRE(read->size() == 2);
  CHECK((*read)[0].text == "one");
  CHECK((*read)[0].mime_type == "text/plain");
  CHECK((*read)[1].uri == "a://b/2");
  CHECK((*read)[1].blob == "AAEC");
  CHECK_FALSE((*read)[1].mime_type.has_value());

  opened.script->respond = always(
      R"({"description":"A plan","messages":[)"
      R"({"role":"user","content":{"type":"text","text":"Go","annotations":{}}},)"
      R"({"role":"assistant","content":{"type":"image","data":"AA==","mimeType":"image/png"}},)"
      R"({"role":"user","content":{"type":"resource","resource":{"uri":"a://b","text":"x"}}}]})");
  const mcp::Reply<mcp::PromptResult> got = opened.client->get_prompt("plan");
  REQUIRE(got.has_value());
  CHECK(got->description == "A plan");
  REQUIRE(got->messages.size() == 3);
  CHECK(text_of(got->messages[0]) == "Go");
  CHECK(got->messages[1].role == mcp::Role::assistant);
  CHECK(std::holds_alternative<mcp::ImageContent>(got->messages[1].content));
  CHECK(std::holds_alternative<mcp::EmbeddedResource>(got->messages[2].content));

  opened.script->respond = always(R"({"messages":[]})");
  const mcp::Reply<mcp::PromptResult> bare = opened.client->get_prompt("plan");
  REQUIRE(bare.has_value());
  CHECK(bare->description.empty());
  CHECK(bare->messages.empty());
}

TEST_CASE("a completion gives the values, the total and whether there are more") {
  Opened opened = open_in(mcp::Era::modern);
  REQUIRE(opened.client.has_value());
  opened.script->respond = always(
      R"({"completion":{"values":["python","pytorch"],"total":10,"hasMore":true}})");
  const mcp::Reply<mcp::Completion> prompt =
      opened.client->complete(mcp::PromptReference{"review"}, "language", "py");
  REQUIRE(prompt.has_value());
  CHECK(prompt->values == std::vector<std::string>{"python", "pytorch"});
  CHECK(prompt->total == 10);
  CHECK(prompt->has_more);

  opened.script->respond = always(R"({"completion":{"values":["flask"]}})");
  const mcp::Reply<mcp::Completion> resource = opened.client->complete(
      mcp::ResourceTemplateReference{"code://{language}/{framework}"}, "framework",
      "fla", {{"language", "python"}});
  REQUIRE(resource.has_value());
  CHECK(resource->values == std::vector<std::string>{"flask"});
  CHECK_FALSE(resource->total.has_value());
  CHECK_FALSE(resource->has_more);
}

// Errors.

TEST_CASE(
    "a resource that is not there is a rejection with the server's code and data") {
  for (const auto& [era, code] :
       {std::pair{mcp::Era::modern, -32602}, std::pair{mcp::Era::legacy, -32002}}) {
    Opened opened = open_in(era);
    REQUIRE(opened.client.has_value());
    opened.script->respond = [code](Script& server, const Sent& sent) {
      server.deliver(
          error_with(sent.id, code, "Resource not found", R"({"uri": "a://gone"})"));
    };
    const mcp::Reply<std::vector<mcp::ResourceContents>> read =
        opened.client->read_resource("a://gone");
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().fault == mcp::Fault::rejected);
    CHECK(read.error().code == code);
    CHECK(read.error().message == "Resource not found");
    REQUIRE(read.error().data.has_value());
    CHECK(read.error().data->text == R"({"uri": "a://gone"})");

    const mcp::Reply<std::string> typed = opened.client->read<std::string>("a://gone");
    REQUIRE_FALSE(typed.has_value());
    CHECK(typed.error().code == code);
  }
}

TEST_CASE("each new call keeps the client's rules on time, stops and moves") {
  mcp::ClientOptions with = options();
  with.timeout = 50ms;
  Opened opened = open_in(mcp::Era::modern, with);
  REQUIRE(opened.client.has_value());
  mcp::Client& client = *opened.client;

  const mcp::Reply<mcp::PromptResult> late = client.get_prompt("plan");
  REQUIRE_FALSE(late.has_value());
  CHECK(late.error().fault == mcp::Fault::timeout);
  CHECK(opened.script->sent().back().method == "notifications/cancelled");

  std::stop_source stop;
  stop.request_stop();
  const std::size_t before = opened.script->sent().size();
  const mcp::CallOptions stopped{
      .progress = {}, .stop = stop.get_token(), .timeout = {}};
  CHECK(client.read_resource("a://b", stopped).error().fault == mcp::Fault::cancelled);
  CHECK(client.list_prompts(stopped).error().fault == mcp::Fault::cancelled);
  CHECK(client.complete(mcp::PromptReference{"plan"}, "a", "", {}, stopped)
            .error()
            .fault == mcp::Fault::cancelled);
  CHECK(opened.script->sent().size() == before);

  mcp::Client moved = std::move(client);
  for (const mcp::ClientError& error :
       {client.list_resources().error(), client.list_resource_templates().error(),
        client.read_resource("a://b").error(), client.list_prompts().error(),
        client.get_prompt("plan").error(),
        client.complete(mcp::PromptReference{"plan"}, "a", "").error(),
        client.complete(mcp::ResourceTemplateReference{"a://{b}"}, "b", "").error(),
        client.read<std::string>("a://b").error(),
        client.prompt<^^declared::plan>({.city = "x", .days = 1, .rainy = {}})
            .error()}) {
    CHECK(error.fault == mcp::Fault::transport);
  }
  CHECK(opened.script->sent().size() == before);
}

// Input the server asks for.

TEST_CASE("a read and a prompt that need input are sent again with the answers") {
  mcp::ClientOptions with = options();
  with.elicitation = [](const mcp::FormRequest& form) {
    CHECK(form.message == "Who?");
    return mcp::Answer<mcp::Json>{.action = mcp::Action::accept,
                                  .content = mcp::Json{R"({"name":"Ada"})"}};
  };
  Opened opened = open_in(mcp::Era::modern, with);
  REQUIRE(opened.client.has_value());
  const std::string asking =
      R"({"inputRequests":{"who":{"method":"elicitation/create","params":{"mode":"form",)"
      R"("message":"Who?","requestedSchema":{"type":"object","properties":{"name":)"
      R"({"type":"string"}}}}}},"requestState":"s1","resultType":"input_required"})";
  const std::string meta =
      R"("_meta":{"io.modelcontextprotocol/clientCapabilities":{"elicitation":{"form":{}}},)"
      R"("io.modelcontextprotocol/clientInfo":{"name":"zoo","version":"0.3.0"},)"
      R"("io.modelcontextprotocol/protocolVersion":"2026-07-28"})";
  const std::string answers =
      R"("inputResponses":{"who":{"action":"accept","content":{"name":"Ada"}}})";

  struct Case {
    std::string method;
    std::string done;
    std::string members;
    std::string name;
    std::function<bool(mcp::Client&)> call;
  };
  for (const Case& each :
       {Case{"resources/read", std::string{one_text}, R"("uri":"a://b")", "a://b",
             [](mcp::Client& client) {
               return client.read_resource("a://b").has_value();
             }},
        Case{"prompts/get", std::string{one_message},
             R"("arguments":{"x":"1"},"name":"plan")", "plan", [](mcp::Client& client) {
               return client.get_prompt("plan", {{"x", "1"}}).has_value();
             }}}) {
    INFO(each.method);
    bool asked = false;
    opened.script->respond = [&](Script& server, const Sent& sent) {
      if (sent.method == each.method) {
        server.deliver(
            support::result(sent.id, std::exchange(asked, true) ? each.done : asking));
      }
    };
    const std::size_t before = opened.script->sent().size();
    REQUIRE(each.call(*opened.client));
    const std::vector<Sent> sent = opened.script->sent();
    REQUIRE(sent.size() == before + 2);
    const Sent& first = sent[before];
    const Sent& again = sent[before + 1];
    CHECK(first.message ==
          request(first.id, each.method, "{" + meta + "," + each.members + "}"));
    // The members in order of their names, the answers and the state among them.
    std::string members = each.members;
    if (each.method == "resources/read") {
      members = answers + R"(,"requestState":"s1",)" + each.members;
    } else {
      members = R"("arguments":{"x":"1"},)" + answers +
                R"(,"name":"plan","requestState":"s1")";
    }
    CHECK(again.message ==
          request(again.id, each.method, "{" + meta + "," + members + "}"));
    CHECK(again.id != first.id);
    CHECK(again.name == each.name);
  }
}

// Logging.

namespace {

struct Received {
  std::mutex mutex;
  std::vector<mcp::LogMessage> messages;

  [[nodiscard]] std::size_t size() {
    const std::scoped_lock lock{mutex};
    return messages.size();
  }
};

mcp::ClientOptions logging(Received& received,
                           const mcp::LogLevel level = mcp::LogLevel::info) {
  mcp::ClientOptions with = options();
  with.log = [&received](const mcp::LogMessage& message) {
    const std::scoped_lock lock{received.mutex};
    received.messages.push_back(message);
  };
  with.log_level = level;
  return with;
}

} // namespace

TEST_CASE(
    "under 2026-07-28 each request asks for the level, which one call may change") {
  Received received;
  Opened opened = open_in(mcp::Era::modern, logging(received, mcp::LogLevel::warning));
  REQUIRE(opened.client.has_value());
  // Discovery asks too, as every request does.
  CHECK(opened.script->sent()[0].message ==
        request("1", "server/discover", "{" + meta_asking("warning") + "}"));

  opened.script->respond = always(R"({"tools":[]})");
  REQUIRE(opened.client->list_tools().has_value());
  Sent sent = opened.script->sent().back();
  CHECK(sent.message ==
        request(sent.id, "tools/list", "{" + meta_asking("warning") + "}"));

  opened.script->respond = always(std::string{one_text});
  REQUIRE(opened.client
              ->read_resource("a://b", {.progress = {},
                                        .stop = {},
                                        .timeout = {},
                                        .log_level = mcp::LogLevel::debug})
              .has_value());
  sent = opened.script->sent().back();
  CHECK(sent.message == request(sent.id, "resources/read",
                                "{" + meta_asking("debug") + R"(,"uri":"a://b"})"));
}

TEST_CASE("a client without a log handler asks for no log messages") {
  Opened modern = open_in(mcp::Era::modern);
  REQUIRE(modern.client.has_value());
  modern.script->respond = always(std::string{one_text});
  REQUIRE(modern.client
              ->read_resource("a://b", {.progress = {},
                                        .stop = {},
                                        .timeout = {},
                                        .log_level = mcp::LogLevel::debug})
              .has_value());
  CHECK(modern.script->sent().back().message.find("logLevel") == std::string::npos);

  // A session's server that declares logging is not asked either.
  const auto script = std::make_shared<Script>();
  script->respond =
      answering({{"initialize",
                  R"({"capabilities":{"logging":{}},"protocolVersion":"2025-11-25",)"
                  R"("serverInfo":{"name":"script","version":"1.0"}})"}});
  const mcp::Reply<mcp::Client> legacy = open(script, options(mcp::Era::legacy));
  REQUIRE(legacy.has_value());
  CHECK(script->events() ==
        std::vector<std::string>{"send initialize", "send notifications/initialized"});
}

TEST_CASE("a session sets the level once it opens, if the server declared logging") {
  const std::string with_logging =
      R"({"capabilities":{"logging":{}},"protocolVersion":"2025-11-25",)"
      R"("serverInfo":{"name":"script","version":"1.0"}})";
  {
    Received received;
    const auto script = std::make_shared<Script>();
    script->respond =
        answering({{"initialize", with_logging}, {"logging/setLevel", "{}"}});
    mcp::ClientOptions with = logging(received, mcp::LogLevel::notice);
    with.era = mcp::Era::legacy;
    const mcp::Reply<mcp::Client> client = open(script, with);
    REQUIRE(client.has_value());
    const std::vector<Sent> sent = script->sent();
    REQUIRE(sent.size() == 3);
    CHECK(sent[1].method == "notifications/initialized");
    CHECK(sent[2].message == request("2", "logging/setLevel", R"({"level":"notice"})"));
    CHECK(sent[2].protocol_version == "2025-11-25");
  }
  {
    // A server that did not declare it is not asked.
    Received received;
    const auto script = std::make_shared<Script>();
    script->respond = answering({{"initialize", initialized("2025-11-25")}});
    mcp::ClientOptions with = logging(received);
    with.era = mcp::Era::legacy;
    const mcp::Reply<mcp::Client> client = open(script, with);
    REQUIRE(client.has_value());
    CHECK(script->sent().size() == 2);
  }
  {
    // One that refuses the level still opens.
    Received received;
    const auto script = std::make_shared<Script>();
    script->respond = [&with_logging](Script& server, const Sent& sent) {
      if (sent.method == "initialize") {
        server.deliver(support::result(sent.id, with_logging));
      } else if (sent.method == "logging/setLevel") {
        server.deliver(support::error(sent.id, -32601, "Method not found"));
      }
    };
    mcp::ClientOptions with = logging(received);
    with.era = mcp::Era::legacy;
    CHECK(open(script, with).has_value());
  }
  {
    // One that goes away does not.
    Received received;
    const auto script = std::make_shared<Script>();
    script->respond = [&with_logging](Script& server, const Sent& sent) {
      if (sent.method == "initialize") {
        server.deliver(support::result(sent.id, with_logging));
      } else if (sent.method == "logging/setLevel") {
        server.close({.fault = mcp::Fault::transport,
                      .code = 0,
                      .message = "the server exited",
                      .data = std::nullopt});
      }
    };
    mcp::ClientOptions with = logging(received);
    with.era = mcp::Era::legacy;
    const mcp::Reply<mcp::Client> client = open(script, with);
    REQUIRE_FALSE(client.has_value());
    CHECK(client.error().message == "the server exited");
  }
}

TEST_CASE("each log message reaches the handler as the server wrote it") {
  Received received;
  Opened opened = open_in(mcp::Era::modern, logging(received));
  REQUIRE(opened.client.has_value());
  for (const std::string_view params :
       {R"({"level":"error","logger":"database","data":{"error": "Connection failed",)"
        R"( "port": 5432}})",
        R"({"data":"plain","level":"info"})",
        // Under 2026-07-28 the server keeps to each request's level, so the
        // client passes on what it is sent.
        R"({"data":[1,2],"level":"debug","logger":7})",
        // Not log messages.
        R"({"level":"loud","data":1})", R"({"level":"info"})", R"({"data":1})",
        R"("info")", R"({"level":3,"data":1})"}) {
    opened.script->deliver(log_message(params));
  }
  opened.script->deliver(R"({"jsonrpc":"2.0","method":"notifications/message"})");

  const std::scoped_lock lock{received.mutex};
  REQUIRE(received.messages.size() == 3);
  CHECK(received.messages[0].level == mcp::LogLevel::error);
  CHECK(received.messages[0].logger == "database");
  CHECK(received.messages[0].data.text ==
        R"({"error": "Connection failed", "port": 5432})");
  CHECK(received.messages[1].level == mcp::LogLevel::info);
  CHECK(received.messages[1].logger.empty());
  CHECK(received.messages[1].data.text == R"("plain")");
  CHECK(received.messages[2].level == mcp::LogLevel::debug);
  CHECK(received.messages[2].logger.empty());
  CHECK(received.messages[2].data.text == "[1,2]");
}

TEST_CASE("a session passes over the messages less severe than its level") {
  Received received;
  mcp::ClientOptions with = logging(received, mcp::LogLevel::warning);
  Opened opened = open_in(mcp::Era::legacy, with);
  REQUIRE(opened.client.has_value());
  for (const std::string_view level : {"debug", "info", "notice", "warning", "error",
                                       "critical", "alert", "emergency"}) {
    opened.script->deliver(
        log_message(R"({"data":"x","level":")" + std::string{level} + R"("})"));
  }
  const std::scoped_lock lock{received.mutex};
  REQUIRE(received.messages.size() == 5);
  CHECK(received.messages.front().level == mcp::LogLevel::warning);
  CHECK(received.messages.back().level == mcp::LogLevel::emergency);
}

TEST_CASE("a log handler that throws harms neither the client nor its calls") {
  int calls = 0;
  mcp::ClientOptions with = options();
  with.log = [&calls](const mcp::LogMessage&) {
    ++calls;
    throw std::runtime_error{"the log is full"};
  };
  Opened opened = open_in(mcp::Era::modern, with);
  REQUIRE(opened.client.has_value());
  opened.script->respond = [](Script& server, const Sent& sent) {
    server.deliver(log_message(R"({"data":"one","level":"info"})"));
    server.deliver(log_message(R"({"data":"two","level":"info"})"));
    server.deliver(support::result(sent.id, one_text));
  };
  CHECK(opened.client->read<std::string>("a://b") == "hi");
  CHECK(calls == 2);
}

TEST_CASE("a log handler may run while calls wait, from several threads") {
  Received received;
  Opened opened = open_in(mcp::Era::modern, logging(received));
  REQUIRE(opened.client.has_value());
  constexpr int threads = 4;
  constexpr int messages = 50;
  std::vector<std::future<void>> senders;
  for (int thread = 0; thread < threads; ++thread) {
    senders.push_back(std::async(std::launch::async, [&opened] {
      for (int message = 0; message < messages; ++message) {
        opened.script->deliver(log_message(R"({"data":0,"level":"info"})"));
      }
    }));
  }
  for (std::future<void>& sender : senders) {
    REQUIRE(sender.wait_for(patience) == std::future_status::ready);
  }
  CHECK(received.size() == threads * messages);
}
