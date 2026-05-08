#pragma once
#include "SecretsConfig.h"
#include "core/ScreenEnum.h"

// Spectre config
//
// Flip switches with ON/OFF (or 1/0). Time values are seconds unless the name
// ends in _MS.

// Shared switch values. ON is a macro for build-flag use; OFF is a typed
// constant so it does not collide with enum values like SubGhzMode::OFF.
#define ON  1
static constexpr uint8_t OFF = 0;

#define SPECTRE_SECONDS_TO_MS(seconds) ((uint32_t)(seconds) * 1000UL)

// -----------------------------------------------------------------------------
// Main switches
// -----------------------------------------------------------------------------

#define PHONE_COMPANION_ENABLED     ON
#define PHONE_COMPANION_ENRICH_THRESHOLD 270UL
#define PHONE_COMPANION_ENRICH_BATCH_MAX 18

#define BOOT_SEQUENCE_ENABLED       ON
#define BOOT_SEQUENCE_VERBOSE       OFF
#define BOOT_RECOVERY_ENABLED       ON
#define BOOT_RECOVERY_BUTTON_PIN    BTN_B
#define BOOT_RECOVERY_HOLD_MS       1500UL

#define BLE_SMOKE_ENABLED           OFF

#define PWNY_ACTIVE_ATTACKS_ENABLED OFF

// -----------------------------------------------------------------------------
// Timing
// -----------------------------------------------------------------------------

// Button timing needs sub-second precision, so it stays in milliseconds.
#define BUTTON_LONG_PRESS_MS          800UL   // ms
#define BUTTON_DEBOUNCE_MS            50UL    // ms

#define MQTT_DUMP_INTERVAL_SEC        7200UL  // seconds
#define MQTT_DUMP_INTERVAL_MS         SPECTRE_SECONDS_TO_MS(MQTT_DUMP_INTERVAL_SEC)
#define MQTT_WIFI_CONNECT_TIMEOUT_SEC 30UL    // seconds
#define MQTT_WIFI_CONNECT_TIMEOUT_MS  SPECTRE_SECONDS_TO_MS(MQTT_WIFI_CONNECT_TIMEOUT_SEC)
#define MQTT_BROKER_CONNECT_TIMEOUT_SEC 15UL  // seconds
#define MQTT_BROKER_CONNECT_TIMEOUT_MS SPECTRE_SECONDS_TO_MS(MQTT_BROKER_CONNECT_TIMEOUT_SEC)
#define MQTT_FAILED_BACKOFF_SEC       300UL   // seconds
#define MQTT_FAILED_BACKOFF_MS        SPECTRE_SECONDS_TO_MS(MQTT_FAILED_BACKOFF_SEC)
#define MQTT_POISON_FAIL_LIMIT        3

#define SLEEP_TIMEOUT_SEC             300UL   // seconds
#define SLEEP_TIMEOUT_MS              SPECTRE_SECONDS_TO_MS(SLEEP_TIMEOUT_SEC)
#define BACKLIGHT_TIMEOUT_SEC         30UL    // seconds
#define BACKLIGHT_TIMEOUT_MS          SPECTRE_SECONDS_TO_MS(BACKLIGHT_TIMEOUT_SEC)

#define POWER_CRITICAL_SLEEP_COUNTDOWN_SEC  300UL  // seconds
#define POWER_CRITICAL_SLEEP_COUNTDOWN_MS   SPECTRE_SECONDS_TO_MS(POWER_CRITICAL_SLEEP_COUNTDOWN_SEC)

// -----------------------------------------------------------------------------
// MQTT upload
// -----------------------------------------------------------------------------

#define MQTT_UPLOAD_READY_THRESHOLD   7000
#define MQTT_DUMP_FETCH_BATCH_SIZE    16   // records loaded per storage scan
#define MQTT_DUMP_RECORDS_PER_SLICE    4   // max publish calls per yield
#define MQTT_DUMP_SLICE_BUDGET_MS     25   // ms
#define MQTT_DUMP_PROGRESS_EVERY_N     64  // events per progress log
#define MQTT_DUMP_CHECKPOINT_EVERY_N  250  // events per flash checkpoint

