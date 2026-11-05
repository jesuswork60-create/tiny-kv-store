#pragma once

#include <cstdint>
#include <cstring>
#include <string>

// Little-endian integer encoding used by every on-disk structure.
namespace tinykv {

inline void put_u32(std::string& out, std::uint32_t v) {
  char b[4];
  for (int i = 0; i < 4; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  out.append(b, sizeof b);
}

inline void put_u64(std::string& out, std::uint64_t v) {
  char b[8];
  for (int i = 0; i < 8; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  out.append(b, sizeof b);
}

inline std::uint32_t get_u32(const char* p) {
  std::uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(p[i]);
  return v;
}

inline std::uint64_t get_u64(const char* p) {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(p[i]);
  return v;
}

}  // namespace tinykv
