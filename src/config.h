#pragma once

// Spectre compile-time configuration. Keep only durable knobs here; helper
// tables live in the headers included below.

#include "SecretsConfig.h"
#include "core/ScreenEnum.h"
#include "core/ButtonBindings.h"

#define ON  1
static constexpr uint8_t OFF = 0;

#define SPECTRE_SECONDS_TO_MS(seconds) ((uint32_t)(seconds) * 1000UL)

// Main switches

#define PHONE_COMPANION_ENABLED     ON
// Stability-first field profile: the experimental Wi-Fi bulk path performs a
// deliberate BLE -> AP reboot. Keep it disabled until the reboot/resume path
// has a non-retrying completion handshake; Android safely falls back to BLE.
#define PHONE_WIFI_BULK_ENABLED     OFF

// Auto-enrich starts when pending events cross these thresholds. WIO is cheaper
// to wake, so its threshold is lower. Manual ENRICH bypasses both.
#define PHONE_COMPANION_ENRICH_THRESHOLD      25UL
#define PHONE_COMPANION_ENRICH_THRESHOLD_WIO 25UL
#define PHONE_COMPANION_ENRICH_BATCH_MAX     18

// Bound terminal writes so flash rotation cannot starve the BLE host.
#define ENRICH_NODATA_BUDGET_PER_WALK        64U
#define ENRICH_NODATA_CHUNK                  32U
#define ENRICH_WALK_BUDGET_MS                2500U

// Consecutive no-progress phone passes before "retry later" becomes NO_DATA.
#define ENRICH_DEFERRED_RETIRE_PASSES        2U

// Preserve BLE host/controller service time while ranking a large window.
#define ENRICH_SCAN_BUDGET_MS                750U

// Manual `wio enrich` rebuilds windows until a full pass makes no progress.

// Enrichment drain policy
// Enriched replies drain in trickle mode until the tiny in-flight ring is full
// or old enough to justify a short storage-maintenance lease.
#define ENRICH_DRAIN_HIGH_WATER       2UL    // batches → torrent
#define ENRICH_DRAIN_LOW_WATER        1UL    // batches → exit torrent at/below
#define ENRICH_DRAIN_MAX_AGE_MS       8000UL // oldest age → torrent
#define ENRICH_DRAIN_PREEMPT_AGE_MS   20000UL// oldest age → preempt capture
#define ENRICH_DRAIN_TORRENT_BUDGET_MS 250UL // max time held in torrent
#define ENRICH_DRAIN_LEASE_HOLD_MS    400UL  // maintenance lease hold

// Boot options

#define BOOT_SEQUENCE_ENABLED       ON
#define BOOT_SEQUENCE_VERBOSE       OFF   // manual boot serial verbosity override
#define BOOT_SEQUENCE_VERBOSE_IN_DEBUG ON  // auto-enable boot serial traces in DEBUG/DEV
#define BOOT_RECOVERY_ENABLED       ON
#define BOOT_RECOVERY_BUTTON_PIN    BTN_B
#define BOOT_RECOVERY_HOLD_MS       1500UL

#define BLE_SMOKE_ENABLED           ON

#define PWNY_ACTIVE_ATTACKS_ENABLED OFF

// Timing

// Button timing needs sub-second precision, so it stays in milliseconds.
#define BUTTON_LONG_PRESS_MS          800UL   // ms
#define BUTTON_DEBOUNCE_MS            50UL    // ms

#define MQTT_WIFI_CONNECT_TIMEOUT_SEC 30UL    // seconds
#define MQTT_WIFI_CONNECT_TIMEOUT_MS  SPECTRE_SECONDS_TO_MS(MQTT_WIFI_CONNECT_TIMEOUT_SEC)
#define MQTT_BROKER_CONNECT_TIMEOUT_SEC 15UL  // seconds
#define MQTT_BROKER_CONNECT_TIMEOUT_MS SPECTRE_SECONDS_TO_MS(MQTT_BROKER_CONNECT_TIMEOUT_SEC)
#define MQTT_FAILED_BACKOFF_SEC       300UL   // seconds
#define MQTT_FAILED_BACKOFF_MS        SPECTRE_SECONDS_TO_MS(MQTT_FAILED_BACKOFF_SEC)
#define MQTT_POISON_FAIL_LIMIT        3

