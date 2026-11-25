/// @file
/// What carries a client's messages to one server and back: the interface a
/// transport implements, and the errors a client reports.
///
/// mcp::Client speaks the protocol and a Channel moves its messages. A
/// transport implements Channel alone, and the client calls it; the channel
/// hands what the server sends to the client's Incoming. <mcp/client.hpp> has
/// the channels this library provides.
#pragma once

#include <cstdint>
#include <expected>
#include <mcp/context.hpp>
#include <mcp/http.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mcp {

/// Why a client's call failed.
enum class Fault : std::uint8_t {
  /// The server cannot be reached, or it went away.
  transport,
  /// No answer came in time. The request was cancelled.
  timeout,
  /// The caller's stop token stopped the call. The request was cancelled.
  cancelled,
  /// The server answered with a JSON-RPC error, whose code, message and data
  /// the error keeps.
  rejected,
  /// The server sent what the protocol does not allow, or the call could not
  /// be sent as the protocol requires.
  protocol,
  /// A typed call only: the tool reported an error (`isError`). The message is
  /// the tool's text.
  tool,
  /// A typed call only: the result does not fit the C++ type.
  mismatch,
  /// The server requires authorization (HTTP 401).
  unauthorized,
};

/// What went wrong with a client's call.
struct ClientError {
  Fault fault{Fault::transport};
  /// The JSON-RPC error code, for Fault::rejected, and zero otherwise.
  int code{};
  /// What happened, for a developer. For Fault::rejected it is the server's own
  /// message, and for Fault::tool the tool's text.
  std::string message{};
  /// The `data` of a JSON-RPC error, as the server wrote it, when it had one.
  std::optional<scrydantic::Json> data{};
};

/// What a client's call returns: its result, or why it failed.
template <typename T> using Reply = std::expected<T, ClientError>;

/// One message for a channel to send, and what a transport with headers needs
/// to route it. The views are valid only during the call that passes it.
struct Outgoing {
  /// One JSON-RPC message, without a line break.
  std::string_view message;
  /// The request's id as JSON text, or empty for a notification or a response.
  std::string_view id;
  /// The method of a request or a notification, or empty for a response.
  std::string_view method;
  /// What a request names: `params.name` of `tools/call` and `prompts/get`,
  /// `params.uri` of `resources/read`, and otherwise empty.
  std::string_view name;
  /// The protocol revision the message is sent under, or empty before a
  /// handshake has agreed one.
  std::string_view protocol_version;
  /// Further headers for a transport that has them.
  std::span<const HttpHeader> headers;
};

/// What a channel tells its client. The client implements it.
///
/// Its functions may be called from any thread of the channel's, and from
/// several at once.
class Incoming {
public:
  virtual ~Incoming() = default;

  /// Takes one message from the server, as its text: a response, a
  /// notification, or a request of the server's.
  virtual void message(std::string_view text) = 0;

  /// Says that the request sent with this id will get no answer, and why: its
  /// HTTP response ended, or came with an error status.
  /// @param id The id as JSON text, as Outgoing::id gave it.
  virtual void failed(std::string_view id, ClientError why) = 0;

  /// Says that the channel is gone, such as when the server's process ended.
  /// Every call that waits fails with `why`, and so does every later one.
  virtual void closed(ClientError why) = 0;
};

/// Carries the messages of one client to one server and back.
///
/// The client calls open() first and once. When that succeeds, it calls send()
/// and abandon() as it needs, and close() once at the end; when open() fails,
/// it destroys the channel without calling close().
class Channel {
public:
  virtual ~Channel() = default;

  /// Starts the channel. From here until close() returns, the channel hands
  /// what the server sends to `incoming`, which lives that long.
  /// @return Nothing, or why the server cannot be reached.
  virtual Reply<void> open(Incoming& incoming) = 0;

  /// Hands one message over to be sent, without waiting for an answer.
  ///
  /// It may be called from several threads at once, and from inside a call of
  /// Incoming::message on the channel's own thread, which must not deadlock.
  /// @return Nothing, or why the message could not be sent.
  virtual Reply<void> send(const Outgoing& message) = 0;

  /// Gives up on a request that was sent and not answered. A transport in
  /// which each request has a stream of its own closes that stream.
  /// @param id The id as JSON text, as Outgoing::id gave it.
  /// @return Whether that alone told the server, as closing the stream of an
  /// HTTP request does under 2026-07-28. When it did not, the client sends
  /// `notifications/cancelled`.
  virtual bool abandon(std::string_view id) = 0;

  /// Ends the channel. Once it returns, the channel calls its Incoming no more.
  virtual void close() = 0;
};

} // namespace mcp
