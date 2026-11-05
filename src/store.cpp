#include "tinykv/store.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <system_error>

#include "coding.h"
#include "compaction.h"
#include "file.h"
#include "hint.h"
#include "keydir.h"
#include "recovery.h"

namespace tinykv {

// --- Iterator ---------------------------------------------------------------

struct Iterator::Snapshot {
  struct Item {
    std::string key;
    KeyEntry entry;
  };
  std::vector<Item> items;
  // Holding the readers keeps the files reachable after a compaction unlinks
  // them or the store closes.
  std::map<std::uint64_t, std::shared_ptr<const SegmentReader>> readers;
  std::size_t position = 0;
  std::optional<std::string> value;
};

Iterator::Iterator() : snapshot_(std::make_unique<Snapshot>()) {}
Iterator::Iterator(std::unique_ptr<Snapshot> snapshot) : snapshot_(std::move(snapshot)) {}
Iterator::~Iterator() = default;
Iterator::Iterator(Iterator&&) noexcept = default;
Iterator& Iterator::operator=(Iterator&&) noexcept = default;

bool Iterator::valid() const {
  return snapshot_ != nullptr && snapshot_->position < snapshot_->items.size();
}

void Iterator::next() {
  if (!valid()) return;
  ++snapshot_->position;
  snapshot_->value.reset();
}

void Iterator::seek(std::string_view target) {
  if (snapshot_ == nullptr) return;
  auto& items = snapshot_->items;
  const auto it = std::lower_bound(
      items.begin(), items.end(), target,
      [](const Snapshot::Item& item, std::string_view t) { return item.key < t; });
  snapshot_->position = static_cast<std::size_t>(it - items.begin());
  snapshot_->value.reset();
}

std::string_view Iterator::key() const {
  if (!valid()) throw Error(ErrorCode::InvalidArgument, "iterator is not valid");
  return snapshot_->items[snapshot_->position].key;
}

const std::string& Iterator::value() {
  if (!valid()) throw Error(ErrorCode::InvalidArgument, "iterator is not valid");
  if (!snapshot_->value) {
    const Snapshot::Item& item = snapshot_->items[snapshot_->position];
    const KeyEntry& e = item.entry;
    snapshot_->value = snapshot_->readers.at(e.segment_id)
                           ->read_value(e.offset, item.key, e.seq, e.value_len);
  }
  return *snapshot_->value;
}

// --- Store internals --------------------------------------------------------

struct Store::Impl {
  std::string dir;
  Options options;
  std::unique_ptr<FileLock> lock;
  mutable std::shared_mutex mutex;
  LogState state;
  RecoveryReport report;

  // The segment receiving writes; null only between sealing and the next write.
  std::unique_ptr<AppendFile> writer;
  std::uint64_t active_id = 0;
  HintBuilder active_hint;

  std::uint32_t unsynced_writes = 0;
  std::uint64_t compactions = 0;
  bool closed = false;

  void check_open() const {
    if (closed) throw Error(ErrorCode::Closed, "store is closed");
  }

  void check_writable() const {
    check_open();
    if (writer != nullptr && writer->damaged()) {
      throw Error(ErrorCode::Closed, "store is read-only after a failed write; reopen it");
    }
  }

  void validate_key(std::string_view key) const {
    if (key.empty()) throw Error(ErrorCode::InvalidArgument, "key must not be empty");
    if (key.size() > kMaxKeyBytes) {
      throw Error(ErrorCode::InvalidArgument, "key is larger than the limit of " +
                                                  std::to_string(kMaxKeyBytes) + " bytes");
    }
  }

  void validate_value(std::string_view value) const {
    const std::uint32_t limit = std::min(options.max_value_bytes, kMaxValueBytes);
    if (value.size() > limit) {
      throw Error(ErrorCode::InvalidArgument,
                  "value is larger than the limit of " + std::to_string(limit) + " bytes");
    }
  }

  void start_new_segment() {
    const std::uint64_t id = state.next_segment_id;
    const std::string path = segment_path(dir, id);
    AppendFile file = AppendFile::create(path);
    SegmentHeader header;
    header.id = id;
    header.base_seq = state.last_seq + 1;
    file.append(encode_segment_header(header));
    file.sync();
    sync_dir(dir);

    state.next_segment_id = id + 1;
    state.segments[id] = SegmentMeta{std::make_shared<const SegmentReader>(path, id), file.size(), 0};
    writer = std::make_unique<AppendFile>(std::move(file));
    active_id = id;
    active_hint.clear();
    unsynced_writes = 0;
  }