// Lease = connect budget + pending events * per-event budget, clamped to min/max.
#define MQTT_UPLOAD_LEASE_CONNECT_SEC   20UL    // seconds
#define MQTT_UPLOAD_LEASE_CONNECT_MS    SPECTRE_SECONDS_TO_MS(MQTT_UPLOAD_LEASE_CONNECT_SEC)
#define MQTT_UPLOAD_LEASE_MS_PER_EVENT  600UL   // ms
#define MQTT_UPLOAD_LEASE_MIN_SEC       90UL    // seconds
#define MQTT_UPLOAD_LEASE_MIN_MS        SPECTRE_SECONDS_TO_MS(MQTT_UPLOAD_LEASE_MIN_SEC)
#define MQTT_UPLOAD_LEASE_MAX_SEC       1200UL  // seconds
#define MQTT_UPLOAD_LEASE_MAX_MS        SPECTRE_SECONDS_TO_MS(MQTT_UPLOAD_LEASE_MAX_SEC)

// One-shot startup FieldVault upload. After boot grace, if FieldVault has
// pending records, fire a single field-only MQTT upload. On success the live
// FieldVault file is cleared. On failure (no broker, publish error) the
// records remain and retry on the next normal/manual/threshold dump. There is
// no periodic FieldVault-only retry loop — exactly one attempt per boot.
#define MQTT_FIELDVAULT_STARTUP_UPLOAD_ENABLED  ON
#define MQTT_FIELDVAULT_STARTUP_GRACE_SEC       30UL    // seconds after boot before attempt
#define MQTT_FIELDVAULT_STARTUP_GRACE_MS        SPECTRE_SECONDS_TO_MS(MQTT_FIELDVAULT_STARTUP_GRACE_SEC)
#define MQTT_FIELDVAULT_STARTUP_MAX_RECORDS     8       // cap per startup attempt
#define MQTT_FIELDVAULT_STARTUP_LEASE_MS        30000UL // short upload lease

// One-shot maintenance wipe. Change the tag before turning this ON again.
#define STORAGE_ONE_SHOT_NON_VAULT_RESET_ENABLED OFF
#define STORAGE_ONE_SHOT_NON_VAULT_RESET_TAG     "5-3-26-reset-backlog-1k"
#define STORAGE_FAST_BOOT_DEFER_SPOOL_REPAIR     ON

// -----------------------------------------------------------------------------
// Debug logging
// -----------------------------------------------------------------------------

// Profiles: OFF=silent, RUN=warnings/errors, DEBUG=targeted info, DEV=everything.
#define SPECTRE_DEBUG_PROFILE_OFF       0
#define SPECTRE_DEBUG_PROFILE_RUN       1
#define SPECTRE_DEBUG_PROFILE_DEBUG     2
#define SPECTRE_DEBUG_PROFILE_DEV       3
//-------------------------------------------------------------------------------
#ifndef SPECTRE_DEBUG_PROFILE
#define SPECTRE_DEBUG_PROFILE           SPECTRE_DEBUG_PROFILE_DEV
#endif

// Area toggles matter in DEBUG only. RUN ignores them and still logs warnings/errors.
#ifndef SPECTRE_DEBUG_AREAS_ALL
  #if defined(SPECTRE_DEBUG_AREAS_ALL_ENABLED)
    #define SPECTRE_DEBUG_AREAS_ALL     ON
  #else
    #define SPECTRE_DEBUG_AREAS_ALL     ON
  #endif
#endif

#ifndef SPECTRE_DEBUG_AREAS_NONE
  #if defined(SPECTRE_DEBUG_AREAS_NONE_ENABLED)
    #define SPECTRE_DEBUG_AREAS_NONE    ON
  #else
    #define SPECTRE_DEBUG_AREAS_NONE    OFF
  #endif
#endif

#if (SPECTRE_DEBUG_AREAS_ALL == ON) && (SPECTRE_DEBUG_AREAS_NONE == ON)
  #error "Set only one of SPECTRE_DEBUG_AREAS_ALL or SPECTRE_DEBUG_AREAS_NONE"
#endif

#if (SPECTRE_DEBUG_AREAS_ALL == ON)
  #define _SPECTRE_DEBUG_AREA_DEFAULT   ON
#else
  #define _SPECTRE_DEBUG_AREA_DEFAULT   ON
#endif

