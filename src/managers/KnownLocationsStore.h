
#pragma once

#include "../core/SpectreState.h"

// Persistence for the user's geofence tags. Pure JSON sidecar; never touches
// the spool, counters, or UI mirror. Reads accept a legacy path and migrate
// it to the vault location on first successful load.

namespace KnownLocationsStore {

// Writes `count` entries to the vault file. Passing nullptr or count<=0
// clears both the vault file and the legacy file.
void save(const SpectreState::KnownLocation* locs, int count);

// Returns the number of entries loaded into `out` (capped by `maxCount`).
// Tries the vault path first; falls back to the legacy path and migrates.
int load(SpectreState::KnownLocation* out, int maxCount);

}  // namespace KnownLocationsStore
