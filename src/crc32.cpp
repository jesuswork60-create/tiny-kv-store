#include "crc32.h"

#include <array>

namespace tinykv {
namespace {

constexpr std::array<std::uint32_t, 256> make_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int bit = 0; bit < 8; ++bit) {
      c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    table[i] = c;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kTable = make_table();

}  // namespace

std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t seed) {
  const auto* p = static_cast<const unsigned char*>(data);
  std::uint32_t c = ~seed;
  for (std::size_t i = 0; i < size; ++i) {
    c = kTable[(c ^ p[i]) & 0xff] ^ (c >> 8);
  }
  return ~c;
}

}  // namespace tinykv
