// A weather server. Every tool is a plain function: its parameters are the
// tool's input schema, its return type is the output schema, and the
// annotations are the descriptions a model reads.
//
// Run it under any MCP client that starts a server as a subprocess, or by hand:
//
//   echo '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{
//     "io.modelcontextprotocol/protocolVersion":"2026-07-28",
//     "io.modelcontextprotocol/clientCapabilities":{}}}}' | tr -d '\n' |
//     ./mcp_weather_example
//
// Or serve it over Streamable HTTP at http://127.0.0.1:8000/mcp:
//
//   ./mcp_weather_example --http 8000

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <charconv>
#include <httplib.h>
#include <iostream>
#include <mcp/httplib.hpp>
#include <mcp/mcp.hpp>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace weather {

enum class Sky : std::uint8_t { clear, cloudy, rain };

struct Forecast {
  [[= mcp::description{"Degrees Celsius"}]] double temperature{};
  Sky sky{Sky::clear};
};

[[= mcp::tool{"Get the forecast for a city"}]] mcp::Result<Forecast>
    forecast([[= mcp::description{"City name"}]] std::string city,
             [[= mcp::range{.min = 1, .max = 7}]] std::int32_t days = 1) {
  if (city.empty()) {
    return std::unexpected(mcp::tool_error("city must not be empty"));
  }
  return Forecast{.temperature = 21.5 - days, .sky = days > 3 ? Sky::rain : Sky::clear};
}

[[= mcp::tool{"List the cities that have a forecast"}]] std::vector<std::string>
    cities() {
  return {"Lima", "Oslo"};
}

[[= mcp::tool{"Download the archive for a city, reporting progress"}]] std::string
    download_archive(std::string city, mcp::Progress progress) {
  for (int step = 1; step <= 3; ++step) {
    progress.report(step, 3.0, "downloading");
  }
  return "archive of " + city + " is ready";
}

struct Alert {
  std::string kind;
  [[= mcp::description{"Hours from now"}]] std::int32_t in_hours{};
};

// A resource template: `{city}` is the parameter of the same name.
[[= mcp::resource{.uri = "weather://{city}/alerts",
                  .description = "Storm alerts for a city",
                  .mime_type = "application/json"}]]
mcp::Result<std::vector<Alert>> alerts(std::string city) {
  if (city != "Lima" && city != "Oslo") {
    return std::unexpected(mcp::tool_error("no alerts are kept for " + city));
  }
  return std::vector<Alert>{{.kind = city == "Oslo" ? "snow" : "fog", .in_hours = 6}};
}

[[= mcp::resource{.uri = "weather://about", .description = "Where the forecasts come from"}]]
std::string about() {
  return "Forecasts are made up for the example.";
}

[[= mcp::prompt{"Plan a trip around the weather"}]] std::string
    plan_trip([[= mcp::description{"Where to go"}]] std::string city,
              [[= mcp::range{.min = 1, .max = 7}]] std::int32_t days = 3) {
  return "Plan " + std::to_string(days) + " days in " + city +
         ". Read the forecast first, and its alerts.";
}

[[= mcp::tool{"Watch a city for storm warnings for a number of seconds"}]] std::string
    watch(std::string city, [[= mcp::range{.min = 0, .max = 60}]] std::int32_t seconds,
          std::stop_token stop) {
  // A call that the client cancels ends at once, and its result is not sent.
  std::mutex mutex;
  std::condition_variable_any changed;
  std::unique_lock lock{mutex};
  changed.wait_for(lock, stop, std::chrono::seconds{seconds}, [] { return false; });
  return "no storm warnings for " + city;
}

} // namespace weather

int main(const int argc, const char* const argv[]) {
  mcp::Server server{"weather", "1.0.0"};
  server.instructions("Forecasts for a few cities. Call cities first.");
  server.add<^^weather>();
  if (argc < 3 || std::string_view{argv[1]} != "--http") {
    return mcp::serve_stdio(server);
  }

  const std::string_view text{argv[2]};
  int port = 0;
  if (std::from_chars(text.data(), text.data() + text.size(), port).ec != std::errc{}) {
    std::cerr << "usage: mcp_weather_example [--http PORT]\n";
    return 2;
  }
  if (const mcp::Status status = server.status(); !status) {
    std::cerr << status.error().message << '\n';
    return 1;
  }
  mcp::HttpEndpoint endpoint{server};
  httplib::Server http;
  mcp::mount(http, "/mcp", endpoint);
  // Only this machine can reach the server, as the protocol advises.
  return http.listen("127.0.0.1", port) ? 0 : 1;
}
