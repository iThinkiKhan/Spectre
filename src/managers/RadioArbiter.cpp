#include "RadioArbiter.h"

#include "../core/CrashBreadcrumb.h"
#include "../core/DebugLog.h"
#include "../core/SpectreState.h"
#include "BLEManager.h"
#include "MQTTManager.h"
#include "StorageManager.h"
#include "WiFiManager.h"

namespace {
constexpr const char* TAG = "RADIO";
}

RadioArbiter RADIO_ARB;

void RadioArbiter::begin() {
    if (_begun) {
        return;
    }

    _begun = true;
    _owner = RADIO_NONE;
    _fallbackOwner = RADIO_WIFI_CAPTURE;
    _leaseDeadlineMs = 0;
    _lastSwitchMs = millis();
    _nextIdleRetryMs = 0;
    _fallbackSuppressed = false;
    _reason[0] = '\0';
    _pmkidIntent = WIFI_PMKID_INTENT_NONE;
    _pmkidTargetBssid[0] = '\0';
    _clearPending();
    DLOG_INFO(TAG, "arbiter ready");
}

void RadioArbiter::tick() {
    if (!_begun) {
        return;
    }

    const uint32_t now = millis();
    if (_owner != RADIO_NONE && _leaseDeadlineMs != 0 &&
        static_cast<int32_t>(now - _leaseDeadlineMs) >= 0) {
        release(_owner, "lease expired");
    }

    const bool idleWorkPending =
        _pendingOwner != RADIO_NONE ||
        (_fallbackOwner != RADIO_NONE && !_fallbackSuppressed);
    if (_owner == RADIO_NONE &&
        idleWorkPending &&
        (_nextIdleRetryMs == 0 ||
         static_cast<int32_t>(now - _nextIdleRetryMs) >= 0)) {
        _serviceIdleOwner("idle fallback");
    }
}

bool RadioArbiter::requestLease(RadioOwner owner,
                                uint32_t holdMs,
                                const char* reason,
                                bool force) {
    if (!_begun) {
        begin();
    }

    if (owner == RADIO_NONE) {
        return false;
    }

    if (!_canStartOwner(owner)) {
        DLOG_WARN(TAG,
                  "reject owner=%s reason=%s missing startup context",
                  ownerName(owner),
                  (reason && reason[0] != '\0') ? reason : "-");
        return false;
    }

    if (owner == RADIO_WIFI_UPLOAD && !MQTT_MGR.uploadLeaseReady(force)) {
        DLOG_INFO(TAG,
                  "defer owner=%s hold=%lu reason=%s queued=%d threshold=%d force=%d",
                  ownerName(owner),
                  static_cast<unsigned long>(holdMs),
                  (reason && reason[0] != '\0') ? reason : "-",
                  MQTT_MGR.uploadReadyCount(),
                  MQTT_UPLOAD_READY_THRESHOLD,
                  force ? 1 : 0);
        _queuePending(owner, holdMs, reason, force);
        return false;
    }

    if (_owner == owner) {
        refreshLease(owner, holdMs, reason);
        return true;
    }

    if (_owner == RADIO_NONE) {
        return _switchTo(owner, holdMs, reason);
    }

    const uint8_t incomingPriority = _priorityFor(owner);
    const uint8_t currentPriority  = _priorityFor(_owner);

    if (incomingPriority > currentPriority) {
        return _switchTo(owner, holdMs, reason);
    }

    _queuePending(owner, holdMs, reason, force);
    _log("defer", owner, reason, holdMs);
    return false;
}

bool RadioArbiter::requestPhoneProbeLease(const char* reason,
                                          uint32_t holdMs,
                                          bool force) {
    return requestLease(RADIO_BLE_GPS, holdMs, reason, force);
}

bool RadioArbiter::requestUploadLease(uint32_t holdMs,
                                      const char* reason,
                                      bool force) {
    return requestLease(RADIO_WIFI_UPLOAD, holdMs, reason, force);
}

bool RadioArbiter::requestPmkidHunt(const char* targetBssid,
                                    uint32_t holdMs,
                                    const char* reason,
                                    bool force) {
    return _requestPmkidLease(WIFI_PMKID_INTENT_HUNT,
                              targetBssid, holdMs, reason, force);
}

bool RadioArbiter::requestPwnyLease(uint32_t holdMs,
                                    const char* reason,
                                    bool force) {
    return _requestPmkidLease(WIFI_PMKID_INTENT_PWNY,
                              nullptr, holdMs, reason, force);
}

void RadioArbiter::refreshLease(RadioOwner owner, uint32_t holdMs, const char* reason) {
    if (_owner != owner) {
        return;
    }

    _setLease(holdMs);
    if (reason && reason[0] != '\0') {
        strlcpy(_reason, reason, sizeof(_reason));
    }
}

