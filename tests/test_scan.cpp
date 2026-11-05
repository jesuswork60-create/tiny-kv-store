#include "check.h"

using namespace tinykv;
using namespace testing;

namespace {

std::vector<std::string> scanned_keys(Iterator it) {
  std::vector<std::string> out;
  for (; it.valid(); it.next()) out.emplace_back(it.key());
  return out;
}

using Keys = std::vector<std::string>;

std::string join(const Keys& keys) {
  std::string out;
  for (const std::string& key : keys) out += key + ",";
  return out;
}

void fill(Store& store) {
  for (const char* key : {"user:2", "user:10", "app", "zebra", "user:1", "user;", "use"}) {
    store.put(key, std::string("v-") + key);
  }
}

ScanRange prefix_range(const std::string& prefix) {
  ScanRange range;
  range.prefix = prefix;
  return range;
}

}  // namespace

TEST_CASE("full scan is sorted") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  fill(*store);
  CHECK_EQ(join(scanned_keys(store->scan())),
           std::string("app,use,user:1,user:10,user:2,user;,zebra,"));
}

TEST_CASE("prefix scan selects exactly the prefix") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  fill(*store);
  CHECK_EQ(join(scanned_keys(store->scan(prefix_range("user:")))), std::string("user:1,user:10,user:2,"));
  CHECK_EQ(join(store->keys("user:")), std::string("user:1,user:10,user:2,"));
  CHECK_EQ(join(scanned_keys(store->scan(prefix_range("nothing")))), std::string());
  CHECK_EQ(join(scanned_keys(store->scan(prefix_range("zebra")))), std::string("zebra,"));
}

TEST_CASE("bounds and limit combine with a prefix") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  fill(*store);

  ScanRange bounded;
  bounded.start = "use";
  bounded.end = "user:2";
  CHECK_EQ(join(scanned_keys(store->scan(bounded))), std::string("use,user:1,user:10,"));

  ScanRange inside = prefix_range("user:");
  inside.start = "user:10";
  CHECK_EQ(join(scanned_keys(store->scan(inside))), std::string("user:10,user:2,"));

  ScanRange before_prefix = prefix_range("user:");
  before_prefix.start = "a";
  before_prefix.end = "user:10";
  CHECK_EQ(join(scanned_keys(store->scan(before_prefix))), std::string("user:1,"));

  ScanRange past_prefix = prefix_range("user:");
  past_prefix.start = "v";
  CHECK(scanned_keys(store->scan(past_prefix)).empty());

  ScanRange limited;
  limited.limit = 2;
  CHECK_EQ(join(scanned_keys(store->scan(limited))), std::string("app,use,"));
}

TEST_CASE("keys sort as unsigned bytes") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  const std::string high("\xff", 1);
  const std::string with_nul("a\0b", 3);
  store->put(high, "1");
  store->put("a", "2");
  store->put(with_nul, "3");
  const Keys keys = scanned_keys(store->scan());
  REQUIRE(keys.size() == 3);
  CHECK_EQ(keys[0], std::string("a"));
  CHECK_EQ(keys[1], with_nul);
  CHECK_EQ(keys[2], high);
}

TEST_CASE("iterator values are correct and cached") {
  TempDir dir;
  auto store = Store::open(dir.path(), small_segments(128));
  fill(*store);
  auto it = store->scan(prefix_range("user:"));
  REQUIRE(it.valid());
  CHECK_EQ(it.key(), std::string_view("user:1"));
  const std::string& first = it.value();
  CHECK_EQ(first, std::string("v-user:1"));
  CHECK_EQ(&it.value(), &first);
  it.next();
  CHECK_EQ(it.value(), std::string("v-user:10"));
}

TEST_CASE("iterator does not see later writes") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  store->put("a", "old");
  store->put("b", "old");
  auto it = store->scan();
  store->put("a", "new");
  store->del("b");
  store->put("c", "new");

  CHECK_EQ(it.key(), std::string_view("a"));
  CHECK_EQ(it.value(), std::string("old"));
  it.next();
  CHECK_EQ(it.key(), std::string_view("b"));
  CHECK_EQ(it.value(), std::string("old"));
  it.next();
  CHECK(!it.valid());
}

TEST_CASE("iterator survives compaction and store close") {
  TempDir dir;
  auto store = Store::open(dir.path(), small_segments(200));
  for (int i = 0; i < 30; ++i) store->put("k" + std::to_string(100 + i), "v" + std::to_string(i));
  auto it = store->scan();
  store->compact();
  for (int i = 0; i < 30; ++i) store->del("k" + std::to_string(100 + i));
  store->compact();
  store.reset();

  int seen = 0;
  for (; it.valid(); it.next()) {
    CHECK_EQ(it.value(), "v" + std::to_string(seen));
    ++seen;
  }
  CHECK_EQ(seen, 30);
}

TEST_CASE("seek moves in both directions") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  fill(*store);
  auto it = store->scan();
  it.seek("user:2");
  CHECK_EQ(it.key(), std::string_view("user:2"));
  CHECK_EQ(it.value(), std::string("v-user:2"));
  it.seek("b");
  CHECK_EQ(it.key(), std::string_view("use"));
  CHECK_EQ(it.value(), std::string("v-use"));
  it.seek("zz");
  CHECK(!it.valid());
  it.seek("");
  CHECK_EQ(it.key(), std::string_view("app"));
}

TEST_CASE("invalid iterators refuse access") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  auto it = store->scan();
  CHECK(!it.valid());
  CHECK_THROWS(it.key(), ErrorCode::InvalidArgument);
  CHECK_THROWS(it.value(), ErrorCode::InvalidArgument);
  it.next();  // A no-op past the end.
  Iterator empty;
  CHECK(!empty.valid());
}

TEST_CASE("iterators can be moved") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  store->put("a", "1");
  auto first = store->scan();
  Iterator second = std::move(first);
  CHECK(!first.valid());
  CHECK_EQ(second.value(), std::string("1"));
  first = std::move(second);
  CHECK_EQ(first.key(), std::string_view("a"));
}

TEST_CASE("a corrupted value surfaces from the iterator") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  store->put("a", "value");
  auto it = store->scan();
  flip_byte(segment_files(dir.path()).back(), 32 + 21 + 1);
  CHECK_THROWS(it.value(), ErrorCode::Corruption);
}

TEST_MAIN
