#include "check.h"
#include "hint.h"
#include "segment.h"
#include "tinykv/inspect.h"

using namespace tinykv;
using namespace testing;

namespace {

void write_sample(const std::string& dir) {
  auto store = Store::open(dir, small_segments(200));
  for (int i = 0; i < 20; ++i) store->put("key" + std::to_string(i), "value" + std::to_string(i));
  store->del("key1");
  WriteBatch batch;
  batch.put("batched", "x");
  store->write(batch);
}

bool has_issue(const VerifyReport& report, VerifyIssue::Severity severity) {
  return std::any_of(report.issues.begin(), report.issues.end(),
                     [&](const VerifyIssue& issue) { return issue.severity == severity; });
}

}  // namespace

TEST_CASE("verify passes a healthy store") {
  TempDir dir;
  write_sample(dir.path());
  const VerifyReport report = verify_store(dir.path());
  CHECK(report.ok());
  CHECK(report.issues.empty());
  CHECK_EQ(report.segments, segment_files(dir.path()).size());
  CHECK_EQ(report.records, 20u + 1u + 2u);
  CHECK_EQ(report.live_keys, 20u);
  CHECK(report.hints_checked > 0);
}

TEST_CASE("verify changes nothing") {
  TempDir dir;
  write_sample(dir.path());
  const std::string last = segment_files(dir.path()).back();
  fs::resize_file(last, fs::file_size(last) - 2);
  const std::uint64_t size = fs::file_size(last);
  verify_store(dir.path());
  CHECK_EQ(fs::file_size(last), size);
}

TEST_CASE("verify treats a torn tail as a warning") {
  TempDir dir;
  write_sample(dir.path());
  const std::string last = segment_files(dir.path()).back();
  fs::resize_file(last, fs::file_size(last) - 2);
  const VerifyReport report = verify_store(dir.path());
  CHECK(report.ok());
  CHECK(has_issue(report, VerifyIssue::Severity::Warning));
  CHECK(!has_issue(report, VerifyIssue::Severity::Error));
}

TEST_CASE("verify flags damage in a sealed segment, past any valid hint") {
  TempDir dir;
  write_sample(dir.path());
  flip_byte(segment_files(dir.path()).front(), kSegmentHeaderSize + 1);
  CHECK(Store::open(dir.path(), small_segments(200))->size() > 0);  // The hint hides it from open.
  const VerifyReport report = verify_store(dir.path());
  CHECK(!report.ok());
  REQUIRE(!report.issues.empty());
  CHECK_EQ(report.issues[0].segment_id, 1u);
}

TEST_CASE("verify flags a bad segment header") {
  TempDir dir;
  write_sample(dir.path());
  flip_byte(segment_files(dir.path()).front(), 0);
  CHECK(!verify_store(dir.path()).ok());
}

TEST_CASE("verify warns about a stale hint and leftover files") {
  TempDir dir;
  write_sample(dir.path());
  flip_byte(files_with_extension(dir.path(), ".hint").front(), 12);
  write_file(segment_path(dir.path(), 77) + ".tmp", "partial");
  write_file(hint_path(dir.path(), 88), "orphan");
  const VerifyReport report = verify_store(dir.path());
  CHECK(report.ok());
  CHECK_EQ(report.issues.size(), 3u);
}

TEST_CASE("verify catches a valid-looking hint that disagrees with its segment") {
  TempDir dir;
  write_sample(dir.path());
  const std::string oldest = segment_files(dir.path()).front();
  const std::string hint = files_with_extension(dir.path(), ".hint").front();

  // Well-formed and matching in id and size, so open would trust it, but it
  // names a key the segment never held.
  HintBuilder forged;
  forged.add(SegmentOp{1, true, "forged-key", kSegmentHeaderSize, 1});
  write_file(hint, forged.finish(1, fs::file_size(oldest)));

  const VerifyReport report = verify_store(dir.path());
  CHECK(!report.ok());
  CHECK(has_issue(report, VerifyIssue::Severity::Error));
}

TEST_CASE("verify refuses a store that is open") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  CHECK_THROWS(verify_store(dir.path()), ErrorCode::Locked);
  store->close();
  CHECK(verify_store(dir.path()).ok());
}

TEST_CASE("dump works without the lock") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  store->put("a", "1");
  std::ostringstream out;
  dump_store(dir.path(), out);
  CHECK(out.str().find("seq=1 put key=a") != std::string::npos);
}

TEST_CASE("verify and dump refuse a missing directory") {
  TempDir dir;
  std::ostringstream out;
  CHECK_THROWS(verify_store(dir.sub("nope")), ErrorCode::InvalidArgument);
  CHECK_THROWS(dump_store(dir.sub("nope"), out), ErrorCode::InvalidArgument);
  CHECK(!fs::exists(dir.sub("nope")));
}

TEST_CASE("dump lists every record, including superseded ones") {
  TempDir dir;
  {
    auto store = Store::open(dir.path(), fast_options());
    store->put("a", "one");
    store->put("a", "two");
    store->del("a");
    WriteBatch batch;
    batch.put("b\x01", "x");
    store->write(batch);
  }
  std::ostringstream plain;
  dump_store(dir.path(), plain);
  const std::string text = plain.str();
  CHECK(text.find("segment 1:") != std::string::npos);
  CHECK(text.find("seq=1 put key=a value_len=3") != std::string::npos);
  CHECK(text.find("seq=3 del key=a") != std::string::npos);
  CHECK(text.find("seq=4 batch-put key=b\\x01") != std::string::npos);
  CHECK(text.find("seq=4 batch-commit") != std::string::npos);

  std::ostringstream with_values;
  DumpOptions options;
  options.show_values = true;
  dump_store(dir.path(), with_values, options);
  CHECK(with_values.str().find("value=two") != std::string::npos);
}

TEST_CASE("dump marks where a segment stops being readable") {
  TempDir dir;
  {
    auto store = Store::open(dir.path(), fast_options());
    store->put("a", "1");
    store->put("b", "2");
  }
  const std::string seg = segment_files(dir.path()).back();
  fs::resize_file(seg, fs::file_size(seg) - 1);
  std::ostringstream torn;
  dump_store(dir.path(), torn);
  CHECK(torn.str().find("torn record") != std::string::npos);

  flip_byte(seg, kSegmentHeaderSize + 5);
  std::ostringstream corrupt;
  dump_store(dir.path(), corrupt);
  CHECK(corrupt.str().find("corrupt record") != std::string::npos);

  flip_byte(seg, 0);
  std::ostringstream bad_header;
  dump_store(dir.path(), bad_header);
  CHECK(bad_header.str().find("bad header") != std::string::npos);
}

TEST_MAIN
