#include "hint.h"

#include <algorithm>
#include <cstring>

#include "coding.h"
#include "crc32.h"
#include "tinykv/error.h"

namespace tinykv {
namespace {

constexpr char kHintMagic[] = "TKVHINT1";
constexpr std::size_t kHintMagicSize = 8;
constexpr std::size_t kHintHeaderSize = kHintMagicSize + 8 + 8 + 8 + 4;
constexpr std::size_t kEntryFixedSize = 8 + 1 + 8 + 4 + 4;

}  // namespace

void HintBuilder::add(const SegmentOp& op) {
  put_u64(body_, op.seq);
  body_.push_back(op.is_put ? 1 : 0);
  put_u64(body_, op.offset);
  put_u32(body_, op.value_len);
  put_u32(body_, static_cast<std::uint32_t>(op.key.size()));
  body_.append(op.key);
  ++count_;
  max_seq_ = std::max(max_seq_, op.seq);
}

void HintBuilder::clear() {
  body_.clear();
  count_ = 0;
  max_seq_ = 0;
}

std::string HintBuilder::finish(std::uint64_t segment_id, std::uint64_t segment_size) const {
  std::string out(kHintMagic, kHintMagicSize);
  put_u64(out, segment_id);
  put_u64(out, segment_size);
  put_u64(out, max_seq_);
  put_u32(out, count_);
  out.append(body_);
  put_u32(out, crc32(out));
  return out;
}

bool load_hint(std::string_view data, std::uint64_t segment_id, std::uint64_t segment_size,
               const OpVisitor& visit, std::uint64_t* max_seq) {
  if (data.size() < kHintHeaderSize + 4) return false;
  if (std::memcmp(data.data(), kHintMagic, kHintMagicSize) != 0) return false;
  const std::size_t body_end = data.size() - 4;
  if (crc32(data.data(), body_end) != get_u32(data.data() + body_end)) return false;

  const char* p = data.data() + kHintMagicSize;
  if (get_u64(p) != segment_id || get_u64(p + 8) != segment_size) return false;
  *max_seq = get_u64(p + 16);
  const std::uint32_t count = get_u32(p + 24);

  std::size_t pos = kHintHeaderSize;
  for (std::uint32_t i = 0; i < count; ++i) {
    // The checksum passed, so a malformed entry means a writer bug or a forged
    // file. Entries may already have been visited, so this cannot fall back.
    if (body_end - pos < kEntryFixedSize) {
      throw Error(ErrorCode::Corruption, "malformed hint for segment " + std::to_string(segment_id));
    }
    const char* e = data.data() + pos;
    const std::uint32_t key_len = get_u32(e + 21);
    if (body_end - pos - kEntryFixedSize < key_len) {
      throw Error(ErrorCode::Corruption, "malformed hint for segment " + std::to_string(segment_id));
    }
    visit(SegmentOp{get_u64(e), e[8] != 0, std::string_view(e + kEntryFixedSize, key_len),
                    get_u64(e + 9), get_u32(e + 17)});
    pos += kEntryFixedSize + key_len;
  }
  return true;
}

void write_hint_best_effort(const std::string& dir, std::uint64_t segment_id,
                            std::uint64_t segment_size, const HintBuilder& builder) {
  try {
    write_file_atomic(hint_path(dir, segment_id), builder.finish(segment_id, segment_size));
  } catch (const Error&) {
    // See the header: the next open scans the segment instead.
  }
}

}  // namespace tinykv
