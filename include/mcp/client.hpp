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
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <mcp/annotations.hpp>
#include <mcp/ask.hpp>
#include <mcp/binding.hpp>
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

/// One log message of the server's, from `notifications/message`.
struct LogMessage {
  /// How severe it is.
  LogLevel level{LogLevel::info};
  /// The name of the logger that wrote it, or empty when the server named none.
  std::string logger{};
  /// What the server logged, as the server wrote it: any JSON, often a string.
  scrydantic::Json data{};
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
  /// Receives the server's log messages. The client asks for them only when
  /// this is set. Under 2026-07-28 each request carries `log_level`, or the
  /// call's own CallOptions::log_level, in its `_meta`, and the server logs
  /// only for a request that asks, at that level or above. In a handshake
  /// session the level is the session's: once the handshake is done, the
  /// client sends `logging/setLevel` with `log_level` if the server declared
  /// `logging`, and passes over any message less severe. A server that refuses
  /// the level does not stop the client from opening; one that gives no
  /// answer to it does.
  ///
  /// It is called on a thread of the channel's as a message arrives, and may
  /// be called from several threads at once. It must return soon, and must
  /// not wait for a call of this client, whose answer may have to come through
  /// the same thread. What it throws is caught and dropped.
  std::function<void(const LogMessage& message)> log{};
  /// The least severe log message the client asks for.
  LogLevel log_level{LogLevel::info};
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
  /// Under 2026-07-28, the least severe log message this call asks for in
  /// place of ClientOptions::log_level. A handshake session has one level for
  /// all its calls, so there it changes nothing.
  std::optional<LogLevel> log_level{};
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

/// A resource that a server offers, from `resources/list`.
struct Resource {
  /// The URI it is read by.
  std::string uri{};
  std::string name{};
  /// A name for display, or empty for none.
  std::string title{};
  /// What the resource is, or empty for none.
  std::string description{};
  /// The media type of its contents, or empty when the server did not say.
  std::string mime_type{};
  /// Its size in bytes, if the server knows.
  std::optional<std::int64_t> size{};
  /// Its annotations object, as the server wrote it, if it gave one.
  std::optional<scrydantic::Json> annotations{};
  /// Its icons, as the server wrote them, if it gave any.
  std::optional<scrydantic::Json> icons{};
  /// Its `_meta` object, as the server wrote it, if it gave one.
  std::optional<scrydantic::Json> meta{};
};

/// A template of resources that a server offers, from
/// `resources/templates/list`. Its URI is filled in to make the URI of a
/// resource, as read() does from a declaration.
struct ResourceTemplate {
  /// The URI template, such as `weather://{city}/alerts`.
  std::string uri_template{};
  std::string name{};
  /// A name for display, or empty for none.
  std::string title{};
  /// What its resources are, or empty for none.
  std::string description{};
  /// The media type of every resource it makes, or empty when the server did
  /// not say.
  std::string mime_type{};
  /// Its annotations object, as the server wrote it, if it gave one.
  std::optional<scrydantic::Json> annotations{};
  /// Its icons, as the server wrote them, if it gave any.
  std::optional<scrydantic::Json> icons{};
  /// Its `_meta` object, as the server wrote it, if it gave one.
  std::optional<scrydantic::Json> meta{};
};

/// A prompt that a server offers, from `prompts/list`.
struct Prompt {
  /// One argument of the prompt, which get_prompt() gives as text.
  struct Argument {
    std::string name{};
    /// A name for display, or empty for none.
    std::string title{};
    /// What the argument is, or empty for none.
    std::string description{};
    /// Whether the prompt needs it.
    bool required{};
  };

