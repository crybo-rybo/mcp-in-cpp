#include "dispatcher.hpp"

#include <cstddef>
#include <expected>
#include <mcp/channel.hpp>
#include <mcp/client.hpp>
#include <mcp/server.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

namespace {

// As many calls run at once as serve_stdio() runs by default.
constexpr std::size_t loopback_workers = 8;

// A channel to a server in the same process: each message goes to a
// Connection of the channel's own, as serve_stdio() hands it the lines it
// reads, and what the connection sends comes back as it is written.
class Loopback final : public Channel {
public:
  explicit Loopback(const Server& server) : server_{&server}, connection_{server} {}

  Loopback(const Loopback&) = delete;
  Loopback& operator=(const Loopback&) = delete;

  ~Loopback() override = default;

  Reply<void> open(Incoming& incoming) override {
    if (const Status status = server_->status(); !status) {
      return std::unexpected(ClientError{.fault = Fault::transport,
                                         .code = 0,
                                         .message = status.error().message,
                                         .data = std::nullopt});
    }
    const Notify out = [&incoming](const std::string_view message) {
      incoming.message(message);
    };
    // The updates of resources a handshake session subscribed to.
    connection_.push_to(out);
    dispatcher_.emplace(connection_, out, loopback_workers);
    return {};
  }

  Reply<void> send(const Outgoing& message) override {
    dispatcher_->submit(std::string{message.message});
    return {};
  }

  // Nothing but a notification tells the connection, as on stdio.
  bool abandon(std::string_view /*id*/) override { return false; }

  void close() override {
    // No one is left to answer a question or to read a result, so every call
    // stops. The dispatcher stays until the channel is destroyed, since a
    // worker that is finishing may still hand it a message.
    connection_.client_gone();
    dispatcher_->cancel_all();
    dispatcher_->finish();
    connection_.push_to({});
  }

private:
  const Server* server_;
  Connection connection_;
  std::optional<detail::Dispatcher> dispatcher_;
};

} // namespace

Reply<Client> connect(const Server& server, ClientOptions options) {
  return Client::open(std::make_unique<Loopback>(server), std::move(options));
}

} // namespace mcp
