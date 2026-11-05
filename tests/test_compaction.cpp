#include <sys/stat.h>

#include "check.h"
#include "segment.h"

using namespace tinykv;
using namespace testing;

TEST_CASE("compaction reclaims garbage and keeps the data") {
  TempDir dir;
  auto store = Store::open(dir.path(), small_segments(300));
  for (int round = 0; round < 5; ++round) {
    for (int i = 0; i < 20; ++i) store->put("k" + std::to_string(i), "round" + std::to_string(round));
  }
  store->del("k0");
  const Stats before = store->stats();
  CHECK(before.garbage_bytes > 0);

  const CompactionStats result = store->compact();
  const Stats after = store->stats();
  CHECK_EQ(after.garbage_bytes, 0u);
  CHECK_EQ(after.live_keys, 19u);
  CHECK(after.total_bytes < before.total_bytes);
  CHECK_EQ(result.keys_written, 19u);
  CHECK_EQ(result.bytes_before, before.total_bytes);
  CHECK(result.segments_written >= 2);  // 19 records do not fit in one 300-byte file.
  CHECK_EQ(after.compactions, 1u);

  CHECK(!store->contains("k0"));
  for (int i = 1; i < 20; ++i) {
    CHECK_EQ(store->get("k" + std::to_string(i)), std::optional<std::string>("round4"));
  }
}

TEST_CASE("compacted data survives reopen with and without hints") {
  TempDir dir;
  {
    auto store = Store::open(dir.path(), small_segments(300));
    for (int i = 0; i < 30; ++i) store->put("k" + std::to_string(i), "v" + std::to_string(i));
    for (int i = 0; i < 10; ++i) store->del("k" + std::to_string(i));
    store->compact();
    store->put("after", "compaction");
  }
  for (const bool hints : {true, false}) {
    Options options = small_segments(300);
    options.use_hints = hints;
    auto store = Store::open(dir.path(), options);
    CHECK_EQ(store->size(), 21u);
    CHECK_EQ(store->get("k15"), std::optional<std::string>("v15"));
    CHECK(!store->contains("k5"));
    CHECK_EQ(store->stats().garbage_bytes, 0u);
    store->close();
  }
}

TEST_CASE("compacting an empty or all-deleted store leaves one fresh segment") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  store->compact();
  CHECK_EQ(store->stats().segments, 1u);

  store->put("a", "1");
  store->del("a");
  const std::uint64_t last = store->stats().last_sequence;
  const CompactionStats result = store->compact();
  CHECK_EQ(result.keys_written, 0u);
  CHECK_EQ(result.segments_written, 0u);
  CHECK_EQ(store->stats().total_bytes, kSegmentHeaderSize);
  store->close();

  // No record remains, yet the sequence must not restart.
  auto reopened = Store::open(dir.path(), fast_options());
  CHECK_EQ(reopened->size(), 0u);
  CHECK_EQ(reopened->stats().last_sequence, last);
}

TEST_CASE("batch records compact into plain records") {
  TempDir dir;
  {
    auto store = Store::open(dir.path(), fast_options());
    WriteBatch batch;
    batch.put("a", "1");
    batch.put("b", "2");
    store->write(batch);
    store->compact();
    CHECK_EQ(store->stats().garbage_bytes, 0u);
  }
  auto store = Store::open(dir.path(), fast_options());
  CHECK_EQ(store->get("a"), std::optional<std::string>("1"));
  CHECK_EQ(store->get("b"), std::optional<std::string>("2"));
}

TEST_CASE("repeated compaction with writes in between stays correct") {
  TempDir dir;
  std::map<std::string, std::string> expected;
  auto store = Store::open(dir.path(), small_segments(400));
  for (int round = 0; round < 6; ++round) {
    for (int i = 0; i < 25; ++i) {
      const std::string key = "k" + std::to_string((round * 3 + i) % 20);
      if ((i + round) % 5 == 0) {
        store->del(key);
        expected.erase(key);
      } else {
        store->put(key, "r" + std::to_string(round));
        expected[key] = "r" + std::to_string(round);
      }
    }
    store->compact();
    CHECK(contents(*store) == expected);
  }
  store->close();
  store = Store::open(dir.path(), small_segments(400));
  CHECK(contents(*store) == expected);
}

TEST_CASE("a crash at any point of old-segment deletion loses and revives nothing") {
  TempDir base;
  const std::string before_dir = base.sub("before");
  const std::string live_dir = base.sub("live");

  // Old segments hold puts, overwrites and tombstones for puts in older files.
  std::map<std::string, std::string> expected;
  {
    auto store = Store::open(live_dir, small_segments(220));
    for (int i = 0; i < 12; ++i) store->put("k" + std::to_string(i), "first");
    for (int i = 0; i < 6; ++i) store->put("k" + std::to_string(i), "second");
    for (int i = 6; i < 12; ++i) store->del("k" + std::to_string(i));
    expected = contents(*store);
    store->sync();
    store->close();
  }
  copy_directory(live_dir, before_dir);
  {
    auto store = Store::open(live_dir, small_segments(220));
    store->compact();
    store->close();
  }

  // State right after the compaction's renames: old files plus the outputs.
  const std::vector<std::string> old_files = segment_files(before_dir);
  REQUIRE(old_files.size() >= 3);
  for (std::size_t deleted = 0; deleted <= old_files.size(); ++deleted) {
    TempDir crashed;
    copy_directory(before_dir, crashed.path());
    copy_directory(live_dir, crashed.path());
    for (std::size_t i = 0; i < deleted; ++i) {
      fs::remove(crashed.path() + "/" + fs::path(old_files[i]).filename().string());
    }
    for (const bool hints : {false, true}) {
      TempDir attempt;
      copy_directory(crashed.path(), attempt.path());
      Options options = small_segments(220);
      options.use_hints = hints;
      auto store = Store::open(attempt.path(), options);
      CHECK(contents(*store) == expected);

      // The interrupted state must also be able to finish a new compaction.
      store->compact();
      CHECK(contents(*store) == expected);
      CHECK_EQ(store->stats().garbage_bytes, 0u);
    }
  }
}

