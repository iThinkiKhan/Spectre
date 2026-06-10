
#pragma once

#include <Arduino.h>

// Storage pressure classification.
//
// Pure function of `usedPct` -> (StoragePressureMode, StorageRetentionPolicy).
// StorageManager owns the live `_pressureMode` / `_retentionPolicy` fields
// and the side-effect paths (event publish, compaction trigger); this header
// only owns the enums, thresholds, and classification.

enum StoragePressureMode : uint8_t {
    STORAGE_MODE_NORMAL = 0,
    STORAGE_MODE_WATCH,
    STORAGE_MODE_FULL,
    STORAGE_MODE_OVERRUN
};

enum StorageRetentionPolicy : uint8_t {
    STORAGE_POLICY_NORMAL = 0,
    STORAGE_POLICY_REDUCED,
    STORAGE_POLICY_CRITICAL_ONLY
};

namespace StoragePressure {

static constexpr uint8_t WATCH_PCT   = 80;
static constexpr uint8_t FULL_PCT    = 92;
static constexpr uint8_t OVERRUN_PCT = 97;

struct Classification {
    StoragePressureMode    mode;
    StorageRetentionPolicy policy;
};

inline Classification classify(int usedPct) {
    if (usedPct >= OVERRUN_PCT) {
        return {STORAGE_MODE_OVERRUN, STORAGE_POLICY_CRITICAL_ONLY};
    }
    if (usedPct >= FULL_PCT) {
        return {STORAGE_MODE_FULL, STORAGE_POLICY_REDUCED};
    }
    if (usedPct >= WATCH_PCT) {
        return {STORAGE_MODE_WATCH, STORAGE_POLICY_NORMAL};
    }
    return {STORAGE_MODE_NORMAL, STORAGE_POLICY_NORMAL};
}

}  // namespace StoragePressure
