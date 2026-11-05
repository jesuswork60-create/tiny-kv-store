#include "tinykv/inspect.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <ostream>
#include <system_error>
#include <utility>

#include "file.h"
#include "hint.h"
#include "keydir.h"
#include "segment.h"
#include "tinykv/error.h"

namespace tinykv {
namespace {

void require_directory(const std::string& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    throw Error(ErrorCode::InvalidArgument, "no store directory at " + dir);
  }
}

// Printable ASCII passes through; everything else becomes \xNN.
std::string printable(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (const char c : bytes) {
    const auto u = static_cast<unsigned char>(c);
    if (std::isprint(u) && c != '\\') {
      out.push_back(c);
    } else {
      char escaped[5];
      std::snprintf(escaped, sizeof escaped, "\\x%02x", u);
      out.append(escaped);
    }
  }
  return out;
}

class Verifier {
 public:
  explicit Verifier(const std::string& dir) : dir_(dir), replayer_(state_) {}

  VerifyReport run() {
    const DirectoryListing listing = list_directory(dir_);
    for (const std::string& path : listing.temp_files) {
      add(VerifyIssue::Severity::Warning, 0, "leftover temporary file " +
                                                 std::filesystem::path(path).filename().string());
    }
    for (const std::uint64_t id : listing.hints) {
      if (!std::binary_search(listing.segments.begin(), listing.segments.end(), id)) {
        add(VerifyIssue::Severity::Warning, id, "hint file without a segment");
      }
    }
    for (const std::uint64_t id : listing.segments) {
      check_segment(id, id == listing.segments.back(), listing.hints.count(id) != 0);
    }
    report_.live_keys = state_.keydir.size();
    return std::move(report_);
  }

 private:
  void add(VerifyIssue::Severity severity, std::uint64_t id, std::string message) {
    report_.issues.push_back(VerifyIssue{severity, id, std::move(message)});
  }

  void check_segment(std::uint64_t id, bool is_last, bool has_hint) {
    const RandomFile file(segment_path(dir_, id));
    const std::uint64_t size = file.size();
    ++report_.segments;
    report_.bytes += size;

    SegmentHeader header;
    if (is_last && size < kSegmentHeaderSize) {
      add(VerifyIssue::Severity::Warning, id, "unfinished segment, open will remove it");
      return;
    }
    if (!read_segment_header(file, size, &header) || header.id != id) {
      add(VerifyIssue::Severity::Error, id, "bad segment header");
      return;
    }

    HintBuilder rebuilt;
    const WalkResult walk = walk_segment(file, size, [&](const SegmentOp& op) {
      replayer_.apply(id, op);
      rebuilt.add(op);
    });
    report_.records += walk.records;
    if (walk.end != WalkEnd::Clean) {
      const auto severity = is_last ? VerifyIssue::Severity::Warning : VerifyIssue::Severity::Error;
      const std::string consequence = is_last ? "; open will truncate the tail" : "";
      add(severity, id, walk.detail + " at offset " + std::to_string(walk.damage_offset) + ", " +
                            std::to_string(size - walk.valid_end) + " bytes affected" + consequence);
    } else if (has_hint) {
      check_hint(id, size, rebuilt);
    }
  }

  void check_hint(std::uint64_t id, std::uint64_t size, const HintBuilder& rebuilt) {
    ++report_.hints_checked;
    const std::string data = read_whole_file(hint_path(dir_, id));
    std::uint64_t max_seq = 0;
    const auto ignore = [](const SegmentOp&) {};
    if (!load_hint(data, id, size, ignore, &max_seq)) {
      add(VerifyIssue::Severity::Warning, id, "stale or damaged hint, open will scan instead");
    } else if (data != rebuilt.finish(id, size)) {
      add(VerifyIssue::Severity::Error, id, "hint disagrees with the segment contents");
    }
  }

  const std::string& dir_;
  LogState state_;
  Replayer replayer_;
  VerifyReport report_;
};

}  // namespace

bool VerifyReport::ok() const {
  for (const VerifyIssue& issue : issues) {
    if (issue.severity == VerifyIssue::Severity::Error) return false;
  }
  return true;
}

VerifyReport verify_store(const std::string& dir) {
  require_directory(dir);
  const FileLock lock(dir + "/LOCK");
  return Verifier(dir).run();
}

void dump_store(const std::string& dir, std::ostream& out, const DumpOptions& options) {
  require_directory(dir);
  for (const std::uint64_t id : list_directory(dir).segments) {
    const RandomFile file(segment_path(dir, id));
    const std::uint64_t size = file.size();
    SegmentHeader header;
    if (!read_segment_header(file, size, &header)) {
      out << "segment " << id << ": bad header, " << size << " bytes\n";
      continue;
    }
    out << "segment " << id << ": " << size << " bytes, base sequence " << header.base_seq << "\n";

    SegmentScanner scanner(file, size);
    Record record;
    SegmentScanner::Status status;
    while ((status = scanner.next(&record)) == SegmentScanner::Status::Ok) {
      out << "  @" << scanner.offset() << " seq=" << record.seq << " "
          << record_type_name(record.type);
      if (!record.key.empty()) out << " key=" << printable(record.key);
      if (record.type == RecordType::BatchCommit || !options.show_values) {
        out << " value_len=" << record.value.size() << "\n";
      } else {
        out << " value=" << printable(record.value) << "\n";
      }
    }
    if (status != SegmentScanner::Status::End) {
      out << "  @" << scanner.offset()
          << (status == SegmentScanner::Status::Torn ? " torn record\n" : " corrupt record\n");
    }
  }
}

}  // namespace tinykv
