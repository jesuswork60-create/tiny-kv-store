#include "check.h"

using namespace tinykv;
using namespace testing;

TEST_CASE("a batch applies all its operations in order") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  store->put("old", "x");

  WriteBatch batch;
  batch.put("a", "1");
  batch.put("b", "2");
  batch.del("old");
  batch.put("a", "3");
  batch.del("b");
  batch.put("b", "4");
  store->write(batch);

  const auto check_state = [](const Store& s) {
    CHECK_EQ(s.get("a"), std::optional<std::string>("3"));
    CHECK_EQ(s.get("b"), std::optional<std::string>("4"));
    CHECK(!s.contains("old"));
    CHECK_EQ(s.size(), 2u);
  };
  check_state(*store);
  store->close();
  check_state(*Store::open(dir.path(), fast_options()));
}

TEST_CASE("an empty batch writes nothing") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  const Stats before = store->stats();
  store->write(WriteBatch{});
  CHECK_EQ(store->stats().total_bytes, before.total_bytes);
  CHECK_EQ(store->stats().last_sequence, before.last_sequence);
}

TEST_CASE("one invalid operation rejects the whole batch") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  WriteBatch batch;
  batch.put("fine", "1");
  batch.put("", "2");
  CHECK_THROWS(store->write(batch), ErrorCode::InvalidArgument);
  CHECK(!store->contains("fine"));
  CHECK_EQ(store->stats().last_sequence, 0u);
}

TEST_CASE("every truncation point leaves a batch whole or absent") {
  TempDir source;
  std::uint64_t start = 0;
  std::uint64_t end = 0;
  {
    auto store = Store::open(source.path(), fast_options());
    store->put("x", "before");
    store->put("untouched", "stays");
    start = store->stats().total_bytes;
    WriteBatch batch;
    batch.put("x", "after");
    batch.put("y", "added");
    batch.del("untouched");
    store->write(batch);
    end = store->stats().total_bytes;
  }
  const std::string segment_name = fs::path(segment_files(source.path()).back()).filename().string();
  const std::string data = read_file(source.path() + "/" + segment_name);
  REQUIRE(data.size() == end);

  for (std::uint64_t cut = start; cut <= end; ++cut) {
    TempDir dir;
    write_file(dir.path() + "/" + segment_name, data.substr(0, static_cast<std::size_t>(cut)));
    auto store = Store::open(dir.path(), fast_options());
    if (cut == end) {
      CHECK_EQ(store->get("x"), std::optional<std::string>("after"));
      CHECK_EQ(store->get("y"), std::optional<std::string>("added"));
      CHECK(!store->contains("untouched"));
      CHECK(!store->recovery_report().tail_repair.has_value());
    } else {
      CHECK_EQ(store->get("x"), std::optional<std::string>("before"));
      CHECK(!store->contains("y"));
      CHECK_EQ(store->get("untouched"), std::optional<std::string>("stays"));
      if (cut == start) {
        CHECK(!store->recovery_report().tail_repair.has_value());
      } else {
        REQUIRE(store->recovery_report().tail_repair.has_value());
        CHECK_EQ(store->recovery_report().tail_repair->truncated_bytes, cut - start);
      }
      CHECK_EQ(store->stats().total_bytes, start);
    }
  }
}

TEST_CASE("the discarded op count is reported for a batch missing its commit") {
  TempDir source;
  std::uint64_t before_commit = 0;
  {
    auto store = Store::open(source.path(), fast_options());
    WriteBatch batch;
    batch.put("a", "1");
    batch.put("b", "2");
    store->write(batch);
    // The commit record is 21 header bytes plus a 4-byte count.
    before_commit = store->stats().total_bytes - (21 + 4);
  }
  const std::string path = segment_files(source.path()).back();
  fs::resize_file(path, before_commit);
  auto store = Store::open(source.path(), fast_options());
  REQUIRE(store->recovery_report().tail_repair.has_value());
  CHECK_EQ(store->recovery_report().tail_repair->discarded_batch_ops, 2u);
  CHECK_EQ(store->size(), 0u);
}

TEST_CASE("a batch is never split across segments") {
  TempDir dir;
  {
    auto store = Store::open(dir.path(), small_segments(150));
    store->put("first", "x");
    WriteBatch batch;
    for (int i = 0; i < 10; ++i) batch.put("batch" + std::to_string(i), "value");
    store->write(batch);
    store->put("last", "y");
    CHECK_EQ(store->stats().segments, 3u);
  }
  // Removing the segment that follows must leave the whole batch intact, and
  // the batch segment alone must walk cleanly as the new last segment.
  fs::remove(segment_files(dir.path()).back());
  auto store = Store::open(dir.path(), small_segments(150));
  CHECK(!store->recovery_report().tail_repair.has_value());
  CHECK_EQ(store->size(), 11u);
}

TEST_CASE("batch sequence numbers continue correctly after reopen") {
  TempDir dir;
  std::uint64_t last = 0;
  {
    auto store = Store::open(dir.path(), fast_options());
    WriteBatch batch;
    batch.put("a", "1");
    batch.del("a");
    store->write(batch);
    last = store->stats().last_sequence;
    CHECK_EQ(last, 2u);  // One per op; the commit record shares the last one.
  }
  auto store = Store::open(dir.path(), fast_options());
  CHECK_EQ(store->stats().last_sequence, last);
  store->put("a", "2");
  store->close();
  CHECK_EQ(Store::open(dir.path(), fast_options())->get("a"), std::optional<std::string>("2"));
}

TEST_CASE("batch garbage accounting counts the commit record") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  WriteBatch batch;
  batch.put("a", "1");
  store->write(batch);
  CHECK_EQ(store->stats().garbage_bytes, 21u + 4u);
}

TEST_MAIN
