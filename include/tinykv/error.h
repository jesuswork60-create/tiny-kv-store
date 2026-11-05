#pragma once

#include <stdexcept>
#include <string>

namespace tinykv {

enum class ErrorCode {
  Io,               // A system call failed.
  Corruption,       // On-disk data failed validation.
  Locked,           // Another handle or process owns the directory.
  InvalidArgument,  // The caller passed something unusable.
  Closed,           // The store was closed, or went read-only after a failed write.
};

// All library failures surface as this exception type. Absent keys are not
// errors; lookups report them through std::optional.
class Error : public std::runtime_error {
 public:
  Error(ErrorCode code, const std::string& message)
      : std::runtime_error(message), code_(code) {}

  ErrorCode code() const noexcept { return code_; }

 private:
  ErrorCode code_;
};

}  // namespace tinykv
