/// @file
/// The Streamable HTTP transport, for an HTTP server that the host already has.
///
/// HttpEndpoint is the MCP endpoint: the host's server routes the requests for
/// one path to it, and it writes each response through an HttpResponse the
/// host implements. It speaks both eras: 2026-07-28 statelessly, where each
/// POST stands alone and a client that closes the response stream cancels the
/// call, and the `initialize` handshake of 2025-11-25 and 2025-06-18, whose
/// sessions it keeps by `Mcp-Session-Id`. <mcp/httplib.hpp> mounts one on a
/// cpp-httplib server.
#pragma once

#include <chrono>
#include <cstddef>
#include <mcp/server.hpp>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

/// One header of a request or a response.
struct HttpHeader {
  std::string name;
  std::string value;
};

/// One request to the MCP endpoint, as the host's server read it.
struct HttpRequest {
  /// The request method, such as `POST`.
  std::string_view method;
  /// The request's headers, in the order they came.
  std::span<const HttpHeader> headers;
  /// The whole body.
  std::string_view body;

  /// The value of the first header with this name, compared without regard to
  /// case, or nothing when there is none.
  [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;

  /// How many headers have this name, compared without regard to case.
  [[nodiscard]] std::size_t header_count(std::string_view name) const;
};

/// Where the response to one request goes. The host implements it over its
/// server.
///
/// The endpoint calls start() once, then write() any number of times, all from
/// the thread that called HttpEndpoint::handle(). A response is complete when
/// handle() returns.
class HttpResponse {
public:
  virtual ~HttpResponse() = default;

  /// Sends the status line and the headers. A response with a body has a
  /// `Content-Type` among them; one of `text/event-stream` is a stream, which
  /// the host sends as it is written, without buffering.
  virtual void start(int status, std::span<const HttpHeader> headers) = 0;

  /// Sends part of the body now.
  /// @return false once the client has gone, after which the endpoint writes
  /// no more.
  virtual bool write(std::string_view data) = 0;
};

/// What an HttpEndpoint accepts, and how long it keeps what it holds.
struct HttpOptions {
  /// The values the `Host` header may take, without a port, compared without
  /// regard to case. An empty list accepts any. The default accepts only the
  /// local machine's own names, which keeps a page in a browser from reaching a
  /// local server through a name it controls (DNS rebinding). A server that is
  /// reached under another name lists it.
  std::vector<std::string> allowed_hosts{"localhost", "127.0.0.1", "[::1]"};
  /// The values the `Origin` header may take besides those whose host is an
  /// allowed host, such as `https://app.example.com`. A request without
  /// `Origin` does not come from a browser, and is accepted.
  std::vector<std::string> allowed_origins{};
  /// How often a quiet stream is sent a comment, which keeps proxies from
  /// closing it and finds a client that has gone.
  std::chrono::milliseconds keepalive{std::chrono::seconds{15}};
  /// How long a handshake session lasts after its last request.
  std::chrono::milliseconds session_timeout{std::chrono::hours{1}};
  /// How many handshake sessions are kept at once. A new one beyond that
  /// replaces the one that has been idle longest.
  std::size_t max_sessions{1024};
};

/// The MCP endpoint of a Streamable HTTP server.
///
/// handle() may be called from several threads at once, one call per HTTP
/// request, so the server's tools run side by side too.
class HttpEndpoint {
public:
  /// @param server The server to serve. It must outlive the endpoint and must
  /// not change while the endpoint is in use.
  /// @param options What the endpoint accepts.
  explicit HttpEndpoint(const Server& server, HttpOptions options = {});
  ~HttpEndpoint();
  HttpEndpoint(const HttpEndpoint&) = delete;
  HttpEndpoint& operator=(const HttpEndpoint&) = delete;

  /// Serves one request, writing its whole response before it returns. A
  /// response that streams, such as a call that reports progress, is written
  /// as it goes.
  /// @param request The request.
  /// @param response Where the response goes.
  /// @param stop Stops when the client has gone, if the host's server can
  /// tell. Under 2026-07-28 that cancels the call. A write the client is not
  /// there for, which `response` reports, does the same.
  void handle(const HttpRequest& request, HttpResponse& response,
              std::stop_token stop = {});

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace mcp
