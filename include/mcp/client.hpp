/// @file
/// An MCP client: the connection to one server.
///
/// A Client speaks either era of the protocol, 2026-07-28 or the `initialize`
/// handshake of 2025-11-25 and 2025-06-18, and finds out which one a server
/// speaks when it opens. A call is made by name with JSON, as a model's host
/// makes it, or with C++ types: those of the caller's choosing, or those of a
/// tool's own declaration, which a header shared with the server provides.
///
///     mcp::Reply<mcp::Client> client = mcp::connect(server, {.name = "zoo"});
///     mcp::Reply<std::vector<mcp::Tool>> tools = client->list_tools();
///     mcp::Reply<mcp::ToolResult> result =
///         client->call_tool("forecast", mcp::Json{R"({"city":"Oslo"})"});
///     mcp::Reply<weather::Forecast> forecast =
///         client->call<^^weather::forecast>({.city = "Oslo", .days = 3});
///
/// Nothing a server sends and nothing a transport does makes a client throw:
/// every call returns a Reply. Calls block, and several threads may call one
/// client at once.
///
/// This header is not included by <mcp/mcp.hpp>.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <mcp/ask.hpp>
#include <mcp/channel.hpp>
#include <mcp/content.hpp>
#include <mcp/context.hpp>
#include <memory>
#include <meta>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mcp {

class Server;

/// Which era of the protocol a client speaks.
enum class Era : std::uint8_t {
  /// Whichever the server speaks: the client asks with `server/discover`
  /// under 2026-07-28, and falls back to `initialize` when the server does not
  /// answer as a modern server does.
  automatic,
  /// 2026-07-28 only, which never falls back.
  modern,
  /// The `initialize` handshake only, asking for 2025-11-25 and accepting
  /// 2025-06-18.
  legacy,
};

/// A form that a server asks the person using the client to fill in, for
/// ClientOptions::elicitation.
struct FormRequest {
  /// What the form is for, as the server wrote it.
  std::string message{};
  /// The JSON Schema of the form, as the server wrote it: a flat object whose
  /// properties are strings, numbers, booleans, and enumerations, each of which
  /// may give a `default`. A `$ref` in it is never fetched.
  scrydantic::Json schema{};
};

/// How a client presents itself, how long it waits, and how it answers what a
/// server asks of it. Several clients may share one.
///
/// What a server may ask of its client while a call runs is answered by the
/// handlers here: a form for the person (`elicitation`), a message from the
/// client's model (`sampling`), and the client's roots (`roots`). Setting a
/// handler declares its capability to the server, in either era, and one
/// handler serves both. Under 2026-07-28 the server asks in a result of
/// `input_required`: the client runs the handlers on the calling thread, in the
/// order of the questions' keys, and sends the request again, as a new
/// request, with their answers and the server's `requestState`. In a handshake
/// session the server sends a request of its own, which the client answers on
/// a thread it starts for that request; 2026-07-28 has no such requests, so a
/// server of that revision that sends one is told -32601. A handler may take
/// as long as a person takes: while any handler runs, the time limits of the
/// client's waiting calls do not run. A handler may call the client, and may
/// be called from several threads at once. Destroying the client waits for
/// the handlers that are running to return.
///
/// When the client does not answer, the call goes on as the era allows. In a
/// handshake session the server is told with a JSON-RPC error, and the call
/// returns what the server then answers. The server is told:
///
/// - -32601 for a question no handler is set for, or a method the client does
///   not know;
/// - -32602 for a question that is not valid, such as a form in `url` mode or
///   sampling with `tools`;
/// - -32603 when a handler throws, or gives an answer that is not valid on the
///   wire, such as accepted content that is not a JSON object;
/// - -1, with the error's message, when the sampling handler returns an error.
///
/// Under 2026-07-28 a question is part of the call's result, so the client
/// sends nothing more and the call fails: with Fault::unanswered when a handler
/// throws or the sampling handler returns an error, and with Fault::protocol
/// in each of the other cases, and for an `input_required` result that is not
/// valid or that comes for a request that cannot take one.
struct ClientOptions {
  /// The client's name, as the server shows and logs it.
  std::string name{};
  /// The client's own version.
  std::string version{};
  /// Which era to speak.
  Era era{Era::automatic};
  /// How long a request may go unanswered before it is cancelled and fails
  /// with Fault::timeout. A progress report for the request starts the time
  /// again. Zero waits for ever.
  std::chrono::milliseconds timeout{std::chrono::seconds{60}};
  /// Under Era::automatic, how long the client waits for an answer to
  /// `server/discover` before it sends `initialize` as well. Whichever of the
  /// two succeeds first decides the era, so a server that starts slowly opens
  /// in its own era.
  std::chrono::milliseconds probe_timeout{std::chrono::seconds{5}};
  /// Under 2026-07-28, how many rounds of `input_required` one call answers.
  /// A server that asks once more fails the call with Fault::protocol, so that
  /// it cannot hold a call for ever.
  std::size_t input_rounds{10};
  /// Shows the person a form and returns what they did with it. The client
  /// declares elicitation in form mode when this is set.
  ///
  /// When the person accepts, the content must be a JSON object of the form's
  /// fields. A field that it leaves out and whose property in the schema has a
  /// `default` is sent with that default, as a form that starts with the
  /// defaults filled in sends it. Content given with Action::decline or
  /// Action::cancel is not sent.
  std::function<Answer<scrydantic::Json>(const FormRequest& form)> elicitation{};
  /// Asks the client's model for a message. The client declares sampling, but
  /// not its `tools` or `context`, when this is set. An error refuses the
  /// request, as a person who does not let the server use the model does.
  ///
  /// The request holds the members a SamplingRequest has; a message whose
  /// content is a list of blocks comes as one message per block, in order.
  /// The answer must have at least one block, each of them text, an image, or
  /// a sound, and only one under 2025-06-18.
  std::function<Result<SamplingResult>(const SamplingRequest& request)> sampling{};
  /// Returns the client's roots, each a `file://` URI. The client declares
  /// roots, without `listChanged`, when this is set.
  std::function<std::vector<Root>()> roots{};
};

