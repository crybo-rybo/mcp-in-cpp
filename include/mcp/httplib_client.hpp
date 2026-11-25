/// @file
/// An mcp::HttpClient over cpp-httplib, for a client's Streamable HTTP
/// transport.
///
///     mcp::HttplibClient http;
///     mcp::Reply<mcp::Client> client =
///         mcp::connect("http://localhost:8080/mcp", http, {.name = "zoo"});
///
/// This header is for a program that already builds with cpp-httplib: it
/// includes <httplib.h>, which the program supplies. The library itself does
/// not depend on it. An `https` URL needs cpp-httplib built with a TLS backend,
/// such as OpenSSL with CPPHTTPLIB_OPENSSL_SUPPORT defined; without one, a
/// request to it fails with Fault::transport.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <httplib.h>
#include <mcp/channel.hpp>
#include <mcp/http.hpp>
#include <mcp/http_client.hpp>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mcp {

/// Sends each request on a connection of its own, through an httplib::Client,
/// and hands its response over as it arrives. It may be used from several
/// threads at once.
class HttplibClient final : public HttpClient {
public:
  /// @param configure Called with the httplib::Client of each connection before
  /// it sends, to set what the program needs, such as CA certificates, a proxy,
  /// or other time limits. By default a connection may take 10 seconds to
  /// open, and its response may be quiet for as long as the server likes: the
  /// mcp::Client's own time limits bound how long a call waits.
  explicit HttplibClient(std::function<void(httplib::Client&)> configure = {})
      : configure_{std::move(configure)} {}

  Reply<void> send(const HttpClientRequest& request, HttpClientResponse& response,
                   std::stop_token stop) override {
    const auto failed = [](std::string message) -> Reply<void> {
      return std::unexpected(ClientError{.fault = Fault::transport,
                                         .code = 0,
                                         .message = std::move(message),
                                         .data = std::nullopt});
    };
    const std::string_view url = request.url;
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
      return failed("the URL is not absolute: " + std::string{url});
    }
    const std::string scheme = lowercase(url.substr(0, scheme_end));
    if (scheme != "http" && scheme != "https") {
      return failed("cpp-httplib cannot send to a URL of scheme " + scheme);
    }
#ifndef CPPHTTPLIB_SSL_ENABLED
    if (scheme == "https") {
      return failed("an https URL needs cpp-httplib built with TLS, such as OpenSSL "
                    "with CPPHTTPLIB_OPENSSL_SUPPORT defined");
    }
#endif
    // The origin names the connection; the rest is the request's target.
    std::string_view target = url.substr(scheme_end + 3);
    const std::size_t slash = target.find('/');
    const std::string origin = scheme + "://" + std::string{target.substr(0, slash)};
    target =
        slash == std::string_view::npos ? std::string_view{"/"} : target.substr(slash);
    target = target.substr(0, target.find('#'));

    std::optional<httplib::Client> client;
    try {
      client.emplace(origin);
    } catch (const std::exception& error) {
      return failed("cpp-httplib cannot reach " + origin + ": " + error.what());
    }
    if (!client->is_valid()) {
      return failed("cpp-httplib cannot reach " + origin);
    }
    client->set_connection_timeout(std::chrono::seconds{10});
    client->set_read_timeout(std::chrono::hours{24});
    if (configure_) {
      configure_(*client);
    }

    httplib::Request sent;
    sent.method = std::string{request.method};
    sent.path = std::string{target};
    sent.body = std::string{request.body};
    for (const HttpHeader& header : request.headers) {
      sent.headers.emplace(header.name, header.value);
    }
    bool started = false;
    bool refused = false;
    sent.response_handler = [&](const httplib::Response& received) {
      std::vector<HttpHeader> headers;
      headers.reserve(received.headers.size());
      for (const auto& [name, value] : received.headers) {
        headers.push_back({.name = name, .value = value});
      }
      started = true;
      response.start(received.status, headers);
      return !stop.stop_requested();
    };
    sent.content_receiver = [&](const char* data, const std::size_t length,
                                std::size_t /*offset*/, std::size_t /*total*/) {
      if (stop.stop_requested()) {
        return false;
      }
      if (!response.write(std::string_view{data, length})) {
        refused = true;
        return false;
      }
      return true;
    };

    httplib::Response received;
    httplib::Error error = httplib::Error::Success;
    bool ok = false;
    {
      // cpp-httplib stops a request only once it has a socket, so a stop that
      // comes before is tried again until the request returns. The stop runs
      // on the thread that asked for it, never on this one.
      std::mutex mutex;
      std::condition_variable changed;
      bool done = false;
      const std::thread::id sender = std::this_thread::get_id();
      const std::stop_callback stopping{stop, [&] {
                                          if (std::this_thread::get_id() == sender) {
                                            return;
                                          }
                                          std::unique_lock lock{mutex};
                                          while (!done) {
                                            lock.unlock();
                                            client->stop();
                                            lock.lock();
                                            changed.wait_for(
                                                lock, std::chrono::milliseconds{20},
                                                [&done] { return done; });
                                          }
                                        }};
      if (!stop.stop_requested()) {
        ok = client->send(sent, received, error);
      }
      {
        const std::scoped_lock lock{mutex};
        done = true;
      }
      changed.notify_all();
    }
    if (ok) {
      if (!started) {
        // A response that has no body, such as 202 or 204, has no handler call.
        std::vector<HttpHeader> headers;
        for (const auto& [name, value] : received.headers) {
          headers.push_back({.name = name, .value = value});
        }
        response.start(received.status, headers);
      }
      return {};
    }
    if (refused || stop.stop_requested()) {
      return {};
    }
    return failed("the request to " + origin + " failed: " + httplib::to_string(error));
  }

private:
  [[nodiscard]] static std::string lowercase(const std::string_view text) {
    std::string lowered{text};
    for (char& c : lowered) {
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c - 'A' + 'a');
      }
    }
    return lowered;
  }

  std::function<void(httplib::Client&)> configure_;
};

} // namespace mcp
