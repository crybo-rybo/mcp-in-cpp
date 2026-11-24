// The Streamable HTTP endpoint: what each era's requests are answered with,
// and which mistakes in a request the transport turns away.
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <mcp/mcp.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using support::called;
using support::modern;
using support::text_content;

namespace slow_http {

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

} // namespace slow_http

namespace {

// A response that keeps what the endpoint wrote. It can be told that the
// client is gone after a number of writes.
class Recorded final : public mcp::HttpResponse {
public:
  void start(const int code, const std::span<const mcp::HttpHeader> sent) override {
    const std::scoped_lock lock{*mutex_};
    ++starts;
    status = code;
    headers.assign(sent.begin(), sent.end());
  }

  bool write(const std::string_view data) override {
    const std::scoped_lock lock{*mutex_};
    if (writes_left == 0) {
      return false;
    }
    if (writes_left > 0) {
      --writes_left;
    }
    body += data;
    return true;
  }

  [[nodiscard]] bool started() const {
    const std::scoped_lock lock{*mutex_};
    return starts > 0;
  }

  [[nodiscard]] std::optional<std::string> header(const std::string_view name) const {
    for (const mcp::HttpHeader& header : headers) {
      if (header.name == name) {
        return header.value;
      }
    }
    return std::nullopt;
  }

  int starts{};
  int status{};
  std::vector<mcp::HttpHeader> headers;
  std::string body;
  // How many more writes succeed, or -1 for all.
  int writes_left{-1};

private:
  // Shared, so that a response can be returned by value.
  std::shared_ptr<std::mutex> mutex_{std::make_shared<std::mutex>()};
};

// A POST with the headers a client of 2026-07-28 sends for `method`.
std::vector<mcp::HttpHeader> modern_headers(const std::string_view method,
                                            const std::string_view name = {}) {
  std::vector<mcp::HttpHeader> headers{
      {.name = "Host", .value = "localhost:8000"},
      {.name = "Content-Type", .value = "application/json"},
      {.name = "Accept", .value = "application/json, text/event-stream"},
      {.name = "MCP-Protocol-Version", .value = "2026-07-28"},
      {.name = "Mcp-Method", .value = std::string{method}},
  };
  if (!name.empty()) {
    headers.push_back({.name = "Mcp-Name", .value = std::string{name}});
  }
  return headers;
}

std::vector<mcp::HttpHeader> legacy_headers(const std::string_view session = {}) {
  std::vector<mcp::HttpHeader> headers{
      {.name = "Host", .value = "127.0.0.1:8000"},
      {.name = "Content-Type", .value = "application/json"},
      {.name = "Accept", .value = "application/json, text/event-stream"},
  };
  if (!session.empty()) {
    headers.push_back({.name = "MCP-Protocol-Version", .value = "2025-11-25"});
    headers.push_back({.name = "Mcp-Session-Id", .value = std::string{session}});
  }
  return headers;
}

Recorded post(mcp::HttpEndpoint& endpoint, const std::vector<mcp::HttpHeader>& headers,
              const std::string_view body, std::stop_token stop = {}) {
  Recorded response;
  endpoint.handle({.method = "POST", .headers = headers, .body = body}, response,
                  std::move(stop));
  return response;
}

const std::string add_call =
    modern("1", "tools/call", R"("name":"add","arguments":{"a":2,"b":3})");

} // namespace

TEST_CASE("a call of 2026-07-28 is answered with JSON when nothing streams") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::HttpEndpoint endpoint{server};

  const Recorded response = post(endpoint, modern_headers("tools/call", "add"), add_call);
  CHECK(response.status == 200);
  CHECK(response.header("Content-Type") == "application/json");
  CHECK(response.body == called("1", text_content("5", false), "5"));
}

