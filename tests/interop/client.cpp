// Drives a server written with the official Python MCP SDK through this
// library's client, which launches it over stdio:
//
//   build/release/tests/mcp_interop_client PYTHON tests/interop/python_server.py
//
// It opens the server four times: as the client opens by default, pinned to
// 2026-07-28, with the `initialize` handshake, and by default with a probe
// timeout so short that the handshake goes out too, as it does to a server
// that is slow to start. Each time it checks what the server said of itself,
// lists the tools, calls them by name and with types, and checks a tool's
// error, progress, and cancellation. It exits non-zero on the first
// difference.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mcp/client.hpp>
#include <mutex>
#include <optional>
#include <semaphore>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct Forecast {
  std::string summary;
  double high{};
};

struct City {
  std::string city;
  std::int32_t days{};
};

struct Terms {
  std::int32_t a{};
  std::int32_t b{};
};

// The SDK sends a result that is not an object as the member `result` of one.
struct Sum {
  std::int32_t result{};
};

// What the server wrote to its stderr, shown when a check fails.
std::mutex logged_mutex;
std::vector<std::string> logged;

template <typename T> std::string shown(const T& value) {
  std::ostringstream out;
  if constexpr (std::is_same_v<T, bool>) {
    out << (value ? "true" : "false");
  } else if constexpr (requires { out << value; }) {
    out << value;
  } else {
    out << "a value";
  }
  return out.str();
}

std::string shown(const std::vector<std::string>& values) {
  std::string text = "[";
  for (const std::string& value : values) {
    text += (text.size() > 1 ? ", " : "") + value;
  }
  return text + "]";
}

template <typename Actual, typename Expected>
void check(const std::string& tag, const std::string_view label, const Actual& actual,
           const Expected& expected) {
  if (actual == expected) {
    std::cout << "ok   " << tag << ' ' << label << '\n';
    return;
  }
  std::cout << "FAIL " << tag << ' ' << label << ": got " << shown(actual)
            << ", expected " << shown(expected) << '\n';
  const std::scoped_lock lock{logged_mutex};
  for (const std::string& line : logged) {
    std::cout << "     server: " << line << '\n';
  }
  std::exit(1);
}

std::string text_of(const mcp::ToolResult& result) {
  if (result.content.size() != 1) {
    return "(" + std::to_string(result.content.size()) + " blocks)";
  }
  const auto* const text = std::get_if<mcp::TextContent>(&result.content.front());
  return text != nullptr ? text->text : "(not text)";
}

// The error of a reply that should have failed, or none.
template <typename T> std::optional<mcp::Fault> fault_of(const mcp::Reply<T>& reply) {
  return reply ? std::nullopt : std::optional{reply.error().fault};
}

struct Case {
  std::string tag;
  mcp::ClientOptions options;
  mcp::Era era;
  std::string_view version;
};

