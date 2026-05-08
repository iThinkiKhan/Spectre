

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
#include <esp_core_dump.h>
#endif
#include <math.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>
#include <esp_freertos_hooks.h>

#include "config.h"
#include "core/CrashBreadcrumb.h"
#include "core/EventBus.h"
#include "core/MissionRuntime.h"
#include "core/ExecutionPolicy.h"
#include "core/Session.h"
#include "core/SpectreState.h"
#include "core/NotifTypes.h"
#include "core/RuntimeContracts.h"
#include "managers/ButtonHandler.h"
#include "managers/DisplayManager.h"
#include "managers/ExportManager.h"
#include "managers/LoRaManager.h"
#include "managers/BadUsbManager.h"
#include "managers/SubGhzManager.h"
#include "managers/ReyaxBackend.h"
#include "managers/SubGhzRecordWriter.h"
#include "managers/StorageManager.h"
#include "managers/MQTTManager.h"
#include "managers/PowerManager.h"
#include "managers/WiFiManager.h"
#include "protocol/CompanionProtocol.h"
#include "managers/BLEManager.h"
#include "managers/RadioArbiter.h"
#include "managers/SettingsManager.h"
#include "managers/TimeService.h"
#include "core/DebugLog.h"
#include "data/FieldVault.h"
#include "ui/BootSequence.h"
#include "ui/LVGLDriver.h"
#include "ui/Theme.h"
#include "ui/PrebootFallback.h"

// ── Hardware objects ──
TFT_eSPI        tft = TFT_eSPI();
ButtonHandler   buttons;
DisplayManager  display;
LoRaManager     lora;
ReyaxBackend    subghzReyax(lora);

namespace {
    // Compile-time subsystem mask, resolved from the SPECTRE_DEBUG_AREA_*
    // toggles in config.h. Only consulted by DEBUG/DEV profiles; OFF rejects
    // everything and RUN passes WARN/ERROR for any subsystem.
    constexpr uint32_t kDebugSubsystemMask =
        (SPECTRE_DEBUG_AREA_GENERAL  ? DEBUG_AREA_GENERAL  : 0u) |
        (SPECTRE_DEBUG_AREA_CORE     ? DEBUG_AREA_CORE     : 0u) |
        (SPECTRE_DEBUG_AREA_SETTINGS ? DEBUG_AREA_SETTINGS : 0u) |
        (SPECTRE_DEBUG_AREA_STORAGE  ? DEBUG_AREA_STORAGE  : 0u) |
        (SPECTRE_DEBUG_AREA_TIME     ? DEBUG_AREA_TIME     : 0u) |
        (SPECTRE_DEBUG_AREA_RADIO    ? DEBUG_AREA_RADIO    : 0u) |
        (SPECTRE_DEBUG_AREA_WIFI     ? DEBUG_AREA_WIFI     : 0u) |
        (SPECTRE_DEBUG_AREA_BLE      ? DEBUG_AREA_BLE      : 0u) |
        (SPECTRE_DEBUG_AREA_MQTT     ? DEBUG_AREA_MQTT     : 0u) |
        (SPECTRE_DEBUG_AREA_EXPORT   ? DEBUG_AREA_EXPORT   : 0u) |
        (SPECTRE_DEBUG_AREA_GPS      ? DEBUG_AREA_GPS      : 0u) |
        (SPECTRE_DEBUG_AREA_MODE     ? DEBUG_AREA_MODE     : 0u);

    constexpr uint32_t DISPLAY_FRAME_INTERVAL_MS = 50;
    constexpr uint32_t DISPLAY_MASCOT_INTERVAL_MS = 140;
    constexpr uint32_t PWNY_SCREEN_REFRESH_MS = 750;

    constexpr size_t USB_CONSOLE_BUF_SIZE = 128;
    char g_usbConsoleBuf[USB_CONSOLE_BUF_SIZE] = {};
    size_t g_usbConsoleLen = 0;
    bool g_usbSerialAttachedAtBoot = false;
    bool g_bootRecoveryMode = false;

    struct DisplayFrameState {
    Screen      currentScreen = SCREEN_LORA;
    MascotState mascotState = MASCOT_STANDBY;
    bool     uploadActive;
    bool     radioBusy;
    uint16_t uploadPercent;
    uint32_t uploadPublished;
    uint32_t uploadTotal;
    char     uploadPhase[16];
    bool        requestSleep = false;
    bool        screenChanged = false;
    bool        dataRefresh = false;
    bool        loraNewPacket = false;
    bool        textInputPending = false;
    bool        wifiListActive = false;
    bool        missionListActive = false;
    bool        badUsbListActive = false;
    bool        debriefActive = false;
    bool        badUsbArmed = false;
    bool        badUsbRunning = false;
    bool        badUsbReady = false;
    int         battPercent = 0;
    uint16_t    battVoltageMv = 0;
    int16_t     battTrendMvPerMin = 0;
    uint16_t    battCapacityMah = 0;
    uint16_t    battRuntimeMin = 0;
    uint8_t     powerSource = POWER_SOURCE_UNKNOWN;
    uint8_t     powerState = POWER_STATE_BATTERY_NORMAL;
    bool        charging = false;
    uint32_t    criticalSinceMs = 0;
    uint32_t    criticalSleepAtMs = 0;
    bool        wifiConnected = false;
    bool        bleConnected = false;
    bool        loraReady = false;
    uint8_t     radioOwner = 0;
    char        loraLastPayload[64] = "--";
    int         loraRSSI = 0;
    int         loraSNR = 0;
    int         loraPacketCount = 0;
    char        subGhzModule[24] = "";
    uint8_t     subGhzMode = static_cast<uint8_t>(SubGhzMode::OFF);
    uint32_t    subGhzFrequencyHz = 0;
    int         subGhzNodeCount = 0;
    char        wifiSSID[32] = "";
    int         wifiNetworkCount = 0;
    int         probePacketCount = 0;
    char        lastProbedMAC[18] = "";
    uint8_t     runContext = static_cast<uint8_t>(RUN_CONTEXT_GENERAL);
    uint8_t     activeMissionProfile = static_cast<uint8_t>(MISSION_RECON);
    uint8_t     missionSelection = static_cast<uint8_t>(MISSION_RECON);
    float       battVoltage = 0.0f;
    unsigned long uptimeMs = 0;
    char        storageStr[32] = "0KB";
    };

    struct ButtonRoutingState {
    bool listActive = false;
    bool missionListActive = false;
    bool badUsbListActive = false;
    bool debriefActive = false;
    bool badUsbArmed = false;
    bool badUsbRunning = false;
    bool badUsbReady = false;
    uint8_t runContext = static_cast<uint8_t>(RUN_CONTEXT_GENERAL);
    uint8_t activeMissionProfile = static_cast<uint8_t>(MISSION_RECON);
    uint8_t missionSelection = static_cast<uint8_t>(MISSION_RECON);
    Screen currentScreen = SCREEN_LORA;
    };

    struct CoreLoadMonitor {
        volatile uint32_t idleHits[2] = {0, 0};
        uint32_t lastIdleHits[2] = {0, 0};
        float idlePeakPerMs[2] = {0.0f, 0.0f};
        uint8_t busyPct[2] = {0, 0};
        bool installed[2] = {false, false};
        uint32_t lastUpdateMs = 0;
    };

CoreLoadMonitor g_coreLoad;

static bool _idleHookCore0() {
    g_coreLoad.idleHits[0] = g_coreLoad.idleHits[0] + 1;
    return false;
}

static bool _idleHookCore1() {
    g_coreLoad.idleHits[1] = g_coreLoad.idleHits[1] + 1;
    return false;
}

static void _initCoreLoadMonitor() {
    g_coreLoad.lastUpdateMs = millis();

    if (esp_register_freertos_idle_hook_for_cpu(_idleHookCore0, 0) == ESP_OK) {
        g_coreLoad.installed[0] = true;
    } else {
        DLOG_WARN("CORE", "Failed to register idle hook for core0");
    }

    if (esp_register_freertos_idle_hook_for_cpu(_idleHookCore1, 1) == ESP_OK) {
        g_coreLoad.installed[1] = true;
    } else {
        DLOG_WARN("CORE", "Failed to register idle hook for core1");
    }

    DLOG_INFO("CORE", "Idle hooks core0=%d core1=%d",
              g_coreLoad.installed[0] ? 1 : 0,
              g_coreLoad.installed[1] ? 1 : 0);
}

static void _updateCoreLoad(uint32_t nowMs) {
    if (g_coreLoad.lastUpdateMs == 0) {
        g_coreLoad.lastUpdateMs = nowMs;
        return;
    }

    const uint32_t elapsedMs = nowMs - g_coreLoad.lastUpdateMs;
    if (elapsedMs < 1000UL) {
        return;
    }

    for (int core = 0; core < 2; ++core) {
        const uint32_t idleNow = g_coreLoad.idleHits[core];
        const uint32_t idleDelta = idleNow - g_coreLoad.lastIdleHits[core];
        g_coreLoad.lastIdleHits[core] = idleNow;

        const float idlePerMs = (elapsedMs > 0)
            ? (static_cast<float>(idleDelta) / static_cast<float>(elapsedMs))
            : 0.0f;

        if (idlePerMs > g_coreLoad.idlePeakPerMs[core]) {
            g_coreLoad.idlePeakPerMs[core] = idlePerMs;
        }

        const float peak = g_coreLoad.idlePeakPerMs[core];
        if (peak <= 0.0f) {
            g_coreLoad.busyPct[core] = 0;
            continue;
        }

        float idlePct = (idlePerMs / peak) * 100.0f;
        if (idlePct < 0.0f) idlePct = 0.0f;
        if (idlePct > 100.0f) idlePct = 100.0f;

        const int busy = 100 - static_cast<int>(idlePct + 0.5f);
        g_coreLoad.busyPct[core] = static_cast<uint8_t>(busy < 0 ? 0 : (busy > 100 ? 100 : busy));
    }

    g_coreLoad.lastUpdateMs = nowMs;
}

    struct UiRefreshState {
        Screen currentScreen = SCREEN_LORA;
        bool debriefActive = false;
        uint8_t activeMissionProfile = static_cast<uint8_t>(MISSION_RECON);
    };
}

// ── Task handles ──
TaskHandle_t taskDisplayHandle  = nullptr;
TaskHandle_t taskHardwareHandle = nullptr;


// Stack high-water marks (2026-04-26): TaskDisplay min_free=20 KB, TaskHardware min_free=25 KB.
// FreeRTOS reports the minimum free stack ever observed for the task; this
// watermark can fall after deep MQTT/WiFi call paths and will not rebound.
// Reduced from 24/32 KB to 18/22 KB — still leaves >10 KB headroom on each task.
// Revisit if min_free ever drops below 4 KB on either task.
static constexpr uint32_t TASK_DISPLAY_STACK_BYTES  = 18432;
static constexpr uint32_t TASK_HARDWARE_STACK_BYTES = 22528;
static constexpr uint32_t STACK_LOG_INTERVAL_MS     = 30000UL;
static constexpr uint32_t HEALTH_LOG_INTERVAL_MS    = 30000UL;
static constexpr uint32_t HEAP_CHECK_INTERVAL_MS    = 120000UL;
static constexpr uint32_t SLEEP_PRESENTATION_HOLD_MS = 1200UL;
static constexpr uint32_t SLEEP_PRESENTATION_TIMEOUT_MS = 3000UL;
static portMUX_TYPE s_displayPowerMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_lastUiActivityMs = 0;
static volatile bool s_displayAwake = true;
static volatile bool s_displayLayerReady = false;
static volatile uint8_t s_displayBrightnessPct = 100;

static void _markUiActivity() {
    portENTER_CRITICAL(&s_displayPowerMux);
    s_lastUiActivityMs = millis();
    portEXIT_CRITICAL(&s_displayPowerMux);
}

static uint32_t _lastUiActivityMs() {
    uint32_t value = 0;
    portENTER_CRITICAL(&s_displayPowerMux);
    value = s_lastUiActivityMs;
    portEXIT_CRITICAL(&s_displayPowerMux);
    return value;
}

static bool _isDisplayAwake() {
    bool awake = false;
    portENTER_CRITICAL(&s_displayPowerMux);
    awake = s_displayAwake;
    portEXIT_CRITICAL(&s_displayPowerMux);
    return awake;
}

static bool _setDisplayAwake(bool awake) {
    bool changed = false;
    portENTER_CRITICAL(&s_displayPowerMux);
    if (s_displayAwake != awake) {
        s_displayAwake = awake;
        changed = true;
    }
    portEXIT_CRITICAL(&s_displayPowerMux);

    if (changed) {
        const uint8_t brightnessPct = s_displayBrightnessPct;
        const uint8_t pwm = awake
            ? static_cast<uint8_t>((static_cast<uint16_t>(brightnessPct) * 255U) / 100U)
            : 0U;
        analogWrite(LCD_BL, pwm);
    }
    return changed;
}

static void _setDisplayBrightnessPct(uint8_t brightnessPct) {
    bool awake = false;
    uint8_t nextPct = constrain(brightnessPct, static_cast<uint8_t>(0), static_cast<uint8_t>(100));

    portENTER_CRITICAL(&s_displayPowerMux);
    s_displayBrightnessPct = nextPct;
    awake = s_displayAwake;
    portEXIT_CRITICAL(&s_displayPowerMux);

    if (awake) {
        const uint8_t pwm =
            static_cast<uint8_t>((static_cast<uint16_t>(nextPct) * 255U) / 100U);
        analogWrite(LCD_BL, pwm);
    }
}

static const char* _subGhzModeShort(uint8_t mode) {
    switch (static_cast<SubGhzMode>(mode)) {
        case SubGhzMode::OFF:      return "OFF";
        case SubGhzMode::MONITOR:  return "MON";
        case SubGhzMode::DISCOVER: return "DISC";
        case SubGhzMode::BEACON:   return "BCN";
        case SubGhzMode::MESSAGE:  return "MSG";
        case SubGhzMode::TEST:     return "TEST";
        default:                   return "?";
    }
}

void _queueNotification(uint8_t type, const char* text);

static bool _setSubGhzMode(SubGhzMode mode) {
    if (!SUBGHZ.isReady()) {
        DLOG_WARN("SUBGHZ", "Mode change skipped: backend not ready");
        _queueNotification(NOTIF_DEVICE_NEW, "SUBGHZ NOT READY");
        return false;
    }

    if (!SUBGHZ.setMode(mode)) {
        DLOG_WARN("SUBGHZ", "Mode change failed: %s", subGhzModeName(mode));
        _queueNotification(NOTIF_DEVICE_NEW, "SUBGHZ MODE FAIL");
        return false;
    }

    const SubGhzStatus sg = SUBGHZ.status();
    STATE_WRITE_BEGIN();
    g_state.subGhzMode = static_cast<uint8_t>(sg.mode);
    g_state.subGhzFrequencyHz = sg.frequencyHz;
    strlcpy(g_state.subGhzBackend, sg.backendName, sizeof(g_state.subGhzBackend));
    strlcpy(g_state.subGhzModule, sg.moduleName, sizeof(g_state.subGhzModule));
    g_state.screenChanged = true;
    g_state.dataRefresh = true;
    STATE_WRITE_END();

    char notifText[48] = {};
    snprintf(notifText, sizeof(notifText), "SUBGHZ %s", _subGhzModeShort(static_cast<uint8_t>(sg.mode)));
    _queueNotification(NOTIF_DEVICE_NEW, notifText);
    DLOG_INFO("SUBGHZ", "Mode set to %s", subGhzModeName(sg.mode));
    return true;
}

static bool _cycleSubGhzMode(int delta) {
    static constexpr SubGhzMode cycleModes[] = {
        SubGhzMode::MONITOR,
        SubGhzMode::DISCOVER,
        SubGhzMode::BEACON,
        SubGhzMode::TEST,
        SubGhzMode::OFF
    };

    int currentIndex = 0;
    const SubGhzMode currentMode = SUBGHZ.mode();
    for (size_t i = 0; i < (sizeof(cycleModes) / sizeof(cycleModes[0])); ++i) {
        if (cycleModes[i] == currentMode) {
            currentIndex = static_cast<int>(i);
            break;
        }
    }

    const int modeCount = static_cast<int>(sizeof(cycleModes) / sizeof(cycleModes[0]));
    int next = currentIndex + delta;
    while (next < 0) next += modeCount;
    next %= modeCount;
    return _setSubGhzMode(cycleModes[next]);
}

static void _sendSubGhzTestPing() {
    if (!SUBGHZ.isReady()) {
        DLOG_WARN("SUBGHZ", "Test ping skipped: backend not ready");
        return;
    }

    const bool ok = SUBGHZ.send("SPECTRE-PING", 0);

    if (ok) {
        BUS.publish(EVT_LORA_PACKET_TX);
        _queueNotification(NOTIF_DEVICE_NEW, "SUBGHZ PING");
        DLOG_INFO("SUBGHZ", "Test ping sent via %s", SUBGHZ.backendName());
    } else {
        BUS.publish(EVT_LORA_TX_FAIL);
        _queueNotification(NOTIF_DEVICE_NEW, "SUBGHZ PING FAIL");
        DLOG_WARN("SUBGHZ", "Test ping failed via %s", SUBGHZ.backendName());
    }
}

// ── Forward declarations ──
void TaskDisplay(void* pvParameters);
void TaskHardware(void* pvParameters);
void _checkLocationTag();
void _captureDisplayFrameState(DisplayFrameState& snapshot);
ButtonRoutingState _readButtonRoutingState();
UiRefreshState _readUiRefreshState();
void _applyExportSummaryToState(const SessionExportSummary& summary, bool ok);
void _runSessionExport(bool notifyUser);
void _pollUsbSerialConsole();
void _handleUsbConsoleLine(const char* rawLine);
void _printUsbConsoleHelp();
void _printUsbDebugStatus();
uint32_t _debugAreaMaskForToken(const String& token);
void _dispatchUiCommand(UICommand cmd);
void _queueNotification(uint8_t type, const char* text);
bool _requestBleTextEntry(const char* leaseReason, const char* prompt);
SpectreButtonAction _actionForEvent(ButtonEvent evt, const ButtonBindingSet& bindings);
bool _runButtonAction(SpectreButtonAction action, bool storageOk);
void _handleDisplayEvent(const Event& event, const DisplayFrameState& snapshot);
const char* _buttonEventName(ButtonEvent evt);
const char* _buttonActionName(SpectreButtonAction action);
static MissionProfile _sanitizeMissionProfile(uint8_t rawProfile);
const char* _screenName(Screen screen);
void _logRuntimeHealth(uint32_t nowMs);
bool _waitForDisplayLayerReady(uint32_t timeoutMs);
void _publishStorageState(bool storageOk, const String& storageUsed);
void _loadKnownLocationsIntoState();
static const char* _resetReasonName(esp_reset_reason_t r);
static const char* _fieldVaultPowerSourceName(PowerSource source);
static bool _detectBootRecoveryRequest();
void _applySubGhzStatusToState(const SubGhzStatus& status);
void _applyPowerSnapshotToState(const PowerSnapshot& power);
void _initializeHardwareManagers(uint32_t& lastWifiTick);
void _publishHardwareReadyState();
void _publishUiDataRefresh();
void _servicePeriodicUiRefresh(const UiRefreshState& uiRefresh,
                               uint32_t nowMs,
                               ExecutionPolicy::UiRefreshMarks& marks,
                               const ExecutionPolicy::UiRefreshSchedule& schedule);
void _refreshDisplayStatusBar(const DisplayFrameState& snapshot,
                              bool& statusValid,
                              ExecutionPolicy::StatusBarStateView& lastStatus);
void _requestSleepTransition(bool storageOk,
                             bool finalizeSession,
                             bool exportSession,
                             const char* reason);
uint32_t _displayFrameIntervalForPowerState(uint8_t powerState);
uint32_t _displayMascotIntervalForPowerState(uint8_t powerState);
uint8_t _displayBrightnessForPowerState(uint8_t powerState);
ExecutionPolicy::UiRefreshSchedule _uiRefreshScheduleForPowerState(uint8_t powerState);
void _checkRuntimeContracts();
static bool _buildPendingEnrichmentBatch(EventBatchRecord* out,
                                         size_t maxCount,
                                         size_t& outCount);
static bool _applyPhoneEnrichmentBatch(const PendingEnrichment* records,
                                       size_t count,
                                       uint32_t& outApplied,
                                       uint32_t& outFailed,
                                       uint32_t& outStorageMs);
static PhoneStorageFrameV1 _buildPhoneStorageFrame();

enum PhoneProbeReason : uint8_t {
    PHONE_PROBE_BACKLOG = 0,
    PHONE_PROBE_OFFLOAD_PREP,
    PHONE_PROBE_MANUAL,
    PHONE_PROBE_TIME_SYNC,
    PHONE_PROBE_AVAILABILITY
};

enum CompanionPhoneState : uint8_t {
    COMPANION_PHONE_UNKNOWN = 0,
    COMPANION_PHONE_AVAILABLE,
    COMPANION_PHONE_UNAVAILABLE
};

enum CompanionWorkState : uint8_t {
    COMPANION_WORK_IDLE = 0,
    COMPANION_WORK_PROBING,
    COMPANION_WORK_ENRICHING
};

struct CompanionScheduler {
    bool enabled = PHONE_COMPANION_ENABLED;

    CompanionPhoneState phoneState = COMPANION_PHONE_UNKNOWN;
    CompanionWorkState workState = COMPANION_WORK_IDLE;

    uint32_t nextProbeMs = 0;
    uint32_t lastSeenMs = 0;
    uint32_t lastProbeMs = 0;
    uint32_t lastEnrichMs = 0;
    uint32_t lastStoragePublishMs = 0;
    uint32_t lastHighValueWifiMs = 0;

    uint32_t pendingItems = 0;
    uint32_t pendingMissionItems = 0;
    uint32_t pendingNoiseItems = 0;
    uint32_t lastPendingRefreshMs = 0;
    bool manualProbeRequested = false;   // one-shot: bypasses gap check once
    bool manualEnrichRequested = false;
    bool offloadPrepRequested = false;
    bool timeSyncRequested = false;
    bool enrichmentRequestIssued = false;
    size_t lastRequestedEnrichmentCount = 0;
    uint32_t enrichmentSessionStartMs = 0;
    uint32_t enrichmentSessionRequested = 0;
    uint32_t enrichmentSessionApplied = 0;
    uint32_t enrichmentSessionFailed = 0;
    uint32_t enrichmentSessionBatches = 0;
    uint32_t enrichmentSessionXferMs = 0;
    uint32_t enrichmentSessionStorageMs = 0;

    // Probe backoff state.
    // Stage 0 — normal cadence (PHONE_PROBE_MIN_GAP_MS).
    // Stage 1/2/3 — still frequent enough to catch Android Field Mode
    // advertising during a carry instead of waiting out long backoff gaps.
    // Resets to stage 0 on any successful probe.
    uint8_t probeBackoffStage    = 0;
    uint8_t probeBackoffMissCount = 0;
};

static void _finishPhoneEnrichment(CompanionScheduler& cs, bool success);
static void initEnrichQueue();
static void serviceEnrichmentPipeline(CompanionScheduler& cs);

static constexpr uint32_t WIFI_LULL_MIN_MS = 10000UL;
static constexpr uint32_t PHONE_PROBE_MIN_GAP_MS = 30000UL;
static constexpr uint32_t PHONE_AVAILABILITY_TTL_MS = 300000UL;
static constexpr uint32_t PHONE_STORAGE_PUBLISH_MIN_MS = 15000UL;

