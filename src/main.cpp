


#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_memory_utils.h>
#include <esp_system.h>
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
#include "core/BootInfo.h"
#include "core/CrashBreadcrumb.h"
#include "core/TaskStackAudit.h"
#include "core/PsramObject.h"
#include <esp_bt.h>
#include "core/EventBus.h"
#include "core/MissionRuntime.h"
#include "core/ExecutionPolicy.h"
#include "core/Session.h"
#include "core/SpectreState.h"
#include "core/NotifTypes.h"
#include "core/RuntimeContracts.h"
#include "core/ScreenInfo.h"
#include "core/ScreenNavigation.h"
#include "core/StorageExclusiveWindow.h"
#include "core/StorageUiMirror.h"
#include "managers/ButtonHandler.h"
#include "managers/DisplayManager.h"
#include "managers/EntityManager.h"
#include "managers/ExportManager.h"
#include "managers/LoRaManager.h"
#include "managers/BadUsbManager.h"
#include "managers/SubGhzManager.h"
#include "managers/ReyaxBackend.h"
#include "managers/WioSx1262Backend.h"
#include "managers/MeshtasticManager.h"
#include "managers/SubGhzRecordWriter.h"
#include "managers/StorageManager.h"
#include "managers/RAMSpool.h"
#include "managers/MQTTManager.h"
#include "managers/PowerManager.h"
#include "managers/WiFiManager.h"
#include "protocol/CompanionProtocol.h"
#include "managers/BLEManager.h"
#include "managers/DashboardStreamer.h"
#include "managers/LogStreamer.h"
#include "managers/NotificationCenter.h"
#include "managers/PhoneOffloadManager.h"
#include "managers/PhoneTransportRouter.h"
#include "managers/RadioArbiter.h"
#include "managers/WioNrfAccessory.h"
#include "managers/SettingsManager.h"
#include "managers/TimeService.h"
#include "core/DebugLog.h"
#include "data/FieldVault.h"
#include "ui/BootSequence.h"
#include "ui/LVGLDriver.h"
#include "ui/Theme.h"
#include "ui/PrebootFallback.h"

// setup() peaks at 2044 bytes on the normal boot path. The early field-link
// AP resume also runs here, so retain more than 3 KB of additional headroom
// while avoiding an otherwise permanently parked 8 KB framework stack.
// Measured peak under load is 2164 B; 4096 left only 1932 B of margin, which
// is under this project's 2 KB floor for a task that runs application code.
static constexpr size_t SPECTRE_LOOP_TASK_STACK_BYTES = 5120;  // peak 2164B + ~3KB
SET_LOOP_TASK_STACK_SIZE(SPECTRE_LOOP_TASK_STACK_BYTES);

// ── Hardware objects ──
TFT_eSPI        tft = TFT_eSPI();
ButtonHandler   buttons;
DisplayManager  display;
LoRaManager     lora;
ReyaxBackend    subghzReyax(lora);
WioSx1262Backend subghzWioSx1262(WIO_NRF);

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
    constexpr uint32_t BUTTON_POLL_INTERVAL_MS = 10;
    constexpr UBaseType_t BUTTON_EVENT_QUEUE_DEPTH = 8;

    constexpr size_t USB_CONSOLE_BUF_SIZE = 128;
    char g_usbConsoleBuf[USB_CONSOLE_BUF_SIZE] = {};
    size_t g_usbConsoleLen = 0;
    bool g_usbSerialAttachedAtBoot = false;
    bool g_bootRecoveryMode = false;
    bool g_bootHeapTriageMode = false;

    constexpr uint32_t BOOT_TRIAGE_PENDING_UPLOAD_THRESHOLD = 10000UL;
    constexpr uint32_t BOOT_TRIAGE_FREE_INTERNAL_HEAP_BYTES = 128UL * 1024UL;
    constexpr uint32_t BOOT_TRIAGE_LARGEST_INTERNAL_HEAP_BYTES = 48UL * 1024UL;
    constexpr uint32_t DEFERRED_BOOT_OPTIONAL_INIT_DELAY_MS = 2000UL;
    bool g_deferredBootOptionalInitPending = false;
    uint32_t g_deferredBootOptionalInitAtMs = 0;

    struct DisplayFrameState {
    Screen      currentScreen = DEFAULT_GENERAL_SCREEN;
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
    bool        meshAvailable = false;
    bool        meshEnabled = false;
    int         meshNodeCount = 0;
    uint32_t    meshRxText = 0;
    uint32_t    meshTxText = 0;
    uint32_t    meshNodeNum = 0;
    uint32_t    meshLastFrom = 0;
    char        meshLastText[64] = "";
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

    struct DisplayPowerState {
    bool     requestSleep = false;
    bool     textInputPending = false;
    bool     wifiListActive = false;
    bool     missionListActive = false;
    bool     badUsbListActive = false;
    bool     debriefActive = false;
    bool     badUsbArmed = false;
    bool     badUsbRunning = false;
    uint8_t  powerState = POWER_STATE_BATTERY_NORMAL;
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
    Screen currentScreen = DEFAULT_GENERAL_SCREEN;
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
        Screen currentScreen = DEFAULT_GENERAL_SCREEN;
        bool debriefActive = false;
        uint8_t activeMissionProfile = static_cast<uint8_t>(MISSION_RECON);
    };
}

// ── Task handles ──
TaskHandle_t taskDisplayHandle  = nullptr;
TaskHandle_t taskHardwareHandle = nullptr;
TaskHandle_t taskButtonHandle   = nullptr;
extern TaskHandle_t loopTaskHandle;

StaticQueue_t s_buttonEventQueueStorage;
uint8_t s_buttonEventQueueBuffer[BUTTON_EVENT_QUEUE_DEPTH * sizeof(ButtonEvent)] = {};
QueueHandle_t s_buttonEventQueue = nullptr;


// Stack high-water marks (2026-08-15 field return): TaskHardware fell to
// 2.5 KB free with a 14 KB stack during BLE/offload scheduling. Restore enough
// headroom for the deepest authenticated handoff path.
// FreeRTOS reports the minimum free stack ever observed for the task; this
// watermark can fall after deep MQTT/WiFi call paths and will not rebound.
// Keep 6-8 KB above the measured peak while returning scarce internal SRAM.
// Revisit if min_free ever drops below 4 KB on either task.
// REVERTED to 10240 after the 2026-08-19 field panic. The 8192 trim left only
// 3 KB of margin on the bench (core1=3KB/8KB) - under this project's 4 KB
// guard-rail - and the field crash was a DoubleException with a destroyed
// stack surfacing in TaskButtons, whose TCB sits directly below this task's.
// The 2 KB saved is meaningless now that the BLE teardown returns 68 KB.
static constexpr uint32_t TASK_DISPLAY_STACK_BYTES  = 10240;
static constexpr uint32_t TASK_HARDWARE_STACK_BYTES = 15360;  // peak 11760B + 3.5KB
static constexpr uint32_t TASK_BUTTON_STACK_BYTES   = 4096;
static constexpr uint32_t STACK_LOG_INTERVAL_MS     = 30000UL;
static constexpr uint32_t HEALTH_LOG_INTERVAL_MS    = 30000UL;
static constexpr uint32_t HEAP_CHECK_INTERVAL_MS    = 120000UL;
static constexpr uint32_t FIELDVAULT_POWER_SAMPLE_INTERVAL_MS = 15UL * 60UL * 1000UL;
static constexpr uint32_t FIELDVAULT_RUN_SAMPLE_INTERVAL_MS = 5UL * 60UL * 1000UL;
static constexpr uint32_t FIELDVAULT_RUN_TRANSITION_MIN_MS = 60UL * 1000UL;
static constexpr uint32_t FIELDVAULT_RUN_PENDING_DELTA_MIN = 25UL;
static constexpr uint32_t SLEEP_PRESENTATION_HOLD_MS = 1200UL;
static constexpr uint32_t SLEEP_PRESENTATION_TIMEOUT_MS = 3000UL;
static constexpr uint32_t DISPLAY_ASLEEP_POLL_MS    = 50UL;
static portMUX_TYPE s_displayPowerMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_lastUiActivityMs = 0;
static volatile bool s_displayAwake = true;
static volatile bool s_displayResumePending = false;
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
    bool applyBacklight = false;
    portENTER_CRITICAL(&s_displayPowerMux);
    if (s_displayAwake != awake) {
        s_displayAwake = awake;
        s_displayResumePending = awake;
        changed = true;
        applyBacklight = !awake;
    }
    portEXIT_CRITICAL(&s_displayPowerMux);

    if (changed && applyBacklight) {
        analogWrite(LCD_BL, 0);
    }
    return changed;
}

static bool _displayResumePending() {
    bool pending = false;
    portENTER_CRITICAL(&s_displayPowerMux);
    pending = s_displayAwake && s_displayResumePending;
    portEXIT_CRITICAL(&s_displayPowerMux);
    return pending;
}

static void _completeDisplayResume() {
    uint8_t brightnessPct = 0;
    bool applyBacklight = false;

    portENTER_CRITICAL(&s_displayPowerMux);
    if (s_displayAwake) {
        s_displayResumePending = false;
        brightnessPct = s_displayBrightnessPct;
        applyBacklight = true;
    }
    portEXIT_CRITICAL(&s_displayPowerMux);

    if (applyBacklight) {
        const uint8_t pwm =
            static_cast<uint8_t>((static_cast<uint16_t>(brightnessPct) * 255U) / 100U);
        analogWrite(LCD_BL, pwm);
    }
}

