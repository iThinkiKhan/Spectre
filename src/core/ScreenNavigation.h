#pragma once

#include <cstddef>
#include <cstdint>

#include "ScreenEnum.h"

// General-mode pages are ordered by field usefulness. SCREEN_MISSION is an
// exclusive runtime view entered from the mission launcher and is therefore
// intentionally absent from this carousel.
static constexpr Screen GENERAL_SCREEN_ORDER[] = {
    SCREEN_RECON,
    SCREEN_WIFI,
    SCREEN_LORA,
    SCREEN_MESHTASTIC,
    SCREEN_BLE,
    SCREEN_SYSTEM,
    SCREEN_BADUSB,
    SCREEN_MISSION_SUMMARY,
};

static constexpr size_t GENERAL_SCREEN_COUNT =
    sizeof(GENERAL_SCREEN_ORDER) / sizeof(GENERAL_SCREEN_ORDER[0]);
static constexpr Screen DEFAULT_GENERAL_SCREEN = GENERAL_SCREEN_ORDER[0];

static_assert(GENERAL_SCREEN_COUNT == static_cast<size_t>(SCREEN_COUNT - 1),
              "General carousel must include every screen except SCREEN_MISSION");

constexpr Screen nextGeneralScreen(Screen screen) {
    for (size_t i = 0; i < GENERAL_SCREEN_COUNT; ++i) {
        if (GENERAL_SCREEN_ORDER[i] == screen) {
            return GENERAL_SCREEN_ORDER[(i + 1U) % GENERAL_SCREEN_COUNT];
        }
    }
    return DEFAULT_GENERAL_SCREEN;
}

constexpr uint8_t generalScreenOrdinal(Screen screen) {
    for (size_t i = 0; i < GENERAL_SCREEN_COUNT; ++i) {
        if (GENERAL_SCREEN_ORDER[i] == screen) {
            return static_cast<uint8_t>(i + 1U);
        }
    }
    return 0;
}

static_assert(nextGeneralScreen(SCREEN_RECON) == SCREEN_WIFI,
              "Mission launcher must lead into field entities");
static_assert(nextGeneralScreen(SCREEN_MISSION_SUMMARY) == SCREEN_RECON,
              "General carousel must wrap back to the mission launcher");
static_assert(generalScreenOrdinal(SCREEN_SYSTEM) == 6,
              "System health page position changed unexpectedly");
static_assert(generalScreenOrdinal(SCREEN_MISSION) == 0,
              "Active mission view must not appear in the general carousel");
