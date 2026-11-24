/// @file
/// What a call is served with: its context, the progress it reports, and how
/// it learns that it was cancelled.
#pragma once

#include <functional>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <stop_token>
#include <string_view>

namespace mcp {

// A server's source can spell everything it needs under mcp::. These are
// Scrydantic's own annotations and types, so the Scrydantic guide describes
// them.
using scrydantic::check;
using scrydantic::description;
using scrydantic::items;
using scrydantic::Json;
using scrydantic::keyword;
using scrydantic::keyword_string;
using scrydantic::length;
using scrydantic::multiple_of;
using scrydantic::name;
using scrydantic::pattern;
using scrydantic::range;
using scrydantic::Result;
using scrydantic::skip_null;
using scrydantic::Status;
using scrydantic::title;
using scrydantic::tool;
using scrydantic::tool_error;

/// The protocol revision a Connection serves statelessly, from the metadata
/// that each request carries.
inline constexpr std::string_view modern_protocol_version = "2026-07-28";

/// The revisions a Connection serves after an `initialize` handshake, newest
/// first. A client that asks for any other is offered the first.
inline constexpr std::string_view legacy_protocol_versions[] = {
    "2025-11-25",
    "2025-06-18",
};

/// Receives one JSON-RPC notification that belongs to the request being
/// handled, as text without a line break.
using Notify = std::function<void(std::string_view message)>;

/// Receives one line about a failure on the server's side: a tool that threw,
/// or a result that its own type rejects. The text is for a developer, and is
/// never sent to the client.
using Log = std::function<void(std::string_view line)>;

/// Reports how far a tool call has come.
///
/// A tool takes one as a parameter in any position, and the server fills it in.
/// It is not an argument, so it is not in the tool's input schema. Reports
/// reach the client only when its request asked for them with a progress
/// token; otherwise report() does nothing.
class Progress {
public:
  /// A Progress that reports to no one.
  Progress() = default;

  /// Whether the client asked for progress on this call.
  [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }

  /// Sends a `notifications/progress` for this call.
  ///
  /// The protocol requires each report of one call to be greater than the last,
  /// so a report that is not, or that is not a finite number, is dropped.
  /// @param progress How far the call has come.
  /// @param total What `progress` reaches when the call is done, if known.
  /// @param message A short note for a person watching, or empty for none.
  void report(double progress, std::optional<double> total = std::nullopt,
              std::string_view message = {}) const;

private:
  friend class Connection;
  struct State;
  explicit Progress(State* state) noexcept : state_{state} {}
  State* state_{};
};

/// What the server knows about the call it is serving. It is the Scrydantic
/// call context of every tool: a tool may take it as a leading
/// `const mcp::Context&` parameter, which is not part of its input schema.
struct Context {
  /// The protocol revision this call is served under.
  std::string_view protocol_version{};
  /// Reports progress on this call.
  Progress progress{};
  /// Stops when the client cancels the call or goes away. A tool that runs for
  /// a while checks it, or waits on it, and returns early. What it returns
  /// then is not sent: a cancelled call gets no response.
  ///
  /// A tool may also take a `std::stop_token` parameter in any position, which
  /// the server fills in with this one, outside the input schema.
  std::stop_token stop{};
};

} // namespace mcp


/// Lets a tool take a std::stop_token parameter in any position: Context::stop.
template <> struct scrydantic::supplier<std::stop_token, mcp::Context> {
  static std::stop_token supply(const mcp::Context& context) noexcept {
    return context.stop;
  }
};

/// Lets a tool take an mcp::Progress parameter in any position.
template <> struct scrydantic::supplier<mcp::Progress, mcp::Context> {
  static mcp::Progress supply(const mcp::Context& context) noexcept {
    return context.progress;
  }
};
