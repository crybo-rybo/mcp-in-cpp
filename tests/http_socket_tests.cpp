// A client and a server over real sockets: the cpp-httplib HTTP client
// against this library's own HttpEndpoint, mounted on a cpp-httplib server on
// an ephemeral port, in each era.
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <future>
#include <httplib.h>
#include <mcp/client.hpp>
#include <mcp/http_client.hpp>
#include <mcp/httplib.hpp>
#include <mcp/httplib_client.hpp>
#include <mcp/mcp.hpp>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

namespace socket_zoo {

[[= mcp::tool{"Add two integers"}]] inline std::int32_t add(std::int32_t a,
                                                            std::int32_t b) {
  return a + b;
}

[[= mcp::tool{"Count to a number"}]] inline std::int32_t count(std::int32_t steps,
                                                               mcp::Progress progress) {
  for (std::int32_t step = 1; step <= steps; ++step) {
    progress.report(step, steps);
  }
  return steps;
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

struct Guest {
  std::string name;
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

} // namespace socket_zoo

namespace {

constexpr auto patience = 10s;

constexpr std::array<mcp::Era, 3> eras{mcp::Era::modern, mcp::Era::legacy,
                                       mcp::Era::automatic};

mcp::Server zoo_server() {
  mcp::Server server{"socket-zoo", "0.0.1"};
  server.add<^^socket_zoo>();
  return server;
}

// The server on a port of its own, served until it is stopped.
class Served {
public:
  Served() : endpoint_{server_} {
    mcp::mount(http_, "/mcp", endpoint_);
    port_ = http_.bind_to_any_port("127.0.0.1");
    thread_ = std::thread{[this] { http_.listen_after_bind(); }};
    http_.wait_until_ready();
  }

  Served(const Served&) = delete;
  Served& operator=(const Served&) = delete;

  ~Served() { stop(); }

  void stop() {
    http_.stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/mcp";
  }

private:
  mcp::Server server_{zoo_server()};
  mcp::HttpEndpoint endpoint_;
  httplib::Server http_;
  int port_{};
  std::thread thread_;
};

// The cpp-httplib client, keeping the body of each request it sends.
class Recording final : public mcp::HttpClient {
public:
  mcp::Reply<void> send(const mcp::HttpClientRequest& request,
                        mcp::HttpClientResponse& response,
                        const std::stop_token stop) override {
    {
      const std::scoped_lock lock{mutex_};
      bodies_.emplace_back(request.body);
    }
    return inner_.send(request, response, stop);
  }

  [[nodiscard]] bool sent(const std::string_view part) const {
    const std::scoped_lock lock{mutex_};
    for (const std::string& body : bodies_) {
      if (body.find(part) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

private:
  mcp::HttplibClient inner_;
  mutable std::mutex mutex_;
  std::vector<std::string> bodies_;
};

mcp::ClientOptions options(const mcp::Era era) {
  return {.name = "zoo", .version = "0.3.0", .era = era};
}

std::string text_of(const mcp::Reply<mcp::ToolResult>& result) {
  REQUIRE(result.has_value());
  REQUIRE(result->content.size() == 1);
  const auto* const text = std::get_if<mcp::TextContent>(&result->content.front());
  REQUIRE(text != nullptr);
  return text->text;
}

} // namespace

TEST_CASE("a client lists and calls tools over HTTP in each era") {
  Served served;
  for (const mcp::Era era : eras) {
    mcp::HttplibClient http;
    mcp::Reply<mcp::Client> client = mcp::connect(served.url(), http, options(era));
    REQUIRE(client.has_value());
    CHECK(client->server().name == "socket-zoo");
    CHECK(client->server().era ==
          (era == mcp::Era::legacy ? mcp::Era::legacy : mcp::Era::modern));
    const mcp::Reply<std::vector<mcp::Tool>> tools = client->list_tools();
    REQUIRE(tools.has_value());
    CHECK(tools->size() == 5);
    CHECK(client->call<^^socket_zoo::add>({.a = 2, .b = 3}) == 5);
    CHECK(client->ping().has_value());
  }
}

TEST_CASE("a call's progress comes over the stream of its request") {
  Served served;
  for (const mcp::Era era : eras) {
    mcp::HttplibClient http;
    mcp::Reply<mcp::Client> client = mcp::connect(served.url(), http, options(era));
    REQUIRE(client.has_value());
    std::vector<mcp::ProgressReport> reports;
    const mcp::Reply<std::int32_t> counted = client->call<^^socket_zoo::count>(
        {.steps = 3}, {.progress = [&reports](const mcp::ProgressReport& report) {
          reports.push_back(report);
        }});
    CHECK(counted == 3);
    REQUIRE(reports.size() == 3);
    CHECK(reports[2].progress == 3.0);
    CHECK(reports[2].total == 3.0);
  }
}

TEST_CASE("several calls run at once over HTTP") {
  Served served;
  for (const mcp::Era era : eras) {
    mcp::HttplibClient http;
    mcp::Reply<mcp::Client> client = mcp::connect(served.url(), http, options(era));
    REQUIRE(client.has_value());
    constexpr int callers = 4;
    {
      const std::scoped_lock lock{socket_zoo::meeting};
      socket_zoo::arrived = 0;
      socket_zoo::expected = callers;
    }
    mcp::Client& open = *client;
    std::vector<std::future<mcp::Reply<std::string>>> calls;
    for (int caller = 0; caller < callers; ++caller) {
      calls.push_back(std::async(
          std::launch::async, [&open] { return open.call<^^socket_zoo::meet>({}); }));
    }
    for (std::future<mcp::Reply<std::string>>& call : calls) {
      REQUIRE(call.wait_for(patience) == std::future_status::ready);
      CHECK(call.get() == "together");
    }
  }
}

TEST_CASE("cancelling a call over HTTP stops the tool that runs it") {
  Served served;
  for (const mcp::Era era : {mcp::Era::modern, mcp::Era::legacy}) {
    INFO(static_cast<int>(era));
    Recording http;
    mcp::Reply<mcp::Client> client = mcp::connect(served.url(), http, options(era));
    REQUIRE(client.has_value());
    mcp::Client& open = *client;
    std::stop_source stop;
    auto calling = std::async(std::launch::async, [&] {
      return open.call<^^socket_zoo::wait_for_cancel>(
          {}, {.progress = {}, .stop = stop.get_token(), .timeout = {}});
    });
    REQUIRE(socket_zoo::started.try_acquire_for(patience));
    stop.request_stop();
    REQUIRE(calling.wait_for(patience) == std::future_status::ready);
    const mcp::Reply<std::string> called = calling.get();
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().fault == mcp::Fault::cancelled);
    CHECK(socket_zoo::stopped.try_acquire_for(patience));
    // Under 2026-07-28 the closed stream alone told the server.
    CHECK(http.sent("notifications/cancelled") == (era == mcp::Era::legacy));
  }
}

TEST_CASE("a call that runs out of time over HTTP stops its tool") {
  Served served;
  mcp::HttplibClient http;
  mcp::Reply<mcp::Client> client =
      mcp::connect(served.url(), http, options(mcp::Era::modern));
  REQUIRE(client.has_value());
  const mcp::Reply<std::string> called =
      client->call<^^socket_zoo::wait_for_cancel>({}, {.timeout = 300ms});
  REQUIRE_FALSE(called.has_value());
  CHECK(called.error().fault == mcp::Fault::timeout);
  CHECK(socket_zoo::started.try_acquire_for(patience));
  CHECK(socket_zoo::stopped.try_acquire_for(patience));
}

TEST_CASE("a form the server asks for is filled in over HTTP in each era") {
  Served served;
  for (const mcp::Era era : eras) {
    mcp::HttplibClient http;
    mcp::ClientOptions with = options(era);
    with.elicitation = [](const mcp::FormRequest& form) {
      return mcp::Answer<mcp::Json>{
          .action = form.message == "Who is it for?" ? mcp::Action::accept
                                                     : mcp::Action::decline,
          .content = mcp::Json{R"({"name":"Ada","seats":2})"}};
    };
    mcp::Reply<mcp::Client> client = mcp::connect(served.url(), http, with);
    REQUIRE(client.has_value());
    CHECK(text_of(client->call_tool("book", mcp::Json{R"({"place":"Noma"})"})) ==
          "booked 2 at Noma for Ada");
  }
}

TEST_CASE("a server that goes away fails the calls, and closing does not wait for it") {
  for (const mcp::Era era : {mcp::Era::modern, mcp::Era::legacy}) {
    std::optional<Served> served{std::in_place};
    mcp::HttplibClient http;
    mcp::Reply<mcp::Client> client = mcp::connect(served->url(), http, options(era));
    REQUIRE(client.has_value());
    REQUIRE(client->call<^^socket_zoo::add>({.a = 1, .b = 1}) == 2);
    served.reset();
    const mcp::Reply<mcp::ToolResult> result =
        client->call_tool("add", mcp::Json{R"({"a":1,"b":2})"});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().fault == mcp::Fault::transport);
    // The handshake's DELETE fails at once too.
    const auto before = std::chrono::steady_clock::now();
    client = std::unexpected(mcp::ClientError{});
    CHECK(std::chrono::steady_clock::now() - before < patience);
  }
}

TEST_CASE("no server at the URL fails the opening") {
  // A port that was just free, on which nothing listens now.
  std::string url;
  {
    httplib::Server probe;
    const int port = probe.bind_to_any_port("127.0.0.1");
    url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    probe.stop();
  }
  mcp::HttplibClient http;
  const mcp::Reply<mcp::Client> client =
      mcp::connect(url, http, options(mcp::Era::automatic));
  REQUIRE_FALSE(client.has_value());
  CHECK(client.error().fault == mcp::Fault::transport);
}

TEST_CASE("an https URL needs cpp-httplib built with TLS") {
#ifndef CPPHTTPLIB_SSL_ENABLED
  mcp::HttplibClient http;
  const mcp::Reply<mcp::Client> client =
      mcp::connect("https://127.0.0.1:1/mcp", http, options(mcp::Era::modern));
  REQUIRE_FALSE(client.has_value());
  CHECK(client.error().fault == mcp::Fault::transport);
  CHECK(client.error().message ==
        "an https URL needs cpp-httplib built with TLS, such as OpenSSL with "
        "CPPHTTPLIB_OPENSSL_SUPPORT defined");
#else
  SUCCEED("cpp-httplib has TLS here");
#endif
}
