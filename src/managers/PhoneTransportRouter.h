#pragma once

#include <Arduino.h>

#include "../config.h"
#include "protocol/CompanionProtocol.h"
#include "PhoneTransport.h"

// Keep concrete transport managers out of router consumers.
class BLEManager;
class WioNrfAccessory;

// Chooses WIO BLE proxy when available, otherwise internal ESP32-S3 BLE.
class PhoneTransportRouter {
public:
    void begin();
    void tick();

    PhoneTransportKind kind() const { return _state.kind; }
    bool isWioActive()      const { return _state.kind == PhoneTransportKind::WioBle; }
    bool isInternalActive() const { return _state.kind == PhoneTransportKind::InternalBle; }
    bool isNone()           const { return _state.kind == PhoneTransportKind::None; }

    // Pass-through readiness checks.  Internal BLE preserves its granular
    // per-characteristic readiness; WIO uses its single isPhoneCompanionReady
    // gate for every channel (current behavior — granular WIO readiness is a
    // later slice).
    bool isPhoneLinkReady()       const;
    bool isPhoneGpsReady()        const;
    bool isPhoneControlReady()    const;
    bool isPhoneEnrichmentReady() const;
    bool isPhoneStorageReady()    const;
    bool isPhoneCompanionReady()  const;

    bool hasFreshGpsFix()                       const;
    bool getBestTimeEpoch(uint32_t& epochUtc)   const;

    // Storage snapshot publish on whichever transport is active.  Returns
    // false if the active transport rejects the frame (e.g. link not ready).
    bool publishStorageSnapshot(const PhoneStorageFrameV1& frame);

    // Log-stream chunk publish (slice #3).  `plain` is a LogStreamChunkV1
    // header + packed null-terminated lines; the active transport encrypts
    // on PHONE_SECURE_CHANNEL_LOG_STREAM and writes to the phone's log
    // stream characteristic.
    bool publishLogStreamChunk(const uint8_t* plain, size_t plainLen);

    // Dashboard-stream chunk publish (slice #4).  `plain` is a
    // DashboardStreamChunkV1Header + CmdDashboardSnapshotV1 payload;
    // encrypted on PHONE_SECURE_CHANNEL_DASHBOARD_STREAM.
    bool publishDashboardStreamChunk(const uint8_t* plain, size_t plainLen);

    // Notification publish (slice #7).  `plain` is a
    // PhoneNotificationV1Header + optional UTF-8 text; encrypted on
    // PHONE_SECURE_CHANNEL_NOTIFICATION.
    bool publishNotification(const uint8_t* plain, size_t plainLen);

    // Text-input dispatcher.  Internal BLE ignores the optional cancel
    // reason; WIO records it for serial diagnostics.
    bool requestTextInput(const char* prompt,
                          uint32_t timeoutMs = 120000UL);
    bool consumeTextInput(char* out, size_t outLen);
    void cancelTextInput(const char* reason = nullptr);
    bool isTextInputPending() const;
    bool isTextInputReady()   const;

    const PhoneTransportState& state() const { return _state; }
    static const char* kindName(PhoneTransportKind kind);

private:
    PhoneTransportKind _evaluateKind() const;
    void _emitTransitionLog(PhoneTransportKind newKind,
                            const char* reason) const;

    PhoneTransportState _state;
    bool                _begun = false;
};

extern PhoneTransportRouter PHONE_XPORT;
