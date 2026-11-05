#pragma once

#include "keydir.h"
#include "tinykv/options.h"
#include "tinykv/store.h"

namespace tinykv {

// Replaces every segment in `state` with new ones holding only live records.
// The caller must have sealed the active segment and must exclude all other
// access to `state`. On failure `state` and the old segments are unchanged.
//
// Crash safety rests on this order:
//   1. Each output is written to a temp file, synced, then renamed into place.
//      Copies keep their original sequence numbers, so replaying an output next
//      to the segments it came from changes nothing.
//   2. Old segments are deleted oldest first, syncing the directory each time.
//      A tombstone therefore always outlives the older puts it covers: at any
//      crash point, those puts are either already gone or still shadowed.
CompactionStats compact_log(const std::string& dir, const Options& options, LogState& state);

}  // namespace tinykv
