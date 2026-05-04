

#pragma once

#include <Arduino.h>

enum RunContext : uint8_t {
    RUN_CONTEXT_GENERAL = 0,
    RUN_CONTEXT_MISSION,
};

enum MissionProfile : uint8_t {
    MISSION_RECON = 0,
    MISSION_PWNY,
    MISSION_UPLINK,
    MISSION_PROFILE_COUNT
};

inline const char* runContextName(RunContext context) {
    switch (context) {
        case RUN_CONTEXT_GENERAL: return "GENERAL";
        case RUN_CONTEXT_MISSION: return "MISSION";
        default:                  return "UNKNOWN";
    }
}

inline const char* missionProfileName(MissionProfile profile) {
    switch (profile) {
        case MISSION_RECON:  return "RECON";
        case MISSION_PWNY:   return "PWNY";
        case MISSION_UPLINK: return "UPLINK";
        default:             return "UNKNOWN";
    }
}

inline RunContext sanitizeRunContext(uint8_t raw) {
    if (raw > static_cast<uint8_t>(RUN_CONTEXT_MISSION)) {
        return RUN_CONTEXT_GENERAL;
    }
    return static_cast<RunContext>(raw);
}

inline MissionProfile sanitizeMissionProfile(uint8_t raw) {
    if (raw >= static_cast<uint8_t>(MISSION_PROFILE_COUNT)) {
        return MISSION_RECON;
    }
    return static_cast<MissionProfile>(raw);
}

inline const char* sessionContextLabel(RunContext context,
                                       MissionProfile profile = MISSION_RECON) {
    switch (context) {
        case RUN_CONTEXT_GENERAL:
            return "GENERAL";
        case RUN_CONTEXT_MISSION:
            switch (profile) {
                case MISSION_RECON:  return "MISSION_RECON";
                case MISSION_PWNY:   return "MISSION_PWNY";
                case MISSION_UPLINK: return "MISSION_UPLINK";
                default:             return "MISSION_UNKNOWN";
            }
        default:
            return "UNKNOWN";
    }
}



