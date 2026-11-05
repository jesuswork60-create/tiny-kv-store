#include "check.h"
#include "hint.h"

using namespace tinykv;
using namespace testing;

namespace {

Options with_hints() { return small_segments(200); }

void write_sealed_data(const std::string& dir) {
  auto store = Store::open(dir, with_hints());
  for (int i = 0; i < 30; ++i) store->put("k" + std::to_string(i % 20), "v" + std::to_string(i));
  store->del("k3");
  WriteBatch batch;
  batch.put("batched", "yes");
  batch.del("k4");
  store->write(batch);
  REQUIRE(store->stats().segments >= 3);
}

}  // namespace

TEST_CASE("hint builder round trip") {
  HintBuilder builder;
  CHECK(builder.empty());
  builder.add(SegmentOp{5, true, "alpha", 32, 7});
  builder.add(SegmentOp{9, false, "beta", 100, 0});
  CHECK(!builder.empty());
  const std::string data = builder.finish(3, 500);

  std::vector<std::string> keys;
  std::uint64_t max_seq = 0;
  std::uint64_t alpha_offset = 0;
  std::uint32_t alpha_len = 0;
  const bool loaded = load_hint(
      data, 3, 500,
      [&](const SegmentOp& op) {
        keys.emplace_back(op.key);
        if (op.is_put) {
          alpha_offset = op.offset;
          alpha_len = op.value_len;
        }
      },
      &max_seq);
  REQUIRE(loaded);
  REQUIRE(keys.size() == 2);
  CHECK_EQ(keys[0], std::string("alpha"));
  CHECK_EQ(keys[1], std::string("beta"));
  CHECK_EQ(max_seq, 9u);
  CHECK_EQ(alpha_offset, 32u);
  CHECK_EQ(alpha_len, 7u);
}

TEST_CASE("a hint for another segment or size is refused") {
  HintBuilder builder;
  builder.add(SegmentOp{1, true, "a", 32, 1});
  const std::string data = builder.finish(3, 500);
  std::uint64_t max_seq = 0;
  const auto ignore = [](const SegmentOp&) {};
  CHECK(load_hint(data, 3, 500, ignore, &max_seq));
  CHECK(!load_hint(data, 4, 500, ignore, &max_seq));
  CHECK(!load_hint(data, 3, 501, ignore, &max_seq));
  CHECK(!load_hint(std::string_view(data).substr(0, data.size() - 1), 3, 500, ignore, &max_seq));
  CHECK(!load_hint("", 3, 500, ignore, &max_seq));
}

TEST_CASE("any damaged byte makes the hint invalid without visiting entries") {
  HintBuilder builder;
  builder.add(SegmentOp{1, true, "alpha", 32, 1});
  const std::string data = builder.finish(3, 500);
  for (std::size_t i = 0; i < data.size(); ++i) {
    std::string bad = data;
    bad[i] = static_cast<char>(bad[i] ^ 0x20);
    std::uint64_t max_seq = 0;
    int visited = 0;
    CHECK(!load_hint(bad, 3, 500, [&](const SegmentOp&) { ++visited; }, &max_seq));
    CHECK_EQ(visited, 0);
  }
}

TEST_CASE("open uses hints for sealed segments and scans only the last") {
  TempDir dir;
  write_sealed_data(dir.path());
  const std::size_t segments = segment_files(dir.path()).size();
  CHECK_EQ(files_with_extension(dir.path(), ".hint").size(), segments - 1);

  auto store = Store::open(dir.path(), with_hints());
  const RecoveryReport& report = store->recovery_report();
  CHECK_EQ(report.segments_from_hints, segments - 1);
  CHECK_EQ(report.segments_scanned, 1u);
  CHECK_EQ(report.hints_rejected, 0u);
}

TEST_CASE("hint-based and scan-based recovery agree exactly") {
  TempDir dir;
  write_sealed_data(dir.path());
  std::map<std::string, std::string> from_hints;
  Stats hint_stats;
  {
    auto store = Store::open(dir.path(), with_hints());
    from_hints = contents(*store);
    hint_stats = store->stats();
  }
  Options no_hints = with_hints();
  no_hints.use_hints = false;
  auto store = Store::open(dir.path(), no_hints);
  CHECK_EQ(store->recovery_report().segments_from_hints, 0u);
  CHECK(contents(*store) == from_hints);
  const Stats scan_stats = store->stats();
  CHECK_EQ(scan_stats.live_bytes, hint_stats.live_bytes);
  CHECK_EQ(scan_stats.garbage_bytes, hint_stats.garbage_bytes);
  CHECK(from_hints.count("batched") == 1);
  CHECK(from_hints.count("k3") == 0);
  CHECK(from_hints.count("k4") == 0);
}

TEST_CASE("a damaged hint falls back to scanning") {
  TempDir dir;
  write_sealed_data(dir.path());
  std::map<std::string, std::string> expected;
  { expected = contents(*Store::open(dir.path(), with_hints())); }

  const std::string hint = files_with_extension(dir.path(), ".hint").front();
  flip_byte(hint, 12);
  auto store = Store::open(dir.path(), with_hints());
  CHECK_EQ(store->recovery_report().hints_rejected, 1u);
  CHECK(store->recovery_report().segments_scanned >= 2);
  CHECK(contents(*store) == expected);
}

TEST_CASE("a hint is ignored when the segment size no longer matches") {
  TempDir dir;
  write_sealed_data(dir.path());
  const std::string hint = files_with_extension(dir.path(), ".hint").front();
  const std::string oldest = segment_files(dir.path()).front();
  CHECK_EQ(fs::path(hint).stem(), fs::path(oldest).stem());

  // A sealed segment that lost its tail: the stale hint must not paper over it.
  fs::resize_file(oldest, fs::file_size(oldest) - 1);
  CHECK_THROWS(Store::open(dir.path(), with_hints()), ErrorCode::Corruption);
}

TEST_CASE("no hints are written when they are turned off") {
  TempDir dir;
  Options options = with_hints();
  options.use_hints = false;
  {
    auto store = Store::open(dir.path(), options);
    for (int i = 0; i < 30; ++i) store->put("k" + std::to_string(i), "value");
    store->compact();
    for (int i = 0; i < 30; ++i) store->put("k" + std::to_string(i), "again");
  }
  CHECK(files_with_extension(dir.path(), ".hint").empty());
}

TEST_CASE("the last segment's old hint is dropped when writes continue in it") {
  TempDir dir;
  write_sealed_data(dir.path());
  // Simulate a crash after sealing, before the next segment appeared.
  const std::vector<std::string> segments = segment_files(dir.path());
  fs::remove(segments.back());
  const std::string new_last = segments[segments.size() - 2];
  const std::string its_hint = fs::path(new_last).replace_extension(".hint").string();
  CHECK(fs::exists(its_hint));
  {
    auto store = Store::open(dir.path(), with_hints());
    CHECK(!fs::exists(its_hint));
    store->put("appended", "1");
  }
  auto store = Store::open(dir.path(), with_hints());
  CHECK_EQ(store->get("appended"), std::optional<std::string>("1"));
}

TEST_CASE("values are still read from segments when a hint loaded them") {
  TempDir dir;
  write_sealed_data(dir.path());
  auto store = Store::open(dir.path(), with_hints());
  CHECK_EQ(store->get("batched"), std::optional<std::string>("yes"));
  CHECK_EQ(store->get("k19"), std::optional<std::string>("v19"));
  CHECK_EQ(store->get("k0"), std::optional<std::string>("v20"));
}

TEST_MAIN
