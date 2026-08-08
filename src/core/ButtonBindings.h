#pragma once

// Button action vocabulary and per-screen binding tables.
//
// Extracted from config.h to keep that file focused on tunable knobs. Pulled in
// by config.h, so every translation unit that already includes config.h keeps
// these symbols transitively — no extra includes needed at call sites.

#include <cstdint>
#include "ScreenEnum.h"

enum SpectreButtonAction : uint8_t {
    BUTTON_ACTION_NONE = 0,
    BUTTON_ACTION_SCREEN_NEXT,
    BUTTON_ACTION_SUBGHZ_MODE_CYCLE,
    BUTTON_ACTION_LORA_PING,
    BUTTON_ACTION_SLEEP,
    BUTTON_ACTION_WIFI_REFRESH,
    BUTTON_ACTION_WIFI_SCAN_LIST,
    BUTTON_ACTION_WIFI_ALLSCAN,
    BUTTON_ACTION_WIFI_LIST_SELECT,
    BUTTON_ACTION_WIFI_LIST_DOWN,
    BUTTON_ACTION_WIFI_LIST_CLOSE,
    BUTTON_ACTION_WIFI_LIST_HUNT,
    BUTTON_ACTION_ANTENNA_TOGGLE,
    BUTTON_ACTION_SYSTEM_DEBRIEF,
    BUTTON_ACTION_SESSION_TAG,
    BUTTON_ACTION_SAVE_LOCATION,
    BUTTON_ACTION_MISSION_NEXT,
    BUTTON_ACTION_MISSION_ENTER,
    BUTTON_ACTION_MISSION_EXIT,
    BUTTON_ACTION_MISSION_LIST_OPEN,
    BUTTON_ACTION_MISSION_LIST_SELECT,
    BUTTON_ACTION_MISSION_LIST_DOWN,
    BUTTON_ACTION_MISSION_LIST_CLOSE,
    BUTTON_ACTION_UPLINK_TRIGGER,
    BUTTON_ACTION_BADUSB_LIST_OPEN,
    BUTTON_ACTION_BADUSB_LIST_SELECT,
    BUTTON_ACTION_BADUSB_LIST_DOWN,
    BUTTON_ACTION_BADUSB_LIST_CLOSE,
    BUTTON_ACTION_BADUSB_ARM,
    BUTTON_ACTION_BADUSB_RUN,
    BUTTON_ACTION_BADUSB_CANCEL,
    BUTTON_ACTION_PWNY_FORCE_DEAUTH,
    BUTTON_ACTION_DEBRIEF_EXPORT,
    BUTTON_ACTION_DEBRIEF_CLEAR,
    BUTTON_ACTION_DEBRIEF_BACK,
    BUTTON_ACTION_BLE_TEST,
    BUTTON_ACTION_MESH_TOGGLE,
    BUTTON_ACTION_MESH_SEND
};

struct ButtonBindingSet {
    SpectreButtonAction aShort;
    SpectreButtonAction aLong;
    SpectreButtonAction bLong;
    SpectreButtonAction bShort;
};

static inline const char* spectreButtonActionLabel(SpectreButtonAction action,
                                                   bool busy = false) {
    switch (action) {
        case BUTTON_ACTION_SUBGHZ_MODE_CYCLE: return "MODE";
        case BUTTON_ACTION_LORA_PING:         return "PING";
        case BUTTON_ACTION_SLEEP:             return "SLEEP";
        case BUTTON_ACTION_SCREEN_NEXT:       return "NEXT";
        case BUTTON_ACTION_WIFI_REFRESH:      return busy ? "BUSY" : "REFRESH";
        case BUTTON_ACTION_WIFI_SCAN_LIST:    return "LIST";
        case BUTTON_ACTION_WIFI_ALLSCAN:      return "ALLSCAN";
        case BUTTON_ACTION_WIFI_LIST_SELECT:  return "SELECT";
        case BUTTON_ACTION_WIFI_LIST_DOWN:    return "DOWN";
        case BUTTON_ACTION_WIFI_LIST_CLOSE:   return "EXIT";
        case BUTTON_ACTION_WIFI_LIST_HUNT:    return "HUNT";
        case BUTTON_ACTION_ANTENNA_TOGGLE:    return "ANT";
        case BUTTON_ACTION_SYSTEM_DEBRIEF:    return "DEBRIEF";
        case BUTTON_ACTION_SESSION_TAG:       return "TAG";
        case BUTTON_ACTION_SAVE_LOCATION:     return "SAVE";
        case BUTTON_ACTION_MISSION_NEXT:      return "NEXT";
        case BUTTON_ACTION_MISSION_ENTER:     return "LAUNCH";
        case BUTTON_ACTION_MISSION_EXIT:      return "EXIT";
        case BUTTON_ACTION_MISSION_LIST_OPEN: return "MISSIONS";
        case BUTTON_ACTION_MISSION_LIST_SELECT:return "SELECT";
        case BUTTON_ACTION_MISSION_LIST_DOWN: return "DOWN";
        case BUTTON_ACTION_MISSION_LIST_CLOSE:return "CLOSE";
        case BUTTON_ACTION_UPLINK_TRIGGER:    return "SYNC";
        case BUTTON_ACTION_DEBRIEF_EXPORT:    return "EXPORT";
        case BUTTON_ACTION_DEBRIEF_CLEAR:     return "CLEAR";
        case BUTTON_ACTION_DEBRIEF_BACK:      return "BACK";
        case BUTTON_ACTION_BADUSB_LIST_OPEN:  return "LIST";
        case BUTTON_ACTION_BADUSB_LIST_SELECT:return "SELECT";
        case BUTTON_ACTION_BADUSB_LIST_DOWN:  return "DOWN";
        case BUTTON_ACTION_BADUSB_LIST_CLOSE: return "CLOSE";
        case BUTTON_ACTION_BADUSB_ARM:        return "ARM";
        case BUTTON_ACTION_BADUSB_RUN:        return "RUN";
        case BUTTON_ACTION_BADUSB_CANCEL:     return "STOP";
        case BUTTON_ACTION_PWNY_FORCE_DEAUTH: return "DEAUTH";
        case BUTTON_ACTION_BLE_TEST:          return "ENRICH";
        case BUTTON_ACTION_MESH_TOGGLE:       return busy ? "MESH ON" : "MESH";
        case BUTTON_ACTION_MESH_SEND:         return "SEND";
        default:                              return nullptr;
    }
}

