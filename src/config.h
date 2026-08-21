#pragma once

// =============================================================================
// Spectre compile-time configuration
// =============================================================================
//
// Layout:
//   PART 1  OPERATOR LEVERS   the handful of knobs actually worth turning.
//   PART 2  SUBSYSTEM TUNING  values that have a right answer; change with care.
//   PART 3  HARDWARE MAP      pins and board facts. Wrong values = dead board.
//
// Every setting below is reachable from code. If you change something here and
// nothing happens, that is a bug — say so, don't work around it.
//
// ON / OFF convention — read this before adding a switch.
//
// ON is a macro. OFF deliberately is NOT: several enums have a member named
// OFF (SubGhzMode::OFF among them), and a macro would textually clobber every
// one of them. So OFF stays a constexpr.
//
// The consequence: inside `#if`, OFF is an identifier the preprocessor has
// never heard of, and unknown identifiers evaluate to 0. That makes
// `#if FEATURE` behave correctly for both ON and OFF — but it also means a
// MISSPELLED macro silently reads as false with no warning. If a feature
// switch appears to do nothing, suspect the spelling first.
//
// Also: when a switch resolves through an `#if/#else`, make sure the two arms
// actually differ. Two arms that both say ON is not a no-op, it is a switch
// that has been quietly welded shut. That bug disabled boot diagnostics here
// for months.
#define ON  1
static constexpr uint8_t OFF = 0;

#include "SecretsConfig.h"
#include "core/ScreenEnum.h"
#include "core/ButtonBindings.h"

// Timeouts are written in seconds and converted here, so the units are visible
// at the point you edit them.
#define SPECTRE_SECONDS_TO_MS(seconds) ((uint32_t)(seconds) * 1000UL)


// =============================================================================
// PART 1 — OPERATOR LEVERS
// =============================================================================
// If you are changing one thing before a run, it is almost certainly in here.

// ── When does the backlog get uploaded? ──────────────────────────────────────
//
// Uploads are burst-based, not continuous. Nothing is sent until the pending
// count reaches READY_THRESHOLD; then the device drains hard until pending
// falls to DRAIN_EXIT, and goes quiet again. So the backlog sawtooths between
// DRAIN_EXIT and READY_THRESHOLD. `upload now` ignores both and drains
// immediately.
//
// Two unrelated things also key off 10000, neither of them severe:
//   MQTT_BACKLOG_LARGE_WARN_THRESHOLD  a boot log line, no behaviour at all.
//   BOOT_TRIAGE_PENDING_UPLOAD_THRESHOLD (main.cpp) defers optional boot init
//     — DebugLog, FieldVault, known locations — by 2 s. That deferral used to
//     drop the boot/crash records outright; that is fixed, so what remains is
//     a 2 s delay.
// Neither is a reason on its own to keep the backlog small.
//
// The real cost of a high threshold is what a large backlog does to storage
// and to drain time: upload prep stream-fetch is O(N^2), so 30k does not
// behave like 3k. That is the thing to measure.
#define MQTT_UPLOAD_READY_THRESHOLD   30000
#define MQTT_UPLOAD_DRAIN_EXIT_RECORDS 2000   // 0 = drain to empty, stream forever

// ── How chatty is the serial console? ────────────────────────────────────────
//
// OFF   silent
// RUN   warnings and errors only — field default
// DEBUG warnings, errors, and info from the areas enabled in PART 2
// DEV   everything
#define SPECTRE_DEBUG_PROFILE_OFF       0
#define SPECTRE_DEBUG_PROFILE_RUN       1
#define SPECTRE_DEBUG_PROFILE_DEBUG     2
#define SPECTRE_DEBUG_PROFILE_DEV       3

#ifndef SPECTRE_DEBUG_PROFILE
#define SPECTRE_DEBUG_PROFILE           SPECTRE_DEBUG_PROFILE_DEBUG
#endif