// Retry trusted UTC while the boot is still recoverable for enrichment backfill.
#define UTC_ACQUIRE_RETRY_SEC         60UL    // seconds
#define UTC_ACQUIRE_RETRY_MS          SPECTRE_SECONDS_TO_MS(UTC_ACQUIRE_RETRY_SEC)

// Hold pending-enrichment records briefly before upload; after this, ship them
// un-enriched so a missing phone/GPS cannot wedge the backlog.
#define UPLOAD_ENRICH_GRACE_SEC       900UL   // seconds (15 min)
#define UPLOAD_ENRICH_GRACE_MS        SPECTRE_SECONDS_TO_MS(UPLOAD_ENRICH_GRACE_SEC)

#define SLEEP_TIMEOUT_SEC             300UL   // seconds
#define SLEEP_TIMEOUT_MS              SPECTRE_SECONDS_TO_MS(SLEEP_TIMEOUT_SEC)
#define BACKLIGHT_TIMEOUT_SEC         30UL    // seconds
#define BACKLIGHT_TIMEOUT_MS          SPECTRE_SECONDS_TO_MS(BACKLIGHT_TIMEOUT_SEC)

#define POWER_CRITICAL_SLEEP_COUNTDOWN_SEC  300UL  // seconds
#define POWER_CRITICAL_SLEEP_COUNTDOWN_MS   SPECTRE_SECONDS_TO_MS(POWER_CRITICAL_SLEEP_COUNTDOWN_SEC)

// Radio-arbiter BLE lease hold durations.
#define BLE_TEXT_ACTIVE_HOLD_SEC      10UL    // seconds
#define BLE_TEXT_ACTIVE_HOLD_MS_VAL   SPECTRE_SECONDS_TO_MS(BLE_TEXT_ACTIVE_HOLD_SEC)
#define BLE_TEXT_IDLE_HOLD_SEC        180UL   // seconds
#define BLE_TEXT_IDLE_HOLD_MS_VAL     SPECTRE_SECONDS_TO_MS(BLE_TEXT_IDLE_HOLD_SEC)
#define BLE_PHONE_PROBE_HOLD_SEC      60UL    // seconds
#define BLE_PHONE_PROBE_HOLD_MS_VAL   SPECTRE_SECONDS_TO_MS(BLE_PHONE_PROBE_HOLD_SEC)
#define BLE_PHONE_ENRICH_HOLD_SEC     40UL    // seconds
#define BLE_PHONE_ENRICH_HOLD_MS_VAL  SPECTRE_SECONDS_TO_MS(BLE_PHONE_ENRICH_HOLD_SEC)

// Same-owner radio churn watchdog: kick WiFi capture first, reboot only after a
// sustained unbroken streak.
#define RADIO_CHURN_MAX_GAP_SEC       8UL    // streak resets if re-grants spaced wider
#define RADIO_CHURN_MAX_GAP_MS        SPECTRE_SECONDS_TO_MS(RADIO_CHURN_MAX_GAP_SEC)
#define RADIO_CHURN_KICK_THRESHOLD    6U     // same-owner streak that arms a fallback kick
#define RADIO_CHURN_REBOOT_THRESHOLD  14U    // same-owner streak that forces a reboot
#define RADIO_CHURN_BYPASS_SEC        30UL   // gate-bypass window after a kick
#define RADIO_CHURN_BYPASS_MS         SPECTRE_SECONDS_TO_MS(RADIO_CHURN_BYPASS_SEC)