TEST_CASE("a crash while outputs are being renamed loses and revives nothing") {
  TempDir base;
  const std::string before_dir = base.sub("before");
  const std::string live_dir = base.sub("live");
  std::map<std::string, std::string> expected;
  {
    auto store = Store::open(live_dir, small_segments(220));
    for (int i = 0; i < 20; ++i) store->put("k" + std::to_string(i), "first");
    for (int i = 10; i < 20; ++i) store->del("k" + std::to_string(i));
    expected = contents(*store);
    store->close();
  }
  copy_directory(live_dir, before_dir);
  const std::size_t old_count = segment_files(before_dir).size();
  {
    auto store = Store::open(live_dir, small_segments(220));
    store->compact();
    store->close();
  }

  // Outputs have higher ids than the old files. Add them one at a time; the
  // highest one each time plays the unfinished last segment.
  std::vector<std::string> outputs;
  for (const std::string& path : segment_files(live_dir)) {
    if (!fs::exists(before_dir + "/" + fs::path(path).filename().string())) outputs.push_back(path);
  }
  REQUIRE(outputs.size() >= 2);
  for (std::size_t present = 1; present <= outputs.size(); ++present) {
    TempDir crashed;
    copy_directory(before_dir, crashed.path());
    for (std::size_t i = 0; i < present; ++i) {
      fs::copy_file(outputs[i], crashed.path() + "/" + fs::path(outputs[i]).filename().string());
    }
    CHECK_EQ(segment_files(crashed.path()).size(), old_count + present);
    auto store = Store::open(crashed.path(), small_segments(220));
    CHECK(contents(*store) == expected);
    store->put("after-crash", "1");
    store->close();
    auto reopened = Store::open(crashed.path(), small_segments(220));
    CHECK_EQ(reopened->get("after-crash"), std::optional<std::string>("1"));
    CHECK_EQ(reopened->size(), expected.size() + 1);
  }
}

TEST_CASE("deleting a tombstone before the put it covers would revive the key") {
  // Documents why deletion runs oldest first: this is the state the opposite
  // order would pass through, and it really does bring the key back.
  TempDir dir;
  {
    auto store = Store::open(dir.path(), small_segments(100));
    store->put("victim", "value-long-enough-to-rotate-the-segment");
    store->put("filler", "value-long-enough-to-rotate-the-segment");
    store->del("victim");
  }
  const std::vector<std::string> files = segment_files(dir.path());
  REQUIRE(files.size() >= 2);
  fs::remove(files.back());  // Holds the tombstone.
  CHECK(Store::open(dir.path(), small_segments(100))->contains("victim"));
}

TEST_CASE("a crash before the rename leaves only a temp file behind") {
  TempDir dir;
  std::map<std::string, std::string> expected;
  {
    auto store = Store::open(dir.path(), small_segments(200));
    for (int i = 0; i < 10; ++i) store->put("k" + std::to_string(i), "v");
    expected = contents(*store);
  }
  write_file(segment_path(dir.path(), 99) + ".tmp", "half-written compaction output");
  auto store = Store::open(dir.path(), small_segments(200));
  CHECK_EQ(store->recovery_report().stale_files_removed, 1u);
  CHECK(contents(*store) == expected);
}

TEST_CASE("a corrupt live record aborts compaction with nothing changed") {
  TempDir dir;
  auto store = Store::open(dir.path(), small_segments(200));
  for (int i = 0; i < 20; ++i) store->put("k" + std::to_string(100 + i), "value" + std::to_string(i));
  const std::vector<std::string> files_before = segment_files(dir.path());
  const std::uint64_t keys_before = store->size();

  // Damage the last value only, so compaction fails after writing outputs.
  const std::string last = files_before.back();
  flip_byte(last, fs::file_size(last) - 1);
  CHECK_THROWS(store->compact(), ErrorCode::Corruption);

  CHECK_EQ(segment_files(dir.path()).size(), files_before.size());
  CHECK(files_with_extension(dir.path(), ".tmp").empty());
  CHECK_EQ(store->size(), keys_before);
  CHECK_EQ(store->get("k100"), std::optional<std::string>("value0"));

  // Writes continue, in a new segment, and the good data can still be read.
  store->put("after-failure", "ok");
  CHECK_EQ(store->get("after-failure"), std::optional<std::string>("ok"));
  CHECK_THROWS(store->get("k119"), ErrorCode::Corruption);

  // Deleting the damaged key lets compaction through again.
  CHECK(store->del("k119"));
  store->compact();
  CHECK_EQ(store->get("k100"), std::optional<std::string>("value0"));
  CHECK(!store->contains("k119"));
}

TEST_CASE("an output directory that cannot be written fails without losing data") {
  if (geteuid() == 0) return;  // Root ignores directory permissions.
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  store->put("a", "1");
  store->put("b", "2");
  REQUIRE(chmod(dir.path().c_str(), 0500) == 0);
  CHECK_THROWS(store->compact(), ErrorCode::Io);
  REQUIRE(chmod(dir.path().c_str(), 0700) == 0);

  CHECK_EQ(store->get("a"), std::optional<std::string>("1"));
  store->put("c", "3");
  store->close();
  auto reopened = Store::open(dir.path(), fast_options());
  CHECK_EQ(reopened->size(), 3u);
}

TEST_MAIN
