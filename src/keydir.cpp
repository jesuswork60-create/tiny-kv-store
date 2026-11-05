#include "keydir.h"

#include <algorithm>

#include "record.h"

namespace tinykv {

const KeyEntry* KeyDir::find(std::string_view key) const {
  const auto it = entries_.find(key);
  return it == entries_.end() ? nullptr : &it->second;
}

std::optional<KeyEntry> KeyDir::upsert(std::string_view key, const KeyEntry& entry) {
  const auto it = entries_.find(key);
  if (it == entries_.end()) {
    entries_.emplace(std::string(key), entry);
    return std::nullopt;
  }
  const KeyEntry previous = it->second;
  it->second = entry;
  return previous;
}

std::optional<KeyEntry> KeyDir::erase(std::string_view key) {
  const auto it = entries_.find(key);
  if (it == entries_.end()) return std::nullopt;
  const KeyEntry previous = it->second;
  entries_.erase(it);
  return previous;
}

KeyDir::Map::const_iterator KeyDir::range_begin(const ScanRange& range) const {
  std::string_view lower = range.prefix;
  if (range.start && *range.start > lower) lower = *range.start;
  return entries_.lower_bound(lower);
}

bool KeyDir::past_range_end(const ScanRange& range, std::string_view key) {
  if (range.end && key >= *range.end) return true;
  return key.substr(0, range.prefix.size()) != range.prefix;
}

void LogState::install(std::string_view key, const KeyEntry& entry) {
  if (const auto previous = keydir.upsert(key, entry)) release(*previous, key.size());
  const auto it = segments.find(entry.segment_id);
  if (it != segments.end()) it->second.live_bytes += record_size(key.size(), entry.value_len);
}

void LogState::remove(std::string_view key) {
  if (const auto previous = keydir.erase(key)) release(*previous, key.size());
}

void LogState::release(const KeyEntry& entry, std::size_t key_len) {
  const auto it = segments.find(entry.segment_id);
  if (it != segments.end()) it->second.live_bytes -= record_size(key_len, entry.value_len);
}

void Replayer::apply(std::uint64_t segment_id, const SegmentOp& op) {
  state_.last_seq = std::max(state_.last_seq, op.seq);
  const KeyEntry* current = state_.keydir.find(op.key);

  if (op.is_put) {
    const auto tomb = tombstones_.find(std::string(op.key));
    if (tomb != tombstones_.end() && tomb->second > op.seq) return;
    // An equal sequence is the same record copied by an interrupted
    // compaction; the later file is the copy that survives, so prefer it.
    if (current != nullptr && current->seq > op.seq) return;
    state_.install(op.key, KeyEntry{segment_id, op.offset, op.value_len, op.seq});
    return;
  }

  std::uint64_t& newest = tombstones_[std::string(op.key)];
  newest = std::max(newest, op.seq);
  if (current != nullptr && current->seq < op.seq) state_.remove(op.key);
}

}  // namespace tinykv
