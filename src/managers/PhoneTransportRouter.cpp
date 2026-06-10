#include "PhoneTransportRouter.h"

#include "../core/DebugLog.h"
#include "BLEManager.h"
#include "WioNrfAccessory.h"

namespace {
constexpr const char* TAG = "XPORT";
}

PhoneTransportRouter PHONE_XPORT;

const char* PhoneTransportRouter::kindName(PhoneTransportKind kind) {
    switch (kind) {
        case PhoneTransportKind::WioBle:      return "wio_ble";
        case PhoneTransportKind::InternalBle: return "internal_ble";
        case PhoneTransportKind::None:
        default:                              return "none";
    }
}

void PhoneTransportRouter::begin() {
    if (_begun) {
        return;
    }
    _begun = true;
    _state.previous     = PhoneTransportKind::None;
    _state.transitions  = 0;

    // Seed the initial kind synchronously so accessors called between begin()
    // and the first tick() see the correct transport, not None.
    const PhoneTransportKind initial = _evaluateKind();
    const uint32_t now = millis();
    _state.kind         = initial;
    _state.selectedAtMs = now;
    _state.lastChangeMs = now;
    _emitTransitionLog(initial, "initial");
}

PhoneTransportKind PhoneTransportRouter::_evaluateKind() const {
#if WIO_NRF_ACCESSORY_ENABLED
    if (WIO_NRF.available() && WIO_NRF.hasBleProxy()) {
        return PhoneTransportKind::WioBle;
    }
#endif
    return PhoneTransportKind::InternalBle;
}

void PhoneTransportRouter::_emitTransitionLog(PhoneTransportKind newKind,
                                              const char* reason) const {
#if WIO_NRF_ACCESSORY_ENABLED
    const int wioAvailable = WIO_NRF.available() ? 1 : 0;
    const int wioProxy     = WIO_NRF.hasBleProxy() ? 1 : 0;
#else
    const int wioAvailable = 0;
    const int wioProxy     = 0;
#endif
    DLOG_INFO(TAG,
              "transport=%s previous=%s reason=%s wio.available=%d wio.proxy=%d ble.begun=%d ble.state=%u transitions=%lu",
              kindName(newKind),
              kindName(_state.kind),
              reason ? reason : "unknown",
              wioAvailable,
              wioProxy,
              BLE_MGR.isBegun() ? 1 : 0,
              static_cast<unsigned>(BLE_MGR.getState()),
              static_cast<unsigned long>(_state.transitions));
}

void PhoneTransportRouter::tick() {
    if (!_begun) {
        begin();
        return;
    }

    const PhoneTransportKind newKind = _evaluateKind();
    if (newKind == _state.kind) {
        return;
    }

    const uint32_t now = millis();

    const char* reason = "unknown";
    if (newKind == PhoneTransportKind::WioBle) {
        reason = "wio_proxy_acquired";
    } else if (newKind == PhoneTransportKind::InternalBle) {
#if WIO_NRF_ACCESSORY_ENABLED
        reason = WIO_NRF.available() ? "wio_proxy_lost" : "wio_unavailable";
#else
        reason = "wio_disabled";
#endif
    }

    _state.transitions++;
    _emitTransitionLog(newKind, reason);
    _state.previous     = _state.kind;
    _state.kind         = newKind;
    _state.lastChangeMs = now;
    _state.selectedAtMs = now;
}

bool PhoneTransportRouter::isPhoneLinkReady() const {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        return WIO_NRF.isPhoneCompanionReady();
    }
#endif
    return BLE_MGR.isPhoneLinkReady();
}

bool PhoneTransportRouter::isPhoneGpsReady() const {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        // Preserves today's behavior at publishCompanionState: WIO GPS
        // readiness requires both a live companion link and a fresh fix.
        return WIO_NRF.isPhoneCompanionReady() && WIO_NRF.hasFreshGpsFix();
    }
#endif
    return BLE_MGR.isPhoneGpsReady();
}

bool PhoneTransportRouter::isPhoneControlReady() const {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        return WIO_NRF.isPhoneCompanionReady();
    }
#endif
    return BLE_MGR.isPhoneControlReady();
}

bool PhoneTransportRouter::isPhoneEnrichmentReady() const {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        return WIO_NRF.isPhoneCompanionReady();
    }
#endif
    return BLE_MGR.isPhoneEnrichmentReady();
}

bool PhoneTransportRouter::isPhoneStorageReady() const {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        return WIO_NRF.isPhoneCompanionReady();
    }
#endif
    return BLE_MGR.isPhoneStorageReady();
}

bool PhoneTransportRouter::isPhoneCompanionReady() const {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        return WIO_NRF.isPhoneCompanionReady();
    }
#endif
    return BLE_MGR.isPhoneCompanionReady();
}

bool PhoneTransportRouter::hasFreshGpsFix() const {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        return WIO_NRF.hasFreshGpsFix();
    }
#endif
    return BLE_MGR.hasFreshGpsFix();
}

bool PhoneTransportRouter::getBestTimeEpoch(uint32_t& epochUtc) const {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        return WIO_NRF.getBestTimeEpoch(epochUtc);
    }
#endif
    return BLE_MGR.getBestTimeEpoch(epochUtc);
}

bool PhoneTransportRouter::publishStorageSnapshot(const PhoneStorageFrameV1& frame) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        return WIO_NRF.publishStorageSnapshot(frame);
    }
#endif
    return BLE_MGR.publishStorageSnapshot(frame);
}

bool PhoneTransportRouter::publishLogStreamChunk(const uint8_t* plain,
                                                 size_t plainLen) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        return WIO_NRF.publishLogStreamChunk(plain, plainLen);
    }
#endif
    return BLE_MGR.publishLogStreamChunk(plain, plainLen);
}

bool PhoneTransportRouter::publishDashboardStreamChunk(const uint8_t* plain,
                                                       size_t plainLen) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        return WIO_NRF.publishDashboardStreamChunk(plain, plainLen);
    }
#endif
    return BLE_MGR.publishDashboardStreamChunk(plain, plainLen);
}

bool PhoneTransportRouter::publishNotification(const uint8_t* plain,
                                               size_t plainLen) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        return WIO_NRF.publishNotification(plain, plainLen);
    }
#endif
    return BLE_MGR.publishNotification(plain, plainLen);
}

bool PhoneTransportRouter::requestTextInput(const char* prompt,
                                            uint32_t timeoutMs) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        return WIO_NRF.requestTextInput(prompt, timeoutMs);
    }
#endif
    return BLE_MGR.requestTextInput(prompt, timeoutMs);
}

bool PhoneTransportRouter::consumeTextInput(char* out, size_t outLen) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        return WIO_NRF.consumeTextInput(out, outLen);
    }
#endif
    return BLE_MGR.consumeTextInput(out, outLen);
}

void PhoneTransportRouter::cancelTextInput(const char* reason) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        WIO_NRF.cancelTextInput(reason);
        return;
    }
#endif
    (void)reason;  // internal BLE doesn't surface the cancellation reason.
    BLE_MGR.cancelTextInput();
}

bool PhoneTransportRouter::isTextInputPending() const {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        return WIO_NRF.isTextInputPending();
    }
#endif
    return BLE_MGR.isTextInputPending();
}

bool PhoneTransportRouter::isTextInputReady() const {
#if WIO_NRF_ACCESSORY_ENABLED
    if (isWioActive()) {
        return WIO_NRF.isTextInputReady();
    }
#endif
    return BLE_MGR.isTextInputReady();
}
