// A client and a server in one process, through the loopback channel: the
// server's resources, prompts, completion and log messages, as the client sees
// them in each era.
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <mcp/client.hpp>
#include <mcp/mcp.hpp>
#include <mutex>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace shelf {

struct Book {
  std::string title;
  std::int32_t year{};
};

[[= mcp::resource{.uri = "shelf://about",
                  .description = "What the shelf holds",
                  .name = "about"}]] inline std::string about() {
  return "Books and covers.";
}

[[= mcp::resource{.uri = "shelf://books/{id}",
                  .description = "One book",
                  .title = "Book"}]] inline mcp::Result<Book>
    book(const std::int32_t id) {
  if (id != 1) {
    return std::unexpected(mcp::tool_error("no book has that id"));
  }
  return Book{.title = "Dune", .year = 1965};
}

[[= mcp::resource{.uri = "shelf://covers/{id}",
                  .mime_type = "image/png"}]] inline std::vector<std::byte>
    cover(const std::int32_t id) {
  return {std::byte{static_cast<unsigned char>(id)}, std::byte{0xFF}, std::byte{0}};
}

[[= mcp::resource{.uri = "shelf://notes/{name}"}]] inline std::string
    note(std::string name) {
  return "note " + name;
}

[[= mcp::resource{.uri = "shelf://files/{+path}"}]] inline std::string
    file(std::string path) {
  return "file " + path;
}

[[= mcp::resource{.uri = "shelf://pairs/{left}-{right}"}]] inline std::string
    pair(std::string left, std::string right) {
  return left + "|" + right;
}

[[= mcp::resource{.uri = "shelf://shelves/{number}/{flag}"}]] inline std::string
    place(const std::int32_t number, const bool flag) {
  return std::to_string(number) + (flag ? " yes" : " no");
}

[[= mcp::resource{.uri = "shelf://log"}]] inline std::string noisy(mcp::Logger logger) {
  logger.debug("opening");
  logger.warning("dusty");
  return "read";
}

struct Name {
  std::string name;
};

[[= mcp::resource{.uri = "shelf://secret"}]] inline mcp::Result<std::string>
    secret(mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<Name>> answer = ask.form<Name>("Who reads it?");
  if (!answer) {
    return std::unexpected(answer.error());
  }
  return "secret for " + answer->content.value_or(Name{.name = "no one"}).name;
}

[[= mcp::prompt{"Recommend books"}]] inline std::string
    recommend([[= mcp::description{"A genre"}]] std::string genre,
              const std::int32_t count = 1) {
  return "Recommend " + std::to_string(count) + " " + genre + " books.";
}

[[= mcp::prompt{.description = "Talk about a book",
                .name = "discuss",
                .title = "Discuss"}]] inline std::vector<mcp::PromptMessage>
    talk() {
  return {{.role = mcp::Role::user, .content = mcp::TextContent{"Which book?"}},
          {.role = mcp::Role::assistant, .content = mcp::TextContent{"Dune."}}};
}

[[= mcp::prompt{"Greet the person"}]] inline mcp::Result<std::string>
    introduce(mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<Name>> answer = ask.form<Name>("Who are you?");
  if (!answer) {
    return std::unexpected(answer.error());
  }
  return "Hello " + answer->content.value_or(Name{.name = "stranger"}).name;
}

[[= mcp::prompt{"Chatter while rendering"}]] inline std::string
    chatter(mcp::Logger logger) {
  logger.info("rendering");
  return "chatter";
}

[[= mcp::tool{"Log at three levels"}]] inline std::string chat(mcp::Logger logger) {
  logger.debug("thinking");
  logger.info("working");
  logger.error("oh no");
  return "done";
}

} // namespace shelf

namespace {

constexpr std::array<mcp::Era, 3> eras{mcp::Era::modern, mcp::Era::legacy,
                                       mcp::Era::automatic};

mcp::Server shelf_server() {
  mcp::Server server{"shelf", "0.0.1"};
  server.add<^^shelf>();
  return server;
}

mcp::ClientOptions options(const mcp::Era era) {
  return {.name = "zoo", .version = "0.3.0", .era = era};
}

std::string text_of(const mcp::PromptMessage& message) {
  const auto* const text = std::get_if<mcp::TextContent>(&message.content);
  REQUIRE(text != nullptr);
  return text->text;
}

// The log messages a client has received.
struct Received {
  std::mutex mutex;
  std::vector<mcp::LogMessage> messages;

  void take(const mcp::LogMessage& message) {
    const std::scoped_lock lock{mutex};
    messages.push_back(message);
  }

