// The client's channel over Streamable HTTP, which connect() opens a client
// with, and which the tests drive on its own.
#pragma once

#include <mcp/channel.hpp>
#include <mcp/http_client.hpp>
#include <memory>
#include <string>

namespace mcp {

// A channel to the endpoint at `url` through `http`, which must outlive it.
// Each message is a POST on a thread of the channel's; the URL and the
// headers are not checked here.
[[nodiscard]] std::unique_ptr<Channel> http_channel(std::string url, HttpClient& http,
                                                    HttpConnectOptions options);

} // namespace mcp
