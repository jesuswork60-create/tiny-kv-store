#include "recovery.h"

#include <algorithm>

#include "tinykv/error.h"

namespace tinykv {
namespace {

class Recovery {
 public:
  Recovery(const std::string& dir, const Options& options, LogState& state)
      : dir_(dir), options_(options), state_(state), replayer_(state) {}

  RecoveredLog run() {
    DirectoryListing listing = list_directory(dir_);
    for (const std::string& path : listing.temp_files) {
      remove_file(path);
      ++result_.report.stale_files_removed;
    }
    drop_unfinished_last_segment(listing);
    remove_orphan_hints(listing);

    for (const std::uint64_t id : listing.segments) {
      load_segment(id, id == listing.segments.back(), listing.hints.count(id) != 0);
    }
    if (!listing.segments.empty()) state_.next_segment_id = listing.segments.back() + 1;
    return std::move(result_);
  }

 private:
  // A crash between creating a segment and writing its header leaves a file
  // too short to hold one. It cannot contain data, so it simply goes away.
  void drop_unfinished_last_segment(DirectoryListing& listing) {
    if (listing.segments.empty()) return;
    const std::uint64_t id = listing.segments.back();
    const std::string path = segment_path(dir_, id);
    if (RandomFile(path).size() >= kSegmentHeaderSize) return;
    remove_file(path);
    listing.segments.pop_back();
    ++result_.report.stale_files_removed;
  }

  void remove_orphan_hints(DirectoryListing& listing) {
    for (auto it = listing.hints.begin(); it != listing.hints.end();) {
      if (std::binary_search(listing.segments.begin(), listing.segments.end(), *it)) {
        ++it;
      } else {
        remove_file(hint_path(dir_, *it));
        ++result_.report.stale_files_removed;
        it = listing.hints.erase(it);
      }
    }
  }

  void load_segment(std::uint64_t id, bool is_last, bool has_hint) {
    auto reader = std::make_shared<const SegmentReader>(segment_path(dir_, id), id);
    const std::uint64_t size = reader->file().size();
    state_.segments[id] = SegmentMeta{reader, size, 0};

    SegmentHeader header;
    if (!read_segment_header(reader->file(), size, &header) || header.id != id) {
      handle_old_damage(id, 0, size, "bad segment header");
      return;
    }
    if (header.base_seq > 0) state_.last_seq = std::max(state_.last_seq, header.base_seq - 1);

    if (is_last) {
      // The active segment keeps growing, so its hint would go stale anyway.
      if (has_hint) remove_file(hint_path(dir_, id));
      scan_last_segment(id, *reader, size);
    } else if (!(has_hint && options_.use_hints && load_from_hint(id, size))) {
      scan_sealed_segment(id, *reader, size);
    }
  }

  bool load_from_hint(std::uint64_t id, std::uint64_t size) {
    std::uint64_t max_seq = 0;
    const std::string data = read_whole_file(hint_path(dir_, id));
    const bool loaded = load_hint(
        data, id, size, [&](const SegmentOp& op) { replayer_.apply(id, op); }, &max_seq);
    if (!loaded) {
      ++result_.report.hints_rejected;
      return false;
    }
    state_.last_seq = std::max(state_.last_seq, max_seq);
    ++result_.report.segments_from_hints;
    return true;
  }

  void scan_sealed_segment(std::uint64_t id, const SegmentReader& reader, std::uint64_t size) {
    ++result_.report.segments_scanned;
    const WalkResult walk = walk_segment(reader.file(), size,
                                         [&](const SegmentOp& op) { replayer_.apply(id, op); });
    if (walk.end != WalkEnd::Clean) {
      handle_old_damage(id, walk.damage_offset, size - walk.valid_end, walk.detail);
    }
  }

  void scan_last_segment(std::uint64_t id, const SegmentReader& reader, std::uint64_t size) {
    ++result_.report.segments_scanned;
    const WalkResult walk = walk_segment(reader.file(), size, [&](const SegmentOp& op) {
      replayer_.apply(id, op);
      result_.active_hint.add(op);
    });
    result_.can_append = true;
    if (walk.end == WalkEnd::Clean) return;

    truncate_file(reader.file().path(), walk.valid_end);
    state_.segments[id].file_size = walk.valid_end;
    RecoveryReport::TailRepair repair;
    repair.segment_id = id;
    repair.truncated_bytes = size - walk.valid_end;
    repair.discarded_batch_ops = walk.discarded_ops;
    repair.reason = walk.detail;
    result_.report.tail_repair = std::move(repair);
  }

  // Damage where a crash cannot explain it: fatal unless salvaging. A salvaged
  // segment stays in place, unchanged, until the next compaction replaces it.
  void handle_old_damage(std::uint64_t id, std::uint64_t offset, std::uint64_t skipped,
                         const std::string& reason) {
    if (!options_.salvage) {
      throw Error(ErrorCode::Corruption, "segment " + std::to_string(id) + " is damaged at offset " +
                                             std::to_string(offset) + ": " + reason);
    }
    RecoveryReport::SalvagedSegment salvaged;
    salvaged.segment_id = id;
    salvaged.damage_offset = offset;
    salvaged.skipped_bytes = skipped;
    salvaged.reason = reason;
    result_.report.salvaged.push_back(std::move(salvaged));
  }

  const std::string& dir_;
  const Options& options_;
  LogState& state_;
  Replayer replayer_;
  RecoveredLog result_;
};

}  // namespace

RecoveredLog recover_log(const std::string& dir, const Options& options, LogState& state) {
  return Recovery(dir, options, state).run();
}

}  // namespace tinykv
