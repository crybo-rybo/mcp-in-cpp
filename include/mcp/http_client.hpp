/// @file
/// A client's Streamable HTTP transport, over the HTTP client that the host
/// program already has.
///
/// The library sends no HTTP itself. The program implements HttpClient over
/// its own HTTP library, and connect() opens an mcp::Client to the MCP
/// endpoint at a URL through it:
///
///     MyHttpClient http;   // an mcp::HttpClient
///     mcp::Reply<mcp::Client> client =
///         mcp::connect("http://localhost:8080/mcp", http, {.name = "zoo"});
///
/// <mcp/httplib_client.hpp> has an HttpClient over cpp-httplib.
///
/// This header is not included by <mcp/mcp.hpp>.
#pragma once

#include <functional>
#include <mcp/channel.hpp>
#include <mcp/client.hpp>
#include <mcp/http.hpp>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

/// One request that a client sends over HTTP. The views are valid until
/// HttpClient::send() returns.
struct HttpClientRequest {
  /// The request method, such as `POST` or `DELETE`.
  std::string_view method;
  /// The absolute URL, such as `https://example.com/mcp`.
  std::string_view url;
  /// The request's headers. The HTTP client adds those of its own that HTTP
  /// needs, such as `Host` and `Content-Length`, and replaces none of these.
  std::span<const HttpHeader> headers;
  /// The whole body, which may be empty.
  std::string_view body;
};

/// Where an HttpClient puts the response it receives, as it arrives. The
/// transport implements it.
///
/// The HTTP client calls start() once, then write() any number of times, one
/// call at a time and never after HttpClient::send() has returned.
class HttpClientResponse {
public:
  virtual ~HttpClientResponse() = default;

  /// Takes the status and the headers of the final response, not those of an
  /// interim one such as `100 Continue`. A response without a body has them
  /// too.
  virtual void start(int status, std::span<const HttpHeader> headers) = 0;

  /// Takes the next part of the body, decoded from its transfer coding, as
  /// soon as it has arrived. An event stream must not wait in a buffer: the
  /// transport reads each event as the server writes it.
  /// @return false when the transport wants no more of the body, after which
  /// the HTTP client closes the connection and returns.
  virtual bool write(std::string_view data) = 0;
};

/// The HTTP client of the host program, which a client's Streamable HTTP
/// transport sends its requests through.
///
/// A transport calls send() from threads of its own, one for each request it
/// has in flight, so several calls run at once; each call waits for its own
/// response. The HttpClient must outlive every mcp::Client opened with it.
class HttpClient {
public:
  virtual ~HttpClient() = default;

  /// Sends one request and hands its response to `response` as it arrives,
  /// returning when the response has ended.
  ///
  /// When `stop` is requested, which may happen from another thread at any
  /// time, the client must end the exchange promptly, within a fraction of a
  /// second even while the server sends nothing, by closing the connection.
  /// Under 2026-07-28 closing the connection is how a call is cancelled. What
  /// send() returns after a stop does not matter.
  /// @param request The request.
  /// @param response Where the status, the headers, and the body go.
  /// @param stop Ends the exchange.
  /// @return Nothing once the whole response has arrived, an event stream that
  /// the server ended included, or once write() has returned false. Otherwise
  /// why the exchange failed, with Fault::transport: the server could not be
  /// reached, the connection broke before the response ended, or the URL is
  /// one the client cannot reach, such as `https` without TLS.
  virtual Reply<void> send(const HttpClientRequest& request,
                           HttpClientResponse& response, std::stop_token stop) = 0;
};

/// What a client's Streamable HTTP transport adds to its requests.
struct HttpConnectOptions {
  /// Headers to send with every request, such as one that names the program
  /// to a gateway. A header that the transport sets itself is left out:
  /// `Content-Type`, `Accept`, `Authorization` while `bearer` is set, and
  /// those of MCP, whose names begin with `MCP-` or `Mcp-`.
  std::vector<HttpHeader> headers{};
  /// Returns the bearer token to send in `Authorization` with a request, or
  /// empty for none. It is called before each request, on the thread that
  /// sends it, so several calls may run at once; it may take as long as
  /// fetching a token takes. The program obtains the tokens itself.
  std::function<std::string()> bearer{};
};

/// Opens a client to the Streamable HTTP endpoint at `url` through `http`, in
/// either era of the protocol, as ClientOptions::era says.
///
/// Every message is a POST of its own. Under 2026-07-28 each carries the
/// headers that name its method, the tool or prompt or resource it names, and
/// the arguments that a tool's schema marks with `x-mcp-header`, and closing a
/// request's stream cancels it. Under the `initialize` handshake the client
/// keeps the session the server gives it, answers what the server asks on the
/// stream of a request, cancels with `notifications/cancelled`, and ends the
/// session with a DELETE when it closes. A session that the server no longer
/// knows (HTTP 404) ends the client: every call then fails with
/// Fault::transport.
///
/// A 4xx whose body is the request's JSON-RPC response gives that response,
/// such as an error that names the versions a server supports. Any other
/// status the protocol does not expect fails the call: 401 with
/// Fault::unauthorized, whose `data` is the server's `WWW-Authenticate`
/// challenge as a JSON string; another 4xx with Fault::protocol; and 500 and
/// above, or a redirect, which the transport does not follow, with
/// Fault::transport. So when the client opens under Era::automatic, a server
/// that answers `server/discover` with a 4xx but 401, and no modern error, is
/// sent `initialize`.
///
/// `http` must outlive the client.
/// @param url The endpoint's absolute URL, `http://` or `https://`.
/// @param http The program's HTTP client.
/// @param options How the client presents itself and what it answers.
/// @param transport What the transport adds to its requests.
/// @return The open client, or why it could not open.
[[nodiscard]] Reply<Client> connect(std::string_view url, HttpClient& http,
                                    ClientOptions options = {},
                                    HttpConnectOptions transport = {});

} // namespace mcp
