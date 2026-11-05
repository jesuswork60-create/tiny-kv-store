#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "segment.h"

namespace tinykv {

// A hint file lists a sealed segment's committed writes without their values,
// so open() can rebuild the key directory without reading the segment:
//   magic "TKVHINT1" | segment id u64 | segment size u64 | max sequence u64 | count u32
//   count * (seq u64 | is_put u8 | offset u64 | value_len u32 | key_len u32 | key)
//   crc32 u32 over everything before it
// Hints are advisory. Any mismatch just means the segment is scanned instead.
class HintBuilder {
 public:
  void add(const SegmentOp& op);
  bool empty() const { return count_ == 0; }
  void clear();
  // Serializes the hint for a segment of `segment_size` bytes.
  std::string finish(std::uint64_t segment_id, std::uint64_t segment_size) const;

 private:
  std::string body_;
  std::uint32_t count_ = 0;
  std::uint64_t max_seq_ = 0;
};

// Validates `data` against the segment id and size, then replays its entries in
// their original order. Returns false, without visiting anything, on mismatch.
bool load_hint(std::string_view data, std::uint64_t segment_id, std::uint64_t segment_size,
               const OpVisitor& visit, std::uint64_t* max_seq);

// Writes the hint file for a sealed segment. Failures are ignored on purpose:
// a missing hint only costs a scan at the next open.
void write_hint_best_effort(const std::string& dir, std::uint64_t segment_id,
                            std::uint64_t segment_size, const HintBuilder& builder);

}  // namespace tinykv
