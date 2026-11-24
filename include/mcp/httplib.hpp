/// @file
/// Mounts an HttpEndpoint on a cpp-httplib server.
///
/// This header is for a program that already builds with cpp-httplib: it
/// includes <httplib.h>, which the program supplies. The library itself does
/// not depend on it.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <httplib.h>
#include <mcp/http.hpp>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mcp {

namespace detail {

// The response of one request, which the endpoint writes on a thread of its
// own while cpp-httplib sends it from the thread that serves the connection.
class HttplibReply final : public HttpResponse {
public:
  void start(const int status, const std::span<const HttpHeader> headers) override {
    const std::scoped_lock lock{mutex_};
    status_ = status;
    headers_.assign(headers.begin(), headers.end());
    started_ = true;
    changed_.notify_all();
  }

  bool write(const std::string_view data) override {
    // A client that reads slowly slows the writer, rather than letting what
    // waits for it grow.
    std::unique_lock lock{mutex_};
    changed_.wait(lock, [this] { return gone_ || chunks_.size() < most_queued; });
    if (gone_) {
      return false;
    }
    chunks_.emplace_back(data);
    changed_.notify_all();
    return true;
  }

  // Marks the response complete.
  void finish() {
    const std::scoped_lock lock{mutex_};
    started_ = true;
    done_ = true;
    changed_.notify_all();
  }

  // Waits for the status and the headers, then copies them to `response`.
  // Returns whether the body is a stream.
  bool wait_for_start(httplib::Response& response, const std::function<bool()>& closed) {
    std::unique_lock lock{mutex_};
    wait(lock, closed, [this] { return started_; });
    response.status = status_ == 0 ? 500 : status_;
    bool stream = false;
    for (const HttpHeader& header : headers_) {
      if (header.name == "Content-Type") {
        content_type_ = header.value;
        stream = header.value.starts_with("text/event-stream");
      } else {
        response.set_header(header.name, header.value);
      }
    }
    return stream;
  }

  // Waits for the whole body.
  std::string wait_for_body(const std::function<bool()>& closed) {
    std::unique_lock lock{mutex_};
    wait(lock, closed, [this] { return done_; });
    std::string body;
    for (const std::string& chunk : chunks_) {
      body += chunk;
    }
    return body;
  }

  // Sends what has been written since the last call to `sink`, waiting for
  // more first. Returns false once the stream has ended or the client has gone.
  bool pump(httplib::DataSink& sink, const std::function<bool()>& closed) {
    std::unique_lock lock{mutex_};
    wait(lock, closed, [this] { return done_ || !chunks_.empty(); });
    while (!chunks_.empty()) {
      std::string chunk = std::move(chunks_.front());
      chunks_.pop_front();
      changed_.notify_all();
      lock.unlock();
      const bool sent = sink.write(chunk.data(), chunk.size());
      lock.lock();
      if (!sent) {
        gone_ = true;
        stop_.request_stop();
        changed_.notify_all();
        return false;
      }
    }
    if (done_) {
      sink.done();
      return false;
    }
    return true;
  }

  // The client has gone without the stream having ended.
  void abandon() {
    const std::scoped_lock lock{mutex_};
    gone_ = true;
    stop_.request_stop();
    changed_.notify_all();
  }

  [[nodiscard]] const std::string& content_type() const { return content_type_; }
  [[nodiscard]] std::stop_token stop_token() const { return stop_.get_token(); }

  std::jthread worker;

private:
  static constexpr std::size_t most_queued = 256;

  // Waits for `ready`, and meanwhile looks every so often for a client that
  // has closed the connection, which stops the endpoint's work.
  template <typename Ready>
  void wait(std::unique_lock<std::mutex>& lock, const std::function<bool()>& closed,
            Ready ready) {
    while (!changed_.wait_for(lock, std::chrono::milliseconds{100}, ready)) {
      if (!gone_ && closed && closed()) {
        gone_ = true;
        stop_.request_stop();
        changed_.notify_all();
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  int status_{};
  std::vector<HttpHeader> headers_;
  std::string content_type_;
  std::deque<std::string> chunks_;
  bool started_{};
  bool done_{};
  bool gone_{};
  std::stop_source stop_;
};

inline void serve_with_httplib(HttpEndpoint& endpoint, const httplib::Request& request,
                               httplib::Response& response) {
  auto reply = std::make_shared<HttplibReply>();
  std::vector<HttpHeader> headers;
  headers.reserve(request.headers.size());
  for (const auto& [name, value] : request.headers) {
    headers.push_back({.name = name, .value = value});
  }
  reply->worker = std::jthread{[&endpoint, reply = reply.get(), headers = std::move(headers),
                                method = request.method, body = request.body] {
    const HttpRequest incoming{.method = method, .headers = headers, .body = body};
    endpoint.handle(incoming, *reply, reply->stop_token());
    reply->finish();
  }};
  const std::function<bool()> closed = request.is_connection_closed;
  if (!reply->wait_for_start(response, closed)) {
    const std::string body = reply->wait_for_body(closed);
    reply->worker.join();
    if (!body.empty() || !reply->content_type().empty()) {
      response.set_content(body, reply->content_type());
    }
    return;
  }
  response.set_chunked_content_provider(
      reply->content_type(),
      [reply, closed](std::size_t, httplib::DataSink& sink) {
        return reply->pump(sink, closed);
      },
      [reply](const bool success) {
        if (!success) {
          reply->abandon();
        }
        reply->worker.join();
      });
}

} // namespace detail

/// Routes every request for `path` on `server` to `endpoint`, which must
/// outlive the server's listening.
///
/// Each request's endpoint work runs on a thread of its own, so a stream can be
/// sent while it is written. A client that closes its connection, which under
/// 2026-07-28 cancels the call, is noticed within a tenth of a second.
inline void mount(httplib::Server& server, const std::string& path, HttpEndpoint& endpoint) {
  const auto handler = [&endpoint](const httplib::Request& request,
                                   httplib::Response& response) {
    detail::serve_with_httplib(endpoint, request, response);
  };
  server.Post(path, handler);
  server.Get(path, handler);
  server.Delete(path, handler);
}

} // namespace mcp
