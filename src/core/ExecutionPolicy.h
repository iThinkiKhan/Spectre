#pragma once

#include "../core/ScreenEnum.h"
#include "../core/RunContext.h"
#include "../core/SpectreState.h"
#include "../managers/RadioArbiter.h"

namespace ExecutionPolicy {

struct StatusBarStateView {
    int     battPercent = 0;
    uint16_t runtimeMinutes = 0;
    uint8_t powerSource = POWER_SOURCE_UNKNOWN;
    uint8_t powerState = POWER_STATE_BATTERY_NORMAL;
    bool    charging = false;
    bool    wifiConnected = false;
    bool    bleConnected = false;
    bool    loraActive = false;
    uint8_t radioOwner = static_cast<uint8_t>(RADIO_NONE);
};

struct UiRefreshMarks {
    uint32_t wifiMs = 0;
    uint32_t pwnyMs = 0;
    uint32_t systemMs = 0;
};

struct UiRefreshSchedule {
    uint32_t wifiIntervalMs = 2000;
    uint32_t pwnyIntervalMs = 750;
    uint32_t systemIntervalMs = 1000;
};

enum UiRefreshReason : uint8_t {
    UI_REFRESH_NONE = 0,
    UI_REFRESH_WIFI,
    UI_REFRESH_PWNY,
    UI_REFRESH_SYSTEM
};

constexpr bool elapsed(uint32_t nowMs, uint32_t lastMs, uint32_t intervalMs) {
    return (nowMs - lastMs) > intervalMs;
}

constexpr bool shouldTickWiFi(RadioOwner owner) {
    return owner == RADIO_WIFI_CAPTURE ||
           owner == RADIO_WIFI_SCAN ||
           owner == RADIO_WIFI_PMKID;
}

constexpr bool shouldTickBle(RadioOwner owner) {
    return owner == RADIO_BLE_TEXT ||
           owner == RADIO_BLE_GPS;
}

constexpr bool statusBarChanged(const StatusBarStateView& previous,
                                const StatusBarStateView& next) {
    return previous.battPercent != next.battPercent ||
           previous.runtimeMinutes != next.runtimeMinutes ||
           previous.powerSource != next.powerSource ||
           previous.powerState != next.powerState ||
           previous.charging != next.charging ||
           previous.wifiConnected != next.wifiConnected ||
           previous.bleConnected != next.bleConnected ||
           previous.loraActive != next.loraActive ||
           previous.radioOwner != next.radioOwner;
}

constexpr UiRefreshReason dueUiRefresh(Screen currentScreen,
                                       bool debriefActive,
                                       MissionProfile missionProfile,
                                       uint32_t nowMs,
                                       const UiRefreshMarks& marks,
                                       const UiRefreshSchedule& schedule) {
    if (currentScreen == SCREEN_WIFI &&
        elapsed(nowMs, marks.wifiMs, schedule.wifiIntervalMs)) {
        return UI_REFRESH_WIFI;
    }

    if (currentScreen == SCREEN_MISSION &&
        missionProfile == MISSION_RECON &&
        elapsed(nowMs, marks.wifiMs, schedule.wifiIntervalMs)) {
        return UI_REFRESH_WIFI;
    }

    if (currentScreen == SCREEN_MISSION &&
        missionProfile == MISSION_PWNY &&
        elapsed(nowMs, marks.pwnyMs, schedule.pwnyIntervalMs)) {
        return UI_REFRESH_PWNY;
    }

    if ((currentScreen == SCREEN_SYSTEM ||
         (currentScreen == SCREEN_MISSION &&
          missionProfile == MISSION_UPLINK) ||
         debriefActive) &&
        elapsed(nowMs, marks.systemMs, schedule.systemIntervalMs)) {
        return UI_REFRESH_SYSTEM;
    }

    return UI_REFRESH_NONE;
}

inline void markUiRefresh(UiRefreshReason reason,
                          uint32_t nowMs,
                          UiRefreshMarks& marks) {
    switch (reason) {
        case UI_REFRESH_WIFI:
            marks.wifiMs = nowMs;
            break;
        case UI_REFRESH_PWNY:
            marks.pwnyMs = nowMs;
            break;
        case UI_REFRESH_SYSTEM:
            marks.systemMs = nowMs;
            break;
        case UI_REFRESH_NONE:
        default:
            break;
    }
}

}  // namespace ExecutionPolicy