TEST_CASE("a call's progress opens an event stream that ends with the response") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::HttpEndpoint endpoint{server};

  const Recorded response = post(
      endpoint, modern_headers("tools/call", "count"),
      R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"count",)"
      R"("arguments":{"steps":1},"_meta":{"progressToken":"t",)"
      R"("io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
      R"("io.modelcontextprotocol/clientCapabilities":{}}}})");
  CHECK(response.status == 200);
  CHECK(response.header("Content-Type") == "text/event-stream");
  CHECK(response.body ==
        "event: message\r\ndata: "
        R"({"jsonrpc":"2.0","method":"notifications/progress",)"
        R"("params":{"progress":1,"progressToken":"t","total":1}})"
        "\r\n\r\nevent: message\r\ndata: " +
            called("1", text_content("1", false), "1") + "\r\n\r\n");
}

TEST_CASE("the routing headers must match the body") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::HttpEndpoint endpoint{server};

  const auto code_of = [](const Recorded& response) {
    return response.body.substr(0, response.body.find(','));
  };

  // A name in base64 is decoded before it is compared: "add".
  CHECK(post(endpoint, modern_headers("tools/call", "=?base64?YWRk?="), add_call).body ==
        called("1", text_content("5", false), "5"));

  const Recorded wrong_name = post(endpoint, modern_headers("tools/call", "greet"), add_call);
  CHECK(wrong_name.status == 400);
  CHECK(code_of(wrong_name) == R"({"error":{"code":-32020)");

  const Recorded no_name = post(endpoint, modern_headers("tools/call"), add_call);
  CHECK(no_name.status == 400);
  CHECK(code_of(no_name) == R"({"error":{"code":-32020)");

  const Recorded wrong_method = post(endpoint, modern_headers("tools/list", "add"), add_call);
  CHECK(wrong_method.status == 400);
  CHECK(code_of(wrong_method) == R"({"error":{"code":-32020)");

  std::vector<mcp::HttpHeader> other_version = modern_headers("tools/call", "add");
  other_version[3].value = "2026-01-01";
  const Recorded version = post(endpoint, other_version, add_call);
  CHECK(version.status == 400);
  CHECK(code_of(version) == R"({"error":{"code":-32020)");
}

TEST_CASE("a request of 2026-07-28 without its envelope is a bad request") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::HttpEndpoint endpoint{server};

  const Recorded response =
      post(endpoint, modern_headers("tools/list"),
           R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})");
  CHECK(response.status == 400);
  CHECK(response.body.starts_with(R"({"error":{"code":-32602)"));
  CHECK(response.body.ends_with(R"(,"id":1,"jsonrpc":"2.0"})"));
}

TEST_CASE("a method that 2026-07-28 lacks is not found, initialize among them") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::HttpEndpoint endpoint{server};

  for (const std::string_view method : {"ping", "logging/setLevel", "initialize"}) {
    const Recorded response =
        post(endpoint, modern_headers(method), modern("7", method));
    CHECK(response.status == 404);
    CHECK(response.body.starts_with(R"({"error":{"code":-32601)"));
    CHECK(response.body.ends_with(R"(,"id":7,"jsonrpc":"2.0"})"));
  }
}

TEST_CASE("a notification of 2026-07-28 is accepted and has no body") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::HttpEndpoint endpoint{server};

  const Recorded response =
      post(endpoint, modern_headers("notifications/cancelled"),
           R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":1}})");
  CHECK(response.status == 202);
  CHECK(response.body.empty());
}