#ifndef SPECTRE_DEBUG_AREA_GENERAL
#define SPECTRE_DEBUG_AREA_GENERAL      _SPECTRE_DEBUG_AREA_DEFAULT  // catch-all / unmatched tags
#endif
#ifndef SPECTRE_DEBUG_AREA_CORE
#define SPECTRE_DEBUG_AREA_CORE         _SPECTRE_DEBUG_AREA_DEFAULT  // SYS, CORE, STACK, HEAP, BTN
#endif
#ifndef SPECTRE_DEBUG_AREA_SETTINGS
#define SPECTRE_DEBUG_AREA_SETTINGS     _SPECTRE_DEBUG_AREA_DEFAULT  // SETTINGS
#endif
#ifndef SPECTRE_DEBUG_AREA_STORAGE
#define SPECTRE_DEBUG_AREA_STORAGE      _SPECTRE_DEBUG_AREA_DEFAULT  // STOR, STORAGE
#endif
#ifndef SPECTRE_DEBUG_AREA_TIME
#define SPECTRE_DEBUG_AREA_TIME         _SPECTRE_DEBUG_AREA_DEFAULT  // TIME
#endif
#ifndef SPECTRE_DEBUG_AREA_RADIO
#define SPECTRE_DEBUG_AREA_RADIO        _SPECTRE_DEBUG_AREA_DEFAULT  // RADIO, LORA, SUBGHZ
#endif
#ifndef SPECTRE_DEBUG_AREA_WIFI
#define SPECTRE_DEBUG_AREA_WIFI         _SPECTRE_DEBUG_AREA_DEFAULT  // WIFI, ANT, DRONE
#endif
#ifndef SPECTRE_DEBUG_AREA_BLE
#define SPECTRE_DEBUG_AREA_BLE          _SPECTRE_DEBUG_AREA_DEFAULT  // BLE
#endif
#ifndef SPECTRE_DEBUG_AREA_MQTT
#define SPECTRE_DEBUG_AREA_MQTT         _SPECTRE_DEBUG_AREA_DEFAULT  // MQTT
#endif
#ifndef SPECTRE_DEBUG_AREA_EXPORT
#define SPECTRE_DEBUG_AREA_EXPORT       _SPECTRE_DEBUG_AREA_DEFAULT  // EXPORT
#endif
#ifndef SPECTRE_DEBUG_AREA_GPS
#define SPECTRE_DEBUG_AREA_GPS          _SPECTRE_DEBUG_AREA_DEFAULT  // GPS
#endif
#ifndef SPECTRE_DEBUG_AREA_MODE
#define SPECTRE_DEBUG_AREA_MODE         _SPECTRE_DEBUG_AREA_DEFAULT  // MODE, MISSION, UI
#endif

// -----------------------------------------------------------------------------
// Display and UI geometry
// -----------------------------------------------------------------------------

#define UI_STATUS_H   20
#define UI_MASCOT_W   70
#define UI_MASCOT_H   146
#define UI_CONTENT_X  78
#define UI_CONTENT_W  248
#define UI_CONTENT_Y  22
#define UI_DIVIDER_X  72
#define UI_CONTENT_H  124   // 170 - 28 - 18
#define UI_ACTION_H   18
#define UI_ACTION_Y   152   // 170 - 18
#define UI_SCREEN_W   320
#define UI_SCREEN_H   170

// -----------------------------------------------------------------------------
// Board pins
// -----------------------------------------------------------------------------

// Display
#define LCD_BL      38
#define LCD_CS      6
#define LCD_DC      7
#define LCD_RST     5
#define LCD_MOSI    11
#define LCD_SCLK    12
#define LCD_RD      9
#define LCD_WR      8
#define LCD_D0      39
#define LCD_POWER   15

// Buttons
#define BTN_A       0   // IO0 - top button
#define BTN_B       14  // IO14 - bottom button

// RYLR998 UART
#define LORA_TX     18  // ESP TX -> RYLR RX
#define LORA_RX     17  // ESP RX -> RYLR TX
#define LORA_UART   1

// Battery
#define BAT_ADC     4   // LCD_BAT_VOLT pin from pinout

// -----------------------------------------------------------------------------
// Radio and antenna control
// -----------------------------------------------------------------------------

#define ANTENNA_SWITCH_DISABLED  0
#define ANTENNA_SWITCH_GPIO      1
#define ANTENNA_SWITCH_DUAL_GPIO 2

#if defined(BOARD_HAS_PSRAM)
  #if (BTN_A == 0)
    #define WIFI_ANTENNA_SWITCH_MODE    ANTENNA_SWITCH_DISABLED
  #else
    #define WIFI_ANTENNA_SWITCH_MODE    ANTENNA_SWITCH_GPIO
  #endif
#else
  #define WIFI_ANTENNA_SWITCH_MODE    ANTENNA_SWITCH_DISABLED
#endif

