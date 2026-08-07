#include "StorageExclusiveWindow.h"

#include "DebugLog.h"
#include "RuntimeContracts.h"
#include "../managers/RAMSpool.h"
#include "../managers/RadioArbiter.h"

extern TaskHandle_t taskDisplayHandle;

const char* StorageExclusiveWindow::_kindName(StorageWindowKind kind) const {
    switch (kind) {
        case STORAGE_WINDOW_UPLOAD:            return "upload";
        case STORAGE_WINDOW_MAINTENANCE:       return "maintenance";
        case STORAGE_WINDOW_EXPORT:            return "export";
        case STORAGE_WINDOW_FIELDVAULT_UPLOAD: return "fieldvault_upload";
        case STORAGE_WINDOW_COMPACTION:        return "compaction";
        case STORAGE_WINDOW_ENRICHMENT:        return "enrichment";
        default:                               return "unknown";
    }
}

bool StorageExclusiveWindow::_ownerMatches(StorageWindowKind kind) const {
    switch (kind) {
        case STORAGE_WINDOW_UPLOAD:
        case STORAGE_WINDOW_FIELDVAULT_UPLOAD:
            return RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD);
        case STORAGE_WINDOW_MAINTENANCE:
        case STORAGE_WINDOW_COMPACTION:
            return RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE);
        case STORAGE_WINDOW_EXPORT:
            return RADIO_ARB.currentOwner() == RADIO_NONE ||
                   RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE);
        case STORAGE_WINDOW_ENRICHMENT:
            return RADIO_ARB.isOwner(RADIO_BLE_GPS);
        default:
            return false;
    }
}

bool StorageExclusiveWindow::begin(StorageWindowKind kind, const char* reason) {
    if (_active) {
        return true;
    }

    _kind = kind;
    strlcpy(_reason, (reason && reason[0]) ? reason : _kindName(kind),
            sizeof(_reason));

    if (!_ownerMatches(kind)) {
        CONTRACT_WARN_ONCE(kind == STORAGE_WINDOW_FIELDVAULT_UPLOAD
                               ? CONTRACT_FIELDVAULT_UPLOAD_USES_QUIET_WINDOW
                               : CONTRACT_MAINTENANCE_OWNER_FOR_REPAIR,
                           "STORAGE",
                           false,
                           "window=%s owner=%s reason=%s",
                           _kindName(kind),
                           RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                           _reason);
        return false;
    }

    // drainAndPauseWorker pauses the worker unconditionally (sets the pause
    // flag before draining), so the worker is paused regardless of whether
    // the drain completed in time. Track that fact independently so end()
    // always resumes the worker — otherwise a drain timeout would leave the
    // worker permanently paused after the window closes.
    const bool drainOk = RAMSpool::drainAndPauseWorker(5000);
    _workerPaused = true;
    if (!drainOk) {
        DLOG_WARN("STORAGE",
                  "Exclusive window worker pause timed out kind=%s reason=%s",
                  _kindName(kind),
                  _reason);
    }

    if (taskDisplayHandle) {
        vTaskSuspend(taskDisplayHandle);
        _displaySuspended = true;
    }

    _active = true;
    DLOG_INFO("STORAGE",
              "Exclusive window begin kind=%s reason=%s workerPaused=%u displaySuspended=%u",
              _kindName(kind),
              _reason,
              _workerPaused ? 1U : 0U,
              _displaySuspended ? 1U : 0U);
    return true;
}

void StorageExclusiveWindow::end(const char* result) {
    if (!_active) {
        return;
    }

    if (_displaySuspended && taskDisplayHandle) {
        vTaskResume(taskDisplayHandle);
    }
    _displaySuspended = false;

    if (_workerPaused) {
        RAMSpool::resumeWorker();
    }
    _workerPaused = false;

    DLOG_INFO("STORAGE",
              "Exclusive window end kind=%s result=%s reason=%s",
              _kindName(_kind),
              (result && result[0]) ? result : "-",
              _reason);
    _active = false;
    _reason[0] = '\0';
}
