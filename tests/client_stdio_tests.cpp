// The client's stdio transport: over a pair of streams to serve_stdio() in the
// same process, and over a server it launches as a child process.
#include "programs.hpp"
#include "support.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <mcp/client.hpp>
#include <mcp/mcp.hpp>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <streambuf>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

namespace piped {

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

// hold() releases `held` when it starts, and returns once the test releases
// `let_go`.
inline std::counting_semaphore<> held{0};
inline std::counting_semaphore<> let_go{0};

[[= mcp::tool{"Wait until the test lets go"}]] inline std::string hold() {
  held.release();
  return let_go.try_acquire_for(std::chrono::seconds{10}) ? "let go" : "timed out";
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

} // namespace piped

namespace {

constexpr auto patience = 10s;

constexpr std::array<mcp::Era, 2> eras{mcp::Era::modern, mcp::Era::legacy};

mcp::ClientOptions options(const mcp::Era era) {
  return {.name = "piped", .version = "0.2.0", .era = era};
}

std::string text_of(const mcp::Content& content) {
  REQUIRE(content.size() == 1);
  const auto* const text = std::get_if<mcp::TextContent>(&content.front());
  REQUIRE(text != nullptr);
  return text->text;
}

// One direction of a connection between two threads: what one writes, the
// other reads, until it is closed. Only one thread reads.
class Pipe : public std::streambuf {
public:
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

  std::streamsize xsputn(const char* const data, const std::streamsize size) override {
    {
      const std::scoped_lock lock{mutex_};
      // What is written once the pipe is closed goes nowhere.
      if (!closed_) {
        pending_.append(data, static_cast<std::size_t>(size));
      }
    }
    changed_.notify_all();
    return size;
  }

  int_type overflow(const int_type c) override {
    if (!traits_type::eq_int_type(c, traits_type::eof())) {
      const char letter = traits_type::to_char_type(c);
      xsputn(&letter, 1);
    }
    return traits_type::not_eof(c);
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::string pending_;
  std::string current_;
  bool closed_{};
};

// A pair of pipes, as a client and a server in one process use them.
struct Wires {
  Pipe up;
  Pipe down;
  std::istream up_in{&up};
  std::ostream up_out{&up};
  std::istream down_in{&down};
  std::ostream down_out{&down};
};

// A server served by serve_stdio() on a thread of its own, and a client
// connected to it over streams.
class Served {
public:
  Served(const mcp::Server& server, mcp::ClientOptions options)
      : serving_{[this, &server] {
          mcp::serve_stdio(server, wires_.up_in, wires_.down_out);
          wires_.down.close();
        }} {
    client_.emplace(mcp::connect(wires_.down_in, wires_.up_out, std::move(options)));
  }

  Served(const Served&) = delete;
  Served& operator=(const Served&) = delete;

  // The server's input ends, so serve_stdio() returns and ends its output,
  // which ends the client's reader, so that the client can close.
  ~Served() {
    wires_.up.close();
    client_.reset();
  }

