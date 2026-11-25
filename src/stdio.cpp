#include "dispatcher.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <mcp/server.hpp>
#include <mcp/stdio.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

namespace {

// Writes whole lines to the output, one thread at a time.
class Lines {
public:
  explicit Lines(std::ostream& out) noexcept : out_{&out} {}

  void write(const std::string_view line) {
    const std::scoped_lock lock{mutex_};
    *out_ << line << '\n' << std::flush;
  }

private:
  std::mutex mutex_;
  std::ostream* out_;
};

} // namespace

int serve_stdio(const Server& server, std::istream& in, std::ostream& out,
                const StdioOptions options) {
  if (const Status status = server.status(); !status) {
    server.log_line(status.error().message);
    return 1;
  }
  Connection connection{server};
  Lines lines{out};
  // The updates of resources the client subscribed to go out between the
  // responses.
  connection.push_to(
      [&lines](const std::string_view message) { lines.write(message); });
  detail::Dispatcher dispatcher{
      connection, [&lines](const std::string_view message) { lines.write(message); },
      std::max<std::size_t>(options.concurrency, 1)};
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      dispatcher.submit(std::move(line));
      line.clear();
    }
  }
  // A client whose input ended cannot answer: a call that waits for it gives
  // up, and the calls already read are answered.
  connection.client_gone();
  dispatcher.finish();
  connection.push_to({});
  return 0;
}

} // namespace mcp
