#include "check.h"
#include "coding.h"
#include "file.h"
#include "segment.h"

using namespace tinykv;
using namespace testing;

namespace {

std::string header_bytes(std::uint64_t id = 1, std::uint64_t base_seq = 1) {
  SegmentHeader header;
  header.id = id;
  header.base_seq = base_seq;
  return encode_segment_header(header);
}

std::string commit_of(std::uint64_t seq, std::uint32_t count) {
  std::string value;
  put_u32(value, count);
  std::string encoded;
  encode_record(encoded, seq, RecordType::BatchCommit, {}, value);
  return encoded;
}

std::string rec(std::uint64_t seq, RecordType type, std::string_view key, std::string_view value = {}) {
  std::string encoded;
  encode_record(encoded, seq, type, key, value);
  return encoded;
}

struct Walked {
  WalkResult result;
  std::vector<std::string> keys;
};

Walked walk_bytes(const TempDir& dir, const std::string& data) {
  const std::string path = dir.sub("walk.bin");
  write_file(path, data);
  const RandomFile file(path);
  Walked out;
  out.result = walk_segment(file, file.size(), [&](const SegmentOp& op) {
    out.keys.push_back((op.is_put ? "+" : "-") + std::string(op.key));
  });
  return out;
}

}  // namespace

TEST_CASE("segment header round trip") {
  TempDir dir;
  const std::string bytes = header_bytes(77, 1234);
  CHECK_EQ(bytes.size(), kSegmentHeaderSize);
  write_file(dir.sub("a"), bytes);
  const RandomFile file(dir.sub("a"));
  SegmentHeader header;
  REQUIRE(read_segment_header(file, file.size(), &header));
  CHECK_EQ(header.id, 77u);
  CHECK_EQ(header.base_seq, 1234u);
}

TEST_CASE("damaged or short headers are refused") {
  TempDir dir;
  const std::string good = header_bytes();
  for (std::size_t i = 0; i < good.size(); ++i) {
    std::string bad = good;
    bad[i] = static_cast<char>(bad[i] ^ 0x10);
    write_file(dir.sub("h"), bad);
    const RandomFile file(dir.sub("h"));
    SegmentHeader header;
    CHECK(!read_segment_header(file, file.size(), &header));
  }
  write_file(dir.sub("short"), good.substr(0, 10));
  const RandomFile file(dir.sub("short"));
  SegmentHeader header;
  CHECK(!read_segment_header(file, file.size(), &header));
}

TEST_CASE("directory listing sorts ids and ignores foreign files") {
  TempDir dir;
  write_file(segment_path(dir.path(), 12), "");
  write_file(segment_path(dir.path(), 3), "");
  write_file(hint_path(dir.path(), 3), "");
  write_file(segment_path(dir.path(), 5) + ".tmp", "");
  write_file(dir.sub("LOCK"), "");
  write_file(dir.sub("notes.seg"), "");
  write_file(dir.sub("12x.hint"), "");

  const DirectoryListing listing = list_directory(dir.path());
  REQUIRE(listing.segments.size() == 2);
  CHECK_EQ(listing.segments[0], 3u);
  CHECK_EQ(listing.segments[1], 12u);
  CHECK_EQ(listing.hints.size(), 1u);
  CHECK_EQ(listing.temp_files.size(), 1u);
}

TEST_CASE("scanner reads records bigger than its read chunk") {
  TempDir dir;
  const std::string big(3 * 1024 * 1024, 'x');
  std::string data = header_bytes();
  data += rec(1, RecordType::Put, "small", "v");
  data += rec(2, RecordType::Put, "big", big);
  data += rec(3, RecordType::Put, "after", "w");
  write_file(dir.sub("s"), data);

  const RandomFile file(dir.sub("s"));
  SegmentScanner scanner(file, file.size());
  Record record;
  REQUIRE(scanner.next(&record) == SegmentScanner::Status::Ok);
  CHECK_EQ(record.key, std::string_view("small"));
  REQUIRE(scanner.next(&record) == SegmentScanner::Status::Ok);
  CHECK_EQ(record.value.size(), big.size());
  CHECK(record.value == big);
  REQUIRE(scanner.next(&record) == SegmentScanner::Status::Ok);
  CHECK_EQ(record.value, std::string_view("w"));
  CHECK(scanner.next(&record) == SegmentScanner::Status::End);
}

