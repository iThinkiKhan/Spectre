
#include "MissionRuntime.h"

#include "DebugLog.h"
#include "ScreenEnum.h"
#include "SpectreState.h"
#include "../managers/MQTTManager.h"
#include "../managers/RadioArbiter.h"
#include "../managers/SubGhzManager.h"
#include "../managers/SubGhzTypes.h"
#include "../managers/WiFiManager.h"

namespace {

SubGhzMode _sanitizeSubGhzMode(uint8_t raw) {
    switch (static_cast<SubGhzMode>(raw)) {
        case SubGhzMode::OFF:
        case SubGhzMode::MONITOR:
        case SubGhzMode::DISCOVER:
        case SubGhzMode::BEACON:
        case SubGhzMode::MESSAGE:
        case SubGhzMode::TEST:
            return static_cast<SubGhzMode>(raw);
        default:
            return SubGhzMode::OFF;
    }
}

void _applyMissionConfiguration(MissionProfile profile) {
    switch (profile) {
        case MISSION_RECON:
            RADIO_ARB.ensureDefaultCapture("mission_recon");
            if (SUBGHZ.isReady() && SUBGHZ.mode() == SubGhzMode::OFF) {
                SUBGHZ.setMode(SubGhzMode::MONITOR);
            }
            break;
        case MISSION_PWNY:
            WIFI_MGR.pauseRadio();
            break;
        case MISSION_UPLINK:
            WIFI_MGR.pauseRadio();
            MQTT_MGR.requestDump(true);
            break;
        default:
            break;
    }
}

Screen _sanitizeScreen(uint8_t raw) {
    if (raw >= static_cast<uint8_t>(SCREEN_COUNT)) {
        return SCREEN_LORA;
    }
    return static_cast<Screen>(raw);
}

MascotState _missionMascot(MissionProfile profile) {
    switch (profile) {
        case MISSION_RECON:  return MASCOT_RECON_WALK;
        case MISSION_PWNY:   return MASCOT_PWNY;
        case MISSION_UPLINK: return MASCOT_HOMELAB_SYNC;
        default:             return MASCOT_STANDBY;
    }
}

MascotState _generalMascot(Screen screen) {
    switch (screen) {
        case SCREEN_LORA:       return MASCOT_LORA_RECON;
        case SCREEN_WIFI:       return MASCOT_WIFI_RECON;
        case SCREEN_BADUSB:     return MASCOT_BAD_USB;
        case SCREEN_RECON:      return MASCOT_PREFLIGHT;
        case SCREEN_MISSION:    return MASCOT_PREFLIGHT;
        case SCREEN_MESHTASTIC:
        case SCREEN_SYSTEM:
        default:                return MASCOT_STANDBY;
    }
}

}  // namespace

RunContext currentRunContext() {
    STATE_READ_BEGIN();
    const RunContext context = sanitizeRunContext(g_state.runContext);
    STATE_READ_END();
    return context;
}

MissionProfile activeMissionProfile() {
    STATE_READ_BEGIN();
    const MissionProfile profile = sanitizeMissionProfile(g_state.activeMissionProfile);
    STATE_READ_END();
    return profile;
}

bool isMissionActive() {
    return currentRunContext() == RUN_CONTEXT_MISSION;
}

const char* currentSessionContextLabel() {
    RunContext context = RUN_CONTEXT_GENERAL;
    MissionProfile profile = MISSION_RECON;

    STATE_READ_BEGIN();
    context = sanitizeRunContext(g_state.runContext);
    profile = sanitizeMissionProfile(g_state.activeMissionProfile);
    STATE_READ_END();

    return sessionContextLabel(context, profile);
}

