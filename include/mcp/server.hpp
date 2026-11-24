/// @file
/// An MCP server whose tools are annotated C++ functions.
///
/// Server holds what a server offers. Connection speaks the protocol for one
/// client: it takes one JSON-RPC message and returns the response, with no
/// transport of its own. serve_stdio() in <mcp/stdio.hpp> runs one over the
/// standard streams.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <meta>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

/// What an MCP server offers: its identity and its tools.
///
/// Set a server up, then serve it. A Connection reads a server and never
/// changes it, so one server can serve several connections at once, from
/// several threads, when its tools allow concurrent calls.
class Server {
public:
  /// @param name The server's name, as clients show and log it.
  /// @param version The server's own version.
  Server(std::string name, std::string version);

  /// Sets the guidance a client may show its model about how to use the server.
  Server& instructions(std::string text);

  /// Sets how long a client may treat the tool list as fresh (`ttlMs`). The
  /// default is zero: a client asks again whenever it needs the list.
  Server& list_ttl(std::chrono::milliseconds ttl);

  /// Sets where failures on the server's side are written. The default writes
  /// each line to standard error. An empty function discards them.
  Server& log(Log sink);

  /// Adds the tools of a namespace, in declaration order, or one tool function.
  ///
  /// The functions follow the rules of scrydantic::bind(), with mcp::Context as
  /// the call context.
  /// @tparam Entity Reflection of a namespace or of a tool function.
  /// @tparam Tools Leave defaulted: the tools as the calling translation unit
  /// sees them.
  template <std::meta::info Entity,
            typename Tools = typename[:scrydantic::tools_of(Entity, ^^Context):]>
  Server& add() {
    return add(scrydantic::bind<Entity, Context, Tools>());
  }

  /// Adds the tool member functions of a shared object, which lives as long as
  /// the server does.
  /// @tparam Toolbox Class whose member functions carry the tool annotation.
  /// @tparam Tools Leave defaulted, as for add<Entity>().
  template <typename Toolbox, typename Tools = typename[:scrydantic::toolbox_tools_of(
                                                             ^^Toolbox, ^^Context):]>
  Server& add(std::shared_ptr<Toolbox> toolbox) {
    return add(scrydantic::bind<Context, Toolbox, Tools>(std::move(toolbox)));
  }

  /// Adds tools that are already bound, such as one built by hand around
  /// scrydantic::bind_handler().
  Server& add(std::vector<scrydantic::BoundTool<Context>> tools);

  /// Whether the server can be served. It cannot when two of its tools share a
  /// name or one has no handler; the error names the tool. A Connection to such
  /// a server answers every request with an internal error.
  [[nodiscard]] Status status() const;

  /// Writes one line to the server's log, as the server does for a failure of
  /// its own.
  void log_line(std::string_view line) const;

  /// The tools added so far, in the order they are listed to a client.
  [[nodiscard]] std::span<const scrydantic::BoundTool<Context>> tools() const noexcept {
    return tools_.tools();
  }

private:
  friend class Connection;

  // Writes the tool lists again after the tools changed.
  void write_tool_lists();

  std::string name_;
  std::string version_;
  std::optional<std::string> instructions_{};
  std::chrono::milliseconds list_ttl_{0};
  Log log_;
  scrydantic::ToolSet<Context> tools_{};
  std::optional<scrydantic::Error> problem_{};
  // The `tools` array of a tools/list result, as JSON text, for each era. The
  // legacy list leaves out an output schema that is not an object's.
  std::string modern_tool_list_{"[]"};
  std::string legacy_tool_list_{"[]"};
  // For each tool, by its position: whether a legacy client is sent its
  // result as structured content.
  std::vector<bool> legacy_structured_{};
};

/// The protocol, for one client.
///
/// A request that carries its protocol version in `_meta` is served under the
/// modern revision, and nothing is remembered between two of them. An
/// `initialize` request starts a legacy session instead, which lasts as long as
/// the connection object. One connection serves both kinds.
///
/// handle() may be called from several threads at once, so a transport can
/// serve calls side by side. The server's tools then run side by side too.
class Connection {
public:
  /// @param server The server to speak for. It must outlive the connection and
  /// must not change while the connection is in use.
  explicit Connection(const Server& server) noexcept : server_{&server} {}

  /// Handles one JSON-RPC message.
  ///
  /// It does not throw for anything a client sends or a tool does. A tool's
  /// exception ends that call as a failed call.
  /// @param message The message's text: one JSON value.
  /// @param notify Receives the notifications that belong to this request
  /// while it is handled. An empty function discards them. With several calls
  /// in flight it is called from several threads.
  /// @param stop Stops when the client cancels this request or goes away. The
  /// transport owns it, and a tool receives it as Context::stop.
  /// @return The response to send, or nothing where the protocol sends none: for
  /// a notification, for a response, and for a subscription, which stays open.
  /// A response to a request that was cancelled meanwhile is the transport's
  /// to drop.
  [[nodiscard]] std::optional<std::string> handle(std::string_view message,
                                                  const Notify& notify = {},
                                                  std::stop_token stop = {});

private:
  struct Request;

  [[nodiscard]] std::optional<std::string> dispatch(Request& request,
                                                    const Notify& notify);
  [[nodiscard]] std::string_view legacy_version() const noexcept;
  [[nodiscard]] std::string initialize(const Request& request);
  [[nodiscard]] std::string discover(const Request& request) const;
  [[nodiscard]] std::string list_tools(const Request& request) const;
  [[nodiscard]] std::string call_tool(const Request& request,
                                      const Notify& notify) const;
  [[nodiscard]] std::optional<std::string> listen(const Request& request,
                                                  const Notify& notify) const;

  const Server* server_;
  // The position in legacy_protocol_versions of the revision an `initialize`
  // request settled on, or none before one.
  static constexpr int no_legacy_version = -1;
  std::atomic<int> legacy_version_{no_legacy_version};
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
