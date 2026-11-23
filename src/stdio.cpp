#include <iostream>
#include <mcp/server.hpp>
#include <mcp/stdio.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace mcp {

int serve_stdio(const Server& server, std::istream& in, std::ostream& out) {
  if (const Status status = server.status(); !status) {
    server.log_line(status.error().message);
    return 1;
  }
  const Notify notify = [&out](const std::string_view message) {
    out << message << '\n' << std::flush;
  };
  Connection connection{server};
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    if (const std::optional<std::string> response = connection.handle(line, notify)) {
      out << *response << '\n' << std::flush;
    }
  }
  return 0;
}

} // namespace mcp