  mcp::Reply<mcp::Client>& client() { return *client_; }
  Wires& wires() { return wires_; }

private:
  Wires wires_;
  std::optional<mcp::Reply<mcp::Client>> client_;
  std::jthread serving_;
};

mcp::Server piped_server(std::vector<std::string>& log) {
  mcp::Server server = support::sample_server(log);
  server.add<^^piped>();
  return server;
}

// The answer of a modern server to the client's first request.
constexpr std::string_view discovered =
    R"({"jsonrpc":"2.0","id":1,"result":{"supportedVersions":["2026-07-28"],)"
    R"("capabilities":{"tools":{}},)"
    R"("_meta":{"io.modelcontextprotocol/serverInfo":{"name":"script","version":"1"}}}})";

} // namespace

// Over a pair of streams.

TEST_CASE("a client over streams opens in either era, lists the tools and calls them") {
  std::vector<std::string> log;
  const mcp::Server server = piped_server(log);
  struct Case {
    mcp::Era asked;
    mcp::Era got;
    std::string_view version;
  };
  for (const Case& each : {Case{mcp::Era::automatic, mcp::Era::modern, "2026-07-28"},
                           Case{mcp::Era::modern, mcp::Era::modern, "2026-07-28"},
                           Case{mcp::Era::legacy, mcp::Era::legacy, "2025-11-25"}}) {
    Served served{server, options(each.asked)};
    mcp::Reply<mcp::Client>& client = served.client();
    REQUIRE(client.has_value());
    CHECK(client->server().era == each.got);
    CHECK(client->server().protocol_version == each.version);
    CHECK(client->server().name == "sample");
    CHECK(client->ping().has_value());

    const mcp::Reply<std::vector<mcp::Tool>> tools = client->list_tools();
    REQUIRE(tools.has_value());
    CHECK(tools->size() == server.tools().size());

    const mcp::Reply<mcp::ToolResult> added =
        client->call_tool("add", mcp::Json{R"({"a":2,"b":3})"});
    REQUIRE(added.has_value());
    CHECK(text_of(added->content) == "5");
    CHECK(client->call<^^sample::greet>({.who = "Ada"}) == "hello Ada");
    const mcp::Reply<void> refused = client->call<^^sample::refuse>({});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().fault == mcp::Fault::tool);

    std::vector<double> reports;
    CHECK(client->call<^^sample::count>(
              {.steps = 2}, {.progress = [&reports](const mcp::ProgressReport& report) {
                reports.push_back(report.progress);
              }}) == 2);
    CHECK(reports == std::vector<double>{1, 2});
  }
}

TEST_CASE("several threads call one client over streams at once") {
  std::vector<std::string> log;
  const mcp::Server server = piped_server(log);
  for (const mcp::Era era : eras) {
    Served served{server, options(era)};
    REQUIRE(served.client().has_value());
    mcp::Client& client = *served.client();
    constexpr int callers = 4;
    {
      const std::scoped_lock lock{piped::meeting};
      piped::arrived = 0;
      piped::expected = callers;
    }
    std::vector<std::future<mcp::Reply<std::string>>> calls;
    for (int caller = 0; caller < callers; ++caller) {
      calls.push_back(std::async(std::launch::async,
                                 [&client] { return client.call<^^piped::meet>({}); }));
    }
    for (std::future<mcp::Reply<std::string>>& call : calls) {
      REQUIRE(call.wait_for(patience) == std::future_status::ready);
      CHECK(call.get() == "together");
    }
  }
}

TEST_CASE("cancelling a call over streams stops the tool that runs it") {
  std::vector<std::string> log;
  const mcp::Server server = piped_server(log);
  for (const mcp::Era era : eras) {
    Served served{server, options(era)};
    REQUIRE(served.client().has_value());
    mcp::Client& client = *served.client();
    std::stop_source stop;
    auto calling = std::async(std::launch::async, [&] {
      return client.call<^^piped::wait_for_cancel>(
          {}, {.progress = {}, .stop = stop.get_token(), .timeout = {}});
    });
    REQUIRE(piped::started.try_acquire_for(patience));
    stop.request_stop();
    REQUIRE(calling.wait_for(patience) == std::future_status::ready);
    const mcp::Reply<std::string> called = calling.get();
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().fault == mcp::Fault::cancelled);
    CHECK(piped::stopped.try_acquire_for(patience));
    // The server goes on.
    CHECK(client.call<^^sample::add>({.a = 1, .b = 1}) == 2);
  }
}

TEST_CASE(
    "the end of the server's stream fails the waiting call, and every later one") {
  std::vector<std::string> log;
  const mcp::Server server = piped_server(log);
  for (const mcp::Era era : eras) {
    Served served{server, options(era)};
    REQUIRE(served.client().has_value());
    mcp::Client& client = *served.client();
    auto calling =
        std::async(std::launch::async, [&] { return client.call<^^piped::hold>({}); });
    REQUIRE(piped::held.try_acquire_for(patience));
    served.wires().down.close();
    REQUIRE(calling.wait_for(patience) == std::future_status::ready);
    const mcp::Reply<std::string> called = calling.get();
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().fault == mcp::Fault::transport);
    CHECK(called.error().message == "the server's stream ended");

    const mcp::Reply<void> later = client.ping();
    REQUIRE_FALSE(later.has_value());
    CHECK(later.error().fault == mcp::Fault::transport);
    // The server's call finishes, so that serve_stdio() can return.
    piped::let_go.release();
  }
}

