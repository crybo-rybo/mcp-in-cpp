# mcp-in-cpp

An MCP server library for C++26. A tool is a plain function: static reflection
reads its parameters into the input schema, its return type into the output
schema, and its annotations into the descriptions a model sees. There are no
macros, no code generator, and no schema written by hand.

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

## Status

This is the first slice: a tool server over stdio. It is early, and the API can
change.

| | |
|---|---|
| Protocol | MCP 2026-07-28, served statelessly, and the `initialize` handshake of 2025-11-25 and 2025-06-18. One server speaks both to whichever client connects. |
| Methods | `server/discover`, `tools/list`, `tools/call`, `subscriptions/listen` (acknowledged; a server's tools do not change while it runs), and for a handshake session `initialize` and `ping`. |
| Tool results | Text content, and structured content with the tool's output schema. A returned string is sent as the text itself. |
| Tool failures | Arguments that do not decode and a tool's own refusal are tool errors with the text meant for the model. A tool that throws, or whose result breaks its own schema, is a tool error that says only `the tool failed`; the reason goes to the server's log. An unknown tool or a malformed call is a JSON-RPC error. |
| Progress | A tool takes an `mcp::Progress` parameter in any position. It is not in the input schema, and its reports reach a client that sent a progress token. |
| Cancellation | A tool takes a `std::stop_token` parameter in any position, or reads `mcp::Context::stop`. It stops when the client cancels the call, and a cancelled call gets no response. |
| Concurrency | `mcp::serve_stdio` reads on one thread and runs up to eight requests at once on others (`StdioOptions::concurrency`; one runs them in order). A server's tools must then be safe to call side by side. |
| Transport | stdio: `mcp::serve_stdio`. `mcp::Connection::handle` takes one message and returns the response, and may be called from several threads, for a transport of your own. |

Not there yet: Streamable HTTP, resources, prompts, elicitation, authorization,
and a client.

## Requirements

- GCC 16.2 or newer. It is the only compiler that implements C++26 reflection.
- CMake 3.30 or newer.

Scrydantic is fetched at configure time, pinned to its 0.4.0 release.

## Build and test

```sh
cmake --workflow --preset dev      # configure, build, and run the tests
cmake --workflow --preset asan     # the same under the address and UB sanitizers
cmake --workflow --preset tsan     # the same under the thread sanitizer (Linux)
```

Set `CXX` on the first configure if `g++` is not GCC 16.2.

`tests/interop/python_client.py` drives the example server through the official
Python client, under the modern revision and under the handshake:

```sh
pip install mcp
python tests/interop/python_client.py build/dev/examples/mcp_weather_example
```

## Use it in a project

```cmake
include(FetchContent)
FetchContent_Declare(
  mcp-in-cpp
  GIT_REPOSITORY https://github.com/crybo-rybo/mcp-in-cpp.git
  GIT_TAG <commit>
)
FetchContent_MakeAvailable(mcp-in-cpp)
target_link_libraries(my_server PRIVATE mcp::mcp)
```

The `mcp::mcp` target turns on C++26 and `-freflection` for its users.
[`examples/weather.cpp`](examples/weather.cpp) is a complete server.