// Boot-time diagnostics: reset reason, crash-breadcrumb ring, allocation
// failures. Costs nothing at runtime and is the only narrative you get after
// an unattended crash.
//   ON    always print
//   OFF   never print
//   AUTO  print when SPECTRE_DEBUG_PROFILE is DEBUG or higher
#define BOOT_SEQUENCE_VERBOSE       OFF  // ON forces it regardless of profile
#define BOOT_SEQUENCE_VERBOSE_IN_DEBUG ON   // the AUTO behaviour above

// ── How much do we throw away? ───────────────────────────────────────────────
//
// OFF      keep everything, including exact repeats. Storage fills fast.
// STANDARD drop repeats of the same probe/device seen inside DEDUP_WINDOW.
//          Mission records are never dropped. This is the sane default.
// STRICT   also drop records with fewer than 2 unique fields — anything that
//          could never be meaningfully GPS-enriched. For long missions where
//          storage is the binding constraint.
//
// Storage pressure can escalate retention beyond this on its own; this setting
// is a floor, not a ceiling.
#define DEDUP_PROFILE_OFF      0
#define DEDUP_PROFILE_STANDARD 1
#define DEDUP_PROFILE_STRICT   2

#define DEDUP_PROFILE          DEDUP_PROFILE_STANDARD

// ── Battery behaviour ────────────────────────────────────────────────────────
//
// OFF (normal): once the cell hits BAT_CRITICAL_MV the device counts down
//   POWER_CRITICAL_SLEEP_COUNTDOWN_SEC, checkpoints, and deep-sleeps, so it
//   does not run flat and brown out mid-write.
// ON (characterization only): that auto-sleep is suppressed and the device
//   runs until the cell collapses. Use for a deliberate discharge run, then
//   put it back. Most boards have a hardware undervoltage cutoff; if yours
//   does not, watch the run and unplug before the cell sags under ~2.9 V.
#define POWER_RUN_UNTIL_DEAD               OFF

// Deliberately low so the device under-warns rather than auto-sleeping on a
// cell that still has real headroom — the runtime estimate has been wrong in
// the optimistic direction before.
#define BAT_WARN_MV     3300
#define BAT_CRITICAL_MV 3000

// ── Big feature switches ─────────────────────────────────────────────────────

#define PHONE_COMPANION_ENABLED     ON
#define BOOT_SEQUENCE_ENABLED       ON
#define BOOT_RECOVERY_ENABLED       ON   // hold BTN_B at boot for USB flash mode
#define BLE_SMOKE_ENABLED           ON
#define WIO_NRF_ACCESSORY_ENABLED   ON   // XIAO nRF52840 + Wio-SX1262 coprocessor
#define MESHTASTIC_ENABLED          ON
#define PWNY_ACTIVE_ATTACKS_ENABLED OFF

// The Wi-Fi bulk offload path deliberately reboots between its BLE phase and
// its AP phase. It has no non-retrying completion handshake yet, so a failed
// resume can loop. Android falls back to BLE cleanly, so this stays off until
// that handshake exists.
#define PHONE_WIFI_BULK_ENABLED     OFF

// Publishing the AP inventory to the `network` topic coincided with events
// getting IDs but never reaching a spool segment. Off until the spool append
// path is confirmed to handle this payload shape.
#define SPECTRE_PUBLISH_NETWORKS      0

// One-shot wipes. These fire once per tag: change the tag string to re-arm.
#define STORAGE_ONE_SHOT_VAULT_RESET_ENABLED     OFF
#define STORAGE_ONE_SHOT_VAULT_RESET_TAG         "6-15-26-reset-vault"
#define STORAGE_ONE_SHOT_NON_VAULT_RESET_ENABLED OFF
#define STORAGE_ONE_SHOT_NON_VAULT_RESET_TAG     "6-15-26-reset-new-pc"

// ── Timeouts you might actually want to change ───────────────────────────────

