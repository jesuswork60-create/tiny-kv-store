#include "file.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "tinykv/error.h"

namespace tinykv {
namespace {

[[noreturn]] void throw_io(const std::string& what, const std::string& path) {
  throw Error(ErrorCode::Io, what + " " + path + ": " + std::strerror(errno));
}

int open_or_throw(const std::string& path, int flags, const char* what) {
  int fd;
  do {
    fd = ::open(path.c_str(), flags | O_CLOEXEC, 0644);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) throw_io(what, path);
  return fd;
}

std::uint64_t fd_size(int fd, const std::string& path) {
  struct stat st {};
  if (::fstat(fd, &st) != 0) throw_io("cannot stat", path);
  return static_cast<std::uint64_t>(st.st_size);
}

int fsync_retrying(int fd) {
  int rc;
  do {
    rc = ::fsync(fd);
  } while (rc != 0 && errno == EINTR);
  return rc;
}

}  // namespace

RandomFile::RandomFile(const std::string& path)
    : fd_(open_or_throw(path, O_RDONLY, "cannot open")), path_(path) {}

RandomFile::~RandomFile() { ::close(fd_); }

void RandomFile::read_exact(std::uint64_t offset, char* dst, std::size_t size) const {
  std::size_t done = 0;
  while (done < size) {
    const ssize_t n = ::pread(fd_, dst + done, size - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      throw_io("cannot read", path_);
    }
    if (n == 0) {
      throw Error(ErrorCode::Corruption, "unexpected end of file in " + path_ + " at offset " +
                                             std::to_string(offset + done));
    }
    done += static_cast<std::size_t>(n);
  }
}

std::uint64_t RandomFile::size() const { return fd_size(fd_, path_); }

AppendFile::AppendFile(int fd, std::string path, std::uint64_t size)
    : fd_(fd), path_(std::move(path)), size_(size) {}

AppendFile AppendFile::create(const std::string& path) {
  const int fd = open_or_throw(path, O_WRONLY | O_APPEND | O_CREAT | O_EXCL, "cannot create");
  return AppendFile(fd, path, 0);
}

AppendFile AppendFile::open_existing(const std::string& path) {
  const int fd = open_or_throw(path, O_WRONLY | O_APPEND, "cannot open");
  return AppendFile(fd, path, fd_size(fd, path));
}

AppendFile::AppendFile(AppendFile&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      path_(std::move(other.path_)),
      size_(other.size_),
      damaged_(other.damaged_) {}

AppendFile::~AppendFile() {
  if (fd_ >= 0) ::close(fd_);
}

void AppendFile::append(std::string_view data) {
  std::size_t done = 0;
  while (done < data.size()) {
    const ssize_t n = ::write(fd_, data.data() + done, data.size() - done);
    if (n >= 0) {
      done += static_cast<std::size_t>(n);
      continue;
    }
    if (errno == EINTR) continue;
    const int saved = errno;
    if (done > 0 && ::ftruncate(fd_, static_cast<off_t>(size_)) != 0) damaged_ = true;
    errno = saved;
    throw_io("cannot append to", path_);
  }
  size_ += data.size();
}

void AppendFile::sync() {
  if (fsync_retrying(fd_) != 0) {
    // The kernel may have dropped the dirty pages, so a retry could falsely
    // succeed. Treat the file as lost for writing.
    damaged_ = true;
    throw_io("cannot sync", path_);
  }
}

FileLock::FileLock(const std::string& path)
    : fd_(open_or_throw(path, O_RDWR | O_CREAT, "cannot open lock file")) {
  if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
    const int saved = errno;
    ::close(fd_);
    if (saved == EWOULDBLOCK) {
      throw Error(ErrorCode::Locked, "store is already open: " + path);
    }
    errno = saved;
    throw_io("cannot lock", path);
  }
}

FileLock::~FileLock() { ::close(fd_); }

void sync_dir(const std::string& dir) {
  const int fd = open_or_throw(dir, O_RDONLY | O_DIRECTORY, "cannot open directory");
  const int rc = fsync_retrying(fd);
  const int saved = errno;
  ::close(fd);
  if (rc != 0) {
    errno = saved;
    throw_io("cannot sync directory", dir);
  }
}

void remove_file(const std::string& path) {
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) throw_io("cannot remove", path);
}

void rename_file(const std::string& from, const std::string& to) {
  if (::rename(from.c_str(), to.c_str()) != 0) throw_io("cannot rename", from);
}

void truncate_file(const std::string& path, std::uint64_t size) {
  const int fd = open_or_throw(path, O_WRONLY, "cannot open");
  const bool ok = ::ftruncate(fd, static_cast<off_t>(size)) == 0 && fsync_retrying(fd) == 0;
  const int saved = errno;
  ::close(fd);
  if (!ok) {
    errno = saved;
    throw_io("cannot truncate", path);
  }
}

std::string read_whole_file(const std::string& path) {
  RandomFile file(path);
  std::string data(static_cast<std::size_t>(file.size()), '\0');
  if (!data.empty()) file.read_exact(0, &data[0], data.size());
  return data;
}

void write_file_atomic(const std::string& path, std::string_view data) {
  const std::string tmp = path + ".tmp";
  remove_file(tmp);
  AppendFile file = AppendFile::create(tmp);
  file.append(data);
  file.sync();
  rename_file(tmp, path);
}

}  // namespace tinykv
