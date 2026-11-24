// Resources, resource templates, prompts, and completion, from annotated
// functions, in both eras.
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mcp/mcp.hpp>
#include <optional>
#include <string>
#include <vector>

using support::error;
using support::legacy;
using support::modern;
using support::result;
using support::server_info;

namespace library {

struct Book {
  std::string title;
  std::int32_t year{};
};

[[= mcp::resource{.uri = "books://catalog", .description = "Every title", .name = "catalog"}]]
inline std::string catalog() {
  return "Dune\nEmma";
}

[[= mcp::resource{.uri = "books://{id}", .description = "One book", .title = "Book"}]]
inline mcp::Result<Book> book(const std::int32_t id) {
  if (id != 1) {
    return std::unexpected(mcp::tool_error("no book has that id"));
  }
  return Book{.title = "Dune", .year = 1965};
}

[[= mcp::resource{.uri = "books://{id}/cover", .mime_type = "image/png"}]]
inline std::vector<std::byte> cover(const std::int32_t id) {
  return {std::byte{static_cast<unsigned char>(id)}, std::byte{0xFF}};
}

[[= mcp::resource{.uri = "files://{+path}"}]] inline std::string file(std::string path) {
  return "contents of " + path;
}

[[= mcp::prompt{"Recommend a book"}]] inline std::string
    recommend([[= mcp::description{"A genre"}]] std::string genre,
              const std::int32_t count = 1) {
  return "Recommend " + std::to_string(count) + " " + genre + " books.";
}

[[= mcp::prompt{.description = "Talk about a book", .name = "discuss", .title = "Discuss"}]]
inline std::vector<mcp::PromptMessage> talk() {
  return {{.role = mcp::Role::user, .content = mcp::TextContent{"Which book?"}},
          {.role = mcp::Role::assistant, .content = mcp::TextContent{"Dune."}}};
}

[[= mcp::tool{"Count the books"}]] inline std::int32_t count() { return 2; }

} // namespace library

namespace {

mcp::Server library_server(std::vector<std::string>& log) {
  mcp::Server server{"sample", "0.0.1"};
  server.log([&log](const std::string_view line) { log.emplace_back(line); });
  server.add<^^library>();
  return server;
}

std::string modern_result(const std::string_view id, const std::string_view members) {
  std::string text{"{"};
  text += server_info;
  text += ",";
  text += members;
  text += "}";
  return result(id, text);
}

} // namespace

TEST_CASE("a namespace's resources, prompts and tools are all added") {
  std::vector<std::string> log;
  const mcp::Server server = library_server(log);
  REQUIRE(server.status());
  CHECK(server.tools().size() == 1);
  CHECK(server.resources().size() == 4);
  CHECK(server.prompts().size() == 2);
  mcp::Connection connection{server};

  CHECK(connection.handle(modern("1", "server/discover")) ==
        modern_result("1", R"("cacheScope":"public","capabilities":{"completions":{},)"
                           R"("prompts":{},"resources":{},"tools":{}},"resultType":"complete",)"
                           R"("supportedVersions":["2026-07-28","2025-11-25","2025-06-18"],)"
                           R"("ttlMs":0)"));
}

TEST_CASE("resources/list lists the resources and the templates list the rest") {
  std::vector<std::string> log;
  const mcp::Server server = library_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(modern("1", "resources/list")) ==
        modern_result("1", R"("cacheScope":"public","resources":[{"description":"Every title",)"
                           R"("name":"catalog","uri":"books://catalog"}],"resultType":"complete",)"
                           R"("ttlMs":0)"));
  CHECK(connection.handle(modern("2", "resources/templates/list")) ==
        modern_result(
            "2", R"("cacheScope":"public","resourceTemplates":[)"
                 R"({"description":"One book","name":"book","title":"Book","uriTemplate":"books://{id}"},)"
                 R"({"mimeType":"image/png","name":"cover","uriTemplate":"books://{id}/cover"},)"
                 R"({"name":"file","uriTemplate":"files://{+path}"}],"resultType":"complete",)"
                 R"("ttlMs":0)"));
}