#define BACKLIGHT_TIMEOUT_SEC         30UL
#define BACKLIGHT_TIMEOUT_MS          SPECTRE_SECONDS_TO_MS(BACKLIGHT_TIMEOUT_SEC)

// How long a record waits for GPS enrichment before it ships un-enriched. This
// is the guard against a missing phone wedging the whole backlog.
#define UPLOAD_ENRICH_GRACE_SEC       900UL   // 15 min

// Grace period after entering BATTERY_CRITICAL before the device deep-sleeps.
// Ignored entirely when POWER_RUN_UNTIL_DEAD is ON.
#define POWER_CRITICAL_SLEEP_COUNTDOWN_SEC  300UL
#define POWER_CRITICAL_SLEEP_COUNTDOWN_MS   SPECTRE_SECONDS_TO_MS(POWER_CRITICAL_SLEEP_COUNTDOWN_SEC)

// Button feel.
#define BUTTON_LONG_PRESS_MS          800UL
#define BUTTON_AB_LONG_PRESS_MS       1500UL  // both buttons held together
#define BUTTON_DEBOUNCE_MS            50UL


// =============================================================================
// PART 2 — SUBSYSTEM TUNING
// =============================================================================
// These have been measured or fought for. Changing them is fine, but read the
// note first — most of them exist because something broke.

// ── Memory placement ─────────────────────────────────────────────────────────
//
// Plain malloc() requests LARGER than this may be served from PSRAM; anything
// at or below is always internal DRAM. Framework default is 4096; 0 leaves the
// framework alone.
//
// Measured internal free during WIFI_CAPTURE:
//   4096 (default) -> 82-84 KB
//   2048           -> 86-87 KB
//   1024           -> chosen
//    512           -> 92 KB, but starts putting sub-KB allocations in PSRAM
//
// Lower is not automatically better. Anything reachable from an ISR, or
// touched while the flash cache is disabled, must not live in PSRAM — that
// combination faults. The largest blocks (19,456 B, 15,872 B, 7 x 2,176 B)
// request INTERNAL/DMA explicitly and never move at any setting.
//
// NOTE: this only routes plain malloc(). An explicit heap_caps_malloc() with
// MALLOC_CAP_SPIRAM ignores it completely — that is how a task stack ended up
// in PSRAM and double-faulted. Use `stack audit` on the console to check.
#define SPECTRE_EXTMEM_MALLOC_LIMIT 1024

// One-shot boot-time BLE controller/host memory split measurement.
#define SPECTRE_BOOT_MEMPROBE       0

// ── Phone companion: enrichment ──────────────────────────────────────────────

// Auto-enrich fires once this many events are waiting. WIO is cheaper to wake
// than the phone, hence its own threshold. Manual ENRICH ignores both.
#define PHONE_COMPANION_ENRICH_THRESHOLD      25UL
#define PHONE_COMPANION_ENRICH_THRESHOLD_WIO 25UL
#define PHONE_COMPANION_ENRICH_BATCH_MAX     18

// Caps on terminal ("this record has no GPS match") writes, so flash rotation
// cannot starve the BLE host mid-exchange.
#define ENRICH_NODATA_BUDGET_PER_WALK        64U
#define ENRICH_NODATA_CHUNK                  32U
#define ENRICH_WALK_BUDGET_MS                2500U

// How many fruitless phone passes before "retry later" becomes a permanent
// NO_DATA verdict on a record.
#define ENRICH_DEFERRED_RETIRE_PASSES        2U

// Ranking a large enrichment window is CPU-heavy; this bounds it so the BLE
// host and controller keep getting service time.
#define ENRICH_SCAN_BUDGET_MS                750U

