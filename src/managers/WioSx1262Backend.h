#pragma once

#include "ISubGhzBackend.h"
#include "WioNrfAccessory.h"

// Sub-GHz backend that delegates to the WIO nRF accessory's onboard SX1262.
//
// The nRF firmware drives the radio as a thin modem (SPECTRE/1 SUBGHZ_* verbs);
// this backend maps the existing text-oriented ISubGhzBackend interface onto
// that raw bridge for Spectre-native LoRa.  It reports ready only once the WIO
// CAPS handshake confirms SX1262_PRESENT, so SubGhzManager prefers it over the
// Reyax backend when the WIO accessory is attached and falls back otherwise.
class WioSx1262Backend : public ISubGhzBackend {
public:
    explicit WioSx1262Backend(WioNrfAccessory& wio) : _wio(wio) {}

    bool begin() override;
    void tick() override;
    bool isReady() const override { return _ready; }

    SubGhzBackendType backendType() const override {
        return SubGhzBackendType::SX126X_DIRECT;
    }

    const char* backendName() const override { return "WIO_SX1262"; }
    const char* moduleName() const override { return "SX1262"; }

    SubGhzCapabilities capabilities() const override;

    bool setMode(SubGhzMode mode) override;
    SubGhzMode mode() const override { return _mode; }

    bool available() override;
    bool readPacket(SubGhzPacket& outPacket) override;
    bool send(const char* payload, uint16_t destination = 0) override;

    bool setFrequencyHz(uint32_t hz) override;
    uint32_t frequencyHz() const override { return _frequencyHz; }
    bool applyProfile(const SubGhzRadioProfile& profile) override;
    SubGhzRadioProfile profile() const override { return _profile; }

    String firmwareVersion() const override { return _fwVersion; }
    SubGhzStats stats() const override { return _stats; }

private:
    // Push the cached profile (freq/sf/bw/cr/preamble + native sync word) to
    // the modem over the SUBGHZ_CONFIG verb.
    bool _pushConfig();
    // RYLR998-style bandwidth index (as stored in RuntimeSettings.loraBW) to Hz.
    static uint32_t _bwIndexToHz(uint8_t index);
    uint32_t _beaconIntervalMsForMode() const;
    void _emitBeacon(const char* label);

    WioNrfAccessory& _wio;
    bool _ready = false;
    SubGhzMode _mode = SubGhzMode::OFF;
    uint32_t _frequencyHz = 0;
    uint32_t _bandwidthHz = 125000;  // resolved Hz for the SX1262 PHY
    int8_t   _powerDbm = SUBGHZ_SX1262_TX_POWER_DBM;
    SubGhzRadioProfile _profile;
    SubGhzStats _stats;
    String _fwVersion;

    WioNrfAccessory::SubGhzRxFrame _pending = {};
    bool _hasPending = false;
    bool _yielded = false;  // true while the Meshtastic client owns the radio
    uint32_t _lastBeaconMs = 0;
    uint32_t _beaconSeq = 0;
};