  std::string name{};
  /// A name for display, or empty for none.
  std::string title{};
  /// What the prompt does, or empty for none.
  std::string description{};
  std::vector<Argument> arguments{};
  /// Its icons, as the server wrote them, if it gave any.
  std::optional<scrydantic::Json> icons{};
  /// Its `_meta` object, as the server wrote it, if it gave one.
  std::optional<scrydantic::Json> meta{};
};

/// A prompt as the server rendered it for some arguments, from `prompts/get`.
struct PromptResult {
  /// What the prompt is, or empty when the server did not say.
  std::string description{};
  /// The messages, in order.
  std::vector<PromptMessage> messages{};
};

/// Names a prompt whose argument complete() completes.
struct PromptReference {
  std::string name{};
};

/// Names a resource template whose variable complete() completes, by its URI
/// template as the server lists it.
struct ResourceTemplateReference {
  std::string uri{};
};

/// What a server suggests for an argument, from `completion/complete`.
struct Completion {
  /// The suggestions, the most fitting first.
  std::vector<std::string> values{};
  /// How many suggestions there are in all, if the server knows.
  std::optional<std::int64_t> total{};
  /// Whether there are more suggestions than `values` holds.
  bool has_more{};
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
// The value inside structured content of the form `{"result": value}`, as some
// servers wrap a result that is not an object, or nothing for any other form.
[[nodiscard]] std::optional<scrydantic::Json> wrapped_result(const ToolResult& result);

// Whether a function carries an annotation of this type.
consteval bool carries(const std::meta::info function,
                       const std::meta::info annotation) {
  return std::meta::is_function(function) &&
         !std::meta::annotations_of_with_type(function, annotation).empty();
}

// The annotation that a function carries, or `none` when it carries none, so
// that a static_assert alone reports the mistake.
template <typename Annotation>
consteval Annotation carried(const std::meta::info function, const Annotation none) {
  return carries(function, ^^Annotation) ? annotation_of<Annotation>(function) : none;
}

// Why a typed read cannot read `function`, or empty.
template <typename Arguments>
consteval std::string_view read_problem(const std::meta::info function) {
  if (!carries(function, ^^resource)) {
    return "read<^^f>() reads a function that carries mcp::resource";
  }
  return resource_problem<Arguments>(annotation_of<resource>(function).uri.view());
}

// What a typed read of `function` returns: the function's value type, with a
// Result unwrapped.
consteval std::meta::info read_value_of(const std::meta::info function) {
  return std::meta::is_function(function) ? call_value_of(function) : ^^void;
}

// The URI of a template with its variables filled in from `arguments`, a JSON
// object of the variables' values: `{name}` percent-encoded, `{+name}` as it
// is. The URI is one that match_uri_template() reads back to the same values.
[[nodiscard]] Reply<std::string> expand_uri(std::string_view uri_template,
                                            const scrydantic::Json& arguments);
// The text of each member of `arguments`, a JSON object, as a prompt takes
// it: a string as it is, a number or a boolean as JSON spells it. A member
// that is null is left out.
[[nodiscard]] Reply<std::map<std::string, std::string>>
argument_texts(const scrydantic::Json& arguments);
// What a resource holds, from the one item of its contents: its text, its
// bytes, or the item itself.
[[nodiscard]] Reply<std::string> resource_text(std::vector<ResourceContents> contents);
[[nodiscard]] Reply<std::vector<std::byte>>
resource_bytes(const std::vector<ResourceContents>& contents);
[[nodiscard]] Reply<ResourceContents>
resource_item(std::vector<ResourceContents> contents);

// A value of type T from the contents of a resource, as the server's binding
// makes the contents from the value of a function: the inverse of
// detail::resource_contents.
template <typename T> Reply<T> resource_value(std::vector<ResourceContents> contents) {
  if constexpr (std::is_same_v<T, std::vector<ResourceContents>>) {
    return contents;
  } else if constexpr (std::is_same_v<T, ResourceContents>) {
    return resource_item(std::move(contents));
  } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
    return resource_bytes(contents);
  } else {
    Reply<std::string> text = resource_text(std::move(contents));
    if constexpr (std::is_same_v<T, std::string>) {
      return text;
    } else {
      if (!text) {
        return std::unexpected(std::move(text).error());
      }
      scrydantic::Result<T> value =
          scrydantic::decode<T>(scrydantic::Json{std::move(*text)});
      if (!value) {
        return std::unexpected(mismatched(value.error()));
      }
      return std::move(*value);
    }
  }
}

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
  /// itself when it is not a JSON string. Structured content of the form
  /// `{"result": value}`, as the official Python SDK's servers wrap a result
  /// that is not an object, is read as the value when the type does not take
  /// the wrapper itself. A tool's own failure is Fault::tool with the tool's
  /// text, and a result that does not decode is Fault::mismatch.
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
        // The value may be wrapped, when the type did not take the wrapper.
        if (const std::optional<scrydantic::Json> wrapped =
                detail::wrapped_result(*result)) {
          if (scrydantic::Result<T> inner = scrydantic::decode<T>(*wrapped)) {
            return std::move(*inner);
          }
        }
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

