// The stdio transport: one message per line, each way.
#include "support.hpp"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <mcp/mcp.hpp>
#include <mutex>
#include <semaphore>
#include <sstream>
#include <stop_token>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

using support::called;
using support::modern;
using support::text_content;

namespace slow {

// Released once by each call of wait_for_cancel when it starts.
inline std::counting_semaphore<> started{0};

[[= mcp::tool{"Wait until the call is cancelled"}]] inline std::string
    wait_for_cancel(std::stop_token stop) {
  started.release();
  std::mutex mutex;
  std::condition_variable_any never;
  std::unique_lock lock{mutex};
  const bool cancelled =
      never.wait_for(lock, stop, std::chrono::seconds{10}, [] { return false; }) ||
      stop.stop_requested();
  return cancelled ? "cancelled" : "not cancelled";
}

// How many calls of meet have begun.
inline std::atomic<int> arrived{0};

[[= mcp::tool{"Wait for a second call to arrive"}]] inline std::string meet() {
  arrived.fetch_add(1);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (arrived.load() < 2 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return arrived.load() >= 2 ? "together" : "alone";
}

[[= mcp::tool{"Add two integers"}]] inline std::int32_t add(std::int32_t a,
                                                            std::int32_t b) {
  return a + b;
}

} // namespace slow

namespace {

// An input stream that a test writes to while the server reads it, and closes
// to end it.
class Feed : public std::streambuf {
public:
  void push(const std::string_view text) {
    {
      const std::scoped_lock lock{mutex_};
      pending_ += text;
      pending_ += '\n';
    }
    changed_.notify_all();
  }

  void close() {
    {
      const std::scoped_lock lock{mutex_};
      closed_ = true;
    }
    changed_.notify_all();
  }

protected:
  int_type underflow() override {
    std::unique_lock lock{mutex_};
    changed_.wait(lock, [this] { return closed_ || !pending_.empty(); });
    if (pending_.empty()) {
      return traits_type::eof();
    }
    current_ = std::move(pending_);
    pending_.clear();
    setg(current_.data(), current_.data(), current_.data() + current_.size());
    return traits_type::to_int_type(current_.front());
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::string pending_;
  std::string current_;
  bool closed_{};
};

mcp::Server slow_server() {
  mcp::Server server{"sample", "0.0.1"};
  server.add<^^slow>();
  return server;
}

std::string cancelled(const std::string_view id) {
  std::string text{R"({"jsonrpc":"2.0","method":"notifications/cancelled",)"};
  text += R"("params":{"requestId":)";
  text += id;
  text += R"(,"reason":"no longer needed"}})";
  return text;
}

// The lines a server wrote, in sorted order, since calls may finish in any.
std::vector<std::string> sorted_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in{text};
  for (std::string line; std::getline(in, line);) {
    lines.push_back(line);
  }
  std::ranges::sort(lines);
  return lines;
}

std::vector<std::string> sorted(std::vector<std::string> lines) {
  std::ranges::sort(lines);
  return lines;
}

} // namespace

TEST_CASE("serve_stdio answers each line with one line and ends with the input") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);

  // A blank line and a carriage return before the line feed are tolerated,
  // and a notification gets no line back.
  std::istringstream in{
      modern("1", "tools/call", R"("name":"add","arguments":{"a":2,"b":3})") + "\n\n" +
      R"({"jsonrpc":"2.0","method":"notifications/initialized"})" + "\n" +
      support::legacy("2", "ping") + "\r\n" + "nonsense\n" +
      modern("3", "tools/call", R"("name":"greet")")};
  std::ostringstream out;

  CHECK(mcp::serve_stdio(server, in, out) == 0);
  CHECK(sorted_lines(out.str()) ==
        sorted({called("1", text_content("5", false), "5"), support::result("2", "{}"),
                support::error("", -32700,
                               "Parse error: JSON text is not valid: expected a value at "
                               "line 1, column 1"),
                called("3", text_content("hello world", false), R"("hello world")")}));
}

TEST_CASE("serve_stdio answers the requests in order when it runs one at a time") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);

  std::istringstream in{
      modern("1", "tools/call", R"("name":"add","arguments":{"a":2,"b":3})") + "\n" +
      support::legacy("2", "ping") + "\n" + modern("3", "tools/call", R"("name":"greet")")};
  std::ostringstream out;

  CHECK(mcp::serve_stdio(server, in, out, {.concurrency = 1}) == 0);
  CHECK(out.str() ==
        called("1", text_content("5", false), "5") + "\n" + support::result("2", "{}") +
            "\n" + called("3", text_content("hello world", false), R"("hello world")") +
            "\n");
}

