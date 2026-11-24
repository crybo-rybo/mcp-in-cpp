// SHA-256 and HMAC-SHA256 against the published test vectors.
#include "sha256.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

std::string hex(const mcp::crypto::Digest& digest) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string text;
  for (const std::uint8_t byte : digest) {
    text += digits[byte >> 4U];
    text += digits[byte & 0xFU];
  }
  return text;
}

} // namespace

TEST_CASE("SHA-256 gives the digests of FIPS 180-4's examples") {
  using mcp::crypto::sha256;
  CHECK(hex(sha256("")) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hex(sha256("abc")) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(hex(sha256(std::string(1000000, 'a'))) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("HMAC-SHA256 gives the MACs of RFC 4231") {
  using mcp::crypto::hmac_sha256;
  CHECK(hex(hmac_sha256(std::string(20, '\x0b'), "Hi There")) ==
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  CHECK(hex(hmac_sha256("Jefe", "what do ya want for nothing?")) ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  // A key longer than a block is hashed first.
  CHECK(hex(hmac_sha256(std::string(131, '\xaa'),
                        "Test Using Larger Than Block-Size Key - Hash Key First")) ==
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST_CASE("digests compare equal only when every byte is") {
  const mcp::crypto::Digest one = mcp::crypto::sha256("one");
  mcp::crypto::Digest other = one;
  CHECK(mcp::crypto::equal(one, other));
  other[31] ^= 1U;
  CHECK_FALSE(mcp::crypto::equal(one, other));
}