void RadioArbiter::release(RadioOwner owner,
                           const char* reason,
                           bool serviceIdleOwner) {
    if (_owner != owner || _owner == RADIO_NONE) {
        return;
    }

    _stopOwner(_owner, reason ? reason : "release");
    _log("release", _owner, reason ? reason : "release", 0);

    if (owner == RADIO_WIFI_PMKID) {
        _resetPmkidIntent();
    }
    _clearActiveOwnerState();
    _lastSwitchMs = millis();
    if (serviceIdleOwner) {
        crashCheckpointVolatile(CrashPhase::RADIO_RESUME,
                                static_cast<uint8_t>(owner),
                                STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U);
        _serviceIdleOwner(reason ? reason : "release");
        crashBreadcrumbClearVolatile(CrashPhase::RADIO_RESUME);
    }
}

bool RadioArbiter::ensureDefaultCapture(const char* reason) {
    if (_owner == RADIO_WIFI_CAPTURE) {
        return true;
    }
    if (_owner != RADIO_NONE) {
        return false;
    }
    if (_pendingOwner != RADIO_NONE) {
        return false;
    }
    const bool granted = _switchTo(RADIO_WIFI_CAPTURE, LEASE_INFINITE, reason);
    if (!granted) {
        _fallbackSuppressed = true;
        _nextIdleRetryMs = 0;
        DLOG_WARN(TAG, "fallback suppressed after capture start failed reason=%s",
                  (reason && reason[0] != '\0') ? reason : "-");
    }
    return granted;
}

void RadioArbiter::setFallbackOwner(RadioOwner owner, const char* reason) {
    if (!_isFallbackOwnerValid(owner)) {
        DLOG_WARN(TAG, "reject fallback owner=%s", ownerName(owner));
        return;
    }

    const RadioOwner previous = _fallbackOwner;
    _fallbackOwner = owner;
    _fallbackSuppressed = false;

    DLOG_INFO(TAG, "fallback owner=%s prev=%s reason=%s",
              ownerName(_fallbackOwner),
              ownerName(previous),
              (reason && reason[0] != '\0') ? reason : "-");

    if (_owner == previous && _owner != _fallbackOwner) {
        release(_owner, reason ? reason : "fallback_change");
        return;
    }

    if (_owner == RADIO_NONE &&
        (_pendingOwner != RADIO_NONE || _fallbackOwner != RADIO_NONE)) {
        _serviceIdleOwner(reason ? reason : "fallback_change");
    }
}

const char* RadioArbiter::ownerName(RadioOwner owner) {
    switch (owner) {
        case RADIO_WIFI_CAPTURE: return "WIFI_CAPTURE";
        case RADIO_WIFI_SCAN:    return "WIFI_SCAN";
        case RADIO_WIFI_PMKID:   return "WIFI_PMKID";
        case RADIO_WIFI_UPLOAD:  return "WIFI_UPLOAD";
        case RADIO_BLE_TEXT:     return "BLE_TEXT";
        case RADIO_BLE_GPS:      return "BLE_GPS";
        default:                 return "NONE";
    }
}

uint8_t RadioArbiter::_priorityFor(RadioOwner owner) const {
    switch (owner) {
        case RADIO_WIFI_UPLOAD:  return 220;
        case RADIO_BLE_TEXT:     return 180;
        case RADIO_BLE_GPS:      return 160;
        case RADIO_WIFI_PMKID:   return 140;
        case RADIO_WIFI_SCAN:    return 120;
        case RADIO_WIFI_CAPTURE: return 100;
        default:                 return 0;
    }
}

bool RadioArbiter::_isFallbackOwnerValid(RadioOwner owner) const {
    return owner == RADIO_NONE || owner == RADIO_WIFI_CAPTURE;
}

bool RadioArbiter::_canStartOwner(RadioOwner owner) const {
    switch (owner) {
        case RADIO_WIFI_PMKID:
            return _pmkidIntent == WIFI_PMKID_INTENT_PWNY ||
                   (_pmkidIntent == WIFI_PMKID_INTENT_HUNT &&
                    _pmkidTargetBssid[0] != '\0');
        case RADIO_WIFI_UPLOAD:
        case RADIO_WIFI_CAPTURE:
        case RADIO_WIFI_SCAN:
        case RADIO_BLE_TEXT:
        case RADIO_BLE_GPS:
            return true;
        case RADIO_NONE:
        default:
            return false;
    }
}