// Probe absence backoff ladder. Keep this fairly aggressive in field mode:
// the phone is expected to advertise continuously, so missed links should
// recover in minutes, not disappear for the rest of a carry.
static constexpr uint8_t PROBE_BACKOFF_STAGES = 4;
static constexpr uint8_t PROBE_BACKOFF_MISS_LIMIT[PROBE_BACKOFF_STAGES] = {
    1,   // stage 0 -> stage 1 after the first automatic miss
    2,   // stage 1 -> stage 2 after two misses
    2,   // stage 2 -> stage 3 after two misses
    0,   // stage 3: indefinite (never advances further)
};
static constexpr uint32_t PROBE_BACKOFF_INTERVAL_MS[PROBE_BACKOFF_STAGES] = {
    PHONE_PROBE_MIN_GAP_MS,   // stage 0: first automatic retry window
     1UL * 60000UL,           // stage 1: 1 min
     2UL * 60000UL,           // stage 2: 2 min
     5UL * 60000UL,           // stage 3: 5 min floor
};
static constexpr uint32_t ENRICH_MIN_GAP_MS = 60000UL;
static constexpr size_t PHONE_ENRICH_BATCH_MAX   = PHONE_COMPANION_ENRICH_BATCH_MAX;
static constexpr size_t ENRICH_QUEUE_DEPTH       = 2;
static constexpr size_t ENRICH_CLAIM_MAX         = ENRICH_QUEUE_DEPTH * PHONE_ENRICH_BATCH_MAX;
static constexpr uint32_t REPAIR_BUDGET_MS       = 8;
static constexpr uint16_t REPAIR_MAX_RECORDS     = 64;
static constexpr uint32_t REPAIR_UI_QUIET_MS     = 500;
static constexpr uint32_t REPAIR_HEAP_FREE_GUARD = 96UL * 1024UL;
static constexpr uint32_t REPAIR_HEAP_BLOCK_GUARD = 32UL * 1024UL;
static constexpr uint32_t REPAIR_WIFI_CAPTURE_PENDING_LIMIT = 512UL;
static constexpr uint32_t HARDWARE_LOOP_GAP_WARN_MS = 250UL;

struct QueuedEnrichBatch {
    PendingEnrichment records[PHONE_ENRICH_BATCH_MAX];
    size_t    count     = 0;
    uint32_t  queuedMs  = 0;
};

static QueuedEnrichBatch enrichQueue[ENRICH_QUEUE_DEPTH];
static size_t    enrichQueueHead  = 0;
static size_t    enrichQueueTail  = 0;
static size_t    enrichQueueSize  = 0;
static uint32_t  enrichClaimedEventIds[ENRICH_CLAIM_MAX];
static size_t    enrichClaimedCount = 0;

static bool companionHasPriorityReason(const CompanionScheduler& cs) {
    return cs.manualProbeRequested ||
           cs.manualEnrichRequested ||
           cs.offloadPrepRequested ||
           cs.timeSyncRequested;
}

static bool companionHasEnrichmentWork(const CompanionScheduler& cs) {
    return cs.pendingItems > 0 ||
           cs.manualEnrichRequested ||
           cs.offloadPrepRequested ||
           cs.timeSyncRequested;
}

static void resetEnrichmentSessionStats(CompanionScheduler& cs) {
    cs.enrichmentSessionStartMs = millis();
    cs.enrichmentSessionRequested = 0;
    cs.enrichmentSessionApplied = 0;
    cs.enrichmentSessionFailed = 0;
    cs.enrichmentSessionBatches = 0;
    cs.enrichmentSessionXferMs = 0;
    cs.enrichmentSessionStorageMs = 0;
}

static bool companionPhoneAvailabilityStale(const CompanionScheduler& cs) {
    return cs.phoneState == COMPANION_PHONE_AVAILABLE &&
           (cs.lastSeenMs == 0 ||
            millis() - cs.lastSeenMs > PHONE_AVAILABILITY_TTL_MS);
}

static const char* phoneProbeReasonName(PhoneProbeReason reason) {
    switch (reason) {
        case PHONE_PROBE_BACKLOG:      return "backlog_probe";
        case PHONE_PROBE_OFFLOAD_PREP: return "offload_prep";
        case PHONE_PROBE_MANUAL:       return "manual_probe";
        case PHONE_PROBE_TIME_SYNC:    return "time_sync";
        case PHONE_PROBE_AVAILABILITY: return "availability_probe";
        default:                       return "probe";
    }
}

static const char* companionPhoneStateName(CompanionPhoneState s) {
    switch (s) {
        case COMPANION_PHONE_UNKNOWN:     return "UNKNOWN";
        case COMPANION_PHONE_AVAILABLE:   return "AVAILABLE";
        case COMPANION_PHONE_UNAVAILABLE: return "UNAVAILABLE";
        default:                          return "?";
    }
}

static const char* companionWorkStateName(CompanionWorkState s) {
    switch (s) {
        case COMPANION_WORK_IDLE:      return "IDLE";
        case COMPANION_WORK_PROBING:   return "PROBING";
        case COMPANION_WORK_ENRICHING: return "ENRICHING";
        default:                       return "?";
    }
}

static const char* bleLinkStateName(BLEManager::LinkState s) {
    switch (s) {
        case BLEManager::BLE_IDLE:       return "IDLE";
        case BLEManager::BLE_SCANNING:   return "SCANNING";
        case BLEManager::BLE_CONNECTING: return "CONNECTING";
        case BLEManager::BLE_CONNECTED:  return "CONNECTED";
        case BLEManager::BLE_SUBSCRIBED: return "SUBSCRIBED";
        default:                         return "?";
    }
}

static const char* radioOwnerName(RadioOwner owner) {
    switch (owner) {
        case RADIO_NONE:         return "none";
        case RADIO_WIFI_CAPTURE: return "wifi_capture";
        case RADIO_WIFI_SCAN:    return "wifi_scan";
        case RADIO_WIFI_PMKID:   return "wifi_pmkid";
        case RADIO_WIFI_UPLOAD:  return "wifi_upload";
        case RADIO_BLE_TEXT:     return "ble_text";
        case RADIO_BLE_GPS:      return "ble_gps";
        default:                 return "?";
    }
}

// ---------------------------------------------------------------------------
// Companion inter-task command queue and status snapshot.
//
// g_companionCmd  — written by the USB console task (display loop),
//                   drained by TaskHardware. Volatile bools are sufficient
//                   for single-bit signals on this MCU.
// g_companionStatus — written by TaskHardware each companion tick,
//                   read by the USB console for "companion status".
//                   No lock: a slightly stale read is acceptable for a
//                   debug diagnostic.
// ---------------------------------------------------------------------------
struct CompanionCmd {
    volatile bool probe  = false;
    volatile bool enrich = false;
    volatile bool cancel = false;
};
static CompanionCmd g_companionCmd;

struct CompanionStatusSnapshot {
    bool enabled        = false;
    CompanionPhoneState   phoneState = COMPANION_PHONE_UNKNOWN;
    CompanionWorkState    workState  = COMPANION_WORK_IDLE;
    bool bleBegun        = false;
    bool bleRadioEnabled = false;
    BLEManager::LinkState bleState  = BLEManager::BLE_IDLE;
    bool phoneLinkReady       = false;
    bool phoneGpsReady        = false;
    bool phoneControlReady    = false;
    bool phoneEnrichmentReady = false;
    bool phoneFreshGps        = false;
    uint32_t pendingMission  = 0;
    uint32_t pendingNoise    = 0;
    uint32_t pendingTotal    = 0;
    uint32_t lastProbeAgeMs       = 0;
    uint32_t lastEnrichAgeMs      = 0;
    RadioOwner radioOwner         = RADIO_NONE;
    uint32_t lastBleInitAgeMs     = 0;
    uint32_t lastScanStartAgeMs   = 0;
    int      lastDisconnectReason = 0;
};
static CompanionStatusSnapshot g_companionStatus;

static bool isHighValueWiFiOwner(RadioOwner owner) {
    return owner == RADIO_WIFI_SCAN ||
           owner == RADIO_WIFI_PMKID ||
           owner == RADIO_WIFI_UPLOAD;
}

static bool isWiFiBusyForPhoneProbe(const CompanionScheduler& cs) {
    const RadioOwner owner = RADIO_ARB.currentOwner();

    if (owner == RADIO_WIFI_SCAN ||
        owner == RADIO_WIFI_PMKID ||
        owner == RADIO_WIFI_UPLOAD) {
        return true;
    }

    if (millis() - cs.lastHighValueWifiMs < WIFI_LULL_MIN_MS) {
        return true;
    }

    return false;
}

static bool hasActiveUiOperation(uint32_t nowMs) {
    if (nowMs - _lastUiActivityMs() < REPAIR_UI_QUIET_MS) {
        return true;
    }

    bool active = false;
    STATE_READ_BEGIN();
    active = g_state.textInputPending ||
             g_state.wifiListActive ||
             g_state.missionListActive ||
             g_state.badUsbListActive ||
             g_state.debriefActive ||
             g_state.badUsbRunning ||
             g_state.requestSleep;
    STATE_READ_END();
    return active;
}

static bool canRunStorageMaintenance(const CompanionScheduler& companion,
                                     const PowerSnapshot& power,
                                     uint32_t nowMs) {
    const RadioOwner owner = RADIO_ARB.currentOwner();
    if (owner != RADIO_WIFI_CAPTURE && owner != RADIO_NONE) {
        return false;
    }

    const uint32_t pendingUpload =
        STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U;
    if (owner == RADIO_WIFI_CAPTURE &&
        pendingUpload >= REPAIR_WIFI_CAPTURE_PENDING_LIMIT) {
        return false;
    }

    bool uploadActive = false;
    STATE_READ_BEGIN();
    uploadActive = g_state.uploadActive;
    STATE_READ_END();

    if (uploadActive ||
        MQTT_MGR.getState() != MQTT_IDLE ||
        STORAGE.isUploadBatchActive()) {
        return false;
    }

    if (companion.workState != COMPANION_WORK_IDLE) {
        return false;
    }

    if (hasActiveUiOperation(nowMs)) {
        return false;
    }

    const uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const uint32_t largestHeap =
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (freeHeap < REPAIR_HEAP_FREE_GUARD ||
        largestHeap < REPAIR_HEAP_BLOCK_GUARD) {
        return false;
    }

    if (power.state == POWER_STATE_BATTERY_CRITICAL ||
        (power.source == POWER_SOURCE_BATTERY && power.percent <= 3)) {
        return false;
    }

    return true;
}

static void _applyCompanionPendingFromMirror(CompanionScheduler& companion) {
    uint32_t mission = 0;
    uint32_t noise = 0;
    bool valid = false;

    STATE_READ_BEGIN();
    valid = g_state.storageSummaryValid;
    mission = g_state.storagePendingEnrichMission;
    noise = g_state.storagePendingEnrichNoise;
    STATE_READ_END();

    if (!valid) {
        return;
    }

    companion.pendingMissionItems = mission;
    companion.pendingNoiseItems = noise;
    companion.pendingItems = mission + noise;
}

static void refreshCompanionPending(CompanionScheduler& companion, bool force) {
    if (!companion.enabled) return;

    const uint32_t now = millis();
    if (!force && now - companion.lastPendingRefreshMs < 15000UL) {
        return;
    }

    const RadioOwner owner = RADIO_ARB.currentOwner();
    const bool radioActive =
        owner == RADIO_WIFI_CAPTURE ||
        owner == RADIO_WIFI_SCAN ||
        owner == RADIO_WIFI_PMKID ||
        owner == RADIO_BLE_GPS ||
        owner == RADIO_BLE_TEXT ||
        owner == RADIO_WIFI_UPLOAD;

    // Automatic backlog probes should not perform a full spool scan every time
    // the threshold is exceeded. At 15k-30k records that becomes background
    // pressure. Use the storage mirror and let explicit/manual paths or idle
    // windows do full recounts.
    if (!force || radioActive) {
        _applyCompanionPendingFromMirror(companion);
        companion.lastPendingRefreshMs = now;
        return;
    }

    StorageLaneCounts pending = STORAGE.getPendingEnrichmentCounts();

    companion.pendingMissionItems = pending.mission;
    companion.pendingNoiseItems = pending.noise;
    companion.pendingItems = pending.total();
    companion.lastPendingRefreshMs = now;
}

static PhoneStorageFrameV1 _buildPhoneStorageFrame() {
    PhoneStorageFrameV1 frame = {};
    frame.version = COMPANION_PROTOCOL_VERSION;

    STATE_READ_BEGIN();
    frame.flags =
        (g_state.storageSummaryValid ? PHONE_STORAGE_FLAG_VALID : 0) |
        (g_state.uploadActive ? PHONE_STORAGE_FLAG_UPLOAD_ACTIVE : 0) |
        (g_state.storageNearlyFull ? PHONE_STORAGE_FLAG_NEARLY_FULL : 0) |
        (g_state.storageFull ? PHONE_STORAGE_FLAG_FULL : 0) |
        (g_state.storageOverrun ? PHONE_STORAGE_FLAG_OVERRUN : 0);
    frame.storageMode = g_state.storageMode;
    frame.retentionPolicy = g_state.storagePolicy;
    frame.usedPct = g_state.storageUsedPct;
    frame.freeBytes = g_state.storageFreeBytes;
    frame.missionTotal = g_state.storageMissionTotal;
    frame.noiseTotal = g_state.storageNoiseTotal;
    frame.p0Total = g_state.storageP0Total;
    frame.p1Total = g_state.storageP1Total;
    frame.p2Total = g_state.storageP2Total;
    frame.p3Total = g_state.storageP3Total;
    frame.pendingUploadMission = g_state.storagePendingUploadMission;
    frame.pendingUploadNoise = g_state.storagePendingUploadNoise;
    frame.pendingEnrichMission = g_state.storagePendingEnrichMission;
    frame.pendingEnrichNoise = g_state.storagePendingEnrichNoise;
    frame.enrichmentDeltas = g_state.storageEnrichmentDeltas;
    frame.firstEventId = g_state.storageFirstEventId;
    frame.lastEventId = g_state.storageLastEventId;
    frame.updatedMs = g_state.storageSummaryUpdatedMs;
    STATE_READ_END();

    return frame;
}

static void publishPhoneStorageSnapshotIfDue(CompanionScheduler& cs, bool force = false) {
    if (!cs.enabled || !BLE_MGR.isPhoneStorageReady()) {
        return;
    }

    if (!RADIO_ARB.isOwner(RADIO_BLE_GPS) ||
        RADIO_ARB.currentOwner() == RADIO_WIFI_UPLOAD) {
        return;
    }

    const uint32_t now = millis();
    if (!force &&
        cs.lastStoragePublishMs != 0 &&
        now - cs.lastStoragePublishMs < PHONE_STORAGE_PUBLISH_MIN_MS) {
        return;
    }

    if (BLE_MGR.publishStorageSnapshot(_buildPhoneStorageFrame())) {
        cs.lastStoragePublishMs = now;
    }
}

static bool shouldRunPhoneProbe(const CompanionScheduler& cs) {
    if (!cs.enabled) {
        return false;
    }
    if (cs.workState != COMPANION_WORK_IDLE) {
        return false;
    }
    if (RADIO_ARB.isBleOwner()) {
        return false;
    }
    const bool priority = companionHasPriorityReason(cs);
    const bool bypassGap = (cs.lastProbeMs == 0) || priority;
    if (!bypassGap && millis() - cs.lastProbeMs < PHONE_PROBE_MIN_GAP_MS) {
        return false;
    }
    if (!priority && millis() < cs.nextProbeMs) {
        return false;
    }
    if (isWiFiBusyForPhoneProbe(cs)) {
        return false;
    }
    return true;
}

static bool shouldRunEnrichment(const CompanionScheduler& cs) {
    if (!cs.enabled) {
        return false;
    }
    if (cs.phoneState != COMPANION_PHONE_AVAILABLE) {
        return false;
    }
    if (cs.workState != COMPANION_WORK_IDLE) {
        return false;
    }
    if (RADIO_ARB.isBleOwner()) {
        return false;
    }
    if (isWiFiBusyForPhoneProbe(cs)) {
        return false;
    }
    const bool priority = companionHasPriorityReason(cs);
    if (!priority && millis() - cs.lastEnrichMs < ENRICH_MIN_GAP_MS) {
        return false;
    }

    if (priority) {
        return true;
    }

    return cs.pendingItems >= PHONE_COMPANION_ENRICH_THRESHOLD;
}

static bool requestPhoneProbeLease(CompanionScheduler& cs,
                                   PhoneProbeReason reason) {
    const char* probeReason = phoneProbeReasonName(reason);
    const bool allowCachedReconnect =
        reason == PHONE_PROBE_BACKLOG ||
        reason == PHONE_PROBE_OFFLOAD_PREP ||
        reason == PHONE_PROBE_TIME_SYNC;

    if (RADIO_ARB.currentOwner() == RADIO_WIFI_UPLOAD) {
        DLOG_WARN("BLE", "Phone probe skipped reason=%s upload active", probeReason);
        return false;
    }

    if (!RADIO_ARB.requestLease(
            RADIO_BLE_GPS,
            RadioArbiter::BLE_PHONE_PROBE_HOLD_MS,
            probeReason)) {
        return false;
    }

    if (!BLE_MGR.requestCompanionLink(probeReason, allowCachedReconnect)) {
        RADIO_ARB.release(RADIO_BLE_GPS, "probe_link_request_failed");
        cs.workState = COMPANION_WORK_IDLE;
        return false;
    }

    cs.workState = COMPANION_WORK_PROBING;
    cs.lastProbeMs = millis();
    cs.manualProbeRequested = false;   // consume one-shot
    crashCheckpoint(CrashPhase::BACKLOG_PROBE,
                    static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                    static_cast<uint32_t>(cs.pendingItems));
    DLOG_INFO("BLE", "Phone probe lease granted reason=%s cachedFirst=%u hold=%lums",
              probeReason,
              allowCachedReconnect ? 1u : 0u,
              static_cast<unsigned long>(RadioArbiter::BLE_PHONE_PROBE_HOLD_MS));
    return true;
}

static bool requestManualBleTest(const char* reason) {
    const RadioOwner owner = RADIO_ARB.currentOwner();

    if (owner == RADIO_WIFI_UPLOAD) {
        DLOG_WARN("BLE_TEST", "manual enrich rejected: upload active");
        return true;
    }

    if (owner == RADIO_BLE_TEXT) {
        DLOG_WARN("BLE_TEST", "manual enrich rejected: BLE text active");
        return true;
    }

    if (owner != RADIO_NONE && owner != RADIO_WIFI_CAPTURE) {
        if (owner == RADIO_BLE_GPS) {
            g_companionCmd.enrich = true;
            DLOG_INFO("BLE_TEST", "btcon enrich queued on active BLE link reason=%s",
                      reason ? reason : "ble_test");
            _queueNotification(NOTIF_DEVICE_NEW, "BLE ENRICH QUEUED");
            return true;
        }

        DLOG_WARN("BLE_TEST", "manual enrich rejected: owner=%s",
                  RADIO_ARB.ownerName(owner));
        return true;
    }

    g_companionCmd.enrich = true;
    DLOG_INFO("BLE_TEST", "btcon manual enrich queued reason=%s",
              reason ? reason : "ble_test");
    _queueNotification(NOTIF_DEVICE_NEW, "BLE ENRICH QUEUED");

    return true;
}

static bool requestPhoneEnrichmentLease(CompanionScheduler& cs,
                                        const char* reason) {
    if (RADIO_ARB.currentOwner() == RADIO_WIFI_UPLOAD) {
        DLOG_WARN("BLE", "Phone enrichment skipped reason=%s upload active",
                  reason ? reason : "enrich");
        return false;
    }

    if (!RADIO_ARB.requestLease(
            RADIO_BLE_GPS,
            RadioArbiter::BLE_PHONE_ENRICH_HOLD_MS,
            reason)) {
        return false;
    }

    if (!BLE_MGR.requestCompanionLink(reason ? reason : "enrich", true)) {
        RADIO_ARB.release(RADIO_BLE_GPS, "enrich_link_request_failed");
        return false;
    }

    cs.workState = COMPANION_WORK_ENRICHING;
    resetEnrichmentSessionStats(cs);
    cs.enrichmentRequestIssued = false;
    cs.lastRequestedEnrichmentCount = 0;
    initEnrichQueue();
    crashCheckpoint(CrashPhase::BACKLOG_ENRICH,
                    static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                    static_cast<uint32_t>(cs.pendingItems));
    DLOG_INFO("BLE", "Phone enrichment lease granted reason=%s",
              reason ? reason : "enrich");
    return true;
}

static bool _buildPendingEnrichmentBatch(EventBatchRecord* out,
                                         size_t maxCount,
                                         size_t& outCount) {
    outCount = 0;

    if (!out || maxCount == 0 || maxCount > PHONE_ENRICH_BATCH_MAX) {
        return false;
    }

    DLOG_INFO("BLE", "Enrichment pending scan begin claimed=%u max=%u",
              static_cast<unsigned>(enrichClaimedCount),
              static_cast<unsigned>(maxCount));

    PendingEventDescriptor pending[PHONE_ENRICH_BATCH_MAX] = {};
    if (!STORAGE.getPendingEnrichmentBatchExcluding(
            enrichClaimedEventIds, enrichClaimedCount,
            pending, maxCount, outCount)) {
        DLOG_WARN("BLE", "Failed to read spool enrichment backlog batch");
        return false;
    }

    DLOG_INFO("BLE", "Enrichment pending scan done count=%u",
              static_cast<unsigned>(outCount));

    for (size_t i = 0; i < outCount; ++i) {
        uint32_t eventEpochUtc = 0;
        out[i].eventId     = pending[i].eventId;
        out[i].timestampMs =
            TIME_SVC.epochForMillis(pending[i].timestampMs, eventEpochUtc)
                ? eventEpochUtc
                : pending[i].timestampMs;
        out[i].type        = pending[i].type;
        out[i].status      = pending[i].status;
    }

    return true;
}

