// The client's side of the stdio transport over a pair of streams that the
// program owns.
#include "jsonrpc.hpp"
#include "lines.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <expected>
#include <istream>
#include <mcp/channel.hpp>
#include <mcp/client.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace mcp {

namespace {

[[nodiscard]] ClientError broken(std::string message) {
  return {.fault = Fault::transport,
          .code = 0,
          .message = std::move(message),
          .data = std::nullopt};
}

// The most read from a stream at once, when it has that much ready.
constexpr std::size_t chunk_size = std::size_t{64} << 10U;

class Streams final : public Channel {
public:
  Streams(std::istream& in, std::ostream& out, const std::size_t max_line) noexcept
      : in_{&in}, out_{&out}, max_line_{max_line} {}

  Streams(const Streams&) = delete;
  Streams& operator=(const Streams&) = delete;

  ~Streams() override = default;

  Reply<void> open(Incoming& incoming) override {
    try {
      reader_ = std::jthread{[this, &incoming] { read(incoming); }};
    } catch (const std::system_error& error) {
      return std::unexpected(
          broken(jsonrpc::join({"cannot start a thread to read: ", error.what()})));
    }
    return {};
  }

  Reply<void> send(const Outgoing& message) override {
    const std::scoped_lock lock{writing_};
    if (closing_.load()) {
      return std::unexpected(broken("the client is closed"));
    }
    try {
      *out_ << message.message << '\n' << std::flush;
    } catch (...) {
      // The stream was told to throw. It has failed all the same.
      static_cast<void>(std::current_exception());
    }
    if (!*out_) {
      return std::unexpected(broken("cannot write to the server: the stream failed"));
    }
    return {};
  }

  // Nothing but a notification tells a server on stdio.
  bool abandon(std::string_view /*id*/) override { return false; }

  // Stops the client from taking what the server sends, then waits for the
  // read in progress, which only the end of the stream ends.
  void close() override {
    {
      const std::scoped_lock lock{writing_};
      closing_.store(true);
    }
    if (reader_.joinable()) {
      reader_.join();
    }
  }

private:
  void read(Incoming& incoming);

  std::istream* in_;
  std::ostream* out_;
  std::size_t max_line_;
  std::mutex writing_;
  std::atomic<bool> closing_{false};
  // Last, so that it stops before what it reads is destroyed.
  std::jthread reader_;
};

void Streams::read(Incoming& incoming) {
  detail::LineSplitter lines{max_line_};
  // Once the client has been told the channel is gone, it is told nothing more.
  bool ended = false;
  const auto end = [&](std::string message) {
    if (!ended && !closing_.load()) {
      incoming.closed(broken(std::move(message)));
    }
    ended = true;
  };
  const auto message = [&](const std::string_view line) {
    if (!line.empty() && !ended && !closing_.load()) {
      incoming.message(line);
    }
  };
  const auto overlong = [&](std::string_view /*start*/) {
    end(jsonrpc::join({"the server sent a line longer than ", std::to_string(max_line_),
                       " bytes, the most this client reads"}));
  };

  std::string why{"the server's stream ended"};
  try {
    std::streambuf* const buffer = in_->rdbuf();
    std::string chunk(chunk_size, '\0');
    while (buffer != nullptr) {
      // What the buffer holds is taken at once, and otherwise one character is
      // waited for.
      const std::streamsize ready = buffer->in_avail();
      if (ready > 0) {
        const std::streamsize got = buffer->sgetn(
            chunk.data(), std::min(ready, static_cast<std::streamsize>(chunk.size())));
        lines.take(std::string_view{chunk.data(), static_cast<std::size_t>(got)},
                   message, overlong);
        continue;
      }
      const std::streambuf::int_type next = buffer->sbumpc();
      if (std::streambuf::traits_type::eq_int_type(
              next, std::streambuf::traits_type::eof())) {
        break;
      }
      const char letter = std::streambuf::traits_type::to_char_type(next);
      lines.take(std::string_view{&letter, 1}, message, overlong);
    }
    lines.finish(message, overlong);
  } catch (...) {
    static_cast<void>(std::current_exception());
    why = "reading from the server failed";
  }
  end(std::move(why));
}

} // namespace

Reply<Client> connect(std::istream& in, std::ostream& out, ClientOptions options,
                      const std::size_t max_line) {
  return Client::open(std::make_unique<Streams>(in, out, max_line), std::move(options));
}

} // namespace mcp
