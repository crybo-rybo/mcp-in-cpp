// A client. It starts a server, says what the server is, lists its tools, and
// calls one of them with arguments in JSON:
//
//   ./mcp_client_example forecast '{"city":"Oslo","days":2}' ./mcp_weather_example
//
// Any server that speaks MCP over stdio will do, in either era of the
// protocol, such as one that npx or uvx starts:
//
//   ./mcp_client_example add '{"a":2,"b":3}' uvx some-mcp-server

#include <iostream>
#include <mcp/client.hpp>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

int main(const int argc, const char* const argv[]) {
  if (argc < 4) {
    std::cerr << "usage: mcp_client_example TOOL ARGUMENTS COMMAND [ARGUMENT...]\n";
    return 2;
  }
  const std::string_view tool_name{argv[1]};
  const mcp::Json arguments{.text = argv[2]};
  mcp::Command command{.command = argv[3], .arguments = {argv + 4, argv + argc}};

  // The server's own stderr shows here, and it is stopped when the client is
  // destroyed.
  mcp::Reply<mcp::Client> client =
      mcp::launch(std::move(command), {.name = "example", .version = "1.0.0"});
  if (!client) {
    std::cerr << "cannot open the server: " << client.error().message << '\n';
    return 1;
  }
  const mcp::ServerInfo& server = client->server();
  std::cout << server.name << ' ' << server.version << ", protocol "
            << server.protocol_version << '\n';

  const mcp::Reply<std::vector<mcp::Tool>> tools = client->list_tools();
  if (!tools) {
    std::cerr << "cannot list the tools: " << tools.error().message << '\n';
    return 1;
  }
  for (const mcp::Tool& tool : *tools) {
    std::cout << "  " << tool.name << ": " << tool.description << '\n';
  }

  const mcp::Reply<mcp::ToolResult> result = client->call_tool(tool_name, arguments);
  if (!result) {
    std::cerr << tool_name << " failed: " << result.error().message << '\n';
    return 1;
  }
  // A tool's own failure is a result, which says what went wrong.
  std::ostream& shown = result->is_error ? std::cerr : std::cout;
  for (const mcp::ContentBlock& block : result->content) {
    if (const auto* const text = std::get_if<mcp::TextContent>(&block)) {
      shown << text->text << '\n';
    }
  }
  return result->is_error ? 1 : 0;
}