void syncRuntimePresentation() {
    RunContext context = RUN_CONTEXT_GENERAL;
    MissionProfile profile = MISSION_RECON;
    Screen screen = SCREEN_LORA;
    RadioOwner owner = RADIO_NONE;
    uint8_t powerState = POWER_STATE_BATTERY_NORMAL;

    STATE_READ_BEGIN();
    context = sanitizeRunContext(g_state.runContext);
    profile = sanitizeMissionProfile(g_state.activeMissionProfile);
    screen = g_state.currentScreen;
    owner = static_cast<RadioOwner>(g_state.radioOwner);
    powerState = g_state.powerState;
    STATE_READ_END();

    MascotState nextMascot = MASCOT_STANDBY;
    if (powerState == POWER_STATE_BATTERY_CRITICAL) {
        nextMascot = MASCOT_LOW_BATTERY;
    } else if (owner == RADIO_BLE_TEXT) {
        nextMascot = MASCOT_TRANSMIT;
    } else if (owner == RADIO_WIFI_SCAN) {
        nextMascot = MASCOT_SCANNING;
    } else if (context == RUN_CONTEXT_MISSION) {
        nextMascot = _missionMascot(profile);
    } else {
        nextMascot = _generalMascot(screen);
    }

    STATE_WRITE_BEGIN();
    g_state.mascotState = nextMascot;
    STATE_WRITE_END();
}

bool enterMission(MissionProfile profile) {
    const MissionProfile sanitized = sanitizeMissionProfile(
        static_cast<uint8_t>(profile));
    const SubGhzMode generalSubGhzMode = SUBGHZ.mode();

    STATE_WRITE_BEGIN();
    g_state.generalSubGhzMode = static_cast<uint8_t>(generalSubGhzMode);
    g_state.runContext = RUN_CONTEXT_MISSION;
    g_state.activeMissionProfile = static_cast<uint8_t>(sanitized);
    g_state.missionSelection = static_cast<uint8_t>(sanitized);
    g_state.generalScreen = static_cast<uint8_t>(g_state.currentScreen);
    g_state.currentScreen = SCREEN_MISSION;
    g_state.wifiListActive = false;
    g_state.missionListActive = false;
    g_state.badUsbListActive = false;
    g_state.debriefActive = false;
    g_state.screenChanged = true;
    g_state.dataRefresh = true;
    STATE_WRITE_END();

    _applyMissionConfiguration(sanitized);
    syncRuntimePresentation();
    DLOG_INFO("MISSION", "Entered %s",
              missionProfileName(sanitized));
    return true;
}

void exitMission() {
    MissionProfile mission = MISSION_RECON;
    SubGhzMode restoreSubGhzMode = SubGhzMode::OFF;
    Screen restoreScreen = SCREEN_RECON;

    STATE_READ_BEGIN();
    mission = sanitizeMissionProfile(g_state.activeMissionProfile);
    restoreSubGhzMode = _sanitizeSubGhzMode(g_state.generalSubGhzMode);
    restoreScreen = _sanitizeScreen(g_state.generalScreen);
    STATE_READ_END();

    if (mission == MISSION_RECON && SUBGHZ.isReady()) {
        SUBGHZ.setMode(restoreSubGhzMode);
    }
    if (mission == MISSION_PWNY && WIFI_MGR.isPwnyActive()) {
        WIFI_MGR.stopPwnyMode();
    }
    RADIO_ARB.ensureDefaultCapture("mission_exit");

    STATE_WRITE_BEGIN();
    g_state.runContext = RUN_CONTEXT_GENERAL;
    g_state.activeMissionProfile = static_cast<uint8_t>(MISSION_RECON);
    g_state.currentScreen = restoreScreen;
    g_state.generalScreen = static_cast<uint8_t>(restoreScreen);
    g_state.wifiListActive = false;
    g_state.missionListActive = false;
    g_state.badUsbListActive = false;
    g_state.debriefActive = false;
    g_state.screenChanged = true;
    g_state.dataRefresh = true;
    STATE_WRITE_END();

    syncRuntimePresentation();
    DLOG_INFO("MISSION", "Exited %s",
              missionProfileName(mission));
}