// Enriched replies drain in trickle mode until the in-flight ring fills or
// gets old enough to justify taking a short storage-maintenance lease
// ("torrent" mode). Preempting capture is the last resort.
#define ENRICH_DRAIN_HIGH_WATER       2UL     // batches queued -> torrent
#define ENRICH_DRAIN_LOW_WATER        1UL     // batches queued -> leave torrent
#define ENRICH_DRAIN_MAX_AGE_MS       8000UL  // oldest batch age -> torrent
#define ENRICH_DRAIN_PREEMPT_AGE_MS   20000UL // oldest batch age -> preempt capture
#define ENRICH_DRAIN_TORRENT_BUDGET_MS 250UL  // max time held in torrent
#define ENRICH_DRAIN_LEASE_HOLD_MS    400UL   // maintenance lease hold

// ── MQTT upload mechanics ────────────────────────────────────────────────────
// (the threshold that decides *when* to upload lives in PART 1)

#define MQTT_BACKLOG_LARGE_WARN_THRESHOLD 10000  // boot warning only, no behaviour
#define MQTT_DUMP_FETCH_BATCH_SIZE     32   // records per storage scan
#define MQTT_DUMP_RECORDS_PER_SLICE   48    // publishes per yield
#define MQTT_DUMP_SLICE_BUDGET_MS    300
#define MQTT_DUMP_PROGRESS_EVERY_N     64   // events per progress log line
#define MQTT_DUMP_CHECKPOINT_EVERY_N  250   // events per flash checkpoint

#define MQTT_WIFI_CONNECT_TIMEOUT_SEC 30UL
#define MQTT_WIFI_CONNECT_TIMEOUT_MS  SPECTRE_SECONDS_TO_MS(MQTT_WIFI_CONNECT_TIMEOUT_SEC)
#define MQTT_BROKER_CONNECT_TIMEOUT_SEC 15UL
#define MQTT_BROKER_CONNECT_TIMEOUT_MS SPECTRE_SECONDS_TO_MS(MQTT_BROKER_CONNECT_TIMEOUT_SEC)
#define MQTT_FAILED_BACKOFF_SEC       300UL
#define MQTT_FAILED_BACKOFF_MS        SPECTRE_SECONDS_TO_MS(MQTT_FAILED_BACKOFF_SEC)
#define MQTT_POISON_FAIL_LIMIT        3     // failures before a record is quarantined

// Upload radio lease = connect budget + (pending events * per-event), clamped.
#define MQTT_UPLOAD_LEASE_CONNECT_SEC   20UL
#define MQTT_UPLOAD_LEASE_CONNECT_MS    SPECTRE_SECONDS_TO_MS(MQTT_UPLOAD_LEASE_CONNECT_SEC)
#define MQTT_UPLOAD_LEASE_MS_PER_EVENT  600UL
#define MQTT_UPLOAD_LEASE_MIN_SEC       90UL
#define MQTT_UPLOAD_LEASE_MIN_MS        SPECTRE_SECONDS_TO_MS(MQTT_UPLOAD_LEASE_MIN_SEC)
#define MQTT_UPLOAD_LEASE_MAX_SEC       1200UL
#define MQTT_UPLOAD_LEASE_MAX_MS        SPECTRE_SECONDS_TO_MS(MQTT_UPLOAD_LEASE_MAX_SEC)

// Exactly one FieldVault-only upload attempt per boot, after a grace period.
// On success the live vault file is cleared; on failure the records simply
// wait for the next normal or manual dump. There is no retry loop.
#define MQTT_FIELDVAULT_STARTUP_UPLOAD_ENABLED  ON
#define MQTT_FIELDVAULT_STARTUP_GRACE_SEC       30UL
#define MQTT_FIELDVAULT_STARTUP_GRACE_MS        SPECTRE_SECONDS_TO_MS(MQTT_FIELDVAULT_STARTUP_GRACE_SEC)
#define MQTT_FIELDVAULT_STARTUP_MAX_RECORDS     8
#define MQTT_FIELDVAULT_STARTUP_LEASE_SEC       30UL
#define MQTT_FIELDVAULT_STARTUP_LEASE_MS        SPECTRE_SECONDS_TO_MS(MQTT_FIELDVAULT_STARTUP_LEASE_SEC)

