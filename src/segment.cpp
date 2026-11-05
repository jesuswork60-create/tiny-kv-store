#include "segment.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#include "coding.h"
#include "crc32.h"
#include "tinykv/error.h"

namespace tinykv {
namespace {

constexpr char kMagic[] = "TKVSEG01";
constexpr std::size_t kMagicSize = 8;
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kScanChunk = 1024 * 1024;

std::string numbered_path(const std::string& dir, std::uint64_t id, const char* suffix) {
  char name[32];
  std::snprintf(name, sizeof name, "%012llu", static_cast<unsigned long long>(id));
  return dir + "/" + name + suffix;
}

// Parses "000000000012.seg" style names. Returns false for anything else.
bool parse_numbered(const std::string& name, const char* suffix, std::uint64_t* id) {
  const std::size_t suffix_len = std::strlen(suffix);
  if (name.size() <= suffix_len || name.compare(name.size() - suffix_len, suffix_len, suffix) != 0) {
    return false;
  }
  const std::string stem = name.substr(0, name.size() - suffix_len);
  if (stem.size() > 19 || !std::all_of(stem.begin(), stem.end(), [](unsigned char c) {
        return c >= '0' && c <= '9';
      })) {
    return false;
  }
  *id = std::stoull(stem);
  return true;
}

}  // namespace

std::string encode_segment_header(const SegmentHeader& header) {
  std::string out(kMagic, kMagicSize);
  put_u32(out, kFormatVersion);
  put_u64(out, header.id);
  put_u64(out, header.base_seq);
  put_u32(out, crc32(out));
  return out;
}

bool read_segment_header(const RandomFile& file, std::uint64_t file_size, SegmentHeader* out) {
  if (file_size < kSegmentHeaderSize) return false;
  char raw[kSegmentHeaderSize];
  file.read_exact(0, raw, sizeof raw);
  if (std::memcmp(raw, kMagic, kMagicSize) != 0) return false;
  if (crc32(raw, kSegmentHeaderSize - 4) != get_u32(raw + kSegmentHeaderSize - 4)) return false;
  if (get_u32(raw + 8) != kFormatVersion) return false;
  out->id = get_u64(raw + 12);
  out->base_seq = get_u64(raw + 20);
  return true;
}

std::string segment_path(const std::string& dir, std::uint64_t id) {
  return numbered_path(dir, id, ".seg");
}

std::string hint_path(const std::string& dir, std::uint64_t id) {
  return numbered_path(dir, id, ".hint");
}

DirectoryListing list_directory(const std::string& dir) {
  namespace fs = std::filesystem;
  DirectoryListing listing;
  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  if (ec) throw Error(ErrorCode::Io, "cannot list " + dir + ": " + ec.message());
  for (const fs::directory_entry& entry : it) {
    const std::string name = entry.path().filename().string();
    std::uint64_t id = 0;
    if (parse_numbered(name, ".seg", &id)) {
      listing.segments.push_back(id);
    } else if (parse_numbered(name, ".hint", &id)) {
      listing.hints.insert(id);
    } else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
      listing.temp_files.push_back(entry.path().string());
    }
  }
  std::sort(listing.segments.begin(), listing.segments.end());
  return listing;
}

std::string SegmentReader::read_value(std::uint64_t offset, std::string_view key, std::uint64_t seq,
                                      std::uint32_t value_len) const {
  const auto fail = [&](const char* what) -> Error {
    return Error(ErrorCode::Corruption, std::string(what) + " in segment " + std::to_string(id_) +
                                            " at offset " + std::to_string(offset));
  };
  const auto size = static_cast<std::size_t>(record_size(key.size(), value_len));
  std::string raw(size, '\0');
  file_.read_exact(offset, &raw[0], size);

  RecordHeader header;
  Record record;
  if (!parse_record_header(raw.data(), &header) || header.total_size() != size) {
    throw fail("bad record header");
  }
  if (!decode_record(raw.data(), header, &record)) throw fail("checksum mismatch");
  const bool is_put = record.type == RecordType::Put || record.type == RecordType::BatchPut;
  if (!is_put || record.seq != seq || record.key != key) throw fail("unexpected record");
  return std::string(record.value);
}

