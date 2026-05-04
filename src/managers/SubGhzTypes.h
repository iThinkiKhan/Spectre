

#pragma once

#include <Arduino.h>
#include <stdint.h>

enum class SubGhzBackendType : uint8_t {
    NONE = 0,
    REYAX_AT,
    SX126X_DIRECT,
    UNKNOWN
};

enum class SubGhzMode : uint8_t {
    OFF = 0,
    MONITOR,
    DISCOVER,
    BEACON,
    MESSAGE,
    TEST
};

enum SubGhzCapability : uint32_t {
    SUBGHZ_CAP_NONE            = 0,
    SUBGHZ_CAP_RX              = 1u << 0,
    SUBGHZ_CAP_TX              = 1u << 1,
    SUBGHZ_CAP_ADDR_FILTER     = 1u << 2,
    SUBGHZ_CAP_BROADCAST       = 1u << 3,
    SUBGHZ_CAP_FREQ_CONFIG     = 1u << 4,
    SUBGHZ_CAP_PROFILE_CONFIG  = 1u << 5,
    SUBGHZ_CAP_RSSI_SNR        = 1u << 6
};

struct SubGhzRadioProfile {
    uint32_t frequencyHz = 0;
    uint16_t networkId = 0;
    uint16_t address = 0;
    uint8_t  spreadingFactor = 0;
    uint8_t  bandwidth = 0;
    uint8_t  codingRate = 0;
    uint8_t  preamble = 0;
};

struct SubGhzCapabilities {
    uint32_t flags = SUBGHZ_CAP_NONE;

    bool has(uint32_t cap) const {
        return (flags & cap) != 0;
    }
};

enum class SubGhzPacketKind : uint8_t {
    UNKNOWN = 0,
    DATA,
    TELEMETRY,
    BEACON,
    TEST
};

struct SubGhzPacket {
    uint16_t source = 0;
    uint16_t destination = 0;
    uint16_t networkId = 0;
    uint16_t localAddress = 0;
    uint16_t length = 0;
    int16_t  rssi = 0;
    int16_t  snr = 0;
    uint32_t frequencyHz = 0;
    uint32_t timestampMs = 0;
    bool     broadcast = false;
    SubGhzBackendType backendType = SubGhzBackendType::NONE;
    SubGhzMode mode = SubGhzMode::OFF;
    SubGhzPacketKind kind = SubGhzPacketKind::UNKNOWN;
    char     backendName[24] = "";
    char     moduleName[24] = "";
    uint8_t  spreadingFactor = 0;
    uint8_t  bandwidth = 0;
    uint8_t  codingRate = 0;
    uint8_t  preamble = 0;
    char     payload[96] = "";
};

struct SubGhzStats {
    uint32_t rxPackets = 0;
    uint32_t txPackets = 0;
    uint32_t rxDropped = 0;
    uint32_t txFailed = 0;
    int16_t  lastRSSI = 0;
    int16_t  lastSNR = 0;
    uint16_t nodesSeen = 0;
    uint16_t lastSource = 0;
    uint16_t lastDestination = 0;
    uint32_t lastFrequencyHz = 0;
    uint32_t lastPacketMs = 0;
};

struct SubGhzNodeSeen {
    bool     valid = false;
    uint16_t address = 0;
    int16_t  lastRSSI = 0;
    int16_t  lastSNR = 0;
    uint32_t firstSeenMs = 0;
    uint32_t lastSeenMs = 0;
    uint32_t packetCount = 0;
};

struct SubGhzStatus {
    bool ready = false;
    bool modulePresent = false;
    bool rxActive = false;
    bool txActive = false;
    SubGhzBackendType backendType = SubGhzBackendType::NONE;
    SubGhzMode mode = SubGhzMode::OFF;
    uint32_t frequencyHz = 0;
    char backendName[24] = "";
    char moduleName[24] = "";
    char firmware[32] = "";
    uint16_t nodeCount = 0;
    uint16_t lastSource = 0;
    uint32_t lastPacketMs = 0;
    SubGhzRadioProfile profile = {};
};

static inline const char* subGhzModeName(SubGhzMode mode) {
    switch (mode) {
        case SubGhzMode::OFF:      return "OFF";
        case SubGhzMode::MONITOR:  return "MONITOR";
        case SubGhzMode::DISCOVER: return "DISCOVER";
        case SubGhzMode::BEACON:   return "BEACON";
        case SubGhzMode::MESSAGE:  return "MESSAGE";
        case SubGhzMode::TEST:     return "TEST";
        default:                   return "UNKNOWN";
    }
}

static inline const char* subGhzBackendName(SubGhzBackendType type) {
    switch (type) {
        case SubGhzBackendType::NONE:         return "NONE";
        case SubGhzBackendType::REYAX_AT:     return "REYAX";
        case SubGhzBackendType::SX126X_DIRECT:return "SX126X";
        default:                              return "UNKNOWN";
    }
}

static inline const char* subGhzPacketKindName(SubGhzPacketKind kind) {
    switch (kind) {
        case SubGhzPacketKind::DATA:      return "DATA";
        case SubGhzPacketKind::TELEMETRY: return "TELE";
        case SubGhzPacketKind::BEACON:    return "BEACON";
        case SubGhzPacketKind::TEST:      return "TEST";
        case SubGhzPacketKind::UNKNOWN:
        default:                          return "UNKNOWN";
    }
}