#define MQTT_BROKER_HOST        SPECTRE_MQTT_BROKER
#define MQTT_BROKER_PORT        SPECTRE_MQTT_PORT
#define MQTT_SENSOR_ID          SPECTRE_MQTT_SENSOR_ID

// ── Radio arbitration ────────────────────────────────────────────────────────

// How long each subsystem may hold the radio once granted.
#define BLE_TEXT_ACTIVE_HOLD_SEC      10UL
#define BLE_TEXT_ACTIVE_HOLD_MS_VAL   SPECTRE_SECONDS_TO_MS(BLE_TEXT_ACTIVE_HOLD_SEC)
#define BLE_TEXT_IDLE_HOLD_SEC        180UL
#define BLE_TEXT_IDLE_HOLD_MS_VAL     SPECTRE_SECONDS_TO_MS(BLE_TEXT_IDLE_HOLD_SEC)
#define BLE_PHONE_PROBE_HOLD_SEC      60UL
#define BLE_PHONE_PROBE_HOLD_MS_VAL   SPECTRE_SECONDS_TO_MS(BLE_PHONE_PROBE_HOLD_SEC)
#define BLE_PHONE_ENRICH_HOLD_SEC     40UL
#define BLE_PHONE_ENRICH_HOLD_MS_VAL  SPECTRE_SECONDS_TO_MS(BLE_PHONE_ENRICH_HOLD_SEC)

// Death-loop guard. If one owner keeps re-grabbing the radio with less than
// MAX_GAP between grants, the streak counts up: at KICK the arbiter forces
// Wi-Fi capture back on, at REBOOT it gives up and restarts. BYPASS is a
// window after a kick during which normal gating is skipped so the recovery
// can actually take.
#define RADIO_CHURN_MAX_GAP_SEC       8UL
#define RADIO_CHURN_MAX_GAP_MS        SPECTRE_SECONDS_TO_MS(RADIO_CHURN_MAX_GAP_SEC)
#define RADIO_CHURN_KICK_THRESHOLD    6U
#define RADIO_CHURN_REBOOT_THRESHOLD  14U
#define RADIO_CHURN_BYPASS_SEC        30UL
#define RADIO_CHURN_BYPASS_MS         SPECTRE_SECONDS_TO_MS(RADIO_CHURN_BYPASS_SEC)

// Capture is the resting state of the radio. A failed start is retried with
// linear backoff (base * attempt, capped) and never abandoned.
#define CAPTURE_RETRY_BACKOFF_BASE_MS 1000UL
#define CAPTURE_RETRY_BACKOFF_STEPS   10U
#define CAPTURE_RETRY_BACKOFF_MAX_MS  10000UL

// ── Storage ──────────────────────────────────────────────────────────────────

// Event records are always written immediately. These only batch the small
// sidecar files that can be rebuilt by an audit after an unclean shutdown, so
// raising them trades crash-recovery work for less flash wear.
#define STORAGE_EVENT_COUNTER_SAVE_EVERY_N  256
#define STORAGE_SPOOL_INDEX_SAVE_EVERY_N    512
#define STORAGE_HOT_META_SAVE_INTERVAL_SEC  60UL
#define STORAGE_HOT_META_SAVE_INTERVAL_MS   SPECTRE_SECONDS_TO_MS(STORAGE_HOT_META_SAVE_INTERVAL_SEC)
#define STORAGE_FS_STATS_CACHE_SEC          5UL
#define STORAGE_FS_STATS_CACHE_MS           SPECTRE_SECONDS_TO_MS(STORAGE_FS_STATS_CACHE_SEC)
#define STORAGE_FAST_BOOT_DEFER_SPOOL_REPAIR     ON

