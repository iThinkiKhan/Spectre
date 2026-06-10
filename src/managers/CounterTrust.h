
#pragma once

#include <Arduino.h>

// Pending-event-counter trust state.
//
// The state machine itself (transitions + maintenance side effects) lives
// on StorageManager because each transition can trigger maintenance-flag
// changes. This header only owns the enum, the legacy STORAGE_COUNTER_*
// aliases, and the pure read helpers.

enum class CounterTrust : uint8_t {
    Trusted = 0,
    TrustedSnapshotLagged = 1,
    Degraded = 2,
    RepairRequired = 3,
    EmergencyOnly = 4
};

using StorageCounterTrustState = CounterTrust;

static constexpr CounterTrust STORAGE_COUNTER_TRUSTED = CounterTrust::Trusted;
static constexpr CounterTrust STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED =
    CounterTrust::TrustedSnapshotLagged;
static constexpr CounterTrust STORAGE_COUNTER_DEGRADED = CounterTrust::Degraded;
static constexpr CounterTrust STORAGE_COUNTER_REPAIR_REQUIRED =
    CounterTrust::RepairRequired;
static constexpr CounterTrust STORAGE_COUNTER_EMERGENCY_ONLY =
    CounterTrust::EmergencyOnly;

// Stable lowercase token used in serial logs and diagnostics.
const char* counterTrustText(CounterTrust state);

// True when the live pending counter can be relied on for upload gating
// and UI display. Returns false for RepairRequired and EmergencyOnly.
inline bool counterTrustIsAuthoritative(CounterTrust state) {
    return state != CounterTrust::RepairRequired &&
           state != CounterTrust::EmergencyOnly;
}