// Per-screen bindings. The layout rule, applied consistently everywhere:
//
//   A short  primary action of this page
//   A long   secondary / more deliberate action (SLEEP where nothing fits)
//   B long   drill-in: the page's list or detail view
//   B short  NEXT — the navigation anchor, identical on every general screen
//
// A+B held is a global SLEEP chord handled in TaskHardware, so it works from
// any screen and even with the display blanked; it is not part of this table.
static inline ButtonBindingSet spectreScreenBindings(Screen screen) {
    switch (screen) {
        case SCREEN_LORA:
            // Sub-GHz: cycle the mode, ping the band. No third radio action
            // exists, so A-long carries SLEEP.
            return {BUTTON_ACTION_SUBGHZ_MODE_CYCLE, BUTTON_ACTION_SLEEP,
                    BUTTON_ACTION_LORA_PING, BUTTON_ACTION_SCREEN_NEXT};
        case SCREEN_WIFI:
            return {BUTTON_ACTION_WIFI_REFRESH, BUTTON_ACTION_WIFI_ALLSCAN,
                    BUTTON_ACTION_WIFI_SCAN_LIST, BUTTON_ACTION_SCREEN_NEXT};
        case SCREEN_BADUSB:
            return {BUTTON_ACTION_BADUSB_ARM, BUTTON_ACTION_BADUSB_RUN,
                    BUTTON_ACTION_BADUSB_LIST_OPEN, BUTTON_ACTION_SCREEN_NEXT};
        case SCREEN_SYSTEM:
            // ANT replaces SESSION_TAG here: this page already displays the
            // antenna state in its RADIO row (".. EXT"/".. INT"), so toggling
            // it belongs with the readout. Tagging is a session concern and
            // lives on BOOT SUMMARY / DEBRIEF instead. ANTENNA_TOGGLE was
            // implemented but bound to no button on any screen before this.
            return {BUTTON_ACTION_SYSTEM_DEBRIEF, BUTTON_ACTION_ANTENNA_TOGGLE,
                    BUTTON_ACTION_UPLINK_TRIGGER, BUTTON_ACTION_SCREEN_NEXT};
        case SCREEN_MISSION_SUMMARY:
            // Boot summary reports records/pending, so a manual enrich pass is
            // the natural secondary. BLE_TEST was likewise unreachable before.
            return {BUTTON_ACTION_SESSION_TAG, BUTTON_ACTION_BLE_TEST,
                    BUTTON_ACTION_UPLINK_TRIGGER, BUTTON_ACTION_SCREEN_NEXT};
        case SCREEN_MESHTASTIC:
            return {BUTTON_ACTION_MESH_TOGGLE, BUTTON_ACTION_SLEEP,
                    BUTTON_ACTION_MESH_SEND, BUTTON_ACTION_SCREEN_NEXT};
        case SCREEN_RECON:
            // Was LAUNCH on both A-short and A-long, which burned the slot on a
            // duplicate. Long-press now sleeps, matching every other page that
            // has no distinct secondary.
            return {BUTTON_ACTION_MISSION_ENTER, BUTTON_ACTION_SLEEP,
                    BUTTON_ACTION_MISSION_LIST_OPEN, BUTTON_ACTION_SCREEN_NEXT};
        case SCREEN_MISSION:
            return {BUTTON_ACTION_NONE, BUTTON_ACTION_MISSION_EXIT,
                    BUTTON_ACTION_NONE, BUTTON_ACTION_NONE};
        default:
            return {BUTTON_ACTION_NONE, BUTTON_ACTION_SLEEP,
                    BUTTON_ACTION_NONE, BUTTON_ACTION_SCREEN_NEXT};
    }
}

static inline ButtonBindingSet spectreBadUsbListBindings() {
    return {BUTTON_ACTION_BADUSB_LIST_SELECT, BUTTON_ACTION_BADUSB_ARM,
            BUTTON_ACTION_BADUSB_LIST_CLOSE, BUTTON_ACTION_BADUSB_LIST_DOWN};
}

static inline ButtonBindingSet spectreWifiListBindings() {
    return {BUTTON_ACTION_WIFI_LIST_SELECT, BUTTON_ACTION_WIFI_LIST_HUNT,
            BUTTON_ACTION_WIFI_LIST_CLOSE, BUTTON_ACTION_WIFI_LIST_DOWN};
}

static inline ButtonBindingSet spectreMissionListBindings() {
    return {BUTTON_ACTION_MISSION_LIST_SELECT, BUTTON_ACTION_MISSION_ENTER,
            BUTTON_ACTION_MISSION_LIST_CLOSE, BUTTON_ACTION_MISSION_LIST_DOWN};
}

static inline ButtonBindingSet spectreDebriefBindings() {
    return {BUTTON_ACTION_DEBRIEF_EXPORT, BUTTON_ACTION_DEBRIEF_CLEAR,
            BUTTON_ACTION_NONE, BUTTON_ACTION_DEBRIEF_BACK};
}
