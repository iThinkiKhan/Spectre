#pragma once
#include "SecretsConfig.h"
#include "core/ScreenEnum.h"

// -----------------------------------------------------------------------------
// Feature flags
// -----------------------------------------------------------------------------

#define PHONE_COMPANION_ENABLED     false
// Boot-time BLE probe (8 s → 2 m → 5 m → 10 m → 30 m retry ladder).
#define PHONE_COMPANION_BOOT_PROBE  false
#define PHONE_COMPANION_AUTO_ENRICH false
// Pending event count that triggers automatic phone enrichment.
// Keep this below MQTT_UPLOAD_READY_THRESHOLD so records can be enriched
// before normal MQTT upload drains the spool.
#define PHONE_COMPANION_ENRICH_THRESHOLD 48UL
// Enrichment batch size. Batches above 18 exceed a single 247-MTU encrypted
// ATT payload, so the BLE/write path and phone app must support long writes.
// Keep <=64 unless BLEManager buffer sizing is audited.
#define PHONE_COMPANION_ENRICH_BATCH_MAX 18
#define SPECTRE_SECONDS_TO_MS(seconds) ((uint32_t)(seconds) * 1000UL)

// -----------------------------------------------------------------------------
// Diagnostic tools (keep false in production builds).
// -----------------------------------------------------------------------------

#define BOOT_SEQUENCE_ENABLED       true
#define BOOT_SEQUENCE_VERBOSE       false

// Gate off so the command isn't accidentally triggered in the field.
#define BLE_SMOKE_ENABLED           true

// Safety gate: passive Pwny capture remains available, but active deauth is
// disabled unless explicitly enabled here or manually requested from the UI.
#define PWNY_ACTIVE_ATTACKS_ENABLED false

// -----------------------------------------------------------------------------
// Timing and upload behavior
// -----------------------------------------------------------------------------

// Fine-grained timing stays in milliseconds to preserve sub-second precision.
#define BUTTON_LONG_PRESS_MS          800UL
#define BUTTON_DEBOUNCE_MS            50UL

#define MQTT_DUMP_INTERVAL_SEC        7200UL
#define MQTT_DUMP_INTERVAL_MS         SPECTRE_SECONDS_TO_MS(MQTT_DUMP_INTERVAL_SEC)
#define MQTT_CONNECT_TIMEOUT_SEC      10UL
#define MQTT_CONNECT_TIMEOUT_MS       SPECTRE_SECONDS_TO_MS(MQTT_CONNECT_TIMEOUT_SEC)
#define MQTT_FAILED_BACKOFF_SEC       300UL
#define MQTT_FAILED_BACKOFF_MS        SPECTRE_SECONDS_TO_MS(MQTT_FAILED_BACKOFF_SEC)
#define MQTT_POISON_FAIL_LIMIT        3

#define MQTT_UPLOAD_READY_THRESHOLD   1000

// ── Upload slice / batch tuning ───────────────────────────────────────────────
//
// MQTT upload runs in cooperative slices on TaskHardware.  Each call to
// _runDumpSlice() does one of two things:
//
//   FETCH  — scan spool storage once, cache FETCH_BATCH_SIZE records in RAM,
//            then yield (return false).  One scan ≈ 500 ms on a cold spool.
//
//   PROCESS — publish up to RECORDS_PER_SLICE cached records, then yield when
//            either the record budget or SLICE_BUDGET_MS is exceeded.
//
// Because fetch and process alternate, raising FETCH_BATCH_SIZE amortises the
// 500 ms scan cost across more publishes.  RECORDS_PER_SLICE and
// SLICE_BUDGET_MS bound how long a single process call holds the CPU.
//
// Rule of thumb: FETCH_BATCH_SIZE ≥ 2 × RECORDS_PER_SLICE so each scan
// pays for at least two full process calls before the next scan.
//
// Durable checkpoints flush upload watermarks to flash every
// CHECKPOINT_EVERY_N published events so a crash-loop cannot lose more than
// that many records' worth of upload progress.  Flushing costs ~10–30 ms of
// LittleFS activity mid-radio; set this low enough to keep recovery windows
// short, high enough to avoid hammering flash on large uploads.
#define MQTT_DUMP_FETCH_BATCH_SIZE    16   // records loaded per storage scan
#define MQTT_DUMP_RECORDS_PER_SLICE    4   // max publish calls per yield
#define MQTT_DUMP_SLICE_BUDGET_MS     25   // cooperative yield deadline (ms)
#define MQTT_DUMP_CHECKPOINT_EVERY_N  150   // watermark flush cadence (events)