static bool _applyPhoneEnrichmentBatch(const PendingEnrichment* records,
                                       size_t count,
                                       uint32_t& outApplied,
                                       uint32_t& outFailed,
                                       uint32_t& outStorageMs) {
    outApplied = 0;
    outFailed = 0;
    outStorageMs = 0;

    if (!records || count == 0) {
        return false;
    }

    if (!STORAGE.prepareForEnrichmentAppend(count)) {
        DLOG_WARN("BLE", "Enrich preflight rotate failed; dropping batch count=%u",
                  static_cast<unsigned>(count));
        return false;
    }

    bool anySuccess = false;
    uint32_t applied = 0;
    uint32_t failed = 0;
    uint32_t eventIds[PHONE_ENRICH_BATCH_MAX] = {};
    String sessionIds[PHONE_ENRICH_BATCH_MAX];

    const size_t lookupCount = std::min(count, PHONE_ENRICH_BATCH_MAX);
    for (size_t i = 0; i < lookupCount; ++i) {
        eventIds[i] = records[i].eventId;
    }

    if (!STORAGE.findEventSessions(eventIds, lookupCount, sessionIds)) {
        DLOG_WARN("BLE", "Enrichment session lookup failed");
        return false;
    }

    // Build a flat entry array for the batch writer.
    // Records with no session are counted as failures and skipped.
    SpoolEnrichBatchEntry batchEntries[PHONE_ENRICH_BATCH_MAX];
    size_t batchSize = 0;
    for (size_t i = 0; i < count; ++i) {
        const PendingEnrichment& r = records[i];
        if (r.eventId == 0) continue;
        if (i >= lookupCount || !sessionIds[i].length()) {
            failed++;
            DLOG_WARN("BLE", "Enrichment no session event=%lu",
                      static_cast<unsigned long>(r.eventId));
            continue;
        }
        batchEntries[batchSize++] = {
            r.eventId, sessionIds[i].c_str(),
            r.lat, r.lon, r.alt, r.accuracy, r.tag
        };
    }

    STORAGE.beginHotPathDiagnosticsSuppressed();
    const uint32_t tApplyStart = millis();

    uint32_t bApplied = 0;
    uint32_t bFailed  = 0;
    if (batchSize > 0) {
        STORAGE.appendEnrichDeltasBatch(batchEntries, batchSize, &bApplied, &bFailed);
    }
    applied   += bApplied;
    failed    += bFailed;
    anySuccess = (bApplied > 0);

    const uint32_t tApplyMs = millis() - tApplyStart;
    STORAGE.endHotPathDiagnosticsSuppressed();
    const uint32_t pending = STORAGE.getPendingEventCount();
    DLOG_INFO("BLE",
              "enrich_perf count=%u applied=%u failed=%u storageMs=%lu pendingUpload=%lu",
              static_cast<unsigned>(count),
              static_cast<unsigned>(applied),
              static_cast<unsigned>(failed),
              static_cast<unsigned long>(tApplyMs),
              static_cast<unsigned long>(pending));

    outApplied = applied;
    outFailed = failed;
    outStorageMs = tApplyMs;
    return anySuccess;
}

static void _finishPhoneEnrichment(CompanionScheduler& cs, bool success) {
    const uint32_t sessionStartMs = cs.enrichmentSessionStartMs;
    const uint32_t requested = cs.enrichmentSessionRequested;
    const uint32_t applied = cs.enrichmentSessionApplied;
    const uint32_t failed = cs.enrichmentSessionFailed;
    const uint32_t batches = cs.enrichmentSessionBatches;
    const uint32_t xferMs = cs.enrichmentSessionXferMs;
    const uint32_t storageMs = cs.enrichmentSessionStorageMs;
    const uint32_t totalMs = sessionStartMs ? (millis() - sessionStartMs) : 0U;

    if (sessionStartMs != 0 && (requested > 0 || batches > 0)) {
        DLOG_INFO("BLE",
                  "enrich_session_summary requested=%lu applied=%lu failed=%lu batches=%lu xferMs=%lu storageMs=%lu totalMs=%lu",
                  static_cast<unsigned long>(requested),
                  static_cast<unsigned long>(applied),
                  static_cast<unsigned long>(failed),
                  static_cast<unsigned long>(batches),
                  static_cast<unsigned long>(xferMs),
                  static_cast<unsigned long>(storageMs),
                  static_cast<unsigned long>(totalMs));
    }

    cs.workState = COMPANION_WORK_IDLE;
    cs.lastEnrichMs = millis();
    cs.enrichmentRequestIssued = false;
    cs.lastRequestedEnrichmentCount = 0;
    cs.enrichmentSessionStartMs = 0;
    cs.enrichmentSessionRequested = 0;
    cs.enrichmentSessionApplied = 0;
    cs.enrichmentSessionFailed = 0;
    cs.enrichmentSessionBatches = 0;
    cs.enrichmentSessionXferMs = 0;
    cs.enrichmentSessionStorageMs = 0;

    if (success) {
        // Decrement cached pending counts by what was applied this session.
        // A full spool scan here risks OOM on heap-depleted post-enrichment state
        // (10 batches of spool scans + map allocations leave heap fragmented).
        // Assumes applied records are noise-lane; the next regular refresh fixes
        // any inaccuracy precisely.
        if (applied > 0) {
            cs.pendingNoiseItems = (cs.pendingNoiseItems >= applied)
                                   ? cs.pendingNoiseItems - applied : 0;
            cs.pendingItems      = cs.pendingMissionItems + cs.pendingNoiseItems;
        }
        cs.lastPendingRefreshMs  = millis();
        cs.phoneState = COMPANION_PHONE_AVAILABLE;
        cs.lastSeenMs = millis();
        cs.manualProbeRequested  = false;
        cs.manualEnrichRequested = false;
        cs.offloadPrepRequested  = false;
        cs.timeSyncRequested     = false;
        DLOG_INFO("BLE", "Phone enrichment finished successfully");
    } else {
        // Pending counts are unchanged on failure; the next probe will refresh.
        cs.phoneState = COMPANION_PHONE_UNAVAILABLE;
        DLOG_WARN("BLE", "Phone enrichment failed");
    }

    if (RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
        RADIO_ARB.release(RADIO_BLE_GPS,
                          success ? "enrich_done" : "enrich_fail");
    }

    if (success) {
        crashBreadcrumbClear(CrashPhase::BACKLOG_ENRICH);
    }
}

// ── Enrichment pipeline queue helpers ────────────────────────────────────────

static void initEnrichQueue() {
    enrichQueueHead  = 0;
    enrichQueueTail  = 0;
    enrichQueueSize  = 0;
    enrichClaimedCount = 0;
    memset(enrichClaimedEventIds, 0, sizeof(enrichClaimedEventIds));
    for (size_t i = 0; i < ENRICH_QUEUE_DEPTH; ++i) {
        enrichQueue[i].count    = 0;
        enrichQueue[i].queuedMs = 0;
    }
}

// Claims are added AFTER a valid BLE response is received and enqueued,
// not at request-send time.  This ensures a timeout leaves zero stuck state.
static void enrichClaimReceived(const PendingEnrichment* recs, size_t count) {
    for (size_t i = 0; i < count && enrichClaimedCount < ENRICH_CLAIM_MAX; ++i) {
        if (recs[i].eventId != 0) {
            enrichClaimedEventIds[enrichClaimedCount++] = recs[i].eventId;
        }
    }
}

static void enrichClearAllClaims() {
    enrichClaimedCount = 0;
    memset(enrichClaimedEventIds, 0, sizeof(enrichClaimedEventIds));
}

static void enrichRemoveClaims(const PendingEnrichment* records, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const uint32_t eid = records[i].eventId;
        if (eid == 0) continue;
        for (size_t j = 0; j < enrichClaimedCount; ++j) {
            if (enrichClaimedEventIds[j] == eid) {
                enrichClaimedEventIds[j] =
                    enrichClaimedEventIds[--enrichClaimedCount];
                enrichClaimedEventIds[enrichClaimedCount] = 0;
                break;
            }
        }
    }
}

static bool enrichQueueHasRoom() {
    return enrichQueueSize < ENRICH_QUEUE_DEPTH;
}

static bool enqueueEnrichBatch(const PendingEnrichment* records, size_t count) {
    if (enrichQueueSize >= ENRICH_QUEUE_DEPTH || count == 0) return false;
    const size_t n = std::min(count, PHONE_ENRICH_BATCH_MAX);
    QueuedEnrichBatch& slot = enrichQueue[enrichQueueTail];
    memcpy(slot.records, records, n * sizeof(PendingEnrichment));
    slot.count    = n;
    slot.queuedMs = millis();
    enrichQueueTail = (enrichQueueTail + 1) % ENRICH_QUEUE_DEPTH;
    enrichQueueSize++;
    DLOG_INFO("BLE", "Enrich batch queued count=%u queueSize=%u",
              static_cast<unsigned>(n),
              static_cast<unsigned>(enrichQueueSize));
    return true;
}

static void drainOneEnrichBatch(CompanionScheduler& cs) {
    if (enrichQueueSize == 0) return;
    QueuedEnrichBatch& oldest = enrichQueue[enrichQueueHead];
    uint32_t batchApplied = 0, batchFailed = 0, batchStorageMs = 0;
    DLOG_INFO("BLE", "Enrich drain begin count=%u queueSize=%u ageMs=%lu",
              static_cast<unsigned>(oldest.count),
              static_cast<unsigned>(enrichQueueSize),
              static_cast<unsigned long>(millis() - oldest.queuedMs));
    crashCheckpointVolatile(CrashPhase::STORAGE_APPEND,
                            static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                            STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U);
    _applyPhoneEnrichmentBatch(oldest.records, oldest.count,
                               batchApplied, batchFailed, batchStorageMs);
    crashBreadcrumbClearVolatile(CrashPhase::STORAGE_APPEND);
    enrichRemoveClaims(oldest.records, oldest.count);
    cs.enrichmentSessionApplied   += batchApplied;
    cs.enrichmentSessionFailed    += batchFailed;
    cs.enrichmentSessionStorageMs += batchStorageMs;
    oldest.count    = 0;
    oldest.queuedMs = 0;
    enrichQueueHead = (enrichQueueHead + 1) % ENRICH_QUEUE_DEPTH;
    enrichQueueSize--;
    DLOG_INFO("BLE", "Enrich drain done applied=%lu failed=%lu queueSize=%u",
              static_cast<unsigned long>(batchApplied),
              static_cast<unsigned long>(batchFailed),
              static_cast<unsigned>(enrichQueueSize));
}

static void serviceEnrichmentPipeline(CompanionScheduler& cs) {
    // Drain oldest queued batch first (storage I/O, independent of BLE state).
    // Track whether we drained this tick so we can skip the spool scan below —
    // back-to-back storage write + full spool read in the same tick fragments
    // the heap unnecessarily.
    bool justDrained = false;
    if (enrichQueueSize > 0) {
        drainOneEnrichBatch(cs);
        justDrained = true;
    }
    bool justReceivedBatch = false;

    if (BLE_MGR.isPhoneCompanionReady()) {
        cs.phoneState = COMPANION_PHONE_AVAILABLE;
        cs.lastSeenMs = millis();
        publishPhoneStorageSnapshotIfDue(cs);

        // Consume any in-flight BLE response.
        if (cs.enrichmentRequestIssued) {
            if (BLE_MGR.consumeEnrichmentFailure()) {
                DLOG_WARN("BLE", "BLE enrichment exchange failed");
                enrichClearAllClaims();
                _finishPhoneEnrichment(cs, false);
                return;
            }

            PendingEnrichment enrichments[PHONE_ENRICH_BATCH_MAX] = {};
            size_t outCount = 0;
            if (BLE_MGR.consumeEnrichmentBatch(enrichments,
                                               PHONE_ENRICH_BATCH_MAX,
                                               outCount)) {
                // Valid response received — enqueue and claim BEFORE clearing
                // requestIssued so that the next _buildPendingEnrichmentBatch
                // call excludes these IDs while they sit in the queue.
                if (enqueueEnrichBatch(enrichments, outCount)) {
                    enrichClaimReceived(enrichments, outCount);
                    cs.enrichmentSessionBatches++;
                    cs.enrichmentSessionXferMs += BLE_MGR.getLastEnrichmentTransferMs();
                    cs.enrichmentRequestIssued = false;
                    cs.lastRequestedEnrichmentCount = 0;
                    justReceivedBatch = true;
                    DLOG_INFO("BLE", "Enrichment batch received count=%u claimed=%u queued=%u",
                              static_cast<unsigned>(outCount),
                              static_cast<unsigned>(enrichClaimedCount),
                              static_cast<unsigned>(enrichQueueSize));
                } else {
                    DLOG_WARN("BLE", "Enrichment queue full; ending session count=%u queued=%u",
                              static_cast<unsigned>(outCount),
                              static_cast<unsigned>(enrichQueueSize));
                    enrichClearAllClaims();
                    _finishPhoneEnrichment(cs, false);
                    return;
                }
            }
        }

        // Issue next request if the queue has room and no request is in flight.
        // Skip if we just drained — separates the storage write from the spool
        // scan by one 10ms tick, giving the heap one breath between them.
        // Also skip immediately after receiving a response.  The queued batch
        // should be drained before another full pending-enrichment spool scan.
        if (!cs.enrichmentRequestIssued &&
            enrichQueueHasRoom() &&
            !justDrained &&
            !justReceivedBatch) {
            EventBatchRecord batch[PHONE_ENRICH_BATCH_MAX] = {};
            size_t batchCount = 0;

            if (!_buildPendingEnrichmentBatch(batch,
                                              PHONE_ENRICH_BATCH_MAX,
                                              batchCount)) {
                DLOG_WARN("BLE", "Failed to build enrichment batch");
                if (enrichQueueSize == 0) {
                    enrichClearAllClaims();
                    _finishPhoneEnrichment(cs, false);
                }
            } else if (batchCount == 0) {
                // No unclaimed pending events; finish when queue fully drained.
                if (enrichQueueSize == 0) {
                    enrichClearAllClaims();
                    _finishPhoneEnrichment(cs, true);
                }
            } else if (BLE_MGR.requestEnrichmentBatch(batch, batchCount)) {
                cs.enrichmentRequestIssued = true;
                cs.lastRequestedEnrichmentCount = batchCount;
                cs.enrichmentSessionRequested +=
                    static_cast<uint32_t>(batchCount);
                DLOG_INFO("BLE",
                          "Requested enrichment batch count=%u queued=%u",
                          static_cast<unsigned>(batchCount),
                          static_cast<unsigned>(enrichQueueSize));
            } else {
                DLOG_WARN("BLE", "BLE requestEnrichmentBatch failed");
                if (enrichQueueSize == 0) {
                    enrichClearAllClaims();
                    _finishPhoneEnrichment(cs, false);
                }
            }
        }
    } else if (!RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
        enrichClearAllClaims();
        cs.phoneState = COMPANION_PHONE_UNAVAILABLE;
        cs.workState  = COMPANION_WORK_IDLE;
        cs.enrichmentRequestIssued = false;
        cs.lastRequestedEnrichmentCount = 0;
        crashBreadcrumbClear(CrashPhase::BACKLOG_ENRICH);
        DLOG_WARN("BLE", "Enrichment lease ended without ready companion");
    }
}


uint32_t _debugAreaMaskForToken(const String& token) {
    if (token == "all") return DEBUG_AREA_ALL;
    if (token == "ops" || token == "operators") return DEBUG_AREA_OPERATORS;
    if (token == "core" || token == "sys") return DEBUG_AREA_CORE;
    if (token == "settings") return DEBUG_AREA_SETTINGS;
    if (token == "storage" || token == "stor") return DEBUG_AREA_STORAGE;
    if (token == "time") return DEBUG_AREA_TIME;
    if (token == "radio" || token == "lora") return DEBUG_AREA_RADIO;
    if (token == "wifi") return DEBUG_AREA_WIFI;
    if (token == "ble") return DEBUG_AREA_BLE;
    if (token == "mqtt") return DEBUG_AREA_MQTT;
    if (token == "export") return DEBUG_AREA_EXPORT;
    if (token == "gps") return DEBUG_AREA_GPS;
    if (token == "mode") return DEBUG_AREA_MODE;
    return 0;
}

void _printUsbConsoleHelp() {
    Serial.println("[USB] Commands:");
    Serial.println("[USB]   debug on | debug off");
    Serial.println("[USB]   debug status | debug dump");
    Serial.println("[USB]   debug level d|i|w|e");
    Serial.println("[USB]   debug focus ops|all|radio,wifi,mqtt");
    Serial.println("[USB]   debug focus add <areas> | remove <areas>");
    Serial.println("[USB]   debug focus +wifi -ble (mixed +/-)");
    Serial.println("[USB]   debug focus list");
    Serial.println("[USB]   companion status");
    Serial.println("[USB]   companion probe   (one-shot manual BLE probe)");
    Serial.println("[USB]   companion enrich  (manual enrichment; probes first if needed)");
    Serial.println("[USB]   companion cancel  (clear all pending companion requests)");
    Serial.println("[USB]   spool audit       (read-only spool scan, prints mismatches)");
    Serial.println("[USB]   spool repair      (audit + repair counters, quarantine bad segs)");
    Serial.println("[USB]   spool diag        (segment list, counters, flags)");
    Serial.println("[USB]   spool quarantine list  (list /spool_bad files)");
    Serial.println("[USB]   spool quarantine meta  (print quarantine JSON records)");
    Serial.println("[USB]   spool quarantine clear (delete all quarantine files)");
    Serial.println("[USB]   fieldvault dump    (print FieldVault records to serial)");
    Serial.println("[USB]   fieldvault upload  (upload pending FieldVault records only)");
#if BLE_SMOKE_ENABLED
    Serial.println("[USB]   ble smoke         (suspend WiFi, init/deinit NimBLE, resume promisc)");
#endif
}

void _printUsbDebugFocusList(uint32_t mask) {
    if (mask == DEBUG_AREA_ALL) {
        Serial.println("[USB] debug focus=all");
        return;
    }
    if (mask == 0) {
        Serial.println("[USB] debug focus=none");
        return;
    }
    if (mask == DEBUG_AREA_OPERATORS) {
        Serial.println("[USB] debug focus=ops");
        return;
    }

    struct AreaLabel {
        uint32_t mask;
        const char* label;
    };
    static const AreaLabel kAreas[] = {
        { DEBUG_AREA_GENERAL, "general" },
        { DEBUG_AREA_CORE, "core" },
        { DEBUG_AREA_SETTINGS, "settings" },
        { DEBUG_AREA_STORAGE, "storage" },
        { DEBUG_AREA_TIME, "time" },
        { DEBUG_AREA_RADIO, "radio" },
        { DEBUG_AREA_WIFI, "wifi" },
        { DEBUG_AREA_BLE, "ble" },
        { DEBUG_AREA_MQTT, "mqtt" },
        { DEBUG_AREA_EXPORT, "export" },
        { DEBUG_AREA_GPS, "gps" },
        { DEBUG_AREA_MODE, "mode" }
    };

    Serial.print("[USB] debug focus=");
    bool first = true;
    for (const auto& area : kAreas) {
        if ((mask & area.mask) != 0) {
            if (!first) {
                Serial.print(",");
            }
            Serial.print(area.label);
            first = false;
        }
    }
    Serial.print("\r\n");
}

void _printUsbDebugStatus() {
    if (!SETTINGS.isReady()) {
        Serial.println("[USB] settings unavailable");
        return;
    }

    const RuntimeSettings& settings = SETTINGS.get();
    Serial.printf("[USB] debug enabled=%d level=%c mask=0x%08lx\r\n",
                  settings.usbSerialDebugEnabled ? 1 : 0,
                  settings.usbSerialDebugLevel,
                  static_cast<unsigned long>(settings.usbSerialDebugAreas));
    _printUsbDebugFocusList(settings.usbSerialDebugAreas);
}

