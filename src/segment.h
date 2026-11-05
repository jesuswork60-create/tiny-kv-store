#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "file.h"
#include "record.h"

namespace tinykv {

// Segment files start with a fixed header:
//   magic "TKVSEG01" | format version u32 | segment id u64 | base sequence u64 | crc32 u32
// followed by records. The base sequence is one more than the last sequence
// used when the segment was created, so numbers stay monotonic even after a
// compaction removes every record that carried the highest ones.
constexpr std::size_t kSegmentHeaderSize = 32;

struct SegmentHeader {
  std::uint64_t id = 0;
  std::uint64_t base_seq = 1;
};

std::string encode_segment_header(const SegmentHeader& header);
// Returns false if the file is too short or the header fails validation.
bool read_segment_header(const RandomFile& file, std::uint64_t file_size, SegmentHeader* out);

std::string segment_path(const std::string& dir, std::uint64_t id);
std::string hint_path(const std::string& dir, std::uint64_t id);

struct DirectoryListing {
  std::vector<std::uint64_t> segments;  // Ascending.
  std::set<std::uint64_t> hints;
  std::vector<std::string> temp_files;  // Full paths.
};
DirectoryListing list_directory(const std::string& dir);

// Point reads against one segment. Safe to share between threads, and keeps
// working after the file is unlinked by a compaction.
class SegmentReader {
 public:
  SegmentReader(const std::string& path, std::uint64_t id) : file_(path), id_(id) {}

  // Reads the record at `offset`, checks its checksum and that it really is
  // the expected put for `key`, and returns the value.
  std::string read_value(std::uint64_t offset, std::string_view key, std::uint64_t seq,
                         std::uint32_t value_len) const;

  const RandomFile& file() const { return file_; }
  std::uint64_t id() const { return id_; }

 private:
  RandomFile file_;
  std::uint64_t id_;
};

// Sequential decoding of raw records. Views returned by next() stay valid
// until the following call.
class SegmentScanner {
 public:
  enum class Status {
    Ok,
    End,      // Stopped cleanly at the end of the file.
    Torn,     // The file ends inside a record.
    Corrupt,  // A record failed validation.
  };

  SegmentScanner(const RandomFile& file, std::uint64_t file_size);

  Status next(Record* out);
  // Start of the record just returned, or of the one that failed.
  std::uint64_t offset() const { return offset_; }
  std::uint64_t next_offset() const { return next_offset_; }

 private:
  const char* fill(std::size_t size);

  const RandomFile& file_;
  std::uint64_t file_size_;
  std::uint64_t offset_;
  std::uint64_t next_offset_;
  std::string buffer_;
  std::uint64_t buffer_start_ = 0;
};

// One committed logical write in a segment.
struct SegmentOp {
  std::uint64_t seq;
  bool is_put;
  std::string_view key;
  std::uint64_t offset;  // Of the record.
  std::uint32_t value_len;
};
using OpVisitor = std::function<void(const SegmentOp&)>;

enum class WalkEnd {
  Clean,
  TornRecord,
  CorruptRecord,
  UncommittedBatch,  // Reached the end while a batch was still open.
  BadFraming,        // Batch records in an order no writer produces.
};

struct WalkResult {
  WalkEnd end = WalkEnd::Clean;
  std::uint64_t valid_end = kSegmentHeaderSize;  // End of the last committed record.
  std::uint64_t damage_offset = 0;               // Where the walk stopped, if not Clean.
  std::uint64_t records = 0;
  std::uint64_t discarded_ops = 0;  // Batch ops dropped because no commit followed.
  std::string detail;
};

// Walks a segment applying batch framing: `visit` sees plain writes at once and
// batch ops only when their commit record turns up. Never throws for bad data;
// the result says where and why the walk stopped.
WalkResult walk_segment(const RandomFile& file, std::uint64_t file_size, const OpVisitor& visit);

}  // namespace tinykv