  // Each message's level and data, and empties the list.
  std::vector<std::pair<mcp::LogLevel, std::string>> drain() {
    const std::scoped_lock lock{mutex};
    std::vector<std::pair<mcp::LogLevel, std::string>> seen;
    for (const mcp::LogMessage& message : messages) {
      seen.emplace_back(message.level, message.data.text);
    }
    messages.clear();
    return seen;
  }
};

mcp::ClientOptions logging(Received& received, const mcp::Era era,
                           const mcp::LogLevel level = mcp::LogLevel::info) {
  mcp::ClientOptions with = options(era);
  with.log = [&received](const mcp::LogMessage& message) { received.take(message); };
  with.log_level = level;
  return with;
}

using Logged = std::vector<std::pair<mcp::LogLevel, std::string>>;

} // namespace

TEST_CASE("the resources and templates are listed as the server binds them") {
  const mcp::Server server = shelf_server();
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());
    CHECK(client->server().has("resources"));
    CHECK(client->server().has("completions"));

    const mcp::Reply<std::vector<mcp::Resource>> resources = client->list_resources();
    REQUIRE(resources.has_value());
    const mcp::Reply<std::vector<mcp::ResourceTemplate>> templates =
        client->list_resource_templates();
    REQUIRE(templates.has_value());
    std::size_t fixed = 0;
    std::size_t templated = 0;
    for (const mcp::BoundResource& bound : server.resources()) {
      INFO(bound.uri);
      if (bound.is_template) {
        REQUIRE(templated < templates->size());
        const mcp::ResourceTemplate& listed = (*templates)[templated++];
        CHECK(listed.uri_template == bound.uri);
        CHECK(listed.name == bound.name);
        CHECK(listed.title == bound.title);
        CHECK(listed.description == bound.description);
        CHECK(listed.mime_type == bound.mime_type);
      } else {
        REQUIRE(fixed < resources->size());
        const mcp::Resource& listed = (*resources)[fixed++];
        CHECK(listed.uri == bound.uri);
        CHECK(listed.name == bound.name);
        CHECK(listed.description == bound.description);
        CHECK(listed.mime_type == bound.mime_type);
        CHECK_FALSE(listed.size.has_value());
        CHECK_FALSE(listed.annotations.has_value());
      }
    }
    CHECK(fixed == resources->size());
    CHECK(templated == templates->size());
    CHECK(resources->front().uri == "shelf://about");
    CHECK(resources->front().description == "What the shelf holds");
  }
}

TEST_CASE("a resource is read by its URI: text, a blob, or a value") {
  const mcp::Server server = shelf_server();
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());

    const mcp::Reply<std::vector<mcp::ResourceContents>> about =
        client->read_resource("shelf://about");
    REQUIRE(about.has_value());
    REQUIRE(about->size() == 1);
    CHECK(about->front().uri == "shelf://about");
    CHECK(about->front().mime_type == "text/plain");
    CHECK(about->front().text == "Books and covers.");
    CHECK_FALSE(about->front().blob.has_value());

    const mcp::Reply<std::vector<mcp::ResourceContents>> cover =
        client->read_resource("shelf://covers/7");
    REQUIRE(cover.has_value());
    REQUIRE(cover->size() == 1);
    CHECK(cover->front().mime_type == "image/png");
    CHECK(cover->front().blob == "B/8A");

    const mcp::Reply<std::vector<mcp::ResourceContents>> book =
        client->read_resource("shelf://books/1");
    REQUIRE(book.has_value());
    REQUIRE(book->size() == 1);
    CHECK(book->front().mime_type == "application/json");

    // By URI, with the caller's own types.
    CHECK(client->read<std::string>("shelf://about") == "Books and covers.");
    const mcp::Reply<shelf::Book> typed = client->read<shelf::Book>("shelf://books/1");
    REQUIRE(typed.has_value());
    CHECK(typed->title == "Dune");
    CHECK(typed->year == 1965);
    CHECK(client->read<std::vector<std::byte>>("shelf://covers/7") ==
          std::vector<std::byte>{std::byte{7}, std::byte{0xFF}, std::byte{0}});
    const mcp::Reply<mcp::ResourceContents> item =
        client->read<mcp::ResourceContents>("shelf://about");
    REQUIRE(item.has_value());
    CHECK(item->uri == "shelf://about");

    for (const mcp::Reply<std::int32_t>& mismatched :
         {client->read<std::int32_t>("shelf://about"),
          client->read<std::int32_t>("shelf://covers/7")}) {
      REQUIRE_FALSE(mismatched.has_value());
      CHECK(mismatched.error().fault == mcp::Fault::mismatch);
    }
    const mcp::Reply<std::vector<std::byte>> not_bytes =
        client->read<std::vector<std::byte>>("shelf://about");
    REQUIRE_FALSE(not_bytes.has_value());
    CHECK(not_bytes.error().fault == mcp::Fault::mismatch);
  }
}