  /// The resources the server offers, from every page of its list. A list
  /// that does not end within 1000 pages fails with Fault::protocol.
  Reply<std::vector<Resource>> list_resources(const CallOptions& options = {});

  /// The resource templates the server offers, from every page of their list,
  /// as list_resources() reads it.
  Reply<std::vector<ResourceTemplate>>
  list_resource_templates(const CallOptions& options = {});

  /// Reads a resource by its URI.
  ///
  /// A URI the server has no resource for is Fault::rejected with the server's
  /// code: -32602 under 2026-07-28 and -32002 before it, as the revisions
  /// define it, usually with the URI in the error's data.
  /// @return The contents, each text or a blob, in the server's order.
  Reply<std::vector<ResourceContents>> read_resource(std::string_view uri,
                                                     const CallOptions& options = {});

  /// Reads a resource by its URI, as a C++ type of the caller's choosing.
  ///
  /// The contents are read as the server's binding writes what a resource
  /// function returns: a `std::string` is the text of the one item, a
  /// `std::vector<std::byte>` the bytes of its blob, an mcp::ResourceContents
  /// the item itself, a vector of them every item, and any other type the text
  /// read as JSON. Contents that do not fit are Fault::mismatch.
  template <typename T>
  Reply<T> read(const std::string_view uri, const CallOptions& options = {}) {
    Reply<std::vector<ResourceContents>> contents = read_resource(uri, options);
    if (!contents) {
      return std::unexpected(std::move(contents).error());
    }
    return detail::resource_value<T>(std::move(*contents));
  }

  /// Reads a resource by its declaration, which a header shared with the
  /// server provides; the function need not be defined in the client's
  /// program.
  ///
  /// The URI is the declaration's, with each variable of a template filled in
  /// from the argument of the same name: `{name}` percent-encoded, and
  /// `{+name}` as it is, so that the server's matcher reads back the same
  /// values. Arguments that the URI cannot carry, such as an object, an empty
  /// value, or a `{+name}` value that would read back otherwise, fail with
  /// Fault::protocol, unsent. The result is what the function returns, a
  /// Result unwrapped, read as read<T>() reads it.
  /// @tparam Function Reflection of a function that carries mcp::resource.
  /// @tparam Binding Leave defaulted: the function as the calling translation
  /// unit declares it.
  template <std::meta::info Function,
            typename Binding = typename[:scrydantic::binding_of(Function, ^^Context):]>
  Reply<typename[:detail::read_value_of(Function):]>
  read(const scrydantic::arguments_t<Function, Context, Binding>& arguments,
       const CallOptions& options = {}) {
    using Arguments = scrydantic::arguments_t<Function, Context, Binding>;
    static_assert(detail::read_problem<Arguments>(Function).empty(),
                  detail::read_problem<Arguments>(Function));
    static constexpr mcp::resource annotation =
        detail::carried(Function, mcp::resource{.uri = ""});
    scrydantic::Result<scrydantic::Json> encoded = scrydantic::encode(arguments);
    if (!encoded) {
      return std::unexpected(detail::unsendable(encoded.error()));
    }
    Reply<std::string> uri = detail::expand_uri(annotation.uri.view(), *encoded);
    if (!uri) {
      return std::unexpected(std::move(uri).error());
    }
    return read<typename[:detail::read_value_of(Function):]>(*uri, options);
  }