/// A report of how far a call has come, from `notifications/progress`.
struct ProgressReport {
  /// How far the call has come. Each report of one call is greater than the
  /// last, if the server keeps to the protocol.
  double progress{};
  /// What `progress` reaches when the call is done, if the server knows.
  std::optional<double> total{};
  /// A short note for a person watching, or empty for none.
  std::string message{};
};

/// What one call may do beyond its defaults.
struct CallOptions {
  /// Receives the call's progress reports, which the server is asked for only
  /// when this is set. It is called on a thread of the channel's, one report
  /// at a time, and never once the call has returned. It must not wait for the
  /// call it reports on.
  std::function<void(const ProgressReport&)> progress{};
  /// Stops the call: the request is cancelled and the call fails with
  /// Fault::cancelled.
  std::stop_token stop{};
  /// The time limit of this call in place of ClientOptions::timeout. Zero waits
  /// for ever.
  std::optional<std::chrono::milliseconds> timeout{};
};

/// What a client learned about the server when it opened, in either era.
struct ServerInfo {
  /// The server's name. Under 2026-07-28 a server may leave it out, and then it
  /// is empty.
  std::string name{};
  /// A name for display, or empty for none.
  std::string title{};
  /// The server's own version, or empty when it was left out.
  std::string version{};
  /// The server's guidance on how to use it, if it gave any.
  std::optional<std::string> instructions{};
  /// The server's capabilities object, as the server wrote it.
  scrydantic::Json capabilities{.text = "{}"};
  /// The protocol revision the client speaks with the server.
  std::string protocol_version{};
  /// The era of that revision: Era::modern or Era::legacy.
  Era era{Era::modern};

  /// Whether the server declared a capability, such as `tools`.
  [[nodiscard]] bool has(std::string_view capability) const;
};

/// A tool that a server offers.
struct Tool {
  std::string name{};
  /// A name for display, or empty for none.
  std::string title{};
  /// What the tool does, for a model, or empty for none.
  std::string description{};
  /// The JSON Schema of the tool's arguments, as the server wrote it. A `$ref`
  /// in it is never fetched.
  scrydantic::Json input_schema{};
  /// The JSON Schema of the tool's structured content, as the server wrote it,
  /// if it gave one.
  std::optional<scrydantic::Json> output_schema{};
  /// The tool's annotations object, as the server wrote it, if it gave one.
  std::optional<scrydantic::Json> annotations{};
};

/// What a tool call returned.
///
/// A tool's own failure is a result too, with `is_error` set and the reason in
/// `content`, which a model reads and acts on.
struct ToolResult {
  /// The blocks for the model.
  Content content{};
  /// The result as a value, as the server wrote it, if it sent one.
  std::optional<scrydantic::Json> structured_content{};
  /// Whether the tool reported a failure.
  bool is_error{};
};

namespace detail {

// What a typed call of `function` returns: the function's value type, with a
// Result unwrapped, and void for void and Status.
consteval std::meta::info call_value_of(const std::meta::info function) {
  const std::meta::info type =
      std::meta::dealias(std::meta::remove_cvref(std::meta::return_type_of(function)));
  if (std::meta::has_template_arguments(type) &&
      std::meta::template_of(type) == ^^std::expected) {
    return std::meta::dealias(std::meta::template_arguments_of(type)[0]);
  }
  return type;
}

// The error of a typed call whose arguments did not encode.
[[nodiscard]] ClientError unsendable(const scrydantic::Error& error);
// The error of a typed call whose tool reported a failure: its text.
[[nodiscard]] ClientError tool_failed(const ToolResult& result);
// The error of a typed call whose result did not decode.
[[nodiscard]] ClientError mismatched(const scrydantic::Error& error);
// The JSON a typed call decodes its result from: the structured content, or
// else the one text block, read as JSON. When `text` is set, the caller wants a
// string, which a text block that is not a JSON string is as it is.
[[nodiscard]] Reply<scrydantic::Json> typed_source(const ToolResult& result, bool text);

} // namespace detail