TEST_CASE("the endpoint turns away what is not an MCP POST") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::HttpEndpoint endpoint{server};
  const std::vector<mcp::HttpHeader> good = modern_headers("tools/call", "add");

  SECTION("another host, which a page could reach through DNS rebinding") {
    std::vector<mcp::HttpHeader> headers = good;
    headers[0].value = "evil.example.com";
    CHECK(post(endpoint, headers, add_call).status == 421);
  }
  SECTION("a page of another origin") {
    std::vector<mcp::HttpHeader> headers = good;
    headers.push_back({.name = "Origin", .value = "https://evil.example.com"});
    CHECK(post(endpoint, headers, add_call).status == 403);
    headers.back().value = "http://localhost:3000";
    CHECK(post(endpoint, headers, add_call).status == 200);
  }
  SECTION("a method other than POST") {
    Recorded response;
    endpoint.handle({.method = "GET", .headers = good, .body = {}}, response);
    CHECK(response.status == 405);
    CHECK(response.header("Allow") == "POST");
  }
  SECTION("a body that is not JSON by its type") {
    std::vector<mcp::HttpHeader> headers = good;
    headers[1].value = "text/plain";
    CHECK(post(endpoint, headers, add_call).status == 415);
  }
  SECTION("a client that takes no JSON") {
    std::vector<mcp::HttpHeader> headers = good;
    headers[2].value = "text/html";
    CHECK(post(endpoint, headers, add_call).status == 406);
  }
  SECTION("a body that is not JSON") {
    const Recorded response = post(endpoint, good, "{");
    CHECK(response.status == 400);
    CHECK(response.body.starts_with(R"({"error":{"code":-32700)"));
  }
  SECTION("a batch") {
    const Recorded response = post(endpoint, good, "[" + add_call + "]");
    CHECK(response.status == 400);
    CHECK(response.body.starts_with(R"({"error":{"code":-32600)"));
  }
}

TEST_CASE("any host is accepted when the list of hosts is empty") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::HttpEndpoint endpoint{server, {.allowed_hosts = {}}};
  std::vector<mcp::HttpHeader> headers = modern_headers("tools/call", "add");
  headers[0].value = "mcp.example.com";
  headers.push_back({.name = "Origin", .value = "https://app.example.com"});
  CHECK(post(endpoint, headers, add_call).status == 200);
}

TEST_CASE("a handshake starts a session, which its id names until it ends") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::HttpEndpoint endpoint{server};

  const Recorded initialized =
      post(endpoint, legacy_headers(), support::initialize("2025-11-25"));
  REQUIRE(initialized.status == 200);
  const std::optional<std::string> session = initialized.header("Mcp-Session-Id");
  REQUIRE(session.has_value());
  CHECK(session->size() == 32);

  const Recorded listed =
      post(endpoint, legacy_headers(*session), support::legacy("1", "tools/list"));
  CHECK(listed.status == 200);
  CHECK(listed.body.starts_with(R"({"id":1,"jsonrpc":"2.0","result":{"tools":[)"));

  const Recorded pinged =
      post(endpoint, legacy_headers(*session), support::legacy("2", "ping"));
  CHECK(pinged.body == support::result("2", "{}"));

  const Recorded notified =
      post(endpoint, legacy_headers(*session),
           R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  CHECK(notified.status == 202);

  Recorded ended;
  endpoint.handle({.method = "DELETE", .headers = legacy_headers(*session), .body = {}},
                  ended);
  CHECK(ended.status == 204);
  CHECK(post(endpoint, legacy_headers(*session), support::legacy("3", "tools/list")).status ==
        404);
}

TEST_CASE("a request of an earlier revision needs its session") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::HttpEndpoint endpoint{server};

  CHECK(post(endpoint, legacy_headers(), support::legacy("1", "tools/list")).status == 400);
  CHECK(post(endpoint, legacy_headers("0123"), support::legacy("1", "tools/list")).status ==
        404);
}

TEST_CASE("a session's call is cancelled by a notification, and its stream ends with "
          "an error") {
  mcp::Server server{"sample", "0.0.1"};
  server.add<^^slow_http>();
  mcp::HttpEndpoint endpoint{server};
  const Recorded initialized =
      post(endpoint, legacy_headers(), support::initialize("2025-11-25"));
  const std::string session = initialized.header("Mcp-Session-Id").value_or("");

  Recorded call;
  std::jthread calling{[&] {
    call = post(endpoint, legacy_headers(session),
                support::legacy("5", "tools/call", R"({"name":"wait_for_cancel"})"));
  }};
  REQUIRE(slow_http::started.try_acquire_for(std::chrono::seconds{10}));
  const Recorded cancel =
      post(endpoint, legacy_headers(session),
           R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":5}})");
  CHECK(cancel.status == 202);
  calling.join();

  CHECK(call.status == 200);
  CHECK(call.body == R"({"error":{"code":-32800,"message":"Request cancelled"},"id":5,)"
                     R"("jsonrpc":"2.0"})");
}