void exercise(const mcp::Command& command, const Case& each) {
  const std::string& tag = each.tag;
  mcp::Reply<mcp::Client> opened = mcp::launch(command, each.options);
  check(tag, "opens",
        opened.has_value() ? std::string{"opened"} : opened.error().message,
        std::string{"opened"});
  mcp::Client client = std::move(*opened);

  const mcp::ServerInfo& info = client.server();
  check(tag, "era",
        std::string_view{info.era == mcp::Era::modern ? "modern" : "legacy"},
        std::string_view{each.era == mcp::Era::modern ? "modern" : "legacy"});
  check(tag, "protocol version", info.protocol_version, std::string{each.version});
  check(tag, "server name", info.name, std::string{"python-zoo"});
  check(tag, "server version", info.version, std::string{"2.3.0"});
  check(tag, "instructions", info.instructions.value_or(""),
        std::string{"A few tools to call from another SDK."});
  check(tag, "tools capability", info.has("tools"), true);
  check(tag, "ping", client.ping().has_value(), true);

  const mcp::Reply<std::vector<mcp::Tool>> tools = client.list_tools();
  check(tag, "tools are listed", tools.has_value(), true);
  std::vector<std::string> names;
  for (const mcp::Tool& tool : *tools) {
    names.push_back(tool.name);
  }
  check(tag, "tool names", names,
        std::vector<std::string>{"forecast", "add", "refuse", "count",
                                 "wait_for_cancel", "cancellations"});
  const mcp::Tool& forecast = tools->front();
  check(tag, "forecast description", forecast.description,
        std::string{"Get the forecast for a city"});
  check(tag, "forecast input schema names the city",
        forecast.input_schema.text.find(R"("city")") != std::string::npos, true);
  check(tag, "forecast has an output schema", forecast.output_schema.has_value(), true);

  const mcp::Reply<mcp::ToolResult> called =
      client.call_tool("forecast", mcp::Json{R"({"city":"Oslo","days":2})"});
  check(tag, "a call by name succeeds", called.has_value(), true);
  check(tag, "a call by name is not an error", called->is_error, false);
  check(tag, "a call by name has structured content",
        called->structured_content.has_value(), true);

  const mcp::Reply<Forecast> typed =
      client.call<Forecast>("forecast", City{.city = "Lima", .days = 3});
  check(tag, "a typed call succeeds", typed.has_value(), true);
  check(tag, "a typed call's summary", typed->summary, std::string{"Lima for 3 days"});
  check(tag, "a typed call's high", typed->high, 21.5);
  check(tag, "a wrapped result", client.call<Sum>("add", Terms{.a = 2, .b = 3})->result,
        5);

  const mcp::Reply<mcp::ToolResult> refused = client.call_tool("refuse");
  check(tag, "a tool's error is a result", refused.has_value(), true);
  check(tag, "a tool's error is flagged", refused->is_error, true);
  check(tag, "a tool's error text", text_of(*refused),
        std::string{"Error executing tool refuse: not today"});
  const mcp::Reply<std::string> refused_typed =
      client.call<std::string>("refuse", mcp::Json{"{}"});
  check(tag, "a typed call of a failing tool", fault_of(refused_typed),
        std::optional{mcp::Fault::tool});
  const mcp::Reply<mcp::ToolResult> unknown = client.call_tool("fly");
  // The SDK answers a tool it does not have as the tool's own error.
  check(tag, "an unknown tool is a tool's error",
        unknown.has_value() && unknown->is_error, true);
  check(tag, "an unknown tool's text", text_of(*unknown),
        std::string{"Unknown tool: fly"});

  std::vector<mcp::ProgressReport> reports;
  const mcp::Reply<mcp::ToolResult> counted =
      client.call_tool("count", mcp::Json{R"({"steps":3})"},
                       {.progress = [&reports](const mcp::ProgressReport& report) {
                         reports.push_back(report);
                       }});
  check(tag, "a call with progress succeeds", counted.has_value(), true);
  check(tag, "progress reports", reports.size(), std::size_t{3});
  check(tag, "the last report", reports.back().progress, 3.0);
  check(tag, "the last report's total", reports.back().total, std::optional{3.0});
  check(tag, "the last report's message", reports.back().message,
        std::string{"step 3"});

  // The call is cancelled once the server has said it runs.
  std::binary_semaphore running{0};
  std::stop_source stop;
  auto waiting = std::async(std::launch::async, [&] {
    return client.call_tool(
        "wait_for_cancel", mcp::Json{"{}"},
        {.progress = [&running](const mcp::ProgressReport&) { running.release(); },
         .stop = stop.get_token(),
         .timeout = {}});
  });
  check(tag, "the call to cancel runs", running.try_acquire_for(10s), true);
  stop.request_stop();
  check(tag, "a cancelled call returns",
        waiting.wait_for(10s) == std::future_status::ready, true);
  check(tag, "a cancelled call fails", fault_of(waiting.get()),
        std::optional{mcp::Fault::cancelled});
  check(tag, "the server saw the cancellation",
        client.call<Sum>("cancellations", mcp::Json{R"({"at_least":1})"})->result, 1);

  // A server that exits once its input ends is not signalled.
  const auto start = std::chrono::steady_clock::now();
  {
    const mcp::Client closing = std::move(client);
  }
  check(tag, "the server exits when its input ends",
        std::chrono::steady_clock::now() - start < command.grace, true);
}

} // namespace

int main(const int argc, const char* const argv[]) {
  if (argc != 3) {
    std::cerr << "usage: mcp_interop_client PYTHON SERVER_SCRIPT\n";
    return 2;
  }
  const mcp::Command command{.command = argv[1],
                             .arguments = {argv[2]},
                             .stderr_output = mcp::Stderr::lines,
                             .stderr_line =
                                 [](const std::string_view line) {
                                   const std::scoped_lock lock{logged_mutex};
                                   logged.emplace_back(line);
                                 },
                             .grace = 10s};
  const auto options = [](const mcp::Era era) {
    return mcp::ClientOptions{
        .name = "mcp-in-cpp interop", .version = "0.1.0", .era = era};
  };
  mcp::ClientOptions hurried = options(mcp::Era::automatic);
  hurried.probe_timeout = 1ms;
  for (const Case& each :
       {Case{"[automatic]", options(mcp::Era::automatic), mcp::Era::modern,
             "2026-07-28"},
        Case{"[modern]", options(mcp::Era::modern), mcp::Era::modern, "2026-07-28"},
        Case{"[legacy]", options(mcp::Era::legacy), mcp::Era::legacy, "2025-11-25"},
        Case{"[automatic, slow to start]", hurried, mcp::Era::modern, "2026-07-28"}}) {
    {
      const std::scoped_lock lock{logged_mutex};
      logged.clear();
    }
    exercise(command, each);
  }
  std::cout << "all interop checks of the client passed\n";
  return 0;
}