TEST_CASE("a resource that is not there is rejected with the code of the era") {
  const mcp::Server server = shelf_server();
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());
    const int code = client->server().era == mcp::Era::modern ? -32602 : -32002;

    const mcp::Reply<std::vector<mcp::ResourceContents>> missing =
        client->read_resource("shelf://nowhere");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().fault == mcp::Fault::rejected);
    CHECK(missing.error().code == code);
    CHECK(missing.error().message == "Resource not found");
    REQUIRE(missing.error().data.has_value());
    CHECK(missing.error().data->text == R"({"uri":"shelf://nowhere"})");

    // A template's match that its function refuses is not there either.
    const mcp::Reply<shelf::Book> refused = client->read<^^shelf::book>({.id = 2});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().fault == mcp::Fault::rejected);
    CHECK(refused.error().code == code);
    CHECK(refused.error().message == "no book has that id");
  }
}

TEST_CASE(
    "a resource is read by its declaration, its URI filled in from the arguments") {
  const mcp::Server server = shelf_server();
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());

    CHECK(client->read<^^shelf::about>({}) == "Books and covers.");
    const mcp::Reply<shelf::Book> book = client->read<^^shelf::book>({.id = 1});
    REQUIRE(book.has_value());
    CHECK(book->title == "Dune");
    CHECK(client->read<^^shelf::cover>({.id = 9}) ==
          std::vector<std::byte>{std::byte{9}, std::byte{0xFF}, std::byte{0}});
    CHECK(client->read<^^shelf::place>({.number = -3, .flag = true}) == "-3 yes");
    CHECK(client->read<^^shelf::noisy>({}) == "read");
  }
}

TEST_CASE("awkward values travel through the server's matcher and back") {
  const mcp::Server server = shelf_server();
  mcp::Reply<mcp::Client> client = mcp::connect(server, options(mcp::Era::modern));
  REQUIRE(client.has_value());
  for (const std::string value :
       {"plain", "two words", "a/b", "100%", "%41", "q?x=1#top", "caf\xC3\xA9",
        "\xE5\x8C\x97\xE4\xBA\xAC", "a+b&c=d", "~._-", "{x}", "\"quoted\"\\"}) {
    INFO(value);
    CHECK(client->read<^^shelf::note>({.name = value}) == "note " + value);
    CHECK(client->read<^^shelf::file>({.path = value}) == "file " + value);
    CHECK(client->read<^^shelf::file>({.path = "dir/" + value + "/end"}) ==
          "file dir/" + value + "/end");
  }
  // Where the template's own text may be read as part of a value, the client
  // encodes it.
  CHECK(client->read<^^shelf::pair>({.left = "a-b", .right = "c"}) == "a-b|c");
  CHECK(client->read<^^shelf::pair>({.left = "a", .right = "b-c"}) == "a|b-c");
  CHECK(client->read<^^shelf::pair>({.left = "x-y-z", .right = "-"}) == "x-y-z|-");
}

TEST_CASE("arguments that a URI cannot carry are not sent") {
  const mcp::Server server = shelf_server();
  mcp::Reply<mcp::Client> client = mcp::connect(server, options(mcp::Era::modern));
  REQUIRE(client.has_value());
  const mcp::Reply<std::string> empty = client->read<^^shelf::note>({.name = ""});
  REQUIRE_FALSE(empty.has_value());
  CHECK(empty.error().fault == mcp::Fault::protocol);
  CHECK(empty.error().message.find("name") != std::string::npos);
}