TEST_CASE("a line longer than the bound ends a client over streams") {
  Wires wires;
  // The peer answers discovery after a line too long, and then ends.
  std::jthread peer{[&wires] {
    std::string line;
    if (std::getline(wires.up_in, line)) {
      wires.down_out << std::string(65, 'x') << '\n'
                     << discovered << '\n'
                     << std::flush;
    }
    wires.down.close();
  }};
  const mcp::Reply<mcp::Client> client =
      mcp::connect(wires.down_in, wires.up_out, options(mcp::Era::modern), 64);
  REQUIRE_FALSE(client.has_value());
  CHECK(client.error().fault == mcp::Fault::transport);
  CHECK(client.error().message ==
        "the server sent a line longer than 64 bytes, the most this client reads");
}

TEST_CASE("a line as long as the bound is read, without its line break") {
  Wires wires;
  std::string answer{discovered};
  const std::size_t bound = answer.size() + 10;
  // Space before the last brace is still the same JSON.
  answer.insert(answer.size() - 1, std::string(10, ' '));
  REQUIRE(answer.size() == bound);
  std::jthread peer{[&wires, &answer] {
    std::string line;
    if (std::getline(wires.up_in, line)) {
      wires.down_out << answer << "\r\n" << std::flush;
    }
    wires.down.close();
  }};
  const mcp::Reply<mcp::Client> client =
      mcp::connect(wires.down_in, wires.up_out, options(mcp::Era::modern), bound);
  REQUIRE(client.has_value());
  CHECK(client->server().name == "script");
}

TEST_CASE("closing a client over streams waits for the server's stream to end") {
  Wires wires;
  std::jthread peer{[&wires] {
    std::string line;
    if (std::getline(wires.up_in, line)) {
      wires.down_out << discovered << '\n' << std::flush;
    }
  }};
  mcp::Reply<mcp::Client> client =
      mcp::connect(wires.down_in, wires.up_out, options(mcp::Era::modern));
  REQUIRE(client.has_value());
  // The client is destroyed in the task, whose read of the stream goes on.
  auto closing = std::async(
      std::launch::async, [&client] { const mcp::Client closed = std::move(*client); });
  CHECK(closing.wait_for(200ms) == std::future_status::timeout);
  wires.down.close();
  REQUIRE(closing.wait_for(patience) == std::future_status::ready);
}

// Over a child process.

namespace {

// Skips a test that needs the example server when the examples are not built.
std::string weather_example() {
  if (programs::weather.empty()) {
    SKIP("the examples are not built");
  }
  return std::string{programs::weather};
}

// A server written in sh, which answers the requests it reads, in order, with
// `answers`, one line each, and then runs `then`.
mcp::Command scripted(const std::vector<std::string>& answers,
                      const std::string_view then = "exec cat >/dev/null") {
  std::string script{R"(for answer in "$@"; do IFS= read -r line || exit 0; )"
                     R"(printf '%s\n' "$answer"; done; )"};
  script += then;
  mcp::Command command{.command = "/bin/sh", .arguments = {"-c", script, "sh"}};
  command.arguments.insert(command.arguments.end(), answers.begin(), answers.end());
  return command;
}

// A command that runs `script` in sh and then becomes the example server.
mcp::Command before_weather(const std::string_view script) {
  return {
      .command = "/bin/sh",
      .arguments = {"-c", std::string{script} + R"(; exec "$0")", weather_example()}};
}

// The lines a server writes to its stderr, as Stderr::lines hands them over.
class Collected {
public:
  std::function<void(std::string_view)> sink() {
    return [this](const std::string_view line) {
      {
        const std::scoped_lock lock{mutex_};
        lines_.emplace_back(line);
      }
      changed_.notify_all();
    };
  }

  bool wait_for(const std::size_t count) {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, patience, [&] { return lines_.size() >= count; });
  }

