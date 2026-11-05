#include "check.h"
#include "record.h"
#include "segment.h"

using namespace tinykv;
using namespace testing;

namespace {

std::string header_bytes(std::uint64_t id, std::uint64_t base_seq = 1) {
  SegmentHeader header;
  header.id = id;
  header.base_seq = base_seq;
  return encode_segment_header(header);
}

std::string rec(std::uint64_t seq, RecordType type, std::string_view key, std::string_view value = {}) {
  std::string encoded;
  encode_record(encoded, seq, type, key, value);
  return encoded;
}

// Three sealed segments plus a little more in the active one.
void write_many(const std::string& dir) {
  auto store = Store::open(dir, small_segments(150));
  for (int i = 0; i < 20; ++i) store->put("key" + std::to_string(i), "value" + std::to_string(i));
  REQUIRE(store->stats().segments >= 3);
}

Options no_hints() {
  Options options = small_segments(150);
  options.use_hints = false;
  return options;
}

}  // namespace

TEST_CASE("cutting the last record at any byte loses only that record") {
  TempDir source;
  std::uint64_t good_end = 0;
  {
    auto store = Store::open(source.path(), fast_options());
    store->put("a", "1");
    store->put("b", "2");
    good_end = store->stats().total_bytes;
    store->put("c", "3");
  }
  const std::string seg = segment_files(source.path()).back();
  const std::string data = read_file(seg);

  for (std::uint64_t cut = good_end + 1; cut < data.size(); ++cut) {
    TempDir dir;
    const std::string path = dir.path() + "/" + fs::path(seg).filename().string();
    write_file(path, data.substr(0, static_cast<std::size_t>(cut)));
    {
      auto store = Store::open(dir.path(), fast_options());
      REQUIRE(store->recovery_report().tail_repair.has_value());
      CHECK_EQ(store->recovery_report().tail_repair->truncated_bytes, cut - good_end);
      CHECK_EQ(store->recovery_report().tail_repair->segment_id, 1u);
      CHECK_EQ(store->size(), 2u);
      CHECK(!store->contains("c"));
      // The tail must really be gone, so a new write lands on a clean boundary.
      CHECK_EQ(fs::file_size(path), good_end);
      store->put("d", "4");
    }
    auto store = Store::open(dir.path(), fast_options());
    CHECK(!store->recovery_report().tail_repair.has_value());
    CHECK_EQ(store->get("d"), std::optional<std::string>("4"));
    CHECK_EQ(store->size(), 3u);
  }
}

TEST_CASE("flipping a byte in the last record truncates it") {
  TempDir source;
  std::uint64_t good_end = 0;
  std::uint64_t total = 0;
  {
    auto store = Store::open(source.path(), fast_options());
    store->put("a", "1");
    good_end = store->stats().total_bytes;
    store->put("b", "2");
    total = store->stats().total_bytes;
  }
  const std::string seg = segment_files(source.path()).back();
  const std::string data = read_file(seg);
  for (std::uint64_t at = good_end; at < total; ++at) {
    TempDir dir;
    const std::string path = dir.path() + "/" + fs::path(seg).filename().string();
    write_file(path, data);
    flip_byte(path, at);
    auto store = Store::open(dir.path(), fast_options());
    REQUIRE(store->recovery_report().tail_repair.has_value());
    CHECK_EQ(store->get("a"), std::optional<std::string>("1"));
    CHECK(!store->contains("b"));
  }
}

TEST_CASE("damage inside the last segment drops everything after it") {
  TempDir dir;
  std::uint64_t first_end = 0;
  {
    auto store = Store::open(dir.path(), fast_options());
    store->put("a", "1");
    first_end = store->stats().total_bytes;
    store->put("b", "2");
    store->put("c", "3");
  }
  flip_byte(segment_files(dir.path()).back(), first_end + 2);
  auto store = Store::open(dir.path(), fast_options());
  REQUIRE(store->recovery_report().tail_repair.has_value());
  CHECK_EQ(store->size(), 1u);
  CHECK(store->contains("a"));
}

