#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <iostream>
#include <mcp/server.hpp>
#include <mcp/stdio.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcp {

namespace {

using scrydantic::JsonKind;
using scrydantic::JsonView;

// Writes whole lines to the output, one thread at a time.
class Lines {
public:
  explicit Lines(std::ostream& out) noexcept : out_{&out} {}

  void write(const std::string_view line) {
    const std::scoped_lock lock{mutex_};
    *out_ << line << '\n' << std::flush;
  }

private:
  std::mutex mutex_;
  std::ostream* out_;
};

// One request waiting for a worker, or being handled by one.
struct Job {
  std::string message;
  // The request's id as JSON text, by which a cancellation names it, or empty
  // for a request that cannot be cancelled.
  std::string key;
  std::stop_source stop;
};

// Hands the requests that one thread reads to a pool of workers, and stops the
// ones the client cancels.
class Dispatcher {
public:
  Dispatcher(Connection& connection, Lines& lines, const std::size_t workers)
      : connection_{&connection}, lines_{&lines} {
    workers_.reserve(workers);
    for (std::size_t worker = 0; worker < workers; ++worker) {
      workers_.emplace_back([this] { work(); });
    }
  }

  Dispatcher(const Dispatcher&) = delete;
  Dispatcher& operator=(const Dispatcher&) = delete;

  ~Dispatcher() { finish(); }

  // Takes one line from the client.
  void submit(std::string line) {
    const scrydantic::Result<JsonView> document = JsonView::parse(line);
    const std::optional<JsonView> method =
        document && document->kind() == JsonKind::object ? document->find("method")
                                                         : std::nullopt;
    if (!method.has_value()) {
      // Not a request or a notification: a response, which needs no answer, or
      // a message the connection rejects at once.
      reply(connection_->handle(line));
      return;
    }
    const std::optional<JsonView> id = document->find("id");
    if (!id.has_value()) {
      if (method->string() == "notifications/cancelled") {
        cancel(document->find("params"));
      }
      reply(connection_->handle(line));
      return;
    }

    auto job = std::make_shared<Job>();
    // The protocol forbids cancelling `initialize`.
    if (method->string() != "initialize") {
      job->key = id->to_json().text;
    }
    job->message = std::move(line);
    {
      const std::scoped_lock lock{mutex_};
      if (!job->key.empty()) {
        running_[job->key] = job;
      }
      queue_.push_back(std::move(job));
    }
    ready_.notify_one();
  }

  // Answers every request submitted so far, then stops the workers.
  void finish() {
    {
      const std::scoped_lock lock{mutex_};
      closed_ = true;
    }
    ready_.notify_all();
    workers_.clear();
  }

private:
  void reply(const std::optional<std::string>& response) {
    if (response.has_value()) {
      lines_->write(*response);
    }
  }

  void cancel(const std::optional<JsonView>& params) {
    if (!params.has_value() || params->kind() != JsonKind::object) {
      return;
    }
    const std::optional<JsonView> id = params->find("requestId");
    if (!id.has_value()) {
      return;
    }
    // A request that is not known has been answered already, or never came.
    const std::scoped_lock lock{mutex_};
    if (const auto found = running_.find(id->to_json().text); found != running_.end()) {
      found->second->stop.request_stop();
    }
  }

  void work() {
    for (;;) {
      std::shared_ptr<Job> job;
      {
        std::unique_lock lock{mutex_};
        ready_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) {
          return;
        }
        job = std::move(queue_.front());
        queue_.pop_front();
      }
      const std::stop_token stop = job->stop.get_token();
      if (!stop.stop_requested()) {
        // Nothing of a cancelled call is written, its progress included.
        const Notify notify = [this, stop](const std::string_view message) {
          if (!stop.stop_requested()) {
            lines_->write(message);
          }
        };
        const std::optional<std::string> response =
            connection_->handle(job->message, notify, stop);
        if (!stop.stop_requested()) {
          reply(response);
        }
      }
      forget(*job);
    }
  }

  void forget(const Job& job) {
    if (job.key.empty()) {
      return;
    }
    const std::scoped_lock lock{mutex_};
    // A later request may have reused the id.
    if (const auto found = running_.find(job.key);
        found != running_.end() && found->second.get() == &job) {
      running_.erase(found);
    }
  }

  Connection* connection_;
  Lines* lines_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::shared_ptr<Job>> queue_;
  std::unordered_map<std::string, std::shared_ptr<Job>> running_;
  bool closed_{};
  // Last, so that the workers stop before what they use is destroyed.
  std::vector<std::jthread> workers_;
};

} // namespace

int serve_stdio(const Server& server, std::istream& in, std::ostream& out,
                const StdioOptions options) {
  if (const Status status = server.status(); !status) {
    server.log_line(status.error().message);
    return 1;
  }
  Connection connection{server};
  Lines lines{out};
  Dispatcher dispatcher{connection, lines, std::max<std::size_t>(options.concurrency, 1)};
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      dispatcher.submit(std::move(line));
      line.clear();
    }
  }
  dispatcher.finish();
  return 0;
}

} // namespace mcp
