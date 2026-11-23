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

#include <cstdint>
#include <expected>
#include <mcp/mcp.hpp>
#include <string>
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

} // namespace weather

int main() {
  mcp::Server server{"weather", "1.0.0"};
  server.instructions("Forecasts for a few cities. Call cities first.");
  server.add<^^weather>();
  return mcp::serve_stdio(server);
}
