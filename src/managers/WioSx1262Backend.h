#pragma once

#include "ISubGhzBackend.h"
#include "WioNrfAccessory.h"

// Sub-GHz backend that delegates to the WIO nRF accessory's onboard SX1262.
//
// The wire protocol (SPECTRE/1 SUBGHZ_*) is not yet implemented on the WIO
// firmware side; this class registers a placeholder so SubGhzManager picks it
// up the moment CAPS reports SX1262_PRESENT.  Until then, begin() returns
// false and the existing Reyax backend remains the active sub-GHz radio.
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

    bool available() override { return false; }
    bool readPacket(SubGhzPacket& outPacket) override;
    bool send(const char* payload, uint16_t destination = 0) override;

    bool setFrequencyHz(uint32_t hz) override;
    uint32_t frequencyHz() const override { return _frequencyHz; }
    bool applyProfile(const SubGhzRadioProfile& profile) override;
    SubGhzRadioProfile profile() const override { return _profile; }

    String firmwareVersion() const override { return _fwVersion; }
    SubGhzStats stats() const override { return _stats; }

private:
    WioNrfAccessory& _wio;
    bool _ready = false;
    SubGhzMode _mode = SubGhzMode::OFF;
    uint32_t _frequencyHz = 0;
    SubGhzRadioProfile _profile;
    SubGhzStats _stats;
    String _fwVersion;
};
