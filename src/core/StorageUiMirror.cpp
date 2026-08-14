
#include "StorageUiMirror.h"

#include "SpectreState.h"

namespace StorageUiMirror {

void applyFull(const StorageUiSnapshot& snap,
               uint32_t nowMs,
               uint32_t dedupedSuppressed,
               uint32_t dedupedDropped) {
    STATE_WRITE_BEGIN();
    g_state.storageNearlyFull = snap.nearlyFull;
    g_state.storageFull = snap.full;
    g_state.storageOverrun = snap.overrun;
    g_state.storageMode = snap.mode;
    g_state.storagePolicy = snap.policy;
    g_state.storageUsedPct = snap.usedPct;
    g_state.storageFreeBytes = snap.freeBytes;
    g_state.storagePending = snap.pending;
    g_state.storageDeduped = dedupedSuppressed;
    g_state.storageDropped = dedupedDropped;
    g_state.storageSummaryUpdatedMs = nowMs;
    g_state.storageSummaryValid = snap.summaryValid;
    g_state.storageSummaryNeedsMaint = !snap.summaryValid;
    g_state.storageSummaryStatus = snap.summaryValid
                                       ? STORAGE_SUMMARY_FRESH
                                       : STORAGE_SUMMARY_MAINTENANCE_NEEDED;
    g_state.storageMissionTotal = snap.missionTotal;
    g_state.storageNoiseTotal = snap.noiseTotal;
    g_state.storageEventTotal = snap.eventTotal;
    g_state.storageRecordTotal = snap.recordTotal;
    g_state.storageP0Total = snap.p0Total;
    g_state.storageP1Total = snap.p1Total;
    g_state.storageP2Total = snap.p2Total;
    g_state.storageP3Total = snap.p3Total;
    g_state.storagePendingUploadMission = snap.pendingUploadMission;
    g_state.storagePendingUploadNoise = snap.pendingUploadNoise;
    g_state.storagePendingEnrichMission = snap.pendingEnrichMission;
    g_state.storagePendingEnrichNoise = snap.pendingEnrichNoise;
    g_state.storageEnrichmentDeltas = snap.enrichmentDeltas;
    g_state.storageFirstEventId = snap.firstEventId;
    g_state.storageLastEventId = snap.lastEventId;
    g_state.storageDumpAdvised = snap.dumpAdvised;
    g_state.storageRepairRequired = snap.repairRequired;
    g_state.storageCounterTrust = snap.counterTrust;
    strlcpy(g_state.storagePolicyText, snap.policyText, sizeof(g_state.storagePolicyText));
    g_state.dataRefresh = true;
    STATE_WRITE_END();
}

void publishCounters(uint32_t pending,
                     uint32_t pendingUploadMission,
                     uint32_t pendingUploadNoise,
                     uint32_t eventTotal,
                     uint32_t recordTotal) {
    STATE_WRITE_BEGIN();
    g_state.storagePending = pending;
    g_state.storagePendingUploadMission = pendingUploadMission;
    g_state.storagePendingUploadNoise = pendingUploadNoise;
    g_state.storageEventTotal = eventTotal;
    g_state.storageRecordTotal = recordTotal;
    STATE_WRITE_END();
}

void publishPendingEnrichment(uint32_t pendingMission,
                              uint32_t pendingNoise,
                              uint32_t nowMs) {
    STATE_WRITE_BEGIN();
    g_state.storagePendingEnrichMission = pendingMission;
    g_state.storagePendingEnrichNoise = pendingNoise;
    g_state.storageSummaryUpdatedMs = nowMs;
    g_state.dataRefresh = true;
    STATE_WRITE_END();
}

void publishReadyState(bool storageOk,
                       const char* usedStr,
                       uint32_t pending,
                       uint32_t pendingUploadMission,
                       uint32_t pendingUploadNoise,
                       uint32_t eventTotal,
                       uint32_t recordTotal) {
    STATE_WRITE_BEGIN();
    g_state.storageReady = storageOk;
    if (storageOk) {
        if (usedStr) {
            strlcpy(g_state.storageStr, usedStr, sizeof(g_state.storageStr));
        }
        g_state.storagePending = pending;
        g_state.storagePendingUploadMission = pendingUploadMission;
        g_state.storagePendingUploadNoise = pendingUploadNoise;
        g_state.storageEventTotal = eventTotal;
        g_state.storageRecordTotal = recordTotal;
    }
    STATE_WRITE_END();
}

void publishMaintenance(bool storageReady,
                        bool running,
                        bool ranWindow,
                        uint32_t durationMs,
                        bool captureSafe,
                        uint32_t flags,
                        const char* text) {
    STATE_WRITE_BEGIN();
    uint8_t status = STORAGE_MAINT_UI_UNKNOWN;
    if (!storageReady) {
        status = STORAGE_MAINT_UI_OFFLINE;
    } else if (running) {
        status = STORAGE_MAINT_UI_RUNNING;
    } else if (flags == 0) {
        status = STORAGE_MAINT_UI_COMPLETE;
    } else if (!captureSafe &&
               (ranWindow ||
                g_state.storageMaintenanceStatus == STORAGE_MAINT_UI_INCOMPLETE)) {
        status = STORAGE_MAINT_UI_INCOMPLETE;
    } else {
        status = STORAGE_MAINT_UI_PENDING;
    }

    g_state.storageMaintenanceStatus = status;
    g_state.storageMaintenanceFlags = flags;
    if (text) {
        strlcpy(g_state.storageMaintenanceText, text, sizeof(g_state.storageMaintenanceText));
    }
    if (ranWindow) {
        g_state.storageMaintenanceLastRunMs = millis();
        g_state.storageMaintenanceLastDurationMs = durationMs;
    }
    STATE_WRITE_END();
}

void publishClearedSummary(bool storageReady,
                           bool storageMaintPending,
                           uint32_t nowMs,
                           uint32_t pending,
                           uint32_t pendingUploadMission,
                           uint32_t pendingUploadNoise,
                           uint32_t eventTotal,
                           uint32_t recordTotal) {
    STATE_WRITE_BEGIN();
    g_state.storageSummaryValid = false;
    g_state.storageSummaryUpdatedMs = nowMs;
    g_state.storageSummaryNeedsMaint = storageMaintPending;
    g_state.storageSummaryStatus = g_state.storageSummaryNeedsMaint
                                       ? STORAGE_SUMMARY_MAINTENANCE_NEEDED
                                       : STORAGE_SUMMARY_UNKNOWN;

    if (!storageReady) {
        g_state.storageMissionTotal = 0;
        g_state.storageNoiseTotal = 0;
        g_state.storageEventTotal = 0;
        g_state.storageRecordTotal = 0;
        g_state.storageP0Total = 0;
        g_state.storageP1Total = 0;
        g_state.storageP2Total = 0;
        g_state.storageP3Total = 0;
        g_state.storagePendingUploadMission = 0;
        g_state.storagePendingUploadNoise = 0;
        g_state.storagePendingEnrichMission = 0;
        g_state.storagePendingEnrichNoise = 0;
        g_state.storageEnrichmentDeltas = 0;
        g_state.storageFirstEventId = 0;
        g_state.storageLastEventId = 0;
    } else {
        g_state.storageEventTotal = eventTotal;
        g_state.storageRecordTotal = recordTotal;
        g_state.storagePending = pending;
        g_state.storagePendingUploadMission = pendingUploadMission;
        g_state.storagePendingUploadNoise = pendingUploadNoise;
    }
    STATE_WRITE_END();
}

void publishSummaryRadioActive() {
    STATE_WRITE_BEGIN();
    g_state.storageSummaryNeedsMaint = true;
    // Preserve storageSummaryValid: prior data is still the best we have.
    // Just demote status so the UI shows that maintenance is owed.
    if (!g_state.storageSummaryValid) {
        g_state.storageSummaryStatus = STORAGE_SUMMARY_MAINTENANCE_NEEDED;
    }
    STATE_WRITE_END();
}

bool publishSummaryStamp(uint32_t nowMs) {
    bool ok = false;
    STATE_WRITE_BEGIN();
    ok = g_state.storageSummaryValid;
    g_state.storageSummaryUpdatedMs = nowMs;
    g_state.storageSummaryNeedsMaint = !ok;
    g_state.storageSummaryStatus = ok
                                       ? STORAGE_SUMMARY_FRESH
                                       : STORAGE_SUMMARY_MAINTENANCE_NEEDED;
    STATE_WRITE_END();
    return ok;
}

void publishPostAuditRecordTotal(uint32_t recordTotal,
                                 uint32_t eventTotal,
                                 uint32_t nowMs) {
    STATE_WRITE_BEGIN();
    g_state.storageEventTotal = eventTotal;
    g_state.storageRecordTotal = recordTotal;
    g_state.storageSummaryUpdatedMs = nowMs;
    g_state.dataRefresh = true;
    STATE_WRITE_END();
}

void writeReady_locked(bool ready) {
    g_state.storageReady = ready;
}

void writeStr_locked(const char* usedStr) {
    if (usedStr) {
        strlcpy(g_state.storageStr, usedStr, sizeof(g_state.storageStr));
    }
}

void writePostReconcileCounters_locked(uint32_t pending,
                                       uint32_t eventTotal,
                                       uint32_t recordTotal,
                                       uint32_t nowMs,
                                       bool summaryNeedsMaint) {
    g_state.storagePending = pending;
    g_state.storagePendingUploadMission = 0;
    g_state.storagePendingUploadNoise = pending;
    g_state.storageEventTotal = eventTotal;
    g_state.storageRecordTotal = recordTotal;
    g_state.storageSummaryUpdatedMs = nowMs;
    g_state.storageSummaryNeedsMaint = summaryNeedsMaint;
    g_state.storageSummaryStatus = summaryNeedsMaint
                                       ? STORAGE_SUMMARY_MAINTENANCE_NEEDED
                                       : STORAGE_SUMMARY_FRESH;
}

}  // namespace StorageUiMirror
