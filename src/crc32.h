#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tinykv {

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
// Feed the previous result back in as `seed` to checksum data in pieces.
std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t seed = 0);

inline std::uint32_t crc32(std::string_view data, std::uint32_t seed = 0) {
  return crc32(data.data(), data.size(), seed);
}

}  // namespace tinykv