// Upload radio lease sizing.
//
// The lease granted to RADIO_WIFI_UPLOAD scales with the number of records
// pending at dump start, using the formula:
//
//   lease = clamp(MQTT_UPLOAD_LEASE_CONNECT_MS
//                 + pending * MQTT_UPLOAD_LEASE_MS_PER_EVENT,
//                 MQTT_UPLOAD_LEASE_MIN_MS,
//                 MQTT_UPLOAD_LEASE_MAX_MS)
//
// Tune CONNECT_MS to cover WiFi association + broker handshake.
// Tune MS_PER_EVENT to match observed publish round-trip on your network.
// MIN_MS is the floor so even tiny batches get a comfortable window.
// MAX_MS is the refreshed live hold window ceiling for very large backlogs.
#define MQTT_UPLOAD_LEASE_CONNECT_MS    20000UL  // WiFi + broker connect budget
#define MQTT_UPLOAD_LEASE_MS_PER_EVENT    600UL  // per-event publish time budget
#define MQTT_UPLOAD_LEASE_MIN_MS        90000UL  // floor  —  90 s
#define MQTT_UPLOAD_LEASE_MAX_MS       600000UL  // ceiling — 10 min

#define SLEEP_TIMEOUT_SEC             300UL
#define SLEEP_TIMEOUT_MS              SPECTRE_SECONDS_TO_MS(SLEEP_TIMEOUT_SEC)
#define BACKLIGHT_TIMEOUT_SEC         30UL
#define BACKLIGHT_TIMEOUT_MS          SPECTRE_SECONDS_TO_MS(BACKLIGHT_TIMEOUT_SEC)

#define POWER_CRITICAL_SLEEP_COUNTDOWN_SEC  300UL
#define POWER_CRITICAL_SLEEP_COUNTDOWN_MS   SPECTRE_SECONDS_TO_MS(POWER_CRITICAL_SLEEP_COUNTDOWN_SEC)

// One-shot maintenance wipe: when enabled, the next boot clears all LittleFS
// content except /config/vault, then writes the tag below so it only runs once
// for that tag value.
#define STORAGE_ONE_SHOT_NON_VAULT_RESET_ENABLED false
#define STORAGE_ONE_SHOT_NON_VAULT_RESET_TAG     "5-3-26-reset-backlog-1k"

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

// WiFi antenna switching
// Set WIFI_ANTENNA_SWITCH_MODE and the matching GPIO values for your board's
// RF switch. GPIO mode drives one control line; dual GPIO mode uses the ESP32
// WiFi antenna API for boards wired as ANT0/ANT1.
#define ANTENNA_SWITCH_DISABLED  0
#define ANTENNA_SWITCH_GPIO      1
#define ANTENNA_SWITCH_DUAL_GPIO 2

#if defined(BOARD_HAS_PSRAM)
  // T-Display S3 style RF switch
  // NOTE: GPIO0 is also used for BTN_A in this config. If BTN_A is GPIO0,
  // keep switching disabled to avoid breaking button input during recovery.
  #if (BTN_A == 0)
    #define WIFI_ANTENNA_SWITCH_MODE    ANTENNA_SWITCH_DISABLED
  #else
    #define WIFI_ANTENNA_SWITCH_MODE    ANTENNA_SWITCH_GPIO
  #endif
#else
  #define WIFI_ANTENNA_SWITCH_MODE    ANTENNA_SWITCH_DISABLED
#endif

#define WIFI_ANTENNA_DEFAULT_EXTERNAL true

// GPIO switch mode
#define WIFI_ANTENNA_CTRL_PIN       0
#define WIFI_ANTENNA_INTERNAL_LEVEL  LOW
#define WIFI_ANTENNA_EXTERNAL_LEVEL  HIGH

// Dual GPIO / ANT0-ANT1 mode
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
#define POWER_ECONOMY_PERCENT              30
#define POWER_CRITICAL_RUNTIME_MIN         7

// Power thresholds (millivolts)
#define BAT_WARN_MV     3500
#define BAT_CRITICAL_MV 3200

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

