/// @file
/// The stdio transport: one JSON-RPC message per line.
#pragma once

#include <cstddef>
#include <iostream>
#include <mcp/server.hpp>

namespace mcp {

/// How serve_stdio() runs the requests it reads.
struct StdioOptions {
  /// How many requests are handled at once. Further requests wait their turn,
  /// in the order they arrived. One serves the requests one at a time, for
  /// tools that cannot run side by side; cancellation still reaches a call
  /// while it runs. Zero counts as one.
  std::size_t concurrency{8};
};

/// Serves one client over a pair of streams until the input ends.
///
/// Each line of `in` is one message, and each response or notification is
/// written to `out` as one line and flushed. Nothing else is written to `out`,
/// so a tool must not print to the same stream.
///
/// One thread reads, and requests are handled on others, so a slow call does
/// not hold up the rest, and responses may be written in a different order from
/// the requests. A `notifications/cancelled` stops the call it names, through
/// Context::stop, whether it is running or still waiting; a cancelled call gets
/// no response. When the input ends, the requests already read are answered
/// before this returns.
/// @param server The server to serve. If its status() has failed, the error is
/// written to its log and nothing is read.
/// @param in Where the client's messages come from.
/// @param out Where the server's messages go.
/// @param options How the requests are run.
/// @return 0 when the input ended, as `main` returns it, or 1 when the server
/// could not be served.
int serve_stdio(const Server& server, std::istream& in = std::cin,
                std::ostream& out = std::cout, StdioOptions options = {});

} // namespace mcp