  std::vector<std::string> lines() {
    const std::scoped_lock lock{mutex_};
    return lines_;
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<std::string> lines_;
};

// A directory of the test's own, removed afterwards.
struct TemporaryDirectory {
  TemporaryDirectory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "mcp-stdio-XXXXXX").string();
    REQUIRE(::mkdtemp(pattern.data()) != nullptr);
    path = std::filesystem::canonical(pattern);
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

std::string contents_of(const std::filesystem::path& path) {
  std::ifstream in{path};
  return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// The test program's stderr, sent to a file until this is destroyed.
class StderrToFile {
public:
  explicit StderrToFile(const std::filesystem::path& path)
      : saved_{::dup(STDERR_FILENO)} {
    const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    REQUIRE(file >= 0);
    REQUIRE(::dup2(file, STDERR_FILENO) == STDERR_FILENO);
    ::close(file);
  }
  StderrToFile(const StderrToFile&) = delete;
  StderrToFile& operator=(const StderrToFile&) = delete;
  ~StderrToFile() {
    ::dup2(saved_, STDERR_FILENO);
    ::close(saved_);
  }

private:
  int saved_;
};

int open_descriptors() {
  int count = 0;
  for (int fd = 0; fd < 1024; ++fd) {
    if (::fcntl(fd, F_GETFD) != -1) {
      ++count;
    }
  }
  return count;
}

// How long `body` takes.
template <typename Body> std::chrono::steady_clock::duration timed(Body&& body) {
  const auto start = std::chrono::steady_clock::now();
  body();
  return std::chrono::steady_clock::now() - start;
}

struct Forecast {
  double temperature{};
  std::string sky;
};

struct City {
  std::string city;
  std::int32_t days{};
};

} // namespace

TEST_CASE("a launched server opens in either era and serves its tools") {
  const std::string weather = weather_example();
  struct Case {
    mcp::Era asked;
    mcp::Era got;
    std::string_view version;
  };
  for (const Case& each : {Case{mcp::Era::automatic, mcp::Era::modern, "2026-07-28"},
                           Case{mcp::Era::modern, mcp::Era::modern, "2026-07-28"},
                           Case{mcp::Era::legacy, mcp::Era::legacy, "2025-11-25"}}) {
    mcp::Reply<mcp::Client> client =
        mcp::launch({.command = weather}, options(each.asked));
    REQUIRE(client.has_value());
    const mcp::ServerInfo& info = client->server();
    CHECK(info.name == "weather");
    CHECK(info.version == "1.0.0");
    CHECK(info.era == each.got);
    CHECK(info.protocol_version == each.version);
    CHECK(info.instructions == "Forecasts for a few cities. Call cities first.");
    CHECK(info.has("tools"));

    const mcp::Reply<std::vector<mcp::Tool>> tools = client->list_tools();
    REQUIRE(tools.has_value());
    std::vector<std::string> names;
    for (const mcp::Tool& tool : *tools) {
      names.push_back(tool.name);
    }
    CHECK(names == std::vector<std::string>{"forecast", "cities", "download_archive",
                                            "set_home", "watch"});

    const mcp::Reply<mcp::ToolResult> called =
        client->call_tool("forecast", mcp::Json{R"({"city":"Oslo","days":2})"});
    REQUIRE(called.has_value());
    CHECK(text_of(called->content) == R"({"sky":"clear","temperature":19.5})");
    const mcp::Reply<Forecast> typed =
        client->call<Forecast>("forecast", City{.city = "Oslo", .days = 5});
    REQUIRE(typed.has_value());
    CHECK(typed->sky == "rain");
    CHECK(typed->temperature == 16.5);
    const mcp::Reply<mcp::ToolResult> refused =
        client->call_tool("forecast", mcp::Json{R"({"city":""})"});
    REQUIRE(refused.has_value());
    CHECK(refused->is_error);

    std::vector<mcp::ProgressReport> reports;
    const mcp::Reply<std::string> archive = client->call<std::string>(
        "download_archive", mcp::Json{R"({"city":"Lima"})"},
        {.progress = [&reports](const mcp::ProgressReport& report) {
          reports.push_back(report);
        }});
    CHECK(archive == "archive of Lima is ready");
    REQUIRE(reports.size() == 3);
    CHECK(reports.back().progress == 3);
    CHECK(reports.back().message == "downloading");

    std::stop_source stop;
    mcp::Client& open = *client;
    auto watching = std::async(std::launch::async, [&] {
      return open.call_tool("watch", mcp::Json{R"({"city":"Lima","seconds":60})"},
                            {.progress = {}, .stop = stop.get_token(), .timeout = {}});
    });
    stop.request_stop();
    REQUIRE(watching.wait_for(patience) == std::future_status::ready);
    const mcp::Reply<mcp::ToolResult> watched = watching.get();
    REQUIRE_FALSE(watched.has_value());
    CHECK(watched.error().fault == mcp::Fault::cancelled);
    CHECK(client->ping().has_value());
  }
}

TEST_CASE("several threads call one launched server at once") {
  const std::string weather = weather_example();
  mcp::Reply<mcp::Client> client = mcp::launch({.command = weather});
  REQUIRE(client.has_value());
  mcp::Client& open = *client;
  std::vector<std::future<bool>> callers;
  for (int caller = 0; caller < 8; ++caller) {
    callers.push_back(std::async(std::launch::async, [&open, caller] {
      bool right = true;
      for (int call = 0; call < 5; ++call) {
        const std::int32_t days = 1 + ((caller + call) % 7);
        const mcp::Reply<Forecast> forecast =
            open.call<Forecast>("forecast", City{.city = "Oslo", .days = days});
        right = right && forecast.has_value() && forecast->temperature == 21.5 - days;
      }
      return right;
    }));
  }
  for (std::future<bool>& caller : callers) {
    REQUIRE(caller.wait_for(patience) == std::future_status::ready);
    CHECK(caller.get());
  }
}

TEST_CASE("a program that cannot be started fails the launch, saying why") {
  const mcp::Reply<mcp::Client> missing =
      mcp::launch({.command = "mcp-no-such-server"});
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().fault == mcp::Fault::transport);
  CHECK(missing.error().message ==
        "cannot start mcp-no-such-server: No such file or directory");

