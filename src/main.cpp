
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <math.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>
#include <esp_freertos_hooks.h>

#include "config.h"
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
#include "managers/BLEManager.h"
#include "managers/RadioArbiter.h"
#include "managers/SettingsManager.h"
#include "managers/TimeService.h"
#include "core/DebugLog.h"
#include "ui/BootSequence.h"
#include "ui/LVGLDriver.h"
#include "ui/Theme.h"
#include "ui/PrebootFallback.h"

// ─── Hardware objects ─────────────────────────────────────────
TFT_eSPI        tft = TFT_eSPI();
ButtonHandler   buttons;
DisplayManager  display;
LoRaManager     lora;
ReyaxBackend    subghzReyax(lora);

namespace {
    constexpr uint32_t DISPLAY_FRAME_INTERVAL_MS = 50;
    constexpr uint32_t DISPLAY_MASCOT_INTERVAL_MS = 140;
    constexpr uint32_t PWNY_SCREEN_REFRESH_MS = 750;

    constexpr size_t USB_CONSOLE_BUF_SIZE = 128;
    char g_usbConsoleBuf[USB_CONSOLE_BUF_SIZE] = {};
    size_t g_usbConsoleLen = 0;

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

// ─── Task handles ─────────────────────────────────────────────
TaskHandle_t taskDisplayHandle  = nullptr;
TaskHandle_t taskHardwareHandle = nullptr;

static constexpr uint32_t TASK_DISPLAY_STACK_BYTES  = 24576;
static constexpr uint32_t TASK_HARDWARE_STACK_BYTES = 32768;
static constexpr uint32_t STACK_LOG_INTERVAL_MS     = 30000UL;
static constexpr uint32_t HEALTH_LOG_INTERVAL_MS    = 30000UL;
static constexpr uint32_t HEAP_CHECK_INTERVAL_MS    = 120000UL;
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

// ─── Forward declarations ─────────────────────────────────────
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
                                       size_t count);

enum PhoneProbeReason : uint8_t {
    PHONE_PROBE_BOOT = 0,
    PHONE_PROBE_BACKLOG,
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

    bool bootProbeDone = !PHONE_COMPANION_BOOT_PROBE;
    uint8_t bootRetryIndex = 0;

    uint32_t nextProbeMs = 0;
    uint32_t lastSeenMs = 0;
    uint32_t lastProbeMs = 0;
    uint32_t lastEnrichMs = 0;
    uint32_t lastHighValueWifiMs = 0;

    uint32_t pendingItems = 0;
    bool manualEnrichRequested = false;
    bool offloadPrepRequested = false;
    bool timeSyncRequested = false;
    bool enrichmentRequestIssued = false;
    size_t lastRequestedEnrichmentCount = 0;
};

static void _finishPhoneEnrichment(CompanionScheduler& cs, bool success);

static constexpr uint32_t PHONE_BOOT_RETRY_SCHEDULE_MS[] = {
    8000UL,
    120000UL,
    300000UL,
    600000UL,
    1800000UL
};

static constexpr uint32_t WIFI_LULL_MIN_MS = 30000UL;
static constexpr uint32_t PHONE_PROBE_MIN_GAP_MS = 120000UL;
static constexpr uint32_t ENRICH_PENDING_THRESHOLD = 25UL;
static constexpr uint32_t ENRICH_MIN_GAP_MS = 60000UL;
static constexpr size_t PHONE_ENRICH_BATCH_MAX = 24;

static const char* phoneProbeReasonName(PhoneProbeReason reason) {
    switch (reason) {
        case PHONE_PROBE_BOOT:         return "boot_probe";
        case PHONE_PROBE_BACKLOG:      return "backlog_probe";
        case PHONE_PROBE_OFFLOAD_PREP: return "offload_prep";
        case PHONE_PROBE_MANUAL:       return "manual_enrich";
        case PHONE_PROBE_TIME_SYNC:    return "time_sync";
        default:                       return "probe";
    }
}

