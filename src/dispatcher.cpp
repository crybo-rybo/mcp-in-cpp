#include "dispatcher.hpp"

#include <cstddef>
#include <mcp/server.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace mcp::detail {

using scrydantic::JsonKind;
using scrydantic::JsonView;

Dispatcher::Dispatcher(Connection& connection, Notify out, const std::size_t workers)
    : connection_{&connection}, out_{std::move(out)} {
  workers_.reserve(workers);
  for (std::size_t worker = 0; worker < workers; ++worker) {
    workers_.emplace_back([this] { work(); });
  }
}

void Dispatcher::submit(std::string message) {
  const scrydantic::Result<JsonView> document = JsonView::parse(message);
  const std::optional<JsonView> method =
      document && document->kind() == JsonKind::object ? document->find("method")
                                                       : std::nullopt;
  if (!method.has_value()) {
    // Not a request or a notification: a response, which needs no answer, or
    // a message the connection rejects at once.
    reply(connection_->handle(message));
    return;
  }
  const std::optional<JsonView> id = document->find("id");
  if (!id.has_value()) {
    if (method->string() == "notifications/cancelled") {
      cancel(document->find("params"));
    }
    reply(connection_->handle(message));
    return;
  }

  auto job = std::make_shared<Job>();
  // The protocol forbids cancelling `initialize`.
  if (method->string() != "initialize") {
    job->key = id->to_json().text;
  }
  job->message = std::move(message);
  {
    const std::scoped_lock lock{mutex_};
    if (!job->key.empty()) {
      running_[job->key] = job;
    }
    queue_.push_back(std::move(job));
  }
  ready_.notify_one();
}

void Dispatcher::cancel_all() {
  const std::scoped_lock lock{mutex_};
  for (const auto& [key, job] : running_) {
    job->stop.request_stop();
  }
  for (const std::shared_ptr<Job>& job : queue_) {
    job->stop.request_stop();
  }
}

void Dispatcher::finish() {
  {
    const std::scoped_lock lock{mutex_};
    closed_ = true;
  }
  ready_.notify_all();
  workers_.clear();
}

void Dispatcher::reply(const std::optional<std::string>& response) const {
  if (response.has_value() && out_) {
    out_(*response);
  }
}

void Dispatcher::cancel(const std::optional<JsonView>& params) {
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

void Dispatcher::work() {
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
        if (!stop.stop_requested() && out_) {
          out_(message);
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

void Dispatcher::forget(const Job& job) {
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

} // namespace mcp::detail