// Capture is the default radio state. When a default-capture start fails it is
// retried with a linear backoff (base * attempt, capped) — never abandoned.
#define CAPTURE_RETRY_BACKOFF_BASE_MS 1000UL
#define CAPTURE_RETRY_BACKOFF_STEPS   10U
#define CAPTURE_RETRY_BACKOFF_MAX_MS  10000UL

// MQTT upload
// Upload is threshold-triggered, low-water drained, plus one FieldVault boot
// attempt. Manual SYNC forces a dump.
// AP-inventory publishing to the `network` topic. OFF until the spool append
// path is confirmed to handle the generic (non-structured) binary payload for
// this type — enabling it coincided with events getting IDs but never reaching
// a segment, so it stays gated until that is understood.
#define SPECTRE_PUBLISH_NETWORKS      0

#define MQTT_UPLOAD_READY_THRESHOLD   40000
// Low-water mark: drain mode exits once pending drops below this, so the next
// upload only fires after pending climbs back to MQTT_UPLOAD_READY_THRESHOLD.
// Keep it well below the threshold so each burst flushes most of the batch.
// Set to 0 to restore the old "drain to empty / stream forever" behavior.
#define MQTT_UPLOAD_DRAIN_EXIT_RECORDS 2000
#define MQTT_BACKLOG_LARGE_WARN_THRESHOLD 10000   // boot diagnostic only
#define MQTT_DUMP_FETCH_BATCH_SIZE     32  // records loaded per storage scan (<= UPLOAD_FETCH_STACK_CAPACITY)
// Upload slice size while capture is suspended under the WIFI_UPLOAD lease.
#define MQTT_DUMP_RECORDS_PER_SLICE   48   // max publish calls per yield
#define MQTT_DUMP_SLICE_BUDGET_MS    300   // ms
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

// Spool hot-path metadata durability.
// Event records are always appended immediately; these limits only batch the
// smaller sidecar files that can be rebuilt/audited after an interrupted run.
#define STORAGE_EVENT_COUNTER_SAVE_EVERY_N  256
#define STORAGE_SPOOL_INDEX_SAVE_EVERY_N    512
#define STORAGE_HOT_META_SAVE_INTERVAL_SEC  60UL    // seconds
#define STORAGE_HOT_META_SAVE_INTERVAL_MS   SPECTRE_SECONDS_TO_MS(STORAGE_HOT_META_SAVE_INTERVAL_SEC)

// WiFi capture retune guard. If esp_wifi_set_channel() blocks for longer than
// this during promiscuous capture, stretch channel dwell temporarily so capture
// keeps running instead of spending most TaskHardware time retuning.
// _WARN values stay in ms — they're sub-second thresholds.
#define WIFI_CHANNEL_HOP_SLOW_WARN_MS         250UL  // ms
#define WIFI_CHANNEL_HOP_SLOW_BACKOFF_SEC     10UL   // seconds
#define WIFI_CHANNEL_HOP_SLOW_BACKOFF_MS      SPECTRE_SECONDS_TO_MS(WIFI_CHANNEL_HOP_SLOW_BACKOFF_SEC)
#define WIFI_CAPTURE_FILE_CHECK_SLOW_WARN_MS  250UL  // ms
#define WIFI_FRAME_PROCESS_SLOW_WARN_MS       250UL  // ms
#define STORAGE_FS_STATS_CACHE_SEC            5UL    // seconds
#define STORAGE_FS_STATS_CACHE_MS             SPECTRE_SECONDS_TO_MS(STORAGE_FS_STATS_CACHE_SEC)

