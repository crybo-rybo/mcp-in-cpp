/// @file
/// An MCP server whose tools, resources and prompts are annotated C++
/// functions.
///
/// Server holds what a server offers. Connection speaks the protocol for one
/// client: it takes one JSON-RPC message and returns the response, with no
/// transport of its own. serve_stdio() in <mcp/stdio.hpp> runs one over the
/// standard streams, and HttpEndpoint in <mcp/http.hpp> over HTTP.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <mcp/binding.hpp>
#include <mcp/content.hpp>
#include <mcp/context.hpp>
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

namespace wire {
struct ResultMeta;
struct ServerCapabilities;
} // namespace wire

namespace detail {

// Whether a function's return type is mcp::Content, or a Result of it.
consteval bool returns_content(const std::meta::info function) {
  const std::meta::info type =
      std::meta::dealias(std::meta::remove_cvref(std::meta::return_type_of(function)));
  const std::meta::info content = std::meta::dealias(^^Content);
  if (type == content) {
    return true;
  }
  if (std::meta::has_template_arguments(type) &&
      std::meta::template_of(type) == ^^std::expected) {
    return std::meta::dealias(std::meta::template_arguments_of(type)[0]) == content;
  }
  return false;
}

// Whether add<Entity>() has tools to bind: a namespace that declares any, or a
// tool function.
consteval bool has_tools(const std::meta::info entity) {
  if (std::meta::is_namespace(entity)) {
    return !scrydantic::annotated_functions_of(entity, ^^scrydantic::tool).empty();
  }
  return std::ranges::any_of(std::meta::annotations_of(entity), [](const std::meta::info note) {
    const std::meta::info type = std::meta::type_of(note);
    return std::meta::has_template_arguments(type) &&
           std::meta::template_of(type) == ^^scrydantic::tool;
  });
}

// For each tool that binding `entity` binds, in order: whether it returns
// mcp::Content. `entity` is a namespace, a toolbox class, or one function.
consteval std::vector<bool> content_results(const std::meta::info entity) {
  std::vector<bool> marks;
  if (std::meta::is_namespace(entity) || std::meta::is_class_type(entity)) {
    for (const std::meta::info function :
         scrydantic::annotated_functions_of(entity, ^^scrydantic::tool)) {
      marks.push_back(returns_content(function));
    }
  } else {
    marks.push_back(returns_content(entity));
  }
  return marks;
}

} // namespace detail

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

  /// Adds the tools, resources and prompts of a namespace, each in declaration
  /// order, or one function that is any of them.
  ///
  /// A tool carries mcp::tool and follows the rules of scrydantic::bind(), with
  /// mcp::Context as the call context. A resource carries mcp::resource and a
  /// prompt mcp::prompt; their parameters follow the same rules.
  /// @tparam Entity Reflection of a namespace or of a function.
  /// @tparam Tools, Resources, Prompts Leave defaulted: the functions as the
  /// calling translation unit sees them.
  template <std::meta::info Entity,
            typename Tools = typename[:scrydantic::tools_of(Entity, ^^Context):],
            typename Resources = typename[:detail::entries_of(Entity, ^^resource):],
            typename Prompts = typename[:detail::entries_of(Entity, ^^prompt):]>
  Server& add() {
    if constexpr (detail::has_tools(Entity)) {
      static constexpr std::span<const bool> content =
          std::define_static_array(detail::content_results(Entity));
      add_bound(scrydantic::bind<Entity, Context, Tools>(), content);
    }
    add(detail::bind_resources(Resources{}));
    add(detail::bind_prompts(Prompts{}));
    return *this;
  }

  /// Adds the tool member functions of a shared object, which lives as long as
  /// the server does.
  /// @tparam Toolbox Class whose member functions carry the tool annotation.
  /// @tparam Tools Leave defaulted, as for add<Entity>().
  template <typename Toolbox, typename Tools = typename[:scrydantic::toolbox_tools_of(
                                                             ^^Toolbox, ^^Context):]>
  Server& add(std::shared_ptr<Toolbox> toolbox) {
    static constexpr std::span<const bool> content =
        std::define_static_array(detail::content_results(^^Toolbox));
    return add_bound(scrydantic::bind<Context, Toolbox, Tools>(std::move(toolbox)), content);
  }

  /// Adds tools that are already bound, such as one built by hand around
  /// scrydantic::bind_handler(). Their results are values, never mcp::Content.
  Server& add(std::vector<scrydantic::BoundTool<Context>> tools);

  /// Adds resources that are already bound.
  Server& add(std::vector<BoundResource> resources);

  /// Adds prompts that are already bound.
  Server& add(std::vector<BoundPrompt> prompts);

  /// Whether the server can be served. It cannot when two of its tools share a
  /// name or one has no handler, or when two resources share a URI or two
  /// prompts a name; the error says which. A Connection to such
  /// a server answers every request with an internal error.
  [[nodiscard]] Status status() const;

  /// Writes one line to the server's log, as the server does for a failure of
  /// its own.
  void log_line(std::string_view line) const;

  /// The tools added so far, in the order they are listed to a client.
  [[nodiscard]] std::span<const scrydantic::BoundTool<Context>> tools() const noexcept {
    return tools_.tools();
  }

  /// The resources and resource templates added so far, in order.
  [[nodiscard]] std::span<const BoundResource> resources() const noexcept {
    return resources_;
  }

  /// The prompts added so far, in order.
  [[nodiscard]] std::span<const BoundPrompt> prompts() const noexcept { return prompts_; }

private:
  friend class Connection;

  // Adds bound tools. `content` says for each whether it returns mcp::Content,
  // or is empty when none does.
  Server& add_bound(std::vector<scrydantic::BoundTool<Context>> tools,
                    std::span<const bool> content);

  // Writes the tool lists again after the tools changed.
  void write_tool_lists();

  // Writes the resource and prompt lists again after they changed.
  void write_resource_lists();
  void write_prompt_list();

  // Whether the server answers completion/complete: it has a prompt or a
  // resource template.
  [[nodiscard]] bool completes() const noexcept;
  [[nodiscard]] wire::ServerCapabilities capabilities() const;
  // The `_meta` of every modern result.
  [[nodiscard]] wire::ResultMeta result_meta() const;

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
  // For each tool, by its position: whether it returns mcp::Content.
  std::vector<bool> content_{};
  std::vector<BoundResource> resources_{};
  std::vector<BoundPrompt> prompts_{};
  // The arrays of resources/list, resources/templates/list and prompts/list.
  std::string resource_list_{"[]"};
  std::string template_list_{"[]"};
  std::string prompt_list_{"[]"};
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
  // Serves a method of resources, prompts, or completion, or returns nothing
  // for another method.
  [[nodiscard]] std::optional<std::string> serve_feature(const Request& request,
                                                         const Notify& notify) const;
  [[nodiscard]] std::string list_resources(const Request& request, bool templates) const;
  [[nodiscard]] std::string read_resource(const Request& request,
                                          const Notify& notify) const;
  [[nodiscard]] std::string list_prompts(const Request& request) const;
  [[nodiscard]] std::string get_prompt(const Request& request, const Notify& notify) const;
  [[nodiscard]] std::string complete(const Request& request) const;
  // The context of a call of this request, which reports progress to `notify`.
  [[nodiscard]] Context context_of(const Request& request, Progress::State& progress,
                                   const Notify& notify) const;

  const Server* server_;
  // The position in legacy_protocol_versions of the revision an `initialize`
  // request settled on, or none before one.
  static constexpr int no_legacy_version = -1;
  std::atomic<int> legacy_version_{no_legacy_version};
};

} // namespace mcp
