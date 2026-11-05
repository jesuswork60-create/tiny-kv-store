#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace tinykv {

struct VerifyIssue {
  enum class Severity {
    Warning,  // Open would handle it, for example a torn tail or stale hint.
    Error,    // Data was lost or open would refuse the store.
  };
  Severity severity;
  std::uint64_t segment_id;  // 0 when the issue is not tied to a segment.
  std::string message;
};

struct VerifyReport {
  std::uint64_t segments = 0;
  std::uint64_t records = 0;
  std::uint64_t bytes = 0;
  std::uint64_t live_keys = 0;
  std::uint64_t hints_checked = 0;
  std::vector<VerifyIssue> issues;

  bool ok() const;
};

// Checks every segment and hint file without changing anything. Takes the
// directory lock, so it throws Error(Locked) while a Store has the directory.
VerifyReport verify_store(const std::string& dir);

struct DumpOptions {
  bool show_values = false;
};

// Prints each record, including superseded ones, one per line. Does not lock,
// so running it against a store being written may show a partial tail.
void dump_store(const std::string& dir, std::ostream& out, const DumpOptions& options = {});

}  // namespace tinykv