  /// The prompts the server offers, from every page of their list, as
  /// list_resources() reads it.
  Reply<std::vector<Prompt>> list_prompts(const CallOptions& options = {});

  /// Renders a prompt by name.
  /// @param name The prompt's name.
  /// @param arguments The prompt's arguments, each a string, by name.
  /// @return The prompt's messages, or why it could not be rendered: a name or
  /// arguments the server rejects is Fault::rejected, usually with -32602.
  Reply<PromptResult>
  get_prompt(std::string_view name,
             const std::map<std::string, std::string>& arguments = {},
             const CallOptions& options = {});

  /// Renders a prompt by its declaration, which a header shared with the
  /// server provides; the function need not be defined in the client's
  /// program.
  ///
  /// The prompt's name is the declaration's, and its arguments are the
  /// parameters the server does not supply itself, each sent as the text the
  /// server's binding reads back: a string as it is, and a number or a boolean
  /// as JSON spells it. An argument left empty, as a default argument may be,
  /// is not sent. One that is not text, a number or a boolean fails with
  /// Fault::protocol, unsent.
  /// @tparam Function Reflection of a function that carries mcp::prompt.
  /// @tparam Binding Leave defaulted: the function as the calling translation
  /// unit declares it.
  template <std::meta::info Function,
            typename Binding = typename[:scrydantic::binding_of(Function, ^^Context):]>
  Reply<PromptResult>
  prompt(const scrydantic::arguments_t<Function, Context, Binding>& arguments,
         const CallOptions& options = {}) {
    static_assert(detail::carries(Function, ^^mcp::prompt),
                  "prompt<^^f>() renders a function that carries mcp::prompt");
    static constexpr std::string_view name = detail::name_or(
        detail::carried(Function, mcp::prompt{.description = ""}).name.view(),
        Function);
    scrydantic::Result<scrydantic::Json> encoded = scrydantic::encode(arguments);
    if (!encoded) {
      return std::unexpected(detail::unsendable(encoded.error()));
    }
    Reply<std::map<std::string, std::string>> texts = detail::argument_texts(*encoded);
    if (!texts) {
      return std::unexpected(std::move(texts).error());
    }
    return get_prompt(name, *texts, options);
  }

  /// Asks the server to suggest values for an argument of a prompt, as a
  /// person types it.
  /// @param prompt The prompt.
  /// @param argument The argument's name.
  /// @param value What has been typed so far.
  /// @param context The prompt's other arguments that are chosen already, by
  /// name.
  Reply<Completion> complete(const PromptReference& prompt, std::string_view argument,
                             std::string_view value,
                             const std::map<std::string, std::string>& context = {},
                             const CallOptions& options = {});