// ── Capture stall guards ─────────────────────────────────────────────────────
//
// If a channel hop blocks longer than SLOW_WARN, dwell is stretched for
// BACKOFF so capture keeps running instead of spending TaskHardware retuning.
// The _WARN values are sub-second, so they stay in milliseconds.
#define WIFI_CHANNEL_HOP_SLOW_WARN_MS         250UL
#define WIFI_CHANNEL_HOP_SLOW_BACKOFF_SEC     10UL
#define WIFI_CHANNEL_HOP_SLOW_BACKOFF_MS      SPECTRE_SECONDS_TO_MS(WIFI_CHANNEL_HOP_SLOW_BACKOFF_SEC)
#define WIFI_CAPTURE_FILE_CHECK_SLOW_WARN_MS  250UL
#define WIFI_FRAME_PROCESS_SLOW_WARN_MS       250UL

// ── Dedup and sampling windows ───────────────────────────────────────────────
// (which profile is active lives in PART 1)

#define DEDUP_WINDOW_SEC       600UL
#define DEDUP_WINDOW_MS        SPECTRE_SECONDS_TO_MS(DEDUP_WINDOW_SEC)
#define DEDUP_WINDOW_MAX       256

// Localization is deliberately NOT deduplicated like inventory. A moving
// receiver needs repeated RSSI readings from separated positions, but logging
// every beacon would bury storage. So: one sample per target per INTERVAL, or
// sooner if RSSI moves by RSSI_DELTA_DB and MIN_GAP has passed.
#define LOCALIZATION_SAMPLE_INTERVAL_MS 30000UL
#define LOCALIZATION_SAMPLE_MIN_GAP_MS  10000UL
#define LOCALIZATION_RSSI_DELTA_DB       8

// How long 4-way handshake frame state is held, so a completed capture is
// recognised and later replays of the same handshake are dropped.
#define HANDSHAKE_WINDOW_SEC   900UL
#define HANDSHAKE_WINDOW_MS    SPECTRE_SECONDS_TO_MS(HANDSHAKE_WINDOW_SEC)

// ── Debug areas ──────────────────────────────────────────────────────────────
//
// Only consulted when SPECTRE_DEBUG_PROFILE is DEBUG. RUN ignores these and
// still prints warnings and errors; DEV prints everything regardless.
// Set SPECTRE_DEBUG_AREAS_NONE to ON to start from silence and enable
// individual areas below.

#ifndef SPECTRE_DEBUG_AREAS_NONE
#define SPECTRE_DEBUG_AREAS_NONE    OFF
#endif

#if SPECTRE_DEBUG_AREAS_NONE
  #define _SPECTRE_DEBUG_AREA_DEFAULT   OFF
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

// Resolved from the two switches in PART 1. Both arms must differ — if they
// ever read the same, the setting above it is doing nothing.
#ifndef BOOT_SEQUENCE_VERBOSE_ACTIVE
  #if BOOT_SEQUENCE_VERBOSE || \
      (BOOT_SEQUENCE_VERBOSE_IN_DEBUG && (SPECTRE_DEBUG_PROFILE >= SPECTRE_DEBUG_PROFILE_DEBUG))
    #define BOOT_SEQUENCE_VERBOSE_ACTIVE ON
  #else
    #define BOOT_SEQUENCE_VERBOSE_ACTIVE OFF
  #endif
#endif


// =============================================================================
// PART 3 — HARDWARE MAP
// =============================================================================
// Board facts. Wrong values here mean a dead screen or a dead radio, not a
// behaviour change.

// ── Display ──────────────────────────────────────────────────────────────────
// Only LCD_BL and LCD_POWER are driven from application code. The bus pins are
// owned by TFT_eSPI through -DTFT_* in platformio.ini; the copies here are for
// reference and MUST match, or the panel stays dark.
#define LCD_BL      38   // backlight PWM
#define LCD_POWER   15   // panel power rail
#define LCD_CS      6
#define LCD_DC      7
#define LCD_RST     5
#define LCD_MOSI    11
#define LCD_SCLK    12
#define LCD_RD      9
#define LCD_WR      8
#define LCD_D0      39

// Screen geometry lives in ui/Theme.h as THEME_*, which is what the display
// code actually reads. Do not reintroduce a second set of constants here.

