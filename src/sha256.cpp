#include "sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mcp::crypto {

namespace {

constexpr std::array<std::uint32_t, 64> rounds{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
};

[[nodiscard]] constexpr std::uint32_t rotate(const std::uint32_t value,
                                             const unsigned by) {
  return (value >> by) | (value << (32U - by));
}

void compress(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) {
  std::array<std::uint32_t, 64> words{};
  for (std::size_t i = 0; i < 16; ++i) {
    words[i] = (std::uint32_t{block[i * 4]} << 24U) |
               (std::uint32_t{block[i * 4 + 1]} << 16U) |
               (std::uint32_t{block[i * 4 + 2]} << 8U) |
               std::uint32_t{block[i * 4 + 3]};
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t low =
        rotate(words[i - 15], 7) ^ rotate(words[i - 15], 18) ^ (words[i - 15] >> 3U);
    const std::uint32_t high =
        rotate(words[i - 2], 17) ^ rotate(words[i - 2], 19) ^ (words[i - 2] >> 10U);
    words[i] = words[i - 16] + low + words[i - 7] + high;
  }
  std::array<std::uint32_t, 8> v = state;
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t sum1 = rotate(v[4], 6) ^ rotate(v[4], 11) ^ rotate(v[4], 25);
    const std::uint32_t choose = (v[4] & v[5]) ^ (~v[4] & v[6]);
    const std::uint32_t first = v[7] + sum1 + choose + rounds[i] + words[i];
    const std::uint32_t sum0 = rotate(v[0], 2) ^ rotate(v[0], 13) ^ rotate(v[0], 22);
    const std::uint32_t majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
    const std::uint32_t second = sum0 + majority;
    v[7] = v[6];
    v[6] = v[5];
    v[5] = v[4];
    v[4] = v[3] + first;
    v[3] = v[2];
    v[2] = v[1];
    v[1] = v[0];
    v[0] = first + second;
  }
  for (std::size_t i = 0; i < 8; ++i) {
    state[i] += v[i];
  }
}

} // namespace

Digest sha256(const std::string_view data) {
  std::array<std::uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::string padded{data};
  padded += static_cast<char>(0x80);
  while (padded.size() % 64 != 56) {
    padded += '\0';
  }
  const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8U;
  for (int shift = 56; shift >= 0; shift -= 8) {
    padded += static_cast<char>((bits >> static_cast<unsigned>(shift)) & 0xFFU);
  }
  for (std::size_t at = 0; at < padded.size(); at += 64) {
    compress(state, reinterpret_cast<const std::uint8_t*>(padded.data() + at));
  }
  Digest digest{};
  for (std::size_t i = 0; i < 8; ++i) {
    digest[i * 4] = static_cast<std::uint8_t>(state[i] >> 24U);
    digest[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16U);
    digest[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8U);
    digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
  }
  return digest;
}

Digest hmac_sha256(std::string_view key, const std::string_view data) {
  std::string block;
  if (key.size() > 64) {
    const Digest hashed = sha256(key);
    block.assign(reinterpret_cast<const char*>(hashed.data()), hashed.size());
  } else {
    block.assign(key);
  }
  block.resize(64, '\0');
  std::string inner(64, '\0');
  std::string outer(64, '\0');
  for (std::size_t i = 0; i < 64; ++i) {
    inner[i] = static_cast<char>(block[i] ^ 0x36);
    outer[i] = static_cast<char>(block[i] ^ 0x5c);
  }
  inner += data;
  const Digest first = sha256(inner);
  outer.append(reinterpret_cast<const char*>(first.data()), first.size());
  return sha256(outer);
}

bool equal(const Digest& a, const Digest& b) noexcept {
  std::uint8_t difference = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    difference = static_cast<std::uint8_t>(difference | (a[i] ^ b[i]));
  }
  return difference == 0;
}

} // namespace mcp::crypto
