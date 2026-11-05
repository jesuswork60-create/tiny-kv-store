#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tinykv/error.h"
#include "tinykv/options.h"
#include "tinykv/write_batch.h"

namespace tinykv {

// What open() found and repaired.
struct RecoveryReport {
  struct TailRepair {
    std::uint64_t segment_id = 0;
    std::uint64_t truncated_bytes = 0;
    std::uint64_t discarded_batch_ops = 0;
    std::string reason;
  };
  struct SalvagedSegment {
    std::uint64_t segment_id = 0;
    std::uint64_t damage_offset = 0;
    std::uint64_t skipped_bytes = 0;
    std::string reason;
  };

  std::uint64_t segments_scanned = 0;
  std::uint64_t segments_from_hints = 0;
  std::uint64_t hints_rejected = 0;
  std::uint64_t stale_files_removed = 0;
  std::optional<TailRepair> tail_repair;
  std::vector<SalvagedSegment> salvaged;
};

struct Stats {
  std::uint64_t live_keys = 0;
  std::uint64_t live_bytes = 0;     // Encoded size of the records keys point at.
  std::uint64_t total_bytes = 0;    // Size of all segment files.
  std::uint64_t garbage_bytes = 0;  // Record space that compaction would reclaim.
  std::uint64_t segments = 0;
  std::uint64_t active_segment = 0;
  std::uint64_t last_sequence = 0;
  std::uint64_t compactions = 0;  // Since this store was opened.
};

struct CompactionStats {
  std::uint64_t segments_removed = 0;
  std::uint64_t segments_written = 0;
  std::uint64_t keys_written = 0;
  std::uint64_t bytes_before = 0;
  std::uint64_t bytes_after = 0;
};

// Selects a contiguous run of keys in sorted order. All fields are optional
// and combine as an intersection.
struct ScanRange {
  std::string prefix;
  std::optional<std::string> start;  // Inclusive.
  std::optional<std::string> end;    // Exclusive.
  std::size_t limit = 0;             // 0 means no limit.
};

// A point-in-time view over a range of keys. Later writes and compactions do
// not affect it. It may outlive the Store, since it keeps the segment files it
// needs open. An iterator instance must not be shared between threads.
class Iterator {
 public:
  Iterator();
  ~Iterator();
  Iterator(Iterator&&) noexcept;
  Iterator& operator=(Iterator&&) noexcept;

  bool valid() const;
  void next();
  // Moves to the first key >= target within the snapshot, in either direction.
  void seek(std::string_view target);
  std::string_view key() const;
  // Reads and checks the record on first use; throws Error on corruption.
  const std::string& value();

 private:
  friend class Store;
  struct Snapshot;
  explicit Iterator(std::unique_ptr<Snapshot> snapshot);
  std::unique_ptr<Snapshot> snapshot_;
};

// A persistent log-structured key-value store. All methods are safe to call
// from several threads; reads run concurrently and exclude writes, and a
// compaction excludes everything else while it runs.
class Store {
 public:
  // Opens or creates the store in `dir`. Throws Error with code Locked when
  // the directory is already open, Corruption when a sealed segment is damaged
  // and Options::salvage is off.
  static std::unique_ptr<Store> open(const std::string& dir, const Options& options = {});

  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  void put(std::string_view key, std::string_view value);
  std::optional<std::string> get(std::string_view key) const;
  bool contains(std::string_view key) const;
  // Returns whether the key existed. Deleting an absent key writes nothing.
  bool del(std::string_view key);
  // Applies the batch atomically: after a crash, either all of it or none of
  // it is visible.
  void write(const WriteBatch& batch);

  Iterator scan(const ScanRange& range = {}) const;
  std::vector<std::string> keys(std::string_view prefix = {}) const;
  std::size_t size() const;

  // Rewrites live records into new segments and removes all older ones.
  CompactionStats compact();
  void sync();
  Stats stats() const;
  const RecoveryReport& recovery_report() const;

  // Syncs and releases the directory lock. Further calls throw Closed. The
  // destructor closes too, but swallows errors.
  void close();

 private:
  struct Impl;
  explicit Store(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace tinykv