SegmentScanner::SegmentScanner(const RandomFile& file, std::uint64_t file_size)
    : file_(file), file_size_(file_size), offset_(kSegmentHeaderSize), next_offset_(kSegmentHeaderSize) {}

const char* SegmentScanner::fill(std::size_t size) {
  if (offset_ < buffer_start_ || offset_ + size > buffer_start_ + buffer_.size()) {
    const std::uint64_t remaining = file_size_ - offset_;
    const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(std::max(size, kScanChunk), remaining));
    buffer_.resize(want);
    file_.read_exact(offset_, &buffer_[0], want);
    buffer_start_ = offset_;
  }
  return buffer_.data() + (offset_ - buffer_start_);
}

SegmentScanner::Status SegmentScanner::next(Record* out) {
  offset_ = next_offset_;
  if (offset_ >= file_size_) return Status::End;
  const std::uint64_t remaining = file_size_ - offset_;
  if (remaining < kRecordHeaderSize) return Status::Torn;

  RecordHeader header;
  if (!parse_record_header(fill(kRecordHeaderSize), &header)) return Status::Corrupt;
  if (header.total_size() > remaining) return Status::Torn;
  if (!decode_record(fill(static_cast<std::size_t>(header.total_size())), header, out)) {
    return Status::Corrupt;
  }
  next_offset_ = offset_ + header.total_size();
  return Status::Ok;
}

namespace {

struct PendingOp {
  std::uint64_t seq;
  bool is_put;
  std::string key;
  std::uint64_t offset;
  std::uint32_t value_len;
};

}  // namespace

WalkResult walk_segment(const RandomFile& file, std::uint64_t file_size, const OpVisitor& visit) {
  WalkResult result;
  SegmentScanner scanner(file, file_size);
  std::vector<PendingOp> pending;

  const auto stop = [&](WalkEnd end, std::string detail) {
    result.end = end;
    result.damage_offset = scanner.offset();
    result.discarded_ops = pending.size();
    result.detail = std::move(detail);
    return result;
  };

  Record record;
  for (;;) {
    switch (scanner.next(&record)) {
      case SegmentScanner::Status::End:
        if (!pending.empty()) return stop(WalkEnd::UncommittedBatch, "batch without a commit record");
        return result;
      case SegmentScanner::Status::Torn:
        return stop(WalkEnd::TornRecord, "file ends inside a record");
      case SegmentScanner::Status::Corrupt:
        return stop(WalkEnd::CorruptRecord, "record failed validation");
      case SegmentScanner::Status::Ok:
        break;
    }
    ++result.records;

    switch (record.type) {
      case RecordType::Put:
      case RecordType::Delete:
        if (!pending.empty()) return stop(WalkEnd::BadFraming, "plain record inside a batch");
        visit(SegmentOp{record.seq, record.type == RecordType::Put, record.key, scanner.offset(),
                        static_cast<std::uint32_t>(record.value.size())});
        result.valid_end = scanner.next_offset();
        break;
      case RecordType::BatchPut:
      case RecordType::BatchDelete:
        pending.push_back(PendingOp{record.seq, record.type == RecordType::BatchPut,
                                    std::string(record.key), scanner.offset(),
                                    static_cast<std::uint32_t>(record.value.size())});
        break;
      case RecordType::BatchCommit: {
        if (pending.empty() || record.value.size() != 4 ||
            get_u32(record.value.data()) != pending.size()) {
          return stop(WalkEnd::BadFraming, "commit record does not match its batch");
        }
        for (const PendingOp& op : pending) {
          visit(SegmentOp{op.seq, op.is_put, op.key, op.offset, op.value_len});
        }
        pending.clear();
        result.valid_end = scanner.next_offset();
        break;
      }
    }
  }
}

}  // namespace tinykv
