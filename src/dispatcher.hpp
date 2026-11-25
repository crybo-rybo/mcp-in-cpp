// Runs the requests of one client on a pool of workers, as serve_stdio() and
// the in-process channel of a client both do, and stops the ones the client
// cancels.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mcp/server.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mcp::detail {

class Dispatcher {
public:
  // Starts `workers` threads that serve `connection`, and write every message
  // for the client to `out`. `out` may be called from several threads at once.
  Dispatcher(Connection& connection, Notify out, std::size_t workers);

  Dispatcher(const Dispatcher&) = delete;
  Dispatcher& operator=(const Dispatcher&) = delete;

  ~Dispatcher() { finish(); }

  // Takes one message from the client. A request waits for a worker; a
  // response, a notification, and a message that is not one are handled at
  // once, on the calling thread. A `notifications/cancelled` stops the call it
  // names.
  void submit(std::string message);

  // Stops every call, running or waiting, as if the client had cancelled it.
  void cancel_all();

  // Answers every request submitted so far, then stops the workers.
  void finish();

private:
  // One request waiting for a worker, or being handled by one.
  struct Job {
    std::string message;
    // The request's id as JSON text, by which a cancellation names it, or
    // empty for a request that cannot be cancelled.
    std::string key;
    std::stop_source stop;
  };

  void reply(const std::optional<std::string>& response) const;
  void cancel(const std::optional<scrydantic::JsonView>& params);
  void work();
  void forget(const Job& job);

  Connection* connection_;
  Notify out_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::shared_ptr<Job>> queue_;
  std::unordered_map<std::string, std::shared_ptr<Job>> running_;
  bool closed_{};
  // Last, so that the workers stop before what they use is destroyed.
  std::vector<std::jthread> workers_;
};

} // namespace mcp::detail
