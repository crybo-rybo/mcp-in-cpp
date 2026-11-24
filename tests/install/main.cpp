// Serves one tool to one request, and checks the answer.
#include <iostream>
#include <mcp/mcp.hpp>
#include <optional>
#include <string>

namespace installed {

[[= mcp::tool{"Add two integers"}]] int add(int a, int b) { return a + b; }

} // namespace installed

int main() {
  mcp::Server server{"consumer", "1.0.0"};
  server.add<^^installed>();
  mcp::Connection connection{server};
  const std::optional<std::string> response = connection.handle(
      R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"add",)"
      R"("arguments":{"a":2,"b":3},"_meta":{"io.modelcontextprotocol/protocolVersion":)"
      R"("2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}})");
  if (!response || response->find(R"("structuredContent":5)") == std::string::npos) {
    std::cerr << "unexpected response: " << response.value_or("none") << '\n';
    return 1;
  }
  std::cout << "the installed package serves a tool\n";
  return 0;
}
