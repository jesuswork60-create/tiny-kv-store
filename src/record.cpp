#include "record.h"

#include <cstring>

#include "coding.h"
#include "crc32.h"

namespace tinykv {

const char* record_type_name(RecordType type) {
  switch (type) {
    case RecordType::Put: return "put";
    case RecordType::Delete: return "del";
    case RecordType::BatchPut: return "batch-put";
    case RecordType::BatchDelete: return "batch-del";
    case RecordType::BatchCommit: return "batch-commit";
  }
  return "unknown";
}

void encode_record(std::string& out, std::uint64_t seq, RecordType type, std::string_view key,
                   std::string_view value) {
  const std::size_t start = out.size();
  out.reserve(start + record_size(key.size(), value.size()));
  put_u32(out, 0);  // Patched below, once the covered bytes exist.
  put_u64(out, seq);
  out.push_back(static_cast<char>(type));
  put_u32(out, static_cast<std::uint32_t>(key.size()));
  put_u32(out, static_cast<std::uint32_t>(value.size()));
  out.append(key);
  out.append(value);

  const std::uint32_t crc = crc32(out.data() + start + 4, out.size() - start - 4);
  std::string encoded;
  put_u32(encoded, crc);
  std::memcpy(&out[start], encoded.data(), encoded.size());
}

bool parse_record_header(const char* data, RecordHeader* out) {
  out->crc = get_u32(data);
  out->seq = get_u64(data + 4);
  const auto raw_type = static_cast<unsigned char>(data[12]);
  out->key_len = get_u32(data + 13);
  out->value_len = get_u32(data + 17);
  if (raw_type < 1 || raw_type > 5) return false;
  out->type = static_cast<RecordType>(raw_type);
  if (out->key_len > kMaxKeyBytes || out->value_len > kMaxValueBytes) return false;
  return out->type == RecordType::BatchCommit ? out->key_len == 0 : out->key_len != 0;
}

bool decode_record(const char* data, const RecordHeader& header, Record* out) {
  const std::size_t covered = static_cast<std::size_t>(header.total_size()) - 4;
  if (crc32(data + 4, covered) != header.crc) return false;
  out->seq = header.seq;
  out->type = header.type;
  out->key = std::string_view(data + kRecordHeaderSize, header.key_len);
  out->value = std::string_view(data + kRecordHeaderSize + header.key_len, header.value_len);
  return true;
}

}  // namespace tinykv