TEST_CASE("a stop token parameter is not an argument") {
  const mcp::Server server = slow_server();
  mcp::Connection connection{server};
  const std::optional<std::string> listed = connection.handle(modern("1", "tools/list"));
  REQUIRE(listed.has_value());
  CHECK(listed->find(R"("name":"wait_for_cancel")") != std::string::npos);
  CHECK(listed->find(R"("inputSchema":{"additionalProperties":false,"properties":{},"required":[],"type":"object"})") !=
        std::string::npos);
}

TEST_CASE("a call receives the stop token its transport passed") {
  const mcp::Server server = slow_server();
  mcp::Connection connection{server};
  std::stop_source stop;
  stop.request_stop();

  CHECK(connection.handle(modern("1", "tools/call", R"("name":"wait_for_cancel")"), {},
                          stop.get_token()) ==
        called("1", text_content("cancelled", false), R"("cancelled")"));
  CHECK(slow::started.try_acquire());
}

TEST_CASE("serve_stdio stops a running call that the client cancels, and sends no "
          "response for it") {
  const mcp::Server server = slow_server();
  Feed feed;
  std::istream in{&feed};
  std::ostringstream out;
  std::jthread serving{[&] { CHECK(mcp::serve_stdio(server, in, out) == 0); }};

  feed.push(modern("1", "tools/call", R"("name":"wait_for_cancel")"));
  REQUIRE(slow::started.try_acquire_for(std::chrono::seconds{10}));
  // Another call is served while the first one runs.
  feed.push(modern("2", "tools/call", R"("name":"add","arguments":{"a":1,"b":1})"));
  feed.push(cancelled("1"));
  feed.close();
  serving.join();

  CHECK(out.str() == called("2", text_content("2", false), "2") + "\n");
}

TEST_CASE("serve_stdio cancels a request that is still waiting for its turn") {
  const mcp::Server server = slow_server();
  Feed feed;
  std::istream in{&feed};
  std::ostringstream out;
  std::jthread serving{
      [&] { CHECK(mcp::serve_stdio(server, in, out, {.concurrency = 1}) == 0); }};

  feed.push(modern(R"("a")", "tools/call", R"("name":"wait_for_cancel")"));
  REQUIRE(slow::started.try_acquire_for(std::chrono::seconds{10}));
  feed.push(modern(R"("b")", "tools/call", R"("name":"wait_for_cancel")"));
  feed.push(cancelled(R"("b")"));
  feed.push(cancelled(R"("a")"));
  feed.close();
  serving.join();

  CHECK(out.str().empty());
  // The second call never ran.
  CHECK_FALSE(slow::started.try_acquire());
}

TEST_CASE("serve_stdio runs calls side by side") {
  const mcp::Server server = slow_server();
  std::istringstream in{modern("1", "tools/call", R"("name":"meet")") + "\n" +
                        modern("2", "tools/call", R"("name":"meet")") + "\n"};
  std::ostringstream out;

  CHECK(mcp::serve_stdio(server, in, out) == 0);
  CHECK(sorted_lines(out.str()) ==
        sorted({called("1", text_content("together", false), R"("together")"),
                called("2", text_content("together", false), R"("together")")}));
}

TEST_CASE("a cancellation of initialize, or of a request that is not running, is "
          "ignored") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  std::istringstream in{cancelled("7") + "\n" + R"({"jsonrpc":"2.0","method":"notifications/cancelled"})" +
                        "\n" + support::initialize("2025-11-25") + "\n"};
  std::ostringstream out;

  CHECK(mcp::serve_stdio(server, in, out) == 0);
  CHECK(out.str().starts_with(R"({"id":0,"jsonrpc":"2.0","result":{"capabilities")"));
}

TEST_CASE("serve_stdio writes a call's progress before its response") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);

  std::istringstream in{
      R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"count",)"
      R"("arguments":{"steps":1},"_meta":{"progressToken":"t",)"
      R"("io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
      R"("io.modelcontextprotocol/clientCapabilities":{}}}})"
      "\n"};
  std::ostringstream out;

  CHECK(mcp::serve_stdio(server, in, out) == 0);
  CHECK(out.str() ==
        std::string{R"({"jsonrpc":"2.0","method":"notifications/progress",)"
                    R"("params":{"progress":1,"progressToken":"t","total":1}})"} +
            "\n" + called("1", text_content("1", false), "1") + "\n");
}

