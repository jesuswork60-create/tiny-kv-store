#pragma once

#include <string>

#include "hint.h"
#include "keydir.h"
#include "tinykv/options.h"
#include "tinykv/store.h"

namespace tinykv {

struct RecoveredLog {
  RecoveryReport report;
  // Whether new writes may continue in the highest-numbered segment, and the
  // hint entries already gathered for it if so.
  bool can_append = false;
  HintBuilder active_hint;
};

// Rebuilds `state` from the files in `dir`. Removes temporary files and
// orphaned hints, truncates a damaged tail of the last segment, and throws
// Error(Corruption) for damage in older segments unless options.salvage is set.
RecoveredLog recover_log(const std::string& dir, const Options& options, LogState& state);

}  // namespace tinykv
