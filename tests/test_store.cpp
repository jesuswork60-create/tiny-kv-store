#include <signal.h>
#include <sys/resource.h>

#include "check.h"

using namespace tinykv;
using namespace testing;

TEST_CASE("put get overwrite delete") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  CHECK(!store->get("a").has_value());
  store->put("a", "1");
  CHECK_EQ(store->get("a"), std::optional<std::string>("1"));
  store->put("a", "2");
  CHECK_EQ(store->get("a"), std::optional<std::string>("2"));
  CHECK(store->contains("a"));
  CHECK(store->del("a"));
  CHECK(!store->contains("a"));
  CHECK(!store->get("a").has_value());
  CHECK_EQ(store->size(), 0u);
}

TEST_CASE("deleting an absent key writes nothing") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  const Stats before = store->stats();
  CHECK(!store->del("ghost"));
  const Stats after = store->stats();
  CHECK_EQ(after.total_bytes, before.total_bytes);
  CHECK_EQ(after.last_sequence, before.last_sequence);
}

TEST_CASE("empty values and binary keys are fine") {
  TempDir dir;
  const std::string binary_key("a\0b\xff", 4);
  const std::string binary_value(1000, '\0');
  {
    auto store = Store::open(dir.path(), fast_options());
    store->put("empty", "");
    store->put(binary_key, binary_value);
  }
  auto store = Store::open(dir.path(), fast_options());
  CHECK_EQ(store->get("empty"), std::optional<std::string>(""));
  CHECK_EQ(store->get(binary_key), std::optional<std::string>(binary_value));
}

TEST_CASE("bad arguments are refused without touching the log") {
  TempDir dir;
  Options options = fast_options();
  options.max_value_bytes = 16;
  auto store = Store::open(dir.path(), options);
  const std::uint64_t size_before = store->stats().total_bytes;

  CHECK_THROWS(store->put("", "v"), ErrorCode::InvalidArgument);
  CHECK_THROWS(store->del(""), ErrorCode::InvalidArgument);
  CHECK_THROWS(store->put(std::string(64 * 1024 + 1, 'k'), "v"), ErrorCode::InvalidArgument);
  CHECK_THROWS(store->put("k", std::string(17, 'v')), ErrorCode::InvalidArgument);
  CHECK_EQ(store->stats().total_bytes, size_before);

  store->put(std::string(64 * 1024, 'k'), std::string(16, 'v'));
  CHECK_EQ(store->size(), 1u);
}

TEST_CASE("bad options are refused") {
  TempDir dir;
  Options tiny = fast_options();
  tiny.max_segment_bytes = 10;
  CHECK_THROWS(Store::open(dir.path(), tiny), ErrorCode::InvalidArgument);
  Options no_interval;
  no_interval.sync = SyncMode::EveryN;
  no_interval.sync_every = 0;
  CHECK_THROWS(Store::open(dir.path(), no_interval), ErrorCode::InvalidArgument);
}

TEST_CASE("data survives reopen, deletes included") {
  TempDir dir;
  {
    auto store = Store::open(dir.path(), fast_options());
    store->put("keep", "1");
    store->put("change", "old");
    store->put("change", "new");
    store->put("gone", "x");
    store->del("gone");
  }
  auto store = Store::open(dir.path(), fast_options());
  CHECK_EQ(store->size(), 2u);
  CHECK_EQ(store->get("keep"), std::optional<std::string>("1"));
  CHECK_EQ(store->get("change"), std::optional<std::string>("new"));
  CHECK(!store->contains("gone"));
}

TEST_CASE("segments rotate at the size limit and all stay readable") {
  TempDir dir;
  {
    auto store = Store::open(dir.path(), small_segments(200));
    for (int i = 0; i < 50; ++i) store->put("key" + std::to_string(i), "value" + std::to_string(i));
    CHECK(store->stats().segments > 1);
    CHECK_EQ(store->get("key0"), std::optional<std::string>("value0"));
  }
  for (const std::string& path : segment_files(dir.path())) {
    CHECK(fs::file_size(path) <= 200);
  }
  auto store = Store::open(dir.path(), small_segments(200));
  for (int i = 0; i < 50; ++i) {
    CHECK_EQ(store->get("key" + std::to_string(i)), std::optional<std::string>("value" + std::to_string(i)));
  }
}

TEST_CASE("a record over the limit gets its own segment") {
  TempDir dir;
  auto store = Store::open(dir.path(), small_segments(128));
  store->put("small", "x");
  const std::string big(4096, 'b');
  store->put("big", big);
  store->put("after", "y");
  CHECK_EQ(store->stats().segments, 3u);
  CHECK_EQ(store->get("big"), std::optional<std::string>(big));
}

TEST_CASE("reopening continues the last segment") {
  TempDir dir;
  { Store::open(dir.path(), fast_options())->put("a", "1"); }
  { Store::open(dir.path(), fast_options())->put("b", "2"); }
  auto store = Store::open(dir.path(), fast_options());
  CHECK_EQ(store->stats().segments, 1u);
  CHECK_EQ(store->size(), 2u);
}

