#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace doaxbv {

std::string sha1Hex(const std::vector<std::uint8_t>& bytes);
std::string sha1Hex(const std::uint8_t* bytes, std::size_t size);
std::string sha256Hex(const std::vector<std::uint8_t>& bytes);
/* The digest itself, for a caller that needs bytes rather than text (the Live
   identity's XUID and proof). */
void sha256(const std::uint8_t* bytes, std::size_t size, std::uint8_t digest[32]);

} // namespace doaxbv

/* The same digest with C linkage, which live_identity.c asks for. A C caller
   cannot take a C++ exception, so a failure zeroes the digest and returns
   false rather than escaping the boundary. */
extern "C" bool doaxbv_sha256_digest(const std::uint8_t* bytes, std::size_t size, std::uint8_t digest[32]);