  const TemporaryDirectory directory;
  const std::filesystem::path plain = directory.path / "plain";
  std::ofstream{plain} << "not a program\n";
  const mcp::Reply<mcp::Client> refused = mcp::launch({.command = plain.string()});
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().fault == mcp::Fault::transport);
  CHECK(refused.error().message ==
        "cannot start " + plain.string() + ": Permission denied");

  const mcp::Reply<mcp::Client> nowhere =
      mcp::launch({.command = "/bin/sh", .directory = directory.path / "missing"});
  REQUIRE_FALSE(nowhere.has_value());
  CHECK(nowhere.error().message == "cannot start /bin/sh in " +
                                       (directory.path / "missing").string() +
                                       ": there is no such directory");

  const mcp::Reply<mcp::Client> unnamed = mcp::launch({.command = ""});
  REQUIRE_FALSE(unnamed.has_value());
  CHECK(unnamed.error().message == "there is no command to start");

  const mcp::Reply<mcp::Client> badly_named = mcp::launch(
      {.command = "/bin/sh", .environment = {{.name = "A=B", .value = "c"}}});
  REQUIRE_FALSE(badly_named.has_value());
  CHECK(badly_named.error().message ==
        R"(cannot start /bin/sh: "A=B" cannot name an environment variable)");
}

TEST_CASE("a server that exits fails the waiting call with how it exited") {
  struct Case {
    std::string_view then;
    std::string_view how;
  };
  for (const Case& each :
       {Case{"IFS= read -r line; exit 3", "/bin/sh exited with status 3"},
        Case{"IFS= read -r line; kill -9 $$", "/bin/sh was killed by signal 9"}}) {
    mcp::Reply<mcp::Client> client = mcp::launch(
        scripted({std::string{discovered}}, each.then), options(mcp::Era::modern));
    REQUIRE(client.has_value());
    const mcp::Reply<mcp::ToolResult> called = client->call_tool("anything");
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().fault == mcp::Fault::transport);
    CHECK(called.error().message == each.how);
    // A later call fails at once, and the server is not started again.
    const mcp::Reply<void> later = client->ping();
    REQUIRE_FALSE(later.has_value());
    CHECK(later.error().message == each.how);
  }
}

TEST_CASE("a server that exits before it answers fails the opening") {
  const mcp::Reply<mcp::Client> client =
      mcp::launch({.command = "/bin/sh", .arguments = {"-c", "exit 7"}});
  REQUIRE_FALSE(client.has_value());
  CHECK(client.error().fault == mcp::Fault::transport);
  // Whether the request was written before the server exited or not, the
  // error says how it exited.
  CHECK(client.error().message == "/bin/sh exited with status 7");
}