static void _setDisplayBrightnessPct(uint8_t brightnessPct) {
    bool awake = false;
    bool resumePending = false;
    uint8_t nextPct = constrain(brightnessPct, static_cast<uint8_t>(0), static_cast<uint8_t>(100));

    portENTER_CRITICAL(&s_displayPowerMux);
    s_displayBrightnessPct = nextPct;
    awake = s_displayAwake;
    resumePending = s_displayResumePending;
    portEXIT_CRITICAL(&s_displayPowerMux);

    if (awake && !resumePending) {
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
void TaskButtons(void* pvParameters);
void TaskDisplay(void* pvParameters);
void TaskHardware(void* pvParameters);
void _checkLocationTag();
void _captureDisplayFrameState(DisplayFrameState& snapshot);
void _peekDisplayPowerState(DisplayPowerState& snapshot);
bool _shouldKeepDisplayAwake(const DisplayPowerState& snapshot,
                             uint32_t now,
                             uint32_t displayTimeoutMs);
bool _shouldKeepDisplayAwake(const DisplayFrameState& snapshot,
                             uint32_t now,
                             uint32_t displayTimeoutMs);
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
// heap_caps_get_minimum_free_size() sums per-region low watermarks taken at
// different times, so with four internal regions it reads far below the true
// global minimum (the IDF header documents this). Track our own by sampling the
// summed current free, which is a real global figure.
static uint32_t g_dramMinObserved = UINT32_MAX;

static inline uint32_t _sampleDramFree() {
    const uint32_t freeNow = heap_caps_get_free_size(SPECTRE_CAP_DRAM);
    if (freeNow < g_dramMinObserved) g_dramMinObserved = freeNow;
    return freeNow;
}

void _logRuntimeHealth(uint32_t nowMs);
void _enforceCaptureHeapGuard(uint32_t nowMs);
void _releaseIdleBleStack(uint32_t nowMs);
void _runBleMemProbe(bool manageRadio);
bool _waitForDisplayLayerReady(uint32_t timeoutMs);
void _publishStorageState(bool storageOk, const String& storageUsed);
static void _refreshStorageCounterMirror();
static void _publishStorageMaintenanceMirror(bool storageReady,
                                             bool running,
                                             bool ranWindow = false,
                                             uint32_t durationMs = 0);
static bool _shouldEnterBootHeapTriage(bool storageOk);
static void _publishBootHeapTriageState(uint32_t pending,
                                        uint32_t freeInternal,
                                        uint32_t largestInternal);
void _loadKnownLocationsIntoState();
static void _clearStorageSummaryMirror();
static bool _refreshStorageSummaryMirror(bool force);
static bool _forceQuickNtp(const char* reason);
static const char* _resetReasonName(esp_reset_reason_t r);
static bool _resetReasonIsCrashLike(esp_reset_reason_t r);
static uint32_t _auditTaskStackPlacement(bool verbose);
static const char* _fieldVaultPowerSourceName(PowerSource source);
static const char* _fieldVaultPowerStateName(PowerState state);
static bool _detectBootRecoveryRequest();
void _applySubGhzStatusToState(const SubGhzStatus& status);
void _appendFieldVaultPowerSample(const PowerSnapshot& power,
                                  uint8_t radioOwner,
                                  const char* reason);
void _appendFieldVaultRunSample(uint8_t radioOwner,
                                const char* reason);
void _applyPowerSnapshotToState(const PowerSnapshot& power);
void _initializeHardwareManagers(uint32_t& lastWifiTick);
void _writeFieldVaultBootRecords();
// Set when the vault was not ready at hardware-ready (deferred boot init under
// heap pressure); the main loop flushes the record once the vault comes up.
static bool g_fieldVaultBootRecordPending = false;
static void _logHardwareSectionIfSlow(const char* section,
                                      uint32_t startMs,
                                      bool storageOk);
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
                                         size_t& outCount,
                                         const char* logTag = "BLE");
static bool _applyPhoneEnrichmentBatch(const PendingEnrichment* records,
                                       size_t count,
                                       uint32_t& outApplied,
                                       uint32_t& outFailed,
                                       uint32_t& outDeferred,
                                       uint32_t& outStorageMs,
                                       const char* logTag = "BLE");
PhoneStorageFrameV1 _buildPhoneStorageFrame();

enum PhoneProbeReason : uint8_t {
    PHONE_PROBE_BACKLOG = 0,
    PHONE_PROBE_OFFLOAD_PREP,
    PHONE_PROBE_MANUAL,
    PHONE_PROBE_TIME_SYNC
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
    bool manualLinkHold = false;         // device BLE page LINK; held until RELEASE
    bool manualEnrichRequested = false;
    bool manualEnrichProbeBypass = false; // one-shot probe bypass for manual enrich
    bool offloadPrepRequested = false;
    bool automaticOffloadHold = false;
    bool automaticOffloadWasActive = false;
    bool automaticOffloadPrepObserved = false;
    uint32_t automaticOffloadDeadlineMs = 0;
    uint32_t automaticOffloadRetryNotBeforeMs = 0;
    bool timeSyncRequested = false;
    bool enrichmentRequestIssued = false;
    size_t lastRequestedEnrichmentCount = 0;
    bool enrichmentWindowActive = false;
    uint32_t enrichmentSessionStartMs = 0;
    uint32_t enrichmentSessionRequested = 0;
    uint32_t enrichmentSessionApplied = 0;
    uint32_t enrichmentSessionFailed = 0;
    uint32_t enrichmentSessionDeferred = 0;
    uint32_t enrichmentSessionBatches = 0;
    uint32_t enrichmentSessionXferMs = 0;
    uint32_t enrichmentSessionStorageMs = 0;
    bool externalTransportActive = false;
    bool manualEnrichExclusiveActive = false;
    bool manualEnrichWorkerPaused = false;
    bool manualEnrichStoppedCapture = false;

    // Manual drain progress is tracked per full enrichment-index window.
    uint32_t lastWalkNoDataRetired = 0;
    uint32_t enrichWindowStartPending = 0;
    bool enrichWindowSnapshotValid = false;
    bool lastWindowProgressed = true;
    // Distinguishes "build next window" from "session fully drained".
    bool manualDrainComplete = false;

    // Phone-deferred records get bounded re-offers before NO_DATA retirement.
    uint8_t manualDeferredPasses = 0;
    bool manualRetireDeferred = false;

    // Probe backoff stays short enough to catch Android Field Mode while carried.
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
// Pending-event counts that trip automatic enrichment.
static constexpr uint32_t ENRICH_PENDING_THRESHOLD_INTERNAL = PHONE_COMPANION_ENRICH_THRESHOLD;
static constexpr uint32_t ENRICH_PENDING_THRESHOLD_WIO = PHONE_COMPANION_ENRICH_THRESHOLD_WIO;

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
// Automatic BLE offload is a bounded handoff, not a persistent link. Android
// gets a short command window and one longer index-preparation window. Failure
// backs off so a stranded backlog cannot churn the radio/reboot watchdog.
static constexpr uint32_t OFFLOAD_COMMAND_WINDOW_MS = 30000UL;
static constexpr uint32_t OFFLOAD_PREP_WINDOW_MS    = 60000UL;
static constexpr uint32_t OFFLOAD_RETRY_BACKOFF_MS  = 5UL * 60000UL;
static constexpr size_t PHONE_ENRICH_BATCH_MAX   = PHONE_COMPANION_ENRICH_BATCH_MAX;
static constexpr size_t ENRICH_QUEUE_DEPTH       = 2;
static constexpr size_t ENRICH_CLAIM_MAX         = ENRICH_QUEUE_DEPTH * PHONE_ENRICH_BATCH_MAX;
// Cap resident enrichment windows to keep internal heap stable under capture.
// 8,192 compact descriptors consume about 128 KiB of PSRAM. This bounds each
// flash scan while keeping a 25k-50k backlog to a small number of windows.
static constexpr size_t ENRICH_MANUAL_WINDOW_MAX = 8192;
static constexpr uint32_t REPAIR_BUDGET_MS       = 2;
static constexpr uint16_t REPAIR_MAX_RECORDS     = 8;
static constexpr uint32_t REPAIR_UI_QUIET_MS     = 500;
static constexpr uint32_t REPAIR_HEAP_FREE_GUARD = 96UL * 1024UL;
static constexpr uint32_t REPAIR_HEAP_BLOCK_GUARD = 32UL * 1024UL;
// Minimum gap between maintenance steps while WIFI_CAPTURE owns the radio.
// Each step is 35-50ms of LittleFS time; 150ms cap gives wifi_tick room.
static constexpr uint32_t REPAIR_WIFI_CAPTURE_GAP_MS = 150UL;
static constexpr uint32_t CAPTURE_MAINTENANCE_MIN_GAP_MS = 5000UL;
static constexpr uint32_t CAPTURE_MAINTENANCE_BUDGET_MS = 120UL;
static uint32_t g_lastMaintenanceRunMs = 0;
static uint32_t g_lastCaptureMaintenanceMs = 0;
static constexpr uint32_t HARDWARE_LOOP_GAP_WARN_MS = 250UL;
static constexpr uint32_t HARDWARE_SECTION_WARN_MS  = 250UL;

struct QueuedEnrichBatch {
    PendingEnrichment records[PHONE_ENRICH_BATCH_MAX];
    size_t    count     = 0;
    uint32_t  queuedMs  = 0;
};

// Keep the queue in internal RAM.  PendingEnrichment batches are copied out of
// the NimBLE receive path and immediately consumed by the flash writer; placing
// this hot handoff buffer in PSRAM caused a reproducible panic between
// consumeEnrichmentBatch() and enqueueEnrichBatch() on the S3.
// ~2.1 KB of internal DRAM as a plain array. The enrichment path runs on the
// hardware task (never an ISR), so PSRAM latency is harmless here.
static QueuedEnrichBatch* enrichQueue = nullptr;
static QueuedEnrichBatch* _enrichQueueStorage() {
    if (!enrichQueue) {
        enrichQueue = static_cast<QueuedEnrichBatch*>(
            allocateManagerStorage(sizeof(QueuedEnrichBatch) * ENRICH_QUEUE_DEPTH));
        // allocateManagerStorage() falls back to internal DRAM, so a null here
        // means both pools are exhausted; there is no safe way to continue.
        configASSERT(enrichQueue);
    }
    return enrichQueue;
}
static size_t    enrichQueueHead  = 0;
static size_t    enrichQueueTail  = 0;
static size_t    enrichQueueSize  = 0;
static uint32_t  enrichClaimedEventIds[ENRICH_CLAIM_MAX];
static size_t    enrichClaimedCount = 0;

static bool companionHasPriorityReason(const CompanionScheduler& cs) {
    return cs.manualProbeRequested ||
           cs.manualEnrichRequested ||
           cs.timeSyncRequested;
}

static bool companionHasProbePriorityReason(const CompanionScheduler& cs) {
    return cs.manualProbeRequested ||
           cs.manualEnrichProbeBypass ||
           cs.offloadPrepRequested ||
           cs.timeSyncRequested;
}

static const char* companionTransportTag(const CompanionScheduler& cs) {
    return cs.externalTransportActive ? "WIO" : "BLE";
}

static bool companionHasEnrichmentWork(const CompanionScheduler& cs) {
    return cs.pendingItems > 0 ||
           cs.manualEnrichRequested ||
           cs.timeSyncRequested;
}

static bool companionHasAutomaticEnrichmentWork(const CompanionScheduler& cs,
                                                uint32_t pendingThreshold) {
    return cs.manualEnrichRequested ||
           cs.timeSyncRequested ||
           cs.pendingItems >= pendingThreshold;
}

static bool companionHasAutomaticOffloadWork(const CompanionScheduler& cs) {
    // Retry a phone handoff even when enrichment is already complete (for
    // example after a reboot or a phone disconnect between the final GPS delta
    // and offload). Without this, upload-only backlog has no reason to reopen
    // the companion link and can remain stranded indefinitely.
    if (cs.pendingItems > 0) {
        return false;
    }
    if (cs.automaticOffloadHold ||
        (cs.automaticOffloadRetryNotBeforeMs != 0 &&
         static_cast<int32_t>(millis() -
                              cs.automaticOffloadRetryNotBeforeMs) < 0)) {
        return false;
    }

    bool storageReady = false;
    uint32_t pendingUpload = 0;
    STATE_READ_BEGIN();
    storageReady = g_state.storageReady;
    pendingUpload =
        g_state.storagePendingUploadMission + g_state.storagePendingUploadNoise;
    STATE_READ_END();
    return storageReady && pendingUpload > 0;
}

static bool automaticCompanionShouldYieldToUpload() {
    if (MQTT_MGR.uploadStoppedBySerial()) {
        return false;
    }
    // Pending enrichment records are also pending upload records. Treating any
    // upload-ready count as higher priority therefore prevents the automatic
    // phone probe from ever starting precisely when localization work exists.
    // Yield only to an upload that has actually acquired the pipeline; idle
    // backlog must be GPS-resolved before it is allowed to leave the device.
    return MQTT_MGR.backlogDrainActive();
}

static void resetEnrichmentSessionStats(CompanionScheduler& cs) {
    cs.enrichmentSessionStartMs = millis();
    cs.enrichmentSessionRequested = 0;
    cs.enrichmentSessionApplied = 0;
    cs.enrichmentSessionFailed = 0;
    cs.enrichmentSessionDeferred = 0;
    cs.enrichmentSessionBatches = 0;
    cs.enrichmentSessionXferMs = 0;
    cs.enrichmentSessionStorageMs = 0;
}

static void beginManualEnrichmentExclusive(CompanionScheduler& cs,
                                           const char* reason) {
    if (!cs.manualEnrichRequested || cs.manualEnrichExclusiveActive) {
        return;
    }

    const char* safeReason = (reason && reason[0])
                                 ? reason
                                 : "manual_enrich_exclusive";
    STORAGE.releaseUploadIndexMemory("manual_enrich_exclusive_start");
    STORAGE.releaseEnrichmentIndexMemory("manual_enrich_exclusive_start");

    if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE) {
        RADIO_ARB.release(RADIO_WIFI_CAPTURE, safeReason, false);
        cs.manualEnrichStoppedCapture = true;
    }

    const bool workerDrainOk = RAMSpool::drainAndPauseWorker(1500UL);
    cs.manualEnrichWorkerPaused = RAMSpool::isWorkerPaused();
    cs.manualEnrichExclusiveActive = true;
    DLOG_INFO("COMP",
              "Manual enrich exclusive start reason=%s workerPaused=%u drained=%u owner=%s",
              safeReason,
              cs.manualEnrichWorkerPaused ? 1U : 0U,
              workerDrainOk ? 1U : 0U,
              RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
}

static void endManualEnrichmentExclusive(CompanionScheduler& cs,
                                         const char* reason) {
    if (!cs.manualEnrichExclusiveActive &&
        !cs.manualEnrichWorkerPaused &&
        !cs.manualEnrichStoppedCapture) {
        return;
    }

    const char* safeReason = (reason && reason[0])
                                 ? reason
                                 : "manual_enrich_exclusive_done";
    if (cs.manualEnrichWorkerPaused) {
        RAMSpool::resumeWorker(true);
        cs.manualEnrichWorkerPaused = false;
    }

    const bool shouldResumeCapture =
        cs.manualEnrichStoppedCapture &&
        RADIO_ARB.currentOwner() == RADIO_NONE;
    cs.manualEnrichStoppedCapture = false;
    cs.manualEnrichExclusiveActive = false;

    if (shouldResumeCapture) {
        RADIO_ARB.ensureDefaultCapture(safeReason);
    }

    DLOG_INFO("COMP",
              "Manual enrich exclusive end reason=%s resumedCapture=%u owner=%s",
              safeReason,
              shouldResumeCapture ? 1U : 0U,
              RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
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
        case RADIO_STORAGE_MAINTENANCE: return "storage_maintenance";
        case RADIO_BLE_TEXT:     return "ble_text";
        case RADIO_BLE_GPS:      return "ble_gps";
        default:                 return "?";
    }
}

// USB console writes commands; TaskHardware drains them and publishes status.
struct CompanionCmd {
    volatile bool link   = false;
    volatile bool probe  = false;
    volatile bool enrich = false;
    volatile bool upload = false;
    volatile bool cancel = false;
};
static CompanionCmd g_companionCmd;

// Exposed for the phone command dispatcher (slice #5).  Flipping this flag
// is the same path the USB console + button handlers use to request a
// manual enrichment pass; the companion scheduler reads-and-clears it on
// the next hardware tick.
void companionRequestEnrichNow() {
    g_companionCmd.enrich = true;
}

// Phone upload commands are acknowledged on the secure BLE channel first,
// then consumed by TaskHardware. This keeps radio teardown and the potentially
// expensive upload-index build out of the command response path.
void companionRequestUploadNow() {
    g_companionCmd.upload = true;
}

// The authenticated phone control channel uses the same scheduler-owned
// cancellation path as USB and the device BLE page. BLEManager only parses
// the transport frame; TaskHardware remains the sole owner that tears down
// the persistent lease and returns the radio to capture.
void companionRequestCancel() {
    g_companionCmd.cancel = true;
}

struct CompanionStatusSnapshot {
    bool enabled        = false;
    bool externalAccessory = false;
    bool externalBleProxy  = false;
    bool externalPhoneConnected = false;
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
    bool manualLinkHold      = false;
    bool automaticOffloadHold = false;
    bool offloadManagerActive = false;
    bool offloadPreparationPending = false;
    uint32_t offloadDeadlineInMs = 0;
    uint32_t offloadRetryInMs = 0;
    uint32_t lastProbeAgeMs       = 0;
    uint32_t lastEnrichAgeMs      = 0;
    RadioOwner radioOwner         = RADIO_NONE;
    uint32_t lastBleInitAgeMs     = 0;
    uint32_t lastScanStartAgeMs   = 0;
    int      lastDisconnectReason = 0;
    uint32_t externalLastSeenAgeMs = 0;
    int      externalPhoneRssi     = 0;
    char     externalPhone[16]     = "";
    bool     externalSx1262Present = false;
    bool     externalTextInputPending = false;
    char     externalLinkState[16] = "";
    PhoneTransportKind transportKind         = PhoneTransportKind::None;
    PhoneTransportKind transportPreviousKind = PhoneTransportKind::None;
    uint32_t           transportLastChangeAgeMs = 0;
    uint32_t           transportTransitions  = 0;
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

static CompanionPhoneState externalCompanionPhoneState() {
    if (!PHONE_XPORT.isWioActive()) {
        return COMPANION_PHONE_UNKNOWN;
    }
    return PHONE_XPORT.isPhoneCompanionReady()
               ? COMPANION_PHONE_AVAILABLE
               : COMPANION_PHONE_UNKNOWN;
}

static void publishCompanionState(const CompanionScheduler& companion) {
    const uint32_t now = millis();
    const bool externalPresent =
#if WIO_NRF_ACCESSORY_ENABLED
        WIO_NRF.available();
#else
        false;
#endif
    const bool externalProxy = PHONE_XPORT.isWioActive();
    const bool externalPhone =
#if WIO_NRF_ACCESSORY_ENABLED
        WIO_NRF.phoneConnected();
#else
        false;
#endif
    const uint32_t externalLastSeen =
#if WIO_NRF_ACCESSORY_ENABLED
        WIO_NRF.lastSeenMs();
#else
        0;
#endif

    CompanionPhoneState phoneState = companion.phoneState;
    CompanionWorkState workState = companion.workState;
    uint32_t lastSeenMs = companion.lastSeenMs;

    if (externalProxy) {
        phoneState = externalCompanionPhoneState();
        workState = companion.workState;
        lastSeenMs = externalLastSeen;
    }

    CompanionStatusSnapshot snapshot = {};
    snapshot.enabled = companion.enabled;
    snapshot.externalAccessory = externalPresent;
    snapshot.externalBleProxy = externalProxy;
    snapshot.externalPhoneConnected = externalPhone;
    snapshot.phoneState = phoneState;
    snapshot.workState = workState;
    snapshot.bleBegun = BLE_MGR.isBegun();
    snapshot.bleRadioEnabled = BLE_MGR.isRadioEnabled();
    snapshot.bleState = BLE_MGR.getState();
    snapshot.phoneLinkReady       = PHONE_XPORT.isPhoneLinkReady();
    snapshot.phoneGpsReady        = PHONE_XPORT.isPhoneGpsReady();
    snapshot.phoneControlReady    = PHONE_XPORT.isPhoneControlReady();
    snapshot.phoneEnrichmentReady = PHONE_XPORT.isPhoneEnrichmentReady();
    snapshot.phoneFreshGps        = PHONE_XPORT.hasFreshGpsFix();
    snapshot.pendingMission = companion.pendingMissionItems;
    snapshot.pendingNoise = companion.pendingNoiseItems;
    snapshot.pendingTotal = companion.pendingItems;
    snapshot.manualLinkHold = companion.manualLinkHold;
    snapshot.automaticOffloadHold = companion.automaticOffloadHold;
    snapshot.offloadManagerActive = PHONE_OFFLOAD.active();
    snapshot.offloadPreparationPending = PHONE_OFFLOAD.preparationPending();
    snapshot.offloadDeadlineInMs =
        companion.automaticOffloadDeadlineMs != 0 &&
        static_cast<int32_t>(companion.automaticOffloadDeadlineMs - now) > 0
            ? companion.automaticOffloadDeadlineMs - now
            : 0;
    snapshot.offloadRetryInMs =
        companion.automaticOffloadRetryNotBeforeMs != 0 &&
        static_cast<int32_t>(companion.automaticOffloadRetryNotBeforeMs - now) > 0
            ? companion.automaticOffloadRetryNotBeforeMs - now
            : 0;
    snapshot.lastProbeAgeMs = companion.lastProbeMs ? now - companion.lastProbeMs : 0;
    snapshot.lastEnrichAgeMs = companion.lastEnrichMs ? now - companion.lastEnrichMs : 0;
    snapshot.radioOwner = RADIO_ARB.currentOwner();
    snapshot.lastBleInitAgeMs = BLE_MGR.getLastBeginMs() ? now - BLE_MGR.getLastBeginMs() : 0;
    snapshot.lastScanStartAgeMs = BLE_MGR.getLastScanStartMs() ? now - BLE_MGR.getLastScanStartMs() : 0;
    snapshot.lastDisconnectReason = BLE_MGR.getLastDisconnectReason();
    snapshot.externalLastSeenAgeMs = externalLastSeen ? now - externalLastSeen : 0;
#if WIO_NRF_ACCESSORY_ENABLED
    snapshot.externalPhoneRssi = WIO_NRF.phoneRssi();
    strlcpy(snapshot.externalPhone, WIO_NRF.phoneState(), sizeof(snapshot.externalPhone));
    snapshot.externalSx1262Present = WIO_NRF.hasSx1262();
    snapshot.externalTextInputPending =
        WIO_NRF.isTextInputPending() || WIO_NRF.isTextInputReady();
    strlcpy(snapshot.externalLinkState, WIO_NRF.linkStateName(),
            sizeof(snapshot.externalLinkState));
#endif
    const PhoneTransportState& xport = PHONE_XPORT.state();
    snapshot.transportKind             = xport.kind;
    snapshot.transportPreviousKind     = xport.previous;
    snapshot.transportLastChangeAgeMs  =
        xport.lastChangeMs ? now - xport.lastChangeMs : 0;
    snapshot.transportTransitions      = xport.transitions;
    g_companionStatus = snapshot;

    const bool bleAdvertising = BLE_MGR.isAdvertising();
    const bool bleInboundConnected = BLE_MGR.isInboundConnected();
    const uint8_t bleAuthFail =
        static_cast<uint8_t>(BLE_MGR.getLastAuthFailReason());
    const uint8_t transportKind = static_cast<uint8_t>(xport.kind);
    const int8_t blePeerRssi = BLE_MGR.getLastTargetRssi();

    STATE_WRITE_BEGIN();
    const bool bleUiChanged =
        g_state.bleRadioEnabled != snapshot.bleRadioEnabled ||
        g_state.bleAdvertising != bleAdvertising ||
        g_state.bleInboundConnected != bleInboundConnected ||
        g_state.bleSecureReady != snapshot.phoneLinkReady ||
        g_state.bleGpsReady != snapshot.phoneGpsReady ||
        g_state.bleEnrichmentReady != snapshot.phoneEnrichmentReady ||
        g_state.bleFreshGps != snapshot.phoneFreshGps ||
        g_state.bleLinkState != static_cast<uint8_t>(snapshot.bleState) ||
        g_state.bleAuthFailReason != bleAuthFail ||
        g_state.phoneTransportKind != transportKind ||
        g_state.blePeerRssi != blePeerRssi ||
        g_state.bleLastDisconnectReason != snapshot.lastDisconnectReason ||
        g_state.companionPhone != static_cast<uint8_t>(phoneState) ||
        g_state.companionWork != static_cast<uint8_t>(workState) ||
        g_state.companionPending != companion.pendingItems;
    g_state.bleRadioEnabled = snapshot.bleRadioEnabled;
    g_state.bleAdvertising = bleAdvertising;
    g_state.bleInboundConnected = bleInboundConnected;
    g_state.bleSecureReady = snapshot.phoneLinkReady;
    g_state.bleGpsReady = snapshot.phoneGpsReady;
    g_state.bleEnrichmentReady = snapshot.phoneEnrichmentReady;
    g_state.bleFreshGps = snapshot.phoneFreshGps;
    g_state.bleLinkState = static_cast<uint8_t>(snapshot.bleState);
    g_state.bleAuthFailReason = bleAuthFail;
    g_state.phoneTransportKind = transportKind;
    g_state.blePeerRssi = blePeerRssi;
    g_state.bleLastDisconnectReason = snapshot.lastDisconnectReason;
    g_state.companionEnabled = companion.enabled ? 1 : 0;
    g_state.companionPhone = static_cast<uint8_t>(phoneState);
    g_state.companionWork = static_cast<uint8_t>(workState);
    g_state.companionPending = companion.pendingItems;
    g_state.companionLastSeenMs = lastSeenMs;
    if (bleUiChanged && g_state.currentScreen == SCREEN_BLE) {
        g_state.dataRefresh = true;
    }
    if (externalProxy) {
        g_state.bleConnected = externalPhone;
        strlcpy(g_state.bleDeviceName,
                externalPhone ? "Wio nRF phone" : "Wio nRF BLE proxy",
                sizeof(g_state.bleDeviceName));
        // BLE_MGR is suspended while WIO owns the BLE radio, so reflect the
        // WIO accessory's text-input state into g_state directly.
        g_state.textInputPending =
            WIO_NRF.isTextInputPending() || WIO_NRF.isTextInputReady();
        g_state.gpsAvailable = WIO_NRF.hasFreshGpsFix();
        if (WIO_NRF.hasFreshGpsFix()) {
            g_state.gpsLat = WIO_NRF.gpsLat();
            g_state.gpsLon = WIO_NRF.gpsLon();
            g_state.gpsAlt = WIO_NRF.gpsAlt();
            g_state.gpsAccuracy = WIO_NRF.gpsAccuracy();
        }
    }
    STATE_WRITE_END();
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

    // Throttle while WIFI_CAPTURE owns the radio. Each step takes 35-50ms
    // of LittleFS time; running them back-to-back monopolizes TaskHardware
    // and starves frame handling. Cap at one step per REPAIR_WIFI_CAPTURE_GAP_MS
    // during capture — still drains repairs in minutes, lets wifi_tick breathe.
    // When idle (RADIO_NONE), no throttle. Caller updates g_lastMaintenanceRunMs
    // after the step completes.
    if (owner == RADIO_WIFI_CAPTURE) {
        if (nowMs - g_lastMaintenanceRunMs < REPAIR_WIFI_CAPTURE_GAP_MS) {
            return false;
        }
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

    STATE_READ_BEGIN();
    mission = g_state.storagePendingEnrichMission;
    noise = g_state.storagePendingEnrichNoise;
    STATE_READ_END();

    // The lane counters are updated incrementally and remain the best-known
    // scheduler input while the expensive full storage summary is marked
    // maintenance-pending during continuous capture. Requiring summaryValid
    // here makes automatic localization wait forever for a radio-idle audit.
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
        owner == RADIO_WIFI_UPLOAD ||
        owner == RADIO_STORAGE_MAINTENANCE;

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

PhoneStorageFrameV1 _buildPhoneStorageFrame() {
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
    if (!cs.enabled) {
        return;
    }

    const bool externalProxy = PHONE_XPORT.isWioActive();

    if (!externalProxy) {
        if (!PHONE_XPORT.isPhoneStorageReady()) {
            return;
        }
        if (!RADIO_ARB.isOwner(RADIO_BLE_GPS) ||
            RADIO_ARB.currentOwner() == RADIO_WIFI_UPLOAD) {
            return;
        }
    } else {
        if (!PHONE_XPORT.isPhoneCompanionReady()) {
            return;
        }
        if (RADIO_ARB.currentOwner() == RADIO_WIFI_UPLOAD) {
            return;
        }
    }

    const uint32_t now = millis();
    if (!force &&
        cs.lastStoragePublishMs != 0 &&
        now - cs.lastStoragePublishMs < PHONE_STORAGE_PUBLISH_MIN_MS) {
        return;
    }

    const PhoneStorageFrameV1 frame = _buildPhoneStorageFrame();
    const bool ok = PHONE_XPORT.publishStorageSnapshot(frame);
    if (ok) {
        cs.lastStoragePublishMs = now;
    }
}

static void finishAutomaticOffloadHold(CompanionScheduler& cs,
                                       const char* reason) {
    if (!cs.automaticOffloadHold) {
        return;
    }

    const bool transferStarted = cs.automaticOffloadWasActive;
    cs.automaticOffloadHold = false;
    cs.automaticOffloadWasActive = false;
    cs.automaticOffloadPrepObserved = false;
    cs.automaticOffloadDeadlineMs = 0;
    cs.automaticOffloadRetryNotBeforeMs = millis() + OFFLOAD_RETRY_BACKOFF_MS;
    cs.offloadPrepRequested = false;
    cs.phoneState = COMPANION_PHONE_UNKNOWN;
    PHONE_OFFLOAD.abandonPreparation(reason);

    DLOG_INFO("OFFLOAD",
              "automatic handoff ended reason=%s started=%u retryIn=%lus",
              reason ? reason : "unknown",
              transferStarted ? 1U : 0U,
              static_cast<unsigned long>(OFFLOAD_RETRY_BACKOFF_MS / 1000UL));

    if (RADIO_ARB.isOwner(RADIO_BLE_GPS) && !cs.manualLinkHold) {
        RADIO_ARB.release(RADIO_BLE_GPS,
                          reason ? reason : "automatic_offload_done");
    }
}

static void deferAutomaticOffloadRecovery(CompanionScheduler& cs,
                                          const char* reason) {
    if (!cs.offloadPrepRequested) {
        return;
    }
    cs.offloadPrepRequested = false;
    cs.automaticOffloadRetryNotBeforeMs = millis() + OFFLOAD_RETRY_BACKOFF_MS;
    DLOG_INFO("OFFLOAD",
              "automatic recovery deferred reason=%s retryIn=%lus",
              reason ? reason : "unknown",
              static_cast<unsigned long>(OFFLOAD_RETRY_BACKOFF_MS / 1000UL));
}

static void beginAutomaticOffloadHold(CompanionScheduler& cs,
                                      const char* reason) {
    if (cs.externalTransportActive || cs.automaticOffloadHold) {
        return;
    }

    const uint32_t now = millis();
    cs.offloadPrepRequested = false;
    cs.automaticOffloadHold = true;
    cs.automaticOffloadWasActive = PHONE_OFFLOAD.active();
    cs.automaticOffloadPrepObserved = false;
    cs.automaticOffloadDeadlineMs = now + OFFLOAD_COMMAND_WINDOW_MS;
    cs.workState = COMPANION_WORK_IDLE;
    publishPhoneStorageSnapshotIfDue(cs, true);

    if (RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
        RADIO_ARB.refreshLease(RADIO_BLE_GPS,
                               RadioArbiter::BLE_PHONE_ENRICH_HOLD_MS,
                               "automatic_offload_handoff");
    }
    DLOG_INFO("OFFLOAD",
              "automatic handoff opened reason=%s commandWindow=%lus active=%u",
              reason ? reason : "unknown",
              static_cast<unsigned long>(OFFLOAD_COMMAND_WINDOW_MS / 1000UL),
              cs.automaticOffloadWasActive ? 1U : 0U);
}

static void serviceAutomaticOffloadHold(CompanionScheduler& cs) {
    if (!cs.automaticOffloadHold) {
        return;
    }

    if (PHONE_OFFLOAD.wifiBulkActive()) {
        return; // the manager now owns the controlled BLE -> Wi-Fi transition
    }

    if (!RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
        finishAutomaticOffloadHold(cs, "automatic_offload_owner_lost");
        return;
    }

    if (PHONE_OFFLOAD.active()) {
        cs.automaticOffloadWasActive = true;
        RADIO_ARB.refreshLease(RADIO_BLE_GPS,
                               RadioArbiter::BLE_PHONE_ENRICH_HOLD_MS,
                               "automatic_offload_active");
        return;
    }

    if (cs.automaticOffloadWasActive) {
        finishAutomaticOffloadHold(cs, "automatic_offload_complete");
        return;
    }

    if (PHONE_OFFLOAD.preparationPending() &&
        !cs.automaticOffloadPrepObserved) {
        cs.automaticOffloadPrepObserved = true;
        cs.automaticOffloadDeadlineMs = millis() + OFFLOAD_PREP_WINDOW_MS;
        DLOG_INFO("OFFLOAD", "index preparation observed; extending handoff window");
    }

    if (static_cast<int32_t>(millis() -
                             cs.automaticOffloadDeadlineMs) >= 0) {
        finishAutomaticOffloadHold(cs, "automatic_offload_timeout");
        return;
    }

    RADIO_ARB.refreshLease(RADIO_BLE_GPS,
                           RadioArbiter::BLE_PHONE_ENRICH_HOLD_MS,
                           "automatic_offload_wait");
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
    const bool priority = companionHasProbePriorityReason(cs);
    if (!priority && automaticCompanionShouldYieldToUpload()) {
        return false;
    }
    // Apply the enrichment cadence before taking the radio, not only after a
    // phone has been found. This prevents stale/rapidly changing mirror counts
    // from spending another BLE probe window during the post-enrich cooldown.
    if (!priority && cs.lastEnrichMs != 0 &&
        millis() - cs.lastEnrichMs < ENRICH_MIN_GAP_MS) {
        return false;
    }
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
    if (!priority && automaticCompanionShouldYieldToUpload()) {
        return false;
    }
    if (!priority && millis() - cs.lastEnrichMs < ENRICH_MIN_GAP_MS) {
        return false;
    }

    if (priority) {
        return true;
    }

    return companionHasAutomaticEnrichmentWork(cs,
                                               ENRICH_PENDING_THRESHOLD_INTERNAL);
}

static bool shouldRunExternalPhoneProbe(const CompanionScheduler& cs) {
    if (!cs.enabled || cs.workState != COMPANION_WORK_IDLE) {
        return false;
    }
    const bool priority = companionHasProbePriorityReason(cs);
    if (!priority && automaticCompanionShouldYieldToUpload()) {
        return false;
    }
    const bool bypassGap = (cs.lastProbeMs == 0) || priority;
    if (!bypassGap && millis() - cs.lastProbeMs < PHONE_PROBE_MIN_GAP_MS) {
        return false;
    }
    if (!priority && millis() < cs.nextProbeMs) {
        return false;
    }
    return true;
}

static bool shouldRunExternalEnrichment(const CompanionScheduler& cs) {
    if (!cs.enabled ||
        cs.phoneState != COMPANION_PHONE_AVAILABLE ||
        cs.workState != COMPANION_WORK_IDLE) {
        return false;
    }
    const bool priority = companionHasPriorityReason(cs);
    if (!priority && automaticCompanionShouldYieldToUpload()) {
        return false;
    }
    if (!priority && millis() - cs.lastEnrichMs < ENRICH_MIN_GAP_MS) {
        return false;
    }
    return companionHasAutomaticEnrichmentWork(cs,
                                               ENRICH_PENDING_THRESHOLD_WIO);
}

static bool requestExternalPhoneProbe(CompanionScheduler& cs, PhoneProbeReason reason) {
    const char* probeReason = phoneProbeReasonName(reason);
    if (!WIO_NRF.requestCompanionLink(probeReason, true)) {
        return false;
    }

    cs.externalTransportActive = true;
    cs.workState = COMPANION_WORK_PROBING;
    cs.lastProbeMs = millis();
    cs.manualProbeRequested = false;
    cs.manualEnrichProbeBypass = false;
    DLOG_INFO("WIO", "External phone probe requested reason=%s", probeReason);
    return true;
}

static bool requestExternalPhoneEnrichment(CompanionScheduler& cs, const char* reason) {
    beginManualEnrichmentExclusive(cs, reason);

    if (!WIO_NRF.requestCompanionLink(reason ? reason : "external_enrich", true)) {
        endManualEnrichmentExclusive(cs, "external_enrich_link_request_failed");
        return false;
    }

    cs.externalTransportActive = true;
    cs.workState = COMPANION_WORK_ENRICHING;
    resetEnrichmentSessionStats(cs);
    cs.enrichmentRequestIssued = false;
    cs.lastRequestedEnrichmentCount = 0;
    cs.enrichmentWindowActive = false;
    initEnrichQueue();
    DLOG_INFO("WIO", "External enrichment session started reason=%s",
              reason ? reason : "external_enrich");
    return true;
}

static bool requestPhoneProbeLease(CompanionScheduler& cs,
                                   PhoneProbeReason reason) {
    if (PHONE_XPORT.isWioActive()) {
        DLOG_WARN("BLE", "Internal phone probe suppressed: WIO BLE proxy active");
        return false;
    }

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

    cs.externalTransportActive = false;
    cs.workState = COMPANION_WORK_PROBING;
    cs.lastProbeMs = millis();
    cs.manualProbeRequested = false;   // consume one-shot
    cs.manualEnrichProbeBypass = false;
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
    if (PHONE_XPORT.isWioActive()) {
        DLOG_WARN("BLE", "Internal phone enrichment suppressed: WIO BLE proxy active");
        return false;
    }

    if (RADIO_ARB.currentOwner() == RADIO_WIFI_UPLOAD) {
        DLOG_WARN("BLE", "Phone enrichment skipped reason=%s upload active",
                  reason ? reason : "enrich");
        return false;
    }

    beginManualEnrichmentExclusive(cs, reason);

    if (!RADIO_ARB.requestLease(
            RADIO_BLE_GPS,
            RadioArbiter::BLE_PHONE_ENRICH_HOLD_MS,
            reason)) {
        endManualEnrichmentExclusive(cs, "enrich_lease_request_failed");
        return false;
    }

    if (!BLE_MGR.requestCompanionLink(reason ? reason : "enrich", true)) {
        RADIO_ARB.release(RADIO_BLE_GPS, "enrich_link_request_failed");
        endManualEnrichmentExclusive(cs, "enrich_link_request_failed");
        return false;
    }

    cs.externalTransportActive = false;
    cs.workState = COMPANION_WORK_ENRICHING;
    resetEnrichmentSessionStats(cs);
    cs.enrichmentRequestIssued = false;
    cs.lastRequestedEnrichmentCount = 0;
    cs.enrichmentWindowActive = false;
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
                                         size_t& outCount,
                                         const char* logTag) {
    outCount = 0;

    if (!out || maxCount == 0 || maxCount > PHONE_ENRICH_BATCH_MAX) {
        return false;
    }

    DLOG_INFO(logTag, "Enrichment pending scan begin claimed=%u max=%u",
              static_cast<unsigned>(enrichClaimedCount),
              static_cast<unsigned>(maxCount));

    PendingEventDescriptor pendingBatch[PHONE_ENRICH_BATCH_MAX] = {};
    if (!STORAGE.getPendingEnrichmentBatchExcluding(enrichClaimedEventIds,
                                                    enrichClaimedCount,
                                                    pendingBatch,
                                                    maxCount,
                                                    outCount)) {
        DLOG_WARN(logTag, "Failed to build enrichment backlog batch");
        return false;
    }

    DLOG_INFO(logTag, "Enrichment pending scan done count=%u",
              static_cast<unsigned>(outCount));

    // Only absolute capture UTC can be matched against phone GPS history.
    size_t kept = 0;
    size_t skippedNoEpoch = 0;
    for (size_t i = 0; i < outCount; ++i) {
        if (pendingBatch[i].epochUtc == 0) {
            skippedNoEpoch++;
            continue;
        }
        out[kept].eventId     = pendingBatch[i].eventId;
        out[kept].timestampMs = pendingBatch[i].epochUtc;
        out[kept].type        = pendingBatch[i].type;
        out[kept].status      = pendingBatch[i].status;
        kept++;
    }
    outCount = kept;

    if (skippedNoEpoch > 0) {
        DLOG_INFO(logTag,
                  "Enrichment pending scan skipped no-epoch=%u kept=%u",
                  static_cast<unsigned>(skippedNoEpoch),
                  static_cast<unsigned>(kept));
    }

    return true;
}

static void _fillEventBatchRecordFromPending(EventBatchRecord& out,
                                             const PendingEventDescriptor& pending) {
    out.eventId = pending.eventId;
    // pending.epochUtc is the absolute capture UTC persisted with the record
    // (segment epoch base + millis delta). It is reboot-safe; the live-clock
    // epochForMillis() conversion is not. Callers only forward records whose
    // epochUtc is valid, so no live fallback is needed here.
    out.timestampMs = pending.epochUtc;
    out.type = pending.type;
    out.status = pending.status;
}

static bool _buildManualEnrichmentWindowBatch(CompanionScheduler& cs,
                                              EventBatchRecord* out,
                                              size_t maxCount,
                                              size_t& outCount,
                                              const char* logTag,
                                              const char* reason) {
    outCount = 0;
    if (!out || maxCount == 0 || maxCount > PHONE_ENRICH_BATCH_MAX) {
        return false;
    }

    // A no-progress full window means only phone-deferred records remain.
    if (!cs.enrichmentWindowActive && cs.enrichWindowSnapshotValid &&
        !cs.lastWindowProgressed) {
        if (cs.manualRetireDeferred) {
            cs.manualDrainComplete = true;
            DLOG_INFO(logTag,
                      "Manual enrich complete: backlog resolved (enriched or no-data retired)");
            return true;
        }
        ++cs.manualDeferredPasses;
        if (cs.manualDeferredPasses >= ENRICH_DEFERRED_RETIRE_PASSES) {
            cs.manualRetireDeferred = true;
            DLOG_INFO(logTag,
                      "Manual enrich: phone declined %u passes; retiring remaining records as NO_DATA",
                      static_cast<unsigned>(cs.manualDeferredPasses));
        } else {
            DLOG_INFO(logTag,
                      "Manual enrich: window drained nothing (deferred pass %u/%u); re-offering to phone",
                      static_cast<unsigned>(cs.manualDeferredPasses),
                      static_cast<unsigned>(ENRICH_DEFERRED_RETIRE_PASSES));
        }
        // Re-arm for a re-offer or retirement pass.
        cs.enrichWindowSnapshotValid = false;
        cs.lastWindowProgressed = true;
        // fall through to (re)build a window
    }

    const char* safeReason = (reason && reason[0])
                                 ? reason
                                 : "manual_enrich_window";
    const RadioOwner owner = RADIO_ARB.currentOwner();
    const bool bleOwnsWindow = owner == RADIO_BLE_GPS;
    bool stoppedCapture = false;
    bool tookMaintenanceLease = false;

    if (owner == RADIO_WIFI_CAPTURE) {
        RADIO_ARB.release(RADIO_WIFI_CAPTURE, safeReason, false);
        stoppedCapture = true;
    }

    if (!bleOwnsWindow && !RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE)) {
        if (!RADIO_ARB.requestStorageMaintenanceLease(5000UL,
                                                     safeReason,
                                                     true)) {
            DLOG_WARN(logTag,
                      "Manual enrichment window unavailable owner=%s",
                      RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
            if (stoppedCapture && !cs.manualEnrichExclusiveActive) {
                RADIO_ARB.ensureDefaultCapture(safeReason);
            }
            return false;
        }
        tookMaintenanceLease = true;
    }

    StorageExclusiveWindow window;
    const StorageWindowKind windowKind = bleOwnsWindow
                                             ? STORAGE_WINDOW_ENRICHMENT
                                             : STORAGE_WINDOW_MAINTENANCE;
    if (!window.begin(windowKind, safeReason)) {
        DLOG_WARN(logTag,
                  "Manual enrichment window begin failed owner=%s",
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
        if (tookMaintenanceLease && RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE)) {
            RADIO_ARB.release(RADIO_STORAGE_MAINTENANCE,
                              "manual_enrich_window_begin_failed",
                              false);
        }
        if (stoppedCapture && !cs.manualEnrichExclusiveActive) {
            RADIO_ARB.ensureDefaultCapture("manual_enrich_window_begin_failed");
        }
        return false;
    }

    if (!cs.enrichmentWindowActive) {
        // The phone delivers UTC over the GPS/time channel as soon as the
        // secure session is ready (just before this point). Stamp any this-boot
        // segments that were captured before the clock arrived so their records
        // resolve to a real epoch in the window we are about to build.
        STORAGE.backfillSegmentEpochsForEnrich();

        const size_t requestedWindow =
            static_cast<size_t>(std::min<uint32_t>(
                std::max<uint32_t>(cs.pendingItems, PHONE_ENRICH_BATCH_MAX),
                static_cast<uint32_t>(ENRICH_MANUAL_WINDOW_MAX)));
        bool windowReady = false;
        const bool ok = STORAGE.prepareEnrichmentIndexForWindow(
            requestedWindow, ENRICH_SCAN_BUDGET_MS, windowReady);
        if (!ok) {
            window.end("manual_enrich_window_failed");
            if (tookMaintenanceLease && RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE)) {
                RADIO_ARB.release(RADIO_STORAGE_MAINTENANCE,
                                  "manual_enrich_window_failed",
                                  false);
            }
            if (!cs.manualEnrichExclusiveActive &&
                (stoppedCapture || RADIO_ARB.currentOwner() == RADIO_NONE)) {
                RADIO_ARB.ensureDefaultCapture("manual_enrich_window_failed");
            }
            return false;
        }
        if (!windowReady) {
            window.end("manual_index_slice");
            if (tookMaintenanceLease && RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE)) {
                RADIO_ARB.release(RADIO_STORAGE_MAINTENANCE,
                                  "manual_index_slice",
                                  false);
            }
            if (!cs.manualEnrichExclusiveActive &&
                (stoppedCapture || RADIO_ARB.currentOwner() == RADIO_NONE)) {
                RADIO_ARB.ensureDefaultCapture("manual_index_slice");
            }
            return true;
        }
        cs.enrichmentWindowActive = true;
        if (STORAGE.enrichmentWindowSize() == 0) {
            STORAGE.releaseEnrichmentIndexMemory("manual_scan_empty");
            cs.enrichmentWindowActive = false;
            cs.manualDrainComplete = true;
            cs.enrichWindowSnapshotValid = false;
            DLOG_INFO(logTag,
                      "Manual enrich complete: authoritative scan found no pending records");
            window.end("manual_scan_empty");
            if (tookMaintenanceLease && RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE)) {
                RADIO_ARB.release(RADIO_STORAGE_MAINTENANCE,
                                  "manual_scan_empty",
                                  false);
            }
            if (!cs.manualEnrichExclusiveActive &&
                (stoppedCapture || RADIO_ARB.currentOwner() == RADIO_NONE)) {
                RADIO_ARB.ensureDefaultCapture("manual_scan_empty");
            }
            return true;
        }
        // Snapshot the live pending-enrich total for this window pass so we can
        // tell, when the window is fully walked, whether it drained anything.
        cs.enrichWindowStartPending = STORAGE.livePendingEnrichmentTotal();
        cs.enrichWindowSnapshotValid = true;
    }

    // Retire unenrichable records in chunks without starving enrichable output.
    uint32_t noDataRetired = 0;
    bool noDataBudgetExhausted = false;
    bool walkTimeBudgetHit = false;
    const uint32_t walkStartMs = millis();

    uint32_t noDataChunkIds[ENRICH_NODATA_CHUNK] = {};
    size_t noDataChunkCount = 0;

    auto flushNoData = [&]() {
        if (noDataChunkCount == 0) return;
        String sessionIds[ENRICH_NODATA_CHUNK];
        if (!STORAGE.findEventSessions(noDataChunkIds, noDataChunkCount,
                                       sessionIds)) {
            DLOG_WARN(logTag, "Failed to resolve NO_DATA sessions count=%u",
                      static_cast<unsigned>(noDataChunkCount));
            noDataChunkCount = 0;
            return;
        }
        SpoolEnrichBatchEntry entries[ENRICH_NODATA_CHUNK];
        size_t n = 0;
        for (size_t i = 0; i < noDataChunkCount; ++i) {
            if (noDataChunkIds[i] == 0 || !sessionIds[i].length()) {
                continue;
            }
            entries[n++] = {noDataChunkIds[i], sessionIds[i].c_str(),
                            0.0f, 0.0f, 0.0f, 0.0f, nullptr, 0, true};
        }
        if (n > 0) {
            uint32_t applied = 0;
            uint32_t failed = 0;
            STORAGE.beginHotPathDiagnosticsSuppressed();
            const bool ok =
                STORAGE.appendEnrichDeltasBatch(entries, n, &applied, &failed);
            STORAGE.endHotPathDiagnosticsSuppressed();
            noDataRetired += applied;
            if (!ok || failed > 0 || applied != n) {
                DLOG_WARN(logTag,
                          "NO_DATA retire partial applied=%lu failed=%lu requested=%u",
                          static_cast<unsigned long>(applied),
                          static_cast<unsigned long>(failed),
                          static_cast<unsigned>(n));
            }
        }
        noDataChunkCount = 0;
    };

    while (outCount < maxCount) {
        if (millis() - walkStartMs > ENRICH_WALK_BUDGET_MS) {
            walkTimeBudgetHit = true;
            break;
        }

        PendingEventDescriptor pending;
        bool found = false;
        if (!STORAGE.getNextPendingEnrichmentRecord(pending, found)) {
            flushNoData();
            DLOG_WARN(logTag, "Manual enrichment window cursor failed");
            STORAGE.releaseEnrichmentIndexMemory("manual_cursor_failed");
            cs.enrichmentWindowActive = false;
            window.end("manual_cursor_failed");
            if (tookMaintenanceLease && RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE)) {
                RADIO_ARB.release(RADIO_STORAGE_MAINTENANCE,
                                  "manual_cursor_failed",
                                  false);
            }
            if (!cs.manualEnrichExclusiveActive &&
                (stoppedCapture || RADIO_ARB.currentOwner() == RADIO_NONE)) {
                RADIO_ARB.ensureDefaultCapture("manual_cursor_failed");
            }
            return false;
        }
        if (!found) {
            flushNoData();
            STORAGE.releaseEnrichmentIndexMemory("manual_cursor_done");
            cs.enrichmentWindowActive = false;
            break;
        }

        // NO_DATA means either no absolute timestamp, or bounded phone declines.
        if (pending.epochUtc == 0 || cs.manualRetireDeferred) {
            if (noDataRetired + noDataChunkCount >= ENRICH_NODATA_BUDGET_PER_WALK) {
                noDataBudgetExhausted = true;
                break;
            }
            noDataChunkIds[noDataChunkCount++] = pending.eventId;
            if (noDataChunkCount >= ENRICH_NODATA_CHUNK) {
                flushNoData();
            }
            continue;  // does not consume the enrichable batch budget
        }

        _fillEventBatchRecordFromPending(out[outCount], pending);
        outCount++;
    }

    flushNoData();

    // Progress can be no-data retirement even when no phone batch is produced.
    cs.lastWalkNoDataRetired = noDataRetired;

    if (!cs.enrichmentWindowActive && cs.enrichWindowSnapshotValid) {
        // The pending total can lag stale summaries, so count retirements too.
        cs.lastWindowProgressed =
            (noDataRetired > 0) ||
            (STORAGE.livePendingEnrichmentTotal() < cs.enrichWindowStartPending);
        if (cs.lastWindowProgressed) {
            cs.manualDeferredPasses = 0;
            cs.manualRetireDeferred = false;
        }
    }

    // A capped NO_DATA pass can resume the resident PSRAM cursor. Rebuilding
    // here would rescan every accumulated delta after each small flash batch.
    if (outCount == 0 && (noDataBudgetExhausted || walkTimeBudgetHit)) {
        DLOG_WARN(logTag,
                  "Manual enrichment retired %lu no-data records this pass; cursor retained (budget=%u timeHit=%u)",
                  static_cast<unsigned long>(noDataRetired),
                  noDataBudgetExhausted ? 1U : 0U,
                  walkTimeBudgetHit ? 1U : 0U);
    }

    const bool stoppedOnCap = noDataBudgetExhausted || walkTimeBudgetHit;
    const char* endReason = stoppedOnCap ? "manual_no_data_budget"
                                         : "manual_batch_built";
    DLOG_INFO(logTag,
              "Manual enrichment window batch count=%u active=%u noData=%lu budgetExhausted=%u timeHit=%u",
              static_cast<unsigned>(outCount),
              cs.enrichmentWindowActive ? 1U : 0U,
              static_cast<unsigned long>(noDataRetired),
              noDataBudgetExhausted ? 1U : 0U,
              walkTimeBudgetHit ? 1U : 0U);
    window.end(endReason);
    if (tookMaintenanceLease && RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE)) {
        RADIO_ARB.release(RADIO_STORAGE_MAINTENANCE, endReason, false);
    }
    if (!cs.manualEnrichExclusiveActive &&
        (stoppedCapture || RADIO_ARB.currentOwner() == RADIO_NONE)) {
        RADIO_ARB.ensureDefaultCapture(endReason);
    }
    return true;
}