  void seal_active() {
    if (writer == nullptr) return;
    writer->sync();
    if (options.use_hints && !active_hint.empty()) {
      write_hint_best_effort(dir, active_id, writer->size(), active_hint);
    }
    writer.reset();
    active_hint.clear();
    unsynced_writes = 0;
  }

  // Makes sure the active segment can take `size` more bytes.
  void reserve(std::uint64_t size) {
    const bool full = writer != nullptr && writer->size() > kSegmentHeaderSize &&
                      writer->size() + size > options.max_segment_bytes;
    if (full) seal_active();
    if (writer == nullptr) start_new_segment();
  }

  void append_to_active(std::string_view encoded) {
    writer->append(encoded);
    state.segments[active_id].file_size = writer->size();
  }

  void after_write() {
    ++unsynced_writes;
    const bool due = options.sync == SyncMode::Always ||
                     (options.sync == SyncMode::EveryN && unsynced_writes >= options.sync_every);
    if (due) {
      writer->sync();
      unsynced_writes = 0;
    }
  }

  // Index update for a record already on disk in the active segment.
  void apply_record(const SegmentOp& op) {
    if (op.is_put) {
      state.install(op.key, KeyEntry{active_id, op.offset, op.value_len, op.seq});
    } else {
      state.remove(op.key);
    }
    if (options.use_hints) active_hint.add(op);
  }

  void write_single(RecordType type, std::string_view key, std::string_view value) {
    const std::uint64_t seq = state.last_seq + 1;
    std::string encoded;
    encode_record(encoded, seq, type, key, value);
    reserve(encoded.size());
    const std::uint64_t offset = writer->size();
    append_to_active(encoded);
    state.last_seq = seq;
    apply_record(SegmentOp{seq, type == RecordType::Put, key, offset,
                           static_cast<std::uint32_t>(value.size())});
    after_write();
  }