TEST_CASE("what a server writes to stdout that is not a message is passed over") {
  const mcp::Command noisy{
      .command = "/bin/sh",
      .arguments = {"-c",
                    R"(IFS= read -r line; echo 'this is not JSON'; echo '[1,2]'; )"
                    R"(echo '{"jsonrpc":"2.0","method":"notifications/unknown"}'; )"
                    R"(echo '{"jsonrpc":"2.0","id":99,"result":{}}'; )"
                    R"(printf '%s\n' "$1"; exec cat >/dev/null)",
                    "sh", std::string{discovered}}};
  const mcp::Reply<mcp::Client> client = mcp::launch(noisy, options(mcp::Era::modern));
  REQUIRE(client.has_value());
  CHECK(client->server().name == "script");

  // A server that writes nothing else never answers.
  mcp::ClientOptions hurried = options(mcp::Era::modern);
  hurried.timeout = 200ms;
  const mcp::Reply<mcp::Client> babbling = mcp::launch(
      {.command = "/bin/sh",
       .arguments = {"-c", "while IFS= read -r line; do echo nonsense; done"}},
      hurried);
  REQUIRE_FALSE(babbling.has_value());
  CHECK(babbling.error().fault == mcp::Fault::timeout);
}

TEST_CASE("a launched server that is slow to start still opens in its own era") {
  // The script waits for the handshake that follows the probe, then answers
  // it, as a slow server of the legacy era would.
  const mcp::Command slow{
      .command = "/bin/sh",
      .arguments = {
          "-c",
          R"(IFS= read -r probe; IFS= read -r handshake; printf '%s\n' "$1"; )"
          R"(IFS= read -r initialized; IFS= read -r ping; printf '%s\n' "$2"; )"
          R"(exec cat >/dev/null)",
          "sh",
          R"({"jsonrpc":"2.0","id":2,"result":{"protocolVersion":"2025-11-25",)"
          R"("capabilities":{},"serverInfo":{"name":"slow","version":"1"}}})",
          R"({"jsonrpc":"2.0","id":3,"result":{}})"}};
  mcp::ClientOptions automatic = options(mcp::Era::automatic);
  automatic.probe_timeout = 50ms;
  mcp::Reply<mcp::Client> client = mcp::launch(slow, automatic);
  REQUIRE(client.has_value());
  CHECK(client->server().era == mcp::Era::legacy);
  CHECK(client->server().name == "slow");
  CHECK(client->ping().has_value());
}

TEST_CASE("closing a server's input ends one that exits at the end of its input") {
  const std::string weather = weather_example();
  mcp::Reply<mcp::Client> client = mcp::launch({.command = weather, .grace = 10s});
  REQUIRE(client.has_value());
  // No signal is sent before the grace period is over.
  CHECK(timed([&] { const mcp::Client closing = std::move(*client); }) < 5s);
}

TEST_CASE("a server that ignores the end of its input is sent SIGTERM") {
  Collected collected;
  mcp::Command stubborn = scripted({std::string{discovered}},
                                   R"(trap 'kill $! 2>/dev/null; echo terminated >&2; )"
                                   R"(exit 0' TERM; sleep 30 & wait)");
  stubborn.stderr_output = mcp::Stderr::lines;
  stubborn.stderr_line = collected.sink();
  stubborn.grace = 100ms;
  mcp::Reply<mcp::Client> client = mcp::launch(stubborn, options(mcp::Era::modern));
  REQUIRE(client.has_value());
  const auto took = timed([&] { const mcp::Client closing = std::move(*client); });
  CHECK(took >= 100ms);
  CHECK(took < patience);
  CHECK(collected.lines() == std::vector<std::string>{"terminated"});
}

TEST_CASE("a server that ignores SIGTERM is killed, and reaped") {
  Collected collected;
  mcp::Command stubborn = scripted({std::string{discovered}}, "exec sleep 30");
  // The shell says its process id, which sleep takes over, and ignores SIGTERM,
  // as sleep then does.
  stubborn.arguments[1] = "trap '' TERM; echo $$ >&2; " + stubborn.arguments[1];
  stubborn.stderr_output = mcp::Stderr::lines;
  stubborn.stderr_line = collected.sink();
  stubborn.grace = 100ms;
  mcp::Reply<mcp::Client> client = mcp::launch(stubborn, options(mcp::Era::modern));
  REQUIRE(client.has_value());
  REQUIRE(collected.wait_for(1));
  const pid_t pid = std::stoi(collected.lines().front());

  // Two grace periods pass before SIGKILL.
  const auto took = timed([&] { const mcp::Client closing = std::move(*client); });
  CHECK(took >= 200ms);
  CHECK(took < patience);
  int status = 0;
  CHECK(::waitpid(pid, &status, WNOHANG) == -1);
  CHECK(errno == ECHILD);
  CHECK(::kill(pid, 0) == -1);
  CHECK(errno == ESRCH);
}