TEST_CASE("scanner distinguishes torn from corrupt") {
  TempDir dir;
  const std::string one = rec(1, RecordType::Put, "alpha", "beta");
  const std::string data = header_bytes() + one;

  write_file(dir.sub("torn"), data.substr(0, data.size() - 1));
  {
    const RandomFile file(dir.sub("torn"));
    SegmentScanner scanner(file, file.size());
    Record record;
    CHECK(scanner.next(&record) == SegmentScanner::Status::Torn);
    CHECK_EQ(scanner.offset(), kSegmentHeaderSize);
  }
  write_file(dir.sub("short-header"), data.substr(0, kSegmentHeaderSize + 3));
  {
    const RandomFile file(dir.sub("short-header"));
    SegmentScanner scanner(file, file.size());
    Record record;
    CHECK(scanner.next(&record) == SegmentScanner::Status::Torn);
  }
  std::string flipped = data;
  flipped.back() = static_cast<char>(flipped.back() ^ 1);
  write_file(dir.sub("flip"), flipped);
  {
    const RandomFile file(dir.sub("flip"));
    SegmentScanner scanner(file, file.size());
    Record record;
    CHECK(scanner.next(&record) == SegmentScanner::Status::Corrupt);
  }
}

TEST_CASE("walk reports plain writes in order") {
  TempDir dir;
  const Walked w = walk_bytes(dir, header_bytes() + rec(1, RecordType::Put, "a", "1") +
                                       rec(2, RecordType::Delete, "a"));
  CHECK(w.result.end == WalkEnd::Clean);
  CHECK_EQ(w.result.records, 2u);
  REQUIRE(w.keys.size() == 2);
  CHECK_EQ(w.keys[0], std::string("+a"));
  CHECK_EQ(w.keys[1], std::string("-a"));
}

TEST_CASE("walk holds batch ops back until the commit") {
  TempDir dir;
  const std::string before = header_bytes() + rec(1, RecordType::Put, "plain", "1");
  const std::string batch = rec(2, RecordType::BatchPut, "x", "1") + rec(3, RecordType::BatchDelete, "y");

  const Walked committed = walk_bytes(dir, before + batch + commit_of(4, 2));
  CHECK(committed.result.end == WalkEnd::Clean);
  CHECK_EQ(committed.keys.size(), 3u);

  const Walked open_ended = walk_bytes(dir, before + batch);
  CHECK(open_ended.result.end == WalkEnd::UncommittedBatch);
  CHECK_EQ(open_ended.keys.size(), 1u);
  CHECK_EQ(open_ended.result.valid_end, before.size());
  CHECK_EQ(open_ended.result.discarded_ops, 2u);
}

TEST_CASE("walk rejects impossible batch framing") {
  TempDir dir;
  const std::string head = header_bytes();
  const std::string one = rec(2, RecordType::BatchPut, "x", "1");

  const Walked lone_commit = walk_bytes(dir, head + commit_of(1, 0));
  CHECK(lone_commit.result.end == WalkEnd::BadFraming);

  const Walked wrong_count = walk_bytes(dir, head + one + commit_of(3, 2));
  CHECK(wrong_count.result.end == WalkEnd::BadFraming);
  CHECK(wrong_count.keys.empty());
  CHECK_EQ(wrong_count.result.valid_end, kSegmentHeaderSize);

  const Walked interleaved = walk_bytes(dir, head + one + rec(3, RecordType::Put, "y", "2"));
  CHECK(interleaved.result.end == WalkEnd::BadFraming);
  CHECK(interleaved.keys.empty());
}

TEST_CASE("walk stops at damage and keeps what came before") {
  TempDir dir;
  const std::string first = header_bytes() + rec(1, RecordType::Put, "a", "1");
  std::string second = rec(2, RecordType::Put, "b", "2");
  second.back() = static_cast<char>(second.back() ^ 1);
  const Walked w = walk_bytes(dir, first + second + rec(3, RecordType::Put, "c", "3"));
  CHECK(w.result.end == WalkEnd::CorruptRecord);
  CHECK_EQ(w.result.damage_offset, first.size());
  CHECK_EQ(w.result.valid_end, first.size());
  CHECK_EQ(w.keys.size(), 1u);
}

TEST_CASE("reader validates the record it is pointed at") {
  TempDir dir;
  const std::string first = rec(5, RecordType::Put, "key", "value");
  write_file(dir.sub("r"), header_bytes() + first);
  const SegmentReader reader(dir.sub("r"), 1);

  CHECK_EQ(reader.read_value(kSegmentHeaderSize, "key", 5, 5), std::string("value"));
  CHECK_THROWS(reader.read_value(kSegmentHeaderSize, "kez", 5, 5), ErrorCode::Corruption);
  CHECK_THROWS(reader.read_value(kSegmentHeaderSize, "key", 6, 5), ErrorCode::Corruption);
  CHECK_THROWS(reader.read_value(kSegmentHeaderSize, "key", 5, 4), ErrorCode::Corruption);
  CHECK_THROWS(reader.read_value(kSegmentHeaderSize + 1000, "key", 5, 5), ErrorCode::Corruption);
}

TEST_CASE("read_value refuses a tombstone") {
  TempDir dir;
  write_file(dir.sub("r"), header_bytes() + rec(5, RecordType::Delete, "key"));
  const SegmentReader reader(dir.sub("r"), 1);
  CHECK_THROWS(reader.read_value(kSegmentHeaderSize, "key", 5, 0), ErrorCode::Corruption);
}

TEST_MAIN
