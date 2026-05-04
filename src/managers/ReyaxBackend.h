

#pragma once

#include "ISubGhzBackend.h"
#include "LoRaManager.h"

class ReyaxBackend : public ISubGhzBackend {
public:
    explicit ReyaxBackend(LoRaManager& radio) : _radio(radio) {}

    bool begin() override;
    void tick() override;
    bool isReady() const override { return _ready; }

    SubGhzBackendType backendType() const override {
        return SubGhzBackendType::REYAX_AT;
    }

    const char* backendName() const override {
        return "REYAX";
    }

    const char* moduleName() const override {
        return "RYLR";
    }

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
    void _emitBeacon(const char* label);
    uint32_t _beaconIntervalMsForMode() const;

    LoRaManager& _radio;
    bool _ready = false;
    SubGhzMode _mode = SubGhzMode::OFF;
    uint32_t _frequencyHz = 0;
    String _fwVersion;
    SubGhzStats _stats;
    SubGhzRadioProfile _profile;
    uint32_t _lastBeaconMs = 0;
    uint32_t _beaconSeq = 0;
};