TEST_CASE("the processes a server started end with it") {
  const TemporaryDirectory directory;
  const std::filesystem::path fifo = directory.path / "fifo";
  REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
  // Opened first, so that the grandchild's open for writing does not block.
  const int watch = ::open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
  REQUIRE(watch >= 0);

  mcp::Command parent = scripted({std::string{discovered}}, "exec sleep 30");
  // A process of the server's own holds the FIFO open while it lives.
  parent.arguments[1] =
      R"((exec 3>"$MCP_FIFO"; echo ready >&3; exec sleep 30) & )" + parent.arguments[1];
  parent.environment = {{.name = "MCP_FIFO", .value = fifo.string()}};
  parent.grace = 100ms;
  mcp::Reply<mcp::Client> client = mcp::launch(parent, options(mcp::Era::modern));
  REQUIRE(client.has_value());

  // Reads the FIFO until it holds a line, or, once its writer has come, until
  // its writers have gone. A read of a FIFO that no one writes to ends at once,
  // before a writer has come too on Linux, and macOS's poll() does not always
  // say so, so the reads are paced by a short pause.
  const auto read_fifo = [watch](const bool until_the_end) {
    std::string text;
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (std::chrono::steady_clock::now() < deadline) {
      std::array<char, 64> buffer{};
      const ssize_t got = ::read(watch, buffer.data(), buffer.size());
      if (got == 0 && until_the_end) {
        return std::pair{text, true};
      }
      if (got > 0) {
        text.append(buffer.data(), static_cast<std::size_t>(got));
        if (!until_the_end && text.ends_with('\n')) {
          return std::pair{text, false};
        }
        continue;
      }
      std::this_thread::sleep_for(5ms);
    }
    return std::pair{text, false};
  };
  CHECK(read_fifo(false) == std::pair{std::string{"ready\n"}, false});
  {
    const mcp::Client closing = std::move(*client);
  }
  CHECK(read_fifo(true) == std::pair{std::string{}, true});
  ::close(watch);
}

TEST_CASE("a server's stderr is inherited, thrown away, or read line by line") {
  const TemporaryDirectory directory;
  const std::filesystem::path file = directory.path / "stderr";
  for (const mcp::Stderr mode : {mcp::Stderr::inherit, mcp::Stderr::discard}) {
    mcp::Command command = before_weather("echo 'from the server' >&2");
    command.stderr_output = mode;
    {
      const StderrToFile redirected{file};
      // The server writes before it becomes the server, so before it answers.
      const mcp::Reply<mcp::Client> client = mcp::launch(command);
      REQUIRE(client.has_value());
    }
    CHECK(contents_of(file) ==
          (mode == mcp::Stderr::inherit ? "from the server\n" : ""));
  }

  Collected collected;
  mcp::Command command =
      before_weather("printf 'one\\n\\ntwo\\r\\nthree' >&2; echo ' and more' >&2; "
                     "printf '0123456789abcdefXYZ\\n' >&2; echo last >&2");
  command.stderr_output = mcp::Stderr::lines;
  command.stderr_line = collected.sink();
  command.max_line = 16;
  // The server's messages are longer than the bound, so it fails to open, and
  // its stderr is read all the same.
  const mcp::Reply<mcp::Client> client = mcp::launch(command);
  REQUIRE_FALSE(client.has_value());
  CHECK(client.error().message ==
        "/bin/sh wrote a line longer than 16 bytes, the most this client reads");
  CHECK(collected.lines() == std::vector<std::string>{"one", "", "two",
                                                      "three and more",
                                                      "0123456789abcdef", "last"});
}

TEST_CASE("a launched server gets its environment and its directory") {
  const TemporaryDirectory directory;
  REQUIRE(::setenv("MCP_KEPT", "kept", 1) == 0);
  REQUIRE(::setenv("MCP_REPLACED", "the client's", 1) == 0);
  Collected collected;
  mcp::Command command = before_weather(
      R"(printf '%s\n' "$MCP_GREETING" "$MCP_KEPT" "$MCP_REPLACED" >&2; pwd -P >&2)");
  command.environment = {{.name = "MCP_GREETING", .value = "hello"},
                         {.name = "MCP_REPLACED", .value = "first"},
                         {.name = "MCP_REPLACED", .value = "the server's"}};
  command.directory = directory.path;
  command.stderr_output = mcp::Stderr::lines;
  command.stderr_line = collected.sink();
  const mcp::Reply<mcp::Client> client = mcp::launch(command);
  REQUIRE(client.has_value());
  REQUIRE(collected.wait_for(4));
  CHECK(collected.lines() == std::vector<std::string>{"hello", "kept", "the server's",
                                                      directory.path.string()});
  ::unsetenv("MCP_KEPT");
  ::unsetenv("MCP_REPLACED");
}

