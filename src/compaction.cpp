#include "compaction.h"

#include <utility>
#include <vector>

#include "hint.h"

namespace tinykv {
namespace {

// Writes compaction outputs, rolling over at the segment size limit.
class OutputWriter {
 public:
  struct Finished {
    std::uint64_t id;
    std::uint64_t size;
  };

  OutputWriter(const std::string& dir, const Options& options, LogState& state)
      : dir_(dir), options_(options), state_(state) {}

  // Abandons anything still in progress. Used only when the compaction failed.
  ~OutputWriter() {
    if (file_ != nullptr) remove_quietly(temp_path());
    if (committed_) return;
    for (const Finished& output : finished_) {
      remove_quietly(hint_path(dir_, output.id));
      remove_quietly(segment_path(dir_, output.id));
    }
  }

  // Returns the location of the appended record.
  std::pair<std::uint64_t, std::uint64_t> append(std::string_view record, const SegmentOp& op) {
    const bool over_limit = file_ != nullptr && file_->size() > kSegmentHeaderSize &&
                            file_->size() + record.size() > options_.max_segment_bytes;
    if (over_limit) finish_current();
    if (file_ == nullptr) start_next();

    const std::uint64_t offset = file_->size();
    file_->append(record);
    SegmentOp located = op;
    located.offset = offset;
    if (options_.use_hints) hint_.add(located);
    return {id_, offset};
  }

  void finish_current() {
    if (file_ == nullptr) return;
    file_->sync();
    rename_file(temp_path(), segment_path(dir_, id_));
    if (options_.use_hints) write_hint_best_effort(dir_, id_, file_->size(), hint_);
    finished_.push_back(Finished{id_, file_->size()});
    file_.reset();
    hint_.clear();
  }

  const std::vector<Finished>& finished() const { return finished_; }
  void mark_committed() { committed_ = true; }

 private:
  static void remove_quietly(const std::string& path) {
    try {
      remove_file(path);
    } catch (const Error&) {
      // Temp files are cleared on open, and a stray output only duplicates
      // records that replay already tolerates.
    }
  }

  std::string temp_path() const { return segment_path(dir_, id_) + ".tmp"; }

  void start_next() {
    id_ = state_.next_segment_id++;
    file_ = std::make_unique<AppendFile>(AppendFile::create(temp_path()));
    SegmentHeader header;
    header.id = id_;
    header.base_seq = state_.last_seq + 1;
    file_->append(encode_segment_header(header));
  }

  const std::string& dir_;
  const Options& options_;
  LogState& state_;
  std::unique_ptr<AppendFile> file_;
  std::uint64_t id_ = 0;
  HintBuilder hint_;
  std::vector<Finished> finished_;
  bool committed_ = false;
};

}  // namespace

CompactionStats compact_log(const std::string& dir, const Options& options, LogState& state) {
  CompactionStats stats;
  std::vector<std::uint64_t> old_ids;
  for (const auto& [id, meta] : state.segments) {
    old_ids.push_back(id);
    stats.bytes_before += meta.file_size;
  }

  // Keys go out in sorted order, which also gives scans good locality.
  OutputWriter writer(dir, options, state);
  std::vector<std::pair<const std::string*, KeyEntry>> moved;
  moved.reserve(state.keydir.size());
  std::string record;
  for (const auto& [key, entry] : state.keydir.entries()) {
    const std::string value = state.segments.at(entry.segment_id)
                                  .reader->read_value(entry.offset, key, entry.seq, entry.value_len);
    record.clear();
    encode_record(record, entry.seq, RecordType::Put, key, value);
    const SegmentOp op{entry.seq, true, key, 0, entry.value_len};
    const auto [segment_id, offset] = writer.append(record, op);
    moved.emplace_back(&key, KeyEntry{segment_id, offset, entry.value_len, entry.seq});
  }
  writer.finish_current();
  sync_dir(dir);
  writer.mark_committed();

  // The new files are durable. From here on `state` moves over to them, and a
  // failed delete merely leaves garbage for the next compaction.
  for (const auto& output : writer.finished()) {
    auto reader = std::make_shared<const SegmentReader>(segment_path(dir, output.id), output.id);
    state.segments[output.id] = SegmentMeta{std::move(reader), output.size, 0};
    stats.bytes_after += output.size;
  }
  stats.segments_written = writer.finished().size();
  stats.keys_written = moved.size();
  for (const auto& [key, entry] : moved) state.install(*key, entry);

  for (const std::uint64_t id : old_ids) {
    remove_file(hint_path(dir, id));
    remove_file(segment_path(dir, id));
    sync_dir(dir);
    state.segments.erase(id);
    ++stats.segments_removed;
  }
  return stats;
}

}  // namespace tinykv
