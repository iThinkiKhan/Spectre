
#pragma once

#include <Arduino.h>
#include "../config.h"

// OWNERSHIP CONTRACT
// - TaskHardware is the only caller that may mutate radio ownership.
// - RadioArbiter is the single authority for lease transitions.
// - Other subsystems derive behavior from the current owner; they do not
//   self-assign the radio.

enum RadioOwner : uint8_t {
    RADIO_NONE = 0,
    RADIO_WIFI_CAPTURE,
    RADIO_WIFI_SCAN,
    RADIO_WIFI_PMKID,
    RADIO_WIFI_UPLOAD,
    RADIO_STORAGE_MAINTENANCE,
    RADIO_BLE_TEXT,
    RADIO_BLE_GPS
};

class RadioArbiter {
public:
    static constexpr uint32_t LEASE_INFINITE = 0;
    // Tune via config.h BLE_*_HOLD_SEC.
    static constexpr uint32_t BLE_TEXT_ACTIVE_HOLD_MS  = BLE_TEXT_ACTIVE_HOLD_MS_VAL;
    static constexpr uint32_t BLE_TEXT_IDLE_HOLD_MS    = BLE_TEXT_IDLE_HOLD_MS_VAL;
    static constexpr uint32_t BLE_PHONE_PROBE_HOLD_MS  = BLE_PHONE_PROBE_HOLD_MS_VAL;
    static constexpr uint32_t BLE_PHONE_ENRICH_HOLD_MS = BLE_PHONE_ENRICH_HOLD_MS_VAL;

    void begin();
    void tick();

    // General lease request — callers should prefer the typed helpers below
    // where one exists.  Raw RADIO_WIFI_PMKID leases are rejected; callers
    // must use requestPmkidHunt() or requestPwnyLease().
    bool requestLease(RadioOwner owner,
                      uint32_t holdMs,
                      const char* reason,
                      bool force = false);

    bool requestPhoneProbeLease(const char* reason,
                                uint32_t holdMs,
                                bool force = false);
    bool requestUploadLease(uint32_t holdMs,
                            const char* reason,
                            bool force = false);
    // Adopt an upload radio that was started before TaskHardware during a
    // state-preserving field-offload reboot. This records ownership without
    // reconfiguring the already-running private AP as a station.
    bool adoptPrestartedUploadLease(uint32_t holdMs, const char* reason);
    bool requestStorageMaintenanceLease(uint32_t holdMs,
                                        const char* reason,
                                        bool force = false);
    bool requestPmkidHunt(const char* targetBssid,
                          uint32_t holdMs,
                          const char* reason,
                          bool force = false);
    bool requestPwnyLease(uint32_t holdMs,
                          const char* reason,
                          bool force = false);

    void refreshLease(RadioOwner owner, uint32_t holdMs, const char* reason = nullptr);
    void release(RadioOwner owner,
                 const char* reason = nullptr,
                 bool serviceIdleOwner = true);
    bool ensureDefaultCapture(const char* reason = "default");
    void setFallbackOwner(RadioOwner owner, const char* reason = nullptr);
    RadioOwner fallbackOwner() const { return _fallbackOwner; }

    RadioOwner currentOwner() const { return _owner; }
    bool hasPendingOwner(RadioOwner owner) const { return _pendingOwner == owner; }
    bool isOwner(RadioOwner owner) const { return _owner == owner; }
    bool isReady() const { return _begun; }
    bool isBleOwner() const {
        return _owner == RADIO_BLE_TEXT || _owner == RADIO_BLE_GPS;
    }

    static const char* ownerName(RadioOwner owner);

private:
    enum WiFiPmkidIntent : uint8_t {
        WIFI_PMKID_INTENT_NONE = 0,
        WIFI_PMKID_INTENT_HUNT,
        WIFI_PMKID_INTENT_PWNY
    };

    uint8_t _priorityFor(RadioOwner owner) const;
    bool _isFallbackOwnerValid(RadioOwner owner) const;
    bool _canStartOwner(RadioOwner owner) const;
    bool _requestPmkidLease(WiFiPmkidIntent intent,
                            const char* targetBssid,
                            uint32_t holdMs,
                            const char* reason,
                            bool force);
    void _resetPmkidIntent();
    void _clearActiveOwnerState();
    void _commitOwnerState(RadioOwner owner, uint32_t holdMs, const char* reason);
    bool _startOwner(RadioOwner owner, const char* reason);
    void _stopOwner(RadioOwner owner, const char* reason);
    bool _switchTo(RadioOwner owner,
                   uint32_t holdMs,
                   const char* reason,
                   bool force = false);
    void _serviceIdleOwner(const char* reason);
    void _queuePending(RadioOwner owner, uint32_t holdMs, const char* reason, bool force);
    void _clearPending();
    void _setLease(uint32_t holdMs);
    void _log(const char* action, RadioOwner owner, const char* reason, uint32_t holdMs) const;

    // Death-loop watchdog. _recordGrantForChurn() tracks a streak of back-to-back
    // grants of the same owner; _serviceChurnWatchdog() (run from tick) escalates
    // a wedged streak: first a fallback kick (gate bypass), then a reboot.
    void _recordGrantForChurn(RadioOwner owner);
    void _serviceChurnWatchdog();
    bool _captureGateBypassActive() const;

    bool _begun = false;

    RadioOwner _owner        = RADIO_NONE;
    RadioOwner _fallbackOwner = RADIO_WIFI_CAPTURE;
    uint32_t   _leaseDeadlineMs = 0;
    uint32_t   _lastSwitchMs    = 0;
    uint32_t   _nextIdleRetryMs = 0;
    bool       _fallbackSuppressed = false;
    // Consecutive failed default-capture starts, for retry backoff only.
    // Capture is the default state and is always retried; this never latches
    // the fallback off (see ensureDefaultCapture).
    uint8_t    _captureStartFailures = 0;
    char       _reason[40] = "";

    RadioOwner _pendingOwner  = RADIO_NONE;
    uint32_t   _pendingHoldMs = 0;
    bool       _pendingForce  = false;
    char       _pendingReason[40] = "";

    WiFiPmkidIntent _pmkidIntent = WIFI_PMKID_INTENT_NONE;
    char            _pmkidTargetBssid[18] = "";

    // Death-loop watchdog state (see config.h RADIO_CHURN_*).
    RadioOwner _churnOwner = RADIO_NONE;     // owner of the current grant streak
    uint32_t   _churnCount = 0;              // consecutive same-owner re-grants
    uint32_t   _lastGrantMs = 0;             // millis() of the last grant
    uint32_t   _captureGateBypassUntilMs = 0;// soft maintenance gate bypassed until
    uint32_t   _churnKickCooldownMs = 0;     // don't re-arm a kick before this
};

extern RadioArbiter RADIO_ARB;

