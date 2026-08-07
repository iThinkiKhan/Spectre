
#pragma once

#include <Arduino.h>

// Single writer for the g_state.storage* mirror and refresh snapshot.

struct StorageUiSnapshot {
    bool nearlyFull = false;
    bool full = false;
    bool overrun = false;
    bool dumpAdvised = false;
    uint8_t mode = 0;
    uint8_t policy = 0;
    uint16_t usedPct = 0;
    uint32_t freeBytes = 0;
    uint32_t pending = 0;
    bool summaryValid = false;
    uint32_t missionTotal = 0;
    uint32_t noiseTotal = 0;
    uint32_t eventTotal = 0;
    uint32_t recordTotal = 0;
    uint32_t p0Total = 0;
    uint32_t p1Total = 0;
    uint32_t p2Total = 0;
    uint32_t p3Total = 0;
    uint32_t pendingUploadMission = 0;
    uint32_t pendingUploadNoise = 0;
    uint32_t pendingEnrichMission = 0;
    uint32_t pendingEnrichNoise = 0;
    uint32_t enrichmentDeltas = 0;
    uint32_t deduped = 0;
    uint32_t dropped = 0;
    uint32_t firstEventId = 0;
    uint32_t lastEventId = 0;
    bool repairRequired = false;
    uint8_t counterTrust = 0;
    char policyText[20] = "NORMAL";
};

namespace StorageUiMirror {

// Periodic authoritative refresh. Writes 32 fields + storagePolicyText +
// dataRefresh atomically under STATE_WRITE_BEGIN/END. Replaces the long
// block inside StorageManager::refreshStorageUiState().
void applyFull(const StorageUiSnapshot& snap,
               uint32_t nowMs,
               uint32_t dedupedSuppressed,
               uint32_t dedupedDropped);

// Counter-only refresh (4 fields). Replaces _refreshStorageCounterMirror's
// critical section.
void publishCounters(uint32_t pending,
                     uint32_t pendingUploadMission,
                     uint32_t pendingUploadNoise,
                     uint32_t eventTotal,
                     uint32_t recordTotal);

// Storage-ready bit + (when ready) usedStr and 4 counters. Replaces
// _publishStorageState's critical section.
void publishReadyState(bool storageOk,
                       const char* usedStr,
                       uint32_t pending,
                       uint32_t pendingUploadMission,
                       uint32_t pendingUploadNoise,
                       uint32_t eventTotal,
                       uint32_t recordTotal);

// Maintenance UI status + flags + text + optional run/duration stamp.
// Reads previous storageMaintenanceStatus inside the lock to preserve the
// "INCOMPLETE persists until captureSafe" rule. Replaces
// _publishStorageMaintenanceMirror's critical section.
void publishMaintenance(bool storageReady,
                        bool running,
                        bool ranWindow,
                        uint32_t durationMs,
                        bool captureSafe,
                        uint32_t flags,
                        const char* text);

// Summary-invalidating refresh used at boot and on transitions. When
// !storageReady, also zeros every accumulator. Replaces
// _clearStorageSummaryMirror's critical section.
void publishClearedSummary(bool storageReady,
                           bool storageMaintPending,
                           uint32_t nowMs,
                           uint32_t pending,
                           uint32_t pendingUploadMission,
                           uint32_t pendingUploadNoise,
                           uint32_t eventTotal,
                           uint32_t recordTotal);

// Demote summary to "maintenance needed" without touching valid-bit or
// totals — used when a radio is active and a summary refresh would race.
// Replaces the radio-active branch in _refreshStorageSummaryMirror.
void publishSummaryRadioActive();

// Stamp summary freshness; returns the post-stamp summaryValid flag so the
// caller can return it without doing its own state read. Replaces the
// non-radio-active branch in _refreshStorageSummaryMirror.
bool publishSummaryStamp(uint32_t nowMs);

// Post-spool-audit small patch: recordTotal + summaryUpdatedMs +
// dataRefresh. Replaces the critical section inside spoolAuditToSerial.
void publishPostAuditRecordTotal(uint32_t recordTotal,
                                 uint32_t eventTotal,
                                 uint32_t nowMs);

// ---- _locked variants: caller already holds STATE_WRITE_BEGIN/END ----

// Used by boot recovery (sets false) and boot heap triage (sets true).
void writeReady_locked(bool ready);

// Used by boot heap triage to set storageStr = "triage" inside its
// larger UI-state critical section.
void writeStr_locked(const char* usedStr);

// Post-manual-count reconcile patch (StorageManager::spoolCountToSerial):
// writes storagePending/UploadMission/UploadNoise/RecordTotal +
// summaryUpdatedMs/NeedsMaint/Status. summaryNeedsMaint maps to
// MAINTENANCE_NEEDED / FRESH.
void writePostReconcileCounters_locked(uint32_t pending,
                                       uint32_t eventTotal,
                                       uint32_t recordTotal,
                                       uint32_t nowMs,
                                       bool summaryNeedsMaint);

}  // namespace StorageUiMirror