static bool _buildPendingEnrichmentBatchInSafeWindow(EventBatchRecord* out,
                                                     size_t maxCount,
                                                     size_t& outCount,
                                                     const char* logTag,
                                                     const char* reason) {
    outCount = 0;

    const char* safeReason = (reason && reason[0])
                                 ? reason
                                 : "external_enrich_scan";
    const RadioOwner owner = RADIO_ARB.currentOwner();
    const bool bleOwnsWindow = owner == RADIO_BLE_GPS;
    bool stoppedCapture = false;
    bool tookMaintenanceLease = false;

    if (owner == RADIO_WIFI_CAPTURE) {
        RADIO_ARB.release(RADIO_WIFI_CAPTURE, safeReason, false);
        stoppedCapture = true;
    }

    if (!bleOwnsWindow && !RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE)) {
        if (!RADIO_ARB.requestStorageMaintenanceLease(5000UL,
                                                     safeReason,
                                                     true)) {
            DLOG_WARN(logTag,
                      "Enrichment pending scan safe window unavailable owner=%s",
                      RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
            if (stoppedCapture) {
                RADIO_ARB.ensureDefaultCapture(safeReason);
            }
            return false;
        }
        tookMaintenanceLease = true;
    }

    StorageExclusiveWindow window;
    const StorageWindowKind windowKind = bleOwnsWindow
                                             ? STORAGE_WINDOW_ENRICHMENT
                                             : STORAGE_WINDOW_MAINTENANCE;
    if (!window.begin(windowKind, safeReason)) {
        DLOG_WARN(logTag,
                  "Enrichment pending scan window begin failed owner=%s",
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
        if (tookMaintenanceLease && RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE)) {
            RADIO_ARB.release(RADIO_STORAGE_MAINTENANCE,
                              "external_enrich_scan_window_failed",
                              false);
        }
        if (stoppedCapture) {
            RADIO_ARB.ensureDefaultCapture("external_enrich_scan_window_failed");
        }
        return false;
    }

    const bool ok = _buildPendingEnrichmentBatch(out,
                                                 maxCount,
                                                 outCount,
                                                 logTag);
    window.end(ok ? "batch_built" : "batch_failed");

    if (tookMaintenanceLease && RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE)) {
        RADIO_ARB.release(RADIO_STORAGE_MAINTENANCE,
                          ok ? "external_enrich_scan_done"
                             : "external_enrich_scan_failed",
                          false);
    }
    if (stoppedCapture || RADIO_ARB.currentOwner() == RADIO_NONE) {
        RADIO_ARB.ensureDefaultCapture(ok ? "external_enrich_scan_done"
                                          : "external_enrich_scan_failed");
    }

    return ok;
}

static bool _applyPhoneEnrichmentBatch(const PendingEnrichment* records,
                                       size_t count,
                                       uint32_t& outApplied,
                                       uint32_t& outFailed,
                                       uint32_t& outDeferred,
                                       uint32_t& outStorageMs,
                                       const char* logTag) {
    outApplied = 0;
    outFailed = 0;
    outDeferred = 0;
    outStorageMs = 0;

    if (!records || count == 0) {
        return false;
    }

    if (!STORAGE.prepareForEnrichmentAppend(count)) {
        DLOG_WARN(logTag, "Enrich preflight unavailable; records retained count=%u",
                  static_cast<unsigned>(count));
        return false;
    }

    bool anySuccess = false;
    uint32_t applied = 0;
    uint32_t failed = 0;
    uint32_t deferred = 0;
    uint32_t eventIds[PHONE_ENRICH_BATCH_MAX] = {};
    String sessionIds[PHONE_ENRICH_BATCH_MAX];

    const size_t lookupCount = std::min(count, PHONE_ENRICH_BATCH_MAX);
    for (size_t i = 0; i < lookupCount; ++i) {
        eventIds[i] = records[i].eventId;
    }

    if (!STORAGE.findEventSessions(eventIds, lookupCount, sessionIds)) {
        DLOG_WARN(logTag, "Enrichment session lookup failed");
        return false;
    }

    // Build a flat entry array for the batch writer.
    // Records with no session are counted as failures and skipped.
    SpoolEnrichBatchEntry batchEntries[PHONE_ENRICH_BATCH_MAX];
    size_t batchSize = 0;
    for (size_t i = 0; i < count; ++i) {
        const PendingEnrichment& r = records[i];
        if (r.eventId == 0) {
            deferred++;
            continue;
        }
        if (i >= lookupCount || !sessionIds[i].length()) {
            failed++;
            DLOG_WARN(logTag, "Enrichment no session event=%lu",
                      static_cast<unsigned long>(r.eventId));
            continue;
        }
        batchEntries[batchSize++] = {
            r.eventId, sessionIds[i].c_str(),
            r.lat, r.lon, r.alt, r.accuracy, r.tag, r.gpsEpochUtc, r.noData
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
    DLOG_INFO(logTag,
              "enrich_perf count=%u applied=%u failed=%u deferred=%u storageMs=%lu pendingUpload=%lu",
              static_cast<unsigned>(count),
              static_cast<unsigned>(applied),
              static_cast<unsigned>(failed),
              static_cast<unsigned>(deferred),
              static_cast<unsigned long>(tApplyMs),
              static_cast<unsigned long>(pending));

    outApplied = applied;
    outFailed = failed;
    outDeferred = deferred;
    outStorageMs = tApplyMs;
    return anySuccess;
}

static void _finishPhoneEnrichment(CompanionScheduler& cs, bool success) {
    const uint32_t sessionStartMs = cs.enrichmentSessionStartMs;
    const uint32_t requested = cs.enrichmentSessionRequested;
    const uint32_t applied = cs.enrichmentSessionApplied;
    const uint32_t failed = cs.enrichmentSessionFailed;
    const uint32_t deferred = cs.enrichmentSessionDeferred;
    const uint32_t batches = cs.enrichmentSessionBatches;
    const uint32_t xferMs = cs.enrichmentSessionXferMs;
    const uint32_t storageMs = cs.enrichmentSessionStorageMs;
    const uint32_t totalMs = sessionStartMs ? (millis() - sessionStartMs) : 0U;

    if (sessionStartMs != 0 && (requested > 0 || batches > 0)) {
        const char* logTag = companionTransportTag(cs);
        DLOG_INFO(logTag,
                  "enrich_session_summary requested=%lu applied=%lu failed=%lu deferred=%lu batches=%lu xferMs=%lu storageMs=%lu totalMs=%lu",
                  static_cast<unsigned long>(requested),
                  static_cast<unsigned long>(applied),
                  static_cast<unsigned long>(failed),
                  static_cast<unsigned long>(deferred),
                  static_cast<unsigned long>(batches),
                  static_cast<unsigned long>(xferMs),
                  static_cast<unsigned long>(storageMs),
                  static_cast<unsigned long>(totalMs));
        if (FieldVault::isReady()) {
            uint32_t pendingUpload = 0;
            uint32_t pendingEnrich = 0;
            STATE_READ_BEGIN();
            pendingUpload =
                g_state.storagePendingUploadMission + g_state.storagePendingUploadNoise;
            pendingEnrich =
                g_state.storagePendingEnrichMission + g_state.storagePendingEnrichNoise;
            STATE_READ_END();
            if (!FieldVault::appendEnrichSummary(logTag,
                                                 success,
                                                 requested,
                                                 applied,
                                                 failed,
                                                 deferred,
                                                 batches,
                                                 xferMs,
                                                 storageMs,
                                                 totalMs,
                                                 pendingUpload,
                                                 pendingEnrich)) {
                DLOG_WARN("FIELDVAULT", "enrich summary append failed");
            }
        }
    }

    cs.workState = COMPANION_WORK_IDLE;
    cs.lastEnrichMs = millis();
    cs.enrichmentRequestIssued = false;
    cs.lastRequestedEnrichmentCount = 0;
    if (cs.enrichmentWindowActive) {
        STORAGE.releaseEnrichmentIndexMemory(success ? "enrich_done"
                                                     : "enrich_fail");
        cs.enrichmentWindowActive = false;
    }
    cs.enrichmentSessionStartMs = 0;
    cs.enrichmentSessionRequested = 0;
    cs.enrichmentSessionApplied = 0;
    cs.enrichmentSessionFailed = 0;
    cs.enrichmentSessionDeferred = 0;
    cs.lastWalkNoDataRetired = 0;
    cs.enrichWindowStartPending = 0;
    cs.enrichWindowSnapshotValid = false;
    cs.lastWindowProgressed = true;
    cs.manualDrainComplete = false;
    cs.manualDeferredPasses = 0;
    cs.manualRetireDeferred = false;
    cs.enrichmentSessionBatches = 0;
    cs.enrichmentSessionXferMs = 0;
    cs.enrichmentSessionStorageMs = 0;

    if (success) {
        // drainOneEnrichBatch updates the cached pending count as records are
        // applied.  Avoid a fresh full spool count here; the next scheduled
        // mirror refresh will reconcile lane details precisely.
        cs.lastPendingRefreshMs  = millis();
        cs.phoneState = COMPANION_PHONE_AVAILABLE;
        cs.lastSeenMs = millis();
        cs.manualProbeRequested  = false;
        cs.manualEnrichRequested = false;
        cs.manualEnrichProbeBypass = false;
        cs.offloadPrepRequested  = false;
        cs.timeSyncRequested     = false;
        if (deferred > 0 || failed > 0) {
            DLOG_WARN(companionTransportTag(cs),
                      "Phone enrichment finished with pending retries applied=%lu failed=%lu deferred=%lu",
                      static_cast<unsigned long>(applied),
                      static_cast<unsigned long>(failed),
                      static_cast<unsigned long>(deferred));
        } else {
            DLOG_INFO(companionTransportTag(cs), "Phone enrichment finished successfully");
        }
    } else {
        // Pending counts are unchanged on failure; the next probe will refresh.
        cs.manualLinkHold = false;
        cs.phoneState = COMPANION_PHONE_UNAVAILABLE;
        DLOG_WARN(companionTransportTag(cs), "Phone enrichment failed");
    }

    // Successful internal localization is immediately followed by durable
    // phone offload. Keep the authenticated GATT session only for the bounded
    // handoff/transfer window; OFFLOAD_END, disconnect, inactivity, or timeout
    // returns the radio to capture without relying on a separate cancel pulse.
    if (success &&
        !cs.externalTransportActive &&
        PHONE_XPORT.isPhoneStorageReady()) {
        const PhoneStorageFrameV1 handoff = _buildPhoneStorageFrame();
        const uint32_t pendingUpload =
            handoff.pendingUploadMission + handoff.pendingUploadNoise;
        if (pendingUpload > 0) {
            beginAutomaticOffloadHold(cs, "localization_complete");
            DLOG_INFO("OFFLOAD",
                      "localization complete; bounded handoff pending=%lu",
                      static_cast<unsigned long>(pendingUpload));
        }
    }

    const bool keepPersistentLink =
        success && (cs.manualLinkHold || cs.automaticOffloadHold);
    if (RADIO_ARB.isOwner(RADIO_BLE_GPS) && !keepPersistentLink) {
        RADIO_ARB.release(RADIO_BLE_GPS,
                          success ? "enrich_done" : "enrich_fail");
    } else if (keepPersistentLink) {
        DLOG_INFO("BLE", "Enrichment complete; persistent companion link retained");
    }

    endManualEnrichmentExclusive(cs, success ? "enrich_done"
                                             : "enrich_fail");

    if (success) {
        crashBreadcrumbClear(CrashPhase::BACKLOG_ENRICH);
    }
}

// Enrichment pipeline queue helpers.

static void initEnrichQueue() {
    _enrichQueueStorage();  // PSRAM-backed; allocated on first use
    enrichQueueHead  = 0;
    enrichQueueTail  = 0;
    enrichQueueSize  = 0;
    enrichClaimedCount = 0;
    memset(enrichClaimedEventIds, 0, sizeof(enrichClaimedEventIds));
    for (size_t i = 0; i < ENRICH_QUEUE_DEPTH; ++i) {
        _enrichQueueStorage()[i].count    = 0;
        _enrichQueueStorage()[i].queuedMs = 0;
    }
}

// Claim only after a valid BLE response; timeouts leave no stuck state.
static void enrichClaimReceived(const PendingEnrichment* recs, size_t count) {
    for (size_t i = 0; i < count && enrichClaimedCount < ENRICH_CLAIM_MAX; ++i) {
        if (recs[i].eventId != 0) {
            enrichClaimedEventIds[enrichClaimedCount++] = recs[i].eventId;
        }
    }
}

static uint32_t enrichmentBatchDeferredCount(const PendingEnrichment* recs,
                                             size_t count) {
    if (!recs || count == 0) return 0;
    uint32_t deferred = 0;
    for (size_t i = 0; i < count; ++i) {
        if (recs[i].eventId == 0) {
            deferred++;
        }
    }
    return deferred;
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

static void applyEnrichmentProgressToScheduler(CompanionScheduler& cs,
                                               uint32_t applied) {
    if (applied == 0) {
        return;
    }

    cs.pendingNoiseItems = (cs.pendingNoiseItems >= applied)
                           ? cs.pendingNoiseItems - applied : 0;
    cs.pendingItems = cs.pendingMissionItems + cs.pendingNoiseItems;
    cs.lastPendingRefreshMs = millis();
}

static void markCompanionEnrichmentAuthoritativelyEmpty(CompanionScheduler& cs) {
    const uint32_t now = millis();
    cs.pendingMissionItems = 0;
    cs.pendingNoiseItems = 0;
    cs.pendingItems = 0;
    cs.lastPendingRefreshMs = now;
    StorageUiMirror::publishPendingEnrichment(0, 0, now);
}

static bool enrichQueueHasRoom() {
    return enrichQueueSize < ENRICH_QUEUE_DEPTH;
}

static bool enqueueEnrichBatch(const PendingEnrichment* records, size_t count, const char* logTag) {
    if (enrichQueueSize >= ENRICH_QUEUE_DEPTH || count == 0) return false;
    const size_t n = std::min(count, PHONE_ENRICH_BATCH_MAX);
    QueuedEnrichBatch& slot = _enrichQueueStorage()[enrichQueueTail];
    memcpy(slot.records, records, n * sizeof(PendingEnrichment));
    slot.count    = n;
    slot.queuedMs = millis();
    enrichQueueTail = (enrichQueueTail + 1) % ENRICH_QUEUE_DEPTH;
    enrichQueueSize++;
    DLOG_INFO(logTag, "Enrich batch queued count=%u queueSize=%u",
              static_cast<unsigned>(n),
              static_cast<unsigned>(enrichQueueSize));
    return true;
}

// Enrichment drain modes balance sidecar writes against capture appends.
enum class EnrichDrainMode : uint8_t {
    DRAIN_IDLE,
    DRAIN_TRICKLE,
    DRAIN_TORRENT,
};

static uint32_t enrichQueueOldestAgeMs() {
    if (enrichQueueSize == 0) return 0;
    const uint32_t qms = _enrichQueueStorage()[enrichQueueHead].queuedMs;
    if (qms == 0) return 0;
    const uint32_t now = millis();
    return (now >= qms) ? (now - qms) : 0U;
}

static EnrichDrainMode enrichDrainMode(const CompanionScheduler* cs = nullptr) {
    if (enrichQueueSize == 0) return EnrichDrainMode::DRAIN_IDLE;
    // User-initiated enrich is always torrent — the user is explicitly asking
    // for the queue to clear now and accepts the capture impact.
    if (cs && cs->manualEnrichRequested) {
        return EnrichDrainMode::DRAIN_TORRENT;
    }
    // If we already hold a BLE lease (active enrich session) or the
    // maintenance lease, the radio has already yielded for us — torrent.
    if (RADIO_ARB.isBleOwner() ||
        RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE)) {
        return EnrichDrainMode::DRAIN_TORRENT;
    }
    const uint32_t age = enrichQueueOldestAgeMs();
    if (enrichQueueSize >= ENRICH_DRAIN_HIGH_WATER ||
        age >= ENRICH_DRAIN_MAX_AGE_MS) {
        return EnrichDrainMode::DRAIN_TORRENT;
    }
    return EnrichDrainMode::DRAIN_TRICKLE;
}

static void drainOneEnrichBatch(CompanionScheduler& cs);

// Service the enrich queue according to current policy.  Safe to call every
// tick; returns the number of batches drained.  If the torrent lease is
// refused, falls back to a single trickle drain so we still make forward
// progress without blocking capture.
static size_t runEnrichDrain(CompanionScheduler& cs, const char* tagHint = nullptr) {
    if (enrichQueueSize == 0) return 0;

    const char* logTag = tagHint ? tagHint : companionTransportTag(cs);
    const EnrichDrainMode mode = enrichDrainMode(&cs);

    if (mode == EnrichDrainMode::DRAIN_IDLE) {
        return 0;
    }

    if (mode == EnrichDrainMode::DRAIN_TRICKLE) {
        drainOneEnrichBatch(cs);
        return 1;
    }

    // TORRENT path.  If we already hold a BLE lease (active enrich session)
    // or the maintenance lease, skip lease management and drain directly —
    // capture has already yielded the radio.
    const bool alreadyOwnsWindow =
        RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE) ||
        RADIO_ARB.isBleOwner();

    bool tookLease = false;
    bool preempted = false;
    if (!alreadyOwnsWindow) {
        const uint32_t age = enrichQueueOldestAgeMs();
        const bool manual = cs.manualEnrichRequested;
        // Preempt when the user explicitly asked, or when the oldest queued
        // batch has aged past the patience threshold.
        preempted = manual || (age >= ENRICH_DRAIN_PREEMPT_AGE_MS);
        const RadioOwner priorOwner = RADIO_ARB.currentOwner();
        const char* leaseReason =
            manual    ? "enrich_torrent_manual"   :
            preempted ? "enrich_torrent_preempt"  :
                        "enrich_torrent";

        if (RADIO_ARB.requestStorageMaintenanceLease(ENRICH_DRAIN_LEASE_HOLD_MS,
                                                    leaseReason,
                                                    preempted)) {
            tookLease = true;
            DLOG_INFO(logTag,
                      "Enrich torrent lease taken owner_was=%s preempt=%d age=%lu queue=%u",
                      RadioArbiter::ownerName(priorOwner),
                      preempted ? 1 : 0,
                      static_cast<unsigned long>(age),
                      static_cast<unsigned>(enrichQueueSize));
        } else {
            // Arbiter refused — fall back to a single trickle drain so a
            // capture lease holder is not starved waiting for us.
            DLOG_DEBUG(logTag,
                       "Enrich torrent lease refused owner=%s preempt=%d age=%lu; trickle fallback",
                       RadioArbiter::ownerName(priorOwner),
                       preempted ? 1 : 0,
                       static_cast<unsigned long>(age));
            drainOneEnrichBatch(cs);
            return 1;
        }
    }

    const uint32_t tStart = millis();
    size_t drained = 0;
    while (enrichQueueSize > 0 &&
           (millis() - tStart) < ENRICH_DRAIN_TORRENT_BUDGET_MS) {
        drainOneEnrichBatch(cs);
        drained++;
        if (enrichQueueSize <= ENRICH_DRAIN_LOW_WATER &&
            enrichQueueOldestAgeMs() < ENRICH_DRAIN_MAX_AGE_MS) {
            break;
        }
    }

    if (tookLease && RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE)) {
        const char* releaseReason = preempted ? "enrich_torrent_preempt_done"
                                              : "enrich_torrent_done";
        RADIO_ARB.release(RADIO_STORAGE_MAINTENANCE, releaseReason, false);
        RADIO_ARB.ensureDefaultCapture(releaseReason);
    }

    DLOG_INFO(logTag,
              "Enrich torrent done drained=%u remaining=%u tookLease=%d preempt=%d elapsedMs=%lu",
              static_cast<unsigned>(drained),
              static_cast<unsigned>(enrichQueueSize),
              tookLease ? 1 : 0,
              preempted ? 1 : 0,
              static_cast<unsigned long>(millis() - tStart));
    return drained;
}

static void drainOneEnrichBatch(CompanionScheduler& cs) {
    if (enrichQueueSize == 0) return;
    const char* logTag = companionTransportTag(cs);
    QueuedEnrichBatch& oldest = _enrichQueueStorage()[enrichQueueHead];
    uint32_t batchApplied = 0, batchFailed = 0, batchDeferred = 0, batchStorageMs = 0;
    DLOG_INFO(logTag, "Enrich drain begin count=%u queueSize=%u ageMs=%lu",
              static_cast<unsigned>(oldest.count),
              static_cast<unsigned>(enrichQueueSize),
              static_cast<unsigned long>(millis() - oldest.queuedMs));
    crashCheckpointVolatile(CrashPhase::STORAGE_APPEND,
                            static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                            STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U);
    _applyPhoneEnrichmentBatch(oldest.records, oldest.count,
                               batchApplied, batchFailed, batchDeferred, batchStorageMs,
                               logTag);
    crashBreadcrumbClearVolatile(CrashPhase::STORAGE_APPEND);
    enrichRemoveClaims(oldest.records, oldest.count);
    cs.enrichmentSessionApplied   += batchApplied;
    cs.enrichmentSessionFailed    += batchFailed;
    cs.enrichmentSessionDeferred  += batchDeferred;
    cs.enrichmentSessionStorageMs += batchStorageMs;
    applyEnrichmentProgressToScheduler(cs, batchApplied);
    oldest.count    = 0;
    oldest.queuedMs = 0;
    enrichQueueHead = (enrichQueueHead + 1) % ENRICH_QUEUE_DEPTH;
    enrichQueueSize--;
    DLOG_INFO(logTag, "Enrich drain done applied=%lu failed=%lu deferred=%lu queueSize=%u",
              static_cast<unsigned long>(batchApplied),
              static_cast<unsigned long>(batchFailed),
              static_cast<unsigned long>(batchDeferred),
              static_cast<unsigned>(enrichQueueSize));
}