  void write_batch(const WriteBatch& batch) {
    // Encoded as one buffer and written with one append, so the batch sits in
    // a single segment and a failed write rolls back as a unit.
    std::string encoded;
    std::vector<std::uint64_t> relative_offsets;
    relative_offsets.reserve(batch.size());
    std::uint64_t seq = state.last_seq;
    for (const WriteBatch::Op& op : batch.ops()) {
      relative_offsets.push_back(encoded.size());
      encode_record(encoded, ++seq, op.is_put ? RecordType::BatchPut : RecordType::BatchDelete,
                    op.key, op.value);
    }
    std::string count;
    put_u32(count, static_cast<std::uint32_t>(batch.size()));
    // The commit shares the last op's sequence: recovery only tracks ops, and this
    // keeps last_sequence the same before and after a reopen.
    encode_record(encoded, seq, RecordType::BatchCommit, {}, count);

    reserve(encoded.size());
    const std::uint64_t base = writer->size();
    append_to_active(encoded);

    std::uint64_t op_seq = state.last_seq;
    for (std::size_t i = 0; i < batch.size(); ++i) {
      const WriteBatch::Op& op = batch.ops()[i];
      apply_record(SegmentOp{++op_seq, op.is_put, op.key, base + relative_offsets[i],
                             static_cast<std::uint32_t>(op.value.size())});
    }
    state.last_seq = seq;
    after_write();
  }
};

// --- Store ------------------------------------------------------------------

Store::Store(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Store::~Store() {
  try {
    close();
  } catch (const Error&) {
    // Destructors cannot report; callers who care use close().
  }
}

std::unique_ptr<Store> Store::open(const std::string& dir, const Options& options) {
  namespace fs = std::filesystem;
  if (options.max_segment_bytes < 2 * kSegmentHeaderSize) {
    throw Error(ErrorCode::InvalidArgument, "max_segment_bytes is too small");
  }
  if (options.sync == SyncMode::EveryN && options.sync_every == 0) {
    throw Error(ErrorCode::InvalidArgument, "sync_every must be positive");
  }
  std::error_code ec;
  if (options.create_if_missing) {
    fs::create_directories(dir, ec);
    if (ec) throw Error(ErrorCode::Io, "cannot create " + dir + ": " + ec.message());
  } else if (!fs::is_directory(dir, ec)) {
    throw Error(ErrorCode::InvalidArgument, "no store directory at " + dir);
  }

  auto impl = std::make_unique<Impl>();
  impl->dir = dir;
  impl->options = options;
  impl->lock = std::make_unique<FileLock>(dir + "/LOCK");

  RecoveredLog recovered = recover_log(dir, options, impl->state);
  impl->report = std::move(recovered.report);
  if (recovered.can_append) {
    const std::uint64_t id = impl->state.segments.rbegin()->first;
    impl->writer = std::make_unique<AppendFile>(AppendFile::open_existing(segment_path(dir, id)));
    impl->active_id = id;
    impl->active_hint = std::move(recovered.active_hint);
  } else {
    impl->start_new_segment();
  }
  return std::unique_ptr<Store>(new Store(std::move(impl)));
}

void Store::put(std::string_view key, std::string_view value) {
  std::unique_lock lock(impl_->mutex);
  impl_->check_writable();
  impl_->validate_key(key);
  impl_->validate_value(value);
  impl_->write_single(RecordType::Put, key, value);
}

std::optional<std::string> Store::get(std::string_view key) const {
  std::shared_lock lock(impl_->mutex);
  impl_->check_open();
  const KeyEntry* entry = impl_->state.keydir.find(key);
  if (entry == nullptr) return std::nullopt;
  return impl_->state.segments.at(entry->segment_id)
      .reader->read_value(entry->offset, key, entry->seq, entry->value_len);
}

bool Store::contains(std::string_view key) const {
  std::shared_lock lock(impl_->mutex);
  impl_->check_open();
  return impl_->state.keydir.find(key) != nullptr;
}

bool Store::del(std::string_view key) {
  std::unique_lock lock(impl_->mutex);
  impl_->check_writable();
  impl_->validate_key(key);
  if (impl_->state.keydir.find(key) == nullptr) return false;
  impl_->write_single(RecordType::Delete, key, {});
  return true;
}

void Store::write(const WriteBatch& batch) {
  std::unique_lock lock(impl_->mutex);
  impl_->check_writable();
  if (batch.empty()) return;
  for (const WriteBatch::Op& op : batch.ops()) {
    impl_->validate_key(op.key);
    impl_->validate_value(op.value);
  }
  impl_->write_batch(batch);
}

Iterator Store::scan(const ScanRange& range) const {
  auto snapshot = std::make_unique<Iterator::Snapshot>();
  std::shared_lock lock(impl_->mutex);
  impl_->check_open();
  impl_->state.keydir.for_each_in_range(range, [&](const std::string& key, const KeyEntry& entry) {
    snapshot->items.push_back(Iterator::Snapshot::Item{key, entry});
    auto& reader = snapshot->readers[entry.segment_id];
    if (reader == nullptr) reader = impl_->state.segments.at(entry.segment_id).reader;
  });
  return Iterator(std::move(snapshot));
}

std::vector<std::string> Store::keys(std::string_view prefix) const {
  ScanRange range;
  range.prefix = std::string(prefix);
  std::vector<std::string> out;
  std::shared_lock lock(impl_->mutex);
  impl_->check_open();
  impl_->state.keydir.for_each_in_range(
      range, [&](const std::string& key, const KeyEntry&) { out.push_back(key); });
  return out;
}

std::size_t Store::size() const {
  std::shared_lock lock(impl_->mutex);
  impl_->check_open();
  return impl_->state.keydir.size();
}

CompactionStats Store::compact() {
  std::unique_lock lock(impl_->mutex);
  impl_->check_writable();
  impl_->seal_active();
  const CompactionStats stats = compact_log(impl_->dir, impl_->options, impl_->state);
  ++impl_->compactions;
  impl_->start_new_segment();
  return stats;
}

void Store::sync() {
  std::unique_lock lock(impl_->mutex);
  impl_->check_writable();
  if (impl_->writer != nullptr) impl_->writer->sync();
  impl_->unsynced_writes = 0;
}

Stats Store::stats() const {
  std::shared_lock lock(impl_->mutex);
  impl_->check_open();
  Stats out;
  out.live_keys = impl_->state.keydir.size();
  out.segments = impl_->state.segments.size();
  out.active_segment = impl_->active_id;
  out.last_sequence = impl_->state.last_seq;
  out.compactions = impl_->compactions;
  for (const auto& [id, meta] : impl_->state.segments) {
    out.live_bytes += meta.live_bytes;
    out.total_bytes += meta.file_size;
    const std::uint64_t data = meta.file_size - std::min<std::uint64_t>(meta.file_size, kSegmentHeaderSize);
    out.garbage_bytes += data - meta.live_bytes;
  }
  return out;
}

const RecoveryReport& Store::recovery_report() const { return impl_->report; }

void Store::close() {
  std::unique_lock lock(impl_->mutex);
  if (impl_->closed) return;
  impl_->closed = true;
  // Reader handles stay behind in the state until the Store goes away, but the
  // lock is released now so the directory can be reopened.
  const auto release = [&] {
    impl_->writer.reset();
    impl_->lock.reset();
  };
  try {
    if (impl_->writer != nullptr && !impl_->writer->damaged()) impl_->writer->sync();
  } catch (const Error&) {
    release();
    throw;
  }
  release();
}

}  // namespace tinykv
