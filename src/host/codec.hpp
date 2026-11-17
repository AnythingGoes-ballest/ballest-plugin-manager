// Bounded helpers for moving plugin data through text-only transports. Pack is Windows XPRESS_HUFF compressed bytes
// (with the original size in a four-byte prefix), encoded as unpadded base64. Game thread only.
#pragma once

#include <string>

namespace codec {

constexpr size_t kMaxInput = 2 * 1024 * 1024;

std::string Pack(const std::string& text);
std::string Unpack(const std::string& packed);
std::string Digest(const std::string& text);       // lowercase SHA-256; empty on an oversized input or system error

}  // namespace codec
