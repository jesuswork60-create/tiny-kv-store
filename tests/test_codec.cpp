#include <limits>

#include "check.h"
#include "coding.h"
#include "crc32.h"
#include "record.h"

using namespace tinykv;

TEST_CASE("crc32 matches published check values") {
  CHECK_EQ(crc32(std::string_view("123456789")), 0xCBF43926u);
  CHECK_EQ(crc32(std::string_view("")), 0u);
  CHECK_EQ(crc32(std::string_view("The quick brown fox jumps over the lazy dog")), 0x414FA339u);
}

TEST_CASE("crc32 can be computed in pieces") {
  const std::string text = "log structured storage";
  for (std::size_t split = 0; split <= text.size(); ++split) {
    const std::uint32_t head = crc32(std::string_view(text).substr(0, split));
    CHECK_EQ(crc32(std::string_view(text).substr(split), head), crc32(text));
  }
}

TEST_CASE("integers round trip and are little endian") {
  std::string out;
  put_u32(out, 0x04030201u);
  put_u64(out, 0x0807060504030201ull);
  REQUIRE(out.size() == 12);
  CHECK_EQ(static_cast<int>(out[0]), 1);
  CHECK_EQ(static_cast<int>(out[4]), 1);
  CHECK_EQ(static_cast<int>(out[11]), 8);
  CHECK_EQ(get_u32(out.data()), 0x04030201u);
  CHECK_EQ(get_u64(out.data() + 4), 0x0807060504030201ull);

  std::string extreme;
  put_u64(extreme, std::numeric_limits<std::uint64_t>::max());
  put_u32(extreme, 0x80000000u);
  CHECK_EQ(get_u64(extreme.data()), std::numeric_limits<std::uint64_t>::max());
  CHECK_EQ(get_u32(extreme.data() + 8), 0x80000000u);
}

TEST_CASE("record round trip keeps every field") {
  const std::string binary_value("a\0b\xff", 4);
  std::string encoded;
  encode_record(encoded, 42, RecordType::BatchPut, "key", binary_value);
  CHECK_EQ(encoded.size(), record_size(3, 4));

  RecordHeader header;
  REQUIRE(parse_record_header(encoded.data(), &header));
  CHECK_EQ(header.seq, 42u);
  CHECK_EQ(header.total_size(), encoded.size());

  Record record;
  REQUIRE(decode_record(encoded.data(), header, &record));
  CHECK_EQ(record.seq, 42u);
  CHECK(record.type == RecordType::BatchPut);
  CHECK_EQ(record.key, std::string_view("key"));
  CHECK_EQ(record.value, std::string_view(binary_value));
}

TEST_CASE("encoding appends after existing buffer contents") {
  std::string encoded = "prefix";
  encode_record(encoded, 1, RecordType::Put, "k", "v");
  RecordHeader header;
  REQUIRE(parse_record_header(encoded.data() + 6, &header));
  Record record;
  CHECK(decode_record(encoded.data() + 6, header, &record));
}

TEST_CASE("flipping any byte is detected") {
  std::string encoded;
  encode_record(encoded, 7, RecordType::Put, "some-key", "some-value");
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    std::string damaged = encoded;
    damaged[i] = static_cast<char>(damaged[i] ^ 0x01);
    RecordHeader header;
    Record record;
    const bool parsed = parse_record_header(damaged.data(), &header);
    // A damaged length changes how much is covered, so only decode what fits.
    const bool accepted = parsed && header.total_size() <= damaged.size() &&
                          decode_record(damaged.data(), header, &record);
    CHECK(!accepted);
  }
}

TEST_CASE("implausible headers are rejected before any checksum") {
  const auto parses = [](RecordType type, std::string_view key, std::string_view value) {
    std::string encoded;
    encode_record(encoded, 1, type, key, value);
    RecordHeader header;
    return parse_record_header(encoded.data(), &header);
  };
  CHECK(!parses(RecordType::Put, "", "value"));
  CHECK(!parses(RecordType::BatchCommit, "key", ""));
  CHECK(parses(RecordType::BatchCommit, "", "abcd"));

  // An all-zero block, as left by a filesystem that extended the file before
  // writing the data, has type 0.
  const std::string zeros(kRecordHeaderSize, '\0');
  RecordHeader header;
  CHECK(!parse_record_header(zeros.data(), &header));

  std::string oversized;
  encode_record(oversized, 1, RecordType::Put, "k", "v");
  oversized[13 + 3] = '\x7f';  // Top byte of key_len.
  CHECK(!parse_record_header(oversized.data(), &header));
}

TEST_CASE("every record type has a name") {
  CHECK_EQ(std::string(record_type_name(RecordType::Put)), std::string("put"));
  CHECK_EQ(std::string(record_type_name(RecordType::BatchCommit)), std::string("batch-commit"));
}

TEST_MAIN