/// The connection to one MCP server.
///
/// A client is opened by open(), or by a function that makes the channel for
/// it, such as connect(); it is never half open. It may be moved, and it
/// closes its channel when it is destroyed, when no call may be running on it.
/// A moved-from client answers every call with Fault::transport.
class Client {
public:
  /// Opens a client over a channel: the channel opens, and the client agrees
  /// on an era with the server, as ClientOptions::era says.
  /// @return The open client, or why it could not open. When the client fell
  /// back to `initialize` and that failed too, the error is the handshake's.
  [[nodiscard]] static Reply<Client> open(std::unique_ptr<Channel> channel,
                                          ClientOptions options = {});

  ~Client();
  Client(Client&& other) noexcept;
  Client& operator=(Client&& other) noexcept;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  /// What the server said of itself when the client opened.
  [[nodiscard]] const ServerInfo& server() const noexcept;

  /// Checks that the server answers. Under 2026-07-28, which has no `ping`, it
  /// sends `server/discover`.
  Reply<void> ping(const CallOptions& options = {});

  /// The tools the server offers, from every page of its list. A list that
  /// does not end within 1000 pages fails with Fault::protocol.
  Reply<std::vector<Tool>> list_tools(const CallOptions& options = {});

  /// Calls a tool by name.
  /// @param name The tool's name.
  /// @param arguments The arguments: a JSON object, which is sent as it is
  /// written. Anything else fails with Fault::protocol, unsent.
  /// @return The result, a tool's own failure included, or why the call failed.
  Reply<ToolResult> call_tool(std::string_view name,
                              const scrydantic::Json& arguments = {.text = "{}"},
                              const CallOptions& options = {});

  /// Calls a tool by name with C++ types of the caller's choosing.
  ///
  /// The arguments are encoded as Scrydantic encodes them, and must make a JSON
  /// object. The result is decoded from the call's structured content, or else
  /// from its one text block read as JSON; a `std::string` result is the text
  /// itself when it is not a JSON string. A tool's own failure is
  /// Fault::tool with the tool's text, and a result that does not decode is
  /// Fault::mismatch.
  /// @tparam T The result's type: one Scrydantic decodes, mcp::Content for the
  /// blocks themselves, or void to ignore the result.
  template <typename T, typename Arguments>
  Reply<T> call(const std::string_view name, const Arguments& arguments,
                const CallOptions& options = {}) {
    scrydantic::Result<scrydantic::Json> encoded = scrydantic::encode(arguments);
    if (!encoded) {
      return std::unexpected(detail::unsendable(encoded.error()));
    }
    Reply<ToolResult> result = call_tool(name, *encoded, options);
    if (!result) {
      return std::unexpected(std::move(result).error());
    }
    if (result->is_error) {
      return std::unexpected(detail::tool_failed(*result));
    }
    if constexpr (std::is_void_v<T>) {
      return {};
    } else if constexpr (std::is_same_v<T, Content>) {
      return std::move(result->content);
    } else {
      const Reply<scrydantic::Json> source =
          detail::typed_source(*result, std::is_same_v<T, std::string>);
      if (!source) {
        return std::unexpected(source.error());
      }
      scrydantic::Result<T> value = scrydantic::decode<T>(*source);
      if (!value) {
        return std::unexpected(detail::mismatched(value.error()));
      }
      return std::move(*value);
    }
  }

  /// Calls a tool by its declaration, which a header shared with the server
  /// provides; the function need not be defined in the client's program.
  ///
  /// The tool's name, its arguments and its result's type are the
  /// declaration's: the arguments are the parameters the server does not
  /// supply itself, such as an mcp::Progress or a std::stop_token, and the
  /// result is what the function returns, a Result unwrapped, and nothing for
  /// a function that returns void or Status. Otherwise it is call<T>().
  /// @tparam Function Reflection of a function that carries mcp::tool.
  /// @tparam Binding Leave defaulted: the function as the calling translation
  /// unit declares it.
  template <std::meta::info Function,
            typename Binding = typename[:scrydantic::binding_of(Function, ^^Context):]>
  Reply<typename[:detail::call_value_of(Function):]>
  call(const scrydantic::arguments_t<Function, Context, Binding>& arguments,
       const CallOptions& options = {}) {
    static constexpr auto described = scrydantic::describe<Function, Context>();
    return call<typename[:detail::call_value_of(Function):]>(described[0].name,
                                                             arguments, options);
  }

private:
  struct State;
  explicit Client(std::unique_ptr<State> state) noexcept;
  std::unique_ptr<State> state_;
};

/// Opens a client to a server in the same process, through an mcp::Connection
/// of its own: the loopback channel. Up to eight calls run side by side on the
/// channel's threads, as serve_stdio() runs them, and a cancelled call is
/// stopped through its Context::stop.
///
/// The server must outlive the client. Destroying the client stops the
/// server's calls that still run, such as one whose cancellation its tool has
/// not yet seen, and waits for them to return.
/// @return The open client, or why it could not open, such as a server whose
/// status() has failed.
[[nodiscard]] Reply<Client> connect(const Server& server, ClientOptions options = {});

} // namespace mcp
