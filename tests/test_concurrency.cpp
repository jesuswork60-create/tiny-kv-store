#include <atomic>
#include <thread>

#include "check.h"

using namespace tinykv;
using namespace testing;

namespace {

// The value tells which key it belongs to, so a reader can spot a mix-up.
std::string value_for(int key, int version) {
  return "key" + std::to_string(key) + "-version" + std::to_string(version);
}

bool value_matches(int key, const std::string& value) {
  return value.rfind("key" + std::to_string(key) + "-version", 0) == 0;
}

}  // namespace

TEST_CASE("readers see consistent values during writes and compaction") {
  TempDir dir;
  auto store = Store::open(dir.path(), small_segments(2048));
  constexpr int kKeys = 50;
  for (int k = 0; k < kKeys; ++k) store->put("k" + std::to_string(k), value_for(k, 0));

  std::atomic<bool> done{false};
  std::atomic<int> bad{0};
  std::atomic<int> errors{0};

  std::vector<std::thread> threads;
  for (int r = 0; r < 3; ++r) {
    threads.emplace_back([&, r] {
      int k = r;
      try {
        while (!done.load()) {
          k = (k + 7) % kKeys;
          const auto value = store->get("k" + std::to_string(k));
          if (!value || !value_matches(k, *value)) ++bad;
        }
      } catch (const Error&) {
        ++errors;
      }
    });
  }
  threads.emplace_back([&] {
    try {
      while (!done.load()) {
        std::size_t seen = 0;
        for (auto it = store->scan(); it.valid(); it.next()) {
          const int k = std::stoi(std::string(it.key().substr(1)));
          if (!value_matches(k, it.value())) ++bad;
          ++seen;
        }
        if (seen != kKeys) ++bad;
      }
    } catch (const Error&) {
      ++errors;
    }
  });
  threads.emplace_back([&] {
    try {
      for (int i = 0; i < 5 && !done.load(); ++i) {
        store->compact();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } catch (const Error&) {
      ++errors;
    }
  });

  for (int version = 1; version <= 40; ++version) {
    for (int k = 0; k < kKeys; ++k) store->put("k" + std::to_string(k), value_for(k, version));
  }
  done.store(true);
  for (std::thread& thread : threads) thread.join();

  CHECK_EQ(bad.load(), 0);
  CHECK_EQ(errors.load(), 0);
  for (int k = 0; k < kKeys; ++k) {
    CHECK_EQ(store->get("k" + std::to_string(k)), std::optional<std::string>(value_for(k, 40)));
  }
}

TEST_CASE("concurrent writers lose nothing") {
  TempDir dir;
  constexpr int kWriters = 4;
  constexpr int kEach = 150;
  {
    auto store = Store::open(dir.path(), small_segments(1024));
    std::vector<std::thread> threads;
    for (int w = 0; w < kWriters; ++w) {
      threads.emplace_back([&store, w] {
        for (int i = 0; i < kEach; ++i) {
          const std::string name = "w" + std::to_string(w) + "-" + std::to_string(i);
          if (i % 2 == 0) {
            store->put(name, name);
          } else {
            WriteBatch batch;
            batch.put(name, name);
            store->write(batch);
          }
        }
      });
    }
    for (std::thread& thread : threads) thread.join();
    CHECK_EQ(store->size(), static_cast<std::size_t>(kWriters * kEach));
  }
  auto store = Store::open(dir.path(), small_segments(1024));
  CHECK_EQ(store->size(), static_cast<std::size_t>(kWriters * kEach));
  CHECK_EQ(store->get("w3-149"), std::optional<std::string>("w3-149"));
}

TEST_CASE("close while another thread works gives Closed, not a crash") {
  TempDir dir;
  auto store = Store::open(dir.path(), fast_options());
  store->put("a", "1");
  std::atomic<int> unexpected{0};
  std::thread worker([&] {
    try {
      for (;;) {
        store->put("a", "2");
        (void)store->get("a");
      }
    } catch (const Error& error) {
      if (error.code() != ErrorCode::Closed) ++unexpected;
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  store->close();
  worker.join();
  CHECK_EQ(unexpected.load(), 0);
}

TEST_MAIN
