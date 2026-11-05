#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "segment.h"
#include "tinykv/store.h"

namespace tinykv {

// Where the current value of a key lives.
struct KeyEntry {
  std::uint64_t segment_id = 0;
  std::uint64_t offset = 0;
  std::uint32_t value_len = 0;
  std::uint64_t seq = 0;
};

// The in-memory index: every live key, in sorted order.
class KeyDir {
 public:
  using Map = std::map<std::string, KeyEntry, std::less<>>;

  const KeyEntry* find(std::string_view key) const;
  // Both return the entry that was displaced, if any.
  std::optional<KeyEntry> upsert(std::string_view key, const KeyEntry& entry);
  std::optional<KeyEntry> erase(std::string_view key);

  std::size_t size() const { return entries_.size(); }
  const Map& entries() const { return entries_; }

  // Calls fn(key, entry) in key order for keys selected by `range`.
  template <typename Fn>
  void for_each_in_range(const ScanRange& range, Fn&& fn) const;

 private:
  Map::const_iterator range_begin(const ScanRange& range) const;
  static bool past_range_end(const ScanRange& range, std::string_view key);

  Map entries_;
};

template <typename Fn>
void KeyDir::for_each_in_range(const ScanRange& range, Fn&& fn) const {
  std::size_t emitted = 0;
  for (auto it = range_begin(range); it != entries_.end(); ++it) {
    if (past_range_end(range, it->first)) break;
    fn(it->first, it->second);
    if (range.limit != 0 && ++emitted == range.limit) break;
  }
}

struct SegmentMeta {
  std::shared_ptr<const SegmentReader> reader;
  std::uint64_t file_size = 0;
  std::uint64_t live_bytes = 0;
};

// The index together with the segment table it points into. Keeps per-segment
// live-byte accounting correct as entries move around.
struct LogState {
  KeyDir keydir;
  std::map<std::uint64_t, SegmentMeta> segments;
  std::uint64_t last_seq = 0;
  std::uint64_t next_segment_id = 1;

  void install(std::string_view key, const KeyEntry& entry);
  void remove(std::string_view key);

 private:
  void release(const KeyEntry& entry, std::size_t key_len);
};

// Applies segment contents in file order with "highest sequence wins" rules.
// Tombstones are remembered for the duration of the replay, so a put that
// turns up later in file order cannot undo a newer delete.
class Replayer {
 public:
  explicit Replayer(LogState& state) : state_(state) {}

  void apply(std::uint64_t segment_id, const SegmentOp& op);

 private:
  LogState& state_;
  std::unordered_map<std::string, std::uint64_t> tombstones_;
};

}  // namespace tinykv
