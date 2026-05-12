


#pragma once

#include <Arduino.h>
#include <string.h>

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

inline int debugLevelRank(char level) {
    switch (sanitizeDebugLevel(level)) {
        case DEBUG_LEVEL_ERROR:   return 3;
        case DEBUG_LEVEL_WARN:    return 2;
        case DEBUG_LEVEL_INFO:    return 1;
        case DEBUG_LEVEL_VERBOSE:
        default:                  return 0;
    }
}

// Map a log tag to the subsystem area bit it belongs to. Kept inline so the
// early DLOG gate can short-circuit without extra translation units.
inline uint32_t debugAreaMaskForTag(const char* tag) {
    if (!tag || !tag[0]) {
        return DEBUG_AREA_GENERAL;
    }
    if (strcmp(tag, "SYS") == 0 ||
        strcmp(tag, "CORE") == 0 ||
        strcmp(tag, "STACK") == 0 ||
        strcmp(tag, "HEAP") == 0 ||
        strcmp(tag, "BTN") == 0) {
        return DEBUG_AREA_CORE;
    }
    if (strcmp(tag, "SETTINGS") == 0) {
        return DEBUG_AREA_SETTINGS;
    }
    if (strcmp(tag, "STOR") == 0 ||
        strcmp(tag, "STORAGE") == 0) {
        return DEBUG_AREA_STORAGE;
    }
    if (strcmp(tag, "TIME") == 0) {
        return DEBUG_AREA_TIME;
    }
    if (strcmp(tag, "RADIO") == 0 ||
        strcmp(tag, "LORA") == 0 ||
        strcmp(tag, "SUBGHZ") == 0) {
        return DEBUG_AREA_RADIO;
    }
    if (strcmp(tag, "WIFI") == 0 ||
        strcmp(tag, "ANT") == 0 ||
        strcmp(tag, "DRONE") == 0) {
        return DEBUG_AREA_WIFI;
    }
    if (strcmp(tag, "BLE") == 0) {
        return DEBUG_AREA_BLE;
    }
    if (strcmp(tag, "MQTT") == 0) {
        return DEBUG_AREA_MQTT;
    }
    if (strcmp(tag, "EXPORT") == 0) {
        return DEBUG_AREA_EXPORT;
    }
    if (strcmp(tag, "GPS") == 0) {
        return DEBUG_AREA_GPS;
    }
    if (strcmp(tag, "MODE") == 0 ||
        strcmp(tag, "MISSION") == 0 ||
        strcmp(tag, "UI") == 0) {
        return DEBUG_AREA_MODE;
    }
    return DEBUG_AREA_GENERAL;
}

enum DebugProfile : uint8_t {
    DEBUG_PROFILE_OFF   = 0,
    DEBUG_PROFILE_RUN   = 1,
    DEBUG_PROFILE_DEBUG = 2,
    DEBUG_PROFILE_DEV   = 3,
};


