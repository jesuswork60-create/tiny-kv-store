#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tinykv {

// Record layout (little-endian):
//   crc32 u32 | sequence u64 | type u8 | key_len u32 | value_len u32 | key | value
// The checksum covers every byte after the checksum field.
enum class RecordType : std::uint8_t {
  Put = 1,
  Delete = 2,
  BatchPut = 3,
  BatchDelete = 4,
  BatchCommit = 5,  // Empty key; the value is the op count as u32; seq is the last op's.
};

constexpr std::size_t kRecordHeaderSize = 4 + 8 + 1 + 4 + 4;
constexpr std::uint32_t kMaxKeyBytes = 64 * 1024;
constexpr std::uint32_t kMaxValueBytes = 1024u * 1024 * 1024;

struct RecordHeader {
  std::uint32_t crc = 0;
  std::uint64_t seq = 0;
  RecordType type = RecordType::Put;
  std::uint32_t key_len = 0;
  std::uint32_t value_len = 0;

  std::uint64_t total_size() const {
    return std::uint64_t{kRecordHeaderSize} + key_len + value_len;
  }
};

// A decoded record whose key and value point into the caller's buffer.
struct Record {
  std::uint64_t seq = 0;
  RecordType type = RecordType::Put;
  std::string_view key;
  std::string_view value;
};

inline std::uint64_t record_size(std::size_t key_len, std::uint64_t value_len) {
  return kRecordHeaderSize + key_len + value_len;
}

const char* record_type_name(RecordType type);

// Appends the encoded record to `out`.
void encode_record(std::string& out, std::uint64_t seq, RecordType type, std::string_view key,
                   std::string_view value);

// Parses the fixed header at `data` (kRecordHeaderSize bytes). Returns false
// when the fields are implausible: unknown type, oversized or empty key.
bool parse_record_header(const char* data, RecordHeader* out);

// Checks the checksum of the full record at `data` and fills `out`.
bool decode_record(const char* data, const RecordHeader& header, Record* out);

}  // namespace tinykv