static bool isHighValueWiFiOwner(RadioOwner owner) {
    return owner == RADIO_WIFI_CAPTURE ||
           owner == RADIO_WIFI_SCAN ||
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
    if (millis() - cs.lastProbeMs < PHONE_PROBE_MIN_GAP_MS) {
    return false;
    } 
    if (millis() < cs.nextProbeMs) {
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
    if (millis() - cs.lastEnrichMs < ENRICH_MIN_GAP_MS) {
        return false;
    }

    if (cs.manualEnrichRequested ||
        cs.offloadPrepRequested ||
        cs.timeSyncRequested) {
        return true;
    }

    return cs.pendingItems >= ENRICH_PENDING_THRESHOLD;
}

static bool requestPhoneProbeLease(CompanionScheduler& cs,
                                   PhoneProbeReason reason) {
    if (!RADIO_ARB.requestLease(
            RADIO_BLE_GPS,
            RadioArbiter::BLE_PHONE_PROBE_HOLD_MS,
            phoneProbeReasonName(reason))) {
        return false;
    }

    cs.workState = COMPANION_WORK_PROBING;
    cs.lastProbeMs = millis();
    DLOG_INFO("BLE", "Phone probe lease granted reason=%s",
              phoneProbeReasonName(reason));
    return true;
}

static bool requestPhoneEnrichmentLease(CompanionScheduler& cs,
                                        const char* reason) {
    if (!RADIO_ARB.requestLease(
            RADIO_BLE_GPS,
            RadioArbiter::BLE_PHONE_ENRICH_HOLD_MS,
            reason)) {
        return false;
    }

    cs.workState = COMPANION_WORK_ENRICHING;
    DLOG_INFO("BLE", "Phone enrichment lease granted reason=%s",
              reason ? reason : "enrich");
    return true;
}

static bool _buildPendingEnrichmentBatch(EventBatchRecord* out,
                                         size_t maxCount,
                                         size_t& outCount) {
    outCount = 0;

    if (!out || maxCount == 0) {
        return false;
    }

    const String sessionId = SESS.getId();
    if (!sessionId.length()) {
        return false;
    }

    std::vector<PendingEventDescriptor> pending(maxCount);
    if (!STORAGE.getPendingEnrichmentBatchForSession(sessionId.c_str(),
                                                     pending.data(),
                                                     maxCount,
                                                     outCount)) {
        DLOG_WARN("BLE", "Failed to read spool enrichment batch for session");
        return false;
    }

    for (size_t i = 0; i < outCount; ++i) {
        out[i].eventId = pending[i].eventId;
        out[i].timestampMs = pending[i].timestampMs;
        out[i].type = pending[i].type;
        out[i].status = pending[i].status;
    }

    return true;
}

static bool _applyPhoneEnrichmentBatch(const PendingEnrichment* records,
                                       size_t count) {
    if (!records || count == 0) {
        return false;
    }

    bool anySuccess = false;
    uint32_t applied = 0;
    uint32_t failed = 0;

    for (size_t i = 0; i < count; ++i) {
        const PendingEnrichment& r = records[i];
        if (r.eventId == 0) {
            continue;
        }

        if (STORAGE.enrichEvent(r.eventId,
                                r.lat,
                                r.lon,
                                r.alt,
                                r.accuracy,
                                r.tag)) {
            applied++;
            anySuccess = true;
        } else {
            failed++;
            DLOG_WARN("BLE",
                      "Enrichment write failed event=%lu",
                      static_cast<unsigned long>(r.eventId));
        }
    }

    DLOG_INFO("BLE",
              "Enrichment batch applied=%lu failed=%lu",
              static_cast<unsigned long>(applied),
              static_cast<unsigned long>(failed));

    return anySuccess;
}

static void _finishPhoneEnrichment(CompanionScheduler& cs, bool success) {
    cs.workState = COMPANION_WORK_IDLE;
    cs.lastEnrichMs = millis();
    cs.enrichmentRequestIssued = false;
    cs.lastRequestedEnrichmentCount = 0;

    if (success) {
        cs.phoneState = COMPANION_PHONE_AVAILABLE;
        cs.lastSeenMs = millis();
        cs.manualEnrichRequested = false;
        cs.offloadPrepRequested = false;
        cs.timeSyncRequested = false;
        DLOG_INFO("BLE", "Phone enrichment finished successfully");
    } else {
        cs.phoneState = COMPANION_PHONE_UNAVAILABLE;
        DLOG_WARN("BLE", "Phone enrichment failed");
    }

    if (RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
        RADIO_ARB.release(RADIO_BLE_GPS,
                          success ? "enrich_done" : "enrich_fail");
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
                if (tokenMask == DEBUG_AREA_ALL ||
                    tokenMask == DEBUG_AREA_OPERATORS) {
                    if (tokenMode == FOCUS_REMOVE) {
                        if (tokenMask == DEBUG_AREA_ALL) {
                            mask = 0;
                        } else {
                            mask &= ~tokenMask;
                        }
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

    Serial.printf("[USB] unknown command: %s\r\n", line.c_str());
    _printUsbConsoleHelp();
}

void _pollUsbSerialConsole() {
    while (Serial.available() > 0) {
        const char ch = static_cast<char>(Serial.read());
        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            g_usbConsoleBuf[g_usbConsoleLen] = '\0';
            if (g_usbConsoleLen > 0) {
                _handleUsbConsoleLine(g_usbConsoleBuf);
            }
            g_usbConsoleLen = 0;
            continue;
        }

        if ((g_usbConsoleLen + 1) >= USB_CONSOLE_BUF_SIZE) {
            g_usbConsoleLen = 0;
            Serial.println("[USB] command too long");
            continue;
        }

        g_usbConsoleBuf[g_usbConsoleLen++] = ch;
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

void _requestSleepTransition(bool storageOk,
                             bool finalizeSession,
                             bool exportSession,
                             const char* reason) {
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
    g_state.requestSleep = true;
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
        bool wifiConnected = false;
        bool bleConnected = false;
        bool gpsValid = false;
        int wifiCount = 0;
        int pendingFiles = 0;
        uint8_t radioOwner = 0;
        bool timeValid = false;
        unsigned long uptimeMs = 0;
        char timeLocal[24] = "";
        char timeSource[12] = "";
        STATE_READ_BEGIN();
        wifiConnected = g_state.wifiConnected;
        bleConnected = g_state.bleConnected;
        gpsValid = g_state.gpsValid;
        wifiCount = g_state.wifiNetworkCount;
        pendingFiles = g_state.sessionFilesPending;
        radioOwner = g_state.radioOwner;
        timeValid = g_state.timeValid;
        uptimeMs = g_state.uptimeMs;
        strlcpy(timeLocal, g_state.timeLocal, sizeof(timeLocal));
        strlcpy(timeSource, g_state.timeSource, sizeof(timeSource));
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
          "heap used=%lu/%luKB free=%luKB min=%luKB largest=%luKB frag=%lu%% psram used=%lu/%luKB free=%luKB largest=%luKB core=%u/%u%% owner=%s wifi=%d ble=%d gps=%d nets=%d pending=%d",
          static_cast<unsigned long>(kb(usedHeap)),
          static_cast<unsigned long>(kb(totalHeap)),
          static_cast<unsigned long>(kb(freeHeap)),
          static_cast<unsigned long>(kb(minHeap)),
          static_cast<unsigned long>(kb(largestHeap)),
          static_cast<unsigned long>(heapFragPct),
          static_cast<unsigned long>(kb(usedPsram)),
          static_cast<unsigned long>(kb(totalPsram)),
          static_cast<unsigned long>(kb(freePsram)),
          static_cast<unsigned long>(kb(largestPsram)),
          static_cast<unsigned>(g_coreLoad.busyPct[0]),
          static_cast<unsigned>(g_coreLoad.busyPct[1]),
          RadioArbiter::ownerName(static_cast<RadioOwner>(radioOwner)),
          wifiConnected ? 1 : 0,
          bleConnected ? 1 : 0,
          gpsValid ? 1 : 0,
          wifiCount,
          pendingFiles);
        if (timeValid && timeLocal[0]) {
            Serial.printf("[HEALTH] time=%s src=%s\r\n", timeLocal, timeSource);
        } else {
            const unsigned long s = uptimeMs / 1000UL;
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
        Serial.printf("[HEALTH] radio=%s wifi=%d ble=%d gps=%d nets=%d pending=%d\r\n",
                      RadioArbiter::ownerName(static_cast<RadioOwner>(radioOwner)),
                      wifiConnected ? 1 : 0,
                      bleConnected ? 1 : 0,
                      gpsValid ? 1 : 0,
                      wifiCount,
                      pendingFiles);
                DLOG_DEBUG("SUBGHZ", "heartbeat");

        STATE_READ_BEGIN();
        char sgBackend[24];
        strlcpy(sgBackend, g_state.subGhzBackend, sizeof(sgBackend));
        const uint8_t sgMode = g_state.subGhzMode;
        const int sgPkts = g_state.loraPacketCount;
        char subGhzBackend[24];
        strlcpy(subGhzBackend, g_state.subGhzBackend, sizeof(subGhzBackend));
        const uint8_t subGhzMode = g_state.subGhzMode;
        STATE_READ_END();

        STATE_READ_BEGIN();
        const int sgNodes = g_state.subGhzNodeCount;
        STATE_READ_END();

        Serial.printf("[HEALTH] subghz=%s mode=%s pkts=%d nodes=%d\r\n",
                      sgBackend[0] ? sgBackend : "NONE",
                      _subGhzModeShort(sgMode),
                      sgPkts,
                      sgNodes);
            
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
    }

    if (nowMs - lastHeapCheckMs >= HEAP_CHECK_INTERVAL_MS) {
        const bool heapOk = heap_caps_check_integrity_all(false);
        if (!heapOk) {
            DLOG_ERROR("HEAP", "heap integrity check failed");
            Serial.println("[HEAP] integrity check failed");
        } else {
            DLOG_DEBUG("HEAP", "heap integrity check ok");
            Serial.println("[HEAP] integrity check ok");
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

static void _syncDisplayFromSnapshot(const DisplayFrameState& snapshot);

// ─── Core 1: Display Task ─────────────────────────────────────

void TaskDisplay(void* pvParameters) {
    DLOG_INFO("CORE", "Display task started");
    uint32_t lastStackLogMs = millis();
    UBaseType_t minStackWords = uxTaskGetStackHighWaterMark(nullptr);
    DLOG_INFO("STACK", "TaskDisplay free=%luB",
              (unsigned long)(minStackWords * sizeof(StackType_t)));

    PrebootFallback::showInit(tft, "SPECTRE", "BRINGING UP LVGL");

    Serial.println("[DISPLAY] Building LVGL layer");
    LVGLDriver::begin(&tft);
    Serial.println("[DISPLAY] LVGLDriver begin complete");
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

    runBootSequence(display, loraOk, storageOk);

    Serial.println("[DISPLAY] before display.begin");
    display.begin();
    Serial.println("[DISPLAY] after display.begin");
    lv_refr_now(NULL);
    Serial.println("[DISPLAY] Main UI ready");

    STATE_WRITE_BEGIN();
    g_state.screenChanged = true;
    STATE_WRITE_END();

    int      animFrame   = 0;
    uint32_t lastFrameMs = 0;
    bool     statusValid = false;
    ExecutionPolicy::StatusBarStateView lastStatus = {};

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
        while (BUS.receive(queuedEvent, 0)) {
            _handleDisplayEvent(queuedEvent, snapshot);
        }

        // Sleep request
        if (snapshot.requestSleep) {
            lv_obj_t* scr = lv_screen_active();

            lv_obj_t* overlay = lv_obj_create(scr);
            lv_obj_set_size(overlay, THEME_SCREEN_W, THEME_SCREEN_H);
            lv_obj_set_pos(overlay, 0, 0);
            lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(overlay, 0, 0);
            lv_obj_set_style_radius(overlay, 0, 0);
            lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* label = lv_label_create(overlay);
            lv_label_set_text(label, "POWERING DOWN...");
            lv_obj_set_style_text_color(label, lv_color_hex(0xFF4D4D), 0);
            lv_obj_set_style_text_font(label, FONT_HEADER, 0);
            lv_obj_center(label);

            lv_refr_now(NULL);
            vTaskDelay(pdMS_TO_TICKS(1200));
            esp_deep_sleep_start();
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

        if (snapshot.screenChanged || snapshot.loraNewPacket || snapshot.dataRefresh) {
            _syncDisplayFromSnapshot(snapshot);
            if (snapshot.loraNewPacket) {
                display.triggerDataPulse();
            }
        }

        display.tickNotif();

        // Animate mascot in left panel
        lv_timer_handler();
        static uint32_t lastMascotMs = 0;
        if (millis() - lastMascotMs > _displayMascotIntervalForPowerState(snapshot.powerState)) {
            display.drawMascotFrame(snapshot.mascotState, animFrame);
            lastMascotMs = millis();
        }

        if (now - lastStackLogMs >= STACK_LOG_INTERVAL_MS) {
            const UBaseType_t freeWords = uxTaskGetStackHighWaterMark(nullptr);
            if (freeWords < minStackWords) {
                minStackWords = freeWords;
            }
            DLOG_INFO("STACK", "TaskDisplay free=%luB min=%luB",
                      (unsigned long)(freeWords * sizeof(StackType_t)),
                      (unsigned long)(minStackWords * sizeof(StackType_t)));
            lastStackLogMs = now;
        }

        vTaskDelay(1);
    }
}

static void _syncDisplayFromSnapshot(const DisplayFrameState& snapshot);

static void _syncDisplayFromSnapshot(const DisplayFrameState& s) {
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

    display.setActionHints(_displayBindingsForSnapshot(s));
    display.updateDivider();
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

void _runSessionExport(bool notifyUser) {
    SessionExportSummary summary;
    const bool ok = EXPORT_MGR.exportCurrentSession(&summary);

    if (!summary.generatedIso[0]) {
        if (!TIME_SVC.formatNowIso(summary.generatedIso, sizeof(summary.generatedIso))) {
            summary.generatedIso[0] = '\0';
        }
    }

    _applyExportSummaryToState(summary, ok);

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

// ─── Core 0: Hardware Task ────────────────────────────────────

void TaskHardware(void* pvParameters) {
    DLOG_INFO("CORE", "Hardware task started");
    uint32_t lastStackLogMs = millis();
    UBaseType_t minStackWords = uxTaskGetStackHighWaterMark(nullptr);
    DLOG_INFO("STACK", "TaskHardware free=%luB",
              (unsigned long)(minStackWords * sizeof(StackType_t)));
    uint32_t lastWifiTick = 0;
    CompanionScheduler companion = {};
    companion.enabled = PHONE_COMPANION_ENABLED;
    companion.nextProbeMs = PHONE_COMPANION_ENABLED
        ? PHONE_BOOT_RETRY_SCHEDULE_MS[0]
        : 0;
    bool lastTextPending = false;
    ExecutionPolicy::UiRefreshMarks uiRefreshMarks = {};

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

    // Init storage
    bool storageOk = STORAGE.begin();
    String storageUsed = storageOk ? STORAGE.getCachedUsedString() : String();
    _publishStorageState(storageOk, storageUsed);

    if (storageOk) {
        DebugLog::begin();
        _loadKnownLocationsIntoState();
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
        const uint32_t loopNow = millis();

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

        if (!sleepAlreadyRequested &&
            power.state == POWER_STATE_BATTERY_CRITICAL &&
            power.criticalSleepAtMs != 0 &&
            loopNow >= power.criticalSleepAtMs) {
            _requestSleepTransition(storageOk, false, false, "critical_timeout");
        }

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

        // Buttons
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

        // PMKID hunt request from UI
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

        // Update uptime
        STATE_WRITE_BEGIN();
        g_state.uptimeMs = millis();
        STATE_WRITE_END();

        TIME_SVC.tick();

        // WireGuard dump trigger from BLE
        if (BLE_MGR.consumeWireGuardDumpTrigger()) {
            MQTT_MGR.requestDump(true);
            DLOG_INFO("BLE", "WireGuard dump triggered");
        }

        // Upload demand triggers
        static uint32_t lastUploadCheck = 0;
        if (millis() - lastUploadCheck > 60000) {
            lastUploadCheck = millis();

            // Threshold trigger
            if (MQTT_MGR.uploadReadyCount() >= MQTT_UPLOAD_READY_THRESHOLD) {
                MQTT_MGR.requestDump(false);
            }

            // Time-based fallback — 2hr maximum gap
            if (MQTT_MGR.lastDumpAge() > MQTT_DUMP_INTERVAL_MS &&
                MQTT_MGR.queueDepth() > 0) {
                MQTT_MGR.requestDump(false);
            }
        }

        // BLE text input routing
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

        // Location auto-tag on fresh GPS fixes
        uint32_t gpsFix = 0;
        STATE_READ_BEGIN();
        gpsFix = g_state.gpsLastFix;
        STATE_READ_END();
        if (gpsFix != 0 && gpsFix != lastGpsFixSeen) {
            lastGpsFixSeen = gpsFix;
            _checkLocationTag();
        }

        if (!companion.enabled) {
            companion.workState = COMPANION_WORK_IDLE;
            companion.enrichmentRequestIssued = false;
            companion.manualEnrichRequested = false;
            companion.offloadPrepRequested = false;
            companion.timeSyncRequested = false;
        } else {
            // Track recent high-value WiFi activity.
            // Keep this simple for now: if WiFi owns the radio, treat that as active.
            if (isHighValueWiFiOwner(RADIO_ARB.currentOwner())) {
                companion.lastHighValueWifiMs = millis();
            }

            // Refresh pending enrichment backlog.
            // Conservative for now: use session pending count until enrich-specific
            // accounting is wired in.
            companion.pendingItems = STORAGE.getSessionPendingEventCount();

            // Boot retry schedule: 8s, 2m, 5m, 10m, 30m, then stop automatic probing.
            if (!companion.bootProbeDone &&
                companion.bootRetryIndex <
                    (sizeof(PHONE_BOOT_RETRY_SCHEDULE_MS) /
                     sizeof(PHONE_BOOT_RETRY_SCHEDULE_MS[0])) &&
                millis() >= PHONE_BOOT_RETRY_SCHEDULE_MS[companion.bootRetryIndex]) {

                companion.nextProbeMs = millis();

                if (shouldRunPhoneProbe(companion)) {
                    if (requestPhoneProbeLease(companion, PHONE_PROBE_BOOT)) {
                        companion.bootRetryIndex++;
                        if (companion.bootRetryIndex >=
                            (sizeof(PHONE_BOOT_RETRY_SCHEDULE_MS) /
                             sizeof(PHONE_BOOT_RETRY_SCHEDULE_MS[0]))) {
                            companion.bootProbeDone = true;
                        }
                    }
                }
            }

            // Opportunistic probe if work exists but phone is not known available yet.
            if (companion.phoneState != COMPANION_PHONE_AVAILABLE &&
                companion.workState == COMPANION_WORK_IDLE &&
                (companion.pendingItems >= ENRICH_PENDING_THRESHOLD ||
                 companion.manualEnrichRequested ||
                 companion.offloadPrepRequested ||
                 companion.timeSyncRequested)) {

                companion.nextProbeMs = millis();

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
            // BLE companion probe/enrichment completion handling
            if (companion.workState == COMPANION_WORK_PROBING) {
                if (BLE_MGR.isPhoneCompanionReady()) {
                    companion.phoneState = COMPANION_PHONE_AVAILABLE;
                    companion.lastSeenMs = millis();
                    companion.workState = COMPANION_WORK_IDLE;
                    companion.nextProbeMs = 0;
                    companion.bootProbeDone = true;
                    DLOG_INFO("BLE", "Phone probe succeeded");

                    if (RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
                        RADIO_ARB.release(RADIO_BLE_GPS, "probe_success");
                    }
                } else if (!RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
                    companion.phoneState = COMPANION_PHONE_UNAVAILABLE;
                    companion.workState = COMPANION_WORK_IDLE;
                    DLOG_WARN("BLE", "Phone probe failed");
                }
            }

            if (companion.workState == COMPANION_WORK_ENRICHING) {
                if (BLE_MGR.isPhoneCompanionReady()) {
                    companion.phoneState = COMPANION_PHONE_AVAILABLE;
                    companion.lastSeenMs = millis();

                    if (!companion.enrichmentRequestIssued) {
                        EventBatchRecord batch[PHONE_ENRICH_BATCH_MAX] = {};
                        size_t batchCount = 0;

                        if (!_buildPendingEnrichmentBatch(batch,
                                                          PHONE_ENRICH_BATCH_MAX,
                                                          batchCount)) {
                            DLOG_WARN("BLE", "Failed to build enrichment batch");
                            _finishPhoneEnrichment(companion, false);
                        } else if (batchCount == 0) {
                            DLOG_INFO("BLE", "No pending enrichment records");
                            _finishPhoneEnrichment(companion, true);
                        } else if (BLE_MGR.requestEnrichmentBatch(batch, batchCount)) {
                            companion.enrichmentRequestIssued = true;
                            companion.lastRequestedEnrichmentCount = batchCount;
                            DLOG_INFO("BLE",
                                      "Requested enrichment batch count=%u",
                                      static_cast<unsigned>(batchCount));
                        } else {
                            DLOG_WARN("BLE", "BLE requestEnrichmentBatch failed");
                            _finishPhoneEnrichment(companion, false);
                        }
                    }

                    if (companion.enrichmentRequestIssued) {
                        PendingEnrichment enrichments[PHONE_ENRICH_BATCH_MAX] = {};
                        size_t outCount = 0;

                        if (BLE_MGR.consumeEnrichmentBatch(enrichments,
                                                           PHONE_ENRICH_BATCH_MAX,
                                                           outCount)) {
                            const bool ok = _applyPhoneEnrichmentBatch(enrichments,
                                                                       outCount);
                            _finishPhoneEnrichment(companion, ok);
                        }
                    }
                } else if (!RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
                    companion.phoneState = COMPANION_PHONE_UNAVAILABLE;
                    companion.workState = COMPANION_WORK_IDLE;
                    companion.enrichmentRequestIssued = false;
                    companion.lastRequestedEnrichmentCount = 0;
                    DLOG_WARN("BLE", "Enrichment lease ended without ready companion");
                }
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

        // MQTT tick — only when upload lease active
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
        _checkRuntimeContracts();

        if (now - lastStackLogMs >= STACK_LOG_INTERVAL_MS) {
            const UBaseType_t freeWords = uxTaskGetStackHighWaterMark(nullptr);
            if (freeWords < minStackWords) {
                minStackWords = freeWords;
            }
            DLOG_INFO("STACK", "TaskHardware free=%luB min=%luB",
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

// ─── Setup ────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    const bool settingsOk = SETTINGS.begin();
    if (!settingsOk) {
        DLOG_WARN("SETTINGS",
                  "Settings unavailable in setup; using fallback USB serial policy");
    }
    DLOG_INFO("SYS", "Booting");

    // Display power
    pinMode(LCD_POWER, OUTPUT);
    digitalWrite(LCD_POWER, HIGH);
    delay(100);
    pinMode(LCD_BL, OUTPUT);
    analogWriteResolution(LCD_BL, 8);
    analogWrite(LCD_BL, 255);
    _markUiActivity();

    // TFT init — done before tasks start
    tft.init();
    tft.setRotation(3);
    tft.fillScreen(0x0000);

    // Button init
    buttons.begin();

    _initCoreLoadMonitor();

    DLOG_INFO("SYS", "Starting tasks");

    // Display task — Core 1, high priority
    xTaskCreatePinnedToCore(
        TaskDisplay, "TaskDisplay",
        TASK_DISPLAY_STACK_BYTES, nullptr, 2,
        &taskDisplayHandle, 1);

    // Hardware task — Core 0, high priority
    xTaskCreatePinnedToCore(
        TaskHardware, "TaskHardware",
        TASK_HARDWARE_STACK_BYTES, nullptr, 2,
        &taskHardwareHandle, 0);

    DLOG_INFO("SYS", "Tasks launched");
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}

                                                                                                                                                                                                                                                                                                                                               