// A scripted HTTP client for the client's HTTP transport: the test plays the
// server, sees each request exactly as it was sent, and answers with the
// status, the headers, and the body in parts, when it likes.
#pragma once

#include "support.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mcp/http_client.hpp>
#include <mutex>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fake {

using namespace std::chrono_literals;

// How long a test waits for anything that should happen at once.
inline constexpr auto patience = 10s;

inline constexpr std::string_view url = "http://localhost:8080/mcp";

inline bool iequals(const std::string_view a, const std::string_view b) {
  return std::ranges::equal(a, b, [](const char x, const char y) {
    const auto lower = [](const char c) {
      return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return lower(x) == lower(y);
  });
}

// One request as the HTTP client was given it.
struct Request {
  std::string method;
  std::string url;
  std::vector<mcp::HttpHeader> headers;
  std::string body;

  [[nodiscard]] std::optional<std::string> header(const std::string_view name) const {
    for (const mcp::HttpHeader& header : headers) {
      if (iequals(header.name, name)) {
        return header.value;
      }
    }
    return std::nullopt;
  }

  // Each header as `Name: value`, in order.
  [[nodiscard]] std::vector<std::string> lines() const {
    std::vector<std::string> out;
    for (const mcp::HttpHeader& header : headers) {
      out.push_back(header.name + ": " + header.value);
    }
    return out;
  }

  // The JSON-RPC method of the body, or empty for a response or no body.
  [[nodiscard]] std::string rpc() const { return member("method").value_or(""); }

  // The JSON-RPC id of the body as JSON text, or empty for none.
  [[nodiscard]] std::string id() const {
    const scrydantic::Result<scrydantic::JsonView> body_view =
        scrydantic::JsonView::parse(body);
    if (!body_view) {
      return {};
    }
    const std::optional<scrydantic::JsonView> found = body_view->find("id");
    return found.has_value() ? found->to_json().text : std::string{};
  }

private:
  [[nodiscard]] std::optional<std::string> member(const std::string_view key) const {
    const scrydantic::Result<scrydantic::JsonView> body_view =
        scrydantic::JsonView::parse(body);
    if (!body_view) {
      return std::nullopt;
    }
    const std::optional<scrydantic::JsonView> found = body_view->find(key);
    const std::optional<std::string_view> text =
        found.has_value() ? found->string() : std::nullopt;
    return text.has_value() ? std::optional<std::string>{std::string{*text}}
                            : std::nullopt;
  }
};

// One exchange as the server sees it, which the test answers through.
class Exchange {
public:
  Exchange(mcp::HttpClientResponse& response, std::stop_token stop)
      : response_{&response}, stop_{std::move(stop)} {}

  void start(const int status, const std::vector<mcp::HttpHeader>& headers = {}) {
    response_->start(status, headers);
  }

  bool write(const std::string_view data) { return response_->write(data); }

  // A whole JSON body.
  void json(const std::string_view body, const int status = 200,
            std::vector<mcp::HttpHeader> headers = {}) {
    headers.push_back({.name = "Content-Type", .value = "application/json"});
    start(status, headers);
    write(body);
  }

  // Opens an event stream.
  void events(std::vector<mcp::HttpHeader> headers = {}) {
    headers.push_back({.name = "content-type", .value = "text/event-stream"});
    start(200, headers);
  }

  // Sends one message on an open stream.
  bool event(const std::string_view message) {
    return write("event: message\ndata: " + std::string{message} + "\n\n");
  }

  // Waits until the client closes the exchange. Returns whether it did.
  bool closed() {
    std::mutex mutex;
    std::condition_variable_any never;
    std::unique_lock lock{mutex};
    never.wait_for(lock, stop_, patience, [] { return false; });
    return stop_.stop_requested();
  }

  [[nodiscard]] const std::stop_token& stop() const { return stop_; }

private:
  mcp::HttpClientResponse* response_;
  std::stop_token stop_;
};

using Respond = std::function<mcp::Reply<void>(const Request&, Exchange&)>;

class Http final : public mcp::HttpClient {
public:
  // Sets how the server answers. It is called on the thread that sends each
  // request; one that is not set answers 500.
  void answer(Respond respond) {
    const std::scoped_lock lock{mutex_};
    respond_ = std::move(respond);
  }

  mcp::Reply<void> send(const mcp::HttpClientRequest& request,
                        mcp::HttpClientResponse& response,
                        const std::stop_token stop) override {
    Request copy{.method = std::string{request.method},
                 .url = std::string{request.url},
                 .headers = {request.headers.begin(), request.headers.end()},
                 .body = std::string{request.body}};
    Respond respond;
    {
      const std::scoped_lock lock{mutex_};
      requests_.push_back(copy);
      respond = respond_;
    }
    changed_.notify_all();
    Exchange exchange{response, stop};
    if (!respond) {
      exchange.start(500);
      return {};
    }
    return respond(copy, exchange);
  }

  [[nodiscard]] std::vector<Request> requests() const {
    const std::scoped_lock lock{mutex_};
    return requests_;
  }

  // The JSON-RPC methods of the requests so far, `DELETE` for a DELETE and
  // `response` for a POSTed response.
  [[nodiscard]] std::vector<std::string> methods() const {
    std::vector<std::string> out;
    for (const Request& request : requests()) {
      out.push_back(request.method == "DELETE" ? std::string{"DELETE"}
                    : request.rpc().empty()    ? std::string{"response"}
                                               : request.rpc());
    }
    return out;
  }

  // Waits until a request that `wanted` picks has come, and returns the first.
  [[nodiscard]] std::optional<Request>
  await(const std::function<bool(const Request&)>& wanted,
        const std::chrono::milliseconds within = patience) const {
    std::unique_lock lock{mutex_};
    std::optional<Request> found;
    changed_.wait_for(lock, within, [&] {
      for (const Request& request : requests_) {
        if (wanted(request)) {
          found = request;
          return true;
        }
      }
      return false;
    });
    return found;
  }

  // Waits for the `count`th request of this JSON-RPC method.
  [[nodiscard]] std::optional<Request> await_rpc(const std::string_view method,
                                                 const std::size_t count = 1) const {
    std::size_t seen = 0;
    return await([&seen, method, count](const Request& request) {
      return request.rpc() == method && ++seen == count;
    });
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  Respond respond_;
  std::vector<Request> requests_;
};

inline constexpr std::string_view discovered =
    R"({"_meta":{"io.modelcontextprotocol/serverInfo":{"name":"fake","version":"1.0"}},)"
    R"("capabilities":{"tools":{}},"resultType":"complete",)"
    R"("supportedVersions":["2026-07-28","2025-11-25"]})";

inline std::string initialized(const std::string_view version = "2025-11-25") {
  return std::string{R"({"capabilities":{"tools":{}},"protocolVersion":")"} +
         std::string{version} + R"(","serverInfo":{"name":"fake","version":"1.0"}})";
}

