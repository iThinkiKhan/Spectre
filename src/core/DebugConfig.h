

#pragma once

#include <Arduino.h>

enum DebugArea : uint32_t {
    DEBUG_AREA_GENERAL  = 1u << 0,
    DEBUG_AREA_CORE     = 1u << 1,
    DEBUG_AREA_SETTINGS = 1u << 2,
    DEBUG_AREA_STORAGE  = 1u << 3,
    DEBUG_AREA_TIME     = 1u << 4,
    DEBUG_AREA_RADIO    = 1u << 5,
    DEBUG_AREA_WIFI     = 1u << 6,
    DEBUG_AREA_BLE      = 1u << 7,
    DEBUG_AREA_MQTT     = 1u << 8,
    DEBUG_AREA_EXPORT   = 1u << 9,
    DEBUG_AREA_GPS      = 1u << 10,
    DEBUG_AREA_MODE     = 1u << 11,
    DEBUG_AREA_ALL      = 0xFFFFFFFFu
};

static constexpr uint32_t DEBUG_AREA_OPERATORS =
    DEBUG_AREA_GENERAL |
    DEBUG_AREA_CORE |
    DEBUG_AREA_SETTINGS |
    DEBUG_AREA_STORAGE |
    DEBUG_AREA_TIME |
    DEBUG_AREA_RADIO |
    DEBUG_AREA_WIFI |
    DEBUG_AREA_BLE |
    DEBUG_AREA_MQTT |
    DEBUG_AREA_EXPORT |
    DEBUG_AREA_GPS |
    DEBUG_AREA_MODE;

static constexpr char DEBUG_LEVEL_VERBOSE = 'D';
static constexpr char DEBUG_LEVEL_INFO    = 'I';
static constexpr char DEBUG_LEVEL_WARN    = 'W';
static constexpr char DEBUG_LEVEL_ERROR   = 'E';

inline char sanitizeDebugLevel(char level) {
    switch (level) {
        case DEBUG_LEVEL_VERBOSE:
        case DEBUG_LEVEL_INFO:
        case DEBUG_LEVEL_WARN:
        case DEBUG_LEVEL_ERROR:
            return level;
        default:
            return DEBUG_LEVEL_INFO;
    }
}



