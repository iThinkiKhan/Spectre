

#pragma once

#include "SubGhzTypes.h"

class ISubGhzBackend {
public:
    virtual ~ISubGhzBackend() = default;

    virtual bool begin() = 0;
    virtual void tick() = 0;
    virtual bool isReady() const = 0;

    virtual SubGhzBackendType backendType() const = 0;
    virtual const char* backendName() const = 0;
    virtual const char* moduleName() const = 0;
    virtual SubGhzCapabilities capabilities() const = 0;

    virtual bool setMode(SubGhzMode mode) = 0;
    virtual SubGhzMode mode() const = 0;

    virtual bool available() = 0;
    virtual bool readPacket(SubGhzPacket& outPacket) = 0;
    virtual bool send(const char* payload, uint16_t destination = 0) = 0;

    virtual bool setFrequencyHz(uint32_t hz) = 0;
    virtual uint32_t frequencyHz() const = 0;
    virtual bool applyProfile(const SubGhzRadioProfile& profile) = 0;
    virtual SubGhzRadioProfile profile() const = 0;

    virtual String firmwareVersion() const = 0;
    virtual SubGhzStats stats() const = 0;
};



