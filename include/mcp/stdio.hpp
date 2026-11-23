/// @file
/// The stdio transport: one JSON-RPC message per line.
#pragma once

#include <iostream>
#include <mcp/server.hpp>

namespace mcp {

/// Serves one client over a pair of streams until the input ends.
///
/// Each line of `in` is one message, and each response or notification is
/// written to `out` as one line and flushed. Nothing else is written to `out`,
/// so a tool must not print to the same stream. Messages are handled one at a
/// time, in the order they arrive.
/// @param server The server to serve. If its status() has failed, the error is
/// written to its log and nothing is read.
/// @param in Where the client's messages come from.
/// @param out Where the server's messages go.
/// @return 0 when the input ended, as `main` returns it, or 1 when the server
/// could not be served.
int serve_stdio(const Server& server, std::istream& in = std::cin,
                std::ostream& out = std::cout);

} // namespace mcp
