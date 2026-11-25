// A scripted channel for the client's tests: the test plays the server, and
// sees every message the client sends.
#pragma once

#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mcp/client.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scripted {

using namespace std::chrono_literals;

// How long a test waits for anything that should happen at once.
inline constexpr auto patience = 10s;

// One message the client sent, with its routing fields.
struct Sent {
  std::string message;
  std::string id;
  std::string method;
  std::string name;
  std::string protocol_version;
};

// The server's side of a scripted channel: it keeps what the client sends,
// and the test answers when and how it likes.
class Script {
public:
  // Called with each message the client sends, on the thread that sends it.
  // It may answer at once with deliver(). Set it before the client sends.
  std::function<void(Script&, const Sent&)> respond;
  // Whether abandoning a request tells the server, as closing an HTTP
  // request's stream does under 2026-07-28.
  bool abandon_tells{false};
  // When set, every send fails with it.
  std::optional<mcp::ClientError> refuse{};

  // Hands a message to the client, as if the server had sent it.
  void deliver(const std::string_view text) {
    if (mcp::Incoming* const incoming = target()) {
      incoming->message(text);
    }
  }

  void fail(const std::string_view id, mcp::ClientError why) {
    if (mcp::Incoming* const incoming = target()) {
      incoming->failed(id, std::move(why));
    }
  }

  void close(mcp::ClientError why) {
    if (mcp::Incoming* const incoming = target()) {
      incoming->closed(std::move(why));
    }
  }

  [[nodiscard]] std::vector<Sent> sent() const {
    const std::scoped_lock lock{mutex_};
    return sent_;
  }

  [[nodiscard]] std::vector<std::string> messages() const {
    std::vector<std::string> texts;
    for (const Sent& sent : this->sent()) {
      texts.push_back(sent.message);
    }
    return texts;
  }

  // What happened on the channel, in order: `send <method>`, `abandon <id>`,
  // `close`.
  [[nodiscard]] std::vector<std::string> events() const {
    const std::scoped_lock lock{mutex_};
    return events_;
  }

  // Waits until the client has sent `count` messages of `method`, and returns
  // the last of them.
  [[nodiscard]] std::optional<Sent> await(const std::string_view method,
                                          const std::size_t count = 1) const {
    std::unique_lock lock{mutex_};
    std::optional<Sent> found;
    changed_.wait_for(lock, patience, [&] {
      std::size_t seen = 0;
      for (const Sent& sent : sent_) {
        if (sent.method == method && ++seen == count) {
          found = sent;
          return true;
        }
      }
      return false;
    });
    return found;
  }

  // Waits until the client has sent a message that `wanted` picks, and
  // returns the first such.
  [[nodiscard]] std::optional<Sent>
  await_if(const std::function<bool(const Sent&)>& wanted) const {
    std::unique_lock lock{mutex_};
    std::optional<Sent> found;
    changed_.wait_for(lock, patience, [&] {
      for (const Sent& sent : sent_) {
        if (wanted(sent)) {
          found = sent;
          return true;
        }
      }
      return false;
    });
    return found;
  }

private:
  friend class Scripted;

  [[nodiscard]] mcp::Incoming* target() const {
    const std::scoped_lock lock{mutex_};
    return incoming_;
  }

  void record(Sent sent) {
    {
      const std::scoped_lock lock{mutex_};
      events_.push_back("send " + sent.method);
      sent_.push_back(std::move(sent));
    }
    changed_.notify_all();
  }

  void note(std::string event) {
    const std::scoped_lock lock{mutex_};
    events_.push_back(std::move(event));
  }

  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  mcp::Incoming* incoming_{};
  std::vector<Sent> sent_;
  std::vector<std::string> events_;
};

class Scripted final : public mcp::Channel {
public:
  explicit Scripted(std::shared_ptr<Script> script) : script_{std::move(script)} {}

  mcp::Reply<void> open(mcp::Incoming& incoming) override {
    const std::scoped_lock lock{script_->mutex_};
    script_->incoming_ = &incoming;
    return {};
  }

  mcp::Reply<void> send(const mcp::Outgoing& message) override {
    if (script_->refuse.has_value()) {
      return std::unexpected(*script_->refuse);
    }
    const Sent sent{.message = std::string{message.message},
                    .id = std::string{message.id},
                    .method = std::string{message.method},
                    .name = std::string{message.name},
                    .protocol_version = std::string{message.protocol_version}};
    script_->record(sent);
    if (script_->respond) {
      script_->respond(*script_, sent);
    }
    return {};
  }

  bool abandon(const std::string_view id) override {
    script_->note("abandon " + std::string{id});
    return script_->abandon_tells;
  }

  void close() override {
    script_->note("close");
    const std::scoped_lock lock{script_->mutex_};
    script_->incoming_ = nullptr;
  }

private:
  std::shared_ptr<Script> script_;
};

inline constexpr std::string_view discovered =
    R"({"_meta":{"io.modelcontextprotocol/serverInfo":{"name":"script","version":"1.0"}},)"
    R"("capabilities":{ "tools" : {} },"instructions":"Be kind.","resultType":"complete",)"
    R"("supportedVersions":["2026-07-28","2025-11-25"]})";

inline std::string initialized(const std::string_view version) {
  return std::string{R"({"capabilities":{"tools":{}},"protocolVersion":")"} +
         std::string{version} +
         R"(","serverInfo":{"name":"script","title":"Script","version":"1.0"}})";
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

inline std::string error_with(const std::string_view id, const int code,
                              const std::string_view message,
                              const std::string_view data) {
  return std::string{R"({"error":{"code":)"} + std::to_string(code) + R"(,"data":)" +
         std::string{data} + R"(,"message":")" + std::string{message} + R"("},"id":)" +
         std::string{id} + R"(,"jsonrpc":"2.0"})";
}

// Answers each request of these methods with its result, and leaves the rest.
inline std::function<void(Script&, const Sent&)>
answering(std::map<std::string, std::string, std::less<>> results) {
  return [results = std::move(results)](Script& script, const Sent& sent) {
    if (sent.id.empty()) {
      return;
    }
    if (const auto found = results.find(sent.method); found != results.end()) {
      script.deliver(support::result(sent.id, found->second));
    }
  };
}

inline mcp::ClientOptions options(const mcp::Era era = mcp::Era::automatic) {
  return {.name = "zoo",
          .version = "0.3.0",
          .era = era,
          .timeout = std::chrono::seconds{60},
          .probe_timeout = std::chrono::seconds{60}};
}

inline mcp::Reply<mcp::Client> open(const std::shared_ptr<Script>& script,
                                    const mcp::ClientOptions& with = options()) {
  return mcp::Client::open(std::make_unique<Scripted>(script), with);
}

// A client that opened in the modern era, and its script, which answers
// nothing more until the test says how.
struct Opened {
  std::shared_ptr<Script> script;
  mcp::Reply<mcp::Client> client;
};

inline Opened open_in(const mcp::Era era, mcp::ClientOptions with = options()) {
  auto script = std::make_shared<Script>();
  script->respond = answering({{"server/discover", std::string{discovered}},
                               {"initialize", initialized("2025-11-25")}});
  with.era = era;
  mcp::Reply<mcp::Client> client = open(script, with);
  script->respond = {};
  return {.script = std::move(script), .client = std::move(client)};
}

// The text of a call's one text block.
inline std::string text_of(const mcp::ToolResult& result) {
  REQUIRE(result.content.size() == 1);
  const auto* const text = std::get_if<mcp::TextContent>(&result.content.front());
  REQUIRE(text != nullptr);
  return text->text;
}

} // namespace scripted