TEST_CASE("under 2026-07-28 a client that goes away cancels its call") {
  mcp::Server server{"sample", "0.0.1"};
  server.add<^^slow_http>();
  mcp::HttpEndpoint endpoint{server};
  std::stop_source gone;

  Recorded call;
  std::jthread calling{[&] {
    call = post(endpoint, modern_headers("tools/call", "wait_for_cancel"),
                modern("1", "tools/call", R"("name":"wait_for_cancel")"), gone.get_token());
  }};
  REQUIRE(slow_http::started.try_acquire_for(std::chrono::seconds{10}));
  gone.request_stop();
  calling.join();

  // Nothing is sent to a client that is not there.
  CHECK(call.starts == 0);
}

TEST_CASE("a subscription is acknowledged on its stream, which stays open until the "
          "client goes") {
  std::vector<std::string> log;
  const mcp::Server server = support::sample_server(log);
  mcp::HttpEndpoint endpoint{server, {.keepalive = std::chrono::milliseconds{5}}};

  // The client is gone after the acknowledgment and two keepalives.
  Recorded response;
  response.writes_left = 3;
  endpoint.handle({.method = "POST",
                   .headers = modern_headers("subscriptions/listen"),
                   .body = modern("9", "subscriptions/listen", R"("notifications":{})")},
                  response);
  CHECK(response.header("Content-Type") == "text/event-stream");
  CHECK(response.body ==
        "event: message\r\ndata: "
        R"({"jsonrpc":"2.0","method":"notifications/subscriptions/acknowledged",)"
        R"("params":{"_meta":{"io.modelcontextprotocol/subscriptionId":9},)"
        R"("notifications":{}}})"
        "\r\n\r\n: keepalive\r\n\r\n: keepalive\r\n\r\n");
}

namespace watched {

[[= mcp::resource{.uri = "test://watched"}]] inline std::string watched() { return "now"; }

} // namespace watched

TEST_CASE("a session's GET stream carries the updates of what it subscribed to") {
  mcp::Server server{"sample", "0.0.1"};
  server.add<^^watched>();
  mcp::HttpEndpoint endpoint{server, {.keepalive = std::chrono::milliseconds{20}}};
  const Recorded initialized =
      post(endpoint, legacy_headers(), support::initialize("2025-11-25"));
  const std::string session = initialized.header("Mcp-Session-Id").value_or("");
  REQUIRE(post(endpoint, legacy_headers(session),
               support::legacy("1", "resources/subscribe", R"({"uri":"test://watched"})"))
              .body == support::result("1", "{}"));

  // The stream opens with a comment and carries the update, then the client
  // is gone at the next keepalive.
  Recorded stream;
  stream.writes_left = 2;
  std::jthread streaming{[&] {
    endpoint.handle({.method = "GET", .headers = legacy_headers(session), .body = {}}, stream);
  }};
  while (!stream.started()) {
    std::this_thread::yield();
  }
  server.resource_updated("test://watched");
  streaming.join();
  CHECK(stream.header("Content-Type") == "text/event-stream");
  CHECK(stream.body == ": keepalive\r\n\r\nevent: message\r\ndata: "
                       R"({"jsonrpc":"2.0","method":"notifications/resources/updated",)"
                       R"("params":{"uri":"test://watched"}})"
                       "\r\n\r\n");

  // A GET without a session is not allowed.
  Recorded plain;
  endpoint.handle({.method = "GET", .headers = legacy_headers(), .body = {}}, plain);
  CHECK(plain.status == 405);
}