// ── Buttons ──────────────────────────────────────────────────────────────────
#define BTN_A       0    // IO0, top
#define BTN_B       14   // IO14, bottom
#define BOOT_RECOVERY_BUTTON_PIN    BTN_B
#define BOOT_RECOVERY_HOLD_MS       1500UL

// ── Battery sense ────────────────────────────────────────────────────────────
#define BAT_ADC     4    // LCD_BAT_VOLT

#define POWER_BATTERY_DIVIDER_NUM          2
#define POWER_BATTERY_DIVIDER_DEN          1
#define POWER_BATTERY_CAPACITY_DEFAULT_MAH 1100
#define POWER_BATTERY_CAPACITY_MIN_MAH     50
#define POWER_BATTERY_CAPACITY_MAX_MAH     5000
#define POWER_USB_SENSE_PIN                -1   // -1 = not wired on this board
#define POWER_USB_SENSE_ACTIVE             HIGH
#define POWER_CHARGE_SENSE_PIN             -1
#define POWER_CHARGE_SENSE_ACTIVE          LOW
#define POWER_ECONOMY_PERCENT              20   // enter economy mode below this %
#define POWER_CRITICAL_RUNTIME_MIN         3    // minutes left -> critical

// ── RYLR998 LoRa UART (legacy backend) ───────────────────────────────────────
#define LORA_TX     18   // ESP TX -> RYLR RX
#define LORA_RX     17   // ESP RX <- RYLR TX
#define LORA_UART   2

// ── XIAO nRF52840 + Wio-SX1262 coprocessor ───────────────────────────────────
// Spectre stays the controller; the nRF drives the SX1262 as a thin modem
// (SUBGHZ_* verbs over UART). Preferred over RYLR998 when CAPS reports it.
#define WIO_NRF_UART_NUM          1
#define WIO_NRF_UART_TX           1    // ESP TX -> XIAO D7 RX
#define WIO_NRF_UART_RX           2    // ESP RX <- XIAO D6 TX
#define WIO_NRF_BAUD              115200

// ── SX1262 sub-GHz ───────────────────────────────────────────────────────────
#define SUBGHZ_SX1262_DEFAULT_FREQ_HZ 915000000UL
#define SUBGHZ_SX1262_TX_POWER_DBM    17
#define SUBGHZ_SX1262_NATIVE_SYNC     0x12   // private sync, Spectre-native frames

// Meshtastic shares the same SX1262 and the radio holds one PHY profile at a
// time, so this is mutually exclusive with native SubGhz.
// US 915 LongFast, default public channel.
#define MESHTASTIC_FREQ_HZ            906875000UL  // channel 0 center
#define MESHTASTIC_BW_HZ              250000UL
#define MESHTASTIC_SF                 11
#define MESHTASTIC_CR                 5            // 4/5, RadioLib notation
#define MESHTASTIC_PREAMBLE           16
#define MESHTASTIC_SYNC_WORD          0x2B
#define MESHTASTIC_TX_POWER_DBM       22
#define MESHTASTIC_MAX_NODES          32

// ── Wi-Fi antenna switching ──────────────────────────────────────────────────
// This board ties the antenna control line to GPIO0, which is also BTN_A, so
// switching is disabled here. The mode constants and pin map are kept for
// boards that do wire a real switch.
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
#define WIFI_ANTENNA_CTRL_PIN        0
#define WIFI_ANTENNA_INTERNAL_LEVEL  LOW
#define WIFI_ANTENNA_EXTERNAL_LEVEL  HIGH
#define WIFI_ANTENNA_GPIO_ANT0      -1
#define WIFI_ANTENNA_GPIO_ANT1      -1
#define WIFI_ANTENNA_INTERNAL_PATH   0
#define WIFI_ANTENNA_EXTERNAL_PATH   1

// Button action vocabulary and per-screen binding tables live in
// core/ButtonBindings.h, included at the top of this file. Edit bindings there.