static void serviceEnrichmentPipeline(CompanionScheduler& cs) {
    if (PHONE_XPORT.isWioActive()) {
        DLOG_DEBUG("BLE", "Internal enrichment pipeline suppressed: WIO BLE proxy active");
        return;
    }

    // Drain oldest queued batch first (storage I/O, independent of BLE state).
    // Track whether we drained this tick so we can skip the spool scan below —
    // back-to-back storage write + full spool read in the same tick fragments
    // the heap unnecessarily.  runEnrichDrain() classifies the queue and either
    // trickles one batch under the mutex or torrents under a maintenance lease.
    bool justDrained = false;
    if (enrichQueueSize > 0) {
        justDrained = (runEnrichDrain(cs) > 0);
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
                const uint32_t deferredCount =
                    enrichmentBatchDeferredCount(enrichments, outCount);
                const bool hasDeferred = deferredCount > 0;
                // Valid response received — enqueue and claim BEFORE clearing
                // requestIssued so that the next _buildPendingEnrichmentBatch
                // call excludes these IDs while they sit in the queue.
                if (enqueueEnrichBatch(enrichments, outCount, companionTransportTag(cs))) {
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
                    if (hasDeferred) {
                        runEnrichDrain(cs, "BLE");
                        if (cs.manualEnrichRequested) {
                            // Cursor-based manual walk: deferred records are
                            // behind the cursor and won't be re-requested within
                            // a window. Keep the connection and drain the rest in
                            // one go; the per-window-pass progress gate ends the
                            // session once a full window drains nothing.
                            cs.enrichmentRequestIssued = false;
                            DLOG_DEBUG("BLE",
                                       "Enrich deferred count=%u; continuing manual session",
                                       static_cast<unsigned>(deferredCount));
                            return;
                        }
                        // Auto path excludes claimed IDs; unclaimed deferred
                        // placeholders would re-request in a loop. End for retry.
                        DLOG_INFO("BLE",
                                  "Enrichment deferred by phone count=%u; ending session for later retry",
                                  static_cast<unsigned>(deferredCount));
                        enrichClearAllClaims();
                        _finishPhoneEnrichment(cs, true);
                        return;
                    }
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

            const bool buildOk = cs.manualEnrichRequested
                ? _buildManualEnrichmentWindowBatch(cs,
                                                    batch,
                                                    PHONE_ENRICH_BATCH_MAX,
                                                    batchCount,
                                                    companionTransportTag(cs),
                                                    "manual_enrich_window")
                : _buildPendingEnrichmentBatch(batch,
                                               PHONE_ENRICH_BATCH_MAX,
                                               batchCount,
                                               companionTransportTag(cs));
            if (!buildOk) {
                DLOG_WARN("BLE", "Failed to build enrichment batch");
                if (enrichQueueSize == 0) {
                    enrichClearAllClaims();
                    _finishPhoneEnrichment(cs, false);
                }
            } else if (batchCount == 0) {
                if (enrichQueueSize == 0) {
                    if (cs.manualEnrichRequested && !cs.manualDrainComplete) {
                        // Manual bulk drain still in progress; the drain-complete
                        // gate ends it once a full window pass drains nothing.
                        DLOG_DEBUG("BLE",
                                   "Manual enrich continuing active=%u",
                                   cs.enrichmentWindowActive ? 1U : 0U);
                    } else {
                        // Both the automatic spool walk and the completed
                        // manual window are authoritative at this point. Keep
                        // the scheduler and phone storage frame from retaining
                        // a stale pre-enrichment segment summary.
                        markCompanionEnrichmentAuthoritativelyEmpty(cs);
                        enrichClearAllClaims();
                        _finishPhoneEnrichment(cs, true);
                    }
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

static void serviceExternalEnrichmentPipeline(CompanionScheduler& cs) {
    if (WIO_NRF.consumeEnrichmentFailure()) {
        DLOG_WARN("WIO", "External BLE enrichment exchange failed");
        enrichClearAllClaims();
        _finishPhoneEnrichment(cs, false);
        return;
    }

    // A single batch was dropped to UART corruption but the link is still up.
    // Clear the in-flight request so the next tick re-requests instead of
    // ending the whole bulk session. The dropped records stay pending and are
    // retried on a later window pass.
    if (WIO_NRF.consumeEnrichmentBatchDropped()) {
        DLOG_DEBUG("WIO", "Enrichment batch dropped (UART); re-requesting");
        cs.enrichmentRequestIssued = false;
        cs.lastRequestedEnrichmentCount = 0;
    }

    if (cs.enrichmentRequestIssued &&
        !WIO_NRF.isEnrichmentExchangeActive() &&
        !WIO_NRF.isCompanionLinkBusy() &&
        !WIO_NRF.isPhoneCompanionReady()) {
        DLOG_WARN("WIO",
                  "External enrichment request stale; resetting issued state requested=%u link=%s phone=%s",
                  static_cast<unsigned>(cs.lastRequestedEnrichmentCount),
                  WIO_NRF.linkStateName(),
                  WIO_NRF.phoneState());
        cs.enrichmentRequestIssued = false;
        cs.lastRequestedEnrichmentCount = 0;
    }

    if (cs.enrichmentRequestIssued) {
        PendingEnrichment enrichments[PHONE_ENRICH_BATCH_MAX] = {};
        size_t outCount = 0;
        if (WIO_NRF.consumeEnrichmentBatch(enrichments,
                                           PHONE_ENRICH_BATCH_MAX,
                                           outCount)) {
            const uint32_t deferredCount =
                enrichmentBatchDeferredCount(enrichments, outCount);
            const bool hasDeferred = deferredCount > 0;
            if (enqueueEnrichBatch(enrichments, outCount, companionTransportTag(cs))) {
                enrichClaimReceived(enrichments, outCount);
                cs.enrichmentSessionBatches++;
                cs.enrichmentSessionXferMs += WIO_NRF.getLastEnrichmentTransferMs();
                cs.enrichmentRequestIssued = false;
                cs.lastRequestedEnrichmentCount = 0;
                DLOG_DEBUG("WIO", "External enrichment batch received count=%u claimed=%u queued=%u",
                           static_cast<unsigned>(outCount),
                           static_cast<unsigned>(enrichClaimedCount),
                           static_cast<unsigned>(enrichQueueSize));
                if (hasDeferred) {
                    runEnrichDrain(cs, "WIO");
                    if (cs.manualEnrichRequested) {
                        // Manual enrich walks a resident window cursor, so the
                        // deferred records are already behind the cursor and
                        // cannot be re-requested within a window. Keep the phone
                        // connected and drain the rest on the next tick instead
                        // of reconnecting per deferral. The session terminates
                        // via the per-window-pass progress gate (a full window
                        // that drains nothing => only retry-later records left).
                        cs.enrichmentRequestIssued = false;
                        DLOG_DEBUG("WIO",
                                   "External enrich deferred count=%u; continuing manual session",
                                   static_cast<unsigned>(deferredCount));
                        return;
                    }
                    // Auto path selects by excluding claimed IDs; deferred
                    // placeholders (eventId=0) are never claimed, so continuing
                    // would re-request them in a loop. End for later retry.
                    DLOG_INFO("WIO",
                              "External enrichment deferred by phone count=%u; ending session for later retry",
                              static_cast<unsigned>(deferredCount));
                    enrichClearAllClaims();
                    _finishPhoneEnrichment(cs, true);
                    return;
                }
            } else {
                DLOG_WARN("WIO", "External enrichment queue full count=%u queued=%u",
                          static_cast<unsigned>(outCount),
                          static_cast<unsigned>(enrichQueueSize));
                enrichClearAllClaims();
                _finishPhoneEnrichment(cs, false);
                return;
            }
        }

        if (cs.enrichmentRequestIssued ||
            WIO_NRF.isEnrichmentExchangeActive()) {
            return;
        }
    }

    if (!WIO_NRF.isPhoneCompanionReady()) {
        if (WIO_NRF.isCompanionLinkBusy()) {
            return;
        }
        if (!WIO_NRF.requestCompanionLink("external_enrich", true)) {
            enrichClearAllClaims();
            _finishPhoneEnrichment(cs, false);
        }
        return;
    }

    cs.phoneState = COMPANION_PHONE_AVAILABLE;
    cs.lastSeenMs = millis();
    publishPhoneStorageSnapshotIfDue(cs);

    if (!cs.enrichmentRequestIssued &&
        enrichQueueHasRoom()) {
        EventBatchRecord batch[PHONE_ENRICH_BATCH_MAX] = {};
        size_t batchCount = 0;

        const bool buildOk = cs.manualEnrichRequested
            ? _buildManualEnrichmentWindowBatch(cs,
                                                batch,
                                                PHONE_ENRICH_BATCH_MAX,
                                                batchCount,
                                                companionTransportTag(cs),
                                                "external_manual_enrich_scan")
            : _buildPendingEnrichmentBatchInSafeWindow(batch,
                                                       PHONE_ENRICH_BATCH_MAX,
                                                       batchCount,
                                                       companionTransportTag(cs),
                                                       "external_enrich_scan");
        if (!buildOk) {
            DLOG_WARN("WIO", "Failed to build external enrichment batch");
            if (enrichQueueSize == 0) {
                enrichClearAllClaims();
                _finishPhoneEnrichment(cs, false);
            }
        } else if (batchCount == 0) {
            if (enrichQueueSize == 0) {
                if (cs.manualEnrichRequested && !cs.manualDrainComplete) {
                    // Manual bulk drain still in progress: the last window pass
                    // made progress (or is mid-window), so keep the connection
                    // and let the next tick rebuild. The drain-complete gate
                    // ends the session once a full window pass drains nothing.
                    DLOG_DEBUG("WIO",
                               "Manual enrich continuing active=%u",
                               cs.enrichmentWindowActive ? 1U : 0U);
                } else {
                    // Auto session, or manual drain fully complete: finish.
                    markCompanionEnrichmentAuthoritativelyEmpty(cs);
                    enrichClearAllClaims();
                    _finishPhoneEnrichment(cs, true);
                    WIO_NRF.disconnectPhone("external_enrich_empty");
                }
            }
        } else if (WIO_NRF.requestEnrichmentBatch(batch, batchCount)) {
            cs.enrichmentRequestIssued = true;
            cs.lastRequestedEnrichmentCount = batchCount;
            cs.enrichmentSessionRequested += static_cast<uint32_t>(batchCount);
            DLOG_DEBUG("WIO", "Requested external enrichment batch count=%u queued=%u",
                       static_cast<unsigned>(batchCount),
                       static_cast<unsigned>(enrichQueueSize));
        } else {
            DLOG_WARN("WIO", "External requestEnrichmentBatch failed");
            if (enrichQueueSize == 0) {
                enrichClearAllClaims();
                _finishPhoneEnrichment(cs, false);
            }
        }
    }

    // Drain after giving the phone a chance to start the next exchange. This
    // overlaps Android location/encrypt/notify work with local LittleFS writes
    // instead of serializing receive -> drain -> request.
    if (enrichQueueSize > 0) {
        runEnrichDrain(cs, "WIO");
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
    Serial.println("[USB]   wio status");
    Serial.println("[USB]   wio hello | wio ble start | wio ble status | wio drop");
    Serial.println("[USB]   wio probe | wio enrich");
    Serial.println("[USB]   wio text <prompt> | wio text cancel");
    Serial.println("[USB]   wio raw <line>  (send exact UART line to WIO)");
    Serial.println("[USB]   companion status");
    Serial.println("[USB]   companion link    (hold secure BLE link until cancel)");
    Serial.println("[USB]   companion probe   (one-shot manual BLE probe)");
    Serial.println("[USB]   companion enrich  (manual enrichment; probes first if needed)");
    Serial.println("[USB]   companion offload prep  (exercise BLE upload-index preparation)");
    Serial.println("[USB]   companion cancel  (clear all pending companion requests)");
    Serial.println("[USB]   time | time sync   (clock status; 'sync' forces NTP now)");
    Serial.println("[USB]   heap status      (internal/PSRAM heap snapshot)");
    Serial.println("[USB]   entity status    (PSRAM Entity working-set summary)");
    Serial.println("[USB]   spool audit       (read-only spool scan, prints mismatches)");
    Serial.println("[USB]   spool count       (exact total record count + spool stats)");
    Serial.println("[USB]   spool enrich      (pending vs already enriched counts)");
    Serial.println("[USB]   spool repair      (audit + repair counters, quarantine bad segs)");
    Serial.println("[USB]   spool diag        (segment list, counters, flags)");
    Serial.println("[USB]   spool quarantine list  (list /spool_bad files)");
    Serial.println("[USB]   spool quarantine meta  (print quarantine JSON records)");
    Serial.println("[USB]   spool quarantine clear (delete all quarantine files)");
    Serial.println("[USB]   fieldvault dump    (print FieldVault records, keep them)");
    Serial.println("[USB]   fieldvault clear   (delete retained FieldVault records)");
    Serial.println("[USB]   fieldvault upload  (upload pending FieldVault records only)");
    Serial.println("[USB]   crash log         (print retained crash breadcrumb ring)");
    Serial.println("[USB]   crash clear       (wipe ring + NVS snapshot + alloc-fail)");
    Serial.println("[USB]   stack audit       (flag any task stack outside internal DRAM)");
    Serial.println("[USB]   ble rxdiag        (print retained BLE receive crash stage)");
    Serial.println("[USB]   upload now         (manual MQTT upload of pending records)");
    Serial.println("[USB]   upload stop        (safely stop the active MQTT upload)");
    Serial.println("[USB]   upload resume      (allow MQTT uploads again)");
    Serial.println("[USB]   upload rewind [id] (re-offer uploaded records; 0/blank = all)");
#if MESHTASTIC_ENABLED && WIO_NRF_ACCESSORY_ENABLED
    Serial.println("[USB]   mesh status        (Meshtastic client state + counters)");
    Serial.println("[USB]   mesh on | mesh off (take/release SX1262 for Meshtastic)");
    Serial.println("[USB]   mesh nodes         (list heard mesh nodes)");
    Serial.println("[USB]   mesh send <text>   (broadcast a text message)");
#endif
#if BLE_SMOKE_ENABLED
    Serial.println("[USB]   ble smoke         (suspend WiFi, init/deinit NimBLE, resume promisc)");
#endif
}

const char* _mqttStateName(MQTTState state) {
    switch (state) {
        case MQTT_IDLE:              return "idle";
        case MQTT_CONNECTING_WIFI:   return "wifi";
        case MQTT_CONNECTING_BROKER: return "broker";
        case MQTT_DUMPING:           return "dumping";
        case MQTT_DONE:              return "done";
        case MQTT_FAILED:            return "failed";
        default:                     return "unknown";
    }
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

void _printUsbWioStatus() {
    const uint32_t now = millis();
    const uint32_t lastSeen = WIO_NRF.lastSeenMs();
    uint32_t wioEpoch = 0;
    const bool wioTime = WIO_NRF.getBestTimeEpoch(wioEpoch);

    Serial.printf("[WIO] available=%d bleProxy=%d sx1262=%d link=%s lastSeenAgeMs=%lu\r\n",
                  WIO_NRF.available() ? 1 : 0,
                  WIO_NRF.hasBleProxy() ? 1 : 0,
                  WIO_NRF.hasSx1262() ? 1 : 0,
                  WIO_NRF.linkStateName(),
                  static_cast<unsigned long>(lastSeen ? now - lastSeen : 0));
    Serial.printf("[WIO] phoneConnected=%d phone=%s rssi=%d\r\n",
                  WIO_NRF.phoneConnected() ? 1 : 0,
                  WIO_NRF.phoneState(),
                  WIO_NRF.phoneRssi());
    Serial.printf("[WIO] gpsFresh=%d lat=%.6f lon=%.6f acc=%.1f timeTrusted=%d epoch=%lu\r\n",
                  WIO_NRF.hasFreshGpsFix() ? 1 : 0,
                  WIO_NRF.gpsLat(),
                  WIO_NRF.gpsLon(),
                  WIO_NRF.gpsAccuracy(),
                  WIO_NRF.isTimeTrusted() ? 1 : 0,
                  static_cast<unsigned long>(wioTime ? wioEpoch : 0));
    Serial.printf("[WIO] textPending=%d textReady=%d prompt=%s\r\n",
                  WIO_NRF.isTextInputPending() ? 1 : 0,
                  WIO_NRF.isTextInputReady() ? 1 : 0,
                  WIO_NRF.phonePromptText());

    const PhoneTransportState& xport = PHONE_XPORT.state();
    const uint32_t xportAgeMs =
        xport.lastChangeMs ? now - xport.lastChangeMs : 0;
    Serial.printf("[XPORT] active=%s previous=%s ageMs=%lu transitions=%lu\r\n",
                  PhoneTransportRouter::kindName(xport.kind),
                  PhoneTransportRouter::kindName(xport.previous),
                  static_cast<unsigned long>(xportAgeMs),
                  static_cast<unsigned long>(xport.transitions));
}

// Target watermark for USB_SPOOL_UPLOAD_REWIND — the command runs inside the
// storage maintenance window, which the dispatch helper enters on its own, so
// the argument rides here rather than through the shared signature.
uint32_t g_usbRewindToEventId = 0;


// Minimal read-only filesystem inspection over USB serial. Exists so the
// contents of /config/vault (known locations, BadUSB scripts) can be captured
// off the device before a repartition wipes LittleFS -- there is no other way
// to get those files out. Read-only by construction: no write, no delete.
void _usbFsList(const String& dir) {
    fs::File d = LittleFS.open(dir.length() ? dir : String("/"));
    if (!d || !d.isDirectory()) {
        Serial.printf("[FS] not a directory: %s\r\n", dir.c_str());
        if (d) d.close();
        return;
    }
    Serial.printf("[FS] ls %s\r\n", dir.c_str());
    size_t count = 0;
    uint32_t totalBytes = 0;
    for (fs::File e = d.openNextFile(); e; e = d.openNextFile()) {
        const String name = String(e.name());
        if (e.isDirectory()) {
            Serial.printf("  <dir>  %s\r\n", name.c_str());
        } else {
            Serial.printf("  %6u %s\r\n",
                          static_cast<unsigned>(e.size()), name.c_str());
            totalBytes += e.size();
        }
        count++;
        e.close();
    }
    d.close();
    Serial.printf("[FS] %u entries, %lu bytes\r\n",
                  static_cast<unsigned>(count),
                  static_cast<unsigned long>(totalBytes));
}

void _usbFsCat(const String& path) {
    fs::File f = LittleFS.open(path, "r");
    if (!f || f.isDirectory()) {
        Serial.printf("[FS] cannot read: %s\r\n", path.c_str());
        if (f) f.close();
        return;
    }
    // Delimited so a captured serial log can be sliced back into a file.
    Serial.printf("[FS] ---8<--- BEGIN %s (%u bytes)\r\n",
                  path.c_str(), static_cast<unsigned>(f.size()));
    while (f.available()) {
        const int c = f.read();
        if (c < 0) break;
        Serial.write(static_cast<uint8_t>(c));
    }
    Serial.printf("\r\n[FS] ---8<--- END %s\r\n", path.c_str());
    f.close();
}

enum UsbSpoolCommandKind : uint8_t {
    USB_SPOOL_AUDIT = 0,
    USB_SPOOL_COUNT,
    USB_SPOOL_ENRICH,
    USB_SPOOL_REPAIR,
    USB_SPOOL_DIAG,
    USB_SPOOL_SELFTEST,
    USB_SPOOL_UPLOAD_REWIND
};

void _runUsbSpoolCommandInStorageWindow(UsbSpoolCommandKind command,
                                        const char* reason) {
    const char* safeReason = (reason && reason[0]) ? reason : "manual_spool";
    const RadioOwner owner = RADIO_ARB.currentOwner();

    if (owner != RADIO_NONE &&
        owner != RADIO_WIFI_CAPTURE &&
        owner != RADIO_STORAGE_MAINTENANCE) {
        Serial.printf("[SPOOL] deferred owner=%s reason=%s\r\n",
                      RadioArbiter::ownerName(owner),
                      safeReason);
        return;
    }

    bool stoppedCapture = false;
    if (owner == RADIO_WIFI_CAPTURE) {
        RADIO_ARB.release(RADIO_WIFI_CAPTURE, safeReason, false);
        stoppedCapture = true;
    }

    if (!RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE) &&
        !RADIO_ARB.requestStorageMaintenanceLease(180000UL,
                                                 safeReason,
                                                 true)) {
        Serial.printf("[SPOOL] safe window unavailable owner=%s reason=%s\r\n",
                      RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                      safeReason);
        if (stoppedCapture) {
            RADIO_ARB.ensureDefaultCapture(safeReason);
        }
        return;
    }

    StorageExclusiveWindow window;
    if (!window.begin(STORAGE_WINDOW_MAINTENANCE, safeReason)) {
        Serial.printf("[SPOOL] safe window begin failed owner=%s reason=%s\r\n",
                      RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                      safeReason);
        if (RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE)) {
            RADIO_ARB.release(RADIO_STORAGE_MAINTENANCE,
                              "spool_window_failed",
                              false);
        }
        RADIO_ARB.ensureDefaultCapture("spool_window_failed");
        return;
    }

    switch (command) {
        case USB_SPOOL_AUDIT:
            STORAGE.spoolAuditToSerial(false);
            break;
        case USB_SPOOL_COUNT:
            STORAGE.spoolCountToSerial();
            break;
        case USB_SPOOL_ENRICH:
            STORAGE.spoolEnrichToSerial();
            break;
        case USB_SPOOL_REPAIR:
            STORAGE.spoolAuditToSerial(true);
            break;
        case USB_SPOOL_DIAG:
            STORAGE.spoolDiagToSerial();
            break;
        case USB_SPOOL_SELFTEST:
            STORAGE.spoolCodecSelfTestToSerial();
            break;
        case USB_SPOOL_UPLOAD_REWIND: {
            const uint32_t rewound =
                STORAGE.rewindUploadWatermarks(nullptr, g_usbRewindToEventId);
            if (rewound == 0) {
                Serial.println("[UPLOAD] nothing to rewind");
            } else {
                Serial.printf("[UPLOAD] rewound %lu session(s) to event=%lu;"
                              " run upload now to re-send pending=%lu\r\n",
                              static_cast<unsigned long>(rewound),
                              static_cast<unsigned long>(g_usbRewindToEventId),
                              static_cast<unsigned long>(
                                  STORAGE.getPendingEventCount()));
            }
            break;
        }
    }

    window.end("done");
    if (RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE)) {
        RADIO_ARB.release(RADIO_STORAGE_MAINTENANCE,
                          "manual_spool_done",
                          false);
    }
    RADIO_ARB.ensureDefaultCapture("manual_spool_done");
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

#if MESHTASTIC_ENABLED && WIO_NRF_ACCESSORY_ENABLED
    if (lower == "mesh" || lower == "mesh status") {
        Serial.printf("[mesh] available=%d enabled=%d node=0x%08lX nodes=%u "
                      "rxText=%lu rxFrames=%lu decryptFail=%lu txText=%lu\r\n",
                      MESHTASTIC.isAvailable() ? 1 : 0,
                      MESHTASTIC.isEnabled() ? 1 : 0,
                      static_cast<unsigned long>(MESHTASTIC.nodeNum()),
                      static_cast<unsigned>(MESHTASTIC.nodeCount()),
                      static_cast<unsigned long>(MESHTASTIC.rxText()),
                      static_cast<unsigned long>(MESHTASTIC.rxFrames()),
                      static_cast<unsigned long>(MESHTASTIC.rxDecryptFail()),
                      static_cast<unsigned long>(MESHTASTIC.txText()));
        if (MESHTASTIC.lastText()[0]) {
            Serial.printf("[mesh] last text from 0x%08lX: %s\r\n",
                          static_cast<unsigned long>(MESHTASTIC.lastTextFrom()),
                          MESHTASTIC.lastText());
        }
        return;
    }
    if (lower == "mesh on") {
        Serial.printf("[mesh] enable %s\r\n", MESHTASTIC.enable() ? "ok" : "failed");
        return;
    }
    if (lower == "mesh off") {
        MESHTASTIC.disable();
        Serial.println("[mesh] disabled");
        return;
    }
    if (lower == "mesh nodes") {
        const size_t n = MESHTASTIC.nodeCount();
        Serial.printf("[mesh] %u node(s):\r\n", static_cast<unsigned>(n));
        for (size_t i = 0; i < n; ++i) {
            MeshtasticNode node;
            if (!MESHTASTIC.getNode(i, node)) {
                continue;
            }
            Serial.printf("  0x%08lX %-4s %-16s rssi=%d snr=%d hops=%u",
                          static_cast<unsigned long>(node.num),
                          node.shortName, node.longName,
                          node.rssi, node.snr, static_cast<unsigned>(node.hopsAway));
            if (node.hasPosition) {
                Serial.printf(" pos=%.5f,%.5f", node.lat, node.lon);
            }
            if (node.hasTelemetry) {
                Serial.printf(" batt=%u%% %.2fV", static_cast<unsigned>(node.batteryLevel),
                              node.voltage);
            }
            Serial.println();
        }
        return;
    }
    if (line.length() > 10 && lower.startsWith("mesh send ")) {
        const String msg = line.substring(10);  // preserve original case
        Serial.printf("[mesh] send %s\r\n",
                      MESHTASTIC.sendText(msg.c_str()) ? "ok" : "failed");
        return;
    }
#endif

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

    // ── WIO accessory commands ────────────────────────────────────────────
    if (lower == "wio status") {
        _printUsbWioStatus();
        return;
    }

    if (lower == "wio hello" || lower == "wio ping") {
        if (WIO_NRF.sendLine("SPECTRE/1 HELLO")) {
            Serial.println("[WIO] HELLO sent");
        } else {
            Serial.println("[WIO] HELLO send failed");
        }
        return;
    }

    if (lower == "wio ble status" || lower == "wio status req") {
        WIO_NRF.requestBleStatus();
        Serial.println("[WIO] BLE_STATUS requested");
        return;
    }

    if (lower == "wio ble start") {
        WIO_NRF.requestBleStart();
        Serial.println("[WIO] BLE_START requested");
        return;
    }

    if (lower == "wio drop" || lower == "wio ble drop") {
        WIO_NRF.disconnectPhone("usb_serial");
        Serial.println("[WIO] BLE_DROP requested");
        return;
    }

    if (lower == "wio probe") {
        if (!WIO_NRF.hasBleProxy()) {
            Serial.println("[WIO] BLE proxy unavailable");
            return;
        }
        if (WIO_NRF.requestCompanionLink("serial_wio_probe", true)) {
            Serial.println("[WIO] probe requested");
        } else {
            Serial.println("[WIO] probe request failed");
        }
        return;
    }

    if (lower == "wio enrich") {
        if (!PHONE_COMPANION_ENABLED) {
            Serial.println("[WIO] companion not enabled (PHONE_COMPANION_ENABLED=false)");
            return;
        }
        if (!WIO_NRF.hasBleProxy()) {
            Serial.println("[WIO] BLE proxy unavailable");
            return;
        }
        STORAGE.releaseUploadIndexMemory("manual_wio_enrich_start");
        g_companionCmd.enrich = true;
        Serial.println("[WIO] manual enrich queued");
        return;
    }

    if (lower == "wio text cancel") {
        WIO_NRF.cancelTextInput("usb_serial");
        Serial.println("[WIO] text input cancelled");
        return;
    }

    if (lower.startsWith("wio text ")) {
        String prompt = line.substring(strlen("wio text "));
        prompt.trim();
        if (prompt.length() == 0) {
            Serial.println("[WIO] text prompt required");
            return;
        }
        if (WIO_NRF.requestTextInput(prompt.c_str())) {
            Serial.println("[WIO] text input requested");
        } else {
            Serial.println("[WIO] text input request failed");
        }
        return;
    }

    if (lower.startsWith("wio raw ")) {
        String payload = line.substring(strlen("wio raw "));
        payload.trim();
        if (payload.length() == 0) {
            Serial.println("[WIO] raw line required");
            return;
        }
        if (WIO_NRF.sendLine(payload.c_str())) {
            Serial.println("[WIO] raw line sent");
        } else {
            Serial.println("[WIO] raw line send failed");
        }
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
        Serial.printf("[COMP] external accessory=%d bleProxy=%d phoneConnected=%d phone=%s rssi=%d ageMs=%lu\r\n",
                      s.externalAccessory ? 1 : 0,
                      s.externalBleProxy ? 1 : 0,
                      s.externalPhoneConnected ? 1 : 0,
                      s.externalPhone[0] ? s.externalPhone : "unknown",
                      s.externalPhoneRssi,
                      static_cast<unsigned long>(s.externalLastSeenAgeMs));
        Serial.printf("[COMP] wio link=%s sx1262=%d textInput=%d\r\n",
                      s.externalLinkState[0] ? s.externalLinkState : "n/a",
                      s.externalSx1262Present ? 1 : 0,
                      s.externalTextInputPending ? 1 : 0);
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
        Serial.printf("[COMP] hold manual=%d autoOffload=%d managerActive=%d prep=%d deadlineInMs=%lu retryInMs=%lu\r\n",
                      s.manualLinkHold ? 1 : 0,
                      s.automaticOffloadHold ? 1 : 0,
                      s.offloadManagerActive ? 1 : 0,
                      s.offloadPreparationPending ? 1 : 0,
                      static_cast<unsigned long>(s.offloadDeadlineInMs),
                      static_cast<unsigned long>(s.offloadRetryInMs));
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

    if (lower == "companion link") {
        if (!PHONE_COMPANION_ENABLED) {
            Serial.println("[COMP] companion not enabled (PHONE_COMPANION_ENABLED=false)");
            return;
        }
        g_companionCmd.link = true;
        Serial.println("[COMP] persistent link queued");
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

    if (lower == "companion offload prep") {
        if (!RADIO_ARB.isOwner(RADIO_BLE_GPS) ||
            !BLE_MGR.isPhoneCompanionReady()) {
            Serial.printf("[COMP] offload prep refused owner=%s phoneReady=%d\r\n",
                          RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                          BLE_MGR.isPhoneCompanionReady() ? 1 : 0);
            return;
        }
        CmdOffloadBeginResponseV1 response = {};
        const bool ready = PHONE_OFFLOAD.begin(response);
        Serial.printf("[COMP] offload prep requested ready=%d prep=%d active=%d indexed=%lu pending=%lu\r\n",
                      ready ? 1 : 0,
                      PHONE_OFFLOAD.preparationPending() ? 1 : 0,
                      PHONE_OFFLOAD.active() ? 1 : 0,
                      static_cast<unsigned long>(response.indexedTotal),
                      static_cast<unsigned long>(response.pendingTotal));
        return;
    }

    if (lower == "companion cancel") {
        g_companionCmd.cancel = true;
        Serial.println("[COMP] cancel queued");
        return;
    }

    if (lower == "time" || lower == "time status" || lower == "time sync") {
        char iso[24] = {};
        const bool haveIso = TIME_SVC.formatNowIso(iso, sizeof(iso));
        Serial.printf("[TIME] valid=%d accurate=%d source=%s utc=%s\r\n",
                      TIME_SVC.isTimeValid() ? 1 : 0,
                      TIME_SVC.hasAccurateUtc() ? 1 : 0,
                      TIME_SVC.sourceName(),
                      haveIso ? iso : "--");
        Serial.printf("[TIME] lastNtpAttempt=%s age=%lus\r\n",
                      TIME_SVC.lastAttemptReason(),
                      TIME_SVC.everAttempted()
                          ? TIME_SVC.lastAttemptAgeMs() / 1000UL
                          : 0UL);
        if (lower == "time sync") {
            if (TIME_SVC.hasAccurateUtc()) {
                Serial.println("[TIME] already accurate; skipping forced NTP");
            } else {
                Serial.println("[TIME] forcing NTP acquisition (preempts capture ~5-12s)...");
                const bool ok = _forceQuickNtp("serial_time_sync");
                Serial.printf("[TIME] forced NTP %s reason=%s\r\n",
                              ok ? "ok" : "failed",
                              TIME_SVC.lastAttemptReason());
            }
        }
        return;
    }

    if (lower == "heap status" || lower == "mem status") {
        const auto kb = [](uint32_t bytes) -> uint32_t {
            return (bytes + 512UL) / 1024UL;
        };
        const uint32_t totalHeap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        const uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        const uint32_t largestHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        const uint32_t totalInternal =
            heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const uint32_t freeInternal =
            heap_caps_get_free_size(SPECTRE_CAP_DRAM);
        const uint32_t largestInternal =
            heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM);
        const uint32_t totalPsram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        const uint32_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const uint32_t largestPsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        Serial.printf("[HEAP] 8bit total=%luKB free=%luKB largest=%luKB\r\n",
                      static_cast<unsigned long>(kb(totalHeap)),
                      static_cast<unsigned long>(kb(freeHeap)),
                      static_cast<unsigned long>(kb(largestHeap)));
        Serial.printf("[HEAP] internal total=%luKB free=%luKB largest=%luKB\r\n",
                      static_cast<unsigned long>(kb(totalInternal)),
                      static_cast<unsigned long>(kb(freeInternal)),
                      static_cast<unsigned long>(kb(largestInternal)));
        // RTC slow-memory heap is reported separately: it is only touched as a
        // last-resort spill, so any drop below its idle ~7.6 KB means DRAM ran dry.
        Serial.printf("[HEAP] dram low-water observed=%luB (sampled global min)\r\n",
                      static_cast<unsigned long>(
                          g_dramMinObserved == UINT32_MAX ? 0UL : g_dramMinObserved));
        Serial.printf("[HEAP] rtc-slow free=%luKB (spill indicator; DRAM dry if falling)\r\n",
                      static_cast<unsigned long>(
                          (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) -
                           heap_caps_get_free_size(SPECTRE_CAP_DRAM)) / 1024));
        Serial.printf("[HEAP] psram total=%luKB free=%luKB largest=%luKB\r\n",
                      static_cast<unsigned long>(kb(totalPsram)),
                      static_cast<unsigned long>(kb(freePsram)),
                      static_cast<unsigned long>(kb(largestPsram)));
        Serial.printf("[HEAP] uploadIndex=%d enrichWindow=%d maint=%s owner=%s\r\n",
                      STORAGE.isUploadIndexResident() ? 1 : 0,
                      STORAGE.isEnrichmentWindowResident() ? 1 : 0,
                      STORAGE.maintenanceFlagsText(),
                      RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
        return;
    }

    if (lower == "entity status" || lower == "entities") {
        const EntitySummary entities = ENTITY_MGR.snapshot();
        Serial.printf("[ENTITY] total=%lu nearby=%lu observations=%lu located=%lu locationUpdates=%lu dropped=%lu linkDrops=%lu selftest=%s capacity=%u links=%lu psram=%luKB\r\n",
                      static_cast<unsigned long>(entities.total),
                      static_cast<unsigned long>(entities.nearby),
                      static_cast<unsigned long>(entities.observations),
                      static_cast<unsigned long>(entities.located),
                      static_cast<unsigned long>(entities.locationUpdates),
                      static_cast<unsigned long>(entities.dropped),
                      static_cast<unsigned long>(entities.eventLinksDropped),
                      entities.selfTestPassed ? "pass" : "FAIL",
                      static_cast<unsigned>(EntityManager::MAX_ENTITIES),
                      static_cast<unsigned long>(EntityManager::MAX_EVENT_LINKS),
                      static_cast<unsigned long>(ENTITY_MGR.psramBytes() / 1024UL));
        Serial.printf("[ENTITY] ap=%u clients=%u drones=%u subghz=%u ble=%u closest=%s name=%s rssi=%d channel=%u located=%d\r\n",
                      static_cast<unsigned>(entities.byKind[ENTITY_WIFI_AP]),
                      static_cast<unsigned>(entities.byKind[ENTITY_WIFI_CLIENT]),
                      static_cast<unsigned>(entities.byKind[ENTITY_DRONE]),
                      static_cast<unsigned>(entities.byKind[ENTITY_SUBGHZ]),
                      static_cast<unsigned>(entities.byKind[ENTITY_BLE]),
                      entities.closestIdentity[0] ? entities.closestIdentity : "-",
                      entities.closestName[0] ? entities.closestName : "-",
                      static_cast<int>(entities.closestRssi),
                      static_cast<unsigned>(entities.closestChannel),
                      entities.closestLocated ? 1 : 0);
        Serial.printf("[ENTITY] session networks=%lu devices=%lu probes=%lu pmkids=%lu drones=%lu\r\n",
                      static_cast<unsigned long>(entities.sessionNetworks),
                      static_cast<unsigned long>(entities.sessionDevices),
                      static_cast<unsigned long>(entities.sessionProbes),
                      static_cast<unsigned long>(entities.sessionPmkids),
                      static_cast<unsigned long>(entities.sessionDrones));
        return;
    }


#if BLE_SMOKE_ENABLED
    // ── BLE smoke test ───────────────────────────────────────────────────────
    // Walk the internal heap block-by-block to answer "why is largest free only
    // ~15 KB when 60-90 KB is free". heap_caps_print_heap_info() writes via
    // printf, which does not reach the USB CDC on this build.
    //
    // The walker callback runs with the heap lock held, so it must not print or
    // allocate - doing so hung TaskHardware into a watchdog reset. Records are
    // collected into a fixed static buffer and printed after the walk returns.
    // Move the internal/external malloc split point at runtime.
    //
    // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 forces every plain malloc() of
    // 4 KB or less into internal DRAM. The heap map shows ~35 KB of live blocks
    // sitting in the 1-4 KB band, so lowering the limit lets them land in PSRAM
    // instead. Only affects allocations made AFTER the call, and only plain
    // malloc/calloc/new - anything requesting MALLOC_CAP_INTERNAL or
    // MALLOC_CAP_DMA explicitly (task stacks, driver DMA buffers) is unaffected.
    if (lower.startsWith("heap extmem")) {
        const int sp = lower.indexOf(' ', 11);
        long limit = -1;
        if (sp > 0) limit = lower.substring(sp + 1).toInt();
        if (limit < 0) {
            Serial.println("[EXTMEM] usage: heap extmem <bytes>  (current default 4096)");
            return;
        }
        const uint32_t before = heap_caps_get_free_size(SPECTRE_CAP_DRAM);
        heap_caps_malloc_extmem_enable(static_cast<size_t>(limit));
        Serial.printf("[EXTMEM] split point set to %ld B (dram free now %luB; "
                      "effect appears as allocations churn)\r\n",
                      limit, (unsigned long)before);
        Serial.flush();
        return;
    }

    if (lower == "heap map" || lower == "heap frag") {
        static constexpr size_t MAP_MAX = 48;
        struct GapRec {
            uint32_t addr;
            uint32_t size;
            uint32_t allocBytesBefore;
            uint32_t allocBlocksBefore;
        };
        static GapRec  s_gaps[MAP_MAX];
        static GapRec  s_allocs[MAP_MAX];   // largest allocated blocks
        static uint32_t s_regionStart[8];
        static uint32_t s_regionEnd[8];

        struct MapCtx {
            uint32_t  gapCount;
            uint32_t  regionCount;
            uint32_t  totalGaps;
            uint32_t  allocRunBytes;
            uint32_t  allocRunBlocks;
            uint32_t  allocCount;
            uint32_t  smallestKept;
            uintptr_t curRegion;
        };
        MapCtx ctx = {};

        auto walker = [](walker_heap_into_t heap, walker_block_info_t blk, void* ud) -> bool {
            MapCtx* c = static_cast<MapCtx*>(ud);
            if (static_cast<uintptr_t>(heap.start) != c->curRegion) {
                c->curRegion = static_cast<uintptr_t>(heap.start);
                if (c->regionCount < 8) {
                    s_regionStart[c->regionCount] = static_cast<uint32_t>(heap.start);
                    s_regionEnd[c->regionCount]   = static_cast<uint32_t>(heap.end);
                }
                c->regionCount++;
                c->allocRunBytes = 0;
                c->allocRunBlocks = 0;
            }

            if (blk.used) {
                c->allocRunBytes += blk.size;
                c->allocRunBlocks++;
                // Keep the MAP_MAX largest allocated blocks: insertion into a
                // small array beats sorting 350+ entries under the heap lock.
                if (blk.size >= 1024) {
                    uint32_t slot = c->allocCount;
                    if (slot < MAP_MAX) {
                        c->allocCount++;
                    } else {
                        uint32_t smallest = 0;
                        for (uint32_t i = 1; i < MAP_MAX; i++) {
                            if (s_allocs[i].size < s_allocs[smallest].size) smallest = i;
                        }
                        if (s_allocs[smallest].size >= blk.size) return true;
                        slot = smallest;
                    }
                    s_allocs[slot].addr = static_cast<uint32_t>(
                        reinterpret_cast<uintptr_t>(blk.ptr));
                    s_allocs[slot].size = static_cast<uint32_t>(blk.size);
                    s_allocs[slot].allocBytesBefore = 0;
                    s_allocs[slot].allocBlocksBefore = 0;
                }
                return true;
            }

            c->totalGaps++;
            if (blk.size >= 1024 && c->gapCount < MAP_MAX) {
                s_gaps[c->gapCount].addr = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(blk.ptr));
                s_gaps[c->gapCount].size = static_cast<uint32_t>(blk.size);
                s_gaps[c->gapCount].allocBytesBefore  = c->allocRunBytes;
                s_gaps[c->gapCount].allocBlocksBefore = c->allocRunBlocks;
                c->gapCount++;
            }
            c->allocRunBytes = 0;
            c->allocRunBlocks = 0;
            return true;
        };

        heap_caps_walk(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, walker, &ctx);

        multi_heap_info_t info = {};
        heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        Serial.printf("[HEAPMAP] free=%u largest=%u minFree=%u blocks free=%u alloc=%u total=%u\r\n",
                      (unsigned)info.total_free_bytes,
                      (unsigned)info.largest_free_block,
                      (unsigned)info.minimum_free_bytes,
                      (unsigned)info.free_blocks,
                      (unsigned)info.allocated_blocks,
                      (unsigned)info.total_blocks);
        for (uint32_t i = 0; i < ctx.regionCount && i < 8; i++) {
            Serial.printf("[HEAPMAP] region %u 0x%08X-0x%08X (%u B)\r\n",
                          (unsigned)(i + 1), (unsigned)s_regionStart[i],
                          (unsigned)s_regionEnd[i],
                          (unsigned)(s_regionEnd[i] - s_regionStart[i]));
        }
        Serial.printf("[HEAPMAP] gaps>=1KB listed=%u of %u total free blocks\r\n",
                      (unsigned)ctx.gapCount, (unsigned)ctx.totalGaps);
        Serial.printf("[HEAPMAP] largest allocated blocks (>=1KB, top %u)\r\n",
                      (unsigned)ctx.allocCount);
        // simple selection print, largest first
        for (uint32_t n = 0; n < ctx.allocCount; n++) {
            uint32_t best = 0;
            bool found = false;
            for (uint32_t i = 0; i < ctx.allocCount; i++) {
                if (s_allocs[i].size == 0) continue;
                if (!found || s_allocs[i].size > s_allocs[best].size) { best = i; found = true; }
            }
            if (!found) break;
            Serial.printf("  ALLOC %6u B @0x%08X\r\n",
                          (unsigned)s_allocs[best].size, (unsigned)s_allocs[best].addr);
            s_allocs[best].size = 0;
            Serial.flush();
        }

        for (uint32_t i = 0; i < ctx.gapCount; i++) {
            Serial.printf("  FREE %6u B @0x%08X  preceded by %u alloc blocks / %u B\r\n",
                          (unsigned)s_gaps[i].size, (unsigned)s_gaps[i].addr,
                          (unsigned)s_gaps[i].allocBlocksBefore,
                          (unsigned)s_gaps[i].allocBytesBefore);
            Serial.flush();
        }
        Serial.flush();
        return;
    }

    if (lower == "ble memprobe") {
        _runBleMemProbe(true);
        return;
    }

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
        _runUsbSpoolCommandInStorageWindow(USB_SPOOL_AUDIT, "manual_spool_audit");
        return;
    }

    if (lower == "spool count") {
        _runUsbSpoolCommandInStorageWindow(USB_SPOOL_COUNT, "manual_spool_count");
        return;
    }

    if (lower == "spool enrich") {
        _runUsbSpoolCommandInStorageWindow(USB_SPOOL_ENRICH, "manual_spool_enrich");
        return;
    }

    if (lower == "spool repair") {
        _runUsbSpoolCommandInStorageWindow(USB_SPOOL_REPAIR, "manual_spool_repair");
        return;
    }

    if (lower == "spool diag") {
        _runUsbSpoolCommandInStorageWindow(USB_SPOOL_DIAG, "manual_spool_diag");
        return;
    }

    if (lower == "antenna gain" || lower.startsWith("antenna gain ")) {
        if (lower.length() > 13) {
            // Accept plain dBi with an optional fraction: "9", "2.15", "-1.5".
            const float dbi = line.substring(13).toFloat();
            if (dbi < -32.0f || dbi > 31.5f) {
                Serial.println("[ANT] gain out of range (-32.0 .. 31.5 dBi)");
                return;
            }
            const int8_t q2 = static_cast<int8_t>(lroundf(dbi * 4.0f));
            if (!SETTINGS.setAntennaGainQ2(q2)) {
                Serial.println("[ANT] failed to save antenna gain");
                return;
            }
        }
        const int8_t q2 = SETTINGS.get().antennaGainQ2;
        Serial.printf("[ANT] antenna gain = %.2f dBi (q2=%d)\r\n",
                      static_cast<double>(q2) / 4.0, static_cast<int>(q2));
        Serial.println("[ANT] recorded on every capture record; set it whenever "
                       "you swap antennas or RSSI comparisons will be offset");
        return;
    }

    if (lower == "fs ls" || lower.startsWith("fs ls ")) {
        const String dir = (line.length() > 6) ? line.substring(6) : String("/");
        _usbFsList(dir);
        return;
    }

    if (lower.startsWith("fs cat ")) {
        _usbFsCat(line.substring(7));
        return;
    }

    if (lower == "fs backup") {
        // Everything on LittleFS that a repartition would destroy and that
        // cannot be regenerated from NVS or re-captured in the field.
        static const char* kBackupPaths[] = {
            PATH_STORE_KNOWN_LOCATIONS,
            PATH_STORE_LEGACY_KNOWN_LOCATIONS,
            PATH_BADUSB_INDEX,
        };
        Serial.println("[FS] backup begin");
        for (const char* pth : kBackupPaths) {
            if (LittleFS.exists(pth)) _usbFsCat(String(pth));
        }
        _usbFsList(String(PATH_BADUSB_DIR));
        Serial.println("[FS] backup end -- capture this log before repartitioning");
        return;
    }

    if (lower == "spool selftest" || lower == "spool codec") {
        _runUsbSpoolCommandInStorageWindow(USB_SPOOL_SELFTEST,
                                           "manual_spool_selftest");
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

    if (lower == "fieldvault dump" ||
        lower == "fieldvault peek" ||
        lower == "fieldvault dump keep" ||
        lower == "fieldvault show") {
        FieldVault::dumpToSerial();
        Serial.println("[FIELD] retained after dump");
        return;
    }

    if (lower == "fieldvault clear" ||
        lower == "fieldvault dump clear") {
        if (FieldVault::clearRetained()) {
            Serial.println("[FIELD] cleared");
        } else {
            Serial.println("[FIELD] clear failed");
        }
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

    if (lower == "crash log" || lower == "crash ring") {
        crashLogPrint();
        return;
    }

    if (lower == "stack audit" || lower == "stacks") {
        _auditTaskStackPlacement(true);
        return;
    }

    if (lower == "crash clear" || lower == "crash reset") {
        crashLogClear();
        // The ring's sequence restarts at 0, so the vault's dedup watermark
        // must go with it or every future crash is silently deduped away.
        FieldVault::resetCrashWatermark();
        return;
    }

    if (lower == "ble rxdiag") {
        BLE_MGR.printRxCrashDiag();
        return;
    }

    if (lower == "upload now" ||
        lower == "upload start" ||
        lower == "upload force" ||
        lower == "mqtt upload" ||
        lower == "mqtt dump" ||
        lower == "dump now") {
        const MQTTState state = MQTT_MGR.getState();
        const bool backlogTrusted =
            !STORAGE.isReady() || STORAGE.isPendingEventCountAuthoritative();
        const int pending =
            backlogTrusted
                ? MQTT_MGR.uploadReadyCount()
                : static_cast<int>(STORAGE.getPendingEventCount());

        if (MQTT_MGR.uploadStoppedBySerial()) {
            Serial.println("[UPLOAD] paused; run upload resume first");
            return;
        }

        if (state != MQTT_IDLE) {
            Serial.printf("[UPLOAD] busy state=%s pending=%d\r\n",
                          _mqttStateName(state),
                          pending);
            return;
        }

        if (!backlogTrusted) {
            Serial.printf("[UPLOAD] pending count not authoritative; forcing indexed recovery drain pending=%d\r\n",
                          pending);
        }

        if (pending <= 0) {
            Serial.println("[UPLOAD] no pending records");
            return;
        }

        g_companionCmd.cancel = true;
        WIO_NRF.disconnectPhone("manual_upload_start");
        initEnrichQueue();
        enrichClearAllClaims();
        STORAGE.releaseEnrichmentIndexMemory("manual_upload_start");
        STORAGE.releaseUploadIndexMemory("manual_upload_start");
        if (MQTT_MGR.requestDump(true)) {
            Serial.printf("[UPLOAD] manual upload queued pending=%d\r\n", pending);
        } else {
            Serial.printf("[UPLOAD] request unavailable state=%s owner=%s pending=%d\r\n",
                          _mqttStateName(MQTT_MGR.getState()),
                          RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                          pending);
        }
        return;
    }

    if (lower == "upload rewind" || lower.startsWith("upload rewind ")) {
        if (!STORAGE.isReady()) {
            Serial.println("[UPLOAD] storage not ready");
            return;
        }
        if (MQTT_MGR.getState() != MQTT_IDLE || STORAGE.isUploadBatchActive()) {
            Serial.println("[UPLOAD] busy; stop the upload first");
            return;
        }

        g_usbRewindToEventId = 0;
        if (lower.length() > 14) {
            g_usbRewindToEventId = static_cast<uint32_t>(
                strtoul(lower.substring(14).c_str(), nullptr, 10));
        }

        // The rewind recounts pending from the spool, which is a maintenance
        // operation — run it under the same storage window the spool commands
        // use so the recount doesn't fire a contract violation against the
        // live capture owner.
        _runUsbSpoolCommandInStorageWindow(USB_SPOOL_UPLOAD_REWIND,
                                           "manual_upload_rewind");
        return;
    }

    if (lower == "upload stop" || lower == "mqtt stop" || lower == "dump stop") {
        if (MQTT_MGR.requestUploadStop("usb_serial")) {
            Serial.println("[UPLOAD] stop requested; automatic uploads paused");
        } else {
            Serial.println("[UPLOAD] automatic uploads paused; no active upload");
        }
        return;
    }

    if (lower == "upload resume" || lower == "mqtt resume" || lower == "dump resume") {
        if (MQTT_MGR.requestUploadResume("usb_serial")) {
            Serial.println("[UPLOAD] automatic uploads resumed");
        } else {
            Serial.println("[UPLOAD] automatic uploads already enabled");
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

    // Also fan out to the phone (slice #7).  NotificationCenter throttles +
    // dedups internally and drops silently if no transport is ready, so this
    // is safe to call on every notification regardless of phone state.
    NOTIF_CENTER.enqueue(type, text);
}

bool _requestBleTextEntry(const char* leaseReason, const char* prompt) {
    if (!leaseReason || !prompt) {
        return false;
    }

    if (PHONE_XPORT.isWioActive()) {
        // No radio lease needed: WIO drives its own BLE radio.
        return PHONE_XPORT.requestTextInput(prompt);
    }

    if (!RADIO_ARB.requestLease(RADIO_BLE_TEXT,
                                RadioArbiter::BLE_TEXT_ACTIVE_HOLD_MS,
                                leaseReason)) {
        return false;
    }

    if (PHONE_XPORT.requestTextInput(prompt)) {
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
    snapshot.meshAvailable = g_state.meshAvailable;
    snapshot.meshEnabled = g_state.meshEnabled;
    snapshot.meshNodeCount = g_state.meshNodeCount;
    snapshot.meshRxText = g_state.meshRxText;
    snapshot.meshTxText = g_state.meshTxText;
    snapshot.meshNodeNum = g_state.meshNodeNum;
    snapshot.meshLastFrom = g_state.meshLastFrom;
    strlcpy(snapshot.meshLastText, g_state.meshLastText, sizeof(snapshot.meshLastText));
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

void _peekDisplayPowerState(DisplayPowerState& snapshot) {
    STATE_READ_BEGIN();
    snapshot.requestSleep = g_state.requestSleep;
    snapshot.textInputPending = g_state.textInputPending;
    snapshot.wifiListActive = g_state.wifiListActive;
    snapshot.missionListActive = g_state.missionListActive;
    snapshot.badUsbListActive = g_state.badUsbListActive;
    snapshot.debriefActive = g_state.debriefActive;
    snapshot.badUsbArmed = g_state.badUsbArmed;
    snapshot.badUsbRunning = g_state.badUsbRunning;
    snapshot.powerState = g_state.powerState;
    STATE_READ_END();
}

bool _shouldKeepDisplayAwake(const DisplayPowerState& snapshot,
                             uint32_t now,
                             uint32_t displayTimeoutMs) {
    return snapshot.requestSleep ||
           snapshot.textInputPending ||
           snapshot.wifiListActive ||
           snapshot.missionListActive ||
           snapshot.badUsbListActive ||
           snapshot.debriefActive ||
           snapshot.badUsbArmed ||
           snapshot.badUsbRunning ||
           (displayTimeoutMs == 0) ||
           ((now - _lastUiActivityMs()) < displayTimeoutMs);
}

bool _shouldKeepDisplayAwake(const DisplayFrameState& snapshot,
                             uint32_t now,
                             uint32_t displayTimeoutMs) {
    DisplayPowerState powerSnapshot;
    powerSnapshot.requestSleep = snapshot.requestSleep;
    powerSnapshot.textInputPending = snapshot.textInputPending;
    powerSnapshot.wifiListActive = snapshot.wifiListActive;
    powerSnapshot.missionListActive = snapshot.missionListActive;
    powerSnapshot.badUsbListActive = snapshot.badUsbListActive;
    powerSnapshot.debriefActive = snapshot.debriefActive;
    powerSnapshot.badUsbArmed = snapshot.badUsbArmed;
    powerSnapshot.badUsbRunning = snapshot.badUsbRunning;
    powerSnapshot.powerState = snapshot.powerState;
    return _shouldKeepDisplayAwake(powerSnapshot, now, displayTimeoutMs);
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
    const uint32_t pending =
        storageOk ? STORAGE.getPendingEventCount() : 0U;
    const uint32_t storedRecords =
        storageOk ? STORAGE.getDisplayRecordCount() : 0U;
    const uint32_t storedEvents =
        storageOk ? STORAGE.getDisplayEventCount() : 0U;
    const StorageLaneCounts pendingUpload =
        storageOk ? STORAGE.getPendingUploadCounts() : StorageLaneCounts{};

    StorageUiMirror::publishReadyState(storageOk,
                                       storageOk ? storageUsed.c_str() : nullptr,
                                       pending,
                                       pendingUpload.mission,
                                       pendingUpload.noise,
                                       storedEvents,
                                       storedRecords);
    _publishStorageMaintenanceMirror(storageOk,
                                     storageOk &&
                                         RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE));
}

// Explicit, operator-requested NTP grab. This may block for up to 12 seconds,
// so automatic boot/runtime paths must not call it. Passive TimeService::tick()
// still accepts phone GPS immediately and NTP whenever WiFi is already online.
static bool _forceQuickNtp(const char* reason) {
    if (TIME_SVC.hasAccurateUtc()) {
        return true;
    }
    if (!RADIO_ARB.requestUploadLease(12000UL,
                                      reason ? reason : "force_ntp", true)) {
        TIME_SVC.recordAttempt("lease_denied");
        DLOG_WARN("TIME", "Force NTP lease denied owner=%s",
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
        return false;
    }
    const bool ok = TIME_SVC.acquireUtcFromSavedWiFi(12000UL);
    RADIO_ARB.release(RADIO_WIFI_UPLOAD,
                      ok ? "force_ntp_done" : "force_ntp_failed", false);
    RADIO_ARB.ensureDefaultCapture(ok ? "force_ntp_done" : "force_ntp_retry");
    return ok;
}

static void _refreshStorageCounterMirror() {
    if (!STORAGE.isReady()) {
        _publishStorageMaintenanceMirror(false, false);
        return;
    }

    const uint32_t now = millis();
    (void)STORAGE.refreshStoredRecordCountCache(now, false);

    const uint32_t pending = STORAGE.getPendingEventCount();
    const uint32_t storedRecords = STORAGE.getDisplayRecordCount();
    const uint32_t storedEvents = STORAGE.getDisplayEventCount();
    const StorageLaneCounts pendingUpload = STORAGE.getPendingUploadCounts();

    StorageUiMirror::publishCounters(pending,
                                     pendingUpload.mission,
                                     pendingUpload.noise,
                                     storedEvents,
                                     storedRecords);
    _publishStorageMaintenanceMirror(true,
                                     RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE));
}

static void _storageMaintenanceCompactText(uint32_t flags,
                                           char* out,
                                           size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    out[0] = '\0';
    if (flags == STORAGE_MAINT_NONE) {
        strlcpy(out, "none", outSize);
        return;
    }

    auto append = [&](const char* text) {
        if (!text || !text[0]) return;
        if (out[0]) strlcat(out, "+", outSize);
        strlcat(out, text, outSize);
    };

    if (flags & (STORAGE_MAINT_EMERGENCY_REPAIR |
                 STORAGE_MAINT_COUNTER_UNTRUSTED |
                 STORAGE_MAINT_ACTIVE_SEGMENT_INVALID |
                 STORAGE_MAINT_SEGMENT_AUDIT)) {
        append("repair");
    }
    if (flags & (STORAGE_MAINT_DIRTY_SPOOL_INDEX |
                 STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST |
                 STORAGE_MAINT_SNAPSHOT_LAGGED |
                 STORAGE_MAINT_BINARY_CHECKPOINT)) {
        append("index");
    }
    if (flags & STORAGE_MAINT_DIRTY_SUMMARY) {
        append("summary");
    }
    if (flags & STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY) {
        append("cursor");
    }
    if (flags & (STORAGE_MAINT_ACTIVE_SEGMENT_NEAR_FULL |
                 STORAGE_MAINT_DELETE_DRAINED)) {
        append("compact");
    }
    if (flags & STORAGE_MAINT_CAPTURE_INDEX_DIRTY) {
        append("pmkid");
    }
    if (flags & STORAGE_MAINT_FS_AUDIT) {
        append("audit");
    }
    if (!out[0]) {
        strlcpy(out, "queued", outSize);
    }
}

static void _publishStorageMaintenanceMirror(bool storageReady,
                                             bool running,
                                             bool ranWindow,
                                             uint32_t durationMs) {
    const uint32_t flags = storageReady ? STORAGE.maintenanceFlags() : STORAGE_MAINT_NONE;
    const bool captureSafe =
        !storageReady || STORAGE.isCaptureSafeToResume();
    char text[32] = {};
    _storageMaintenanceCompactText(flags, text, sizeof(text));

    StorageUiMirror::publishMaintenance(storageReady,
                                        running,
                                        ranWindow,
                                        durationMs,
                                        captureSafe,
                                        flags,
                                        text);
}

static bool _shouldEnterBootHeapTriage(bool storageOk) {
    if (!storageOk || !STORAGE.isReady()) {
        return false;
    }

    const uint32_t pending = STORAGE.getPendingEventCount();
    const uint32_t freeInternal = _sampleDramFree();
    const uint32_t largestInternal =
        heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM);

    if (pending >= BOOT_TRIAGE_PENDING_UPLOAD_THRESHOLD ||
        freeInternal < BOOT_TRIAGE_FREE_INTERNAL_HEAP_BYTES ||
        largestInternal < BOOT_TRIAGE_LARGEST_INTERNAL_HEAP_BYTES) {
        DLOG_WARN("CORE",
                  "Boot heap triage requested pending=%lu freeInternal=%lu largestInternal=%lu",
                  static_cast<unsigned long>(pending),
                  static_cast<unsigned long>(freeInternal),
                  static_cast<unsigned long>(largestInternal));
        return true;
    }

    return false;
}

static void _publishBootHeapTriageState(uint32_t pending,
                                        uint32_t freeInternal,
                                        uint32_t largestInternal) {
    STATE_WRITE_BEGIN();
    g_state.hwInitDone = true;
    StorageUiMirror::writeReady_locked(true);
    g_state.radioBusy = false;
    g_state.uploadActive = false;
    g_state.uploadPercent = 0;
    g_state.uploadPublished = 0;
    g_state.uploadTotal = pending;
    strlcpy(g_state.uploadPhase, "TRIAGE", sizeof(g_state.uploadPhase));
    g_state.sessionFilesPending = static_cast<int>(pending);
    g_state.kaliSyncAvailable = pending > 0;
    g_state.kaliSyncPending = pending > 0;
    g_state.radioOwner = RADIO_NONE;
    g_state.screenChanged = true;
    g_state.dataRefresh = true;
    StorageUiMirror::writeStr_locked("triage");
    STATE_WRITE_END();

    DLOG_WARN("CORE",
              "Boot heap triage active: capture/upload/RAMSpool disabled pending=%lu freeInternal=%lu largestInternal=%lu",
              static_cast<unsigned long>(pending),
              static_cast<unsigned long>(freeInternal),
              static_cast<unsigned long>(largestInternal));
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

static void _runOptionalBootInitialization() {
    DebugLog::begin();
    // Continuous field captures reconnect after USB re-enumerates, so emit the
    // crash-latched BLE stage here (rather than only during the first 250 ms of
    // setup) while it is still protected from live BLE marker updates.
    BLE_MGR.printRxCrashDiag();
    FieldVault::begin();
    _loadKnownLocationsIntoState();
    if (STORAGE.hasStorageMaintenanceWork()) {
        _clearStorageSummaryMirror();
        DLOG_INFO("STORAGE", "Initial storage summary deferred until maintenance completes");
    } else {
        _refreshStorageSummaryMirror(true);
    }
    if (!STORAGE.isCaptureSafeToResume()) {
        STORAGE.requestMaintenance(STORAGE_MAINT_FS_AUDIT,
                                   "boot_fs_audit_recovery");
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

#if MESHTASTIC_ENABLED && WIO_NRF_ACCESSORY_ENABLED
void _applyMeshStatusToState() {
    STATE_WRITE_BEGIN();
    g_state.meshAvailable = MESHTASTIC.isAvailable();
    g_state.meshEnabled = MESHTASTIC.isEnabled();
    g_state.meshNodeCount = static_cast<int>(MESHTASTIC.nodeCount());
    g_state.meshRxText = MESHTASTIC.rxText();
    g_state.meshTxText = MESHTASTIC.txText();
    g_state.meshNodeNum = MESHTASTIC.nodeNum();
    g_state.meshLastFrom = MESHTASTIC.lastTextFrom();
    strlcpy(g_state.meshLastText, MESHTASTIC.lastText(), sizeof(g_state.meshLastText));
    STATE_WRITE_END();
}
#endif

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

    // Signed-delta deadline compares for wraparound safety.
    if (forceAtMs != 0 &&
        static_cast<int32_t>(nowMs - forceAtMs) >= 0) {
        _enterDeepSleepNow(presentationAcked ? "display_timeout_cap"
                                             : "display_timeout");
    }

    if (presentationAcked && commitAtMs != 0 &&
        static_cast<int32_t>(nowMs - commitAtMs) >= 0) {
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

    // Best-effort: quiet the WIO nRF/SX1262 accessory before we sleep. The
    // module has no power-enable pin and the bridge protocol has no nRF
    // system-off verb, so firmware can't cut its rail — but we can stop the
    // SX1262 from burning RX current indefinitely with nobody servicing it,
    // and drop any BLE link. Both are fire-and-forget UART lines; the sleep-
    // presentation hold (>=SLEEP_PRESENTATION_HOLD_MS) lets them flush before
    // esp_deep_sleep_start(). (True near-zero "off" for the module needs a
    // hardware load switch on its supply rail — see POWER notes.)
    if (WIO_NRF.subghzAvailable()) {
        WIO_NRF.subghzSetMode(WioNrfAccessory::SUBGHZ_MODEM_OFF);
    }
    WIO_NRF.disconnectPhone("sleep");

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
        case POWER_STATE_BATTERY_NORMAL:   return 70UL;
        case POWER_STATE_BATTERY_ECONOMY:  return 125UL;
        case POWER_STATE_BATTERY_CRITICAL: return 180UL;
        case POWER_STATE_USB:
        default:                           return DISPLAY_FRAME_INTERVAL_MS;
    }
}

uint32_t _displayMascotIntervalForPowerState(uint8_t powerState) {
    switch (static_cast<PowerState>(powerState)) {
        case POWER_STATE_BATTERY_NORMAL:   return 260UL;
        case POWER_STATE_BATTERY_ECONOMY:  return 420UL;
        case POWER_STATE_BATTERY_CRITICAL: return 650UL;
        case POWER_STATE_USB:
        default:                           return DISPLAY_MASCOT_INTERVAL_MS;
    }
}

uint8_t _displayBrightnessForPowerState(uint8_t powerState) {
    switch (static_cast<PowerState>(powerState)) {
        case POWER_STATE_BATTERY_NORMAL:
            return 72;
        case POWER_STATE_BATTERY_ECONOMY:
            return 38;
        case POWER_STATE_BATTERY_CRITICAL:
            return 22;
        case POWER_STATE_USB:
        default:
            return 100;
    }
}

ExecutionPolicy::UiRefreshSchedule _uiRefreshScheduleForPowerState(uint8_t powerState) {
    switch (static_cast<PowerState>(powerState)) {
        case POWER_STATE_BATTERY_NORMAL:
            return {2500UL, 1000UL, 1400UL};
        case POWER_STATE_BATTERY_ECONOMY:
            return {5000UL, 1800UL, 2500UL};
        case POWER_STATE_BATTERY_CRITICAL:
            return {8000UL, 2600UL, 4000UL};
        case POWER_STATE_USB:
        default:
            return {2000UL, PWNY_SCREEN_REFRESH_MS, 1000UL};
    }
}

void _initializeHardwareManagers(uint32_t& lastWifiTick) {
    MQTT_MGR.begin();
    BADUSB_MGR.begin();

    WIO_NRF.begin(2500);
    PHONE_XPORT.begin();
    LOG_STREAMER.begin();
    DASHBOARD_STREAMER.begin();
    NOTIF_CENTER.begin();

#if WIO_NRF_ACCESSORY_ENABLED
    // WIO SX1262 is the preferred sub-GHz radio: SubGhzManager::begin() keeps
    // the first backend whose begin() succeeds, and the SX1262 backend only
    // reports ready when CAPS confirms SX1262_PRESENT. Register it first so it
    // wins when the WIO accessory is attached; Reyax (RYLR998) stays as the
    // automatic fallback when the WIO/SX1262 is absent.
    SUBGHZ.attachBackend(&subghzWioSx1262);
#endif
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

#if MESHTASTIC_ENABLED && WIO_NRF_ACCESSORY_ENABLED
    // Meshtastic shares the single SX1262 with native SubGhz, so it starts
    // DISABLED (native SubGhz owns the radio at boot). Enable on demand via the
    // MESH command — that hands the radio to the mesh client and back.
    MESHTASTIC.attach(&WIO_NRF);
    MESHTASTIC.begin();
#endif

    DLOG_INFO("BOOTSTEP", "session begin");
    SESS.begin();
    DLOG_INFO("BOOTSTEP", "session ready");
    ENTITY_MGR.begin();
    DLOG_INFO("BOOTSTEP", "entity ready");
    WIFI_MGR.begin();
    DLOG_INFO("BOOTSTEP", "wifi manager ready allocated=%d",
              WIFI_MGR.isAllocated() ? 1 : 0);
    RADIO_ARB.begin();
    DLOG_INFO("BOOTSTEP", "radio arbiter ready");

    // A phone bulk transfer may have deliberately rebooted between its BLE
    // command phase and private Wi-Fi phase. Adopt and prepare that AP before
    // any scheduler path can allocate BLE or start capture again.
    if (PHONE_OFFLOAD.wifiBulkActive()) {
        PHONE_OFFLOAD.tickWifiBulk();
    }

    STATE_WRITE_BEGIN();
    SESS.getId().toCharArray(g_state.sessionId, sizeof(g_state.sessionId));
    g_state.runContext = RUN_CONTEXT_GENERAL;
    g_state.activeMissionProfile = MISSION_RECON;
    g_state.missionSelection = MISSION_RECON;
    g_state.generalScreen = static_cast<uint8_t>(g_state.currentScreen);
    STATE_WRITE_END();
    DLOG_INFO("BOOTSTEP", "presentation sync begin");
    syncRuntimePresentation();
    DLOG_INFO("BOOTSTEP", "presentation sync ready");

    // Write FieldVault records now that sessionId is populated. Heavy file
    // work intentionally happens here (not in setup() or in a crash/panic
    // context) so it cannot interfere with hardware bring-up timing.
    //
    // Order matters for human readability of field.jsonl: a crash from the
    // prior boot (if any) is written first, then this boot's summary, so
    // each boot's narrative reads "prior crash → new boot".
    //
    // Under boot heap pressure FieldVault::begin() is deferred into the main
    // loop, so the vault is NOT ready here and every boot/crash record was
    // being dropped on the floor — silently, and precisely when the device is
    // most likely to be crashing. Latch instead, and let the loop flush once
    // the vault comes up. Crossing BOOT_TRIAGE_PENDING_UPLOAD_THRESHOLD made
    // this fire on every boot and cost us both reboots on 2026-08-20.
    if (!FieldVault::isReady()) {
        g_fieldVaultBootRecordPending = true;
        DLOG_WARN("STOR",
                  "FieldVault not ready at hardware-ready; boot record deferred");
    } else {
        _writeFieldVaultBootRecords();
    }

    // Silent unless something is wrong: a task stack outside internal DRAM is
    // a latent DoubleException and must never ship unnoticed again.
    (void)_auditTaskStackPlacement(false);

    DLOG_INFO("WIFI", "Allocation: %s",
              WIFI_MGR.isAllocated() ? "OK" : "FAIL");
    lastWifiTick = millis();
    if (!PHONE_OFFLOAD.wifiBulkActive()) {
        RADIO_ARB.ensureDefaultCapture("boot");
    }
}

// Writes this boot's vault records. Safe to call once the vault is ready and
// g_state.sessionId is populated; both call sites satisfy that.
void _writeFieldVaultBootRecords() {
    if (!FieldVault::isReady()) return;
    {
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
    g_fieldVaultBootRecordPending = false;
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

    if (reason == ExecutionPolicy::UI_REFRESH_SYSTEM) {
        _refreshStorageCounterMirror();
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

        case SCREEN_MESHTASTIC:
            return previous.meshEnabled != next.meshEnabled ||
                   previous.meshNodeCount != next.meshNodeCount ||
                   previous.meshRxText != next.meshRxText ||
                   previous.meshTxText != next.meshTxText ||
                   previous.meshNodeNum != next.meshNodeNum ||
                   previous.meshLastFrom != next.meshLastFrom ||
                   _textChanged(previous.meshLastText, next.meshLastText);

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

        case SCREEN_BLE:
            // BLEManager mirrors state into g_state and raises dataRefresh only
            // when one of the visible BLE fields changes.
            return next.dataRefresh;

        case SCREEN_SYSTEM:
        case SCREEN_MISSION_SUMMARY:
            return next.dataRefresh ||
                   previous.battVoltage != next.battVoltage ||
                   previous.uptimeMs / 1000UL != next.uptimeMs / 1000UL ||
                   _textChanged(previous.storageStr, next.storageStr);

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
                       !STORAGE.isUploadBatchActive() ||
                           owner == RADIO_WIFI_UPLOAD ||
                           owner == RADIO_BLE_GPS,
                       "batch active with owner=%s mqtt_state=%d",
                       RadioArbiter::ownerName(owner),
                       static_cast<int>(mqttState));

    CONTRACT_WARN_ONCE(CONTRACT_MAINTENANCE_OWNER_FOR_REPAIR,
                       "CORE",
                       !STORAGE.hasSpoolRepairWork() ||
                           owner == RADIO_STORAGE_MAINTENANCE ||
                           owner == RADIO_NONE ||
                           owner == RADIO_WIFI_CAPTURE,
                       "repair pending owner=%s flags=%s",
                       RadioArbiter::ownerName(owner),
                       STORAGE.maintenanceFlagsText());
}

const char* _buttonEventName(ButtonEvent evt) {
    switch (evt) {
        case BTN_A_SHORT: return "A_SHORT";
        case BTN_A_LONG:  return "A_LONG";
        case BTN_B_SHORT: return "B_SHORT";
        case BTN_B_LONG:  return "B_LONG";
        case BTN_AB_SHORT:return "AB_SHORT";
        case BTN_AB_LONG: return "AB_LONG";
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
        case BUTTON_ACTION_MESH_TOGGLE:       return "MESH_TOGGLE";
        case BUTTON_ACTION_MESH_SEND:         return "MESH_SEND";
        case BUTTON_ACTION_BLE_LINK:          return "BLE_LINK";
        case BUTTON_ACTION_BLE_RELEASE:       return "BLE_RELEASE";
        default:                             return "UNKNOWN";
    }
}

static void _emitButtonLog(const char* message) {
    if (!message) {
        return;
    }

    DLOG_INFO("BTN", "%s", message);
    Serial.printf("[BTN] %s\r\n", message);
}

static MissionProfile _sanitizeMissionProfile(uint8_t rawProfile) {
    if (rawProfile >= static_cast<uint8_t>(MISSION_PROFILE_COUNT)) {
        return MISSION_RECON;
    }
    return static_cast<MissionProfile>(rawProfile);
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
    return screenLongName(screen);
}

// NimBLE holds ~62 KB of internal DRAM from the moment it initialises, and
// RADIO_BLE_GPS deliberately never tore it down: calling deinit(true) on the
// probe-timeout handoff panicked, because _releaseWorkerTask() bails out when
// the worker is still inside a blocking connect()/auth call and shutdown()
// used to deinit anyway. That left the 62 KB pinned for the rest of the boot,
// straight through every WiFi capture window - the single largest reclaimable
// block on this board.
//
// shutdown() now refuses to deinit under a busy worker, so the teardown is
// safe as long as we only attempt it once the stack is genuinely idle.
// readyForWifiHandoff() is exactly that predicate (radio off, worker not busy,
// disconnect settle elapsed). Re-init costs ~16 ms, so paying it per BLE
// session is a good trade for 62 KB of capture headroom.
// DISABLED pending a fix for the teardown panic below.
//
// Attempted 2026-08-18 and reverted. Gating the teardown on
// readyForWifiHandoff() (radio off, worker idle, settle elapsed) is NOT enough:
// NimBLEDevice::deinit(true) itself panics. The coredump is unambiguous —
//
//   assert failed: heap_caps_free heap_caps_base.c:80
//   (heap != NULL && "free() target pointer is outside heap areas")
//
// deinit(true) calls esp_bt_controller_mem_release(), which *unregisters* the
// BT memory region from the heap; a host structure allocated out of that region
// is then freed afterwards, against a heap that no longer exists. That is an
// ordering bug inside the NimBLE/controller teardown, not something the caller
// can gate around — which is why the original RADIO_BLE_GPS comment simply
// refused to deinit.
//
// The prize is real and measured: a boot that has not yet initialised NimBLE
// shows heapFree=103K during capture; after NimBLE init the same phase shows
// 36K. ~67 KB of capture headroom is pinned for the rest of the boot.
//
// Next experiment: NimBLEDevice::deinit(false), which skips the object
// destruction (and possibly the mem_release) path — but it must be checked for
// leaking the server/client/scan objects across a begin()/shutdown() cycle
// before it can be trusted.
static constexpr bool     BLE_IDLE_TEARDOWN_ENABLED = true;
static constexpr uint32_t BLE_IDLE_TEARDOWN_MS = 5000UL;

void _releaseIdleBleStack(uint32_t nowMs) {
    static uint32_t idleSinceMs = 0;

    if (!BLE_IDLE_TEARDOWN_ENABLED) return;

    if (!BLE_MGR.isBegun()) {
        idleSinceMs = 0;
        return;
    }

    // Never tear down while BLE owns the radio, or while a BLE lease is queued
    // - that would just pay the re-init cost immediately.
    const bool bleWanted =
        RADIO_ARB.isOwner(RADIO_BLE_GPS)  || RADIO_ARB.isOwner(RADIO_BLE_TEXT) ||
        RADIO_ARB.hasPendingOwner(RADIO_BLE_GPS) ||
        RADIO_ARB.hasPendingOwner(RADIO_BLE_TEXT);

    if (bleWanted || !BLE_MGR.readyForWifiHandoff()) {
        idleSinceMs = 0;
        return;
    }

    if (idleSinceMs == 0) {
        idleSinceMs = nowMs ? nowMs : 1;
        return;
    }
    if ((nowMs - idleSinceMs) < BLE_IDLE_TEARDOWN_MS) {
        return;
    }
    idleSinceMs = 0;

    const uint32_t before =
        heap_caps_get_free_size(SPECTRE_CAP_DRAM);

    BLE_MGR.shutdown();

    const uint32_t after =
        heap_caps_get_free_size(SPECTRE_CAP_DRAM);

    if (BLE_MGR.isBegun()) {
        // shutdown() declined (worker went busy in the meantime); retry later.
        DLOG_INFO("CORE", "idle BLE teardown deferred");
        return;
    }

    DLOG_WARN("CORE", "idle BLE teardown reclaimed %luB internal (%luB -> %luB)",
              static_cast<unsigned long>(after > before ? after - before : 0),
              static_cast<unsigned long>(before),
              static_cast<unsigned long>(after));
}

// Internal-DRAM floor for an active promiscuous capture.
//
// Steady-state capture sits at ~20 KB free / ~12 KB largest on this board, so
// these floors sit below normal operation and only trip when the WiFi driver's
// dynamic buffers have grown well past their usual footprint - which is what a
// dense RF environment does, and what panicked the device five times during the
// 2026-08-18 city run (crash ring recorded min=2K internal free).
static constexpr uint32_t CAPTURE_HEAP_FLOOR_BYTES         = 12U * 1024U;
static constexpr uint32_t CAPTURE_HEAP_LARGEST_FLOOR_BYTES =  6U * 1024U;
static constexpr uint32_t CAPTURE_HEAP_GUARD_COOLDOWN_MS   = 30000UL;

static uint32_t g_captureHeapGuardTrips = 0;

// Recycle the WiFi driver when capture is about to run the internal heap dry.
// pauseRadio() only clears promiscuous mode and leaves the driver initialised,
// so the ~30 KB of driver buffers is only reclaimed by a full suspendRadio()
// (WIFI_OFF) followed by a fresh startPromiscuous(). Costs a sub-second capture
// gap instead of a panic.
void _enforceCaptureHeapGuard(uint32_t nowMs) {
    static uint32_t lastGuardMs = 0;

    if (!RADIO_ARB.isOwner(RADIO_WIFI_CAPTURE)) return;

    const uint32_t freeInternal = _sampleDramFree();
    const uint32_t largestInternal =
        heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM);

    if (freeInternal >= CAPTURE_HEAP_FLOOR_BYTES &&
        largestInternal >= CAPTURE_HEAP_LARGEST_FLOOR_BYTES) {
        return;
    }

    // Don't thrash the radio if the recycle didn't buy much headroom.
    if (lastGuardMs != 0 &&
        (nowMs - lastGuardMs) < CAPTURE_HEAP_GUARD_COOLDOWN_MS) {
        return;
    }
    lastGuardMs = nowMs;
    g_captureHeapGuardTrips++;

    const uint32_t pending =
        (STORAGE.isReady() && STORAGE.isPendingEventCountAuthoritative())
            ? STORAGE.getPendingEventCount() : 0U;

    DLOG_WARN("CORE",
              "capture heap guard: free=%luB largest=%luB floor=%luB/%luB trips=%lu",
              static_cast<unsigned long>(freeInternal),
              static_cast<unsigned long>(largestInternal),
              static_cast<unsigned long>(CAPTURE_HEAP_FLOOR_BYTES),
              static_cast<unsigned long>(CAPTURE_HEAP_LARGEST_FLOOR_BYTES),
              static_cast<unsigned long>(g_captureHeapGuardTrips));

    // Breadcrumb so a panic *during* the recycle is still attributable.
    crashCheckpoint(CrashPhase::RADIO_RESUME,
                    static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                    pending);

    WIFI_MGR.suspendRadio();
    const uint32_t freedInternal =
        heap_caps_get_free_size(SPECTRE_CAP_DRAM);

    if (!WIFI_MGR.startPromiscuous()) {
        DLOG_ERROR("CORE", "capture heap guard: promiscuous restart failed");
    }

    crashBreadcrumbClear(CrashPhase::RADIO_RESUME);

    DLOG_WARN("CORE",
              "capture heap guard recycled driver: free %luB -> %luB",
              static_cast<unsigned long>(freeInternal),
              static_cast<unsigned long>(freedInternal));
}

// Split the NimBLE init cost into controller vs host. Runs before BLE has
// initialised - at boot when SPECTRE_BOOT_MEMPROBE is on, since the phone
// probe brings NimBLE up ~11 s in, well before the console takes commands.
void _runBleMemProbe(bool manageRadio) {
        if (BLE_MGR.isBegun()) {
            Serial.println("[MEMPROBE] BLE already initialised - measurement invalid");
            return;
        }

        if (!manageRadio) {
            // Boot path: USB CDC has not enumerated yet, so wait for the host
            // to attach or the whole measurement is written into the void.
            const uint32_t waitUntil = millis() + 8000UL;
            while (!Serial && static_cast<int32_t>(millis() - waitUntil) < 0) {
                delay(50);
            }
            delay(600);
        }

        if (manageRadio) {
            WIFI_MGR.suspendRadio();
            delay(300);
        }

        auto freeInt = []() {
            return heap_caps_get_free_size(SPECTRE_CAP_DRAM);
        };
        auto largestInt = []() {
            return heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM);
        };

        const uint32_t base = freeInt();
        Serial.printf("[MEMPROBE] baseline free=%lu largest=%lu status=%d\r\n",
                      (unsigned long)base, (unsigned long)largestInt(),
                      (int)esp_bt_controller_get_status());

        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        cfg.ble_max_act = 3;  // what NimBLE itself passes (1 conn + bcast + obs)
        Serial.printf("[MEMPROBE] cfg ble_max_act=%d task_stack=%d normal_adv=%d mesh_adv=%d\r\n",
                      (int)cfg.ble_max_act, (int)cfg.controller_task_stack_size,
                      (int)cfg.normal_adv_size, (int)cfg.mesh_adv_size);

        esp_err_t err = esp_bt_controller_init(&cfg);
        const uint32_t afterInit = freeInt();
        Serial.printf("[MEMPROBE] controller_init err=%s free=%lu largest=%lu (cost=%ld)\r\n",
                      esp_err_to_name(err), (unsigned long)afterInit,
                      (unsigned long)largestInt(), (long)base - (long)afterInit);

        if (err == ESP_OK) {
            err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
            const uint32_t afterEnable = freeInt();
            Serial.printf("[MEMPROBE] controller_enable err=%s free=%lu largest=%lu (cost=%ld)\r\n",
                          esp_err_to_name(err), (unsigned long)afterEnable,
                          (unsigned long)largestInt(), (long)afterInit - (long)afterEnable);

            if (err == ESP_OK) {
                err = esp_bt_controller_disable();
                Serial.printf("[MEMPROBE] controller_disable err=%s free=%lu\r\n",
                              esp_err_to_name(err), (unsigned long)freeInt());
            }
            err = esp_bt_controller_deinit();
            Serial.printf("[MEMPROBE] controller_deinit err=%s free=%lu largest=%lu\r\n",
                          esp_err_to_name(err), (unsigned long)freeInt(),
                          (unsigned long)largestInt());
        }

        Serial.printf("[MEMPROBE] recovered=%ld of %lu (status=%d)\r\n",
                      (long)freeInt() - (long)base + 0L, (unsigned long)base,
                      (int)esp_bt_controller_get_status());
        Serial.flush();
        if (manageRadio) {
            WIFI_MGR.startPromiscuous();
        }
    }


void _logRuntimeHealth(uint32_t nowMs) {
    // Keep the global low-water honest across every radio state, not just
    // the capture windows the heap guard runs in.
    (void)_sampleDramFree();
    static uint32_t lastHealthLogMs = 0;
    static uint32_t lastHeapCheckMs = 0;

    if (nowMs - lastHealthLogMs >= HEALTH_LOG_INTERVAL_MS) {
        uint8_t healthOwner = static_cast<uint8_t>(RADIO_ARB.currentOwner());
        crashCheckpointVolatile(CrashPhase::RUNTIME_HEALTH,
                                healthOwner,
                                (STORAGE.isReady() &&
                                 STORAGE.isPendingEventCountAuthoritative())
                                    ? STORAGE.getPendingEventCount()
                                    : 0U);
        struct HealthSnapshot {
            bool wifiConnected;
            bool bleConnected;
            bool gpsValid;
            int wifiCount;
            int pendingFiles;
            bool backlogTrusted;
            uint32_t pendingEnrich;
            uint8_t radioOwner;
            bool timeValid;
            unsigned long uptimeMs;
            int battPercent;
            uint16_t battVoltageMv;
            int16_t battTrendMvPerMin;
            uint16_t battCapacityMah;
            uint16_t battRuntimeMin;
            uint8_t powerSource;
            uint8_t powerState;
            bool charging;
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
        health.pendingFiles =
            (STORAGE.isReady() && STORAGE.isPendingEventCountAuthoritative())
                ? static_cast<int>(STORAGE.getPendingEventCount())
                : -1;
        health.backlogTrusted = STORAGE.isPendingEventCountAuthoritative();
        health.pendingEnrich =
            g_state.storagePendingEnrichMission + g_state.storagePendingEnrichNoise;
        health.radioOwner = g_state.radioOwner;
        health.timeValid = g_state.timeValid;
        health.uptimeMs = g_state.uptimeMs;
        health.battPercent = g_state.battPercent;
        health.battVoltageMv = g_state.battVoltageMv;
        health.battTrendMvPerMin = g_state.battTrendMvPerMin;
        health.battCapacityMah = g_state.battCapacityMah;
        health.battRuntimeMin = g_state.battRuntimeMin;
        health.powerSource = g_state.powerSource;
        health.powerState = g_state.powerState;
        health.charging = g_state.charging;
        health.loraPacketCount = g_state.loraPacketCount;
        health.subGhzMode = g_state.subGhzMode;
        health.subGhzNodeCount = g_state.subGhzNodeCount;
        strlcpy(health.timeLocal, g_state.timeLocal, sizeof(health.timeLocal));
        strlcpy(health.timeSource, g_state.timeSource, sizeof(health.timeSource));
        strlcpy(health.subGhzBackend, g_state.subGhzBackend, sizeof(health.subGhzBackend));
        STATE_READ_END();

        char pendingUploadText[16] = "?";
        if (health.pendingFiles >= 0) {
            snprintf(pendingUploadText, sizeof(pendingUploadText), "%d", health.pendingFiles);
        }

        const uint32_t totalHeap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        const uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        const uint32_t minHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
        const uint32_t largestHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        const uint32_t totalInternal =
            heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const uint32_t freeInternal =
            heap_caps_get_free_size(SPECTRE_CAP_DRAM);
        const uint32_t minInternal =
            heap_caps_get_minimum_free_size(SPECTRE_CAP_DRAM);
        const uint32_t largestInternal =
            heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM);
        const uint32_t totalPsram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        const uint32_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const uint32_t largestPsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        const uint32_t usedHeap = (totalHeap >= freeHeap) ? (totalHeap - freeHeap) : 0;
        const uint32_t usedInternal =
            (totalInternal >= freeInternal) ? (totalInternal - freeInternal) : 0;
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
        const UBaseType_t loopStackWords =
            loopTaskHandle ? uxTaskGetStackHighWaterMark(loopTaskHandle) : 0;
        
        _updateCoreLoad(nowMs);

        DLOG_INFO("HEAP",
          "heap free=%luKB min=%luKB largest=%luKB frag=%lu%% internalFree=%luKB internalLargest=%luKB psramFree=%luKB core=%u/%u%% owner=%s nets=%d pendingUpload=%s pendingEnrich=%lu",
          static_cast<unsigned long>(kb(freeHeap)),
          static_cast<unsigned long>(kb(minHeap)),
          static_cast<unsigned long>(kb(largestHeap)),
          static_cast<unsigned long>(heapFragPct),
          static_cast<unsigned long>(kb(freeInternal)),
          static_cast<unsigned long>(kb(largestInternal)),
          static_cast<unsigned long>(kb(freePsram)),
          static_cast<unsigned>(g_coreLoad.busyPct[0]),
          static_cast<unsigned>(g_coreLoad.busyPct[1]),
          RadioArbiter::ownerName(static_cast<RadioOwner>(health.radioOwner)),
          health.wifiCount,
          pendingUploadText,
          static_cast<unsigned long>(health.pendingEnrich));
        if (health.timeValid && health.timeLocal[0]) {
            Serial.printf("[HEALTH] time=%s src=%s\r\n", health.timeLocal, health.timeSource);
        } else {
            const unsigned long s = health.uptimeMs / 1000UL;
            const unsigned long h = s / 3600UL;
            const unsigned long m = (s % 3600UL) / 60UL;
            const unsigned long sec = s % 60UL;
            Serial.printf("[HEALTH] uptime=%luh%02lum%02lus\r\n", h, m, sec);
            // No trusted clock this boot => every captured record has epochUtc==0
            // and is permanently unenrichable. Surface it in the always-on
            // health block (TIME logs are debug-area gated and were invisible).
            Serial.printf("[HEALTH] CLOCK=UNSYNCED records-this-boot-unenrichable lastNtp=%s age=%lus\r\n",
                          TIME_SVC.lastAttemptReason(),
                          TIME_SVC.everAttempted()
                              ? TIME_SVC.lastAttemptAgeMs() / 1000UL
                              : 0UL);
        }
        Serial.printf("[HEALTH] heap used=%lu/%luKB free=%luKB min=%luKB largest=%luKB frag=%lu%%\r\n",
                      static_cast<unsigned long>(kb(usedHeap)),
                      static_cast<unsigned long>(kb(totalHeap)),
                      static_cast<unsigned long>(kb(freeHeap)),
                      static_cast<unsigned long>(kb(minHeap)),
                      static_cast<unsigned long>(kb(largestHeap)),
                      static_cast<unsigned long>(heapFragPct));
        Serial.printf("[HEALTH] internal used=%lu/%luKB free=%luKB min=%luKB largest=%luKB\r\n",
                      static_cast<unsigned long>(kb(usedInternal)),
                      static_cast<unsigned long>(kb(totalInternal)),
                      static_cast<unsigned long>(kb(freeInternal)),
                      static_cast<unsigned long>(kb(minInternal)),
                      static_cast<unsigned long>(kb(largestInternal)));
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
        Serial.printf("[HEALTH] loopTask free_min=%luB/%luB\r\n",
                      static_cast<unsigned long>(hb(loopStackWords)),
                      static_cast<unsigned long>(SPECTRE_LOOP_TASK_STACK_BYTES));
        Serial.printf("[HEALTH] core usage: core0=%u%% core1=%u%%\r\n",
                      static_cast<unsigned>(g_coreLoad.busyPct[0]),
                      static_cast<unsigned>(g_coreLoad.busyPct[1]));
        Serial.printf("[HEALTH] power=%s state=%s batt=%d%% %umV trend=%dmV/min runtime=%umin cap=%umAh charging=%d\r\n",
                      _fieldVaultPowerSourceName(static_cast<PowerSource>(health.powerSource)),
                      _fieldVaultPowerStateName(static_cast<PowerState>(health.powerState)),
                      health.battPercent,
                      static_cast<unsigned>(health.battVoltageMv),
                      static_cast<int>(health.battTrendMvPerMin),
                      static_cast<unsigned>(health.battRuntimeMin),
                      static_cast<unsigned>(health.battCapacityMah),
                      health.charging ? 1 : 0);
        Serial.printf("[HEALTH] radio=%s wifi=%d ble=%d gps=%d nets=%d pendingUpload=%s pendingEnrich=%lu\r\n",
                      RadioArbiter::ownerName(static_cast<RadioOwner>(health.radioOwner)),
                      health.wifiConnected ? 1 : 0,
                      health.bleConnected ? 1 : 0,
                      health.gpsValid ? 1 : 0,
                      health.wifiCount,
                      pendingUploadText,
                      static_cast<unsigned long>(health.pendingEnrich));
        DLOG_DEBUG("SUBGHZ", "heartbeat");

        const char* subGhzBackend = health.subGhzBackend[0] ? health.subGhzBackend : "NONE";

        Serial.printf("[HEALTH] subghz=%s mode=%s pkts=%d nodes=%d\r\n",
                      subGhzBackend,
                      _subGhzModeShort(health.subGhzMode),
                      health.loraPacketCount,
                      health.subGhzNodeCount);

        if (RAMSpool::isReady()) {
            // RAMSpool::logStats() prints a superset of what the old two
            // [HEALTH] ramspool lines carried (it adds pressureState,
            // p3/p2/p1 pressure-drop counters, prioEnq, and lane depths).
            // Keep only logStats() to avoid the side-by-side duplicate.
            RAMSpool::logStats();
        }

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
            g_state.currentScreen = nextGeneralScreen(g_state.currentScreen);
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
        case BUTTON_ACTION_MESH_TOGGLE:
#if MESHTASTIC_ENABLED && WIO_NRF_ACCESSORY_ENABLED
            if (!MESHTASTIC.isAvailable()) {
                _queueNotification(NOTIF_DEVICE_NEW, "MESH: no SX1262");
            } else if (MESHTASTIC.isEnabled()) {
                MESHTASTIC.disable();
                _queueNotification(NOTIF_DEVICE_NEW, "MESH OFF");
            } else {
                MESHTASTIC.enable();
                _queueNotification(NOTIF_DEVICE_NEW, "MESH ON");
            }
            STATE_WRITE_BEGIN();
            g_state.dataRefresh = true;
            STATE_WRITE_END();
#endif
            return true;
        case BUTTON_ACTION_MESH_SEND:
#if MESHTASTIC_ENABLED && WIO_NRF_ACCESSORY_ENABLED
            if (!MESHTASTIC.isEnabled()) {
                _queueNotification(NOTIF_DEVICE_NEW, "MESH: enable first");
                return true;
            }
            // Compose over the phone-BLE keyboard; the typed text routes to
            // MESHTASTIC.sendText() in the text-input result dispatch.
            _requestBleTextEntry("mesh_send", "Mesh message:");
#endif
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
            ENTITY_MGR.reset();
            STATE_WRITE_BEGIN();
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
        case BUTTON_ACTION_BLE_LINK:
            if (!PHONE_COMPANION_ENABLED) {
                _queueNotification(NOTIF_DEVICE_NEW, "BLE COMPANION OFF");
                return false;
            }
            g_companionCmd.link = true;
            _queueNotification(NOTIF_DEVICE_NEW, "BLE LINK QUEUED");
            DLOG_INFO("BLE", "Device BLE page requested bidirectional link window");
            return true;
        case BUTTON_ACTION_BLE_RELEASE: {
            g_companionCmd.cancel = true;
            const RadioOwner owner = RADIO_ARB.currentOwner();
            if (owner == RADIO_BLE_TEXT) {
                PHONE_XPORT.cancelTextInput("device_ble_release");
                RADIO_ARB.release(RADIO_BLE_TEXT, "device_ble_release");
            } else if (owner == RADIO_BLE_GPS) {
                RADIO_ARB.release(RADIO_BLE_GPS, "device_ble_release");
            }
            _queueNotification(NOTIF_DEVICE_NEW, "BLE RELEASED");
            DLOG_INFO("BLE", "Device BLE page released owner=%s",
                      RadioArbiter::ownerName(owner));
            return true;
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

// ── Core 1: TaskButtons ──

void TaskButtons(void* pvParameters) {
    (void)pvParameters;
    DLOG_INFO("CORE", "Button input task started");

    TickType_t lastWake = xTaskGetTickCount();
    uint32_t lastStackLogMs = millis();
    UBaseType_t minStackWords = uxTaskGetStackHighWaterMark(nullptr);
    DLOG_INFO("STACK", "TaskButtons watermark=%luB",
              static_cast<unsigned long>(
                  uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    for (;;) {
        const ButtonEvent evt = buttons.getEvent();
        if (evt != BTN_NONE && s_buttonEventQueue) {
            if (xQueueSend(s_buttonEventQueue, &evt, 0) != pdPASS) {
                DLOG_WARN("BTN", "input queue full; dropping evt=%s",
                          _buttonEventName(evt));
            }
        }

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(BUTTON_POLL_INTERVAL_MS));

        const uint32_t now = millis();
        if (now - lastStackLogMs >= STACK_LOG_INTERVAL_MS) {
            lastStackLogMs = now;
            const UBaseType_t freeWords = uxTaskGetStackHighWaterMark(nullptr);
            if (freeWords < minStackWords) minStackWords = freeWords;
            const uint32_t freeBytes = freeWords * sizeof(StackType_t);
            if (freeBytes < 1536U) {
                DLOG_WARN("STACK", "TaskButtons low watermark=%luB min=%luB",
                          static_cast<unsigned long>(freeBytes),
                          static_cast<unsigned long>(minStackWords * sizeof(StackType_t)));
            } else {
                DLOG_INFO("STACK", "TaskButtons watermark=%luB min=%luB",
                          static_cast<unsigned long>(freeBytes),
                          static_cast<unsigned long>(minStackWords * sizeof(StackType_t)));
            }
        }
    }
}

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
        uint32_t displayTimeoutMs = BACKLIGHT_TIMEOUT_MS;
        if (SETTINGS.isReady()) {
            displayTimeoutMs = SETTINGS.get().displayTimeoutMs;
        }

        if (!_isDisplayAwake()) {
            DisplayPowerState powerSnapshot;
            _peekDisplayPowerState(powerSnapshot);
            _setDisplayBrightnessPct(_displayBrightnessForPowerState(powerSnapshot.powerState));
            if (_shouldKeepDisplayAwake(powerSnapshot, now, displayTimeoutMs)) {
                if (_setDisplayAwake(true)) {
                    STATE_WRITE_BEGIN();
                    g_state.screenChanged = true;
                    STATE_WRITE_END();
                }
            }
            lastLvTickMs = millis();
            vTaskDelay(pdMS_TO_TICKS(DISPLAY_ASLEEP_POLL_MS));
            continue;
        }

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
                if (_displayResumePending()) {
                    _completeDisplayResume();
                    DLOG_INFO("CORE", "Display resumed for sleep presentation");
                }
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

        _setDisplayBrightnessPct(_displayBrightnessForPowerState(snapshot.powerState));
        const bool keepDisplayAwake =
            _shouldKeepDisplayAwake(snapshot, now, displayTimeoutMs);
        const bool displayAwakeChanged = _setDisplayAwake(keepDisplayAwake);
        if (displayAwakeChanged && keepDisplayAwake) {
            STATE_WRITE_BEGIN();
            g_state.screenChanged = true;
            STATE_WRITE_END();
            snapshot.screenChanged = true;
        } else if (!keepDisplayAwake) {
            lastLvTickMs = millis();
            vTaskDelay(pdMS_TO_TICKS(DISPLAY_ASLEEP_POLL_MS));
            continue;
        }
        const bool resumePending = _displayResumePending();
        if (resumePending) {
            snapshot.screenChanged = true;
            statusValid = false;
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
        if (resumePending) {
            lv_obj_invalidate(lv_screen_active());
            lv_refr_now(NULL);
            _completeDisplayResume();
            DLOG_INFO("CORE", "Display resumed and refreshed");
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
                display.drawMeshtastic(s.meshEnabled, s.meshNodeNum,
                                       s.meshNodeCount, s.meshRxText,
                                       s.meshTxText, s.meshLastFrom,
                                       s.meshLastText);
                break;

            case SCREEN_WIFI:
                display.drawWifi(
                    s.wifiSSID,
                    s.wifiNetworkCount,
                    s.probePacketCount > 0 ? s.lastProbedMAC : "--");
                break;

            case SCREEN_BLE:
                display.drawBle();
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

            case SCREEN_MISSION_SUMMARY:
                display.drawMissionSummary(s.uptimeMs);
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

    STATE_WRITE_BEGIN();
    g_state.exportLastOk = ok;
    g_state.exportLastEvents = ok ? summary.totalEvents : 0;
    g_state.exportLastFiles = ok ? summary.exportedFiles : 0;
    g_state.exportLastBytes = ok ? summary.exportedBytes : 0;
    g_state.exportLastPending = totalPendingUploads;
    strlcpy(g_state.exportLastISO, summary.generatedIso, sizeof(g_state.exportLastISO));
    strlcpy(g_state.exportLastSessionId, summary.sessionId, sizeof(g_state.exportLastSessionId));
    g_state.sessionFilesPending = static_cast<int>(totalPendingUploads);
    g_state.kaliSyncAvailable = (totalPendingUploads > 0);
    STATE_WRITE_END();
}

static void _clearStorageSummaryMirror() {
    const bool storageReady = STORAGE.isReady();
    const bool storageMaintPending = STORAGE.hasStorageMaintenanceWork();
    const uint32_t pending =
        storageReady ? STORAGE.getPendingEventCount() : 0U;
    const uint32_t storedRecords =
        storageReady ? STORAGE.getDisplayRecordCount() : 0U;
    const uint32_t storedEvents =
        storageReady ? STORAGE.getDisplayEventCount() : 0U;
    const StorageLaneCounts pendingUpload =
        storageReady ? STORAGE.getPendingUploadCounts() : StorageLaneCounts{};

    // Preserve summary classifications while maintenance is owed, but keep
    // total records aligned with the spool index. No filesystem scan here.
    StorageUiMirror::publishClearedSummary(storageReady,
                                           storageMaintPending,
                                           millis(),
                                           pending,
                                           pendingUpload.mission,
                                           pendingUpload.noise,
                                           storedEvents,
                                           storedRecords);
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

    // UI/debrief summary refresh must never surprise the radio path with a
    // spool scan. If the mirror is stale, maintenance owns refreshing it.
    const RadioOwner owner = RADIO_ARB.currentOwner();
    const bool radioActive =
        owner == RADIO_WIFI_CAPTURE ||
        owner == RADIO_WIFI_SCAN ||
        owner == RADIO_WIFI_PMKID ||
        owner == RADIO_BLE_GPS ||
        owner == RADIO_BLE_TEXT ||
        owner == RADIO_WIFI_UPLOAD ||
        owner == RADIO_STORAGE_MAINTENANCE;
    CONTRACT_WARN_ONCE(CONTRACT_SUMMARY_REFRESH_NOT_RADIO_ACTIVE,
                       "CORE",
                       !radioActive,
                       "force=%u backlog=%lu owner=%s",
                       force ? 1U : 0U,
                       static_cast<unsigned long>(pendingBacklog),
                       RadioArbiter::ownerName(owner));
    if (radioActive) {
        STORAGE.requestMaintenance(STORAGE_MAINT_DIRTY_SUMMARY,
                                   force ? "summary_force_radio_active"
                                         : "summary_radio_active");
        StorageUiMirror::publishSummaryRadioActive();
        _publishStorageMaintenanceMirror(true,
                                         owner == RADIO_STORAGE_MAINTENANCE);
        return false;
    }

    STORAGE.refreshStorageUiState(false, false);
    const bool ok = StorageUiMirror::publishSummaryStamp(now);
    _publishStorageMaintenanceMirror(true,
                                     owner == RADIO_STORAGE_MAINTENANCE);

    lastRefreshMs = now;
    return ok;
}

void _runSessionExport(bool notifyUser) {
    SessionExportSummary summary;
    StorageExclusiveWindow exportWindow;
    bool windowActive = false;
    bool ok = false;

    if (RADIO_ARB.currentOwner() == RADIO_NONE ||
        RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE)) {
        windowActive = exportWindow.begin(STORAGE_WINDOW_EXPORT,
                                          "session_export");
        if (windowActive) {
            ok = EXPORT_MGR.exportCurrentSession(&summary);
            exportWindow.end(ok ? "done" : "failed");
        }
    } else {
        DLOG_WARN("EXPORT", "Session export deferred owner=%s",
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
        STORAGE.requestMaintenance(STORAGE_MAINT_DIRTY_SUMMARY,
                                   "export_deferred_radio_active");
    }

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

static void _logHardwareSectionIfSlow(const char* section,
                                      uint32_t startMs,
                                      bool storageOk) {
    const uint32_t elapsedMs = millis() - startMs;
    if (elapsedMs < HARDWARE_SECTION_WARN_MS) {
        return;
    }

    DLOG_WARN("CORE",
              "TaskHardware slow section=%s ms=%lu owner=%s pendingUpload=%lu maintPending=%d",
              section ? section : "?",
              static_cast<unsigned long>(elapsedMs),
              RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
              static_cast<unsigned long>(
                  STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U),
              (storageOk && STORAGE.hasStorageMaintenanceWork()) ? 1 : 0);
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
        StorageUiMirror::writeReady_locked(false);
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

    DLOG_INFO("CORE", "Display ready=%d before storage init",
              s_displayLayerReady ? 1 : 0);
    if (!s_displayLayerReady) {
        crashCheckpoint(CrashPhase::DISPLAY_WAIT,
                        static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                        0);
        crashBreadcrumbClear(CrashPhase::DISPLAY_WAIT);
        DLOG_WARN("CORE", "Display layer not ready; continuing hardware init");
    }

    DLOG_INFO("CORE", "Storage begin start heapFree=%lu largest=%lu",
              static_cast<unsigned long>(
                  heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
              static_cast<unsigned long>(
                  heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM)));
    crashCheckpoint(CrashPhase::STORAGE_BOOT,
                    static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                    0);
    bool storageOk = STORAGE.begin();
    crashBreadcrumbClear(CrashPhase::STORAGE_BOOT);
    DLOG_INFO("CORE", "Storage begin done ok=%d ready=%d pending=%lu heapFree=%lu largest=%lu",
              storageOk ? 1 : 0,
              STORAGE.isReady() ? 1 : 0,
              STORAGE.isReady() ? static_cast<unsigned long>(STORAGE.getPendingEventCount()) : 0UL,
              static_cast<unsigned long>(
                  heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
              static_cast<unsigned long>(
                  heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM)));
    if (storageOk && STORAGE.hasMaintenanceWork() &&
        STORAGE.isCaptureSafeToResume()) {
        crashCheckpoint(CrashPhase::STORAGE_BOOT,
                        static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                        STORAGE.getPendingEventCount());
        _publishStorageMaintenanceMirror(true, true);
        const uint32_t maintStartMs = millis();
        STORAGE.runMaintenanceWindow(5000UL, "pre_capture_boot");
        _publishStorageMaintenanceMirror(true, false, true,
                                         millis() - maintStartMs);
        crashBreadcrumbClear(CrashPhase::STORAGE_BOOT);
    } else if (storageOk && STORAGE.hasMaintenanceWork()) {
        DLOG_WARN("CORE",
                  "Boot maintenance deferred until runtime pending=%lu flags=%s",
                  static_cast<unsigned long>(STORAGE.getPendingEventCount()),
                  STORAGE.maintenanceFlagsText());
    }
    String storageUsed = storageOk ? STORAGE.getCachedUsedString() : String();
    _publishStorageState(storageOk, storageUsed);

    const bool bootHeapPressure =
        STORAGE.isReady() &&
        (STORAGE.getPendingEventCount() >= BOOT_TRIAGE_PENDING_UPLOAD_THRESHOLD ||
         heap_caps_get_free_size(SPECTRE_CAP_DRAM) <
             BOOT_TRIAGE_FREE_INTERNAL_HEAP_BYTES ||
         heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM) <
             BOOT_TRIAGE_LARGEST_INTERNAL_HEAP_BYTES);

    // Phase 1 shadow plumbing: PSRAM slot pool + pinned worker on Core 1.
    // Failure is non-fatal — producers fall back to no-op enqueue.
    RAMSpool::begin();

    if (storageOk) {
        if (bootHeapPressure) {
            g_deferredBootOptionalInitPending = true;
            g_deferredBootOptionalInitAtMs =
                millis() + DEFERRED_BOOT_OPTIONAL_INIT_DELAY_MS;
            DLOG_WARN("CORE",
                      "Deferring optional boot init pending=%lu freeInternal=%lu largestInternal=%lu",
                      static_cast<unsigned long>(STORAGE.getPendingEventCount()),
                      static_cast<unsigned long>(
                          heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
                      static_cast<unsigned long>(
                          heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM)));
        } else {
            _runOptionalBootInitialization();
        }
    }


    _initializeHardwareManagers(lastWifiTick);
    _publishHardwareReadyState();
    _markUiActivity();

    DLOG_INFO("CORE", "Hardware ready");

    // Do not stop field capture for a blocking saved-WiFi/NTP search. In the
    // field the phone supplies trusted GPS time over BLE; when WiFi is already
    // connected TimeService::tick() requests NTP without a connect loop. The
    // serial `time sync` command remains available for an explicit 12 s attempt.
    if (storageOk && !TIME_SVC.hasAccurateUtc()) {
        TIME_SVC.recordAttempt("passive_wait");
        DLOG_INFO("TIME", "Trusted clock deferred; waiting for phone GPS or connected WiFi");
    }

    for (;;) {
        static uint32_t lastWifiRefresh = 0;
        static uint32_t lastPwnyRefresh = 0;
        static uint32_t lastSystemRefresh = 0;
        static uint32_t lastGpsFixSeen = 0;
        static uint32_t lastLoopStartMs = 0;
        static uint32_t lastLoopGapWarnMs = 0;
        static uint32_t suppressedLoopGapWarns = 0;
        static uint32_t lastFieldVaultPowerSampleMs = 0;
        static uint32_t lastFieldVaultRunSampleMs = 0;
        static uint32_t lastFieldVaultRunTransitionMs = 0;
        static uint8_t lastFieldVaultRunOwner = 0xff;
        static uint32_t lastFieldVaultRunPendingUpload = UINT32_MAX;
        static uint32_t lastFieldVaultRunPendingEnrich = UINT32_MAX;
        static bool lastFieldVaultRunUploadActive = false;
        static bool lastFieldVaultRunWioAvailable = false;
        static bool lastFieldVaultRunWioBleProxy = false;
        static bool lastFieldVaultRunWioPhoneConnected = false;
        const uint32_t loopNow = millis();
        if (lastLoopStartMs != 0) {
            const uint32_t loopGapMs = loopNow - lastLoopStartMs;
            if (loopGapMs >= HARDWARE_LOOP_GAP_WARN_MS) {
                if (loopNow - lastLoopGapWarnMs >= 10000UL) {
                    DLOG_WARN("CORE",
                              "TaskHardware loop gap=%lums owner=%s pendingUpload=%lu maintPending=%d suppressed=%lu",
                              static_cast<unsigned long>(loopGapMs),
                              RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                              static_cast<unsigned long>(
                                  STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U),
                              (storageOk && STORAGE.hasStorageMaintenanceWork()) ? 1 : 0,
                              static_cast<unsigned long>(suppressedLoopGapWarns));
                    lastLoopGapWarnMs = loopNow;
                    suppressedLoopGapWarns = 0;
                } else {
                    suppressedLoopGapWarns++;
                }
            }
        }
        lastLoopStartMs = loopNow;

        // Signed-delta compare for wraparound safety (matches the
        // critical-sleep guard below).
        if (g_deferredBootOptionalInitPending &&
            static_cast<int32_t>(loopNow - g_deferredBootOptionalInitAtMs) >= 0) {
            g_deferredBootOptionalInitPending = false;
            if (STORAGE.isReady()) {
                _runOptionalBootInitialization();
            }
        }

        POWER_MGR.tick(loopNow);
        const PowerSnapshot power = POWER_MGR.consumeSnapshot();
        _applyPowerSnapshotToState(power);
        const uint8_t currentRadioOwner =
            static_cast<uint8_t>(RADIO_ARB.currentOwner());

        if (FieldVault::isReady()) {
            // Deferred boot init means this boot's record could not be written
            // at hardware-ready. Flush it now, before anything else, so the
            // vault still reads "prior crash -> new boot" in order.
            if (g_fieldVaultBootRecordPending) {
                _writeFieldVaultBootRecords();
            }

            const bool duePowerSample =
                lastFieldVaultPowerSampleMs == 0 ||
                (loopNow - lastFieldVaultPowerSampleMs) >= FIELDVAULT_POWER_SAMPLE_INTERVAL_MS;
            if (power.sourceChanged || power.stateChanged || power.criticalJustEntered ||
                duePowerSample) {
                const char* reason = power.criticalJustEntered ? "critical" :
                                     power.sourceChanged ? "source" :
                                     power.stateChanged ? "state" :
                                     (lastFieldVaultPowerSampleMs == 0 ? "boot" : "interval");
                _appendFieldVaultPowerSample(
                    power,
                    currentRadioOwner,
                    reason);
                lastFieldVaultPowerSampleMs = loopNow;
            }

            uint32_t pendingUpload = 0;
            uint32_t pendingEnrich = 0;
            bool uploadActive = false;
            STATE_READ_BEGIN();
            pendingUpload =
                g_state.storagePendingUploadMission + g_state.storagePendingUploadNoise;
            pendingEnrich =
                g_state.storagePendingEnrichMission + g_state.storagePendingEnrichNoise;
            uploadActive = g_state.uploadActive;
            STATE_READ_END();
#if WIO_NRF_ACCESSORY_ENABLED
            const bool wioAvailable = WIO_NRF.available();
            const bool wioBleProxy = WIO_NRF.hasBleProxy();
            const bool wioPhoneConnected = WIO_NRF.phoneConnected();
#else
            const bool wioAvailable = false;
            const bool wioBleProxy = false;
            const bool wioPhoneConnected = false;
#endif
            const bool firstRunSample = lastFieldVaultRunSampleMs == 0;
            const bool dueRunSample =
                firstRunSample ||
                (loopNow - lastFieldVaultRunSampleMs) >= FIELDVAULT_RUN_SAMPLE_INTERVAL_MS;
            const bool ownerChanged =
                !firstRunSample && currentRadioOwner != lastFieldVaultRunOwner;
            const bool uploadChanged =
                !firstRunSample && uploadActive != lastFieldVaultRunUploadActive;
            const bool wioChanged =
                !firstRunSample &&
                (wioAvailable != lastFieldVaultRunWioAvailable ||
                 wioBleProxy != lastFieldVaultRunWioBleProxy ||
                 wioPhoneConnected != lastFieldVaultRunWioPhoneConnected);
            const uint32_t pendingUploadDelta =
                (pendingUpload > lastFieldVaultRunPendingUpload)
                    ? (pendingUpload - lastFieldVaultRunPendingUpload)
                    : (lastFieldVaultRunPendingUpload - pendingUpload);
            const uint32_t pendingEnrichDelta =
                (pendingEnrich > lastFieldVaultRunPendingEnrich)
                    ? (pendingEnrich - lastFieldVaultRunPendingEnrich)
                    : (lastFieldVaultRunPendingEnrich - pendingEnrich);
            const bool pendingChanged =
                !firstRunSample &&
                (pendingUploadDelta >= FIELDVAULT_RUN_PENDING_DELTA_MIN ||
                 pendingEnrichDelta >= FIELDVAULT_RUN_PENDING_DELTA_MIN);
            const bool transitionChanged =
                ownerChanged || uploadChanged || wioChanged || pendingChanged;
            const bool transitionAllowed =
                firstRunSample ||
                lastFieldVaultRunTransitionMs == 0 ||
                (loopNow - lastFieldVaultRunTransitionMs) >=
                    FIELDVAULT_RUN_TRANSITION_MIN_MS;

            if (dueRunSample || (transitionChanged && transitionAllowed)) {
                const char* reason = firstRunSample ? "boot" :
                                     uploadChanged ? "upload" :
                                     wioChanged ? "wio" :
                                     ownerChanged ? "owner" :
                                     pendingChanged ? "pending" :
                                     "interval";
                _appendFieldVaultRunSample(currentRadioOwner, reason);
                lastFieldVaultRunSampleMs = loopNow;
                if (transitionChanged) {
                    lastFieldVaultRunTransitionMs = loopNow;
                }
                lastFieldVaultRunOwner = currentRadioOwner;
                lastFieldVaultRunPendingUpload = pendingUpload;
                lastFieldVaultRunPendingEnrich = pendingEnrich;
                lastFieldVaultRunUploadActive = uploadActive;
                lastFieldVaultRunWioAvailable = wioAvailable;
                lastFieldVaultRunWioBleProxy = wioBleProxy;
                lastFieldVaultRunWioPhoneConnected = wioPhoneConnected;
            }
        }

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

        // Signed-delta compare so the timeout still fires correctly across a
        // millis() wraparound (every ~49.7 days). A raw `loopNow >= deadline`
        // misfires immediately when millis() wraps past the stored deadline.
        if (!POWER_RUN_UNTIL_DEAD &&
            !sleepAlreadyRequested &&
            power.state == POWER_STATE_BATTERY_CRITICAL &&
            power.criticalSleepAtMs != 0 &&
            static_cast<int32_t>(loopNow - power.criticalSleepAtMs) >= 0) {
            _requestSleepTransition(storageOk, false, false, "critical_timeout");
        }

        _serviceSleepTransition(loopNow);

        _pollUsbSerialConsole();
        BADUSB_MGR.tick();
        WIO_NRF.tick();
        PHONE_XPORT.tick();
        LOG_STREAMER.tick();
        DASHBOARD_STREAMER.tick();
        NOTIF_CENTER.tick();
        SUBGHZ.tick();
        MESHTASTIC.tick();

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

#if MESHTASTIC_ENABLED && WIO_NRF_ACCESSORY_ENABLED
        _applyMeshStatusToState();
#endif

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

        // Button GPIO edges are sampled by TaskButtons so a blocking storage,
        // WiFi, MQTT, or enrichment slice cannot make a short press vanish.
        // TaskHardware remains the sole action owner and drains the queued
        // semantic events here, preserving every existing routing contract.
        ButtonEvent evt = BTN_NONE;
        if (s_buttonEventQueue) {
            xQueueReceive(s_buttonEventQueue, &evt, 0);
        } else {
            // Boot-time allocation/task failure fallback. This retains the
            // previous behavior instead of leaving the unit with no input.
            evt = buttons.getEvent();
        }

        // Global sleep chord: holding both buttons ~1.5s requests deep sleep
        // from ANY screen, and even when the display has blanked. Handled
        // before the display-wake swallow below so the user doesn't have to
        // first wake the screen and then repeat the hold. It never collides
        // with per-screen A/B bindings because those have no combo slot.
        if (evt == BTN_AB_LONG) {
            _markUiActivity();
            _emitButtonLog("evt=AB_LONG action=SLEEP route=global");
            _requestSleepTransition(storageOk, true, true, "manual_sleep");
            continue;
        }

        if (evt != BTN_NONE) {
            _markUiActivity();
            if (!_isDisplayAwake()) {
                _setDisplayAwake(true);
                STATE_WRITE_BEGIN();
                g_state.screenChanged = true;
                STATE_WRITE_END();
                char btnMsg[160];
                snprintf(btnMsg, sizeof(btnMsg),
                         "evt=%s consumed as display wake",
                         _buttonEventName(evt));
                _emitButtonLog(btnMsg);
                continue;
            }
        }
        if (!PHONE_XPORT.isWioActive() && BLE_MGR.handleButtonEvent(evt)) {
            if (evt != BTN_NONE) {
                char btnMsg[160];
                snprintf(btnMsg, sizeof(btnMsg),
                         "evt=%s route=BLE",
                         _buttonEventName(evt));
                _emitButtonLog(btnMsg);
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
                char btnMsg[192];
                snprintf(btnMsg, sizeof(btnMsg),
                         "evt=%s screen=%s overlay=%s combo=PWNY_ARM handled=%d",
                         _buttonEventName(evt),
                         _screenName(route.currentScreen),
                         overlay,
                         handled ? 1 : 0);
                _emitButtonLog(btnMsg);
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
            char btnMsg[208];
            snprintf(btnMsg, sizeof(btnMsg),
                     "evt=%s screen=%s overlay=%s action=%s label=%s handled=%d",
                     _buttonEventName(evt),
                     _screenName(route.currentScreen),
                     overlay,
                     _buttonActionName(action),
                     spectreButtonActionLabel(action) ? spectreButtonActionLabel(action) : "-",
                     handled ? 1 : 0);
            _emitButtonLog(btnMsg);

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

        // Let queued or active user companion work preempt routine maintenance.
        // Otherwise a multi-window audit can reacquire the storage lease before
        // the companion scheduler has a chance to consume the request.
        if (storageOk && RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE) &&
            !g_companionCmd.link &&
            !g_companionCmd.probe &&
            !g_companionCmd.enrich &&
            !g_companionCmd.cancel &&
            !companionHasPriorityReason(companion) &&
            !STORAGE.isEnrichmentWindowResident() &&
            companion.workState != COMPANION_WORK_ENRICHING) {
            const bool rebuildCaptureIndex =
                (STORAGE.maintenanceFlags() &
                 STORAGE_MAINT_CAPTURE_INDEX_DIRTY) != 0U;
            const uint32_t maintBudgetMs =
                STORAGE.isCaptureSafeToResume() ? 5000UL : 30000UL;
            _publishStorageMaintenanceMirror(true, true);
            const uint32_t maintStartMs = millis();
            bool captureSafe = STORAGE.isCaptureSafeToResume();
            StorageExclusiveWindow maintWindow;
            if (maintWindow.begin(STORAGE_WINDOW_MAINTENANCE,
                                  "radio_owner_window")) {
                captureSafe = STORAGE.runMaintenanceWindow(maintBudgetMs,
                                                           "radio_owner_window");
                if (rebuildCaptureIndex && WIFI_MGR.rebuildPmkidCaptureIndex()) {
                    STORAGE.completeExternalMaintenance(
                        STORAGE_MAINT_CAPTURE_INDEX_DIRTY,
                        "pmkid_index_rebuild");
                }
                maintWindow.end(captureSafe ? "capture_safe" : "pending");
            } else {
                DLOG_WARN("STORAGE",
                          "maintenance exclusive window unavailable remaining=%s",
                          STORAGE.maintenanceFlagsText());
            }
            const uint32_t maintMs = millis() - maintStartMs;
            g_lastMaintenanceRunMs = millis();
            _publishStorageMaintenanceMirror(true, false, true, maintMs);
            DLOG_INFO("STORAGE",
                      "maintenance owner window ms=%lu captureSafe=%d remaining=%s",
                      static_cast<unsigned long>(maintMs),
                      captureSafe ? 1 : 0,
                      STORAGE.maintenanceFlagsText());
            RADIO_ARB.release(RADIO_STORAGE_MAINTENANCE,
                              captureSafe ? "storage_maintenance_done"
                                          : "storage_maintenance_pending");
            continue;
        }

        if (!PHONE_XPORT.isWioActive() && BLE_MGR.consumeWireGuardDumpTrigger()) {
            MQTT_MGR.requestDump(true);
            DLOG_INFO("BLE", "WireGuard dump triggered");
        }

        static uint32_t lastUploadCheck = 0;
        if (millis() - lastUploadCheck > 60000) {
            lastUploadCheck = millis();

            const int uploadReady = MQTT_MGR.uploadReadyCount();
            if (uploadReady > 0 &&
                (MQTT_MGR.backlogDrainActive() ||
                 uploadReady >= MQTT_UPLOAD_READY_THRESHOLD)) {
                MQTT_MGR.requestDump(false);
            }
        }

        // Periodic FS audit safety net: if no audit has completed in the last
        // 24 hours and storage maintenance has nothing else queued, request
        // one. The maintenance owner picks the appropriate ladder rung when
        // it actually runs the pass.
        static uint32_t lastPeriodicAuditCheck = 0;
        if (storageOk && millis() - lastPeriodicAuditCheck > 60000UL) {
            lastPeriodicAuditCheck = millis();
            constexpr uint32_t kPeriodicAuditMs = 24UL * 3600UL * 1000UL;
            const uint32_t lastAudit = STORAGE.lastFsAuditCompletedMs();
            const bool overdue =
                lastAudit != 0 &&
                (millis() - lastAudit > kPeriodicAuditMs);
            if (overdue && !STORAGE.hasMaintenanceWork()) {
                STORAGE.requestMaintenance(STORAGE_MAINT_FS_AUDIT,
                                           "periodic_24h");
            }
        }

        char inputBuf[64] = "";
        const bool consumedText =
            PHONE_XPORT.consumeTextInput(inputBuf, sizeof(inputBuf));
        if (consumedText) {
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
#if MESHTASTIC_ENABLED && WIO_NRF_ACCESSORY_ENABLED
            } else if (strncmp(prompt, "Mesh message:", 13) == 0) {
                const bool sent = MESHTASTIC.sendText(inputBuf);
                _queueNotification(NOTIF_DEVICE_NEW,
                                   sent ? "MESH SENT" : "MESH SEND FAIL");
                DLOG_INFO("MESH", "Manual send (%s): %s",
                          sent ? "ok" : "fail", inputBuf);
#endif
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

        // Command traffic is not guaranteed after Android is backgrounded.
        // Expire abandoned transfers from the hardware loop even when the
        // companion scheduler is disabled or no more commands arrive.
        PHONE_OFFLOAD.expireIfStale();
        // OFFLOAD_BEGIN only queues the expensive LittleFS index walk. Run it
        // here so TaskHardware remains the sole persistent-storage mutator;
        // the storage scanners yield periodically to keep both idle-task
        // watchdogs serviced while the field backlog is indexed.
        PHONE_OFFLOAD.servicePreparation();

        if (!companion.enabled) {
            // Companion disabled: discard any console-queued requests so they
            // can't fire if the feature is later re-enabled at runtime.
            g_companionCmd.link   = false;
            g_companionCmd.probe  = false;
            g_companionCmd.enrich = false;
            g_companionCmd.upload = false;
            g_companionCmd.cancel = false;
            endManualEnrichmentExclusive(companion, "companion_disabled");
            companion.workState = COMPANION_WORK_IDLE;
            companion.enrichmentRequestIssued = false;
            companion.manualProbeRequested = false;
            companion.manualLinkHold = false;
            companion.manualEnrichRequested = false;
            companion.manualEnrichProbeBypass = false;
            companion.offloadPrepRequested = false;
            companion.automaticOffloadHold = false;
            companion.automaticOffloadWasActive = false;
            companion.automaticOffloadPrepObserved = false;
            companion.automaticOffloadDeadlineMs = 0;
            companion.automaticOffloadRetryNotBeforeMs = 0;
            companion.timeSyncRequested = false;
            companion.externalTransportActive = false;
        } else {
            const bool externalProxy = PHONE_XPORT.isWioActive();

            // Drain console-issued companion commands into the scheduler.
            // The console (also running on TaskHardware via _pollUsbSerialConsole)
            // writes g_companionCmd.{link,probe,enrich,upload,cancel}; we read-and-clear here
            // so the priority flags are visible to companionHasPriorityReason()
            // for the rest of this same tick. Cancel is processed first so a
            // simultaneously-queued probe/enrich is also wiped.
            const bool phoneUploadRequested = g_companionCmd.upload;
            if (phoneUploadRequested) {
                g_companionCmd.upload = false;
                // Upload owns Wi-Fi exclusively. Reuse the normal companion
                // cancel path to commit queued enrichment and release BLE first.
                g_companionCmd.cancel = true;
            }
            if (g_companionCmd.cancel) {
                g_companionCmd.cancel = false;
                g_companionCmd.link   = false;
                g_companionCmd.probe  = false;
                g_companionCmd.enrich = false;
                if (companion.manualProbeRequested ||
                    companion.manualEnrichRequested ||
                    companion.offloadPrepRequested ||
                    companion.timeSyncRequested) {
                    DLOG_INFO("COMP",
                              "Cancel: clearing pending companion requests");
                }
                // A response may already have passed authentication and been
                // admitted to the bounded FIFO while the cancel command was
                // waiting in the console queue.  Commit every admitted batch
                // before resetting claims/indices so cancel cannot silently
                // discard valid phone enrichment.
                while (enrichQueueSize > 0) {
                    const size_t drained = runEnrichDrain(companion, "COMP");
                    if (drained == 0) {
                        DLOG_WARN("COMP",
                                  "Cancel deferred: admitted enrichment queue cannot drain size=%u",
                                  static_cast<unsigned>(enrichQueueSize));
                        break;
                    }
                }
                companion.manualProbeRequested  = false;
                companion.manualLinkHold = false;
                companion.manualEnrichRequested = false;
                companion.manualEnrichProbeBypass = false;
                companion.offloadPrepRequested  = false;
                companion.automaticOffloadHold = false;
                companion.automaticOffloadWasActive = false;
                companion.automaticOffloadPrepObserved = false;
                companion.automaticOffloadDeadlineMs = 0;
                companion.automaticOffloadRetryNotBeforeMs =
                    millis() + OFFLOAD_RETRY_BACKOFF_MS;
                companion.timeSyncRequested     = false;
                companion.workState = COMPANION_WORK_IDLE;
                companion.enrichmentRequestIssued = false;
                companion.lastRequestedEnrichmentCount = 0;
                endManualEnrichmentExclusive(companion, "companion_cancel");
                if (companion.enrichmentWindowActive) {
                    STORAGE.releaseEnrichmentIndexMemory("companion_cancel");
                    companion.enrichmentWindowActive = false;
                }
                if (enrichQueueSize == 0) {
                    initEnrichQueue();
                    enrichClearAllClaims();
                }
                if (RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
                    RADIO_ARB.release(RADIO_BLE_GPS, "companion_cancel");
                }
                companion.externalTransportActive = false;
            }
            if (phoneUploadRequested) {
                MQTT_MGR.requestUploadResume("phone_upload_now");
                const int pending = MQTT_MGR.uploadReadyCount();
                const MQTTState state = MQTT_MGR.getState();
                if (state != MQTT_IDLE) {
                    DLOG_INFO("CMD", "phone upload-now accepted: already active state=%s pending=%d",
                              _mqttStateName(state), pending);
                } else if (pending <= 0) {
                    DLOG_INFO("CMD", "phone upload-now accepted: no pending records");
                } else if (MQTT_MGR.requestDump(true)) {
                    DLOG_INFO("CMD", "phone upload-now queued pending=%d", pending);
                } else {
                    DLOG_WARN("CMD", "phone upload-now deferred owner=%s pending=%d",
                              RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                              pending);
                }
            }
            if (g_companionCmd.link) {
                g_companionCmd.link = false;
                companion.manualLinkHold = true;
                companion.manualProbeRequested = true;
                companion.nextProbeMs = 0;
                companion.probeBackoffStage = 0;
                companion.probeBackoffMissCount = 0;
                DLOG_INFO("COMP",
                          "Persistent link request received; holding until release");
            }

            serviceAutomaticOffloadHold(companion);
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
                companion.manualEnrichProbeBypass = true;
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

            if (!externalProxy && companion.externalTransportActive) {
                DLOG_WARN("WIO", "External companion transport unavailable; falling back to internal BLE");
                enrichClearAllClaims();
                companion.externalTransportActive = false;
                companion.enrichmentRequestIssued = false;
                companion.lastRequestedEnrichmentCount = 0;
                if (companion.workState == COMPANION_WORK_PROBING ||
                    companion.workState == COMPANION_WORK_ENRICHING) {
                    companion.workState = COMPANION_WORK_IDLE;
                    companion.phoneState = COMPANION_PHONE_UNKNOWN;
                }
            }

            if (externalProxy) {
                if (RADIO_ARB.isBleOwner()) {
                    const RadioOwner bleOwner = RADIO_ARB.currentOwner();
                    RADIO_ARB.release(bleOwner, "external_ble_proxy");
                    enrichClearAllClaims();
                    crashBreadcrumbClear(CrashPhase::BACKLOG_PROBE);
                    crashBreadcrumbClear(CrashPhase::BACKLOG_ENRICH);
                }

                if (WIO_NRF.isPhoneCompanionReady()) {
                    companion.phoneState = COMPANION_PHONE_AVAILABLE;
                    companion.lastSeenMs = millis();
                    companion.nextProbeMs = 0;
                    companion.probeBackoffStage = 0;
                    companion.probeBackoffMissCount = 0;
                } else if (companionPhoneAvailabilityStale(companion)) {
                    companion.phoneState = COMPANION_PHONE_UNKNOWN;
                }

                // Auto exclusive enrich: the phone is reachable and the enrich
                // backlog crossed the threshold, so commit to a full exclusive
                // drain (stop capture, enrich + 2-pass-retire the whole backlog,
                // then return to capture) instead of a capture-coexisting
                // trickle. Reuses the validated manual-drain machinery; the probe
                // cadence/backoff rate-limits how often we re-enter, and once the
                // backlog is drained the threshold stops re-triggering it.
                if (companion.phoneState == COMPANION_PHONE_AVAILABLE &&
                    companion.workState == COMPANION_WORK_IDLE &&
                    !companion.manualEnrichRequested &&
                    !companion.offloadPrepRequested &&
                    !companion.timeSyncRequested &&
                    companion.pendingItems >= ENRICH_PENDING_THRESHOLD_WIO) {
                    companion.manualEnrichRequested = true;
                    DLOG_INFO("COMP",
                              "Auto exclusive enrich: phone ready, backlog=%lu >= %lu; drain then resume capture",
                              static_cast<unsigned long>(companion.pendingItems),
                              static_cast<unsigned long>(ENRICH_PENDING_THRESHOLD_WIO));
                }

                if (companion.manualProbeRequested) {
                    companion.nextProbeMs = 0;
                }

                if (companion.phoneState != COMPANION_PHONE_AVAILABLE &&
                    companion.workState == COMPANION_WORK_IDLE &&
                    (companion.manualProbeRequested ||
                     companionHasAutomaticEnrichmentWork(
                         companion, ENRICH_PENDING_THRESHOLD_WIO)) &&
                    shouldRunExternalPhoneProbe(companion)) {
                    requestExternalPhoneProbe(
                        companion,
                        companion.offloadPrepRequested ? PHONE_PROBE_OFFLOAD_PREP :
                        (companion.manualProbeRequested ||
                         companion.manualEnrichRequested) ? PHONE_PROBE_MANUAL :
                        companion.timeSyncRequested ? PHONE_PROBE_TIME_SYNC :
                                                      PHONE_PROBE_BACKLOG
                    );
                }

                if (shouldRunExternalEnrichment(companion)) {
                    requestExternalPhoneEnrichment(
                        companion,
                        companion.offloadPrepRequested ? "external_offload_enrich" :
                        companion.manualEnrichRequested ? "external_manual_enrich" :
                        companion.timeSyncRequested ? "external_time_sync_enrich" :
                                                      "external_backlog_enrich"
                    );
                }
            } else if (BLE_MGR.isPhoneCompanionReady()) {
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

            // Capture can add new raw records while an upload-only probe is in
            // flight. Once enrichment work exists, this is no longer an
            // offload-preparation request; normal threshold/backoff scheduling
            // must decide when to contact the phone.
            if (companion.offloadPrepRequested &&
                companion.pendingItems > 0) {
                deferAutomaticOffloadRecovery(companion,
                                              "new_enrichment_work");
            }

            if (!externalProxy &&
                companion.workState == COMPANION_WORK_IDLE &&
                companion.phoneState != COMPANION_PHONE_AVAILABLE &&
                !companion.manualProbeRequested &&
                !companion.manualEnrichRequested &&
                !companion.offloadPrepRequested &&
                !companion.timeSyncRequested &&
                companionHasAutomaticOffloadWork(companion)) {
                companion.offloadPrepRequested = true;
                companion.phoneState = COMPANION_PHONE_UNKNOWN;
                companion.nextProbeMs = 0;
                DLOG_INFO("COMP",
                          "Automatic phone offload recovery requested");
            }

            if (!externalProxy &&
                companion.workState == COMPANION_WORK_IDLE &&
                RADIO_ARB.isOwner(RADIO_BLE_GPS) &&
                BLE_MGR.isPhoneCompanionReady() &&
                (companion.manualEnrichRequested ||
                 companion.offloadPrepRequested ||
                 companion.timeSyncRequested)) {
                RADIO_ARB.refreshLease(RADIO_BLE_GPS,
                                       RadioArbiter::BLE_PHONE_ENRICH_HOLD_MS,
                                       "linked_manual_enrich");
                if (companion.offloadPrepRequested &&
                    companion.pendingItems == 0) {
                    beginAutomaticOffloadHold(companion,
                                              "upload_only_link_ready");
                } else {
                    companion.workState = COMPANION_WORK_ENRICHING;
                    resetEnrichmentSessionStats(companion);
                    companion.enrichmentRequestIssued = false;
                    companion.lastRequestedEnrichmentCount = 0;
                    crashCheckpoint(CrashPhase::BACKLOG_ENRICH,
                                    static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                                    static_cast<uint32_t>(companion.pendingItems));
                    DLOG_INFO("BLE",
                              "Active BLE link promoted to enrichment pendingEnrich=%lu manual=%u timeSync=%u",
                              static_cast<unsigned long>(companion.pendingItems),
                              companion.manualEnrichRequested ? 1u : 0u,
                              companion.timeSyncRequested ? 1u : 0u);
                }
            }

            // Opportunistic probe if work exists but phone is not known available yet.
            if (!externalProxy &&
                companion.phoneState != COMPANION_PHONE_AVAILABLE &&
                companion.workState == COMPANION_WORK_IDLE &&
                (companion.manualLinkHold ||
                 companion.manualProbeRequested ||
                 companion.offloadPrepRequested ||
                 companionHasAutomaticEnrichmentWork(
                     companion, ENRICH_PENDING_THRESHOLD_INTERNAL))) {

                if (shouldRunPhoneProbe(companion)) {
                    const PhoneProbeReason reason =
                        companion.offloadPrepRequested ? PHONE_PROBE_OFFLOAD_PREP :
                        (companion.manualProbeRequested ||
                         companion.manualEnrichRequested) ? PHONE_PROBE_MANUAL :
                        companion.timeSyncRequested ? PHONE_PROBE_TIME_SYNC :
                                                      PHONE_PROBE_BACKLOG;
                    if (!requestPhoneProbeLease(companion, reason) &&
                        reason == PHONE_PROBE_OFFLOAD_PREP) {
                        deferAutomaticOffloadRecovery(
                            companion, "probe_lease_failed");
                    }
                }
            }

            // Start enrichment only after phone is known available and WiFi is in a lull.
            if (!externalProxy && shouldRunEnrichment(companion)) {
                requestPhoneEnrichmentLease(
                    companion,
                    companion.offloadPrepRequested ? "offload_enrich" :
                    companion.manualEnrichRequested ? "manual_enrich" :
                    companion.timeSyncRequested ? "time_sync_enrich" :
                                                  "backlog_enrich"
                );
            }
        }

        publishCompanionState(companion);

        uint32_t wifiTickMs = 100;
        if (RADIO_ARB.isOwner(RADIO_WIFI_CAPTURE) ||
    RADIO_ARB.isOwner(RADIO_WIFI_PMKID)) {
    wifiTickMs = 25;
        } else if (RADIO_ARB.isOwner(RADIO_WIFI_SCAN)) {
    wifiTickMs = 50;
        }

        if (millis() - lastWifiTick >= wifiTickMs) {
            if (ExecutionPolicy::shouldTickWiFi(RADIO_ARB.currentOwner())) {
                const uint32_t sectionStartMs = millis();
                WIFI_MGR.tick();
                _logHardwareSectionIfSlow("wifi_tick", sectionStartMs, storageOk);
            }
            lastWifiTick = millis();
        }

        static uint32_t lastBleTick = 0;
        uint32_t bleTickMs = 200;

        if (RADIO_ARB.isOwner(RADIO_BLE_TEXT)) {
    bleTickMs = 100;
        } else if (RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
    // The phone command channel is an ordered request/response transport.
    // At 150 ms every offload chunk paid one or two scheduler periods, which
    // capped a healthy MTU-247 link at roughly two records/second. Capture is
    // already paused while BLE_GPS owns the radio, so service the brief field
    // handoff aggressively and return to capture sooner.
    bleTickMs = 20;
        }

        if (PHONE_XPORT.isWioActive() && RADIO_ARB.isBleOwner()) {
            RADIO_ARB.release(RADIO_ARB.currentOwner(), "external_ble_proxy_tick_guard");
        }

        if (millis() - lastBleTick >= bleTickMs) {
            if (!PHONE_XPORT.isWioActive() &&
                ExecutionPolicy::shouldTickBle(RADIO_ARB.currentOwner())) {
                const uint32_t sectionStartMs = millis();
                BLE_MGR.tick();
                _logHardwareSectionIfSlow("ble_tick", sectionStartMs, storageOk);
            }
            lastBleTick = millis();
        }

        if (companion.enabled && PHONE_XPORT.isWioActive()) {
            if (companion.workState == COMPANION_WORK_PROBING) {
                if (WIO_NRF.isPhoneCompanionReady()) {
                    companion.phoneState = COMPANION_PHONE_AVAILABLE;
                    companion.lastSeenMs = millis();
                    companion.nextProbeMs = 0;
                    companion.probeBackoffStage = 0;
                    companion.probeBackoffMissCount = 0;
                    companion.manualProbeRequested = false;
                    companion.manualEnrichProbeBypass = false;
                    refreshCompanionPending(companion, true);
                    publishPhoneStorageSnapshotIfDue(companion, true);
                    DLOG_INFO("WIO", "External phone probe succeeded");

                    if (companionHasEnrichmentWork(companion)) {
                        companion.workState = COMPANION_WORK_ENRICHING;
                        resetEnrichmentSessionStats(companion);
                        companion.enrichmentRequestIssued = false;
                        companion.lastRequestedEnrichmentCount = 0;
                        initEnrichQueue();
                    } else {
                        companion.workState = COMPANION_WORK_IDLE;
                        WIO_NRF.disconnectPhone("probe_success");
                    }
                } else if (WIO_NRF.consumeEnrichmentFailure()) {
                    companion.phoneState = COMPANION_PHONE_UNAVAILABLE;
                    companion.workState = COMPANION_WORK_IDLE;
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
                    DLOG_WARN("WIO",
                              "external_probe_summary result=not_seen stage=%u miss=%u nextIn=%lus",
                              static_cast<unsigned>(companion.probeBackoffStage),
                              static_cast<unsigned>(companion.probeBackoffMissCount),
                              static_cast<unsigned long>(interval / 1000UL));
                }
            }

            if (companion.workState == COMPANION_WORK_ENRICHING) {
                serviceExternalEnrichmentPipeline(companion);
            }
        } else if (companion.enabled) {
            if (companion.workState == COMPANION_WORK_PROBING) {
                if (BLE_MGR.isPhoneCompanionReady()) {
                    companion.phoneState = COMPANION_PHONE_AVAILABLE;
                    companion.lastSeenMs = millis();
                    companion.nextProbeMs = 0;
                    companion.probeBackoffStage    = 0;
                    companion.probeBackoffMissCount = 0;
                    companion.manualProbeRequested = false;
                    companion.manualEnrichProbeBypass = false;
                    refreshCompanionPending(companion, true);
                    publishPhoneStorageSnapshotIfDue(companion, true);
                    crashBreadcrumbClear(CrashPhase::BACKLOG_PROBE);
                    DLOG_INFO("BLE", "Phone probe succeeded — backoff reset");

                    if (companion.manualLinkHold) {
                        companion.workState = COMPANION_WORK_IDLE;
                        DLOG_INFO("BLE", "Persistent companion link established");
                    } else if (companion.offloadPrepRequested &&
                               companion.pendingItems == 0 &&
                               RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
                        beginAutomaticOffloadHold(companion,
                                                  "upload_only_probe_ready");
                    } else if (companionHasEnrichmentWork(companion) &&
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
                        RADIO_ARB.isOwner(RADIO_BLE_GPS) &&
                        !companion.manualLinkHold &&
                        !companion.automaticOffloadHold) {
                        RADIO_ARB.release(RADIO_BLE_GPS, "probe_success");
                    }
                } else if (!RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
                    companion.phoneState = COMPANION_PHONE_UNAVAILABLE;
                    companion.workState = COMPANION_WORK_IDLE;
                    crashBreadcrumbClear(CrashPhase::BACKLOG_PROBE);

                    // An upload-only recovery probe is one bounded attempt.
                    // Leaving this priority flag set bypasses nextProbeMs and
                    // immediately reacquires BLE_GPS forever when the phone is
                    // absent.
                    deferAutomaticOffloadRecovery(companion,
                                                  "phone_not_seen");

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

        PHONE_OFFLOAD.tickWifiBulk();
        if (RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD) && !PHONE_OFFLOAD.wifiBulkActive()) {
            MQTT_MGR.tick();
        }

        RADIO_ARB.tick();

        ENTITY_MGR.tick();

        syncRuntimePresentation();

        const uint32_t now = millis();
        const ExecutionPolicy::UiRefreshSchedule uiRefreshSchedule =
            _uiRefreshScheduleForPowerState(power.state);
        uiRefreshMarks.wifiMs = lastWifiRefresh;
        uiRefreshMarks.pwnyMs = lastPwnyRefresh;
        uiRefreshMarks.systemMs = lastSystemRefresh;
        uint32_t sectionStartMs = millis();
        _servicePeriodicUiRefresh(_readUiRefreshState(),
                                  now,
                                  uiRefreshMarks,
                                  uiRefreshSchedule);
        _logHardwareSectionIfSlow("ui_refresh", sectionStartMs, storageOk);
        lastWifiRefresh = uiRefreshMarks.wifiMs;
        lastPwnyRefresh = uiRefreshMarks.pwnyMs;
        lastSystemRefresh = uiRefreshMarks.systemMs;

        if (storageOk &&
            RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
            STORAGE.shouldRunMaintenanceNow() &&
            now - g_lastCaptureMaintenanceMs >= CAPTURE_MAINTENANCE_MIN_GAP_MS &&
            canRunStorageMaintenance(companion, power, now)) {
            g_lastCaptureMaintenanceMs = now;
            sectionStartMs = millis();
            if (STORAGE.needsMaintenanceBeforeCapture()) {
                RADIO_ARB.release(RADIO_WIFI_CAPTURE,
                                  "capture_storage_maintenance",
                                  false);
                const bool granted = RADIO_ARB.requestStorageMaintenanceLease(
                    STORAGE.isCaptureSafeToResume() ? 5000UL : 30000UL,
                    "capture_storage_maintenance",
                    true);
                if (!granted) {
                    DLOG_WARN("STORAGE",
                              "capture maintenance lease unavailable owner=%s flags=%s",
                              RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                              STORAGE.maintenanceFlagsText());
                    RADIO_ARB.ensureDefaultCapture(
                        "capture_storage_maintenance_unavailable");
                }
            } else {
                const bool progressed = STORAGE.runCaptureMaintenanceSlice(
                    CAPTURE_MAINTENANCE_BUDGET_MS,
                    "capture_micro_idle");
                g_lastMaintenanceRunMs = millis();
                if (progressed) {
                    _publishStorageMaintenanceMirror(true,
                                                     false,
                                                     true,
                                                     millis() - sectionStartMs);
                }
            }
            _logHardwareSectionIfSlow("capture_storage_maintenance",
                                      sectionStartMs,
                                      storageOk);
        }

        if (storageOk &&
            RADIO_ARB.currentOwner() == RADIO_NONE &&
            STORAGE.shouldRunMaintenanceNow() &&
            !WIO_NRF.isCompanionLinkBusy() &&
            !WIO_NRF.isEnrichmentExchangeActive() &&
            !STORAGE.isEnrichmentWindowResident() &&
            !companion.manualEnrichRequested) {
            RADIO_ARB.requestStorageMaintenanceLease(
                STORAGE.isCaptureSafeToResume() ? 5000UL : 30000UL,
                "idle_storage_maintenance",
                true);
        }

        sectionStartMs = millis();
        _checkRuntimeContracts();
        _logHardwareSectionIfSlow("runtime_contracts", sectionStartMs, storageOk);

        if (now - lastStackLogMs >= STACK_LOG_INTERVAL_MS) {
            sectionStartMs = millis();
            const UBaseType_t freeWords = uxTaskGetStackHighWaterMark(nullptr);
            if (freeWords < minStackWords) {
                minStackWords = freeWords;
            }
            DLOG_INFO("STACK", "TaskHardware watermark=%luB min=%luB",
                      (unsigned long)(freeWords * sizeof(StackType_t)),
                      (unsigned long)(minStackWords * sizeof(StackType_t)));
            lastStackLogMs = now;
            _logHardwareSectionIfSlow("stack_log", sectionStartMs, storageOk);
        }
        sectionStartMs = millis();
        _releaseIdleBleStack(now);
        _logHardwareSectionIfSlow("ble_idle_teardown", sectionStartMs, storageOk);

        sectionStartMs = millis();
        _enforceCaptureHeapGuard(now);
        _logHardwareSectionIfSlow("capture_heap_guard", sectionStartMs, storageOk);

        sectionStartMs = millis();
        _logRuntimeHealth(now);
        _logHardwareSectionIfSlow("health_log", sectionStartMs, storageOk);

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

// Report any task whose stack is not in internal DRAM.
//
// A stack in PSRAM is a latent DoubleException: spi_flash disables the cache
// for a write, PSRAM goes with it, and the next register-window spill faults
// inside the exception handler. That took the device down repeatedly on
// 2026-08-20 (BLEWorker, explicitly created with MALLOC_CAP_SPIRAM).
//
// It can also happen without anyone asking for it: pvPortMallocStack resolves
// to pvPortMalloc -> MALLOC_CAP_8BIT, which matches internal AND PSRAM, so a
// plain xTaskCreate() falls back to PSRAM when internal DRAM is too tight or
// too fragmented to satisfy the request. That makes it a pressure-dependent
// bug that will not reproduce on a quiet bench.
//
// Returns the number of offending tasks. `verbose` also lists the clean ones.
static uint32_t _auditTaskStackPlacement(bool verbose) {
    return TaskStackAudit::run(verbose).offenders;
}

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
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        // IDF 5.1+ splits reasons that used to fall through to "unknown". USB
        // is the common one on this board: the 1200bps-touch flash, a replug
        // and host re-enumeration all reset the chip through the USB
        // peripheral, and reporting that as unknown made every flash look like
        // a crash on the boot summary screen.
        case ESP_RST_USB:        return "usb_peripheral";
        case ESP_RST_JTAG:       return "jtag";
        case ESP_RST_EFUSE:      return "efuse_error";
        case ESP_RST_PWR_GLITCH: return "power_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
#endif
        default:                return "unknown";
    }
}

// Crash-like = the firmware lost control. USB/JTAG/SDIO resets and deliberate
// software restarts are not faults and must not be reported as such.
static bool _resetReasonIsCrashLike(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        case ESP_RST_PWR_GLITCH:
        case ESP_RST_CPU_LOCKUP:
#endif
            return true;
        default:
            return false;
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

static const char* _fieldVaultPowerStateName(PowerState state) {
    switch (state) {
        case POWER_STATE_USB:              return "usb";
        case POWER_STATE_BATTERY_NORMAL:   return "battery_normal";
        case POWER_STATE_BATTERY_ECONOMY:  return "battery_economy";
        case POWER_STATE_BATTERY_CRITICAL: return "battery_critical";
        default:                           return "unknown";
    }
}

void _appendFieldVaultPowerSample(const PowerSnapshot& power,
                                  uint8_t radioOwner,
                                  const char* reason) {
    if (!FieldVault::isReady()) {
        return;
    }

    uint32_t uptimeMs = 0;
    STATE_READ_BEGIN();
    uptimeMs = g_state.uptimeMs;
    STATE_READ_END();

    if (!FieldVault::appendPowerSample(
            power.voltageMv,
            power.percent,
            power.trendMvPerMin,
            power.batteryCapacityMah,
            power.runtimeRemainingMin,
            _fieldVaultPowerSourceName(power.source),
            _fieldVaultPowerStateName(power.state),
            power.charging,
            radioOwner,
            uptimeMs,
            reason)) {
        DLOG_WARN("FIELDVAULT", "power sample append failed");
    }
}

void _appendFieldVaultRunSample(uint8_t radioOwner,
                                const char* reason) {
    if (!FieldVault::isReady()) {
        return;
    }

    char sessionId[40] = "";
    uint32_t uptimeMs = 0;
    uint32_t pendingUpload = 0;
    uint32_t pendingEnrich = 0;
    uint16_t wifiCount = 0;
    uint32_t probeCount = 0;
    uint32_t loraPackets = 0;
    uint8_t subGhzMode = 0;
    uint16_t subGhzNodes = 0;
    bool uploadActive = false;

    STATE_READ_BEGIN();
    strlcpy(sessionId, g_state.sessionId, sizeof(sessionId));
    uptimeMs = g_state.uptimeMs;
    pendingUpload =
        g_state.storagePendingUploadMission + g_state.storagePendingUploadNoise;
    pendingEnrich =
        g_state.storagePendingEnrichMission + g_state.storagePendingEnrichNoise;
    const int networks =
        (g_state.sessionNetworks > 0) ? g_state.sessionNetworks
                                      : g_state.wifiNetworkCount;
    const int probes =
        (g_state.sessionProbes > 0) ? g_state.sessionProbes
                                    : g_state.probePacketCount;
    wifiCount = static_cast<uint16_t>(
        networks < 0 ? 0 : (networks > 65535 ? 65535 : networks));
    probeCount = static_cast<uint32_t>(probes < 0 ? 0 : probes);
    loraPackets = static_cast<uint32_t>(
        g_state.loraPacketCount < 0 ? 0 : g_state.loraPacketCount);
    subGhzMode = g_state.subGhzMode;
    subGhzNodes = static_cast<uint16_t>(
        g_state.subGhzNodeCount < 0 ? 0 :
            (g_state.subGhzNodeCount > 65535 ? 65535 : g_state.subGhzNodeCount));
    uploadActive = g_state.uploadActive;
    STATE_READ_END();

#if WIO_NRF_ACCESSORY_ENABLED
    const bool wioAvailable = WIO_NRF.available();
    const bool wioBleProxy = WIO_NRF.hasBleProxy();
    const bool wioPhoneConnected = WIO_NRF.phoneConnected();
#else
    const bool wioAvailable = false;
    const bool wioBleProxy = false;
    const bool wioPhoneConnected = false;
#endif

    const uint32_t heapFreeKb =
        heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024UL;
    const uint32_t internalFreeKb =
        heap_caps_get_free_size(SPECTRE_CAP_DRAM) / 1024UL;

    if (!FieldVault::appendRunSample(sessionId,
                                     uptimeMs,
                                     radioOwner,
                                     pendingUpload,
                                     pendingEnrich,
                                     wifiCount,
                                     probeCount,
                                     loraPackets,
                                     subGhzMode,
                                     subGhzNodes,
                                     wioAvailable,
                                     wioBleProxy,
                                     wioPhoneConnected,
                                     uploadActive,
                                     heapFreeKb,
                                     internalFreeKb,
                                     reason)) {
        DLOG_WARN("FIELDVAULT", "run sample append failed");
    }

    // Ride the same sparse cadence to persist this run's totals as the "last
    // session" snapshot the Boot Summary shows after the next reboot.
    uint32_t recTotal = 0, pUpM = 0, pUpN = 0, pEnM = 0, pEnN = 0;
    STATE_READ_BEGIN();
    recTotal = g_state.storageEventTotal;
    pUpM = g_state.storagePendingUploadMission;
    pUpN = g_state.storagePendingUploadNoise;
    pEnM = g_state.storagePendingEnrichMission;
    pEnN = g_state.storagePendingEnrichNoise;
    STATE_READ_END();
    BootInfo::snapshotCurrentSession(recTotal, pUpM, pUpN, pEnM, pEnN,
                                     uptimeMs / 1000UL);
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
#if BOOT_SEQUENCE_VERBOSE_ACTIVE
            Serial.println("[BOOT] recovery button held; keep holding for recovery");
#endif
            announced = true;
        }
        delay(25);
    }

#if BOOT_SEQUENCE_VERBOSE_ACTIVE
    Serial.println("[BOOT] recovery mode requested");
#endif
    return true;
#else
    return false;
#endif
}

void setup() {
    Serial.begin(115200);
    delay(250);
    g_usbSerialAttachedAtBoot = static_cast<bool>(Serial);

    // Open a new crash-ring boot generation before any checkpoint is written,
    // so entries left unresolved by the previous boot stay evictable.
    crashLogBeginBoot();

    // Arm the heap allocation-failure hook before anything else allocates, so
    // the first failed request of the boot is always attributable.
    crashAllocFailInstall();

#if SPECTRE_EXTMEM_MALLOC_LIMIT > 0
    // Lower the internal/external malloc split before anything allocates, so the
    // 1-4 KB band lands in PSRAM instead of scarce internal DRAM. See the
    // `heap extmem` command for the rationale and for sweeping the value live.
    heap_caps_malloc_extmem_enable(SPECTRE_EXTMEM_MALLOC_LIMIT);
#endif

#if SPECTRE_BOOT_MEMPROBE
    _runBleMemProbe(false);
#endif

    // Preserve the final BLE receive-stage marker before the freshly booted
    // TaskHardware loop can overwrite it.  This only snapshots crash resets;
    // ordinary boots retain the most recent crash evidence for USB diagnosis.
    BLE_MGR.latchRxCrashDiag();

    // Apply the compile-time debug profile before any DLOG_* call so disabled
    // logs cost nothing from boot onward.
    DebugLog::applyProfile(static_cast<DebugProfile>(SPECTRE_DEBUG_PROFILE),
                           kDebugSubsystemMask);

    // Recovery input takes priority over an RTC-retained field-offload resume.
    // Normal boots consume the one-time AP material before the regular
    // UI/storage/radio allocation path; a held recovery button discards it so
    // a bad early Wi-Fi transition cannot survive every reset.
    g_bootRecoveryMode = _detectBootRecoveryRequest();
    if (g_bootRecoveryMode) {
        PHONE_OFFLOAD.discardRetainedWifiBulkResume();
    } else {
        PHONE_OFFLOAD.resumeWifiBulkEarly();
    }

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

    if (g_bootRecoveryMode) {
        PrebootFallback::showFatal(tft, "RECOVERY MODE", "USB FLASH READY");
    }

    // ── Boot diagnostics — runs before any manager initializes ───────────────
    if (!g_bootRecoveryMode) {
        const esp_reset_reason_t rr = esp_reset_reason();
#if BOOT_SEQUENCE_VERBOSE_ACTIVE
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

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        if (rr == ESP_RST_CPU_LOCKUP) {
            Serial.printf("[BOOT] *** CPU LOCKUP — double exception, no panic handler ran ***\n");
            DLOG_WARN("CORE", "cpu lockup reset — fault inside the fault handler");
        }

        if (rr == ESP_RST_PWR_GLITCH) {
            Serial.printf("[BOOT] *** POWER GLITCH — supply transient detected ***\n");
            DLOG_WARN("CORE", "power glitch reset — check battery contacts and rail decoupling");
        }
#endif

        // RTC crash ring — last CRASH_LOG_DEPTH checkpoints survive across resets.
        // Entries persist until overwritten; connect any time after a crash.
        crashLogPrint();
#else
        if (_resetReasonIsCrashLike(rr)) {
            DLOG_WARN("CORE", "reset_reason=%d (%s)", (int)rr, _resetReasonName(rr));
        }
#endif

        // A failed allocation from the previous boot names the request that
        // preceded the panic, which the ring alone cannot identify. This is the
        // only code that disarms the record, so it must run at every verbosity
        // or a stale failure is reported for the life of the device. It is
        // silent unless a record is actually waiting.
        crashAllocFailPrint();
    }
    // ── end boot diagnostics ─────────────────────────────────────────────────

    // Boot/run bookkeeping for the Boot Summary screen: bumps the boot count,
    // latches this reset's reason, and loads the previous run's snapshot.
    BootInfo::begin();

    if (!g_bootRecoveryMode) {
        const bool settingsOk = SETTINGS.begin();
        if (!settingsOk) {
            DLOG_WARN("SETTINGS",
                      "Settings unavailable in setup; using fallback USB serial policy");
        }
        DLOG_INFO("SYS", "Booting");
    } else {
        DebugLog::configureUsbSerial(true, DEBUG_LEVEL_INFO, DEBUG_AREA_OPERATORS);
        DLOG_INFO("SYS", "Booting into recovery mode");
    }

    _initCoreLoadMonitor();

    DLOG_INFO("SYS", "Starting tasks");

    s_buttonEventQueue = xQueueCreateStatic(
        BUTTON_EVENT_QUEUE_DEPTH,
        sizeof(ButtonEvent),
        s_buttonEventQueueBuffer,
        &s_buttonEventQueueStorage);
    if (!s_buttonEventQueue) {
        DLOG_ERROR("BTN", "Failed to create button event queue");
    } else {
        const BaseType_t buttonTaskCreated = xTaskCreatePinnedToCore(
            TaskButtons, "TaskButtons",
            TASK_BUTTON_STACK_BYTES, nullptr, 3,
            &taskButtonHandle, 1);
        if (buttonTaskCreated != pdPASS) {
            DLOG_ERROR("BTN", "Failed to create button input task");
            s_buttonEventQueue = nullptr;
        }
    }

    xTaskCreatePinnedToCore(
        TaskDisplay, "TaskDisplay",
        TASK_DISPLAY_STACK_BYTES, nullptr, 2,
        &taskDisplayHandle, 1);

    xTaskCreatePinnedToCore(
        TaskHardware, "TaskHardware",
        TASK_HARDWARE_STACK_BYTES, nullptr, 2,
        &taskHardwareHandle, 0);

    DLOG_INFO("SYS", "Tasks launched");
    DLOG_INFO("STACK", "loopTask setup watermark=%luB",
              static_cast<unsigned long>(
                  uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
}

void loop() {
    // Arduino owns the lifecycle of loopTask. Deleting it reclaimed 8 KB, but
    // caused a repeatable delayed reset after BLE authentication on this core.
    // Keep the framework task parked; the dedicated Spectre tasks do the work.
    vTaskDelay(portMAX_DELAY);
}