// One-shot startup FieldVault upload. After boot grace, if FieldVault has
// pending records, fire a single field-only MQTT upload. On success the live
// FieldVault file is cleared. On failure (no broker, publish error) the
// records remain and retry on the next normal/manual/threshold dump. There is
// no periodic FieldVault-only retry loop — exactly one attempt per boot.
#define MQTT_FIELDVAULT_STARTUP_UPLOAD_ENABLED  ON
#define MQTT_FIELDVAULT_STARTUP_GRACE_SEC       30UL    // seconds after boot before attempt
#define MQTT_FIELDVAULT_STARTUP_GRACE_MS        SPECTRE_SECONDS_TO_MS(MQTT_FIELDVAULT_STARTUP_GRACE_SEC)
#define MQTT_FIELDVAULT_STARTUP_MAX_RECORDS     8       // cap per startup attempt
#define MQTT_FIELDVAULT_STARTUP_LEASE_SEC       30UL    // seconds — short upload lease
#define MQTT_FIELDVAULT_STARTUP_LEASE_MS        SPECTRE_SECONDS_TO_MS(MQTT_FIELDVAULT_STARTUP_LEASE_SEC)

// One-shot maintenance wipes. Change the tag before turning a reset ON again.
#define STORAGE_ONE_SHOT_VAULT_RESET_ENABLED     OFF
#define STORAGE_ONE_SHOT_VAULT_RESET_TAG         "6-15-26-reset-vault"
#define STORAGE_ONE_SHOT_NON_VAULT_RESET_ENABLED OFF
#define STORAGE_ONE_SHOT_NON_VAULT_RESET_TAG     "6-15-26-reset-new-pc"
#define STORAGE_FAST_BOOT_DEFER_SPOOL_REPAIR     ON

// Duplicate suppression
// Three compile-time profiles for how aggressively the spool drops repeats:
//   0 = OFF / ballast        no dedup
//   1 = STANDARD (default)   time-windowed dedup on P2 records (probe/device).
//                            Mission records always saved.
//   2 = STRICT / long-mission drops P3 — records with <2 unique fields, i.e.,
//                            anything that can't be meaningfully GPS-enriched
//
// Pressure-driven retention escalation (STORAGE_POLICY_REDUCED / CRITICAL_ONLY)
// still applies on top: profile is a floor, pressure can raise it further.
#define DEDUP_PROFILE_OFF      0
#define DEDUP_PROFILE_STANDARD 1
#define DEDUP_PROFILE_STRICT   2

#define DEDUP_PROFILE          DEDUP_PROFILE_STANDARD

// Profile 1 tunables.
#define DEDUP_WINDOW_SEC       600UL
#define DEDUP_WINDOW_MS        SPECTRE_SECONDS_TO_MS(DEDUP_WINDOW_SEC)
#define DEDUP_WINDOW_MAX       256

// Localization observations are intentionally different from ordinary
// inventory deduplication. A moving receiver needs repeated RSSI measurements
// from separated positions, but writing every beacon/probe would overwhelm
// storage. Emit one bounded sample per target at this cadence, or sooner when
// signal changes materially after the minimum gap.
#define LOCALIZATION_SAMPLE_INTERVAL_MS 30000UL
#define LOCALIZATION_SAMPLE_MIN_GAP_MS  10000UL
#define LOCALIZATION_RSSI_DELTA_DB       8

// Handshake-completion window: holds 4-way frame state long enough to
// recognize a finished capture and drop replays after that.
#define HANDSHAKE_WINDOW_SEC   900UL
#define HANDSHAKE_WINDOW_MS    SPECTRE_SECONDS_TO_MS(HANDSHAKE_WINDOW_SEC)

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
#define SPECTRE_DEBUG_PROFILE           SPECTRE_DEBUG_PROFILE_DEBUG
#endif

