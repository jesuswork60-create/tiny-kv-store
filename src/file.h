#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Thin POSIX wrappers. Every failure throws tinykv::Error with the path and
// errno text in its message.
namespace tinykv {

// Read-only positional access; safe to share between threads.
class RandomFile {
 public:
  explicit RandomFile(const std::string& path);
  ~RandomFile();
  RandomFile(const RandomFile&) = delete;
  RandomFile& operator=(const RandomFile&) = delete;

  // Fills dst completely or throws (a short file counts as corruption).
  void read_exact(std::uint64_t offset, char* dst, std::size_t size) const;
  std::uint64_t size() const;
  const std::string& path() const { return path_; }

 private:
  int fd_;
  std::string path_;
};

// Append-only writer. A failed append is rolled back with ftruncate so the
// file never ends in a partial record; damaged() reports that the rollback or
// an fsync failed, after which the file state cannot be trusted.
class AppendFile {
 public:
  // Creates a new file; fails if it already exists.
  static AppendFile create(const std::string& path);
  static AppendFile open_existing(const std::string& path);

  AppendFile(AppendFile&& other) noexcept;
  AppendFile& operator=(AppendFile&&) = delete;
  AppendFile(const AppendFile&) = delete;
  AppendFile& operator=(const AppendFile&) = delete;
  ~AppendFile();

  void append(std::string_view data);
  void sync();
  std::uint64_t size() const { return size_; }
  bool damaged() const { return damaged_; }

 private:
  AppendFile(int fd, std::string path, std::uint64_t size);

  int fd_;
  std::string path_;
  std::uint64_t size_;
  bool damaged_ = false;
};

// Exclusive advisory lock on a file, via flock(2). flock locks belong to the
// open file description, so a second open in the same process conflicts too,
// which fcntl record locks would not catch.
class FileLock {
 public:
  explicit FileLock(const std::string& path);
  ~FileLock();
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

 private:
  int fd_;
};

void sync_dir(const std::string& dir);
void remove_file(const std::string& path);
void rename_file(const std::string& from, const std::string& to);
void truncate_file(const std::string& path, std::uint64_t size);
std::string read_whole_file(const std::string& path);
// Writes through a temporary sibling and renames it into place.
void write_file_atomic(const std::string& path, std::string_view data);

}  // namespace tinykv