#define WIFI_ANTENNA_DEFAULT_EXTERNAL ON

#define WIFI_ANTENNA_CTRL_PIN       0
#define WIFI_ANTENNA_INTERNAL_LEVEL  LOW
#define WIFI_ANTENNA_EXTERNAL_LEVEL  HIGH

#define WIFI_ANTENNA_GPIO_ANT0      -1
#define WIFI_ANTENNA_GPIO_ANT1      -1
#define WIFI_ANTENNA_INTERNAL_PATH   0
#define WIFI_ANTENNA_EXTERNAL_PATH   1

// -----------------------------------------------------------------------------
// Power subsystem
// -----------------------------------------------------------------------------

#define POWER_BATTERY_DIVIDER_NUM          2
#define POWER_BATTERY_DIVIDER_DEN          1
#define POWER_BATTERY_CAPACITY_DEFAULT_MAH 1100
#define POWER_BATTERY_CAPACITY_MIN_MAH     50
#define POWER_BATTERY_CAPACITY_MAX_MAH     5000
#define POWER_USB_SENSE_PIN                -1
#define POWER_USB_SENSE_ACTIVE             HIGH
#define POWER_CHARGE_SENSE_PIN             -1
#define POWER_CHARGE_SENSE_ACTIVE          LOW
#define POWER_ECONOMY_PERCENT              20
#define POWER_CRITICAL_RUNTIME_MIN         3

// Battery-life characterization mode. When ON, the automatic deep-sleep that
// fires POWER_CRITICAL_SLEEP_COUNTDOWN_SEC after entering BATTERY_CRITICAL is
// suppressed — the device keeps running until the cell collapses on its own.
// Defaulting to ON: power-saving is being characterized first, and any
// premature critical-trip would cut off the test before useful data lands.
// Li-ion cutoff: most boards include a hardware undervoltage cutoff; if not,
// observe the discharge run and unplug before the cell sags below ~2.9 V.
#define POWER_RUN_UNTIL_DEAD               ON

// Power thresholds (millivolts). Pushed lower than typical to bias toward
// "let the battery run all the way out" rather than trip critical early —
// the runtime estimate has been wrong before, and we'd rather under-warn
// than auto-sleep on a cell that still has real headroom.
#define BAT_WARN_MV     3300
#define BAT_CRITICAL_MV 3000

// -----------------------------------------------------------------------------
// MQTT
// -----------------------------------------------------------------------------

#define MQTT_BROKER_HOST        SPECTRE_MQTT_BROKER
#define MQTT_BROKER_PORT        SPECTRE_MQTT_PORT
#define MQTT_SENSOR_ID          SPECTRE_MQTT_SENSOR_ID

// -----------------------------------------------------------------------------
// UI labels and helpers
// -----------------------------------------------------------------------------

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
    BUTTON_ACTION_BLE_TEST
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
        default:                              return nullptr;
    }
}

static inline ButtonBindingSet spectreScreenBindings(Screen screen) {
    switch (screen) {
        case SCREEN_LORA:
            return {BUTTON_ACTION_SUBGHZ_MODE_CYCLE, BUTTON_ACTION_SLEEP,
                    BUTTON_ACTION_LORA_PING, BUTTON_ACTION_SCREEN_NEXT};
        case SCREEN_WIFI:
            return {BUTTON_ACTION_WIFI_REFRESH, BUTTON_ACTION_WIFI_ALLSCAN,
                    BUTTON_ACTION_WIFI_SCAN_LIST, BUTTON_ACTION_SCREEN_NEXT};
        case SCREEN_BADUSB:
            return {BUTTON_ACTION_BADUSB_ARM, BUTTON_ACTION_BADUSB_RUN,
                    BUTTON_ACTION_BADUSB_LIST_OPEN, BUTTON_ACTION_SCREEN_NEXT};
        case SCREEN_SYSTEM:
            return {BUTTON_ACTION_SYSTEM_DEBRIEF, BUTTON_ACTION_SESSION_TAG,
                    BUTTON_ACTION_UPLINK_TRIGGER, BUTTON_ACTION_SCREEN_NEXT};
        case SCREEN_MESHTASTIC:
            return {BUTTON_ACTION_NONE, BUTTON_ACTION_SLEEP,
                    BUTTON_ACTION_BLE_TEST, BUTTON_ACTION_SCREEN_NEXT};
        case SCREEN_RECON:
            return {BUTTON_ACTION_MISSION_ENTER, BUTTON_ACTION_MISSION_ENTER,
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

