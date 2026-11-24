// SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104), for the integrity of the
// state a server hands a client to send back.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace mcp::crypto {

using Digest = std::array<std::uint8_t, 32>;

[[nodiscard]] Digest sha256(std::string_view data);

[[nodiscard]] Digest hmac_sha256(std::string_view key, std::string_view data);

// Whether two digests are equal, in time that does not depend on where they
// differ.
[[nodiscard]] bool equal(const Digest& a, const Digest& b) noexcept;

} // namespace mcp::crypto
