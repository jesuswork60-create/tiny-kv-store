// Drives the store with a seeded random workload and compares it to a std::map
// after every kind of disturbance: reopen, compaction, torn tail.

#include <random>

#include "check.h"

using namespace tinykv;
using namespace testing;

namespace {

using Model = std::map<std::string, std::string>;

struct Workload {
  explicit Workload(std::uint32_t seed) : rng(seed) {}

  std::mt19937 rng;

  std::string random_key() { return "key" + std::to_string(rng() % 40); }
  std::string random_value() {
    const std::size_t length = rng() % 8 == 0 ? rng() % 2000 : rng() % 40;
    return std::string(length, static_cast<char>('a' + rng() % 26));
  }

  void step(Store& store, Model& model) {
    switch (rng() % 10) {
      case 0:
      case 1:
      case 2:
      case 3:
      case 4: {
        const std::string key = random_key();
        const std::string value = random_value();
        store.put(key, value);
        model[key] = value;
        break;
      }
      case 5:
      case 6: {
        const std::string key = random_key();
        const bool existed = model.erase(key) == 1;
        CHECK_EQ(store.del(key), existed);
        break;
      }
      case 7:
      case 8: {
        WriteBatch batch;
        const std::uint32_t ops = 1 + rng() % 6;
        for (std::uint32_t i = 0; i < ops; ++i) {
          const std::string key = random_key();
          if (rng() % 3 == 0) {
            batch.del(key);
            model.erase(key);
          } else {
            const std::string value = random_value();
            batch.put(key, value);
            model[key] = value;
          }
        }
        store.write(batch);
        break;
      }
      default: {
        const std::string key = random_key();
        const auto found = model.find(key);
        const std::optional<std::string> value = store.get(key);
        CHECK_EQ(value.has_value(), found != model.end());
        if (value && found != model.end()) CHECK(*value == found->second);
      }
    }
  }
};

void run_seed(std::uint32_t seed, std::uint64_t segment_bytes) {
  TempDir dir;
  Options options = small_segments(segment_bytes);
  Workload workload(seed);
  Model model;
  std::unique_ptr<Store> store = Store::open(dir.path(), options);

  for (int round = 0; round < 30; ++round) {
    const std::uint32_t steps = 5 + workload.rng() % 40;
    for (std::uint32_t i = 0; i < steps; ++i) workload.step(*store, model);
    REQUIRE(contents(*store) == model);
    CHECK_EQ(store->size(), model.size());

    switch (workload.rng() % 4) {
      case 0:
        store->compact();
        CHECK_EQ(store->stats().garbage_bytes, 0u);
        break;
      case 1:
        store->close();
        store = Store::open(dir.path(), options);
        break;
      case 2: {
        // A crash in the middle of one more write: the model never saw it.
        store->put("crash-victim", "does not survive");
        store->close();
        const std::string last = segment_files(dir.path()).back();
        fs::resize_file(last, fs::file_size(last) - 1 - workload.rng() % 20);
        store = Store::open(dir.path(), options);
        REQUIRE(store->recovery_report().tail_repair.has_value());
        break;
      }
      default:
        break;
    }
    REQUIRE(contents(*store) == model);
  }

  store->close();
  options.use_hints = false;
  store = Store::open(dir.path(), options);
  CHECK(contents(*store) == model);
}

}  // namespace

TEST_CASE("random workload matches a map with small segments") {
  for (std::uint32_t seed = 1; seed <= 8; ++seed) run_seed(seed, 512);
}

TEST_CASE("random workload matches a map with larger segments") {
  for (std::uint32_t seed = 101; seed <= 104; ++seed) run_seed(seed, 16 * 1024);
}

TEST_MAIN