TEST_CASE("a relative program is found from the directory it starts in") {
  const std::filesystem::path weather{weather_example()};
  const mcp::Reply<mcp::Client> client =
      mcp::launch({.command = "./" + weather.filename().string(),
                   .directory = weather.parent_path()});
  REQUIRE(client.has_value());
  CHECK(client->server().name == "weather");
}

TEST_CASE("writing to a server that has gone fails the call, and raises no SIGPIPE") {
  struct sigaction before{};
  REQUIRE(::sigaction(SIGPIPE, nullptr, &before) == 0);
  // The script closes its input before it answers, so the next request has no
  // reader. Were SIGPIPE raised, its default action would end this test.
  mcp::Command deaf = scripted({std::string{discovered}}, "exec sleep 30");
  deaf.arguments[1] =
      R"(IFS= read -r line; exec 0<&-; printf '%s\n' "$1"; exec sleep 30)";
  deaf.grace = 100ms;
  mcp::Reply<mcp::Client> client = mcp::launch(deaf, options(mcp::Era::modern));
  REQUIRE(client.has_value());
  for (int call = 0; call < 2; ++call) {
    const mcp::Reply<mcp::ToolResult> called = client->call_tool("anything");
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().fault == mcp::Fault::transport);
    CHECK(called.error().message == "/bin/sh no longer reads its input");
  }
  struct sigaction after{};
  REQUIRE(::sigaction(SIGPIPE, nullptr, &after) == 0);
  CHECK(after.sa_handler == before.sa_handler);
  sigset_t pending;
  sigpending(&pending);
  CHECK(sigismember(&pending, SIGPIPE) == 0);
}

TEST_CASE("a line longer than the bound ends a launched server's client") {
  mcp::Command command = scripted({std::string(5000, 'x'), std::string{discovered}});
  command.max_line = 1024;
  const mcp::Reply<mcp::Client> client =
      mcp::launch(command, options(mcp::Era::modern));
  REQUIRE_FALSE(client.has_value());
  CHECK(client.error().fault == mcp::Fault::transport);
  CHECK(client.error().message ==
        "/bin/sh wrote a line longer than 1024 bytes, the most this client reads");
}

TEST_CASE("launching servers leaks no descriptor, to the client or to other servers") {
  const std::string weather = weather_example();
  const int before = open_descriptors();
  for (int round = 0; round < 20; ++round) {
    const mcp::Reply<mcp::Client> client =
        mcp::launch({.command = weather, .stderr_output = mcp::Stderr::lines});
    REQUIRE(client.has_value());
  }
  CHECK(open_descriptors() == before);

  // A server started while others run inherits no more than one started
  // alone: its three standard streams, and what ls opens to list them.
  const auto listed = [] {
    Collected collected;
    mcp::Command listing = before_weather("ls /dev/fd/ >&2; echo end >&2");
    listing.stderr_output = mcp::Stderr::lines;
    listing.stderr_line = collected.sink();
    const mcp::Reply<mcp::Client> client = mcp::launch(listing);
    REQUIRE(client.has_value());
    REQUIRE(collected.wait_for(4));
    std::vector<std::string> lines = collected.lines();
    REQUIRE(lines.back() == "end");
    return lines;
  };
  const std::vector<std::string> alone = listed();
  REQUIRE(alone.size() >= 4);
  CHECK(std::vector<std::string>{alone.begin(), alone.begin() + 3} ==
        std::vector<std::string>{"0", "1", "2"});
  std::vector<mcp::Reply<mcp::Client>> others;
  for (int other = 0; other < 3; ++other) {
    others.push_back(
        mcp::launch({.command = weather, .stderr_output = mcp::Stderr::lines}));
    REQUIRE(others.back().has_value());
  }
  CHECK(listed() == alone);
}