  /// Asks the server to suggest values for a variable of a resource template,
  /// as a person types it.
  /// @param resource The template.
  /// @param variable The variable's name.
  /// @param value What has been typed so far.
  /// @param context The template's other variables that are chosen already,
  /// by name.
  Reply<Completion> complete(const ResourceTemplateReference& resource,
                             std::string_view variable, std::string_view value,
                             const std::map<std::string, std::string>& context = {},
                             const CallOptions& options = {});

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

// The stdio transport.

/// The longest line a client reads from a server over stdio by default, in
/// bytes: 64 MiB, room for a large image or file in one message.
inline constexpr std::size_t default_max_line = std::size_t{64} << 20U;

/// Opens a client to a server over a pair of streams, one message per line each
/// way: the client's side of serve_stdio(). The streams must outlive the
/// client.
///
/// A thread of the client's reads `in`. Each message goes to `out` as one line,
/// flushed, one at a time, from whichever thread sends it. When `in` ends, or a
/// read or write of the streams fails, every call that waits fails with
/// Fault::transport, and so does every later one. A line longer than `max_line`
/// bytes, not counting its line break, ends the client the same way, and what
/// follows it is read and passed over.
///
/// Closing the client, as its destructor does, stops it from taking what `in`
/// holds and from writing to `out`. A read in progress on a stream the client
/// does not own cannot be interrupted, though, so the destructor returns only
/// once `in` has ended, at its end or at a failure; a connect() that fails to
/// open waits the same way. End the server's output before destroying the
/// client, such as by ending its input first, which serve_stdio() answers by
/// returning.
///
/// Writing to a pipe or a socket whose reader has gone may raise SIGPIPE, which
/// is for the owner of `out` to handle; launch() handles it for its own pipes.
/// @return The open client, or why it could not open.
[[nodiscard]] Reply<Client> connect(std::istream& in, std::ostream& out,
                                    ClientOptions options = {},
                                    std::size_t max_line = default_max_line);

/// What becomes of what a launched server writes to its stderr.
enum class Stderr : std::uint8_t {
  /// It goes where the client's own stderr goes.
  inherit,
  /// It is thrown away.
  discard,
  /// Each line goes to Command::stderr_line.
  lines,
};

/// A variable of a launched server's environment.
struct EnvironmentVariable {
  std::string name;
  std::string value{};
};

/// How to start a server as a process of its own, which a client talks to over
/// its stdin and stdout.
struct Command {
  /// The program. One without a slash is looked up on the client's PATH, as a
  /// shell looks it up; one with a slash is a path, relative to `directory`
  /// when that is set.
  std::string command;
  /// The arguments after the program's name.
  std::vector<std::string> arguments{};
  /// Variables added to the environment the server inherits from the client,
  /// each in place of one of the same name.
  std::vector<EnvironmentVariable> environment{};
  /// The directory the server starts in, or empty for the client's own.
  std::filesystem::path directory{};
  /// What becomes of what the server writes to its stderr.
  Stderr stderr_output{Stderr::inherit};
  /// Under Stderr::lines, receives each line the server writes to its stderr,
  /// without its line break, on a thread of the client's. The server's messages
  /// wait while it runs, so it should return soon, and it must not call the
  /// client. A line longer than `max_line` comes cut to that length. When it
  /// is empty, the lines are read and thrown away.
  std::function<void(std::string_view line)> stderr_line{};
  /// How long the server has to exit once its input is closed, and again after
  /// SIGTERM, before it is killed.
  std::chrono::milliseconds grace{std::chrono::seconds{2}};
  /// The longest line read from the server, as connect() over streams bounds
  /// it.
  std::size_t max_line{default_max_line};
};

/// Starts a server as a child process and opens a client to it over the
/// process's stdin and stdout (POSIX only).
///
/// The server inherits the client's environment, with Command::environment
/// added, and no file descriptor of the client's but its three standard
/// streams. It leads a process group of its own, so that a signal from the
/// client's terminal, such as Ctrl-C, is the client's to pass on.
///
/// When the server exits or closes its output, every call that waits fails
/// with Fault::transport, which says how it exited, and so does every later
/// one; the client does not start the server again. Writing to a server that
/// has gone fails the call in the same way, without raising SIGPIPE, and
/// without changing how the program handles SIGPIPE.
///
/// Destroying the client ends the server as the protocol says: it closes the
/// server's input and waits Command::grace for it to exit, then sends SIGTERM
/// to its process group and waits again, then sends the group SIGKILL. A server
/// that exits at the end of its input, as serve_stdio() does, is sent no signal.
/// The server is always reaped, so it leaves no zombie; a program that reaps
/// every child itself, or ignores SIGCHLD, leaves the client unable to say how
/// a server exited.
///
/// A server that is slow to start, as one started through npx or uvx can be,
/// still opens in its own era: under Era::automatic, `initialize` follows
/// `server/discover` after ClientOptions::probe_timeout, and the first answer
/// to succeed decides. Each request, those of opening too, may take up to
/// ClientOptions::timeout, so a server must start within that.
/// @return The open client, or why it could not open, such as a program that
/// cannot be started, with the reason.
[[nodiscard]] Reply<Client> launch(Command command, ClientOptions options = {});

} // namespace mcp