void _handleUsbConsoleLine(const char* rawLine) {
    String line = rawLine ? rawLine : "";
    line.trim();
    if (line.length() == 0) {
        return;
    }

    String lower = line;
    lower.toLowerCase();

    if (lower == "help" || lower == "debug help") {
        _printUsbConsoleHelp();
        return;
    }

    if (lower == "debug status") {
        _printUsbDebugStatus();
        return;
    }

    if (lower == "debug dump") {
        DebugLog::dumpToSerial();
        return;
    }

    if (lower == "debug on" || lower == "debug off") {
        if (!SETTINGS.isReady()) {
            Serial.println("[USB] settings unavailable");
            return;
        }

        const bool enabled = (lower == "debug on");
        if (!SETTINGS.setUsbSerialDebugEnabled(enabled)) {
            Serial.println("[USB] failed to update debug enable");
            return;
        }
        Serial.printf("[USB] debug %s\r\n", enabled ? "enabled" : "disabled");
        _printUsbDebugStatus();
        return;
    }

    if (lower == "debug verbose") {
        lower = "debug level d";
    } else if (lower == "debug quiet") {
        lower = "debug level w";
    }

    if (lower.startsWith("debug level ")) {
        if (!SETTINGS.isReady()) {
            Serial.println("[USB] settings unavailable");
            return;
        }

        String levelToken = lower.substring(strlen("debug level "));
        levelToken.trim();
        char level = DEBUG_LEVEL_INFO;
        if (levelToken == "d" || levelToken == "debug" || levelToken == "verbose") {
            level = DEBUG_LEVEL_VERBOSE;
        } else if (levelToken == "i" || levelToken == "info") {
            level = DEBUG_LEVEL_INFO;
        } else if (levelToken == "w" || levelToken == "warn" || levelToken == "warning") {
            level = DEBUG_LEVEL_WARN;
        } else if (levelToken == "e" || levelToken == "err" || levelToken == "error") {
            level = DEBUG_LEVEL_ERROR;
        } else {
            Serial.printf("[USB] unknown debug level: %s\r\n", levelToken.c_str());
            return;
        }

        if (!SETTINGS.setUsbSerialDebugLevel(level)) {
            Serial.println("[USB] failed to update debug level");
            return;
        }
        Serial.printf("[USB] debug level set to %c\r\n", level);
        _printUsbDebugStatus();
        return;
    }

    if (lower == "debug focus list") {
        if (!SETTINGS.isReady()) {
            Serial.println("[USB] settings unavailable");
            return;
        }
        _printUsbDebugFocusList(SETTINGS.get().usbSerialDebugAreas);
        return;
    }

    if (lower.startsWith("debug focus ")) {
        if (!SETTINGS.isReady()) {
            Serial.println("[USB] settings unavailable");
            return;
        }

        String args = lower.substring(strlen("debug focus "));
        args.trim();
        if (args.length() == 0) {
            Serial.println("[USB] debug focus requires at least one area");
            return;
        }

        enum FocusMode : uint8_t { FOCUS_SET = 0, FOCUS_ADD, FOCUS_REMOVE };
        FocusMode mode = FOCUS_SET;
        if (args.startsWith("add ")) {
            mode = FOCUS_ADD;
            args = args.substring(4);
        } else if (args.startsWith("on ")) {
            mode = FOCUS_ADD;
            args = args.substring(3);
        } else if (args.startsWith("remove ")) {
            mode = FOCUS_REMOVE;
            args = args.substring(7);
        } else if (args.startsWith("off ")) {
            mode = FOCUS_REMOVE;
            args = args.substring(4);
        } else if (args.startsWith("only ")) {
            mode = FOCUS_SET;
            args = args.substring(5);
        }

        args.trim();
        args.replace(",", " ");

        const RuntimeSettings& settings = SETTINGS.get();
        uint32_t mask = (mode == FOCUS_SET) ? 0 : settings.usbSerialDebugAreas;
        while (args.length() > 0) {
            int split = args.indexOf(' ');
            String token = (split >= 0) ? args.substring(0, split) : args;
            token.trim();
            if (token.length() > 0) {
                FocusMode tokenMode = mode;
                if (token[0] == '+' || token[0] == '-') {
                    tokenMode = (token[0] == '+') ? FOCUS_ADD : FOCUS_REMOVE;
                    token = token.substring(1);
                    token.trim();
                }
                const uint32_t tokenMask = _debugAreaMaskForToken(token);
                if (tokenMask == 0) {
                    Serial.printf("[USB] unknown debug focus: %s\r\n", token.c_str());
                    return;
                }
                if (tokenMask == DEBUG_AREA_ALL) {
                    if (tokenMode == FOCUS_REMOVE) {
                        mask = 0;
                    } else {
                        mask = tokenMask;
                        break;
                    }
                } else if (tokenMode == FOCUS_REMOVE) {
                    mask &= ~tokenMask;
                } else {
                    mask |= tokenMask;
                }
            }

            if (split < 0) {
                break;
            }

            args = args.substring(split + 1);
            args.trim();
        }

        if (mask == 0) {
            Serial.println("[USB] debug focus requires at least one area");
            return;
        }

        if (!SETTINGS.setUsbSerialDebugAreas(mask)) {
            Serial.println("[USB] failed to update debug focus");
            return;
        }
        Serial.printf("[USB] debug focus mask set to 0x%08lx\r\n",
                      static_cast<unsigned long>(mask));
        _printUsbDebugStatus();
        return;
    }

    // ── Companion commands ────────────────────────────────────────────────
    if (lower == "companion status") {
        const CompanionStatusSnapshot s = g_companionStatus;   // snapshot copy
        const uint32_t now = millis();

        // BLE fields read live from BLE_MGR (the TaskHardware snapshot writer is
        // not yet wired, so going live avoids always showing defaults).
        const bool     bleBegun    = BLE_MGR.isBegun();
        const bool     bleRadio    = BLE_MGR.isRadioEnabled();
        const auto     bleState    = BLE_MGR.getState();
        const bool     linkReady   = BLE_MGR.isPhoneLinkReady();
        const bool     gpsReady    = BLE_MGR.isPhoneGpsReady();
        const bool     ctrlReady   = BLE_MGR.isPhoneControlReady();
        const bool     enrichReady = BLE_MGR.isPhoneEnrichmentReady();
        const bool     freshGps    = BLE_MGR.hasFreshGpsFix();
        const uint32_t lastBeginMs     = BLE_MGR.getLastBeginMs();
        const uint32_t lastScanStartMs = BLE_MGR.getLastScanStartMs();
        const int      disconnReason   = BLE_MGR.getLastDisconnectReason();

        Serial.printf("[COMP] enabled=%d\r\n",
                      s.enabled ? 1 : 0);
        Serial.printf("[COMP] phone=%s work=%s\r\n",
                      companionPhoneStateName(s.phoneState),
                      companionWorkStateName(s.workState));
        Serial.printf("[COMP] bleBegun=%d bleRadio=%d bleState=%s\r\n",
                      bleBegun ? 1 : 0,
                      bleRadio ? 1 : 0,
                      bleLinkStateName(bleState));
        Serial.printf("[COMP] ready link=%d gps=%d freshGps=%d ctrl=%d enrich=%d\r\n",
                      linkReady ? 1 : 0,
                      gpsReady ? 1 : 0,
                      freshGps ? 1 : 0,
                      ctrlReady ? 1 : 0,
                      enrichReady ? 1 : 0);
        Serial.printf("[COMP] bleInitAgeMs=%lu scanStartAgeMs=%lu disconnReason=%d\r\n",
                      static_cast<unsigned long>(bleBegun && lastBeginMs     ? now - lastBeginMs     : 0),
                      static_cast<unsigned long>(bleBegun && lastScanStartMs ? now - lastScanStartMs : 0),
                      disconnReason);
        Serial.printf("[COMP] pendingEnrichMission=%lu pendingEnrichNoise=%lu pendingEnrichTotal=%lu\r\n",
                      static_cast<unsigned long>(s.pendingMission),
                      static_cast<unsigned long>(s.pendingNoise),
                      static_cast<unsigned long>(s.pendingTotal));
        Serial.printf("[COMP] lastProbeAgeMs=%lu lastEnrichAgeMs=%lu\r\n",
                      static_cast<unsigned long>(s.lastProbeAgeMs),
                      static_cast<unsigned long>(s.lastEnrichAgeMs));
        Serial.printf("[COMP] radioOwner=%s\r\n",
                      radioOwnerName(s.radioOwner));
        return;
    }

    if (lower == "companion probe") {
        if (!PHONE_COMPANION_ENABLED) {
            Serial.println("[COMP] companion not enabled (PHONE_COMPANION_ENABLED=false)");
            return;
        }
        g_companionCmd.probe = true;
        Serial.println("[COMP] manual probe queued");
        return;
    }

    if (lower == "companion enrich") {
        if (!PHONE_COMPANION_ENABLED) {
            Serial.println("[COMP] companion not enabled (PHONE_COMPANION_ENABLED=false)");
            return;
        }
        g_companionCmd.enrich = true;
        Serial.println("[COMP] manual enrich queued");
        return;
    }

    if (lower == "companion cancel") {
        g_companionCmd.cancel = true;
        Serial.println("[COMP] cancel queued");
        return;
    }


#if BLE_SMOKE_ENABLED
    // ── BLE smoke test ───────────────────────────────────────────────────────
    if (lower == "ble smoke") {
        Serial.println("[BLE_SMOKE] requested");

        WIFI_MGR.suspendRadio();
        delay(300);

        // ── Radio / heap state snapshot ─────────────────────────────────────
        wifi_mode_t wifiMode = WIFI_MODE_NULL;
        esp_err_t   wifiErr  = esp_wifi_get_mode(&wifiMode);
        Serial.printf("[BLE_SMOKE] wifi_mode=%d err=%s\r\n",
                      static_cast<int>(wifiMode), esp_err_to_name(wifiErr));

        Serial.printf("[BLE_SMOKE] internal free=%u largest=%u dma_free=%u\r\n",
                      heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      heap_caps_get_free_size(MALLOC_CAP_DMA));
        Serial.printf("[BLE_SMOKE] psram free=%u largest=%u\r\n",
                      heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        Serial.flush();

        // ── Per-region internal heap breakdown ──────────────────────────────
        // Prints every free and allocated block in internal RAM so we can see
        // exactly who is holding the space before BLE tries to claim it.
        Serial.println("[BLE_SMOKE] --- internal heap regions ---");
        Serial.flush();
        heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
        Serial.println("[BLE_SMOKE] --- end regions ---");
        Serial.flush();

        // ── NimBLE init ─────────────────────────────────────────────────────
        Serial.println("[BLE_SMOKE] before NimBLEDevice::init");
        Serial.flush();
        NimBLEDevice::init("SpectreSmoke");
        Serial.println("[BLE_SMOKE] after NimBLEDevice::init");
        Serial.flush();

        NimBLEDevice::setPower(ESP_PWR_LVL_P9);
        Serial.println("[BLE_SMOKE] after setPower");
        Serial.flush();

        NimBLEDevice::deinit(true);
        Serial.println("[BLE_SMOKE] after deinit");
        Serial.flush();

        // ── Post-deinit heap check ───────────────────────────────────────────
        Serial.printf("[BLE_SMOKE] post-deinit internal free=%u largest=%u\r\n",
                      heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

        WIFI_MGR.startPromiscuous();
        Serial.println("[BLE_SMOKE] done");
        return;
    }
#endif  // BLE_SMOKE_ENABLED

    // ── Spool maintenance commands ────────────────────────────────────────────
    if (lower == "spool audit") {
        STORAGE.spoolAuditToSerial(false);
        return;
    }

    if (lower == "spool repair") {
        STORAGE.spoolAuditToSerial(true);
        return;
    }

    if (lower == "spool diag") {
        STORAGE.spoolDiagToSerial();
        return;
    }

    if (lower == "spool quarantine list") {
        STORAGE.spoolQuarantineListToSerial();
        return;
    }

    if (lower == "spool quarantine meta") {
        STORAGE.spoolQuarantineMetaToSerial();
        return;
    }

    if (lower == "spool quarantine clear") {
        STORAGE.spoolQuarantineClear();
        return;
    }

    if (lower == "fieldvault dump") {
        FieldVault::dumpToSerial();
        return;
    }

    if (lower == "fieldvault upload") {
        if (!FieldVault::isReady()) {
            Serial.println("[FIELD] not ready");
            return;
        }
        if (!FieldVault::hasPending()) {
            Serial.println("[FIELD] no pending records");
            return;
        }
        if (MQTT_MGR.requestFieldVaultDump()) {
            Serial.println("[FIELD] upload queued");
        } else {
            Serial.println("[FIELD] upload unavailable");
        }
        return;
    }

    Serial.printf("[USB] unknown command: %s\r\n", line.c_str());
    _printUsbConsoleHelp();
}

void _pollUsbSerialConsole() {
    while (Serial.available() > 0) {
        const char ch = static_cast<char>(Serial.read());

        // Backspace / DEL — erase last character with a terminal-friendly sequence.
        if (ch == '\b' || ch == 0x7F) {
            if (g_usbConsoleLen > 0) {
                --g_usbConsoleLen;
                Serial.print("\b \b");  // move back, overwrite with space, move back again
            }
            continue;
        }

        // CR or LF both terminate the line; absorb a trailing LF after CR.
        if (ch == '\r' || ch == '\n') {
            // If the buffer is empty this might be the \n that follows a \r we
            // already processed — just swallow it silently.
            if (g_usbConsoleLen == 0) {
                continue;
            }
            Serial.println();  // move terminal to next line
            g_usbConsoleBuf[g_usbConsoleLen] = '\0';
            _handleUsbConsoleLine(g_usbConsoleBuf);
            g_usbConsoleLen = 0;
            continue;
        }

        // Printable character — echo and buffer it.
        if (ch >= 0x20 && ch < 0x7F) {
            if ((g_usbConsoleLen + 1) >= USB_CONSOLE_BUF_SIZE) {
                // Buffer full — ring the bell and discard.
                Serial.print('\a');
                continue;
            }
            g_usbConsoleBuf[g_usbConsoleLen++] = ch;
            Serial.print(ch);  // echo back to terminal
        }
    }
}

void _dispatchUiCommand(UICommand cmd) {
    if (cmd == UI_CMD_NONE) return;

    if (!BUS.publishUiCommand(static_cast<int32_t>(cmd))) {
        DLOG_WARN("UI", "Display queue full, dropped cmd=%d",
                  static_cast<int>(cmd));
    }
}

void _queueNotification(uint8_t type, const char* text) {
    if (!text || !text[0]) {
        return;
    }

    if (!BUS.publishNotification(type, text)) {
        DLOG_WARN("UI", "Notification queue full, dropped type=%u",
                  static_cast<unsigned>(type));
    }
}

bool _requestBleTextEntry(const char* leaseReason, const char* prompt) {
    if (!leaseReason || !prompt) {
        return false;
    }

    if (!RADIO_ARB.requestLease(RADIO_BLE_TEXT,
                                RadioArbiter::BLE_TEXT_ACTIVE_HOLD_MS,
                                leaseReason)) {
        return false;
    }

    if (BLE_MGR.requestTextInput(prompt)) {
        return true;
    }

    RADIO_ARB.release(RADIO_BLE_TEXT, "text input request failed");
    return false;
}

static bool _launchMissionProfile(MissionProfile profile) {
    const MissionProfile sanitized = _sanitizeMissionProfile(profile);

    if (sanitized == MISSION_PWNY) {
        if (!RADIO_ARB.requestPwnyLease(RadioArbiter::LEASE_INFINITE,
                                        "mission_pwny")) {
            DLOG_WARN("MISSION", "PWNY launch denied by radio arbiter");
            _queueNotification(NOTIF_DEVICE_NEW, "PWNY DENIED");
            return false;
        }
    }

    return enterMission(sanitized);
}

SpectreButtonAction _actionForEvent(ButtonEvent evt, const ButtonBindingSet& bindings) {
    switch (evt) {
        case BTN_A_SHORT: return bindings.aShort;
        case BTN_A_LONG:  return bindings.aLong;
        case BTN_B_LONG:  return bindings.bLong;
        case BTN_B_SHORT: return bindings.bShort;
        case BTN_AB_SHORT:return BUTTON_ACTION_NONE;
        default:          return BUTTON_ACTION_NONE;
    }
}

void _captureDisplayFrameState(DisplayFrameState& snapshot) {
    STATE_READ_BEGIN();
    snapshot.uploadActive = g_state.uploadActive;
    snapshot.radioBusy = g_state.radioBusy;
    snapshot.uploadPercent = g_state.uploadPercent;
    snapshot.uploadPublished = g_state.uploadPublished;
    snapshot.uploadTotal = g_state.uploadTotal;
    strlcpy(snapshot.uploadPhase, g_state.uploadPhase, sizeof(snapshot.uploadPhase));
    snapshot.currentScreen = g_state.currentScreen;
    snapshot.mascotState = g_state.mascotState;
    snapshot.requestSleep = g_state.requestSleep;
    snapshot.screenChanged = g_state.screenChanged;
    snapshot.dataRefresh = g_state.dataRefresh;
    snapshot.loraNewPacket = g_state.loraNewPacket;
    snapshot.textInputPending = g_state.textInputPending;
    snapshot.wifiListActive = g_state.wifiListActive;
    snapshot.missionListActive = g_state.missionListActive;
    snapshot.badUsbListActive = g_state.badUsbListActive;
    snapshot.debriefActive = g_state.debriefActive;
    snapshot.badUsbArmed = g_state.badUsbArmed;
    snapshot.badUsbRunning = g_state.badUsbRunning;
    snapshot.badUsbReady = g_state.badUsbReady;
    snapshot.battPercent = g_state.battPercent;
    snapshot.battVoltageMv = g_state.battVoltageMv;
    snapshot.battTrendMvPerMin = g_state.battTrendMvPerMin;
    snapshot.battCapacityMah = g_state.battCapacityMah;
    snapshot.battRuntimeMin = g_state.battRuntimeMin;
    snapshot.powerSource = g_state.powerSource;
    snapshot.powerState = g_state.powerState;
    snapshot.charging = g_state.charging;
    snapshot.criticalSinceMs = g_state.criticalSinceMs;
    snapshot.criticalSleepAtMs = g_state.criticalSleepAtMs;
    snapshot.wifiConnected = g_state.wifiConnected;
    snapshot.bleConnected = g_state.bleConnected;
    snapshot.loraReady = g_state.loraReady;
    snapshot.radioOwner = g_state.radioOwner;
    strlcpy(snapshot.loraLastPayload, g_state.loraLastPayload, sizeof(snapshot.loraLastPayload));
    snapshot.loraRSSI = g_state.loraRSSI;
    snapshot.loraSNR = g_state.loraSNR;
    snapshot.loraPacketCount = g_state.loraPacketCount;
    strlcpy(snapshot.subGhzModule, g_state.subGhzModule, sizeof(snapshot.subGhzModule));
    snapshot.subGhzMode = g_state.subGhzMode;
    snapshot.subGhzFrequencyHz = g_state.subGhzFrequencyHz;
    snapshot.subGhzNodeCount = g_state.subGhzNodeCount;
    strlcpy(snapshot.wifiSSID, g_state.wifiSSID, sizeof(snapshot.wifiSSID));
    snapshot.wifiNetworkCount = g_state.wifiNetworkCount;
    snapshot.probePacketCount = g_state.probePacketCount;
    strlcpy(snapshot.lastProbedMAC, g_state.lastProbedMAC, sizeof(snapshot.lastProbedMAC));
    snapshot.runContext = g_state.runContext;
    snapshot.activeMissionProfile = g_state.activeMissionProfile;
    snapshot.missionSelection = g_state.missionSelection;
    snapshot.battVoltage = g_state.battVoltage;
    snapshot.uptimeMs = g_state.uptimeMs;
    strlcpy(snapshot.storageStr, g_state.storageStr, sizeof(snapshot.storageStr));

    g_state.loraNewPacket = false;
    g_state.screenChanged = false;
    g_state.dataRefresh = false;
    STATE_READ_END();
}

ButtonRoutingState _readButtonRoutingState() {
    ButtonRoutingState state;
    STATE_READ_BEGIN();
    state.listActive = g_state.wifiListActive;
    state.missionListActive = g_state.missionListActive;
    state.badUsbListActive = g_state.badUsbListActive;
    state.debriefActive = g_state.debriefActive;
    state.badUsbArmed = g_state.badUsbArmed;
    state.badUsbRunning = g_state.badUsbRunning;
    state.badUsbReady = g_state.badUsbReady;
    state.runContext = g_state.runContext;
    state.activeMissionProfile = g_state.activeMissionProfile;
    state.missionSelection = g_state.missionSelection;
    state.currentScreen = g_state.currentScreen;
    STATE_READ_END();
    return state;
}

UiRefreshState _readUiRefreshState() {
    UiRefreshState state;
    STATE_READ_BEGIN();
    state.currentScreen = g_state.currentScreen;
    state.debriefActive = g_state.debriefActive;
    state.activeMissionProfile = g_state.activeMissionProfile;
    STATE_READ_END();
    return state;
}

bool _waitForDisplayLayerReady(uint32_t timeoutMs) {
    const uint32_t waitStart = millis();
    while (!s_displayLayerReady &&
           (millis() - waitStart) < timeoutMs) {
        vTaskDelay(10);
    }

    return s_displayLayerReady;
}

void _publishStorageState(bool storageOk, const String& storageUsed) {
    STATE_WRITE_BEGIN();
    g_state.storageReady = storageOk;
    if (storageOk) {
        storageUsed.toCharArray(g_state.storageStr, sizeof(g_state.storageStr));
    }
    STATE_WRITE_END();
}

void _loadKnownLocationsIntoState() {
    SpectreState::KnownLocation knownLocs[SpectreState::KNOWN_LOC_COUNT] = {};
    const int locCount = STORAGE.loadKnownLocations(
        knownLocs,
        SpectreState::KNOWN_LOC_COUNT);

    STATE_WRITE_BEGIN();
    memcpy(g_state.knownLocations, knownLocs, sizeof(knownLocs));
    g_state.knownLocCount = locCount;
    STATE_WRITE_END();

    DLOG_INFO("STOR", "Loaded %d known locations", locCount);
}

void _applySubGhzStatusToState(const SubGhzStatus& sg) {
    STATE_WRITE_BEGIN();
    g_state.loraReady = sg.ready;
    g_state.subGhzMode = static_cast<uint8_t>(sg.mode);
    g_state.subGhzNodeCount = static_cast<int>(sg.nodeCount);
    strlcpy(g_state.subGhzBackend, sg.backendName, sizeof(g_state.subGhzBackend));
    strlcpy(g_state.subGhzModule, sg.moduleName, sizeof(g_state.subGhzModule));
    g_state.subGhzFrequencyHz = sg.frequencyHz;
    STATE_WRITE_END();
}

void _applyPowerSnapshotToState(const PowerSnapshot& power) {
    STATE_WRITE_BEGIN();
    g_state.battPercent = power.percent;
    g_state.battVoltage = power.voltage;
    g_state.battVoltageMv = power.voltageMv;
    g_state.battTrendMvPerMin = power.trendMvPerMin;
    g_state.battCapacityMah = power.batteryCapacityMah;
    g_state.battRuntimeMin = power.runtimeRemainingMin;
    g_state.powerSource = static_cast<uint8_t>(power.source);
    g_state.powerState = static_cast<uint8_t>(power.state);
    g_state.charging = power.charging;
    g_state.criticalSinceMs = power.criticalSinceMs;
    g_state.criticalSleepAtMs = power.criticalSleepAtMs;
    STATE_WRITE_END();
}

inline void spectreBeginSleepTransitionLocked(uint32_t nowMs,
                                              uint32_t forceTimeoutMs) {
    g_state.requestSleep = true;
    g_state.sleepPresentationAcked = false;
    g_state.sleepRequestAtMs = nowMs;
    g_state.sleepPresentationAckMs = 0;
    g_state.sleepCommitAtMs = 0;
    g_state.sleepForceAtMs = nowMs + forceTimeoutMs;
    ++g_state.sleepRequestSeq;
}

inline void spectreAcknowledgeSleepPresentationLocked(uint32_t nowMs,
                                                      uint32_t holdMs) {
    if (!g_state.requestSleep) {
        return;
    }

    g_state.sleepPresentationAcked = true;
    g_state.sleepPresentationAckMs = nowMs;
    g_state.sleepCommitAtMs = nowMs + holdMs;
}

[[noreturn]] void _enterDeepSleepNow(const char* reason) {
    DLOG_INFO("POWER", "Entering deep sleep reason=%s",
              reason ? reason : "unknown");
    esp_deep_sleep_start();
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

void _serviceSleepTransition(uint32_t nowMs) {
    bool requestSleep = false;
    bool presentationAcked = false;
    uint32_t commitAtMs = 0;
    uint32_t forceAtMs = 0;

    STATE_READ_BEGIN();
    requestSleep = g_state.requestSleep;
    presentationAcked = g_state.sleepPresentationAcked;
    commitAtMs = g_state.sleepCommitAtMs;
    forceAtMs = g_state.sleepForceAtMs;
    STATE_READ_END();

    if (!requestSleep) {
        return;
    }

    if (forceAtMs != 0 && nowMs >= forceAtMs) {
        _enterDeepSleepNow(presentationAcked ? "display_timeout_cap"
                                             : "display_timeout");
    }

    if (presentationAcked && commitAtMs != 0 && nowMs >= commitAtMs) {
        _enterDeepSleepNow("display_presented");
    }
}

void _requestSleepTransition(bool storageOk,
                             bool finalizeSession,
                             bool exportSession,
                             const char* reason) {
    bool sleepAlreadyRequested = false;
    STATE_READ_BEGIN();
    sleepAlreadyRequested = g_state.requestSleep;
    STATE_READ_END();
    if (sleepAlreadyRequested) {
        return;
    }

    if (storageOk) {
        if (finalizeSession) {
            STORAGE.endSession();
        } else {
            STORAGE.checkpointSessionState();
        }

        if (exportSession) {
            _runSessionExport(false);
        }
    }

    STATE_WRITE_BEGIN();
    spectreBeginSleepTransitionLocked(millis(), SLEEP_PRESENTATION_TIMEOUT_MS);
    g_state.screenChanged = true;
    STATE_WRITE_END();

    DLOG_INFO("POWER",
              "Sleep requested reason=%s finalize=%d export=%d",
              reason ? reason : "unknown",
              finalizeSession ? 1 : 0,
              exportSession ? 1 : 0);
}

uint32_t _displayFrameIntervalForPowerState(uint8_t powerState) {
    switch (static_cast<PowerState>(powerState)) {
        case POWER_STATE_BATTERY_ECONOMY:  return 85UL;
        case POWER_STATE_BATTERY_CRITICAL: return 110UL;
        case POWER_STATE_USB:
        case POWER_STATE_BATTERY_NORMAL:
        default:                           return DISPLAY_FRAME_INTERVAL_MS;
    }
}

uint32_t _displayMascotIntervalForPowerState(uint8_t powerState) {
    switch (static_cast<PowerState>(powerState)) {
        case POWER_STATE_BATTERY_ECONOMY:  return 220UL;
        case POWER_STATE_BATTERY_CRITICAL: return 280UL;
        case POWER_STATE_USB:
        case POWER_STATE_BATTERY_NORMAL:
        default:                           return DISPLAY_MASCOT_INTERVAL_MS;
    }
}

uint8_t _displayBrightnessForPowerState(uint8_t powerState) {
    switch (static_cast<PowerState>(powerState)) {
        case POWER_STATE_BATTERY_ECONOMY:
        case POWER_STATE_BATTERY_CRITICAL:
            return 50;
        case POWER_STATE_USB:
        case POWER_STATE_BATTERY_NORMAL:
        default:
            return 100;
    }
}

ExecutionPolicy::UiRefreshSchedule _uiRefreshScheduleForPowerState(uint8_t powerState) {
    switch (static_cast<PowerState>(powerState)) {
        case POWER_STATE_BATTERY_ECONOMY:
            return {3000UL, 1250UL, 1750UL};
        case POWER_STATE_BATTERY_CRITICAL:
            return {4000UL, 1600UL, 2200UL};
        case POWER_STATE_USB:
        case POWER_STATE_BATTERY_NORMAL:
        default:
            return {2000UL, PWNY_SCREEN_REFRESH_MS, 1000UL};
    }
}

void _initializeHardwareManagers(uint32_t& lastWifiTick) {
    MQTT_MGR.begin();
    BADUSB_MGR.begin();

    SUBGHZ.attachBackend(&subghzReyax);
    const bool subGhzOk = SUBGHZ.begin();
    if (subGhzOk) {
        SUBGHZ.setMode(SubGhzMode::MONITOR);
    }

    const SubGhzStatus sg = SUBGHZ.status();
    _applySubGhzStatusToState(sg);

    if (subGhzOk) {
        DLOG_INFO("SUBGHZ", "Ready backend=%s fw=%s mode=%s",
                  sg.backendName,
                  sg.firmware,
                  subGhzModeName(sg.mode));
    } else {
        DLOG_ERROR("SUBGHZ", "Init failed");
    }

    SESS.begin();
    WIFI_MGR.begin();
    RADIO_ARB.begin();

    STATE_WRITE_BEGIN();
    SESS.getId().toCharArray(g_state.sessionId, sizeof(g_state.sessionId));
    g_state.runContext = RUN_CONTEXT_GENERAL;
    g_state.activeMissionProfile = MISSION_RECON;
    g_state.missionSelection = MISSION_RECON;
    g_state.generalScreen = static_cast<uint8_t>(g_state.currentScreen);
    STATE_WRITE_END();
    syncRuntimePresentation();

    // Write FieldVault records now that sessionId is populated. Heavy file
    // work intentionally happens here (not in setup() or in a crash/panic
    // context) so it cannot interfere with hardware bring-up timing.
    //
    // Order matters for human readability of field.jsonl: a crash from the
    // prior boot (if any) is written first, then this boot's summary, so
    // each boot's narrative reads "prior crash → new boot".
    if (FieldVault::isReady()) {
        const esp_reset_reason_t rr = esp_reset_reason();
        const uint32_t pending =
            STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0;
        const uint32_t heapKb =
            static_cast<uint32_t>(ESP.getMinFreeHeap() / 1024);
        const PowerSnapshot power = POWER_MGR.snapshot();
        const bool usbSerialAttached = g_usbSerialAttachedAtBoot;
        char isoBuf[24] = "";
        TIME_SVC.formatNowIso(isoBuf, sizeof(isoBuf));
        char sidBuf[40];
        STATE_READ_BEGIN();
        strlcpy(sidBuf, g_state.sessionId, sizeof(sidBuf));
        STATE_READ_END();

        FieldVault::appendSeriallessResetCrashIfNeeded(
            static_cast<uint8_t>(rr),
            _resetReasonName(rr),
            sidBuf,
            isoBuf,
            _fieldVaultPowerSourceName(power.source),
            heapKb,
            pending,
            usbSerialAttached);
        FieldVault::vaultUnresolvedCrashIfNew(static_cast<uint8_t>(rr),
                                              _resetReasonName(rr),
                                              sidBuf,
                                              isoBuf,
                                              usbSerialAttached);
        FieldVault::appendBoot(static_cast<uint8_t>(rr),
                               _resetReasonName(rr),
                               heapKb,
                               pending,
                               sidBuf,
                               isoBuf,
                               usbSerialAttached);
    }

    DLOG_INFO("WIFI", "Allocation: %s",
              WIFI_MGR.isAllocated() ? "OK" : "FAIL");
    lastWifiTick = millis();
    RADIO_ARB.ensureDefaultCapture("boot");
}

void _publishHardwareReadyState() {
    STATE_WRITE_BEGIN();
    g_state.hwInitDone = true;
    g_state.screenChanged = true;
    STATE_WRITE_END();
}

void _publishUiDataRefresh() {
    STATE_WRITE_BEGIN();
    g_state.dataRefresh = true;
    STATE_WRITE_END();
}

void _servicePeriodicUiRefresh(const UiRefreshState& uiRefresh,
                               uint32_t nowMs,
                               ExecutionPolicy::UiRefreshMarks& marks,
                               const ExecutionPolicy::UiRefreshSchedule& schedule) {
    const ExecutionPolicy::UiRefreshReason reason =
        ExecutionPolicy::dueUiRefresh(uiRefresh.currentScreen,
                                      uiRefresh.debriefActive,
                                      _sanitizeMissionProfile(uiRefresh.activeMissionProfile),
                                      nowMs,
                                      marks,
                                      schedule);
    if (reason == ExecutionPolicy::UI_REFRESH_NONE) {
        return;
    }

    _publishUiDataRefresh();
    ExecutionPolicy::markUiRefresh(reason, nowMs, marks);
}

void _refreshDisplayStatusBar(const DisplayFrameState& snapshot,
                              bool& statusValid,
                              ExecutionPolicy::StatusBarStateView& lastStatus) {
    ExecutionPolicy::StatusBarStateView nextStatus;
    nextStatus.battPercent = snapshot.battPercent;
    nextStatus.runtimeMinutes = snapshot.battRuntimeMin;
    nextStatus.powerSource = snapshot.powerSource;
    nextStatus.powerState = snapshot.powerState;
    nextStatus.charging = snapshot.charging;
    nextStatus.wifiConnected = snapshot.wifiConnected;
    nextStatus.bleConnected = snapshot.bleConnected;
    nextStatus.loraActive = snapshot.loraReady;
    nextStatus.radioOwner = snapshot.radioOwner;

    if (!statusValid ||
        snapshot.powerState == POWER_STATE_BATTERY_CRITICAL ||
        ExecutionPolicy::statusBarChanged(lastStatus, nextStatus)) {
        StatusBar sb;
        sb.battPercent = nextStatus.battPercent;
        sb.runtimeMinutes = nextStatus.runtimeMinutes;
        sb.powerSource = nextStatus.powerSource;
        sb.powerState = nextStatus.powerState;
        sb.charging = nextStatus.charging;
        sb.wifiConnected = nextStatus.wifiConnected;
        sb.bleConnected = nextStatus.bleConnected;
        sb.loraActive = nextStatus.loraActive;
        sb.radioOwner = nextStatus.radioOwner;
        display.updateStatus(sb);
        lastStatus = nextStatus;
        statusValid = true;
    }
}

bool _textChanged(const char* a, const char* b) {
    return strncmp(a ? a : "", b ? b : "", 64) != 0;
}

bool _buttonBindingsChanged(const ButtonBindingSet& a, const ButtonBindingSet& b) {
    return a.aShort != b.aShort ||
           a.aLong  != b.aLong ||
           a.bLong  != b.bLong ||
           a.bShort != b.bShort;
}

bool _visibleDisplayDataChanged(const DisplayFrameState& previous,
                                const DisplayFrameState& next) {
    if (previous.currentScreen != next.currentScreen ||
        previous.wifiListActive != next.wifiListActive ||
        previous.missionListActive != next.missionListActive ||
        previous.badUsbListActive != next.badUsbListActive ||
        previous.debriefActive != next.debriefActive) {
        return true;
    }

    switch (next.currentScreen) {
        case SCREEN_LORA:
            return previous.subGhzMode != next.subGhzMode ||
                   previous.subGhzFrequencyHz != next.subGhzFrequencyHz ||
                   previous.subGhzNodeCount != next.subGhzNodeCount ||
                   previous.loraRSSI != next.loraRSSI ||
                   previous.loraSNR != next.loraSNR ||
                   previous.loraPacketCount != next.loraPacketCount ||
                   _textChanged(previous.subGhzModule, next.subGhzModule) ||
                   _textChanged(previous.loraLastPayload, next.loraLastPayload);

        case SCREEN_WIFI:
            return previous.wifiNetworkCount != next.wifiNetworkCount ||
                   previous.probePacketCount != next.probePacketCount ||
                   _textChanged(previous.wifiSSID, next.wifiSSID) ||
                   _textChanged(previous.lastProbedMAC, next.lastProbedMAC);

        case SCREEN_RECON:
            return previous.missionSelection != next.missionSelection;

        case SCREEN_MISSION:
            // Pwny/mission detail rows are read by DisplayManager from g_state.
            return previous.activeMissionProfile != next.activeMissionProfile ||
                   next.dataRefresh;

        case SCREEN_BADUSB:
            // BadUSB script/status fields live in g_state, not this snapshot.
            return next.dataRefresh ||
                   previous.badUsbReady != next.badUsbReady ||
                   previous.badUsbArmed != next.badUsbArmed ||
                   previous.badUsbRunning != next.badUsbRunning;

        case SCREEN_SYSTEM:
            return previous.battVoltage != next.battVoltage ||
                   previous.uptimeMs / 1000UL != next.uptimeMs / 1000UL ||
                   _textChanged(previous.storageStr, next.storageStr);

        case SCREEN_MESHTASTIC:
            return false;

        default:
            return true;
    }
}

void _checkRuntimeContracts() {
    const RadioOwner owner = RADIO_ARB.currentOwner();
    const MQTTState mqttState = MQTT_MGR.getState();

    CONTRACT_WARN_ONCE(CONTRACT_PWNY_OWNER_SYNC,
                       "CORE",
                       !WIFI_MGR.isPwnyActive() || owner == RADIO_WIFI_PMKID,
                       "wifi_mode=%d owner=%s",
                       static_cast<int>(WIFI_MGR.getMode()),
                       RadioArbiter::ownerName(owner));

    CONTRACT_WARN_ONCE(CONTRACT_UPLOAD_OWNER_SYNC,
                       "CORE",
                       mqttState == MQTT_IDLE || owner == RADIO_WIFI_UPLOAD,
                       "mqtt_state=%d owner=%s",
                       static_cast<int>(mqttState),
                       RadioArbiter::ownerName(owner));

    CONTRACT_WARN_ONCE(CONTRACT_UPLOAD_BATCH_OWNER_SYNC,
                       "CORE",
                       !STORAGE.isUploadBatchActive() || owner == RADIO_WIFI_UPLOAD,
                       "batch active with owner=%s mqtt_state=%d",
                       RadioArbiter::ownerName(owner),
                       static_cast<int>(mqttState));
}

const char* _buttonEventName(ButtonEvent evt) {
    switch (evt) {
        case BTN_A_SHORT: return "A_SHORT";
        case BTN_A_LONG:  return "A_LONG";
        case BTN_B_SHORT: return "B_SHORT";
        case BTN_B_LONG:  return "B_LONG";
        case BTN_AB_SHORT:return "AB_SHORT";
        case BTN_NONE:
        default:          return "NONE";
    }
}

const char* _buttonActionName(SpectreButtonAction action) {
    switch (action) {
        case BUTTON_ACTION_NONE:             return "NONE";
        case BUTTON_ACTION_SCREEN_NEXT:      return "SCREEN_NEXT";
        case BUTTON_ACTION_SUBGHZ_MODE_CYCLE:return "SUBGHZ_MODE_CYCLE";
       case BUTTON_ACTION_LORA_PING:
            return "LORA_PING";
        case BUTTON_ACTION_SLEEP:            return "SLEEP";
        case BUTTON_ACTION_WIFI_REFRESH:     return "WIFI_REFRESH";
        case BUTTON_ACTION_WIFI_SCAN_LIST:   return "WIFI_SCAN_LIST";
        case BUTTON_ACTION_WIFI_ALLSCAN:     return "WIFI_ALLSCAN";
        case BUTTON_ACTION_WIFI_LIST_SELECT: return "WIFI_LIST_SELECT";
        case BUTTON_ACTION_WIFI_LIST_DOWN:   return "WIFI_LIST_DOWN";
        case BUTTON_ACTION_WIFI_LIST_CLOSE:  return "WIFI_LIST_CLOSE";
        case BUTTON_ACTION_WIFI_LIST_HUNT:   return "WIFI_LIST_HUNT";
        case BUTTON_ACTION_ANTENNA_TOGGLE:   return "ANTENNA_TOGGLE";
        case BUTTON_ACTION_SYSTEM_DEBRIEF:   return "SYSTEM_DEBRIEF";
        case BUTTON_ACTION_SESSION_TAG:      return "SESSION_TAG";
        case BUTTON_ACTION_SAVE_LOCATION:    return "SAVE_LOCATION";
        case BUTTON_ACTION_MISSION_NEXT:     return "MISSION_NEXT";
        case BUTTON_ACTION_MISSION_ENTER:    return "MISSION_ENTER";
        case BUTTON_ACTION_MISSION_EXIT:     return "MISSION_EXIT";
        case BUTTON_ACTION_MISSION_LIST_OPEN:return "MISSION_LIST_OPEN";
        case BUTTON_ACTION_MISSION_LIST_SELECT:return "MISSION_LIST_SELECT";
        case BUTTON_ACTION_MISSION_LIST_DOWN:return "MISSION_LIST_DOWN";
        case BUTTON_ACTION_MISSION_LIST_CLOSE:return "MISSION_LIST_CLOSE";
        case BUTTON_ACTION_UPLINK_TRIGGER:   return "UPLINK_TRIGGER";
        case BUTTON_ACTION_BADUSB_LIST_OPEN:   return "BADUSB_LIST_OPEN";
        case BUTTON_ACTION_BADUSB_LIST_SELECT: return "BADUSB_LIST_SELECT";
        case BUTTON_ACTION_BADUSB_LIST_DOWN:   return "BADUSB_LIST_DOWN";
        case BUTTON_ACTION_BADUSB_LIST_CLOSE:  return "BADUSB_LIST_CLOSE";
        case BUTTON_ACTION_BADUSB_ARM:         return "BADUSB_ARM";
        case BUTTON_ACTION_BADUSB_RUN:         return "BADUSB_RUN";
        case BUTTON_ACTION_BADUSB_CANCEL:      return "BADUSB_CANCEL";
        case BUTTON_ACTION_PWNY_FORCE_DEAUTH:  return "PWNY_FORCE_DEAUTH";
        case BUTTON_ACTION_DEBRIEF_EXPORT:   return "DEBRIEF_EXPORT";
        case BUTTON_ACTION_DEBRIEF_CLEAR:    return "DEBRIEF_CLEAR";
        case BUTTON_ACTION_DEBRIEF_BACK:     return "DEBRIEF_BACK";
        case BUTTON_ACTION_BLE_TEST:          return "BLE_TEST";
        default:                             return "UNKNOWN";
    }
}

static MissionProfile _sanitizeMissionProfile(uint8_t rawProfile) {
    if (rawProfile >= static_cast<uint8_t>(MISSION_PROFILE_COUNT)) {
        return MISSION_RECON;
    }
    return static_cast<MissionProfile>(rawProfile);
}

static Screen _nextGeneralScreen(Screen screen) {
    switch (screen) {
        case SCREEN_LORA:       return SCREEN_MESHTASTIC;
        case SCREEN_MESHTASTIC: return SCREEN_WIFI;
        case SCREEN_WIFI:       return SCREEN_BADUSB;
        case SCREEN_BADUSB:     return SCREEN_RECON;
        case SCREEN_RECON:      return SCREEN_SYSTEM;
        case SCREEN_SYSTEM:
        case SCREEN_MISSION:
        default:                return SCREEN_LORA;
    }
}

static bool _routeUsesBadUsb(const ButtonRoutingState& route) {
    return route.currentScreen == SCREEN_BADUSB;
}

static ButtonBindingSet _missionBindingsForRoute(const ButtonRoutingState& route) {
    switch (_sanitizeMissionProfile(route.activeMissionProfile)) {
        case MISSION_RECON:
            return {BUTTON_ACTION_WIFI_REFRESH, BUTTON_ACTION_MISSION_EXIT,
                    BUTTON_ACTION_SUBGHZ_MODE_CYCLE, BUTTON_ACTION_WIFI_SCAN_LIST};
        case MISSION_PWNY:
            if (!WIFI_MGR.isPwnyActive()) {
                return {BUTTON_ACTION_NONE, BUTTON_ACTION_MISSION_EXIT,
                        BUTTON_ACTION_NONE, BUTTON_ACTION_NONE};
            }
            return {BUTTON_ACTION_PWNY_FORCE_DEAUTH, BUTTON_ACTION_MISSION_EXIT,
                    BUTTON_ACTION_NONE, BUTTON_ACTION_NONE};
        case MISSION_UPLINK:
            return {BUTTON_ACTION_UPLINK_TRIGGER, BUTTON_ACTION_MISSION_EXIT,
                    BUTTON_ACTION_SYSTEM_DEBRIEF, BUTTON_ACTION_NONE};
        default:
            return {BUTTON_ACTION_NONE, BUTTON_ACTION_MISSION_EXIT,
                    BUTTON_ACTION_NONE, BUTTON_ACTION_NONE};
    }
}

static ButtonBindingSet _badUsbBindingsForRoute(const ButtonRoutingState& route) {
    if (!route.badUsbReady) {
        return {BUTTON_ACTION_NONE, BUTTON_ACTION_NONE,
                BUTTON_ACTION_BADUSB_LIST_OPEN, BUTTON_ACTION_SCREEN_NEXT};
    }

    if (route.badUsbRunning) {
        return {BUTTON_ACTION_NONE, BUTTON_ACTION_BADUSB_CANCEL,
                BUTTON_ACTION_BADUSB_LIST_OPEN, BUTTON_ACTION_SCREEN_NEXT};
    }

    if (route.badUsbArmed) {
        return {BUTTON_ACTION_NONE, BUTTON_ACTION_BADUSB_RUN,
                BUTTON_ACTION_BADUSB_LIST_OPEN, BUTTON_ACTION_SCREEN_NEXT};
    }

    return {BUTTON_ACTION_BADUSB_ARM, BUTTON_ACTION_BADUSB_ARM,
            BUTTON_ACTION_BADUSB_LIST_OPEN, BUTTON_ACTION_SCREEN_NEXT};
}

static ButtonBindingSet _bindingsForRoute(const ButtonRoutingState& route) {
    if (route.missionListActive) {
        return spectreMissionListBindings();
    }
    if (route.listActive) {
        return spectreWifiListBindings();
    }
    if (route.badUsbListActive) {
        return spectreBadUsbListBindings();
    }
    if (route.debriefActive) {
        return spectreDebriefBindings();
    }
    if (route.runContext == RUN_CONTEXT_MISSION) {
        return _missionBindingsForRoute(route);
    }
    if (_routeUsesBadUsb(route)) {
        return _badUsbBindingsForRoute(route);
    }

    return spectreScreenBindings(route.currentScreen);
}

const char* _screenName(Screen screen) {
    switch (screen) {
        case SCREEN_LORA:       return "LORA";
        case SCREEN_MESHTASTIC: return "MESHTASTIC";
        case SCREEN_WIFI:       return "WIFI";
        case SCREEN_BADUSB:     return "BADUSB";
        case SCREEN_RECON:      return "RECON";
        case SCREEN_MISSION:    return "MISSION";
        case SCREEN_SYSTEM:     return "SYSTEM";
        default:                return "UNKNOWN";
    }
}

void _logRuntimeHealth(uint32_t nowMs) {
    static uint32_t lastHealthLogMs = 0;
    static uint32_t lastHeapCheckMs = 0;

    if (nowMs - lastHealthLogMs >= HEALTH_LOG_INTERVAL_MS) {
        uint8_t healthOwner = static_cast<uint8_t>(RADIO_ARB.currentOwner());
        crashCheckpointVolatile(CrashPhase::RUNTIME_HEALTH,
                                healthOwner,
                                STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U);
        struct HealthSnapshot {
            bool wifiConnected;
            bool bleConnected;
            bool gpsValid;
            int wifiCount;
            int pendingFiles;
            uint32_t pendingEnrich;
            uint8_t radioOwner;
            bool timeValid;
            unsigned long uptimeMs;
            int loraPacketCount;
            uint8_t subGhzMode;
            int subGhzNodeCount;
            char timeLocal[24];
            char timeSource[12];
            char subGhzBackend[24];
        } health = {};

        STATE_READ_BEGIN();
        health.wifiConnected = g_state.wifiConnected;
        health.bleConnected = g_state.bleConnected;
        health.gpsValid = g_state.gpsValid;
        health.wifiCount = g_state.wifiNetworkCount;
        health.pendingFiles = g_state.sessionFilesPending;
        health.pendingEnrich =
            g_state.storagePendingEnrichMission + g_state.storagePendingEnrichNoise;
        health.radioOwner = g_state.radioOwner;
        health.timeValid = g_state.timeValid;
        health.uptimeMs = g_state.uptimeMs;
        health.loraPacketCount = g_state.loraPacketCount;
        health.subGhzMode = g_state.subGhzMode;
        health.subGhzNodeCount = g_state.subGhzNodeCount;
        strlcpy(health.timeLocal, g_state.timeLocal, sizeof(health.timeLocal));
        strlcpy(health.timeSource, g_state.timeSource, sizeof(health.timeSource));
        strlcpy(health.subGhzBackend, g_state.subGhzBackend, sizeof(health.subGhzBackend));
        STATE_READ_END();

        const uint32_t totalHeap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        const uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        const uint32_t minHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
        const uint32_t largestHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        const uint32_t totalPsram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        const uint32_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const uint32_t largestPsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        const uint32_t usedHeap = (totalHeap >= freeHeap) ? (totalHeap - freeHeap) : 0;
        const uint32_t usedPsram = (totalPsram >= freePsram) ? (totalPsram - freePsram) : 0;
        const uint32_t heapFragPct = (freeHeap > 0 && largestHeap <= freeHeap)
            ? static_cast<uint32_t>(((freeHeap - largestHeap) * 100UL) / freeHeap)
            : 0;
        const auto kb = [](uint32_t bytes) -> uint32_t {
            return (bytes + 512UL) / 1024UL;
        };
        const auto hb = [](UBaseType_t words) -> uint32_t {
            return static_cast<uint32_t>(words * sizeof(StackType_t));
        };
        const UBaseType_t displayStackWords =
            taskDisplayHandle ? uxTaskGetStackHighWaterMark(taskDisplayHandle) : 0;
        const UBaseType_t hardwareStackWords =
            taskHardwareHandle ? uxTaskGetStackHighWaterMark(taskHardwareHandle) : 0;
        
        _updateCoreLoad(nowMs);

        DLOG_INFO("HEAP",
          "heap free=%luKB min=%luKB largest=%luKB frag=%lu%% psramFree=%luKB core=%u/%u%% owner=%s nets=%d pendingUpload=%d pendingEnrich=%lu",
          static_cast<unsigned long>(kb(freeHeap)),
          static_cast<unsigned long>(kb(minHeap)),
          static_cast<unsigned long>(kb(largestHeap)),
          static_cast<unsigned long>(heapFragPct),
          static_cast<unsigned long>(kb(freePsram)),
          static_cast<unsigned>(g_coreLoad.busyPct[0]),
          static_cast<unsigned>(g_coreLoad.busyPct[1]),
          RadioArbiter::ownerName(static_cast<RadioOwner>(health.radioOwner)),
          health.wifiCount,
          health.pendingFiles,
          static_cast<unsigned long>(health.pendingEnrich));
        if (health.timeValid && health.timeLocal[0]) {
            Serial.printf("[HEALTH] time=%s src=%s\r\n", health.timeLocal, health.timeSource);
        } else {
            const unsigned long s = health.uptimeMs / 1000UL;
            const unsigned long h = s / 3600UL;
            const unsigned long m = (s % 3600UL) / 60UL;
            const unsigned long sec = s % 60UL;
            Serial.printf("[HEALTH] uptime=%luh%02lum%02lus\r\n", h, m, sec);
        }
        Serial.printf("[HEALTH] heap used=%lu/%luKB free=%luKB min=%luKB largest=%luKB frag=%lu%%\r\n",
                      static_cast<unsigned long>(kb(usedHeap)),
                      static_cast<unsigned long>(kb(totalHeap)),
                      static_cast<unsigned long>(kb(freeHeap)),
                      static_cast<unsigned long>(kb(minHeap)),
                      static_cast<unsigned long>(kb(largestHeap)),
                      static_cast<unsigned long>(heapFragPct));
        Serial.printf("[HEALTH] psram used=%lu/%luKB free=%luKB largest=%luKB\r\n",
                      static_cast<unsigned long>(kb(usedPsram)),
                      static_cast<unsigned long>(kb(totalPsram)),
                      static_cast<unsigned long>(kb(freePsram)),
                      static_cast<unsigned long>(kb(largestPsram)));
        Serial.printf("[HEALTH] stack free_min: core0=%luKB/%luKB core1=%luKB/%luKB\r\n",
                      static_cast<unsigned long>(kb(hb(hardwareStackWords))),
                      static_cast<unsigned long>(kb(TASK_HARDWARE_STACK_BYTES)),
                      static_cast<unsigned long>(kb(hb(displayStackWords))),
                      static_cast<unsigned long>(kb(TASK_DISPLAY_STACK_BYTES)));
        Serial.printf("[HEALTH] core usage: core0=%u%% core1=%u%%\r\n",
                      static_cast<unsigned>(g_coreLoad.busyPct[0]),
                      static_cast<unsigned>(g_coreLoad.busyPct[1]));
        Serial.printf("[HEALTH] radio=%s wifi=%d ble=%d gps=%d nets=%d pendingUpload=%d pendingEnrich=%lu\r\n",
                      RadioArbiter::ownerName(static_cast<RadioOwner>(health.radioOwner)),
                      health.wifiConnected ? 1 : 0,
                      health.bleConnected ? 1 : 0,
                      health.gpsValid ? 1 : 0,
                      health.wifiCount,
                      health.pendingFiles,
                      static_cast<unsigned long>(health.pendingEnrich));
        DLOG_DEBUG("SUBGHZ", "heartbeat");

        const char* subGhzBackend = health.subGhzBackend[0] ? health.subGhzBackend : "NONE";

        Serial.printf("[HEALTH] subghz=%s mode=%s pkts=%d nodes=%d\r\n",
                      subGhzBackend,
                      _subGhzModeShort(health.subGhzMode),
                      health.loraPacketCount,
                      health.subGhzNodeCount);

        if (freeHeap < 65536UL || heapFragPct >= 60UL) {
            DLOG_WARN("HEAP",
                      "pressure free=%luKB frag=%lu%% largest=%luKB",
                      static_cast<unsigned long>(kb(freeHeap)),
                      static_cast<unsigned long>(heapFragPct),
                      static_cast<unsigned long>(kb(largestHeap)));
            Serial.printf("[HEALTH] pressure free=%luKB frag=%lu%% largest=%luKB\r\n",
                          static_cast<unsigned long>(kb(freeHeap)),
                          static_cast<unsigned long>(heapFragPct),
                          static_cast<unsigned long>(kb(largestHeap)));
        }

        lastHealthLogMs = nowMs;
        crashBreadcrumbClearVolatile(CrashPhase::RUNTIME_HEALTH);
    }

    if (nowMs - lastHeapCheckMs >= HEAP_CHECK_INTERVAL_MS) {
        const RadioOwner owner = RADIO_ARB.currentOwner();
        const bool radioActive =
            owner == RADIO_WIFI_CAPTURE ||
            owner == RADIO_WIFI_SCAN ||
            owner == RADIO_WIFI_PMKID ||
            owner == RADIO_BLE_GPS ||
            owner == RADIO_BLE_TEXT;
        if (radioActive) {
            DLOG_DEBUG("HEAP", "integrity check deferred owner=%s",
                       RadioArbiter::ownerName(owner));
        } else {
            crashCheckpointVolatile(CrashPhase::HEAP_INTEGRITY,
                                    static_cast<uint8_t>(owner),
                                    STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U);
            const bool heapOk = heap_caps_check_integrity_all(false);
            if (!heapOk) {
                DLOG_ERROR("HEAP", "heap integrity check failed");
                Serial.println("[HEAP] integrity check failed");
            } else {
                DLOG_DEBUG("HEAP", "heap integrity check ok");
                Serial.println("[HEAP] integrity check ok");
            }
            crashBreadcrumbClearVolatile(CrashPhase::HEAP_INTEGRITY);
        }
        lastHeapCheckMs = nowMs;
    }
}

bool _runButtonAction(SpectreButtonAction action, bool storageOk) {
    switch (action) {
        case BUTTON_ACTION_NONE:
            return false;
        case BUTTON_ACTION_SCREEN_NEXT:
            if (isMissionActive()) {
                return false;
            }
            STATE_WRITE_BEGIN();
            g_state.currentScreen = _nextGeneralScreen(g_state.currentScreen);
            g_state.generalScreen = static_cast<uint8_t>(g_state.currentScreen);
            g_state.screenChanged = true;
            STATE_WRITE_END();
            return true;
        case BUTTON_ACTION_SUBGHZ_MODE_CYCLE:
            return _cycleSubGhzMode(1);
        case BUTTON_ACTION_LORA_PING:
            _sendSubGhzTestPing();
             DLOG_INFO("SYS", "SubGhz ping sent");
             return true;
        case BUTTON_ACTION_SLEEP:
            _requestSleepTransition(storageOk, true, true, "manual_sleep");
            vTaskDelay(2000);
            return true;
        case BUTTON_ACTION_WIFI_REFRESH:
            STATE_WRITE_BEGIN();
            g_state.screenChanged = true;
            STATE_WRITE_END();
            return true;
        case BUTTON_ACTION_WIFI_SCAN_LIST:
            _dispatchUiCommand(UI_CMD_OPEN_WIFI_LIST);
            return true;
        case BUTTON_ACTION_WIFI_ALLSCAN:
            STATE_WRITE_BEGIN();
            g_state.lastProbedSSID[0] = '\0';
            g_state.lastProbedMAC[0] = '\0';
            g_state.wifiHuntRequest = false;
            g_state.wifiHuntSSID[0] = '\0';
            g_state.wifiHuntBSSID[0] = '\0';
            g_state.dataRefresh = true;
            STATE_WRITE_END();
            _queueNotification(NOTIF_DEVICE_NEW, "ALLSCAN");
            return true;
        case BUTTON_ACTION_WIFI_LIST_SELECT:
            _dispatchUiCommand(UI_CMD_WIFI_LIST_SELECT);
            return true;
        case BUTTON_ACTION_WIFI_LIST_HUNT:
            _dispatchUiCommand(UI_CMD_WIFI_LIST_HUNT);
            return true;
        case BUTTON_ACTION_WIFI_LIST_DOWN:
            _dispatchUiCommand(UI_CMD_SCROLL_WIFI_LIST_DOWN);
            return true;
        case BUTTON_ACTION_WIFI_LIST_CLOSE:
            STATE_WRITE_BEGIN();
            g_state.wifiListActive = false;
            g_state.dataRefresh = true;
            STATE_WRITE_END();
            _dispatchUiCommand(UI_CMD_CLOSE_WIFI_LIST);
            return true;
        case BUTTON_ACTION_SYSTEM_DEBRIEF:
            _dispatchUiCommand(UI_CMD_OPEN_DEBRIEF);
            return true;
        case BUTTON_ACTION_SESSION_TAG:
            _requestBleTextEntry("session_tag", "Tag this session:");
            return true;
        case BUTTON_ACTION_SAVE_LOCATION:
            _requestBleTextEntry("save_location", "Save location:");
            return true;
        case BUTTON_ACTION_MISSION_NEXT: {
            MissionProfile next = MISSION_RECON;
            STATE_READ_BEGIN();
            const MissionProfile selected =
                _sanitizeMissionProfile(g_state.missionSelection);
            next = static_cast<MissionProfile>(
                (static_cast<uint8_t>(selected) + 1U) %
                static_cast<uint8_t>(MISSION_PROFILE_COUNT));
            STATE_READ_END();
            STATE_WRITE_BEGIN();
            g_state.missionSelection = static_cast<uint8_t>(next);
            g_state.dataRefresh = true;
            STATE_WRITE_END();
            return true;
        }
        case BUTTON_ACTION_MISSION_ENTER: {
            bool missionListOpen = false;
            MissionProfile profile = MISSION_RECON;
            STATE_READ_BEGIN();
            missionListOpen = g_state.missionListActive;
            profile = _sanitizeMissionProfile(g_state.missionSelection);
            STATE_READ_END();
            if (!_launchMissionProfile(profile)) {
                return false;
            }
            if (missionListOpen) {
                STATE_WRITE_BEGIN();
                g_state.missionListActive = false;
                g_state.dataRefresh = true;
                STATE_WRITE_END();
                _dispatchUiCommand(UI_CMD_CLOSE_MISSION_LIST);
            }
            return true;
        }
        case BUTTON_ACTION_MISSION_EXIT:
            exitMission();
            return true;
        case BUTTON_ACTION_MISSION_LIST_OPEN: {
            int selected = static_cast<int>(MISSION_RECON);
            STATE_READ_BEGIN();
            selected = static_cast<int>(_sanitizeMissionProfile(g_state.missionSelection));
            STATE_READ_END();
            STATE_WRITE_BEGIN();
            g_state.missionSelection = static_cast<uint8_t>(selected);
            g_state.missionListScroll = max(0, selected - 1);
            g_state.dataRefresh = true;
            STATE_WRITE_END();
            _dispatchUiCommand(UI_CMD_OPEN_MISSION_LIST);
            return true;
        }
        case BUTTON_ACTION_MISSION_LIST_SELECT: {
            MissionProfile profile = MISSION_RECON;
            STATE_READ_BEGIN();
            profile = _sanitizeMissionProfile(g_state.missionSelection);
            STATE_READ_END();
            if (!_launchMissionProfile(profile)) {
                return false;
            }
            STATE_WRITE_BEGIN();
            g_state.missionListActive = false;
            g_state.dataRefresh = true;
            STATE_WRITE_END();
            _dispatchUiCommand(UI_CMD_CLOSE_MISSION_LIST);
            return true;
        }
        case BUTTON_ACTION_MISSION_LIST_DOWN:
            _dispatchUiCommand(UI_CMD_SCROLL_MISSION_LIST_DOWN);
            return true;
        case BUTTON_ACTION_MISSION_LIST_CLOSE:
            STATE_WRITE_BEGIN();
            g_state.missionListActive = false;
            g_state.dataRefresh = true;
            STATE_WRITE_END();
            _dispatchUiCommand(UI_CMD_CLOSE_MISSION_LIST);
            return true;
        case BUTTON_ACTION_UPLINK_TRIGGER:
            return MQTT_MGR.requestDump(true);
        case BUTTON_ACTION_PWNY_FORCE_DEAUTH:
            return WIFI_MGR.forcePwnyDeauth();
        case BUTTON_ACTION_ANTENNA_TOGGLE: {
            bool ext = true;
            STATE_READ_BEGIN();
            ext = g_state.antennaExternal;
            STATE_READ_END();
            if (!WIFI_MGR.setExternalAntenna(!ext)) {
                _queueNotification(NOTIF_DEVICE_NEW,
                                   "ANT SWITCH UNAVAILABLE");
                return false;
            }
            STATE_WRITE_BEGIN();
            g_state.dataRefresh = true;
            STATE_WRITE_END();
            return true;
        }
        case BUTTON_ACTION_DEBRIEF_EXPORT:
            _runSessionExport(true);
            STATE_WRITE_BEGIN();
            g_state.dataRefresh = true;
            STATE_WRITE_END();
            _dispatchUiCommand(UI_CMD_OPEN_DEBRIEF);
            DLOG_INFO("EXPORT", "Manual export triggered from Debrief");
            return true;
        case BUTTON_ACTION_DEBRIEF_CLEAR: {
            const uint32_t pendingUploads =
                storageOk ? STORAGE.getPendingEventCount() : 0;
            STATE_WRITE_BEGIN();
            g_state.sessionNetworks  = 0;
            g_state.sessionDevices   = 0;
            g_state.sessionProbes    = 0;
            g_state.sessionPMKIDs    = 0;
            g_state.sessionDrones    = 0;
            g_state.sessionFilesPending = static_cast<int>(pendingUploads);
            g_state.kaliSyncAvailable = (pendingUploads > 0);
            g_state.dataRefresh      = true;
            STATE_WRITE_END();
            _dispatchUiCommand(UI_CMD_OPEN_DEBRIEF);
            DLOG_INFO("SYS", "Session data cleared");
            STORAGE.checkHealth();
            return true;
        }
        case BUTTON_ACTION_BADUSB_LIST_OPEN: {
            const int scriptCount = BADUSB_MGR.refreshScripts();
            if (scriptCount <= 0) {
                _queueNotification(NOTIF_DEVICE_NEW, "BADUSB NO SCRIPTS");
                return false;
            }

            int selected = 0;
            int scroll = 0;
            STATE_READ_BEGIN();
            selected = g_state.badUsbListSelected;
            scroll = g_state.badUsbListScroll;
            STATE_READ_END();

            if (selected < 0 || selected >= scriptCount) {
                selected = 0;
            }
            if (scroll > selected) {
                scroll = selected;
            }
            if (selected >= scroll + 8) {
                scroll = selected - 7;
            }

            STATE_WRITE_BEGIN();
            g_state.badUsbListActive = true;
            g_state.badUsbListSelected = selected;
            g_state.badUsbListScroll = scroll;
            g_state.dataRefresh = true;
            STATE_WRITE_END();
            _dispatchUiCommand(UI_CMD_OPEN_BADUSB_LIST);
            return true;
        }
        case BUTTON_ACTION_BADUSB_LIST_SELECT:
            _dispatchUiCommand(UI_CMD_BADUSB_LIST_SELECT);
            return true;
        case BUTTON_ACTION_BADUSB_LIST_DOWN:
            _dispatchUiCommand(UI_CMD_SCROLL_BADUSB_LIST_DOWN);
            return true;
        case BUTTON_ACTION_BADUSB_LIST_CLOSE:
            _dispatchUiCommand(UI_CMD_CLOSE_BADUSB_LIST);
            return true;
        case BUTTON_ACTION_BADUSB_ARM: {
            int selected = 0;
            bool listOpen = false;
            STATE_READ_BEGIN();
            selected = g_state.badUsbListSelected;
            listOpen = g_state.badUsbListActive;
            STATE_READ_END();
            const bool ok = BADUSB_MGR.armSelected(selected);
            if (ok && listOpen) {
                _dispatchUiCommand(UI_CMD_CLOSE_BADUSB_LIST);
            }
            return ok;
        }
        case BUTTON_ACTION_BADUSB_RUN:
            return BADUSB_MGR.runArmed();
        case BUTTON_ACTION_BADUSB_CANCEL:
            BADUSB_MGR.cancel();
            return true;
        case BUTTON_ACTION_DEBRIEF_BACK:
            _dispatchUiCommand(UI_CMD_CLOSE_DEBRIEF);
            return true;
        case BUTTON_ACTION_BLE_TEST: {
            return requestManualBleTest("ble_test");
        }
        default:
            return false;
    }
}

void _handleDisplayEvent(const Event& event, const DisplayFrameState& snapshot) {
    switch (event.type) {
        case EVT_NOTIFY:
            display.showNotification(static_cast<uint8_t>(event.intData),
                                     event.strData);
            break;
        case EVT_UI_COMMAND:
            switch (static_cast<UICommand>(event.intData)) {
                case UI_CMD_OPEN_WIFI_LIST:
                    display.openWifiList();
                    break;
                case UI_CMD_CLOSE_WIFI_LIST:
                    display.closeWifiList();
                    break;
                case UI_CMD_SCROLL_WIFI_LIST_DOWN:
                    display.scrollWifiList(1);
                    break;
                case UI_CMD_WIFI_LIST_SELECT:
                    display.wifiListSelect();
                    break;
                case UI_CMD_WIFI_LIST_HUNT:
                    display.wifiListHunt();
                    break;
                case UI_CMD_OPEN_MISSION_LIST:
                    display.openMissionList();
                    break;
                case UI_CMD_CLOSE_MISSION_LIST:
                    display.closeMissionList();
                    break;
                case UI_CMD_SCROLL_MISSION_LIST_DOWN:
                    display.scrollMissionList(1);
                    break;
                case UI_CMD_MISSION_LIST_SELECT:
                    display.missionListSelect();
                    break;
                case UI_CMD_OPEN_BADUSB_LIST:
                    display.openBadUsbList();
                    break;
                case UI_CMD_CLOSE_BADUSB_LIST:
                    display.closeBadUsbList();
                    break;
                case UI_CMD_SCROLL_BADUSB_LIST_DOWN:
                    display.scrollBadUsbList(1);
                    break;
                case UI_CMD_BADUSB_LIST_SELECT:
                    display.badUsbListSelect();
                    break;
                case UI_CMD_OPEN_DEBRIEF:
                    display.drawDebrief();
                    STATE_WRITE_BEGIN();
                    g_state.debriefActive = true;
                    STATE_WRITE_END();
                    break;
                case UI_CMD_CLOSE_DEBRIEF:
                    STATE_WRITE_BEGIN();
                    g_state.debriefActive = false;
                    STATE_WRITE_END();
                    display.drawSystem(snapshot.battVoltage,
                                       snapshot.uptimeMs,
                                       snapshot.storageStr);
                    break;
                default:
                    break;
            }
            break;
        case EVT_STORAGE_NEARLY_FULL:
            display.showNotification(NOTIF_STORAGE, "STORAGE NEARLY FULL");
            break;
        case EVT_BATTERY_CRITICAL:
            display.showNotification(NOTIF_POWER, "LOW POWER SLEEP 5M");
            display.pulseMascot(MASCOT_LOW_BATTERY, 15000);
            break;
        case EVT_LORA_PACKET_RX:
            display.pulseMascot(MASCOT_ALERT, 900);
            break;
        case EVT_LORA_PACKET_TX:
            display.pulseMascot(MASCOT_TRANSMIT, 700);
            break;
        case EVT_ANTENNA_TOGGLED:
            display.showNotification(NOTIF_DEVICE_NEW,
                                     event.intData ? "ANTENNA: EXTERNAL"
                                                   : "ANTENNA: INTERNAL");
            STATE_WRITE_BEGIN();
            g_state.dataRefresh = true;
            STATE_WRITE_END();
            break;
        default:
            break;
    }
}

static ButtonBindingSet _displayBindingsForSnapshot(const DisplayFrameState& snapshot) {
        ButtonRoutingState route{};
        route.currentScreen = snapshot.currentScreen;
        route.listActive = snapshot.wifiListActive;
        route.missionListActive = snapshot.missionListActive;
        route.badUsbListActive = snapshot.badUsbListActive;
        route.debriefActive = snapshot.debriefActive;
        route.badUsbArmed = snapshot.badUsbArmed;
        route.badUsbRunning = snapshot.badUsbRunning;
        route.badUsbReady = snapshot.badUsbReady;
        route.runContext = snapshot.runContext;
        route.activeMissionProfile = snapshot.activeMissionProfile;
        route.missionSelection = snapshot.missionSelection;
        return _bindingsForRoute(route);
    }

static bool _syncDisplayFromSnapshot(const DisplayFrameState& snapshot);

// ── Core 1: TaskDisplay ──

void TaskDisplay(void* pvParameters) {
    DLOG_INFO("CORE", "Display task started");
    uint32_t lastStackLogMs = millis();
    UBaseType_t minStackWords = uxTaskGetStackHighWaterMark(nullptr);
    DLOG_INFO("STACK", "TaskDisplay watermark=%luB",
              (unsigned long)(minStackWords * sizeof(StackType_t)));

    if (g_bootRecoveryMode) {
        PrebootFallback::showFatal(tft, "RECOVERY MODE", "USB FLASH READY");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    PrebootFallback::showInit(tft, "SPECTRE", "BRINGING UP LVGL");

    LVGLDriver::begin(&tft);
    uint32_t lastLvTickMs = millis();
    tft.fillScreen(TFT_BLACK);
    s_displayLayerReady = true;

    while (true) {
        uint32_t now = millis();
        uint32_t delta = now - lastLvTickMs;
        if (delta > 0) {
            lv_tick_inc(delta);
            lastLvTickMs = now;
        }
        STATE_READ_BEGIN();
        bool done = g_state.hwInitDone;
        STATE_READ_END();
        if (done) break;
        vTaskDelay(10);
    }

    bool loraOk, storageOk;
    STATE_READ_BEGIN();
    loraOk    = g_state.loraReady;
    storageOk = g_state.storageReady;
    STATE_READ_END();

    tft.fillScreen(TFT_BLACK);
    lv_refr_now(NULL);

#if BOOT_SEQUENCE_ENABLED
    runBootSequence(display, loraOk, storageOk);
    lv_obj_clean(lv_screen_active());
    lv_refr_now(NULL);
#else
    lv_obj_clean(lv_screen_active());
    lv_refr_now(NULL);
#endif

    display.begin();
    lv_refr_now(NULL);

    STATE_WRITE_BEGIN();
    g_state.screenChanged = true;
    STATE_WRITE_END();

    int      animFrame   = 0;
    uint32_t lastFrameMs = 0;
    bool     statusValid = false;
    ExecutionPolicy::StatusBarStateView lastStatus = {};
    bool     sleepOverlayShown = false;
    lv_obj_t* sleepOverlay = nullptr;

    DisplayFrameState snapshot;

    for (;;) {
        uint32_t now = millis();
        uint32_t lvDelta = now - lastLvTickMs;
        if (lvDelta > 0) {
            lv_tick_inc(lvDelta);
            lastLvTickMs = now;
        }
        uint8_t currentPowerState = POWER_STATE_BATTERY_NORMAL;
        STATE_READ_BEGIN();
        currentPowerState = g_state.powerState;
        STATE_READ_END();
        if (now - lastFrameMs < _displayFrameIntervalForPowerState(currentPowerState)) {
            vTaskDelay(1);
            continue;
        }
        lastFrameMs = now;

        _captureDisplayFrameState(snapshot);

        animFrame++;
        if (animFrame > 10000) animFrame = 0;

        Event queuedEvent;
        bool hadDisplayEvents = false;
        while (BUS.receive(queuedEvent, 0)) {
            hadDisplayEvents = true;
            _handleDisplayEvent(queuedEvent, snapshot);
        }

        if (snapshot.requestSleep) {
            if (!sleepOverlayShown) {
                lv_obj_t* scr = lv_screen_active();

                sleepOverlay = lv_obj_create(scr);
                lv_obj_set_size(sleepOverlay, THEME_SCREEN_W, THEME_SCREEN_H);
                lv_obj_set_pos(sleepOverlay, 0, 0);
                lv_obj_set_style_bg_color(sleepOverlay, lv_color_hex(0x000000), 0);
                lv_obj_set_style_bg_opa(sleepOverlay, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(sleepOverlay, 0, 0);
                lv_obj_set_style_radius(sleepOverlay, 0, 0);
                lv_obj_clear_flag(sleepOverlay, LV_OBJ_FLAG_SCROLLABLE);

                lv_obj_t* label = lv_label_create(sleepOverlay);
                lv_label_set_text(label, "POWERING DOWN...");
                lv_obj_set_style_text_color(label, lv_color_hex(0xFF4D4D), 0);
                lv_obj_set_style_text_font(label, FONT_HEADER, 0);
                lv_obj_center(label);
                lv_obj_move_foreground(sleepOverlay);

                lv_refr_now(NULL);
                STATE_WRITE_BEGIN();
                spectreAcknowledgeSleepPresentationLocked(now, SLEEP_PRESENTATION_HOLD_MS);
                STATE_WRITE_END();
                sleepOverlayShown = true;
            }

            vTaskDelay(1);
            continue;
        }
        if (sleepOverlayShown) {
            if (sleepOverlay) {
                lv_obj_delete(sleepOverlay);
                sleepOverlay = nullptr;
            }
            sleepOverlayShown = false;
            lv_refr_now(NULL);
        }

        uint32_t displayTimeoutMs = BACKLIGHT_TIMEOUT_MS;
        if (SETTINGS.isReady()) {
            displayTimeoutMs = SETTINGS.get().displayTimeoutMs;
        }
        _setDisplayBrightnessPct(_displayBrightnessForPowerState(snapshot.powerState));
        const bool keepDisplayAwake =
            snapshot.textInputPending ||
            snapshot.wifiListActive ||
            snapshot.missionListActive ||
            snapshot.badUsbListActive ||
            snapshot.debriefActive ||
            snapshot.badUsbArmed ||
            snapshot.badUsbRunning ||
            (displayTimeoutMs == 0) ||
            ((now - _lastUiActivityMs()) < displayTimeoutMs);
        if (_setDisplayAwake(keepDisplayAwake) && keepDisplayAwake) {
            STATE_WRITE_BEGIN();
            g_state.screenChanged = true;
            STATE_WRITE_END();
        }

        _refreshDisplayStatusBar(snapshot, statusValid, lastStatus);

        bool didDisplayWork = false;
        if (snapshot.screenChanged || snapshot.loraNewPacket || snapshot.dataRefresh) {
            didDisplayWork = _syncDisplayFromSnapshot(snapshot);
            if (snapshot.loraNewPacket) {
                display.triggerDataPulse();
            }
        }

        display.tickNotif();

        static uint32_t lastWifiListRefreshMs = 0;
        if (snapshot.wifiListActive &&
            millis() - lastWifiListRefreshMs > 750UL) {
            display.refreshWifiList();
            lastWifiListRefreshMs = millis();
            didDisplayWork = true;
        }

        lv_timer_handler();
        static uint32_t lastMascotMs = 0;
        if (didDisplayWork || hadDisplayEvents) {
            lastMascotMs = millis();
        } else if (millis() - lastMascotMs > _displayMascotIntervalForPowerState(snapshot.powerState)) {
            display.drawMascotFrame(snapshot.mascotState, animFrame);
            lastMascotMs = millis();
        }

        if (now - lastStackLogMs >= STACK_LOG_INTERVAL_MS) {
            const UBaseType_t freeWords = uxTaskGetStackHighWaterMark(nullptr);
            if (freeWords < minStackWords) {
                minStackWords = freeWords;
            }
            DLOG_INFO("STACK", "TaskDisplay watermark=%luB min=%luB",
                      (unsigned long)(freeWords * sizeof(StackType_t)),
                      (unsigned long)(minStackWords * sizeof(StackType_t)));
            lastStackLogMs = now;
        }

        vTaskDelay(1);
    }
}

static bool _syncDisplayFromSnapshot(const DisplayFrameState& snapshot);

static bool _syncDisplayFromSnapshot(const DisplayFrameState& s) {
    static bool haveLast = false;
    static DisplayFrameState last = {};
    static ButtonBindingSet lastBindings = {
        BUTTON_ACTION_NONE,
        BUTTON_ACTION_NONE,
        BUTTON_ACTION_NONE,
        BUTTON_ACTION_NONE
    };

    const bool visibleChanged = !haveLast ||
                                s.screenChanged ||
                                s.loraNewPacket ||
                                _visibleDisplayDataChanged(last, s);
    const ButtonBindingSet bindings = _displayBindingsForSnapshot(s);
    const bool bindingsChanged = !haveLast ||
                                 _buttonBindingsChanged(lastBindings, bindings);

    bool didWork = false;

    if (visibleChanged) {
        display.setScreen(s.currentScreen);

        switch (s.currentScreen) {
            case SCREEN_LORA:
                display.drawLora(
                    s.subGhzModule[0] ? s.subGhzModule : "NONE",
                    subGhzModeName(static_cast<SubGhzMode>(s.subGhzMode)),
                    s.subGhzFrequencyHz,
                    static_cast<uint16_t>(s.subGhzNodeCount),
                    s.loraLastPayload,
                    s.loraRSSI,
                    s.loraSNR,
                    s.loraPacketCount);
                break;

            case SCREEN_MESHTASTIC:
                display.drawMeshtastic("--", "LONGFAST");
                break;

            case SCREEN_WIFI:
                display.drawWifi(
                    s.wifiSSID,
                    s.wifiNetworkCount,
                    s.probePacketCount > 0 ? s.lastProbedMAC : "--");
                break;

            case SCREEN_BADUSB:
                display.drawBadUsb();
                break;

            case SCREEN_MISSION:
                display.drawMission(
                    _sanitizeMissionProfile(s.activeMissionProfile));
                break;

            case SCREEN_RECON:
                display.drawRecon(
                    _sanitizeMissionProfile(s.missionSelection));
                break;

            case SCREEN_SYSTEM:
                display.drawSystem(
                    s.battVoltage,
                    s.uptimeMs,
                    s.storageStr);
                break;

            default:
                break;
        }

        display.updateDivider();
        didWork = true;
    }

    if (bindingsChanged) {
        display.setActionHints(bindings);
        didWork = true;
    }

    last = s;
    lastBindings = bindings;
    haveLast = true;
    return didWork;
}

void _applyExportSummaryToState(const SessionExportSummary& summary,
                                bool ok) {
    const uint32_t totalPendingUploads =
        STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0;
    const uint32_t sessionPendingUploads =
        ok ? summary.pendingUploads : totalPendingUploads;

    STATE_WRITE_BEGIN();
    g_state.exportLastOk = ok;
    g_state.exportLastEvents = ok ? summary.totalEvents : 0;
    g_state.exportLastFiles = ok ? summary.exportedFiles : 0;
    g_state.exportLastBytes = ok ? summary.exportedBytes : 0;
    g_state.exportLastPending = sessionPendingUploads;
    strlcpy(g_state.exportLastISO, summary.generatedIso, sizeof(g_state.exportLastISO));
    strlcpy(g_state.exportLastSessionId, summary.sessionId, sizeof(g_state.exportLastSessionId));
    g_state.sessionFilesPending = static_cast<int>(totalPendingUploads);
    g_state.kaliSyncAvailable = (totalPendingUploads > 0);
    STATE_WRITE_END();
}

static void _clearStorageSummaryMirror() {
    STATE_WRITE_BEGIN();
    g_state.storageSummaryValid = false;
    g_state.storageSummaryUpdatedMs = millis();

    g_state.storageMissionTotal = 0;
    g_state.storageNoiseTotal = 0;

    g_state.storageP0Total = 0;
    g_state.storageP1Total = 0;
    g_state.storageP2Total = 0;
    g_state.storageP3Total = 0;

    g_state.storagePendingUploadMission = 0;
    g_state.storagePendingUploadNoise = 0;

    g_state.storagePendingEnrichMission = 0;
    g_state.storagePendingEnrichNoise = 0;

    g_state.storageEnrichmentDeltas = 0;

    g_state.storageFirstEventId = 0;
    g_state.storageLastEventId = 0;
    STATE_WRITE_END();
}

static bool _refreshStorageSummaryMirror(bool force) {
    static uint32_t lastRefreshMs = 0;
    const uint32_t now = millis();
    const uint32_t pendingBacklog =
        STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U;
    const uint32_t refreshIntervalMs =
        pendingBacklog >= 10000UL ? 10UL * 60000UL :
        pendingBacklog >= 1000UL  ?  5UL * 60000UL :
                                    30000UL;

    if (!force && now - lastRefreshMs < refreshIntervalMs) {
        return true;
    }

    if (!STORAGE.isReady()) {
        _clearStorageSummaryMirror();
        lastRefreshMs = now;
        return false;
    }

    // Avoid heavy spool scans while any radio owner is active once the backlog
    // is large. The mirror can be minutes stale under pressure; explicit
    // commands and forced refreshes still recount.
    const RadioOwner owner = RADIO_ARB.currentOwner();
    const bool radioActive =
        owner == RADIO_WIFI_CAPTURE ||
        owner == RADIO_WIFI_SCAN ||
        owner == RADIO_WIFI_PMKID ||
        owner == RADIO_BLE_GPS ||
        owner == RADIO_BLE_TEXT ||
        owner == RADIO_WIFI_UPLOAD;
    if (!force && pendingBacklog >= 1000UL && radioActive) {
        return false;
    }

    StorageSessionSummary summary;
    const bool ok = STORAGE.getSessionStorageSummary(nullptr, summary);

    STATE_WRITE_BEGIN();
    g_state.storageSummaryValid = ok;
    g_state.storageSummaryUpdatedMs = now;

    if (ok) {
        g_state.storageMissionTotal = summary.missionTotal;
        g_state.storageNoiseTotal   = summary.noiseTotal;

        g_state.storageP0Total = summary.p0Total;
        g_state.storageP1Total = summary.p1Total;
        g_state.storageP2Total = summary.p2Total;
        g_state.storageP3Total = summary.p3Total;

        g_state.storagePendingUploadMission = summary.pendingUploadMission;
        g_state.storagePendingUploadNoise   = summary.pendingUploadNoise;

        g_state.storagePendingEnrichMission = summary.pendingEnrichmentMission;
        g_state.storagePendingEnrichNoise   = summary.pendingEnrichmentNoise;

        g_state.storageEnrichmentDeltas = summary.enrichmentDeltas;

        g_state.storageFirstEventId = summary.firstEventId;
        g_state.storageLastEventId  = summary.lastEventId;
    }
    STATE_WRITE_END();

    lastRefreshMs = now;
    return ok;
}

void _runSessionExport(bool notifyUser) {
    SessionExportSummary summary;
    const bool ok = EXPORT_MGR.exportCurrentSession(&summary);

    if (!summary.generatedIso[0]) {
        if (!TIME_SVC.formatNowIso(summary.generatedIso, sizeof(summary.generatedIso))) {
            summary.generatedIso[0] = '\0';
        }
    }

    _applyExportSummaryToState(summary, ok);
    _refreshStorageSummaryMirror(true);

    if (ok) {
        DLOG_INFO("EXPORT", "Session export complete: %lu events, %u files, %lu bytes",
                  static_cast<unsigned long>(summary.totalEvents),
                  static_cast<unsigned>(summary.exportedFiles),
                  static_cast<unsigned long>(summary.exportedBytes));
    } else {
        DLOG_WARN("EXPORT", "Session export failed");
    }

    if (notifyUser) {
        char notifText[48];
        if (ok) {
            snprintf(notifText, sizeof(notifText),
                     "EXPORT %luE %uF",
                     static_cast<unsigned long>(summary.totalEvents),
                     static_cast<unsigned>(summary.exportedFiles));
        } else {
            strlcpy(notifText, "EXPORT FAILED", sizeof(notifText));
        }
        _queueNotification(NOTIF_EXPORT, notifText);
    }
}

// ── Core 0: TaskHardware ──

void TaskHardware(void* pvParameters) {
    DLOG_INFO("CORE", "Hardware task started");
    uint32_t lastStackLogMs = millis();
    UBaseType_t minStackWords = uxTaskGetStackHighWaterMark(nullptr);
    DLOG_INFO("STACK", "TaskHardware watermark=%luB",
              (unsigned long)(minStackWords * sizeof(StackType_t)));
    uint32_t lastWifiTick = 0;
    CompanionScheduler companion = {};
    companion.enabled = PHONE_COMPANION_ENABLED;
    bool lastTextPending = false;
    ExecutionPolicy::UiRefreshMarks uiRefreshMarks = {};

    if (g_bootRecoveryMode) {
        STATE_WRITE_BEGIN();
        g_state.hwInitDone = true;
        g_state.storageReady = false;
        g_state.loraReady = false;
        g_state.screenChanged = true;
        STATE_WRITE_END();
        DLOG_WARN("CORE", "Boot recovery mode active; hardware init skipped");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    const bool settingsOk = SETTINGS.begin();
    const bool timeOk = TIME_SVC.begin();
    POWER_MGR.begin();
    _applyPowerSnapshotToState(POWER_MGR.snapshot());
    DLOG_INFO("CORE", "Settings=%s Time=%s",
              settingsOk ? "OK" : "FAIL",
              timeOk ? "OK" : "FAIL");

    if (!_waitForDisplayLayerReady(4000UL)) {
        DLOG_WARN("CORE", "Display layer not ready before hardware init");
    }

    bool storageOk = STORAGE.begin();
    String storageUsed = storageOk ? STORAGE.getCachedUsedString() : String();
    _publishStorageState(storageOk, storageUsed);

    if (storageOk) {
        DebugLog::begin();
        FieldVault::begin();
        _loadKnownLocationsIntoState();
        if (STORAGE.hasStorageMaintenanceWork()) {
            _clearStorageSummaryMirror();
            DLOG_WARN("STORAGE", "Initial storage summary deferred until repair completes");
        } else {
            _refreshStorageSummaryMirror(true);
        }
    }
    const bool exportOk = EXPORT_MGR.begin();
    if (exportOk) {
        SessionExportSummary latestExport;
        if (EXPORT_MGR.loadLatestSummary(&latestExport)) {
            _applyExportSummaryToState(latestExport, true);
            DLOG_INFO("EXPORT", "Loaded last export: %s (%lu events)",
                      latestExport.sessionId,
                      static_cast<unsigned long>(latestExport.totalEvents));
        }
    }


    _initializeHardwareManagers(lastWifiTick);
    _publishHardwareReadyState();
    _markUiActivity();

    DLOG_INFO("CORE", "Hardware ready");

    for (;;) {
        static uint32_t lastWifiRefresh = 0;
        static uint32_t lastPwnyRefresh = 0;
        static uint32_t lastSystemRefresh = 0;
        static uint32_t lastGpsFixSeen = 0;
        static uint32_t lastLoopStartMs = 0;
        const uint32_t loopNow = millis();
        if (lastLoopStartMs != 0) {
            const uint32_t loopGapMs = loopNow - lastLoopStartMs;
            if (loopGapMs >= HARDWARE_LOOP_GAP_WARN_MS) {
                DLOG_WARN("CORE",
                          "TaskHardware loop gap=%lums owner=%s pendingUpload=%lu maint=%d",
                          static_cast<unsigned long>(loopGapMs),
                          RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                          static_cast<unsigned long>(
                              STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U),
                          (storageOk && STORAGE.hasStorageMaintenanceWork()) ? 1 : 0);
            }
        }
        lastLoopStartMs = loopNow;

        POWER_MGR.tick(loopNow);
        const PowerSnapshot power = POWER_MGR.consumeSnapshot();
        _applyPowerSnapshotToState(power);

        if (power.criticalJustEntered) {
            if (storageOk) {
                STORAGE.checkpointSessionState();
            }
            BUS.publish(EVT_BATTERY_CRITICAL);
        }

        bool sleepAlreadyRequested = false;
        STATE_READ_BEGIN();
        sleepAlreadyRequested = g_state.requestSleep;
        STATE_READ_END();

        if (!POWER_RUN_UNTIL_DEAD &&
            !sleepAlreadyRequested &&
            power.state == POWER_STATE_BATTERY_CRITICAL &&
            power.criticalSleepAtMs != 0 &&
            loopNow >= power.criticalSleepAtMs) {
            _requestSleepTransition(storageOk, false, false, "critical_timeout");
        }

        _serviceSleepTransition(loopNow);

        _pollUsbSerialConsole();
        BADUSB_MGR.tick();
        SUBGHZ.tick();

        {
            const SubGhzStatus sg = SUBGHZ.status();
            STATE_WRITE_BEGIN();
            g_state.loraReady = sg.ready;
            g_state.subGhzMode = static_cast<uint8_t>(sg.mode);
            strlcpy(g_state.subGhzBackend,
                    sg.backendName,
                    sizeof(g_state.subGhzBackend));
            strlcpy(g_state.subGhzModule,
                    sg.moduleName,
                    sizeof(g_state.subGhzModule));
            g_state.subGhzFrequencyHz = sg.frequencyHz;
            g_state.subGhzNodeCount = static_cast<int>(sg.nodeCount);
            STATE_WRITE_END();
        }

        if (SUBGHZ.available()) {
            SubGhzPacket pkt = {};

            if (SUBGHZ.readPacket(pkt)) {
                SUBGHZ.notePacket(pkt);
                if (storageOk) {
                    if (pkt.frequencyHz == 0 && SETTINGS.isReady()) {
                        pkt.frequencyHz = static_cast<uint32_t>(SETTINGS.get().loraFrequency);
                    }
                    SubGhzRecordWriter::logPacketRx(STORAGE, pkt);
                    DLOG_DEBUG("SUBGHZ", "Normalized RX record written");
                }

                const SubGhzStats sgStats = SUBGHZ.stats();

                const SubGhzStatus sg = SUBGHZ.status();

                STATE_WRITE_BEGIN();
                g_state.loraRSSI        = pkt.rssi;
                g_state.loraSNR         = pkt.snr;
                g_state.loraPacketCount = static_cast<int>(sgStats.rxPackets);
                g_state.loraNewPacket   = true;
                g_state.subGhzMode      = static_cast<uint8_t>(sg.mode);
                g_state.subGhzNodeCount = static_cast<int>(SUBGHZ.nodeCount());
                strlcpy(g_state.subGhzBackend,
                        sg.backendName,
                        sizeof(g_state.subGhzBackend));
                strlcpy(g_state.subGhzModule,
                        sg.moduleName,
                        sizeof(g_state.subGhzModule));
                g_state.subGhzFrequencyHz = sg.frequencyHz;
                snprintf(g_state.loraLastPayload,
                         sizeof(g_state.loraLastPayload),
                         "#%u %s",
                         static_cast<unsigned>(pkt.source),
                         pkt.payload);
                STATE_WRITE_END();

                BUS.publish(EVT_LORA_PACKET_RX);
                DLOG_DEBUG("SUBGHZ", "RX backend=%s src=%u payload=%s RSSI=%d SNR=%d",
                           SUBGHZ.status().backendName,
                           static_cast<unsigned>(pkt.source),
                           pkt.payload,
                           static_cast<int>(pkt.rssi),
                           static_cast<int>(pkt.snr));
            }
        }

        ButtonEvent evt = buttons.getEvent();
        if (evt != BTN_NONE) {
            _markUiActivity();
            if (!_isDisplayAwake()) {
                _setDisplayAwake(true);
                STATE_WRITE_BEGIN();
                g_state.screenChanged = true;
                STATE_WRITE_END();
                DLOG_INFO("CORE", "BTN evt=%s consumed as display wake",
                          _buttonEventName(evt));
                Serial.printf("[BTN] evt=%s consumed as display wake\n",
                              _buttonEventName(evt));
                continue;
            }
        }
        if (BLE_MGR.handleButtonEvent(evt)) {
            if (evt != BTN_NONE) {
                DLOG_INFO("CORE", "BTN evt=%s route=BLE",
                          _buttonEventName(evt));
                Serial.printf("[BTN] evt=%s route=BLE\n",
                              _buttonEventName(evt));
            }
            continue;
        }

        if (evt != BTN_NONE) {
            const ButtonRoutingState route = _readButtonRoutingState();

            if (evt == BTN_AB_SHORT &&
                route.runContext == RUN_CONTEXT_MISSION &&
                _sanitizeMissionProfile(route.activeMissionProfile) == MISSION_PWNY) {
                const bool handled =
                    !WIFI_MGR.isPwnyActive() &&
                    RADIO_ARB.requestPwnyLease(RadioArbiter::LEASE_INFINITE, "mission_pwny");
                if (handled) {
                    _markUiActivity();
                    _queueNotification(NOTIF_DEVICE_NEW, "PWNY ARMED");
                }
                const char* overlay = route.listActive ? "WIFI_LIST"
                                   : route.missionListActive ? "MISSION_LIST"
                                   : route.badUsbListActive ? "BADUSB_LIST"
                                   : route.debriefActive ? "DEBRIEF"
                                   : "NONE";
                DLOG_INFO("CORE",
                          "BTN evt=%s screen=%s overlay=%s combo=PWNY_ARM handled=%d",
                          _buttonEventName(evt),
                          _screenName(route.currentScreen),
                          overlay,
                          handled ? 1 : 0);
                Serial.printf("[BTN] evt=%s screen=%s overlay=%s combo=PWNY_ARM handled=%d\n",
                              _buttonEventName(evt),
                              _screenName(route.currentScreen),
                              overlay,
                              handled ? 1 : 0);
                continue;
            }

            const ButtonBindingSet bindings = _bindingsForRoute(route);

            const SpectreButtonAction action = _actionForEvent(evt, bindings);
            const bool handled = _runButtonAction(action, storageOk);
            if (handled) {
                _markUiActivity();
            }
            const char* overlay = route.listActive ? "WIFI_LIST"
                               : route.missionListActive ? "MISSION_LIST"
                               : route.badUsbListActive ? "BADUSB_LIST"
                               : route.debriefActive ? "DEBRIEF"
                               : "NONE";
            DLOG_INFO("CORE",
                      "BTN evt=%s screen=%s overlay=%s action=%s label=%s handled=%d",
                      _buttonEventName(evt),
                      _screenName(route.currentScreen),
                      overlay,
                      _buttonActionName(action),
                      spectreButtonActionLabel(action) ? spectreButtonActionLabel(action) : "-",
                      handled ? 1 : 0);
            Serial.printf("[BTN] evt=%s screen=%s overlay=%s action=%s label=%s handled=%d\n",
                          _buttonEventName(evt),
                          _screenName(route.currentScreen),
                          overlay,
                          _buttonActionName(action),
                          spectreButtonActionLabel(action) ? spectreButtonActionLabel(action) : "-",
                          handled ? 1 : 0);

            if (handled || route.listActive || route.missionListActive ||
                route.badUsbListActive || route.debriefActive) {
                continue;
            }
        }

        char huntBssid[18] = "";
        bool huntReq = false;
        STATE_READ_BEGIN();
        huntReq = g_state.wifiHuntRequest;
        if (huntReq) {
            strlcpy(huntBssid, g_state.wifiHuntBSSID,
                    sizeof(huntBssid));
            g_state.wifiHuntRequest = false;
        }
        STATE_READ_END();
        if (huntReq && huntBssid[0]) {
            if (!RADIO_ARB.requestPmkidHunt(huntBssid, 30000UL, "pmkid_hunt")) {
                DLOG_WARN("WIFI", "PMKID hunt lease denied target=%s", huntBssid);
            }
        }

        STATE_WRITE_BEGIN();
        g_state.uptimeMs = millis();
        STATE_WRITE_END();

        TIME_SVC.tick();

        if (BLE_MGR.consumeWireGuardDumpTrigger()) {
            MQTT_MGR.requestDump(true);
            DLOG_INFO("BLE", "WireGuard dump triggered");
        }

        static uint32_t lastUploadCheck = 0;
        if (millis() - lastUploadCheck > 60000) {
            lastUploadCheck = millis();

            if (MQTT_MGR.uploadReadyCount() >= MQTT_UPLOAD_READY_THRESHOLD) {
                MQTT_MGR.requestDump(false);
            }

            if (MQTT_MGR.lastDumpAge() > MQTT_DUMP_INTERVAL_MS &&
                MQTT_MGR.queueDepth() > 0) {
                MQTT_MGR.requestDump(false);
            }
        }

        char inputBuf[64] = "";
        if (BLE_MGR.consumeTextInput(inputBuf, sizeof(inputBuf))) {
            _markUiActivity();
            char prompt[24] = "";
            STATE_READ_BEGIN();
            strlcpy(prompt, g_state.textInputPrompt, sizeof(prompt));
            STATE_READ_END();

            if (strncmp(prompt, "Tag this session:", 17) == 0) {
                STATE_WRITE_BEGIN();
                strlcpy(g_state.sessionTag, inputBuf, sizeof(g_state.sessionTag));
                g_state.sessionTagSet = true;
                STATE_WRITE_END();
                DLOG_INFO("TAG", "Manual tag: %s", inputBuf);
            } else if (strncmp(prompt, "Save location:", 14) == 0) {
                float lat = 0.0f;
                float lon = 0.0f;
                bool gpsOk = false;
                int count = 0;
                STATE_READ_BEGIN();
                gpsOk = g_state.gpsAvailable;
                lat = g_state.gpsLat;
                lon = g_state.gpsLon;
                count = g_state.knownLocCount;
                STATE_READ_END();

                if (gpsOk && count < SpectreState::KNOWN_LOC_COUNT) {
                    SpectreState::KnownLocation locations[SpectreState::KNOWN_LOC_COUNT] = {};
                    int newCount = 0;
                    STATE_WRITE_BEGIN();
                    strlcpy(g_state.knownLocations[count].tag,
                            inputBuf, sizeof(g_state.knownLocations[count].tag));
                    g_state.knownLocations[count].lat     = lat;
                    g_state.knownLocations[count].lon     = lon;
                    g_state.knownLocations[count].radiusM = 75.0f;
                    g_state.knownLocCount++;
                    newCount = g_state.knownLocCount;
                    memcpy(locations, g_state.knownLocations, sizeof(locations));
                    STATE_WRITE_END();

                    STORAGE.saveKnownLocations(locations, newCount);
                    DLOG_INFO("TAG", "Saved location: %s", inputBuf);
                }
            } else {
                STATE_WRITE_BEGIN();
                strlcpy(g_state.wifiConnectPass,
                        inputBuf, sizeof(g_state.wifiConnectPass));
                STATE_WRITE_END();
            }
        }

        uint32_t gpsFix = 0;
        STATE_READ_BEGIN();
        gpsFix = g_state.gpsLastFix;
        STATE_READ_END();
        if (gpsFix != 0 && gpsFix != lastGpsFixSeen) {
            lastGpsFixSeen = gpsFix;
            _checkLocationTag();
        }

        if (!companion.enabled) {
            // Companion disabled: discard any console-queued requests so they
            // can't fire if the feature is later re-enabled at runtime.
            g_companionCmd.probe  = false;
            g_companionCmd.enrich = false;
            g_companionCmd.cancel = false;
            companion.workState = COMPANION_WORK_IDLE;
            companion.enrichmentRequestIssued = false;
            companion.manualProbeRequested = false;
            companion.manualEnrichRequested = false;
            companion.offloadPrepRequested = false;
            companion.timeSyncRequested = false;
        } else {
            // Drain console-issued companion commands into the scheduler.
            // The console (also running on TaskHardware via _pollUsbSerialConsole)
            // writes g_companionCmd.{probe,enrich,cancel}; we read-and-clear here
            // so the priority flags are visible to companionHasPriorityReason()
            // for the rest of this same tick. Cancel is processed first so a
            // simultaneously-queued probe/enrich is also wiped.
            if (g_companionCmd.cancel) {
                g_companionCmd.cancel = false;
                g_companionCmd.probe  = false;
                g_companionCmd.enrich = false;
                if (companion.manualProbeRequested ||
                    companion.manualEnrichRequested ||
                    companion.offloadPrepRequested ||
                    companion.timeSyncRequested) {
                    DLOG_INFO("COMP",
                              "Cancel: clearing pending companion requests");
                }
                companion.manualProbeRequested  = false;
                companion.manualEnrichRequested = false;
                companion.offloadPrepRequested  = false;
                companion.timeSyncRequested     = false;
            }
            if (g_companionCmd.probe) {
                g_companionCmd.probe = false;
                companion.manualProbeRequested = true;
                companion.nextProbeMs = 0;
                companion.probeBackoffStage = 0;
                companion.probeBackoffMissCount = 0;
                DLOG_INFO("COMP",
                          "Manual probe request received; probe backoff bypassed");
            }
            if (g_companionCmd.enrich) {
                g_companionCmd.enrich = false;
                companion.manualEnrichRequested = true;
                companion.nextProbeMs = 0;
                companion.probeBackoffStage = 0;
                companion.probeBackoffMissCount = 0;
                DLOG_INFO("COMP",
                          "Manual enrich request received; probe backoff bypassed");
            }

            // Track recent high-value WiFi activity.
            // Keep this simple for now: if WiFi owns the radio, treat that as active.
            if (isHighValueWiFiOwner(RADIO_ARB.currentOwner())) {
                companion.lastHighValueWifiMs = millis();
            }

            if (BLE_MGR.isPhoneCompanionReady()) {
                companion.phoneState = COMPANION_PHONE_AVAILABLE;
                companion.lastSeenMs = millis();
            } else if (companionPhoneAvailabilityStale(companion)) {
                companion.phoneState = COMPANION_PHONE_UNKNOWN;
                DLOG_INFO("BLE", "Phone availability expired");
            }

            // Refresh pending enrichment backlog from the enrich-specific
            // spool scan. Upload backlog includes already-enriched records,
            // which causes pointless BLE leases after enrichment catches up.
            refreshCompanionPending(companion, false);

            if (companion.workState == COMPANION_WORK_IDLE &&
                RADIO_ARB.isOwner(RADIO_BLE_GPS) &&
                BLE_MGR.isPhoneCompanionReady() &&
                (companion.manualEnrichRequested ||
                 companion.offloadPrepRequested ||
                 companion.timeSyncRequested)) {
                RADIO_ARB.refreshLease(RADIO_BLE_GPS,
                                       RadioArbiter::BLE_PHONE_ENRICH_HOLD_MS,
                                       "linked_manual_enrich");
                companion.workState = COMPANION_WORK_ENRICHING;
                resetEnrichmentSessionStats(companion);
                companion.enrichmentRequestIssued = false;
                companion.lastRequestedEnrichmentCount = 0;
                crashCheckpoint(CrashPhase::BACKLOG_ENRICH,
                                static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                                static_cast<uint32_t>(companion.pendingItems));
                DLOG_INFO("BLE",
                          "Active BLE link promoted to enrichment pendingEnrich=%lu manual=%u offload=%u timeSync=%u",
                          static_cast<unsigned long>(companion.pendingItems),
                          companion.manualEnrichRequested ? 1u : 0u,
                          companion.offloadPrepRequested ? 1u : 0u,
                          companion.timeSyncRequested ? 1u : 0u);
            }

            const bool companionProbeWorkPending =
                companion.pendingItems >= PHONE_COMPANION_ENRICH_THRESHOLD ||
                companion.manualEnrichRequested ||
                companion.offloadPrepRequested ||
                companion.timeSyncRequested;

            // Opportunistic probe if work exists but phone is not known available yet.
            if (companion.phoneState != COMPANION_PHONE_AVAILABLE &&
                companion.workState == COMPANION_WORK_IDLE &&
                companionProbeWorkPending) {

                if (shouldRunPhoneProbe(companion)) {
                    requestPhoneProbeLease(
                        companion,
                        companion.offloadPrepRequested ? PHONE_PROBE_OFFLOAD_PREP :
                        companion.manualEnrichRequested ? PHONE_PROBE_MANUAL :
                        companion.timeSyncRequested ? PHONE_PROBE_TIME_SYNC :
                                                      PHONE_PROBE_BACKLOG
                    );
                }
            }

            // Field Mode on the phone may already be advertising before
            // Spectre has enrichment work.  Keep probing lightly so the
            // link becomes available as soon as the two radios overlap.
            if (companion.phoneState != COMPANION_PHONE_AVAILABLE &&
                companion.workState == COMPANION_WORK_IDLE &&
                !companionProbeWorkPending &&
                shouldRunPhoneProbe(companion)) {
                requestPhoneProbeLease(companion, PHONE_PROBE_AVAILABILITY);
            }

            // Start enrichment only after phone is known available and WiFi is in a lull.
            if (shouldRunEnrichment(companion)) {
                requestPhoneEnrichmentLease(
                    companion,
                    companion.offloadPrepRequested ? "offload_enrich" :
                    companion.manualEnrichRequested ? "manual_enrich" :
                    companion.timeSyncRequested ? "time_sync_enrich" :
                                                  "backlog_enrich"
                );
            }
        }

        uint32_t wifiTickMs = 100;
        if (RADIO_ARB.isOwner(RADIO_WIFI_CAPTURE) ||
    RADIO_ARB.isOwner(RADIO_WIFI_PMKID)) {
    wifiTickMs = 25;
        } else if (RADIO_ARB.isOwner(RADIO_WIFI_SCAN)) {
    wifiTickMs = 50;
        }

        if (millis() - lastWifiTick >= wifiTickMs) {
            if (ExecutionPolicy::shouldTickWiFi(RADIO_ARB.currentOwner())) {
                WIFI_MGR.tick();
            }
            lastWifiTick = millis();
        }

        static uint32_t lastBleTick = 0;
        uint32_t bleTickMs = 200;

        if (RADIO_ARB.isOwner(RADIO_BLE_TEXT)) {
    bleTickMs = 100;
        } else if (RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
    bleTickMs = 150;
        }

        if (millis() - lastBleTick >= bleTickMs) {
            if (ExecutionPolicy::shouldTickBle(RADIO_ARB.currentOwner())) {
                BLE_MGR.tick();
            }
            lastBleTick = millis();
        }

        if (companion.enabled) {
            if (companion.workState == COMPANION_WORK_PROBING) {
                if (BLE_MGR.isPhoneCompanionReady()) {
                    companion.phoneState = COMPANION_PHONE_AVAILABLE;
                    companion.lastSeenMs = millis();
                    companion.nextProbeMs = 0;
                    companion.probeBackoffStage    = 0;
                    companion.probeBackoffMissCount = 0;
                    companion.manualProbeRequested = false;
                    refreshCompanionPending(companion, true);
                    publishPhoneStorageSnapshotIfDue(companion, true);
                    crashBreadcrumbClear(CrashPhase::BACKLOG_PROBE);
                    DLOG_INFO("BLE", "Phone probe succeeded — backoff reset");

                    if (companionHasEnrichmentWork(companion) &&
                        RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
                        companion.workState = COMPANION_WORK_ENRICHING;
                        resetEnrichmentSessionStats(companion);
                        companion.enrichmentRequestIssued = false;
                        companion.lastRequestedEnrichmentCount = 0;
                        crashCheckpoint(CrashPhase::BACKLOG_ENRICH,
                                        static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                                        static_cast<uint32_t>(companion.pendingItems));
                        DLOG_INFO("BLE",
                                  "Phone probe promoted to enrichment pendingEnrich=%lu maxBatch=%u",
                                  static_cast<unsigned long>(companion.pendingItems),
                                  static_cast<unsigned>(PHONE_ENRICH_BATCH_MAX));
                    } else {
                        companion.workState = COMPANION_WORK_IDLE;
                    }

                    if (companion.workState == COMPANION_WORK_IDLE &&
                        RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
                        RADIO_ARB.release(RADIO_BLE_GPS, "probe_success");
                    }
                } else if (!RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
                    companion.phoneState = COMPANION_PHONE_UNAVAILABLE;
                    companion.workState = COMPANION_WORK_IDLE;
                    crashBreadcrumbClear(CrashPhase::BACKLOG_PROBE);

                    // Advance backoff stage if we've exhausted this stage's
                    // miss budget.  Stage 3 is the indefinite floor.
                    companion.probeBackoffMissCount++;
                    const uint8_t missLimit =
                        PROBE_BACKOFF_MISS_LIMIT[companion.probeBackoffStage];
                    if (missLimit > 0 &&
                        companion.probeBackoffMissCount >= missLimit &&
                        companion.probeBackoffStage < PROBE_BACKOFF_STAGES - 1) {
                        companion.probeBackoffStage++;
                        companion.probeBackoffMissCount = 0;
                    }
                    const uint32_t interval =
                        PROBE_BACKOFF_INTERVAL_MS[companion.probeBackoffStage];
                    companion.nextProbeMs = millis() + interval;

                    // Compact probe summary — one line regardless of failure mode.
                    const auto authFail = BLE_MGR.getLastAuthFailReason();
                    if (authFail != BLEManager::BleAuthFailReason::NONE) {
                        DLOG_WARN("BLE",
                                  "probe_summary seen=0 result=auth_fail=%u stage=%u miss=%u nextIn=%lus",
                                  static_cast<unsigned>(authFail),
                                  static_cast<unsigned>(companion.probeBackoffStage),
                                  static_cast<unsigned>(companion.probeBackoffMissCount),
                                  static_cast<unsigned long>(interval / 1000UL));
                    } else {
                        DLOG_WARN("BLE",
                                  "probe_summary seen=0 result=not_seen stage=%u miss=%u nextIn=%lus",
                                  static_cast<unsigned>(companion.probeBackoffStage),
                                  static_cast<unsigned>(companion.probeBackoffMissCount),
                                  static_cast<unsigned long>(interval / 1000UL));
                    }
                }
            }

            if (companion.workState == COMPANION_WORK_ENRICHING) {
                serviceEnrichmentPipeline(companion);
            }
        }

        bool textPending = false;
        STATE_READ_BEGIN();
        textPending = g_state.textInputPending;
        STATE_READ_END();
        if (textPending && !lastTextPending) {
            _markUiActivity();
        }

        if (RADIO_ARB.isOwner(RADIO_BLE_TEXT)) {
            if (textPending) {
                RADIO_ARB.refreshLease(
                    RADIO_BLE_TEXT,
                    RadioArbiter::BLE_TEXT_ACTIVE_HOLD_MS,
                    "text active");
            } else if (lastTextPending) {
                RADIO_ARB.refreshLease(
                    RADIO_BLE_TEXT,
                    RadioArbiter::BLE_TEXT_IDLE_HOLD_MS,
                    "text idle");
            }
        }
        lastTextPending = textPending;

        if (RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD)) {
            MQTT_MGR.tick();
        }

        RADIO_ARB.tick();

        syncRuntimePresentation();

        const uint32_t now = millis();
        const ExecutionPolicy::UiRefreshSchedule uiRefreshSchedule =
            _uiRefreshScheduleForPowerState(power.state);
        uiRefreshMarks.wifiMs = lastWifiRefresh;
        uiRefreshMarks.pwnyMs = lastPwnyRefresh;
        uiRefreshMarks.systemMs = lastSystemRefresh;
        _servicePeriodicUiRefresh(_readUiRefreshState(),
                                  now,
                                  uiRefreshMarks,
                                  uiRefreshSchedule);
        lastWifiRefresh = uiRefreshMarks.wifiMs;
        lastPwnyRefresh = uiRefreshMarks.pwnyMs;
        lastSystemRefresh = uiRefreshMarks.systemMs;

        if (storageOk &&
            STORAGE.hasStorageMaintenanceWork() &&
            canRunStorageMaintenance(companion, power, now)) {
            const uint32_t maintStartMs = millis();
            STORAGE.serviceStorageMaintenanceStep(REPAIR_BUDGET_MS, REPAIR_MAX_RECORDS);
            const uint32_t maintMs = millis() - maintStartMs;
            if (maintMs >= 25UL) {
                DLOG_WARN("STORAGE",
                          "maintenance step slow ms=%lu owner=%s pendingUpload=%lu",
                          static_cast<unsigned long>(maintMs),
                          RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                          static_cast<unsigned long>(STORAGE.getPendingEventCount()));
            }
        }

        _checkRuntimeContracts();

        if (now - lastStackLogMs >= STACK_LOG_INTERVAL_MS) {
            const UBaseType_t freeWords = uxTaskGetStackHighWaterMark(nullptr);
            if (freeWords < minStackWords) {
                minStackWords = freeWords;
            }
            DLOG_INFO("STACK", "TaskHardware watermark=%luB min=%luB",
                      (unsigned long)(freeWords * sizeof(StackType_t)),
                      (unsigned long)(minStackWords * sizeof(StackType_t)));
            lastStackLogMs = now;
        }
        _logRuntimeHealth(now);

        vTaskDelay(10);
    }
}

void _checkLocationTag() {
    float lat = 0.0f;
    float lon = 0.0f;
    bool gpsOk = false;
    int count = 0;
    bool alreadyTagged = false;
    SpectreState::KnownLocation knownLocs[SpectreState::KNOWN_LOC_COUNT] = {};
    STATE_READ_BEGIN();
    gpsOk = g_state.gpsAvailable;
    lat   = g_state.gpsLat;
    lon   = g_state.gpsLon;
    count = g_state.knownLocCount;
    if (count > SpectreState::KNOWN_LOC_COUNT) {
        count = SpectreState::KNOWN_LOC_COUNT;
    }
    if (count > 0) {
        memcpy(knownLocs, g_state.knownLocations,
               static_cast<size_t>(count) * sizeof(SpectreState::KnownLocation));
    }
    alreadyTagged = g_state.sessionTagSet;
    STATE_READ_END();

    if (!gpsOk || count <= 0 || alreadyTagged) return;

    for (int i = 0; i < count; i++) {
        const SpectreState::KnownLocation& loc = knownLocs[i];

        float dlat = (lat - loc.lat) * 111320.0f;
        float dlon = (lon - loc.lon) * 111320.0f * cosf(lat * 0.01745329f);
        float dist = sqrtf(dlat * dlat + dlon * dlon);

        if (dist <= loc.radiusM) {
            STATE_WRITE_BEGIN();
            if (!g_state.sessionTagSet) {
                strlcpy(g_state.sessionTag,
                        loc.tag,
                        sizeof(g_state.sessionTag));
                g_state.sessionTagSet = true;
                alreadyTagged = true;
            }
            STATE_WRITE_END();

            if (alreadyTagged) {
                DLOG_INFO("GPS", "Auto-tag: %s (%.0fm)", loc.tag, dist);

                char notifText[48];
                snprintf(notifText, sizeof(notifText),
                         "LOCATION: %s", loc.tag);
                _queueNotification(NOTIF_HOMELAB_SYNC, notifText);
            }
            return;
        }
    }
}

// ── Setup ──

static const char* _resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_EXT:       return "ext_pin";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "other_wdt";
        case ESP_RST_DEEPSLEEP: return "deep_sleep_wake";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

static const char* _fieldVaultPowerSourceName(PowerSource source) {
    switch (source) {
        case POWER_SOURCE_BATTERY: return "battery";
        case POWER_SOURCE_USB:     return "usb";
        case POWER_SOURCE_UNKNOWN:
        default:                   return "unknown";
    }
}

static bool _detectBootRecoveryRequest() {
#if BOOT_RECOVERY_ENABLED
    pinMode(BOOT_RECOVERY_BUTTON_PIN, INPUT_PULLUP);
    const uint32_t startMs = millis();
    bool announced = false;

    while (millis() - startMs < BOOT_RECOVERY_HOLD_MS) {
        if (digitalRead(BOOT_RECOVERY_BUTTON_PIN) != LOW) {
            return false;
        }
        if (!announced && millis() - startMs > 250UL) {
            Serial.println("[BOOT] recovery button held; keep holding for recovery");
            announced = true;
        }
        delay(25);
    }

    Serial.println("[BOOT] recovery mode requested");
    return true;
#else
    return false;
#endif
}

void setup() {
    Serial.begin(115200);
    delay(250);
    g_usbSerialAttachedAtBoot = static_cast<bool>(Serial);

    // Apply the compile-time debug profile before any DLOG_* call so disabled
    // logs cost nothing from boot onward.
    DebugLog::applyProfile(static_cast<DebugProfile>(SPECTRE_DEBUG_PROFILE),
                           kDebugSubsystemMask);

    // Bring up the physical UI before touching NVS or LittleFS. If a prior run
    // left storage in a bad state, the user still gets a visible recovery path.
    pinMode(LCD_POWER, OUTPUT);
    digitalWrite(LCD_POWER, HIGH);
    delay(100);
    pinMode(LCD_BL, OUTPUT);
    analogWriteResolution(LCD_BL, 8);
    analogWrite(LCD_BL, 255);
    buttons.begin();
    _markUiActivity();

    tft.init();
    tft.setRotation(3);
    tft.fillScreen(0x0000);

    g_bootRecoveryMode = _detectBootRecoveryRequest();
    if (g_bootRecoveryMode) {
        PrebootFallback::showFatal(tft, "RECOVERY MODE", "USB FLASH READY");
    }

    // ── Boot diagnostics — runs before any manager initializes ───────────────
    if (!g_bootRecoveryMode) {
        const esp_reset_reason_t rr = esp_reset_reason();
        Serial.printf("[BOOT] reset_reason=%d (%s)\n", (int)rr, _resetReasonName(rr));
        DLOG_WARN("CORE", "reset_reason=%d (%s)", (int)rr, _resetReasonName(rr));

        if (rr == ESP_RST_PANIC) {
            Serial.printf("[BOOT] *** PANIC RESET — check serial backlog for abort/assert ***\n");
            DLOG_WARN("CORE", "panic reset — see prior serial output for exception details");
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
            // Best-effort: requires a coredump partition in partitions.csv.
            // esp_core_dump.h is included at the top of this file under the
            // same guard. Silently skipped if summary is unavailable.
            esp_core_dump_summary_t cd = {};
            if (esp_core_dump_get_summary(&cd) == ESP_OK) {
                Serial.printf("[BOOT] panic task=%s pc=0x%08lx\n",
                              cd.exc_task,
                              static_cast<unsigned long>(cd.exc_pc));
                DLOG_WARN("CORE", "panic task=%s pc=0x%08lx",
                          cd.exc_task,
                          static_cast<unsigned long>(cd.exc_pc));
            }
#endif
        }

        if (rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT) {
            Serial.printf("[BOOT] *** WATCHDOG RESET — task stall on TaskHardware or TaskDisplay ***\n");
            DLOG_WARN("CORE", "watchdog reset — check for blocking call or priority inversion");
        }

        if (rr == ESP_RST_BROWNOUT) {
            Serial.printf("[BOOT] *** BROWNOUT — rail drooped during radio/flash activity ***\n");
            DLOG_WARN("CORE", "brownout reset — check flash write during active radio window");
        }

        // RTC crash ring — last CRASH_LOG_DEPTH checkpoints survive across resets.
        // Entries persist until overwritten; connect any time after a crash.
        crashLogPrint();
    }
    // ── end boot diagnostics ─────────────────────────────────────────────────

    if (!g_bootRecoveryMode) {
        const bool settingsOk = SETTINGS.begin();
        if (!settingsOk) {
            DLOG_WARN("SETTINGS",
                      "Settings unavailable in setup; using fallback USB serial policy");
        }
        DLOG_INFO("SYS", "Booting");
    } else {
        DebugLog::configureUsbSerial(true, DEBUG_LEVEL_INFO, DEBUG_AREA_OPERATORS);
        DLOG_WARN("SYS", "Booting into recovery mode");
    }

    _initCoreLoadMonitor();

    DLOG_INFO("SYS", "Starting tasks");

    xTaskCreatePinnedToCore(
        TaskDisplay, "TaskDisplay",
        TASK_DISPLAY_STACK_BYTES, nullptr, 2,
        &taskDisplayHandle, 1);

    xTaskCreatePinnedToCore(
        TaskHardware, "TaskHardware",
        TASK_HARDWARE_STACK_BYTES, nullptr, 2,
        &taskHardwareHandle, 0);

    DLOG_INFO("SYS", "Tasks launched");
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