#ifndef BOOT_SEQUENCE_VERBOSE_ACTIVE
  #if (BOOT_SEQUENCE_VERBOSE == ON) || \
      ((BOOT_SEQUENCE_VERBOSE_IN_DEBUG == ON) && (SPECTRE_DEBUG_PROFILE >= SPECTRE_DEBUG_PROFILE_DEBUG))
    #define BOOT_SEQUENCE_VERBOSE_ACTIVE OFF
  #else
    #define BOOT_SEQUENCE_VERBOSE_ACTIVE OFF
  #endif
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
#define SPECTRE_DEBUG_AREA_SETTINGS     OFF  // SETTINGS
#endif
#ifndef SPECTRE_DEBUG_AREA_STORAGE
#define SPECTRE_DEBUG_AREA_STORAGE      _SPECTRE_DEBUG_AREA_DEFAULT  // STOR, STORAGE
#endif
#ifndef SPECTRE_DEBUG_AREA_TIME
#define SPECTRE_DEBUG_AREA_TIME         OFF  // TIME
#endif
#ifndef SPECTRE_DEBUG_AREA_RADIO
#define SPECTRE_DEBUG_AREA_RADIO        OFF  // RADIO, LORA, SUBGHZ
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
#define SPECTRE_DEBUG_AREA_EXPORT       OFF  // EXPORT
#endif
#ifndef SPECTRE_DEBUG_AREA_GPS
#define SPECTRE_DEBUG_AREA_GPS          OFF  // GPS
#endif
#ifndef SPECTRE_DEBUG_AREA_MODE
#define SPECTRE_DEBUG_AREA_MODE         OFF  // MODE, MISSION, UI
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
#define LORA_UART   2

// Seeed XIAO nRF52840 + Wio-SX1262 UART accessory.
// Spectre remains the controller; the nRF acts as an external BLE/SX1262
// coprocessor that drives the SX1262 as a thin LoRa modem (SUBGHZ_* verbs).
#define WIO_NRF_ACCESSORY_ENABLED ON
#define WIO_NRF_UART_NUM          1
#define WIO_NRF_UART_TX           1   // ESP TX -> XIAO D7 RX
#define WIO_NRF_UART_RX           2   // ESP RX <- XIAO D6 TX
#define WIO_NRF_BAUD              115200

// SX1262 sub-GHz radio (driven over the WIO nRF modem bridge). The WIO SX1262
// is preferred over the RYLR998 (Reyax) backend when CAPS reports it present.
#define SUBGHZ_SX1262_DEFAULT_FREQ_HZ 915000000UL
#define SUBGHZ_SX1262_TX_POWER_DBM    17    // dBm
#define SUBGHZ_SX1262_NATIVE_SYNC     0x12  // private LoRa sync for Spectre-native frames

// Basic Meshtastic client (US 915 MHz, LongFast preset, default public channel).
// These drive the shared SX1262 in MESHTASTIC application mode; the radio holds
// one PHY profile at a time, so this is mutually exclusive with native SubGhz.
#define MESHTASTIC_ENABLED            ON
#define MESHTASTIC_FREQ_HZ            906875000UL  // US LongFast channel 0 center
#define MESHTASTIC_BW_HZ              250000UL     // LongFast bandwidth
#define MESHTASTIC_SF                 11           // LongFast spreading factor
#define MESHTASTIC_CR                 5            // 4/5 coding rate (RadioLib cr=5)
#define MESHTASTIC_PREAMBLE           16
#define MESHTASTIC_SYNC_WORD          0x2B         // Meshtastic LoRa sync word
#define MESHTASTIC_TX_POWER_DBM       22
#define MESHTASTIC_MAX_NODES          32

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
// Set OFF for normal use so the device self-preserves: ~POWER_CRITICAL_SLEEP_
// COUNTDOWN_SEC after entering BATTERY_CRITICAL it checkpoints and deep-sleeps
// instead of running the cell flat and browning out on the desk. Flip back to
// ON only for a deliberate discharge/characterization run.
// Li-ion cutoff: most boards include a hardware undervoltage cutoff; if not,
// observe the discharge run and unplug before the cell sags below ~2.9 V.
#define POWER_RUN_UNTIL_DEAD               OFF

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
// Button action vocabulary and per-screen binding tables now live in
// core/ButtonBindings.h (included at the top of this file). Edit bindings there.