TEST_CASE("the prompts are listed with their arguments") {
  const mcp::Server server = shelf_server();
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());
    const mcp::Reply<std::vector<mcp::Prompt>> prompts = client->list_prompts();
    REQUIRE(prompts.has_value());
    REQUIRE(prompts->size() == server.prompts().size());
    for (std::size_t index = 0; index < prompts->size(); ++index) {
      const mcp::Prompt& listed = (*prompts)[index];
      const mcp::BoundPrompt& bound = server.prompts()[index];
      INFO(bound.name);
      CHECK(listed.name == bound.name);
      CHECK(listed.title == bound.title);
      CHECK(listed.description == bound.description);
      REQUIRE(listed.arguments.size() == bound.arguments.size());
      for (std::size_t at = 0; at < listed.arguments.size(); ++at) {
        CHECK(listed.arguments[at].name == bound.arguments[at].name);
        CHECK(listed.arguments[at].description == bound.arguments[at].description);
        CHECK(listed.arguments[at].required == bound.arguments[at].required);
      }
    }
    const mcp::Prompt& recommend = prompts->front();
    REQUIRE(recommend.arguments.size() == 2);
    CHECK(recommend.arguments[0].name == "genre");
    CHECK(recommend.arguments[0].description == "A genre");
    CHECK(recommend.arguments[0].required);
    CHECK(recommend.arguments[1].name == "count");
    CHECK_FALSE(recommend.arguments[1].required);
  }
}

TEST_CASE("a prompt is rendered by name, with arguments as text") {
  const mcp::Server server = shelf_server();
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());

    const mcp::Reply<mcp::PromptResult> two =
        client->get_prompt("recommend", {{"genre", "sci-fi"}, {"count", "2"}});
    REQUIRE(two.has_value());
    CHECK(two->description == "Recommend books");
    REQUIRE(two->messages.size() == 1);
    CHECK(two->messages[0].role == mcp::Role::user);
    CHECK(text_of(two->messages[0]) == "Recommend 2 sci-fi books.");

    const mcp::Reply<mcp::PromptResult> one =
        client->get_prompt("recommend", {{"genre", "poetry"}});
    REQUIRE(one.has_value());
    CHECK(text_of(one->messages.at(0)) == "Recommend 1 poetry books.");

    const mcp::Reply<mcp::PromptResult> discuss = client->get_prompt("discuss");
    REQUIRE(discuss.has_value());
    REQUIRE(discuss->messages.size() == 2);
    CHECK(discuss->messages[1].role == mcp::Role::assistant);
    CHECK(text_of(discuss->messages[1]) == "Dune.");

    for (const auto& [name, arguments] :
         {std::pair<std::string, std::map<std::string, std::string>>{"recommend", {}},
          {"recommend", {{"genre", "x"}, {"count", "many"}}},
          {"fly", {}}}) {
      INFO(name);
      const mcp::Reply<mcp::PromptResult> refused = client->get_prompt(name, arguments);
      REQUIRE_FALSE(refused.has_value());
      CHECK(refused.error().fault == mcp::Fault::rejected);
      CHECK(refused.error().code == -32602);
    }
    CHECK(client->get_prompt("fly").error().message == "Unknown prompt: fly");
  }
}

TEST_CASE("a prompt is rendered by its declaration") {
  const mcp::Server server = shelf_server();
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());

    const mcp::Reply<mcp::PromptResult> three =
        client->prompt<^^shelf::recommend>({.genre = "crime", .count = 3});
    REQUIRE(three.has_value());
    CHECK(text_of(three->messages.at(0)) == "Recommend 3 crime books.");
    // A default argument left out is the server's to fill in.
    const mcp::Reply<mcp::PromptResult> one =
        client->prompt<^^shelf::recommend>({.genre = "crime", .count = std::nullopt});
    REQUIRE(one.has_value());
    CHECK(text_of(one->messages.at(0)) == "Recommend 1 crime books.");
    // Named by its annotation.
    const mcp::Reply<mcp::PromptResult> discuss = client->prompt<^^shelf::talk>({});
    REQUIRE(discuss.has_value());
    CHECK(discuss->description == "Talk about a book");
    CHECK(discuss->messages.size() == 2);
  }
}

TEST_CASE("completion is asked for a prompt's argument and a template's variable") {
  const mcp::Server server = shelf_server();
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());

    // The server suggests nothing of its own.
    const mcp::Reply<mcp::Completion> genre =
        client->complete(mcp::PromptReference{"recommend"}, "genre", "sc");
    REQUIRE(genre.has_value());
    CHECK(genre->values.empty());
    CHECK(genre->total == 0);
    CHECK_FALSE(genre->has_more);

    const mcp::Reply<mcp::Completion> id =
        client->complete(mcp::ResourceTemplateReference{"shelf://pairs/{left}-{right}"},
                         "right", "b", {{"left", "a"}});
    REQUIRE(id.has_value());
    CHECK(id->values.empty());

    const mcp::Reply<mcp::Completion> unknown =
        client->complete(mcp::PromptReference{"fly"}, "x", "");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().fault == mcp::Fault::rejected);
    CHECK(unknown.error().code == -32602);
  }
}