bool RadioArbiter::_requestPmkidLease(WiFiPmkidIntent intent,
                                      const char* targetBssid,
                                      uint32_t holdMs,
                                      const char* reason,
                                      bool force) {
    if (intent == WIFI_PMKID_INTENT_HUNT &&
        (!targetBssid || !targetBssid[0])) {
        DLOG_WARN(TAG, "reject pmkid hunt without target");
        return false;
    }

    const bool targetChanged =
        intent == WIFI_PMKID_INTENT_HUNT &&
        strcmp(_pmkidTargetBssid, targetBssid) != 0;
    const bool reconfigureActiveOwner =
        _owner == RADIO_WIFI_PMKID &&
        (_pmkidIntent != intent || targetChanged);
    if (reconfigureActiveOwner) {
        _stopOwner(RADIO_WIFI_PMKID, "pmkid_reconfigure");
        _log("release", RADIO_WIFI_PMKID, "pmkid_reconfigure", 0);
        _clearActiveOwnerState();
    }

    _pmkidIntent = intent;
    if (intent == WIFI_PMKID_INTENT_HUNT) {
        strlcpy(_pmkidTargetBssid, targetBssid, sizeof(_pmkidTargetBssid));
    } else {
        _pmkidTargetBssid[0] = '\0';
    }

    const bool granted = requestLease(RADIO_WIFI_PMKID, holdMs, reason, force);
    if (!granted &&
        _owner == RADIO_NONE &&
        _pendingOwner == RADIO_NONE) {
        _serviceIdleOwner("pmkid_request_failed");
    }
    return granted;
}

void RadioArbiter::_resetPmkidIntent() {
    _pmkidIntent = WIFI_PMKID_INTENT_NONE;
    _pmkidTargetBssid[0] = '\0';
}

void RadioArbiter::_clearActiveOwnerState() {
    _owner = RADIO_NONE;
    STATE_WRITE_BEGIN();
    g_state.radioOwner = static_cast<uint8_t>(RADIO_NONE);
    STATE_WRITE_END();
    _leaseDeadlineMs = 0;
    _reason[0] = '\0';
}

void RadioArbiter::_commitOwnerState(RadioOwner owner,
                                     uint32_t holdMs,
                                     const char* reason) {
    _owner = owner;
    STATE_WRITE_BEGIN();
    g_state.radioOwner = static_cast<uint8_t>(owner);
    STATE_WRITE_END();
    _setLease(holdMs);
    _nextIdleRetryMs = 0;
    _fallbackSuppressed = false;
    strlcpy(_reason, reason ? reason : "", sizeof(_reason));
    _lastSwitchMs = millis();
    _log("grant", owner, reason ? reason : "grant", holdMs);

    if (owner == RADIO_WIFI_CAPTURE) {
        crashCheckpoint(CrashPhase::WIFI_CAPTURE,
                        static_cast<uint8_t>(owner),
                        STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U);
    }
}

bool RadioArbiter::_startOwner(RadioOwner owner, const char* reason) {
    switch (owner) {
        case RADIO_WIFI_CAPTURE:
            return WIFI_MGR.startPromiscuous();
        case RADIO_WIFI_SCAN:
            return WIFI_MGR.startScan();
        case RADIO_WIFI_PMKID:
            if (_pmkidIntent == WIFI_PMKID_INTENT_PWNY) {
                return WIFI_MGR.startPwnyMode();
            }
            if (_pmkidIntent == WIFI_PMKID_INTENT_HUNT &&
                _pmkidTargetBssid[0] != '\0') {
                return WIFI_MGR.startPMKIDHunt(_pmkidTargetBssid);
            }
            DLOG_ERROR(TAG, "PMKID owner missing intent");
            return false;
        case RADIO_WIFI_UPLOAD:
            WIFI_MGR.pauseRadio();
            return true;
        case RADIO_BLE_TEXT:
        case RADIO_BLE_GPS:
            WIFI_MGR.suspendRadio();
            // suspendRadio() completes the full WiFi driver teardown and
            // drains in-flight disconnect events before returning — no
            // additional settle needed here.
            if (!BLE_MGR.begin()) {
                DLOG_ERROR(TAG, "BLE begin failed");
                release(owner, "ble_begin_failed");
                return false;
            }
            BLE_MGR.setRadioEnabled(true);
            return true;
        case RADIO_NONE:
        default:
            return true;
    }
}

void RadioArbiter::_stopOwner(RadioOwner owner, const char* reason) {
    switch (owner) {
        case RADIO_WIFI_CAPTURE:
            WIFI_MGR.pauseRadio();
            crashBreadcrumbClear(CrashPhase::WIFI_CAPTURE);
            break;
        case RADIO_WIFI_SCAN:
        case RADIO_WIFI_PMKID:
        case RADIO_WIFI_UPLOAD:
            WIFI_MGR.pauseRadio();
            break;

        case RADIO_BLE_TEXT:
            if (BLE_MGR.isTextInputPending()) {
                BLE_MGR.cancelTextInput();
            }
            BLE_MGR.setRadioEnabled(false);
            break;

        case RADIO_BLE_GPS:
            // GPS probes are opportunistic. Fully release NimBLE and its worker
            // stack before WiFi capture resumes on long-running, low-heap units.
            BLE_MGR.shutdown();
            break;

        case RADIO_NONE:
        default:
            break;
    }

    if (reason && reason[0] != '\0') {
        DLOG_INFO(TAG, "owner %s stopped: %s", ownerName(owner), reason);
    }
}

