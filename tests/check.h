#pragma once

// A small self-contained test helper: named test cases, non-fatal checks, a
// temporary directory, and tools for damaging files the way a crash would.

#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "tinykv/store.h"

namespace testing {

namespace fs = std::filesystem;

struct Case {
  const char* name;
  std::function<void()> body;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int& failure_count() {
  static int count = 0;
  return count;
}

struct Register {
  Register(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

inline void report_failure(const char* file, int line, const std::string& message) {
  std::cerr << "  " << file << ":" << line << ": " << message << "\n";
  ++failure_count();
}

template <typename T>
std::string describe(const T& value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

inline std::string describe(const std::optional<std::string>& value) {
  return value ? "\"" + *value + "\"" : std::string("<absent>");
}

// Stops the current test case, for use when going on would crash.
struct Abort {};

inline int run_all() {
  int failed_cases = 0;
  for (const Case& test : registry()) {
    const int before = failure_count();
    try {
      test.body();
    } catch (const Abort&) {
    } catch (const std::exception& error) {
      report_failure(test.name, 0, std::string("unexpected exception: ") + error.what());
    }
    const bool passed = failure_count() == before;
    std::cout << (passed ? "ok   " : "FAIL ") << test.name << "\n";
    if (!passed) ++failed_cases;
  }
  std::cout << registry().size() - static_cast<std::size_t>(failed_cases) << "/" << registry().size()
            << " cases passed\n";
  return failed_cases == 0 ? 0 : 1;
}

// A directory under the current working directory, removed on destruction.
class TempDir {
 public:
  TempDir() {
    std::string pattern = (fs::current_path() / "tkv-test-XXXXXX").string();
    if (mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
    path_ = pattern;
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }
  std::string sub(const std::string& name) const { return path_ + "/" + name; }

 private:
  std::string path_;
};

// Fast defaults: durability is irrelevant in tests that never lose power.
inline tinykv::Options fast_options() {
  tinykv::Options options;
  options.sync = tinykv::SyncMode::Never;
  return options;
}

inline tinykv::Options small_segments(std::uint64_t bytes = 256) {
  tinykv::Options options = fast_options();
  options.max_segment_bytes = bytes;
  return options;
}

inline std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline void write_file(const std::string& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

inline void flip_byte(const std::string& path, std::uint64_t offset) {
  std::string data = read_file(path);
  data.at(static_cast<std::size_t>(offset)) = static_cast<char>(data.at(static_cast<std::size_t>(offset)) ^ 0x5a);
  write_file(path, data);
}

// Paths of the files with this extension, in ascending name order.
inline std::vector<std::string> files_with_extension(const std::string& dir, const std::string& extension) {
  std::vector<std::string> out;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.path().extension() == extension) out.push_back(entry.path().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

inline std::vector<std::string> segment_files(const std::string& dir) {
  return files_with_extension(dir, ".seg");
}

inline std::map<std::string, std::string> contents(const tinykv::Store& store) {
  std::map<std::string, std::string> out;
  for (auto it = store.scan(); it.valid(); it.next()) out[std::string(it.key())] = it.value();
  return out;
}

inline void copy_directory(const std::string& from, const std::string& to) {
  fs::create_directories(to);
  for (const auto& entry : fs::directory_iterator(from)) {
    fs::copy_file(entry.path(), fs::path(to) / entry.path().filename(),
                  fs::copy_options::overwrite_existing);
  }
}

}  // namespace testing

#define TKV_CONCAT_INNER(a, b) a##b
#define TKV_CONCAT(a, b) TKV_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                       \
  static void TKV_CONCAT(test_body_, __LINE__)();                             \
  static const testing::Register TKV_CONCAT(test_register_, __LINE__)(        \
      name, TKV_CONCAT(test_body_, __LINE__));                                \
  static void TKV_CONCAT(test_body_, __LINE__)()

#define TEST_MAIN \
  int main() { return testing::run_all(); }

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) testing::report_failure(__FILE__, __LINE__, "CHECK(" #condition ") failed"); \
  } while (false)

#define REQUIRE(condition)                                                    \
  do {                                                                        \
    if (!(condition)) {                                                       \
      testing::report_failure(__FILE__, __LINE__, "REQUIRE(" #condition ") failed"); \
      throw testing::Abort{};                                                 \
    }                                                                         \
  } while (false)

#define CHECK_EQ(actual, expected)                                            \
  do {                                                                        \
    const auto& tkv_a = (actual);                                             \
    const auto& tkv_e = (expected);                                           \
    if (!(tkv_a == tkv_e)) {                                                  \
      testing::report_failure(__FILE__, __LINE__, std::string(#actual " == " #expected ": got ") + \
                              testing::describe(tkv_a) + ", expected " + testing::describe(tkv_e)); \
    }                                                                         \
  } while (false)

// Checks that the expression throws tinykv::Error with the given code.
#define CHECK_THROWS(expression, error_code)                                  \
  do {                                                                        \
    bool tkv_caught = false;                                                  \
    try {                                                                     \
      (void)(expression);                                                     \
    } catch (const tinykv::Error& tkv_error) {                                \
      tkv_caught = tkv_error.code() == (error_code);                          \
    }                                                                         \
    if (!tkv_caught) testing::report_failure(__FILE__, __LINE__, "expected Error from " #expression); \
  } while (false)