TEST_CASE("a zero-filled tail is removed") {
  TempDir dir;
  std::uint64_t good_end = 0;
  {
    auto store = Store::open(dir.path(), fast_options());
    store->put("a", "1");
    good_end = store->stats().total_bytes;
  }
  const std::string seg = segment_files(dir.path()).back();
  write_file(seg, read_file(seg) + std::string(4096, '\0'));
  auto store = Store::open(dir.path(), fast_options());
  REQUIRE(store->recovery_report().tail_repair.has_value());
  CHECK_EQ(store->recovery_report().tail_repair->truncated_bytes, 4096u);
  CHECK_EQ(fs::file_size(seg), good_end);
  CHECK(store->contains("a"));
}

TEST_CASE("damage in a sealed segment refuses to open") {
  TempDir dir;
  write_many(dir.path());
  const std::string oldest = segment_files(dir.path()).front();
  const auto before = read_file(oldest);
  flip_byte(oldest, 40);
  CHECK_THROWS(Store::open(dir.path(), no_hints()), ErrorCode::Corruption);
  // A refused open must not have changed the file or dropped the lock.
  CHECK_EQ(fs::file_size(oldest), before.size());
  flip_byte(oldest, 40);
  CHECK_EQ(Store::open(dir.path(), no_hints())->size(), 20u);
}

TEST_CASE("a torn record in a sealed segment is corruption too") {
  TempDir dir;
  write_many(dir.path());
  const std::string oldest = segment_files(dir.path()).front();
  fs::resize_file(oldest, fs::file_size(oldest) - 3);
  CHECK_THROWS(Store::open(dir.path(), no_hints()), ErrorCode::Corruption);
}

TEST_CASE("salvage keeps what precedes the damage and leaves the file alone") {
  TempDir dir;
  write_many(dir.path());
  const std::string oldest = segment_files(dir.path()).front();
  const std::uint64_t size = fs::file_size(oldest);
  // Second record of the oldest segment: past the header and the first record.
  const std::uint64_t first_record = record_size(std::string("key0").size(), std::string("value0").size());
  flip_byte(oldest, kSegmentHeaderSize + first_record + 1);

  Options options = no_hints();
  options.salvage = true;
  std::size_t kept = 0;
  {
    auto store = Store::open(dir.path(), options);
    const RecoveryReport& report = store->recovery_report();
    REQUIRE(report.salvaged.size() == 1);
    CHECK_EQ(report.salvaged[0].segment_id, 1u);
    CHECK_EQ(report.salvaged[0].damage_offset, kSegmentHeaderSize + first_record);
    CHECK_EQ(report.salvaged[0].skipped_bytes, size - kSegmentHeaderSize - first_record);
    CHECK(store->contains("key0"));
    CHECK(!store->contains("key1"));
    CHECK(store->contains("key19"));
    CHECK_EQ(fs::file_size(oldest), size);
    kept = store->size();
    CHECK(kept < 20);
    store->compact();
  }
  // Compaction replaced the damaged file, so salvage is no longer needed.
  auto store = Store::open(dir.path(), no_hints());
  CHECK_EQ(store->size(), kept);
  CHECK(store->recovery_report().salvaged.empty());
}

TEST_CASE("a bad segment header is fatal, or skipped when salvaging") {
  TempDir dir;
  write_many(dir.path());
  const std::string oldest = segment_files(dir.path()).front();
  flip_byte(oldest, 2);
  CHECK_THROWS(Store::open(dir.path(), no_hints()), ErrorCode::Corruption);

  Options options = no_hints();
  options.salvage = true;
  auto store = Store::open(dir.path(), options);
  CHECK(!store->contains("key0"));
  CHECK(store->contains("key19"));
  REQUIRE(store->recovery_report().salvaged.size() == 1);
  CHECK_EQ(store->recovery_report().salvaged[0].damage_offset, 0u);
}

TEST_CASE("a damaged header on the last segment never receives new writes") {
  TempDir dir;
  write_many(dir.path());
  const std::string newest = segment_files(dir.path()).back();
  flip_byte(newest, 2);
  const std::uint64_t damaged_size = fs::file_size(newest);

  Options options = small_segments(150);
  options.salvage = true;
  {
    auto store = Store::open(dir.path(), options);
    store->put("new", "1");
    CHECK_EQ(store->get("new"), std::optional<std::string>("1"));
    CHECK_EQ(fs::file_size(newest), damaged_size);
  }
  CHECK(Store::open(dir.path(), options)->contains("new"));
}