bool RadioArbiter::_switchTo(RadioOwner owner, uint32_t holdMs, const char* reason) {
    const char* transitionReason = reason ? reason : "switch";
    const RadioOwner previous = _owner;

    if (previous != RADIO_NONE) {
        _stopOwner(previous, transitionReason);
        _clearActiveOwnerState();
    }

    // PMKID modes (pwny / hunt) call back into RADIO_ARB.isOwner() during
    // their start routines for contract checks.  Pre-claim the owner in state
    // before invoking _startOwner so those checks see the correct owner rather
    // than NONE.  _commitOwnerState re-assigns the same value on success
    // (idempotent); _clearActiveOwnerState rolls it back on failure.
    if (owner == RADIO_WIFI_PMKID) {
        _owner = owner;
        STATE_WRITE_BEGIN();
        g_state.radioOwner = static_cast<uint8_t>(owner);
        STATE_WRITE_END();
    }

    if (!_startOwner(owner, transitionReason)) {
        if (owner == RADIO_WIFI_PMKID) {
            _resetPmkidIntent();
        }
        _clearActiveOwnerState();
        _nextIdleRetryMs = millis() + 1000UL;
        DLOG_ERROR(TAG, "transition failed from=%s to=%s reason=%s",
                   ownerName(previous), ownerName(owner), transitionReason);
        return false;
    }

    _commitOwnerState(owner, holdMs, transitionReason);
    return true;
}

void RadioArbiter::_serviceIdleOwner(const char* reason) {
    if (_owner != RADIO_NONE) {
        return;
    }

    if (_pendingOwner != RADIO_NONE) {
        const RadioOwner pendingOwner = _pendingOwner;
        const uint32_t pendingHoldMs = _pendingHoldMs;
        const bool pendingForce = _pendingForce;
        char pendingReason[40];
        strlcpy(pendingReason, _pendingReason, sizeof(pendingReason));
        _clearPending();
        const bool granted =
            requestLease(pendingOwner, pendingHoldMs, pendingReason, pendingForce);

        if (!granted && _owner == RADIO_NONE) {
            if (pendingOwner == RADIO_WIFI_UPLOAD &&
                !MQTT_MGR.uploadLeaseReady(pendingForce)) {
                _clearPending();
            }
            _nextIdleRetryMs = millis() + 1000UL;
        }
        return;
    }

    if (_fallbackOwner == RADIO_WIFI_CAPTURE) {
        ensureDefaultCapture(reason);
    }
}

void RadioArbiter::_queuePending(RadioOwner owner, uint32_t holdMs, const char* reason, bool force) {
    if (_pendingOwner != RADIO_NONE &&
        _priorityFor(_pendingOwner) > _priorityFor(owner)) {
        return;
    }

    _pendingOwner = owner;
    _pendingHoldMs = holdMs;
    _pendingForce = force;
    strlcpy(_pendingReason, reason ? reason : "", sizeof(_pendingReason));
}

void RadioArbiter::_clearPending() {
    _pendingOwner = RADIO_NONE;
    _pendingHoldMs = 0;
    _pendingForce = false;
    _pendingReason[0] = '\0';
}

void RadioArbiter::_setLease(uint32_t holdMs) {
    if (holdMs == LEASE_INFINITE) {
        _leaseDeadlineMs = 0;
    } else {
        _leaseDeadlineMs = millis() + holdMs;
    }
}

void RadioArbiter::_log(const char* action,
                        RadioOwner owner,
                        const char* reason,
                        uint32_t holdMs) const {
    const char* safeAction = action ? action : "event";
    const char* safeReason =
        (reason && reason[0] != '\0') ? reason : "-";

    if (strcmp(safeAction, "defer") == 0) {
        DLOG_WARN(TAG,
                  "%s owner=%s current=%s hold=%lu pending=%s reason=%s",
                  safeAction,
                  ownerName(owner),
                  ownerName(_owner),
                  static_cast<unsigned long>(holdMs),
                  ownerName(_pendingOwner),
                  safeReason);
        return;
    }

    DLOG_INFO(TAG,
              "%s owner=%s current=%s hold=%lu reason=%s",
              safeAction,
              ownerName(owner),
              ownerName(_owner),
              static_cast<unsigned long>(holdMs),
              safeReason);
}