TEST_CASE("stats track live data and garbage") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  store->put("key", "value");
  const Stats one = store->stats();
  CHECK_EQ(one.live_keys, 1u);
  CHECK(one.live_bytes > 0);
  CHECK_EQ(one.garbage_bytes, 0u);
  CHECK_EQ(one.last_sequence, 1u);
  CHECK_EQ(one.total_bytes, one.live_bytes + 32);

  store->put("key", "other");
  const Stats two = store->stats();
  CHECK_EQ(two.live_keys, 1u);
  CHECK_EQ(two.live_bytes, one.live_bytes);
  CHECK_EQ(two.garbage_bytes, one.live_bytes);

  store->del("key");
  const Stats three = store->stats();
  CHECK_EQ(three.live_keys, 0u);
  CHECK_EQ(three.live_bytes, 0u);
  CHECK_EQ(three.garbage_bytes, three.total_bytes - 32);
}

TEST_CASE("stats agree before and after reopen") {
  TempDir dir;
  Stats before;
  {
    auto store = Store::open(dir.path(), small_segments(300));
    for (int i = 0; i < 40; ++i) store->put("k" + std::to_string(i % 7), std::string(10 + i, 'v'));
    store->del("k3");
    WriteBatch batch;
    batch.put("batched", "1");
    batch.del("k4");
    store->write(batch);
    before = store->stats();
  }
  auto store = Store::open(dir.path(), small_segments(300));
  const Stats after = store->stats();
  CHECK_EQ(after.live_keys, before.live_keys);
  CHECK_EQ(after.live_bytes, before.live_bytes);
  CHECK_EQ(after.garbage_bytes, before.garbage_bytes);
  CHECK_EQ(after.total_bytes, before.total_bytes);
  CHECK_EQ(after.segments, before.segments);
  CHECK_EQ(after.last_sequence, before.last_sequence);
}

TEST_CASE("all sync modes write durable-format data") {
  for (const SyncMode mode : {SyncMode::Always, SyncMode::EveryN, SyncMode::Never}) {
    TempDir dir;
    Options options;
    options.sync = mode;
    options.sync_every = 2;
    {
      auto store = Store::open(dir.path(), options);
      for (int i = 0; i < 5; ++i) store->put("k" + std::to_string(i), "v");
      store->sync();
    }
    CHECK_EQ(Store::open(dir.path(), options)->size(), 5u);
  }
}

TEST_CASE("a second opener is refused until the first closes") {
  TempDir dir;
  auto first = Store::open(dir.path(), fast_options());
  CHECK_THROWS(Store::open(dir.path(), fast_options()), ErrorCode::Locked);
  first->put("a", "1");
  first->close();
  auto second = Store::open(dir.path(), fast_options());
  CHECK_EQ(second->get("a"), std::optional<std::string>("1"));
}

TEST_CASE("a closed store refuses every operation") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  store->close();
  store->close();  // Harmless.
  CHECK_THROWS(store->put("a", "1"), ErrorCode::Closed);
  CHECK_THROWS(store->get("a"), ErrorCode::Closed);
  CHECK_THROWS(store->del("a"), ErrorCode::Closed);
  CHECK_THROWS(store->scan(), ErrorCode::Closed);
  CHECK_THROWS(store->compact(), ErrorCode::Closed);
  CHECK_THROWS(store->stats(), ErrorCode::Closed);
}

TEST_CASE("create_if_missing off requires an existing directory") {
  TempDir dir;
  Options options = fast_options();
  options.create_if_missing = false;
  CHECK_THROWS(Store::open(dir.sub("nope"), options), ErrorCode::InvalidArgument);
  CHECK(!fs::exists(dir.sub("nope")));
  Options creating = fast_options();
  { Store::open(dir.sub("nested/deeper"), creating); }
  CHECK(fs::is_directory(dir.sub("nested/deeper")));
  CHECK_EQ(Store::open(dir.sub("nested/deeper"), options)->size(), 0u);
}

TEST_CASE("a corrupted value is reported on read, not returned") {
  TempDir dir;
  {
    auto store = Store::open(dir.path(), fast_options());
    store->put("a", "first-value");
    store->put("b", "second-value");
    store->close();
  }
  auto store = Store::open(dir.path(), fast_options());
  const std::string path = segment_files(dir.path()).back();
  // Damage after open has already indexed the record: the first value's last byte.
  flip_byte(path, 32 + 21 + 1 + std::string("first-value").size() - 1);
  CHECK_THROWS(store->get("a"), ErrorCode::Corruption);
  CHECK_EQ(store->get("b"), std::optional<std::string>("second-value"));
}

TEST_CASE("a failed append is rolled back and the store keeps working") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  store->put("before", "1");
  const std::uint64_t size_before = store->stats().total_bytes;

  // Make the kernel refuse writes past a size, partway through the next record.
  signal(SIGXFSZ, SIG_IGN);
  rlimit original{};
  REQUIRE(getrlimit(RLIMIT_FSIZE, &original) == 0);
  rlimit limited = original;
  limited.rlim_cur = static_cast<rlim_t>(size_before + 40);
  REQUIRE(setrlimit(RLIMIT_FSIZE, &limited) == 0);
  CHECK_THROWS(store->put("too-big", std::string(4096, 'x')), ErrorCode::Io);
  REQUIRE(setrlimit(RLIMIT_FSIZE, &original) == 0);

  CHECK(!store->contains("too-big"));
  CHECK_EQ(store->stats().total_bytes, size_before);
  CHECK_EQ(fs::file_size(segment_files(dir.path()).back()), size_before);
  store->put("after", "2");
  store->close();

  auto reopened = Store::open(dir.path(), fast_options());
  CHECK(!reopened->recovery_report().tail_repair.has_value());
  CHECK_EQ(reopened->get("before"), std::optional<std::string>("1"));
  CHECK_EQ(reopened->get("after"), std::optional<std::string>("2"));
}

TEST_MAIN