TEST_CASE("a segment file too short for its header is discarded") {
  TempDir dir;
  {
    auto store = Store::open(dir.path(), fast_options());
    store->put("a", "1");
  }
  write_file(segment_path(dir.path(), 2), std::string(10, 'x'));
  auto store = Store::open(dir.path(), fast_options());
  CHECK_EQ(store->recovery_report().stale_files_removed, 1u);
  CHECK(!fs::exists(segment_path(dir.path(), 2)));
  CHECK_EQ(store->get("a"), std::optional<std::string>("1"));
  CHECK_EQ(store->stats().segments, 1u);
}

TEST_CASE("an empty directory and a header-only segment open cleanly") {
  TempDir dir;
  {
    auto store = Store::open(dir.path(), fast_options());
    CHECK_EQ(store->size(), 0u);
  }
  auto store = Store::open(dir.path(), fast_options());
  CHECK_EQ(store->size(), 0u);
  CHECK(!store->recovery_report().tail_repair.has_value());
  CHECK_EQ(store->stats().segments, 1u);
}

TEST_CASE("temporary files and orphan hints are cleared on open") {
  TempDir dir;
  {
    auto store = Store::open(dir.path(), fast_options());
    store->put("a", "1");
  }
  write_file(segment_path(dir.path(), 9) + ".tmp", "partial");
  write_file(hint_path(dir.path(), 7), "orphan");
  write_file(dir.sub("unrelated.txt"), "keep me");
  auto store = Store::open(dir.path(), fast_options());
  CHECK_EQ(store->recovery_report().stale_files_removed, 2u);
  CHECK(!fs::exists(segment_path(dir.path(), 9) + ".tmp"));
  CHECK(!fs::exists(hint_path(dir.path(), 7)));
  CHECK(fs::exists(dir.sub("unrelated.txt")));
}

TEST_CASE("the highest sequence wins regardless of file order") {
  TempDir dir;
  // File order is deliberately not sequence order, as after a compaction.
  write_file(segment_path(dir.path(), 1),
             header_bytes(1) + rec(10, RecordType::Put, "new-put", "from-1") +
                 rec(20, RecordType::Delete, "deleted"));
  write_file(segment_path(dir.path(), 2),
             header_bytes(2) + rec(5, RecordType::Put, "new-put", "from-2") +
                 rec(15, RecordType::Put, "deleted", "stale") +
                 rec(30, RecordType::Put, "revived", "yes"));
  write_file(segment_path(dir.path(), 3),
             header_bytes(3) + rec(25, RecordType::Delete, "revived") +
                 rec(3, RecordType::Delete, "new-put"));
  auto store = Store::open(dir.path(), fast_options());
  CHECK_EQ(store->get("new-put"), std::optional<std::string>("from-1"));
  CHECK(!store->contains("deleted"));
  CHECK_EQ(store->get("revived"), std::optional<std::string>("yes"));
  CHECK_EQ(store->stats().last_sequence, 30u);
}

TEST_CASE("sequence numbers come from the header when no record has them") {
  TempDir dir;
  write_file(segment_path(dir.path(), 4), header_bytes(4, 1001));
  auto store = Store::open(dir.path(), fast_options());
  CHECK_EQ(store->stats().last_sequence, 1000u);
  store->put("a", "1");
  CHECK_EQ(store->stats().last_sequence, 1001u);
}

TEST_CASE("a header that names another segment is damage") {
  TempDir dir;
  write_file(segment_path(dir.path(), 4), header_bytes(5));
  CHECK_THROWS(Store::open(dir.path(), fast_options()), ErrorCode::Corruption);
}

TEST_CASE("recovery after repeated crash and reopen cycles stays consistent") {
  TempDir dir;
  std::map<std::string, std::string> expected;
  for (int cycle = 0; cycle < 8; ++cycle) {
    auto store = Store::open(dir.path(), small_segments(180));
    CHECK(contents(*store) == expected);
    for (int i = 0; i < 12; ++i) {
      const std::string key = "k" + std::to_string((cycle * 7 + i) % 15);
      store->put(key, "c" + std::to_string(cycle) + "i" + std::to_string(i));
      expected[key] = "c" + std::to_string(cycle) + "i" + std::to_string(i);
    }
    // One more record whose tail is lost to the simulated crash.
    store->put("torn", "this one does not survive");
    store->close();
    const std::string last = segment_files(dir.path()).back();
    fs::resize_file(last, fs::file_size(last) - 5);
  }
}

TEST_MAIN