TEST_CASE("serve_stdio writes no line that holds a line break") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);

  std::istringstream in{
      modern("1", "tools/call", R"("name":"greet","arguments":{"who":"a\nb\r\nc"})") +
      "\n"};
  std::ostringstream out;

  CHECK(mcp::serve_stdio(server, in, out) == 0);
  const std::string written = out.str();
  REQUIRE_FALSE(written.empty());
  CHECK(written.find('\n') == written.size() - 1);
  CHECK(written.find('\r') == std::string::npos);
}

TEST_CASE("serve_stdio does not serve a server that cannot be served") {
  std::vector<std::string> log;
  mcp::Server server = support::sample_server(log);
  server.add<^^one_tool>(); // A second `add`.

  std::istringstream in{modern("1", "tools/list") + "\n"};
  std::ostringstream out;

  CHECK(mcp::serve_stdio(server, in, out) == 1);
  CHECK(out.str().empty());
  CHECK(log == std::vector<std::string>{R"(two tools are named "add")"});
}

namespace asks_on_stdio {

struct Name {
  std::string name;
};

[[= mcp::tool{"Ask a name"}]] inline mcp::Result<std::string> who(mcp::Elicitation ask) {
  mcp::Result<mcp::Answer<Name>> name = ask.form<Name>("Who?");
  if (!name) {
    return std::unexpected(name.error());
  }
  return name->content->name;
}

} // namespace asks_on_stdio

TEST_CASE("serve_stdio returns when the input ends while a call waits for the client") {
  mcp::Server server{"sample", "0.0.1"};
  server.add<^^asks_on_stdio>();
  std::istringstream in{
      support::legacy("0", "initialize",
                      R"({"protocolVersion":"2025-11-25","capabilities":{"elicitation":{}},)"
                      R"("clientInfo":{"name":"t","version":"1"}})") +
      "\n" + support::legacy("1", "tools/call", R"({"name":"who"})") + "\n"};
  std::ostringstream out;
  CHECK(mcp::serve_stdio(server, in, out, {.concurrency = 1}) == 0);
  // The client had gone before the call ran, so it was not asked.
  CHECK(out.str().find(R"("isError":true)") != std::string::npos);
}

namespace {

// An output stream that a test can wait on for some text.
class Watched : public std::streambuf {
public:
  bool wait_for(const std::string_view text) {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, std::chrono::seconds{10},
                             [&] { return written_.find(text) != std::string::npos; });
  }

  std::string text() {
    const std::scoped_lock lock{mutex_};
    return written_;
  }

protected:
  std::streamsize xsputn(const char* data, const std::streamsize size) override {
    {
      const std::scoped_lock lock{mutex_};
      written_.append(data, static_cast<std::size_t>(size));
    }
    changed_.notify_all();
    return size;
  }

  int_type overflow(const int_type c) override {
    if (!traits_type::eq_int_type(c, traits_type::eof())) {
      const char letter = traits_type::to_char_type(c);
      xsputn(&letter, 1);
    }
    return c;
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::string written_;
};

} // namespace

TEST_CASE("a call that waits for the client gives up when the input ends") {
  mcp::Server server{"sample", "0.0.1"};
  server.add<^^asks_on_stdio>();
  Feed feed;
  std::istream in{&feed};
  Watched watched;
  std::ostream out{&watched};
  std::jthread serving{[&] { CHECK(mcp::serve_stdio(server, in, out) == 0); }};

  // The input ends however the test does, so that the server returns.
  struct Closing {
    Feed& feed;
    ~Closing() { feed.close(); }
  } closing{feed};

  feed.push(support::legacy("0", "initialize",
                            R"({"protocolVersion":"2025-11-25","capabilities":{"elicitation":{}},)"
                            R"("clientInfo":{"name":"t","version":"1"}})"));
  // A client waits for the handshake to finish before it calls.
  REQUIRE(watched.wait_for(R"("protocolVersion":"2025-11-25")"));
  feed.push(support::legacy("1", "tools/call", R"({"name":"who"})"));
  REQUIRE(watched.wait_for(R"("method":"elicitation/create")"));
  feed.close();
  serving.join();
  CHECK(watched.text().find(R"("isError":true)") != std::string::npos);
}