TEST_CASE("resources/read reads a resource, or a template's match") {
  std::vector<std::string> log;
  const mcp::Server server = library_server(log);
  mcp::Connection connection{server};

  const auto read = [](const std::string_view contents) {
    std::string members{R"("cacheScope":"public","contents":[)"};
    members += contents;
    members += R"(],"resultType":"complete","ttlMs":0)";
    return members;
  };
  CHECK(connection.handle(modern("1", "resources/read", R"("uri":"books://catalog")")) ==
        modern_result("1", read(R"({"mimeType":"text/plain","text":"Dune\nEmma","uri":"books://catalog"})")));
  // The variable `{id}` arrives as text and is read as an integer.
  CHECK(connection.handle(modern("2", "resources/read", R"("uri":"books://1")")) ==
        modern_result("2", read(R"({"mimeType":"application/json",)"
                                R"("text":"{\"title\":\"Dune\",\"year\":1965}","uri":"books://1"})")));
  CHECK(connection.handle(modern("3", "resources/read", R"("uri":"books://7/cover")")) ==
        modern_result("3", read(R"({"blob":"B/8=","mimeType":"image/png","uri":"books://7/cover"})")));
  // `{+path}` takes slashes, and `{id}` does not.
  CHECK(connection.handle(modern("4", "resources/read", R"("uri":"files://a/b.txt")")) ==
        modern_result("4", read(R"({"mimeType":"text/plain","text":"contents of a/b.txt",)"
                                R"("uri":"files://a/b.txt"})")));
}

TEST_CASE("a resource that is not there is invalid params, or its own code before "
          "2026-07-28") {
  std::vector<std::string> log;
  const mcp::Server server = library_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(modern("1", "resources/read", R"("uri":"books://none/x")")) ==
        R"({"error":{"code":-32602,"data":{"uri":"books://none/x"},"message":"Resource not found"},)"
        R"("id":1,"jsonrpc":"2.0"})");
  // The template matched, and the function said there is no such book.
  CHECK(connection.handle(modern("2", "resources/read", R"("uri":"books://9")")) ==
        R"({"error":{"code":-32602,"data":{"uri":"books://9"},"message":"no book has that id"},)"
        R"("id":2,"jsonrpc":"2.0"})");
  // A variable that does not read as its parameter's type.
  CHECK(connection.handle(modern("3", "resources/read", R"("uri":"books://abc")"))->starts_with(
      R"({"error":{"code":-32602,"data":{"uri":"books://abc"})"));

  REQUIRE(connection.handle(support::initialize("2025-11-25")).has_value());
  CHECK(connection.handle(legacy("4", "resources/read", R"({"uri":"books://none/x"})")) ==
        R"({"error":{"code":-32002,"data":{"uri":"books://none/x"},"message":"Resource not found"},)"
        R"("id":4,"jsonrpc":"2.0"})");
}

TEST_CASE("prompts/list gives each prompt's arguments") {
  std::vector<std::string> log;
  const mcp::Server server = library_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(modern("1", "prompts/list")) ==
        modern_result("1", R"("cacheScope":"public","prompts":[)"
                           R"({"arguments":[{"description":"A genre","name":"genre","required":true},)"
                           R"({"name":"count","required":false}],"description":"Recommend a book",)"
                           R"("name":"recommend"},)"
                           R"({"arguments":[],"description":"Talk about a book","name":"discuss",)"
                           R"("title":"Discuss"}],"resultType":"complete","ttlMs":0)"));
}

TEST_CASE("prompts/get renders a prompt from arguments that arrive as text") {
  std::vector<std::string> log;
  const mcp::Server server = library_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(modern("1", "prompts/get",
                                 R"("name":"recommend","arguments":{"genre":"sf","count":"3"})")) ==
        modern_result("1", R"("description":"Recommend a book","messages":[{"content":)"
                           R"({"text":"Recommend 3 sf books.","type":"text"},"role":"user"}],)"
                           R"("resultType":"complete")"));
  CHECK(connection.handle(modern("2", "prompts/get", R"("name":"discuss")")) ==
        modern_result("2", R"("description":"Talk about a book","messages":[)"
                           R"({"content":{"text":"Which book?","type":"text"},"role":"user"},)"
                           R"({"content":{"text":"Dune.","type":"text"},"role":"assistant"}],)"
                           R"("resultType":"complete")"));
}

TEST_CASE("a prompt that is not there, or a missing argument, is invalid params") {
  std::vector<std::string> log;
  const mcp::Server server = library_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(modern("1", "prompts/get", R"("name":"nope")")) ==
        error("1", -32602, "Unknown prompt: nope"));
  CHECK(connection.handle(modern("2", "prompts/get", R"("name":"recommend")")) ==
        error("2", -32602, "Invalid params: $.genre is a required member"));
}

TEST_CASE("completion/complete knows the server's prompts and templates") {
  std::vector<std::string> log;
  const mcp::Server server = library_server(log);
  mcp::Connection connection{server};

  CHECK(connection.handle(modern("1", "completion/complete",
                                 R"("ref":{"type":"ref/prompt","name":"recommend"},)"
                                 R"("argument":{"name":"genre","value":"s"})")) ==
        modern_result("1", R"("completion":{"hasMore":false,"total":0,"values":[]},)"
                           R"("resultType":"complete")"));
  CHECK(connection.handle(modern("2", "completion/complete",
                                 R"("ref":{"type":"ref/resource","uri":"books://{id}"},)"
                                 R"("argument":{"name":"id","value":"1"})"))
            ->find(R"("completion":)") != std::string::npos);
  CHECK(connection.handle(modern("3", "completion/complete",
                                 R"("ref":{"type":"ref/prompt","name":"nope"},)"
                                 R"("argument":{"name":"x","value":""})")) ==
        error("3", -32602, "Invalid params: ref names no prompt or resource of this server"));
}

TEST_CASE("a legacy session reads resources and renders prompts without the envelope") {
  std::vector<std::string> log;
  const mcp::Server server = library_server(log);
  mcp::Connection connection{server};
  const std::optional<std::string> initialized =
      connection.handle(support::initialize("2025-06-18"));
  REQUIRE(initialized.has_value());
  CHECK(initialized->find(R"("capabilities":{"completions":{},"prompts":{},)"
                          R"("resources":{"subscribe":true},"tools":{}})") != std::string::npos);

  CHECK(connection.handle(legacy("1", "resources/read", R"({"uri":"books://catalog"})")) ==
        result("1", R"({"contents":[{"mimeType":"text/plain","text":"Dune\nEmma",)"
                    R"("uri":"books://catalog"}]})"));
  CHECK(connection.handle(legacy("2", "prompts/get", R"({"name":"discuss"})"))
            ->starts_with(R"({"id":2,"jsonrpc":"2.0","result":{"description":"Talk about a book",)"
                          R"("messages":[)"));
}

TEST_CASE("two resources of one URI, or two prompts of one name, cannot be served") {
  std::vector<std::string> log;
  mcp::Server server = library_server(log);
  server.add<^^library::catalog>();
  CHECK(server.status().error().message == R"(two resources have the URI "books://catalog")");
}

TEST_CASE("a URI template matches whole segments") {
  CHECK(mcp::match_uri_template("a://{x}/b", "a://1/b") == R"({"x":"1"})");
  CHECK(mcp::match_uri_template("a://{x}/b", "a://1/2/b") == std::nullopt);
  CHECK(mcp::match_uri_template("a://{x}", "a://") == std::nullopt);
  CHECK(mcp::match_uri_template("a://{x}", "a://%41%20b") == R"({"x":"A b"})");
  CHECK(mcp::match_uri_template("a://{x}", "a://%4") == std::nullopt);
  CHECK(mcp::match_uri_template("a://{x}-{y}", "a://1-2") == R"({"x":"1","y":"2"})");
  CHECK(mcp::match_uri_template("a://{+p}", "a://x/y?z") == R"({"p":"x/y?z"})");
  CHECK(mcp::match_uri_template("a://b", "a://b/c") == std::nullopt);
}

TEST_CASE("a session subscribes to a resource and is told when it changes") {
  std::vector<std::string> log;
  const mcp::Server server = library_server(log);
  mcp::Connection connection{server};
  REQUIRE(connection.handle(support::initialize("2025-11-25")).has_value());
  std::vector<std::string> pushed;
  connection.push_to([&pushed](const std::string_view message) { pushed.emplace_back(message); });

  // No update before a subscription.
  server.resource_updated("books://catalog");
  CHECK(pushed.empty());

  CHECK(connection.handle(legacy("1", "resources/subscribe", R"({"uri":"books://catalog"})")) ==
        result("1", "{}"));
  server.resource_updated("books://catalog");
  server.resource_updated("books://1");
  CHECK(pushed == std::vector<std::string>{
                      R"({"jsonrpc":"2.0","method":"notifications/resources/updated",)"
                      R"("params":{"uri":"books://catalog"}})"});

  CHECK(connection.handle(legacy("2", "resources/unsubscribe", R"({"uri":"books://catalog"})")) ==
        result("2", "{}"));
  server.resource_updated("books://catalog");
  CHECK(pushed.size() == 1);

  // A connection that is gone is told nothing.
  connection.push_to({});
}

TEST_CASE("2026-07-28 has no resources/subscribe") {
  std::vector<std::string> log;
  const mcp::Server server = library_server(log);
  mcp::Connection connection{server};
  CHECK(connection.handle(modern("1", "resources/subscribe", R"("uri":"books://catalog")")) ==
        error("1", -32601, "Method not found: resources/subscribe"));
}
