
#pragma once

#include "ScreenEnum.h"

// Single source of truth for screen-level metadata that's currently fanned
// out across DisplayManager (status-bar 3-letter tag, indexed by enum) and
// main.cpp (long name in log lines). Bindings still live in config.h next
// to the button enums; content-panel pointers stay private to DisplayManager.
//
// Indexing the table by Screen enum value is enforced by the static_assert
// below — reordering ScreenEnum without updating this table breaks the build
// instead of silently misaligning the status-bar tag.

struct ScreenInfo {
    const char* shortTag;   // 3-char status-bar label
    const char* longName;   // log/serial label
};

inline const ScreenInfo& screenInfo(Screen s) {
    static constexpr ScreenInfo kTable[SCREEN_COUNT] = {
        {"LRA", "LORA"},
        {"MSH", "MESHTASTIC"},
        {"WFI", "WIFI"},
        {"USB", "BADUSB"},
        {"MIS", "MISSION"},
        {"RCN", "RECON"},
        {"SYS", "SYSTEM"},
        {"SUM", "MISSION_SUMMARY"},
    };
    static_assert(sizeof(kTable) / sizeof(kTable[0]) == SCREEN_COUNT,
                  "ScreenInfo table must cover every Screen enum value");

    static constexpr ScreenInfo kUnknown = {"???", "UNKNOWN"};
    if (static_cast<unsigned>(s) >= SCREEN_COUNT) {
        return kUnknown;
    }
    return kTable[s];
}

inline const char* screenShortTag(Screen s) { return screenInfo(s).shortTag; }
inline const char* screenLongName(Screen s) { return screenInfo(s).longName; }