inline constexpr std::string_view tool_done =
    R"({"content":[{"type":"text","text":"done"}],"resultType":"complete"})";

// A server that answers each request of these methods with its result, takes
// every notification and response with 202, and answers any other request
// with 404 and -32601. A handshake server names the session `s1`.
inline Respond serving(std::map<std::string, std::string, std::less<>> results) {
  return [results = std::move(results)](const Request& request,
                                        Exchange& exchange) -> mcp::Reply<void> {
    if (request.method == "DELETE") {
      exchange.start(204);
      return {};
    }
    if (request.id().empty() || request.rpc().empty()) {
      exchange.start(202);
      return {};
    }
    const auto found = results.find(request.rpc());
    if (found == results.end()) {
      exchange.json(support::error(request.id(), -32601, "Method not found"), 404);
      return {};
    }
    std::vector<mcp::HttpHeader> headers;
    if (request.rpc() == "initialize") {
      headers.push_back({.name = "Mcp-Session-Id", .value = "s1"});
    }
    exchange.json(support::result(request.id(), found->second), 200, headers);
    return {};
  };
}

inline Respond
modern_server(std::map<std::string, std::string, std::less<>> more = {}) {
  more.emplace("server/discover", std::string{discovered});
  return serving(std::move(more));
}

inline Respond
legacy_server(std::map<std::string, std::string, std::less<>> more = {}) {
  more.emplace("initialize", initialized());
  return serving(std::move(more));
}

inline mcp::ClientOptions options(const mcp::Era era = mcp::Era::automatic) {
  return {.name = "zoo",
          .version = "0.3.0",
          .era = era,
          .timeout = std::chrono::seconds{60},
          .probe_timeout = std::chrono::seconds{60}};
}

// The metadata of every modern request of a client named zoo.
inline constexpr std::string_view modern_meta =
    R"("_meta":{"io.modelcontextprotocol/clientCapabilities":{},)"
    R"("io.modelcontextprotocol/clientInfo":{"name":"zoo","version":"0.3.0"},)"
    R"("io.modelcontextprotocol/protocolVersion":"2026-07-28"})";

inline std::string request(const std::string_view id, const std::string_view method,
                           const std::string_view params) {
  return std::string{R"({"id":)"} + std::string{id} + R"(,"jsonrpc":"2.0","method":")" +
         std::string{method} + R"(","params":)" + std::string{params} + "}";
}

} // namespace fake
