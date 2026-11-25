# mcp-in-cpp

An MCP server and client library for C++26. A tool, a resource, and a prompt are
each a plain function: static reflection reads a tool's parameters into the input
schema, its return type into the output schema, and its annotations into the
descriptions a model sees. There are no macros, no code generator, and no
schema written by hand.

```cpp
#include <mcp/mcp.hpp>

namespace weather {

struct Forecast {
  [[= mcp::description{"Degrees Celsius"}]] double temperature{};
  bool rain{};
};

[[= mcp::tool{"Get the forecast for a city"}]]
Forecast forecast([[= mcp::description{"City name"}]] std::string city,
                  [[= mcp::range{.min = 1, .max = 7}]] std::int32_t days = 1);

[[= mcp::resource{.uri = "weather://{city}/alerts", .mime_type = "application/json"}]]
std::vector<Alert> alerts(std::string city);

[[= mcp::prompt{"Plan a trip around the weather"}]]
std::string plan_trip(std::string city, std::int32_t days = 3);

}  // namespace weather

int main() {
  mcp::Server server{"weather", "1.0.0"};
  server.add<^^weather>();
  return mcp::serve_stdio(server);
}
```

[Scrydantic](https://github.com/crybo-rybo/scrydantic) supplies the schemas, the
strict decoding with errors a model can act on, and the binding of functions to
JSON arguments. This library supplies the protocol. The annotations under
`mcp::` are Scrydantic's own, and its
[guide](https://github.com/crybo-rybo/scrydantic/blob/main/docs/guide.md) is
the reference for them and for what a tool may take and return.

## The client

`mcp::Client` is the connection to one server, of either protocol era. It calls
a tool by name with JSON, as a model's host does, or through the same
declaration the server binds, as a typed C++ function. A header that declares
the `weather` namespace above is all the client needs: the functions are
defined only in the server.

```cpp
#include <mcp/client.hpp>

int main() {
  // Start a server as a child process and talk to it over stdio.
  mcp::Reply<mcp::Client> client =
      mcp::launch({.command = "weather-server"}, {.name = "zoo", .version = "1.0.0"});
  if (!client) {
    return 1;  // client.error() says why
  }

  // By name: schemas and arguments stay JSON, for a model.
  mcp::Reply<std::vector<mcp::Tool>> tools = client->list_tools();
  mcp::Reply<mcp::ToolResult> result =
      client->call_tool("forecast", mcp::Json{R"({"city":"Oslo","days":3})"});

  // By declaration: the name, the arguments and the result's type are the function's.
  mcp::Reply<weather::Forecast> forecast =
      client->call<^^weather::forecast>({.city = "Oslo", .days = 3});
  mcp::Reply<std::vector<weather::Alert>> alerts =
      client->read<^^weather::alerts>({.city = "Oslo"});
  mcp::Reply<mcp::PromptResult> plan =
      client->prompt<^^weather::plan_trip>({.city = "Oslo"});
}
```

Every call returns a `mcp::Reply`: the value, or a `mcp::ClientError` whose
`fault` says what failed. Nothing throws. Calls block, may be made from several
threads, stop on a `std::stop_token`, and have a time limit.
[`examples/client.cpp`](examples/client.cpp) is a complete client for any stdio
server.

## Status

A complete server of both protocol eras, over stdio and Streamable HTTP, and a
client of both. The server passes every scored scenario of the official
[conformance suite](https://github.com/modelcontextprotocol/conformance) for
2026-07-28 and for 2025-11-25. The client passes the suite's scenarios but
those of authorization, and two of 2025-11-25 that need a session's GET stream.
It is young, and its API can change before 1.0.

The server:

| | |
|---|---|
| Protocol | MCP 2026-07-28, served statelessly, and the `initialize` handshake of 2025-11-25 and 2025-06-18. One server speaks both to whichever client connects. |
| Methods | `server/discover`, `tools/list`, `tools/call`, `resources/list`, `resources/templates/list`, `resources/read`, `prompts/list`, `prompts/get`, `completion/complete` (which suggests nothing yet), `subscriptions/listen` (acknowledged; what a server offers does not change while it runs), and for a handshake session `initialize` and `ping`. A method of a capability the server lacks is not found. |
| Resources | A function marked `mcp::resource{.uri = ...}`. A URI with `{name}` or `{+name}` is a template whose variables are the parameters, read from text as their types. A string is the resource's text, bytes its blob, and any other value its JSON. A resource that is not there is `-32602` under 2026-07-28 and `-32002` before. |
| Subscriptions | A handshake session subscribes to a resource with `resources/subscribe`, and `Server::resource_updated(uri)` sends it `notifications/resources/updated`: over stdio between the responses, and over HTTP on the session's GET event stream. 2026-07-28's `subscriptions/listen` is acknowledged with nothing to notify of yet. |
| Prompts | A function marked `mcp::prompt{"..."}`. Its parameters are the arguments, in order, read from text as their types; a default makes one optional. It returns a string, content blocks, or `mcp::PromptMessage`s. |
| Tool results | Text content, and structured content with the tool's output schema. A returned string is sent as the text itself. A tool that returns `mcp::Content` sends its blocks as they are: text, images, audio, embedded resources, and resource links. |
| Tool failures | Arguments that do not decode and a tool's own refusal are tool errors with the text meant for the model. A tool that throws, or whose result breaks its own schema, is a tool error that says only `the tool failed`; the reason goes to the server's log. An unknown tool or a malformed call is a JSON-RPC error. |
| Progress | A tool takes an `mcp::Progress` parameter in any position. It is not in the input schema, and its reports reach a client that sent a progress token. |
| Logging | A function takes an `mcp::Logger` parameter in any position, outside the input schema, and logs at a level. Under 2026-07-28 a request asks for messages with `_meta["io.modelcontextprotocol/logLevel"]`, and gets none without it; a handshake session sets its level with `logging/setLevel`. |
| Asking the client | A function takes `mcp::Elicitation` (a form from a struct, or a schema given as JSON), `mcp::Sampling`, or `mcp::Roots`, and asks; the same code serves both eras. In a handshake session the question is a request to the client and the call waits. Under 2026-07-28 the call ends the round with `input_required`, and runs again when the client sends the answers, with the earlier rounds' answers in a `requestState` signed with HMAC-SHA256 and bound to the request. A client without the capability is not asked: `-32021`. |
| Cancellation | A tool takes a `std::stop_token` parameter in any position, or reads `mcp::Context::stop`. It stops when the client cancels the call, and a cancelled call gets no response. |
| Concurrency | `mcp::serve_stdio` reads on one thread and runs up to eight requests at once on others (`StdioOptions::concurrency`; one runs them in order). A server's tools must then be safe to call side by side. |
| Transport | stdio: `mcp::serve_stdio`. Streamable HTTP: `mcp::HttpEndpoint`, behind an HTTP server the program already has; `<mcp/httplib.hpp>` mounts it on cpp-httplib. `mcp::Connection::handle` takes one message and returns the response, and may be called from several threads, for a transport of your own. |
| HTTP | 2026-07-28 statelessly, with its routing headers checked (`MCP-Protocol-Version`, `Mcp-Method`, `Mcp-Name`) and a closed stream as cancellation. The handshake revisions in sessions named by `Mcp-Session-Id`, cancelled by notification. A response is JSON, or an event stream once the call sends a notification. `Host` and `Origin` are checked against the local machine's names by default. |

The client:

| | |
|---|---|
| Opening | `mcp::launch` starts a server as a child process (POSIX), `mcp::connect(in, out)` talks over two streams, `mcp::connect(url, http)` over Streamable HTTP, and `mcp::connect(server)` to an `mcp::Server` in the same process, for its tests. `mcp::Client::open` takes any `mcp::Channel`, for a transport of your own. |
| Protocol | Whichever era the server speaks. The client asks with `server/discover` under 2026-07-28 and falls back to the `initialize` handshake as the specification says; a server that is slow to start is sent both, and the first to succeed decides. `ClientOptions::era` pins one. |
| Calls | `list_tools`, `call_tool`, `list_resources`, `list_resource_templates`, `read_resource`, `list_prompts`, `get_prompt`, `complete`, `ping`. Lists follow their cursors to the end. Schemas, structured content and error data are kept as the server wrote them, and a `$ref` is never fetched. |
| Typed calls | `call<T>(name, arguments)` and `read<T>(uri)` with the caller's own types, and `call<^^fn>`, `read<^^fn>`, `prompt<^^fn>` by a declaration shared with the server. A typed read fills the declared URI template from its arguments. A tool's own error is a result in `call_tool` and `Fault::tool` in a typed call. |
| What a server asks | A handler in `ClientOptions` for `elicitation`, `sampling` or `roots` declares its capability and answers in both eras. Under 2026-07-28 the client answers `input_required` inside the call and sends the request again, with the server's `requestState`, for a bounded number of rounds. A field that an accepted form leaves out is sent with its schema's default. |
| Each call | `CallOptions` gives a progress function, a `std::stop_token`, a time limit (60 seconds by default, started again by progress, and standing still while a handler runs), and under 2026-07-28 a log level. `ClientOptions::log` receives the server's log messages. |
| stdio | A launched server inherits only its standard streams. Closing the client closes the server's input, then sends SIGTERM, then SIGKILL, each after a grace period, and reaps it. A server that exits fails the waiting calls with how it exited. |
| HTTP | Through the HTTP client the program has, behind `mcp::HttpClient`; `<mcp/httplib_client.hpp>` implements it over cpp-httplib. 2026-07-28's routing headers, with a tool's `x-mcp-header` arguments mirrored into `Mcp-Param-*`; sessions for the handshake revisions. A 401 is `Fault::unauthorized` with the server's challenge, and `HttpConnectOptions::bearer` supplies a token for each request. |

Not there yet: notifications on 2026-07-28's `subscriptions/listen` and the
client's side of change subscriptions, the handshake session's GET stream in
the client, URL elicitation, authorization (OAuth), and launching a server on
Windows.

## Requirements

- GCC 16.2 or newer. It is the only compiler that implements C++26 reflection.
- CMake 3.30 or newer.

Scrydantic is fetched at configure time, pinned to its 0.4.0 release.

## Build and test

`scripts/ci.sh` runs everything CI runs, so a change can be checked before it
is pushed:

```sh
scripts/ci.sh                # format, then release, asan, and tsan
scripts/ci.sh release        # format and one preset
```

Each step is a script of its own, and CI runs the same scripts:

| Script | What it does |
|---|---|
| `scripts/format.sh [--fix]` | Checks the C++ sources against `.clang-format` with clang-format 22.1.4, or formats them |
| `scripts/build.sh <preset>` | Configures, builds, and runs the tests of `dev`, `release`, `asan`, or `tsan` |
| `scripts/interop.sh <preset>` | Checks against the official Python SDK (mcp 2.3.0) in both eras: its client drives the example server over stdio and HTTP, and this library's client drives a server written with it over stdio |
| `scripts/install-check.sh <preset>` | Installs the package, and builds and runs `tests/install` against it |
| `scripts/conformance.sh <preset>` | Runs the official [conformance suite](https://github.com/modelcontextprotocol/conformance) against the server in `tests/conformance/fixture.cpp` and the client in `tests/conformance/client.cpp`, under 2026-07-28 and 2025-11-25 |

The scripts use `g++-16` when `g++` is not GCC 16, or the compiler `CXX`
names. They install clang-format and the Python SDK into `build/tools` the
first time, with `python3` or the Python `PYTHON` names. The conformance
suite needs Node 22 or newer, and fails on any scenario its baseline in
`tests/conformance/expected-failures.*.yml` does not list, or on a listed one
that passes. Homebrew's GCC has no thread sanitizer, so on macOS
`scripts/ci.sh` leaves tsan out unless it is named.

## Use it in a project

Install it, with Scrydantic beside it, and find the package:

```sh
cmake --workflow --preset release
cmake --install build/release --prefix /opt/mcp
```

```cmake
find_package(mcp-in-cpp 0.2 REQUIRED)  # with /opt/mcp in CMAKE_PREFIX_PATH
target_link_libraries(my_server PRIVATE mcp::mcp)
```

A Scrydantic 0.4 that is already installed is used instead of a fetched one.
Or fetch it with the project:

```cmake
include(FetchContent)
FetchContent_Declare(
  mcp-in-cpp
  GIT_REPOSITORY https://github.com/crybo-rybo/mcp-in-cpp.git
  GIT_TAG v0.2.0
)
FetchContent_MakeAvailable(mcp-in-cpp)
target_link_libraries(my_server PRIVATE mcp::mcp)
```

The `mcp::mcp` target turns on C++26 and `-freflection` for its users.
[`examples/weather.cpp`](examples/weather.cpp) is a complete server.

To serve over Streamable HTTP, route one path of your HTTP server to an
`mcp::HttpEndpoint` through an `mcp::HttpResponse` of your own, or with
cpp-httplib:

```cpp
#include <httplib.h>
#include <mcp/httplib.hpp>

mcp::HttpEndpoint endpoint{server};
httplib::Server http;
mcp::mount(http, "/mcp", endpoint);
http.listen("127.0.0.1", 8000);
```

A client reaches a Streamable HTTP server through an `mcp::HttpClient` of your
own, or through cpp-httplib:

```cpp
#include <httplib.h>
#include <mcp/httplib_client.hpp>

mcp::HttplibClient http;
mcp::Reply<mcp::Client> client =
    mcp::connect("http://127.0.0.1:8000/mcp", http, {.name = "zoo", .version = "1.0.0"});
```

An `https` URL needs cpp-httplib built with TLS. The library does not depend on
cpp-httplib; `<mcp/httplib.hpp>` and `<mcp/httplib_client.hpp>` are for a
program that does.

## License

MIT, as [LICENSE](LICENSE) says.
