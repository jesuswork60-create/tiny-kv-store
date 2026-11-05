#pragma once

#include <cstdint>

namespace tinykv {

enum class SyncMode {
  Always,  // fsync after every put, delete and batch.
  EveryN,  // fsync after every Options::sync_every writes.
  Never,   // Leave flushing to the OS; segments are still synced when sealed.
};

struct Options {
  // A segment is sealed when the next write would take it past this size. A
  // single write bigger than the limit gets a segment of its own.
  std::uint64_t max_segment_bytes = 64u * 1024 * 1024;

  // Largest value accepted by put(). Values already on disk are not checked
  // against this, so the limit can be lowered without breaking a store.
  std::uint32_t max_value_bytes = 64u * 1024 * 1024;

  SyncMode sync = SyncMode::Always;
  std::uint32_t sync_every = 64;  // Only read when sync == SyncMode::EveryN.

  bool create_if_missing = true;

  // Read and write hint files. With this off, open() scans every segment.
  bool use_hints = true;

  // Open despite corruption in sealed segments, keeping whatever precedes the
  // damage. Damaged data can hide a tombstone, so a deleted key may reappear.
  bool salvage = false;
};

}  // namespace tinykv