TEST_CASE("a resource and a prompt may ask the person, in each era") {
  const mcp::Server server = shelf_server();
  for (const mcp::Era era : eras) {
    std::vector<std::string> asked;
    mcp::ClientOptions with = options(era);
    with.elicitation = [&asked](const mcp::FormRequest& form) {
      asked.push_back(form.message);
      return mcp::Answer<scrydantic::Json>{.action = mcp::Action::accept,
                                           .content =
                                               scrydantic::Json{R"({"name":"Ada"})"}};
    };
    mcp::Reply<mcp::Client> client = mcp::connect(server, with);
    REQUIRE(client.has_value());

    CHECK(client->read<std::string>("shelf://secret") == "secret for Ada");
    CHECK(client->read<^^shelf::secret>({}) == "secret for Ada");
    const mcp::Reply<mcp::PromptResult> introduced =
        client->prompt<^^shelf::introduce>({});
    REQUIRE(introduced.has_value());
    CHECK(text_of(introduced->messages.at(0)) == "Hello Ada");
    CHECK(asked ==
          std::vector<std::string>{"Who reads it?", "Who reads it?", "Who are you?"});
  }
}

TEST_CASE("a client without a form handler cannot read what asks for one") {
  const mcp::Server server = shelf_server();
  for (const mcp::Era era : eras) {
    mcp::Reply<mcp::Client> client = mcp::connect(server, options(era));
    REQUIRE(client.has_value());
    const mcp::Reply<std::vector<mcp::ResourceContents>> secret =
        client->read_resource("shelf://secret");
    REQUIRE_FALSE(secret.has_value());
    CHECK(secret.error().fault == mcp::Fault::rejected);
    CHECK(secret.error().code == -32021);
  }
}

TEST_CASE("log messages reach the handler at the level the client asks for") {
  const mcp::Server server = shelf_server();
  for (const mcp::Era era : eras) {
    INFO(static_cast<int>(era));
    Received received;
    mcp::Reply<mcp::Client> client = mcp::connect(server, logging(received, era));
    REQUIRE(client.has_value());
    CHECK(client->server().has("logging"));
    const bool modern = client->server().era == mcp::Era::modern;

    REQUIRE(client->call_tool("chat").has_value());
    CHECK(received.drain() == Logged{{mcp::LogLevel::info, R"("working")"},
                                     {mcp::LogLevel::error, R"("oh no")"}});

    // One call asks for more, which only 2026-07-28 lets it do.
    REQUIRE(
        client->call_tool("chat", mcp::Json{"{}"}, {.log_level = mcp::LogLevel::debug})
            .has_value());
    if (modern) {
      CHECK(received.drain() == Logged{{mcp::LogLevel::debug, R"("thinking")"},
                                       {mcp::LogLevel::info, R"("working")"},
                                       {mcp::LogLevel::error, R"("oh no")"}});
    } else {
      CHECK(received.drain().size() == 2);
    }

    // A resource and a prompt log as a tool does.
    CHECK(client->read<^^shelf::noisy>({}) == "read");
    CHECK(received.drain() == Logged{{mcp::LogLevel::warning, R"("dusty")"}});
    REQUIRE(client->prompt<^^shelf::chatter>({}).has_value());
    CHECK(received.drain() == Logged{{mcp::LogLevel::info, R"("rendering")"}});
  }
}

TEST_CASE("a client that asks for severe messages gets only those") {
  const mcp::Server server = shelf_server();
  for (const mcp::Era era : eras) {
    Received received;
    mcp::Reply<mcp::Client> client =
        mcp::connect(server, logging(received, era, mcp::LogLevel::error));
    REQUIRE(client.has_value());
    REQUIRE(client->call_tool("chat").has_value());
    const std::scoped_lock lock{received.mutex};
    REQUIRE(received.messages.size() == 1);
    CHECK(received.messages[0].level == mcp::LogLevel::error);
    CHECK(received.messages[0].logger.empty());
    CHECK(received.messages[0].data.text == R"("oh no")");
  }
}

TEST_CASE("a log handler that throws does not harm the call") {
  const mcp::Server server = shelf_server();
  for (const mcp::Era era : eras) {
    int calls = 0;
    mcp::ClientOptions with = options(era);
    with.log = [&calls](const mcp::LogMessage&) {
      ++calls;
      throw std::runtime_error{"the log is full"};
    };
    mcp::Reply<mcp::Client> client = mcp::connect(server, with);
    REQUIRE(client.has_value());
    CHECK(client->call<std::string>("chat", mcp::Json{"{}"}) == "done");
    CHECK(calls == 2);
  }
}
