

#include "MQTTManager.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include <algorithm>
#include <cstdlib>
#include <vector>
#include "../config.h"
#include "../data/Schema.h"
#include "../data/FieldVault.h"
#include "../core/EventBus.h"
#include "../core/NotifTypes.h"
#include "../core/DebugLog.h"
#include "../core/RuntimeContracts.h"
#include "../core/CrashBreadcrumb.h"
#include "RAMSpool.h"
#include "RadioArbiter.h"
#include "SettingsManager.h"
#include "StorageManager.h"
#include "TimeService.h"

// Task handle owned by main.cpp. We need it so the upload path can suspend
// TaskDisplay for the duration of the upload mission and dodge the OPI-PSRAM
// flash/PSRAM cache-share race.
extern TaskHandle_t taskDisplayHandle;

void MQTTManager::_pauseDisplayForUpload() {
    if (_displayPausedForUpload || !taskDisplayHandle) return;
    vTaskSuspend(taskDisplayHandle);
    _displayPausedForUpload = true;
    DLOG_INFO("MQTT", "TaskDisplay suspended for upload mission");
}

void MQTTManager::_resumeDisplayAfterUpload() {
    if (!_displayPausedForUpload || !taskDisplayHandle) return;
    vTaskResume(taskDisplayHandle);
    _displayPausedForUpload = false;
    DLOG_INFO("MQTT", "TaskDisplay resumed after upload mission");
}

// Upload fetch/publish runs during an active WiFi lease. DEBUG profile is useful
// elsewhere, but these hot-path traces can destabilize the rail, so leave them
// compile-time dark unless deliberately instrumenting this path.
#define DLOG_UPLOAD_TRACE(tag, fmt, ...) \
    do { if (false) DLOG_DEBUG(tag, fmt, ##__VA_ARGS__); } while (0)

MQTTManager MQTT_MGR;

// ── Legacy queue directory on LittleFS ────────────────────────
#define LEGACY_QUEUE_DIR "/mqtt_queue"
static constexpr const char* LEGACY_QUEUE_MIGRATION_MARKER =
    PATH_MQTT_LEGACY_MIGRATED_FLAG;

namespace {

const RuntimeSettings* _settingsView() {
    return SETTINGS.isReady() ? &SETTINGS.get() : nullptr;
}

const char* _configuredBrokerHost() {
    const RuntimeSettings* settings = _settingsView();
    if (settings && settings->mqttBroker[0]) {
        return settings->mqttBroker;
    }
    return "";
}

uint16_t _configuredBrokerPort() {
    const RuntimeSettings* settings = _settingsView();
    if (settings && settings->mqttPort > 0) {
        return settings->mqttPort;
    }
    return 1883;
}

const char* _configuredBrokerUser() {
    const RuntimeSettings* settings = _settingsView();
    if (settings && settings->mqttUser[0]) {
        return settings->mqttUser;
    }
    return "";
}

const char* _configuredBrokerPassword() {
    const RuntimeSettings* settings = _settingsView();
    if (settings && settings->mqttPassword[0]) {
        return settings->mqttPassword;
    }
    return "";
}

const WiFiCredential* _primaryUploadNetwork() {
    const RuntimeSettings* settings = _settingsView();
    if (settings && settings->wifiNetworkCount > 0 &&
        settings->wifiNetworks[0].ssid[0]) {
        return &settings->wifiNetworks[0];
    }

    static WiFiCredential fallback{};
    if (SPECTRE_WIFI_1_SSID[0]) {
        strlcpy(fallback.ssid, SPECTRE_WIFI_1_SSID, sizeof(fallback.ssid));
        strlcpy(fallback.password, SPECTRE_WIFI_1_PASSWORD, sizeof(fallback.password));
        return &fallback;
    }

    return nullptr;
}

String _mqttTopicFor(const char* suffix) {
    const RuntimeSettings* settings = _settingsView();
    const char* topicBase =
        (settings && settings->mqttTopicBase[0]) ?
            settings->mqttTopicBase : SPECTRE_MQTT_TOPIC_BASE;

    String topic(topicBase);
    topic += "/";
    topic += MQTT_SENSOR_ID;
    topic += "/";
    topic += suffix;
    return topic;
}

String _mqttTopicForEventType(const char* type) {
    if (!type || !type[0]) return _mqttTopicFor("event");

    if (strcmp(type, "subghz") == 0) return _mqttTopicFor("subghz");
    if (strcmp(type, "probe") == 0)  return _mqttTopicFor("probe");
    if (strcmp(type, "device") == 0) return _mqttTopicFor("device");
    if (strcmp(type, "network") == 0) return _mqttTopicFor("network");
    if (strcmp(type, "drone") == 0)  return _mqttTopicFor("drone");
    if (strcmp(type, "pmkid") == 0)  return _mqttTopicFor("pmkid");
    return _mqttTopicFor("event");
}

const char* _legacyEventTypeFromQueueName(const String& name) {
    if (name.startsWith("subghz_")) return "subghz";
    if (name.startsWith("probe_"))  return "probe";
    if (name.startsWith("device_")) return "device";
    if (name.startsWith("network_")) return "network";
    if (name.startsWith("drone_"))  return "drone";
    if (name.startsWith("pmkid_"))  return "pmkid";
    if (name.startsWith("event_"))  return "event";
    return nullptr;
}

constexpr uint32_t kUploadFetchSlowInfoMs = 250;
constexpr uint32_t kUploadFetchSlowWarnMs = 500;
constexpr uint32_t kUploadStackWatermarkDropWarnBytes = 1024;

uint32_t _currentTaskStackWatermarkBytes() {
    return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr) *
                                 sizeof(StackType_t));
}

uint32_t _extractLegacyEventIdFromQueueName(const String& name) {
    const int firstUnderscore = name.indexOf('_');
    if (firstUnderscore < 0) {
        return 0;
    }

    const int secondUnderscore = name.indexOf('_', firstUnderscore + 1);
    if (secondUnderscore < 0) {
        return 0;
    }

    const String eventIdText = name.substring(firstUnderscore + 1,
                                              secondUnderscore);
    if (!eventIdText.length()) {
        return 0;
    }

    return static_cast<uint32_t>(strtoul(eventIdText.c_str(), nullptr, 10));
}

bool _copyEventRecordForPublish(JsonObjectConst record, JsonDocument& out) {
    out.clear();
    JsonObject publishDoc = out.to<JsonObject>();
    if (publishDoc.isNull()) {
        return false;
    }

    for (JsonPairConst kv : record) {
        const char* key = kv.key().c_str();
        if (strcmp(key, "id") == 0 ||
            strcmp(key, "type") == 0 ||
            strcmp(key, "status") == 0 ||
            strcmp(key, F_TIMESTAMP) == 0 ||
            strcmp(key, F_TIMESTAMP_ISO) == 0 ||
            strcmp(key, "uploaded_ts") == 0 ||
            strcmp(key, F_UPLOADED_TS_ISO) == 0 ||
            strcmp(key, "enriched_ts") == 0 ||
            strcmp(key, F_ENRICHED_TS_ISO) == 0) {
            continue;
        }
        publishDoc[key].set(kv.value());
    }

    const char* isoTs = record[F_TIMESTAMP_ISO] | "";
    if (isoTs[0]) {
        publishDoc[F_TIMESTAMP] = isoTs;
    } else {
        // No trusted UTC at capture (clock not yet synced). Say so explicitly:
        // an absent ts leaves the receiver free to stamp its own arrival time,
        // which silently rewrites capture time to hours or weeks later.
        publishDoc["ts_unknown"] = 1;
    }
    return !out.overflowed();
}

void _queueMqttNotification(uint8_t type, const char* text) {
    if (!text || !text[0]) {
        return;
    }

    if (!BUS.publishNotification(type, text)) {
        DLOG_WARN("MQTT", "Notification queue full, dropped type=%u",
                  static_cast<unsigned>(type));
    }
}

void _dumpSlicePause() {
    vTaskDelay(pdMS_TO_TICKS(1));
}

bool _stateRequiresUploadLease(MQTTState state) {
    return state == MQTT_CONNECTING_WIFI ||
           state == MQTT_CONNECTING_BROKER ||
           state == MQTT_DUMPING;
}

static constexpr uint32_t kDumpSliceBudgetMs       = MQTT_DUMP_SLICE_BUDGET_MS;
static constexpr uint8_t  kDumpMaxRecordsPerSlice  = MQTT_DUMP_RECORDS_PER_SLICE;
static constexpr uint16_t kProgressLogEveryN       = MQTT_DUMP_PROGRESS_EVERY_N;
static constexpr uint16_t kDurableCheckpointEveryN = MQTT_DUMP_CHECKPOINT_EVERY_N;
static constexpr uint32_t kBrokerConnectSettleMs   = 300UL;
static constexpr uint32_t kBrokerConnectAttemptGapMs = 2000UL;
static constexpr uint32_t kUploadPublishSettleMs   = 0UL;
static constexpr uint32_t kUploadPublishRetryDelayMs = 750UL;
static constexpr uint32_t kQos1PubackTimeoutMs      = 5000UL;
// Well inside PubSubClient's 15 s keepalive, so the broker never times
// out a dump that pauses for storage work.
static constexpr uint32_t kRawKeepaliveIdleMs       = 5000UL;
static constexpr size_t   kMaxMqttPayloadBytes     = 1535U;
// Streaming upload buffer. Storage walks the spool directly; MQTT only stages
// a small payload window at a time before publishing/checkpointing/yielding.
// The tail of a field backlog can have many stale sessions and very little
// internal heap left, so the actual fill limit is reduced dynamically below
// this ceiling.
static constexpr uint16_t kUploadRamBucketRecords  = 64U;
static constexpr int8_t   kUploadWifiTxPowerQdbm   = 78; // 19.5 dBm, quarter-dBm units.

bool _uploadPausedByMission() {
    RunContext context = RUN_CONTEXT_GENERAL;
    MissionProfile profile = MISSION_RECON;
    uint8_t storageMode = STORAGE_MODE_NORMAL;

    STATE_READ_BEGIN();
    context = sanitizeRunContext(g_state.runContext);
    profile = sanitizeMissionProfile(g_state.activeMissionProfile);
    storageMode = g_state.storageMode;
    STATE_READ_END();

    const bool storagePressureOverride = storageMode >= STORAGE_MODE_FULL;
    return context == RUN_CONTEXT_MISSION &&
           profile != MISSION_UPLINK &&
           !storagePressureOverride;
}

// Compute a radio lease duration that scales with the number of pending
// records so the arbiter knows how long the upload window will actually be.
// Capped between MIN and MAX from config.h.
static uint32_t _calcUploadLeaseMs(int pendingRecords) {
    const uint32_t events =
        pendingRecords > 0 ? static_cast<uint32_t>(pendingRecords) : 0U;
    const uint64_t raw = static_cast<uint64_t>(MQTT_UPLOAD_LEASE_CONNECT_MS)
                       + static_cast<uint64_t>(events) *
                         static_cast<uint64_t>(MQTT_UPLOAD_LEASE_MS_PER_EVENT);
    return std::max(MQTT_UPLOAD_LEASE_MIN_MS,
                    static_cast<uint32_t>(
                        std::min<uint64_t>(MQTT_UPLOAD_LEASE_MAX_MS, raw)));
}

// Refresh the current upload lease window.  holdMs is the value computed at
// dump-start so every refresh uses the same window that was originally granted.
void _refreshUploadLease(const char* reason, uint32_t holdMs) {
    if (RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD)) {
        RADIO_ARB.refreshLease(RADIO_WIFI_UPLOAD, holdMs, reason);
    }
}

}  // namespace

void MQTTManager::begin() {
    _mqtt.setClient(_wifiClient);
    _mqtt.setServer(_configuredBrokerHost(), _configuredBrokerPort());
    _mqtt.setBufferSize(2048);
    _mqtt.setKeepAlive(90);
    _mqtt.setSocketTimeout(3);

    _migrateLegacyQueueFiles();
    _refreshPendingCount();

    const StorageManager::PendingBacklogTrustState backlogState =
        STORAGE.getBacklogTrustState();
    if (backlogState == StorageManager::BACKLOG_TRUSTED) {
        if (_queuedRecords > MQTT_BACKLOG_LARGE_WARN_THRESHOLD) {
            DLOG_WARN("MQTT", "Backlog large: %d records", _queuedRecords);
        } else {
            DLOG_INFO("MQTT", "Backlog loaded: %d records", _queuedRecords);
        }
    } else if (backlogState == StorageManager::BACKLOG_DEGRADED) {
        DLOG_WARN("MQTT",
                  "Backlog degraded: counter trust degraded pending=%d",
                  _queuedRecords);
    } else {
        DLOG_WARN("MQTT", "Backlog unknown: repair required");
    }

    // Schedule the one-shot startup FieldVault upload window. The actual
    // attempt is gated in tick() once boot grace elapses.
    _bootGraceUntilMs = millis() + MQTT_FIELDVAULT_STARTUP_GRACE_MS;
}

void MQTTManager::tick() {
    // Dump slices now run inline on TaskHardware, so only service the MQTT
    // client from the idle/non-dumping path.
    if (_state != MQTT_DUMPING && _mqtt.connected()) {
        _serviceMqttLink();
    }
    // Opportunistic one-shot: if FieldVault has pending records and the boot
    // grace has elapsed, fire a single short field-only dump. Sets the latch
    // so we never retry within this boot.
    _maybeStartStartupFieldDump();
    _runStateMachine();
}

bool MQTTManager::uploadLeaseReady(bool force) const {
    if (!force && _uploadPausedByMission()) {
        return false;
    }
    if (_uploadStoppedBySerial) {
        return false;
    }
    if (!force && STORAGE.isReady() &&
        !STORAGE.isPendingEventCountAuthoritative()) {
        return false;
    }
    const uint32_t pendingRecords =
        STORAGE.isReady() ? STORAGE.getAuthoritativePendingEventCount() : 0U;
    // Signed-delta compare for wraparound safety: a raw `millis() >= deadline`
    // misfires for ~24 days after a millis() wrap past the stored deadline.
    const bool backoffExpired =
        static_cast<int32_t>(millis() - _uploadBackoffUntilMs) >= 0;
    return force ||
           (_continuousDrainActive &&
            pendingRecords > 0 &&
            backoffExpired) ||
           (pendingRecords >= MQTT_UPLOAD_READY_THRESHOLD &&
            backoffExpired);
}
// ── Dump request ──────────────────────────────────────────────

bool MQTTManager::requestDump(bool force) {
    if (_state != MQTT_IDLE) return false;
    if (!force && _uploadPausedByMission()) {
        DLOG_INFO("MQTT", "Upload deferred while non-uplink mission is active");
        return false;
    }
    if (!uploadLeaseReady(force)) return false;

    const uint32_t pendingRecords =
        STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U;
    const uint32_t uploadWindowRecords = pendingRecords;
    _uploadLeaseHoldMs = _calcUploadLeaseMs(static_cast<int>(uploadWindowRecords));
    if (force || pendingRecords >= MQTT_UPLOAD_READY_THRESHOLD) {
        _continuousDrainActive = true;
    }

    bool granted = RADIO_ARB.requestUploadLease(
        _uploadLeaseHoldMs,
        force ? "forced_dump" : "threshold_dump",
        force);

    if (granted) {
        _wifiConnectStarted = false;
        _lastBrokerConnectAttemptMs = 0;
        _brokerConnectSettleLogged = false;
        _resumeDumpAfterReconnect = false;
        _stopRequested = false;
        _stopCleanup = false;
        _dumpCtx = DumpContext{};
        _uploadStartStackWatermarkBytes = _currentTaskStackWatermarkBytes();

        _setUploadUiState(true, "UPLOADING", 0, pendingRecords, true);
        const StorageWindowKind windowKind =
            _fieldOnlyMode ? STORAGE_WINDOW_FIELDVAULT_UPLOAD
                           : STORAGE_WINDOW_UPLOAD;
        if (!_storageWindow.begin(windowKind,
                                  _fieldOnlyMode ? "fieldvault_upload"
                                                  : "upload_stream")) {
            DLOG_WARN("MQTT", "Upload quiet window unavailable");
            _uploadBackoffUntilMs = millis() + MQTT_FAILED_BACKOFF_MS;
            _setUploadUiState(false, "", 0, 0, false);
            RADIO_ARB.release(RADIO_WIFI_UPLOAD, "upload_window_unavailable", false);
            RADIO_ARB.ensureDefaultCapture("upload_window_unavailable");
            _logUploadStackWatermark("window_unavailable");
            return false;
        }

        CONTRACT_WARN_ONCE(CONTRACT_RAMSPOOL_PAUSED_DURING_UPLOAD_INDEX,
                           "MQTT",
                           _storageWindow.workerPaused(),
                           "pending=%lu",
                           static_cast<unsigned long>(pendingRecords));
        CONTRACT_WARN_ONCE(CONTRACT_DISPLAY_SUSPENDED_DURING_UPLOAD,
                           "MQTT",
                           _storageWindow.displaySuspended(),
                           "pending=%lu",
                           static_cast<unsigned long>(pendingRecords));

        if (!STORAGE.prepareUploadIndexForUpload(_uploadLeaseHoldMs)) {
            DLOG_WARN("MQTT", "Upload stream not ready; dump deferred");
            _uploadBackoffUntilMs = millis() + MQTT_FAILED_BACKOFF_MS;
            _storageWindow.end("upload_stream_unavailable");
            _setUploadUiState(false, "", 0, 0, false);
            RADIO_ARB.release(RADIO_WIFI_UPLOAD, "upload_stream_unavailable", false);
            RADIO_ARB.ensureDefaultCapture("upload_stream_unavailable");
            _logUploadStackWatermark("stream_unavailable");
            return false;
        }

        _startDumpPlan();
        if (!_fillUploadBucketRadioQuiet(kUploadRamBucketRecords)) {
            DLOG_WARN("MQTT", "Upload RAM bucket prefill failed; dump deferred");
            _uploadBackoffUntilMs = millis() + MQTT_FAILED_BACKOFF_MS;
            _storageWindow.end("upload_prefill_failed");
            _setUploadUiState(false, "", 0, 0, false);
            RADIO_ARB.release(RADIO_WIFI_UPLOAD, "upload_prefill_failed", false);
            RADIO_ARB.ensureDefaultCapture("upload_prefill_failed");
            _logUploadStackWatermark("prefill_failed");
            return false;
        }
        _resumeDumpAfterReconnect = true;

        // Defer LittleFS watermark flushes until the radio is paused at
        // end-of-dump. Per-ack "w" opens during an active-radio window
        // have been observed to brown the rail.
        STORAGE.beginUploadBatch();

        _state = MQTT_CONNECTING_WIFI;
        _stateEnteredMs = millis();
        _bleTriggered = force;
        _setUploadUiState(false, "", 0, 0, true);

        DLOG_INFO("MQTT",
                  "Upload lease granted pending=%lu window=%lu lease=%lus",
                  static_cast<unsigned long>(pendingRecords),
                  static_cast<unsigned long>(uploadWindowRecords),
                  static_cast<unsigned long>(_uploadLeaseHoldMs / 1000UL));
        return true;
    }
    return false;
}

void MQTTManager::_logUploadStackWatermark(const char* result) {
    const uint32_t afterBytes = _currentTaskStackWatermarkBytes();
    const uint32_t beforeBytes = _uploadStartStackWatermarkBytes;
    if (beforeBytes == 0) {
        return;
    }

    const uint32_t dropBytes = (afterBytes < beforeBytes) ?
        (beforeBytes - afterBytes) : 0U;
    if (dropBytes >= kUploadStackWatermarkDropWarnBytes) {
        DLOG_WARN("STACK",
                  "TaskHardware upload watermark result=%s before=%luB after=%luB drop=%luB",
                  (result && result[0]) ? result : "-",
                  static_cast<unsigned long>(beforeBytes),
                  static_cast<unsigned long>(afterBytes),
                  static_cast<unsigned long>(dropBytes));
    } else {
        DLOG_INFO("STACK",
                  "TaskHardware upload watermark result=%s before=%luB after=%luB drop=%luB",
                  (result && result[0]) ? result : "-",
                  static_cast<unsigned long>(beforeBytes),
                  static_cast<unsigned long>(afterBytes),
                  static_cast<unsigned long>(dropBytes));
    }

    _uploadStartStackWatermarkBytes = 0;
}

bool MQTTManager::bleTriggeredDump() {
    return requestDump(true);
}

bool MQTTManager::requestUploadStop(const char* reason) {
    _uploadStoppedBySerial = true;
    _continuousDrainActive = false;

    if (!_stateRequiresUploadLease(_state)) {
        DLOG_WARN("MQTT",
                  "Upload auto-start paused reason=%s state=%d",
                  (reason && reason[0]) ? reason : "-",
                  static_cast<int>(_state));
        return false;
    }

    _stopRequested = true;
    DLOG_WARN("MQTT",
              "Upload stop requested reason=%s state=%d published=%d pending=%d",
              (reason && reason[0]) ? reason : "-",
              static_cast<int>(_state),
              _lastPublished,
              _queuedRecords);
    return true;
}

bool MQTTManager::requestUploadResume(const char* reason) {
    const bool wasPaused = _uploadStoppedBySerial;
    _uploadStoppedBySerial = false;
    DLOG_INFO("MQTT",
              "Upload auto-start resumed reason=%s wasPaused=%d",
              (reason && reason[0]) ? reason : "-",
              wasPaused ? 1 : 0);
    return wasPaused;
}

bool MQTTManager::requestFieldVaultDump() {
    if (_state != MQTT_IDLE) return false;
    if (_uploadStoppedBySerial) return false;
    if (!FieldVault::hasPending()) return false;
    if (_uploadPausedByMission()) {
        DLOG_INFO("MQTT", "FieldVault upload deferred while non-uplink mission is active");
        return false;
    }
    return _startStartupFieldDump();
}

bool MQTTManager::_maybeStartStartupFieldDump() {
#if (MQTT_FIELDVAULT_STARTUP_UPLOAD_ENABLED == ON)
    if (_startupFieldDumpDone) return false;
    if (_state != MQTT_IDLE)   return false;
    if (_uploadStoppedBySerial) return false;
    // Signed-delta compare for wraparound safety (see uploadLeaseReady).
    if (static_cast<int32_t>(millis() - _bootGraceUntilMs) < 0) return false;

    // No pending records: latch the one-shot so we don't re-check on every
    // tick. The next normal/manual/threshold dump will drain anything that
    // gets appended later.
    if (!FieldVault::hasPending()) {
        _startupFieldDumpDone = true;
        return false;
    }

    if (_uploadPausedByMission()) {
        // Honor mission-pause: latch and let the regular dump path handle it
        // when conditions allow. Matches the "exactly one attempt" rule.
        DLOG_INFO("MQTT", "Startup field upload skipped: mission paused");
        _startupFieldDumpDone = true;
        return false;
    }

    _startupFieldDumpDone = true;  // latch first; any path below is the attempt
    return _startStartupFieldDump();
#else
    return false;
#endif
}

bool MQTTManager::_startStartupFieldDump() {
    // Mirror requestDump(true) but with a short fixed lease and field-only
    // mode. We still flow through the normal MQTT_CONNECTING_WIFI →
    // MQTT_CONNECTING_BROKER → MQTT_DUMPING → MQTT_DONE/FAILED state machine
    // so existing cleanup (lease release, endUploadBatch, ensureDefaultCapture)
    // runs unchanged.
    if (_state != MQTT_IDLE) return false;

    _uploadLeaseHoldMs = MQTT_FIELDVAULT_STARTUP_LEASE_MS;

    const bool granted = RADIO_ARB.requestUploadLease(
        _uploadLeaseHoldMs,
        "startup_field",
        /*force=*/true);
    if (!granted) {
        DLOG_INFO("MQTT", "Startup field upload skipped: lease not granted");
        return false;
    }

    _wifiConnectStarted = false;
    _lastBrokerConnectAttemptMs = 0;
    _brokerConnectSettleLogged = false;
    _stopRequested = false;
    _stopCleanup = false;
    _dumpCtx = DumpContext{};
    _uploadStartStackWatermarkBytes = _currentTaskStackWatermarkBytes();

    _setUploadUiState(true, "UPLOADING", 0, 0, true);
    if (!_storageWindow.begin(STORAGE_WINDOW_FIELDVAULT_UPLOAD,
                              "startup_field")) {
        DLOG_WARN("MQTT", "Startup field quiet window unavailable");
        RADIO_ARB.release(RADIO_WIFI_UPLOAD, "startup_field_window_unavailable", false);
        RADIO_ARB.ensureDefaultCapture("startup_field_window_unavailable");
        return false;
    }
    CONTRACT_WARN_ONCE(CONTRACT_FIELDVAULT_UPLOAD_USES_QUIET_WINDOW,
                       "MQTT",
                       _storageWindow.active() &&
                           _storageWindow.workerPaused() &&
                           _storageWindow.displaySuspended(),
                       "owner=%s",
                       RadioArbiter::ownerName(RADIO_ARB.currentOwner()));

    STORAGE.beginUploadBatch();

    _fieldOnlyMode = true;
    _fieldOnlyPublishedThisDump = 0;
    _fieldOnlyClearAfterRelease = false;
    _state = MQTT_CONNECTING_WIFI;
    _stateEnteredMs = millis();
    _bleTriggered = false;
    _setUploadUiState(false, "", 0, 0, true);

    DLOG_INFO("MQTT",
              "Startup field upload pending=%lu lease=%lus",
              static_cast<unsigned long>(FieldVault::uploadedThrough()),
              static_cast<unsigned long>(_uploadLeaseHoldMs / 1000UL));
    return true;
}

void MQTTManager::_prefetchFirstUploadEvent() {
    (void)_fillUploadBucketRadioQuiet(1);
}

// Adds its lifetime to a running total. Used to split drain wall time into
// "reading records off the spool" vs "getting them onto the wire", which is
// the first question when a high-backlog upload is slower than expected.
namespace {
struct ScopedElapsedAccumulator {
    uint32_t& total;
    uint32_t startMs;
    ScopedElapsedAccumulator(uint32_t& t, uint32_t start) : total(t), startMs(start) {}
    ~ScopedElapsedAccumulator() { total += millis() - startMs; }
};
}  // namespace

bool MQTTManager::_fillUploadBucketRadioQuiet(uint16_t maxRecords) {
    if (!STORAGE.isReady() || maxRecords == 0) {
        return false;
    }

    std::vector<String>& sessionIds = _dumpCtx.sessionIds;
    if (!_dumpCtx.eventsPrefetched) {
        sessionIds.clear();
        STORAGE.listEventSessions(sessionIds);
        _dumpCtx.sessionIndex = 0;
        _dumpCtx.sinceId = 0;
        _dumpCtx.sinceIdInitialized = false;
        _dumpCtx.bucketNumber = 1;
        _dumpCtx.eventsPrefetched = true;
    }

    _dumpCtx.uploadBucket.clear();
    _dumpCtx.uploadBucketIndex = 0;
    _dumpCtx.uploadBucketComplete = false;
    _dumpCtx.cachedBatch.clear();
    _dumpCtx.cachedBatchIndex = 0;
    _dumpCtx.cachedBatchCount = 0;

    // Cached upload-segment File handle benefits within-fill reads only.
    // Closing here prevents a long-lived File handle from spanning the
    // publish phase, which has been observed to churn LittleFS internal
    // state via watermark/index writes.
    STORAGE.closeUploadReadFile();

    const uint32_t indexedRecords =
        STORAGE.isReady() ? STORAGE.getUploadIndexResidentEventCount() : 0U;
    const uint32_t internalFreeBefore =
        heap_caps_get_free_size(SPECTRE_CAP_DRAM);
    uint16_t heapBoundedMax = maxRecords;
    if (internalFreeBefore < 12288UL) {
        heapBoundedMax = std::min<uint16_t>(heapBoundedMax, 1U);
    } else if (internalFreeBefore < 16384UL) {
        heapBoundedMax = std::min<uint16_t>(heapBoundedMax, 2U);
    } else if (internalFreeBefore < 32768UL) {
        heapBoundedMax = std::min<uint16_t>(heapBoundedMax, 8U);
    } else if (internalFreeBefore < 65536UL) {
        heapBoundedMax = std::min<uint16_t>(heapBoundedMax, 16U);
    }
    const uint16_t fillLimit =
        static_cast<uint16_t>(
            std::min<uint32_t>(heapBoundedMax,
                               indexedRecords > 0 ? indexedRecords : heapBoundedMax));
    if (fillLimit == 0) {
        _dumpCtx.uploadBucketComplete = true;
        return true;
    }

    const uint32_t fillStartMs = millis();
    ScopedElapsedAccumulator fillAccum(_dumpFillMs, fillStartMs);
    if (_dumpCtx.uploadBucket.capacity() < fillLimit) {
        _dumpCtx.uploadBucket.reserve(fillLimit);
    }

    DLOG_DEBUG("MQTT",
              "Upload stream fill begin bucket=%u max=%u requested=%u internalFree=%lu indexWindow=%lu indexed=%lu truncated=%u sessions=%u sessionIndex=%u radioQuiet=1",
              static_cast<unsigned>(_dumpCtx.bucketNumber),
              static_cast<unsigned>(fillLimit),
              static_cast<unsigned>(maxRecords),
              static_cast<unsigned long>(internalFreeBefore),
              static_cast<unsigned long>(STORAGE.getUploadIndexWindowLimit()),
              static_cast<unsigned long>(indexedRecords),
              STORAGE.isUploadIndexWindowTruncated() ? 1U : 0U,
              static_cast<unsigned>(sessionIds.size()),
              static_cast<unsigned>(_dumpCtx.sessionIndex));

    while (_dumpCtx.sessionIndex < sessionIds.size() &&
           _dumpCtx.uploadBucket.size() < fillLimit) {
        const String& sessionId = sessionIds[_dumpCtx.sessionIndex];
        if (!_dumpCtx.sinceIdInitialized) {
            DLOG_DEBUG("MQTT",
                      "Upload bucket session start idx=%u sess=%s bucket=%u",
                      static_cast<unsigned>(_dumpCtx.sessionIndex),
                      sessionId.c_str(),
                      static_cast<unsigned>(_dumpCtx.uploadBucket.size()));
            _dumpCtx.sinceId = STORAGE.getLastUploadedEventId(sessionId.c_str());
            _dumpCtx.sinceIdInitialized = true;
        }

        JsonDocument recordBatch;
        const uint32_t fetchT0 = millis();
        if (!_dumpCtx.firstEventFetchLogged) {
            DLOG_INFO("MQTT",
                      "First event batch fetch session=%s since=%lu radioQuiet=1",
                      sessionId.c_str(),
                      static_cast<unsigned long>(_dumpCtx.sinceId));
            _dumpCtx.firstEventFetchLogged = true;
        }

        const uint16_t remainingBucket =
            static_cast<uint16_t>(fillLimit - _dumpCtx.uploadBucket.size());
        const uint16_t fetchHeapCap =
            internalFreeBefore < 32768UL ? 4U :
            internalFreeBefore < 65536UL ? 8U :
            static_cast<uint16_t>(MQTT_DUMP_FETCH_BATCH_SIZE);
        const int fetchMax =
            static_cast<int>(std::min<uint16_t>(
                remainingBucket,
                std::min<uint16_t>(fetchHeapCap,
                                   MQTT_DUMP_FETCH_BATCH_SIZE)));
        const bool fetchOk = STORAGE.getUploadEventBatchForSession(
            sessionId.c_str(), _dumpCtx.sinceId, fetchMax, recordBatch);
        const uint32_t fetchDt = millis() - fetchT0;

        if (!fetchOk) {
            DLOG_WARN("MQTT",
                      "Upload RAM bucket fetch failed session=%s since=%lu ms=%lu",
                      sessionId.c_str(),
                      static_cast<unsigned long>(_dumpCtx.sinceId),
                      static_cast<unsigned long>(fetchDt));
            _dumpCtx.uploadBucket.clear();
            return false;
        }

        JsonArrayConst records = recordBatch.as<JsonArrayConst>();
        if (records.isNull()) {
            DLOG_WARN("MQTT",
                      "Upload RAM bucket fetch returned non-array session=%s since=%lu",
                      sessionId.c_str(),
                      static_cast<unsigned long>(_dumpCtx.sinceId));
            _dumpCtx.uploadBucket.clear();
            return false;
        }

        if (records.size() == 0) {
            DLOG_DEBUG("MQTT",
                      "Upload RAM bucket session exhausted bucket=%u session=%s since=%lu idx=%u/%u",
                      static_cast<unsigned>(_dumpCtx.bucketNumber),
                      sessionId.c_str(),
                      static_cast<unsigned long>(_dumpCtx.sinceId),
                      static_cast<unsigned>(_dumpCtx.sessionIndex),
                      static_cast<unsigned>(sessionIds.size()));
            _dumpCtx.sessionIndex++;
            _dumpCtx.sinceId = 0;
            _dumpCtx.sinceIdInitialized = false;
            continue;
        }

        uint32_t lastFetchedEventId = 0;
        for (JsonObjectConst record : records) {
            if (_dumpCtx.uploadBucket.size() >= fillLimit) {
                break;
            }

            if (record.isNull()) {
                DLOG_WARN("MQTT",
                          "Upload RAM bucket batch entry non-object session=%s since=%lu",
                          sessionId.c_str(),
                          static_cast<unsigned long>(_dumpCtx.sinceId));
                _dumpCtx.uploadBucket.clear();
                return false;
            }

            const uint32_t eventId = record["id"] | 0U;
            if (eventId == 0) {
                DLOG_WARN("MQTT",
                          "Upload RAM bucket record missing id session=%s since=%lu",
                          sessionId.c_str(),
                          static_cast<unsigned long>(_dumpCtx.sinceId));
                _dumpCtx.uploadBucket.clear();
                return false;
            }

            const uint8_t status =
                record["status"] | static_cast<uint8_t>(EVT_RAW);
            if (status == static_cast<uint8_t>(EVT_UPLOADED)) {
                _dumpCtx.sinceId = eventId;
                lastFetchedEventId = eventId;
                DLOG_DEBUG("MQTT",
                           "Upload stream skip retained event=%lu session=%s",
                           static_cast<unsigned long>(eventId),
                           sessionId.c_str());
                continue;
            }

            const char* type = record["type"] | "event";
            JsonDocument publishDoc;
            if (!_copyEventRecordForPublish(record, publishDoc)) {
                DLOG_WARN("MQTT",
                          "Upload RAM bucket copy failed event=%lu",
                          static_cast<unsigned long>(eventId));
                _dumpCtx.uploadBucket.clear();
                return false;
            }
            const size_t payloadLen = measureJson(publishDoc);
            if (payloadLen == 0 || payloadLen > kMaxMqttPayloadBytes) {
                DLOG_WARN("MQTT",
                          "Upload RAM bucket payload invalid event=%lu bytes=%u",
                          static_cast<unsigned long>(eventId),
                          static_cast<unsigned>(payloadLen));
                _dumpCtx.uploadBucket.clear();
                return false;
            }
            DumpContext::UploadPublishRecord queued;
            queued.eventId = eventId;
            queued.lane = record["lane"] | static_cast<uint8_t>(STORAGE_LANE_NOISE);

            // sessionId / topic / payload all live in PSRAM — see note on
            // UploadPublishRecord. Keeps internal-heap retention near zero.
            const size_t sessionIdLen = sessionId.length();
            queued.sessionId = static_cast<char*>(
                heap_caps_malloc(sessionIdLen + 1U, MALLOC_CAP_SPIRAM));
            if (!queued.sessionId) {
                DLOG_WARN("MQTT",
                          "Upload RAM bucket PSRAM alloc failed (sessionId) event=%lu",
                          static_cast<unsigned long>(eventId));
                _dumpCtx.uploadBucket.clear();
                return false;
            }
            memcpy(queued.sessionId, sessionId.c_str(), sessionIdLen + 1U);

            const String topicStr = _mqttTopicForEventType(type);
            const size_t topicLen = topicStr.length();
            queued.topic = static_cast<char*>(
                heap_caps_malloc(topicLen + 1U, MALLOC_CAP_SPIRAM));
            if (!queued.topic) {
                DLOG_WARN("MQTT",
                          "Upload RAM bucket PSRAM alloc failed (topic) event=%lu",
                          static_cast<unsigned long>(eventId));
                _dumpCtx.uploadBucket.clear();
                return false;
            }
            memcpy(queued.topic, topicStr.c_str(), topicLen + 1U);

            queued.payload = static_cast<char*>(
                heap_caps_malloc(payloadLen + 1U, MALLOC_CAP_SPIRAM));
            if (!queued.payload) {
                DLOG_WARN("MQTT",
                          "Upload RAM bucket PSRAM alloc failed (payload) event=%lu bytes=%u",
                          static_cast<unsigned long>(eventId),
                          static_cast<unsigned>(payloadLen));
                _dumpCtx.uploadBucket.clear();
                return false;
            }
            const size_t written = serializeJson(publishDoc, queued.payload,
                                                 payloadLen + 1U);
            if (written != payloadLen) {
                DLOG_WARN("MQTT",
                          "Upload RAM bucket serialize mismatch event=%lu measured=%u written=%u",
                          static_cast<unsigned long>(eventId),
                          static_cast<unsigned>(payloadLen),
                          static_cast<unsigned>(written));
                _dumpCtx.uploadBucket.clear();
                return false;
            }
            queued.payloadLen = written;
            _dumpCtx.uploadBucket.push_back(std::move(queued));
            _dumpCtx.sinceId = eventId;
            lastFetchedEventId = eventId;
            const size_t stagedCount = _dumpCtx.uploadBucket.size();
            if (stagedCount == 1 || (stagedCount % 64U) == 0U) {
                DLOG_DEBUG("MQTT",
                          "Upload RAM bucket staged bucket=%u records=%u lastEvent=%lu heapFree=%lu psramFree=%lu",
                          static_cast<unsigned>(_dumpCtx.bucketNumber),
                          static_cast<unsigned>(stagedCount),
                          static_cast<unsigned long>(eventId),
                          static_cast<unsigned long>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
                          static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }

        if (!_dumpCtx.firstEventFetchedOkLogged) {
            DLOG_INFO("MQTT",
                      "First event batch fetched ok=1 count=%u ms=%lu radioQuiet=1",
                      static_cast<unsigned>(records.size()),
                      static_cast<unsigned long>(fetchDt));
            _dumpCtx.firstEventFetchedOkLogged = true;
        }

        if (fetchDt >= kUploadFetchSlowWarnMs) {
            DLOG_WARN("MQTT",
                      "Upload stream fetch slow boundary session=%s event=%lu records=%u ms=%lu",
                      sessionId.c_str(),
                      static_cast<unsigned long>(lastFetchedEventId),
                      static_cast<unsigned>(_dumpCtx.uploadBucket.size()),
                      static_cast<unsigned long>(fetchDt));
            if (!_dumpCtx.uploadBucket.empty()) {
                break;
            }
        }

        // Yield once per fetch, unconditionally. The %64 yield inside the
        // staging loop above gets starved when fetches return only a few
        // records each (small MQTT_DUMP_FETCH_BATCH_SIZE) and run slowly near
        // the backlog tail: a long unbroken run of ~1s fetches never crosses a
        // 64-record boundary, so nothing yields, the idle task is starved, and
        // the task watchdog resets the chip. One delay per fetch bounds that
        // gap to a single fetch's duration regardless of how many records land.
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    _dumpCtx.uploadBucketComplete =
        _dumpCtx.sessionIndex >= sessionIds.size();

    DLOG_DEBUG("MQTT",
              "Upload stream fill done bucket=%u records=%u complete=%d ms=%lu",
              static_cast<unsigned>(_dumpCtx.bucketNumber),
              static_cast<unsigned>(_dumpCtx.uploadBucket.size()),
              _dumpCtx.uploadBucketComplete ? 1 : 0,
              static_cast<unsigned long>(millis() - fillStartMs));
    _dumpCtx.bucketNumber++;
    return !_dumpCtx.uploadBucket.empty() || _dumpCtx.uploadBucketComplete;
}

bool MQTTManager::_disconnectStaForUploadRefill() {
    if (_mqtt.connected()) {
        _mqtt.disconnect();
    }
    _wifiClient.stop();
    WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(150));
    _wifiConnectStarted = false;
    _lastBrokerConnectAttemptMs = 0;
    _brokerConnectSettleLogged = false;
    return true;
}

// ── State machine ─────────────────────────────────────────────

void MQTTManager::_runStateMachine() {
    uint32_t elapsed = millis() - _stateEnteredMs;

    if (_stateRequiresUploadLease(_state) &&
        !RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD)) {
        DLOG_WARN("MQTT",
                  "Upload lease lost while state=%d owner=%s",
                  static_cast<int>(_state),
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
        _lastFailed++;
        _state = MQTT_FAILED;
        _stateEnteredMs = millis();
        elapsed = 0;
    }

    CONTRACT_WARN_ONCE(CONTRACT_UPLOAD_OWNER_SYNC,
                       "MQTT",
                       !_stateRequiresUploadLease(_state) ||
                           RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD),
                       "state=%d owner=%s",
                       static_cast<int>(_state),
                       RadioArbiter::ownerName(RADIO_ARB.currentOwner()));

    CONTRACT_WARN_ONCE(CONTRACT_UPLOAD_BATCH_OWNER_SYNC,
                       "MQTT",
                       !STORAGE.isUploadBatchActive() ||
                           RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD),
                       "batch active while owner=%s state=%d",
                       RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                       static_cast<int>(_state));

    if (_stopRequested && _stateRequiresUploadLease(_state)) {
        DLOG_WARN("MQTT",
                  "Upload stop accepted state=%d published=%d pending=%d",
                  static_cast<int>(_state),
                  _lastPublished,
                  _queuedRecords);
        _stopRequested = false;
        _stopCleanup = true;
        _lastFailed = 0;
        _state = MQTT_FAILED;
        _stateEnteredMs = millis();
        elapsed = 0;
    }

    switch (_state) {
        case MQTT_IDLE:
            return;

        case MQTT_CONNECTING_WIFI:
            _refreshUploadLease("mqtt_connecting_wifi", _uploadLeaseHoldMs);
            _setUploadUiState(false, "", 0, 0, true);

            if (WiFi.status() == WL_CONNECTED) {
                DLOG_INFO("MQTT", "WiFi connected, connecting broker");
                _state = MQTT_CONNECTING_BROKER;
                _stateEnteredMs = millis();
                _lastBrokerConnectAttemptMs = 0;
                _brokerConnectSettleLogged = false;
                break;
            }

            if (!_wifiConnectStarted) {
                if (!_connectWiFi()) {
                    _state = MQTT_FAILED;
                    _stateEnteredMs = millis();
                    break;
                }
                _wifiConnectStarted = true;
            }

            if (elapsed > MQTT_WIFI_CONNECT_TIMEOUT_MS) {
                DLOG_WARN("MQTT", "WiFi timeout status=%d elapsed=%lus",
                          static_cast<int>(WiFi.status()),
                          static_cast<unsigned long>(elapsed / 1000UL));
                WiFi.disconnect();
                // Drain in-flight disconnect events before the next radio
                // op (ensureDefaultCapture → set_promiscuous(true)) so we
                // do not race the disconnect callback and brown the rail.
                vTaskDelay(pdMS_TO_TICKS(150));
                _wifiConnectStarted = false;
                _state = MQTT_FAILED;
                _stateEnteredMs = millis();
            }
            break;

        case MQTT_CONNECTING_BROKER:
            _refreshUploadLease("mqtt_connecting_broker", _uploadLeaseHoldMs);
            _setUploadUiState(true, "CONNECTED", 0,
                              _dumpCtx.maxEventsThisLease > 0
                                  ? _dumpCtx.maxEventsThisLease
                                  : static_cast<uint32_t>(_queuedRecords),
                              true);
            if (elapsed < kBrokerConnectSettleMs) {
                if (!_brokerConnectSettleLogged) {
                    DLOG_INFO("MQTT",
                              "Broker connect settling after WiFi association ms=%lu",
                              static_cast<unsigned long>(kBrokerConnectSettleMs));
                    _brokerConnectSettleLogged = true;
                }
                break;
            }
            if (_connectBroker()) {
                if (_resumeDumpAfterReconnect) {
                    DLOG_INFO("MQTT", "Broker connected, resuming dump");
                    _resumeDumpAfterReconnect = false;
                } else {
                    DLOG_INFO("MQTT", "Broker connected, dumping");
                    _startDumpPlan();
                }
                _state = MQTT_DUMPING;
                _stateEnteredMs = millis();
            } else if (elapsed > MQTT_BROKER_CONNECT_TIMEOUT_MS) {
                DLOG_WARN("MQTT", "Broker connect failed");
                DLOG_WARN("MQTT", "Broker timeout");
                _state = MQTT_FAILED;
                _stateEnteredMs = millis();
            }
            break;

        case MQTT_DUMPING:
            _refreshUploadLease("mqtt_dumping", _uploadLeaseHoldMs);
            _setUploadUiState(true, "UPLOADING",
                              static_cast<uint32_t>(_lastPublished),
                              _dumpCtx.maxEventsThisLease,
                              true);
            // PubSubClient's keepalive is suppressed while dumping, so hold the
            // broker's timer open ourselves if publishing has gone quiet (a slow
            // storage slice or a publish backoff can outlast the keepalive).
            if (_lastRawLinkActivityMs == 0) {
                _lastRawLinkActivityMs = millis();
            } else if (millis() - _lastRawLinkActivityMs >= kRawKeepaliveIdleMs) {
                _sendRawPingreq();
            }
            if (_runDumpSlice()) {
                const bool ok =
                    (_dumpCtx.phase == DUMP_PHASE_DONE && _lastFailed == 0);
                _state = ok ? MQTT_DONE : MQTT_FAILED;
                _stateEnteredMs = millis();
            }
            break;

        case MQTT_DONE: {
            _setUploadUiState(false, "", 0, 0, false);
            RADIO_ARB.release(RADIO_WIFI_UPLOAD, "dump_complete", false);
            const bool wasFieldOnlyMode = _fieldOnlyMode;
            DLOG_INFO("MQTT",
                      _fieldOnlyMode ? "Startup field dump complete"
                                     : "Dump complete");
            if (_fieldOnlyClearAfterRelease) {
                FieldVault::clearLive();
                _fieldOnlyClearAfterRelease = false;
            } else {
                FieldVault::flushUploadCursor();
            }
            _appendFieldVaultUploadSummary("done", wasFieldOnlyMode);
            _fieldOnlyMode = false;
            _fieldOnlyPublishedThisDump = 0;
            _lastDumpMs = millis();

            _disconnect();
            // Keep the arbiter idle until deferred LittleFS watermarks are
            // flushed; release() otherwise resumes promiscuous capture before
            // returning.
            crashCheckpoint(CrashPhase::UPLOAD_FLUSH,
                            static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                            static_cast<uint32_t>(_queuedRecords));
            STORAGE.endUploadBatch();
            CONTRACT_WARN_ONCE(CONTRACT_UPLOAD_INDEX_RELEASED_AFTER_UPLOAD,
                               "MQTT",
                               !STORAGE.isUploadIndexResident(),
                               "state=done");
            _refreshPendingCount(true);
            if (!wasFieldOnlyMode && STORAGE.isReady()) {
                _setUploadUiState(true, "CLEANUP",
                                  static_cast<uint32_t>(_lastPublished),
                                  _dumpCtx.maxEventsThisLease,
                                  false);
                if (!STORAGE.compactSpool()) {
                    DLOG_WARN("MQTT", "Post-dump spool compact failed");
                } else {
                    _refreshPendingCount(true);
                }
            }
            crashBreadcrumbClear(CrashPhase::UPLOAD_FLUSH);
            crashBreadcrumbClear(CrashPhase::MQTT_DUMPING);
            _dumpCtx = DumpContext{};
            _storageWindow.end("done");
            RADIO_ARB.ensureDefaultCapture("mqtt_done");
            _logUploadStackWatermark("done");
            _stopRequested = false;
            _stopCleanup = false;
            _state = MQTT_IDLE;

            // Note: session data NOT cleared here
            // Only cleared on explicit debrief screen long press
            break;
        }

        case MQTT_FAILED: {
            const bool stopped = _stopCleanup;
            _stopRequested = false;
            _stopCleanup = false;
            _uploadBackoffUntilMs = stopped ? millis()
                                            : millis() + MQTT_FAILED_BACKOFF_MS;
            _setUploadUiState(true, stopped ? "STOPPED" : "FAILED",
                  static_cast<uint32_t>(_lastPublished),
                  _dumpCtx.maxEventsThisLease > 0
                      ? _dumpCtx.maxEventsThisLease
                      : static_cast<uint32_t>(_queuedRecords),
                  false);
            RADIO_ARB.release(RADIO_WIFI_UPLOAD,
                              stopped ? "dump_stopped" : "dump_failed",
                              false);
            if (stopped) {
                DLOG_WARN("MQTT",
                          _fieldOnlyMode ? "Startup field dump stopped by request"
                                         : "Dump stopped by request");
            } else {
                DLOG_WARN("MQTT",
                          _fieldOnlyMode ? "Startup field dump failed, backoff %lu ms"
                                         : "Dump failed, backoff %lu ms",
                          static_cast<unsigned long>(MQTT_FAILED_BACKOFF_MS));
            }
            FieldVault::flushUploadCursor();
            _fieldOnlyClearAfterRelease = false;
            _appendFieldVaultUploadSummary(stopped ? "stopped" : "failed",
                                           _fieldOnlyMode);
            _fieldOnlyMode = false;
            _fieldOnlyPublishedThisDump = 0;
            _disconnect();
            // Flush whatever partial progress we already acked before capture
            // fallback is resumed.
            crashCheckpoint(CrashPhase::UPLOAD_FLUSH,
                            static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                            static_cast<uint32_t>(_queuedRecords));
            STORAGE.endUploadBatch();
            CONTRACT_WARN_ONCE(CONTRACT_UPLOAD_INDEX_RELEASED_AFTER_UPLOAD,
                               "MQTT",
                               !STORAGE.isUploadIndexResident(),
                               "state=failed");
            _refreshPendingCount(true);
            crashBreadcrumbClear(CrashPhase::UPLOAD_FLUSH);
            crashBreadcrumbClear(CrashPhase::MQTT_DUMPING);
            _dumpCtx = DumpContext{};
            _storageWindow.end(stopped ? "stopped" : "failed");
            RADIO_ARB.ensureDefaultCapture(stopped ? "mqtt_stopped" : "mqtt_failed");
            _logUploadStackWatermark(stopped ? "stopped" : "failed");
            _state = MQTT_IDLE;
            break;
        }
    }
}

// ── WiFi + broker connection ──────────────────────────────────

bool MQTTManager::_connectWiFi() {
    const WiFiCredential* network = _primaryUploadNetwork();
    if (!network || !network->ssid[0]) {
        DLOG_WARN("MQTT", "No upload WiFi configured");
        return false;
    }

    WiFi.setSleep(false);
    const esp_err_t txPowerResult = esp_wifi_set_max_tx_power(kUploadWifiTxPowerQdbm);
    if (txPowerResult != ESP_OK) {
        DLOG_WARN("MQTT", "Upload WiFi TX power cap failed err=%d",
                  static_cast<int>(txPowerResult));
    }
    WiFi.begin(network->ssid, network->password);
    DLOG_INFO("MQTT", "Connecting to upload network: %s timeout=%lus txPowerQdbm=%d",
              network->ssid,
              static_cast<unsigned long>(MQTT_WIFI_CONNECT_TIMEOUT_MS / 1000UL),
              static_cast<int>(kUploadWifiTxPowerQdbm));
    return true;
}

bool MQTTManager::_connectBroker() {
    if (_mqtt.connected()) return true;

    const uint32_t now = millis();
    if (_lastBrokerConnectAttemptMs != 0 &&
        now - _lastBrokerConnectAttemptMs < kBrokerConnectAttemptGapMs) {
        return false;
    }
    _lastBrokerConnectAttemptMs = now;

    // PubSubClient may believe the MQTT session is gone while the underlying
    // WiFiClient still holds a half-open TCP socket. Start each broker connect
    // attempt from a clean transport so reconnects do not accumulate state.
    _wifiClient.stop();
    vTaskDelay(pdMS_TO_TICKS(20));

    String clientSuffix = SESS.getId().substring(0, 8);
    if (!clientSuffix.length()) {
        char fallback[12];
        snprintf(fallback, sizeof(fallback), "%08lx",
                 static_cast<unsigned long>(ESP.getEfuseMac() & 0xFFFFFFFFULL));
        clientSuffix = fallback;
    }

    char clientID[32];
    snprintf(clientID, sizeof(clientID), "spectre_%s", clientSuffix.c_str());

    const char* brokerHost = _configuredBrokerHost();
    const char* brokerUser = _configuredBrokerUser();
    const char* brokerPass = _configuredBrokerPassword();

    if (brokerHost[0] == '\0') {
        DLOG_WARN("MQTT", "Broker host not configured");
        return false;
    }

    DLOG_INFO("MQTT", "Broker connect attempt host=%s port=%u client=%s",
              brokerHost,
              static_cast<unsigned>(_configuredBrokerPort()),
              clientID);
    const uint32_t t0 = millis();
    const bool ok = brokerUser[0]
        ? _mqtt.connect(clientID, brokerUser, brokerPass)
        : _mqtt.connect(clientID);
    if (ok) {
        _wifiClient.setNoDelay(true);
    }
    DLOG_INFO("MQTT", "Broker connect result ok=%d state=%d ms=%lu",
              ok ? 1 : 0,
              _mqtt.state(),
              static_cast<unsigned long>(millis() - t0));
    return ok;
}

void MQTTManager::_startDumpPlan() {
    _lastPublished = 0;
    _lastFailed = 0;
    _dumpStartMs = millis();
    _dumpFillMs = 0;
    _dumpPublishMs = 0;
    _dumpLastRateLogPublished = 0;
    _dumpLastRateLogMs = _dumpStartMs;
    _qos1AckedThisDump = 0;
    _qos1FirstAckLogged = false;

    _dumpCtx = DumpContext{};
    _fieldOnlyClearAfterRelease = false;
    // FieldVault reads LittleFS, so regular event upload skips that phase:
    // event payloads are already RAM-staged before STA comes up. Field-only
    // startup/manual uploads still use the dedicated drain path.
    _dumpCtx.phase = _fieldOnlyMode ? DUMP_PHASE_FIELD : DUMP_PHASE_HEALTH;
    _dumpCtx.sessionIndex = 0;
    _dumpCtx.sinceId = 0;
    _dumpCtx.phaseStarted = false;
    // Snapshot for diagnostics/UI: one upload lease is intended to cover the
    // full backlog; only the RAM publish bucket is chunked.
    _dumpCtx.maxEventsThisLease =
        STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U;

    crashCheckpoint(CrashPhase::MQTT_DUMPING,
                    static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                    static_cast<uint32_t>(_queuedRecords));

    DLOG_INFO("MQTT", "Dump start pendingUpload=%u lease=%lus leaseMaxEvents=%u",
              static_cast<unsigned>(_dumpCtx.maxEventsThisLease),
              static_cast<unsigned long>(_uploadLeaseHoldMs / 1000UL),
              static_cast<unsigned>(_dumpCtx.maxEventsThisLease));

    _setUploadUiState(true, "UPLOADING", 0,
                      _dumpCtx.maxEventsThisLease, true);
}

bool MQTTManager::_runDumpSlice() {
    switch (_dumpCtx.phase) {
        case DUMP_PHASE_IDLE:
            return true;

        case DUMP_PHASE_FIELD: {
            // Drain pending FieldVault records before any other dump phase.
            // Each record is a tiny, already-formatted JSON line; we publish
            // up to MQTT_DUMP_RECORDS_PER_SLICE per slice, then yield to keep
            // the cooperative budget. Failed publishes do NOT advance the
            // upload watermark, so the same record is retried later.
            //
            // Two terminal modes:
            //   normal dump  → on drain/fail/empty, advance to HEALTH so the
            //                  rest of the upload pipeline continues.
            //   _fieldOnlyMode (one-shot startup upload) → on drain/fail/cap,
            //                  go straight to DUMP_PHASE_DONE so the regular
            //                  event backlog is NOT touched. On a fully
            //                  successful drain the live JSONL is cleared.

            // Helper-style locals to keep the two terminal paths uniform.
            auto endNormal = [&](DumpPhase next) {
                _dumpCtx.phase = next;
                _dumpCtx.phaseStarted = false;
            };
            auto endFieldOnly = [&](bool drained) {
                if (drained) {
                    // Every pending live record was published successfully.
                    // Clear the live file after the upload radio is released.
                    _fieldOnlyClearAfterRelease = true;
                    DLOG_INFO("MQTT",
                              "Startup field upload drained published=%u",
                              static_cast<unsigned>(_fieldOnlyPublishedThisDump));
                } else {
                    DLOG_INFO("MQTT",
                              "Startup field upload ended early published=%u",
                              static_cast<unsigned>(_fieldOnlyPublishedThisDump));
                }
                _dumpCtx.phase = DUMP_PHASE_DONE;
                _dumpCtx.phaseStarted = false;
            };

            if (!FieldVault::hasPending()) {
                if (_fieldOnlyMode) {
                    endFieldOnly(/*drained=*/true);
                } else {
                    endNormal(DUMP_PHASE_HEALTH);
                }
                return false;
            }

            const uint32_t sliceStart = millis();
            uint8_t published = 0;
            while (published < MQTT_DUMP_RECORDS_PER_SLICE) {
                // Honor the field-only cap separately from the slice cap.
                if (_fieldOnlyMode &&
                    _fieldOnlyPublishedThisDump >=
                        MQTT_FIELDVAULT_STARTUP_MAX_RECORDS) {
                    endFieldOnly(/*drained=*/false);
                    return false;
                }

                char line[512];
                uint32_t recordEnd = 0;
                if (!FieldVault::peekNext(line, sizeof(line), &recordEnd)) {
                    if (_fieldOnlyMode) {
                        endFieldOnly(/*drained=*/true);
                    } else {
                        endNormal(DUMP_PHASE_HEALTH);
                    }
                    return false;
                }

                const size_t len = strlen(line);
                const bool ok = _publishPayloadQos1(TOPIC_FIELD,
                                                    line,
                                                    len,
                                                    false);

                if (!ok) {
                    DLOG_WARN("MQTT",
                              "FieldVault publish failed (off=%lu len=%u); "
                              "will retry next dump",
                              static_cast<unsigned long>(FieldVault::uploadedThrough()),
                              static_cast<unsigned>(len));
                    if (_fieldOnlyMode) {
                        _lastFailed++;
                        endFieldOnly(/*drained=*/false);
                    } else {
                        endNormal(DUMP_PHASE_HEALTH);
                    }
                    return false;
                }

                FieldVault::markUploadedThroughVolatile(recordEnd);
                _lastPublished++;
                published++;
                if (_fieldOnlyMode) _fieldOnlyPublishedThisDump++;

                if (millis() - sliceStart > MQTT_DUMP_SLICE_BUDGET_MS) {
                    return false;
                }
            }
            // Slice budget exhausted on records-per-slice; yield with phase
            // unchanged so the next slice continues draining the vault.
            return false;
        }

        case DUMP_PHASE_HEALTH:
            _dumpCtx.phaseStarted = true;
            DLOG_INFO("MQTT", "Health publish begin");
            _publishHealth();
            DLOG_INFO("MQTT", "Health publish end");
            _dumpCtx.phase = DUMP_PHASE_EVENTS;
            _dumpCtx.phaseStarted = false;
            return false;

        case DUMP_PHASE_EVENTS: {
            if (!_dumpCtx.phaseStarted) {
                DLOG_INFO("MQTT",
                          "Upload events phase sessions=%u bucketRecords=%u radioHot=1",
                          static_cast<unsigned>(_dumpCtx.sessionIds.size()),
                          static_cast<unsigned>(_dumpCtx.uploadBucket.size()));
                _dumpCtx.phaseStarted = true;
            }

            const uint32_t sliceStartMs = millis();
            uint8_t recordsThisSlice = 0;

            while (_dumpCtx.uploadBucketIndex < _dumpCtx.uploadBucket.size()) {
                if ((millis() - sliceStartMs) >= kDumpSliceBudgetMs) {
                    _serviceMqttLink();
                    return false;
                }

                if (recordsThisSlice >= kDumpMaxRecordsPerSlice) {
                    _serviceMqttLink();
                    return false;
                }

                const DumpContext::UploadPublishRecord& record =
                    _dumpCtx.uploadBucket[_dumpCtx.uploadBucketIndex];

                if (_dumpCtx.publishRetryAtMs != 0 &&
                    static_cast<int32_t>(millis() - _dumpCtx.publishRetryAtMs) < 0) {
                    if (_mqtt.connected()) {
                        _serviceMqttLink();
                    }
                    return false;
                }
                _dumpCtx.publishRetryAtMs = 0;

                if (!_dumpCtx.firstEventDocBuiltLogged) {
                    DLOG_INFO("MQTT",
                              "First event payload staged event=%lu bytes=%u",
                              static_cast<unsigned long>(record.eventId),
                              static_cast<unsigned>(record.payloadLen));
                    _dumpCtx.firstEventDocBuiltLogged = true;
                }

                if (!_dumpCtx.firstEventPublishBeginLogged) {
                    DLOG_INFO("MQTT",
                              "First event publish begin event=%lu bytes=%u stack=%luB",
                              static_cast<unsigned long>(record.eventId),
                              static_cast<unsigned>(record.payloadLen),
                              static_cast<unsigned long>(_currentTaskStackWatermarkBytes()));
                    _dumpCtx.firstEventPublishBeginLogged = true;
                }

                _refreshUploadLease("mqtt_publish", _uploadLeaseHoldMs);
                if (kUploadPublishSettleMs > 0) {
                    vTaskDelay(pdMS_TO_TICKS(kUploadPublishSettleMs));
                }

                if (!_mqtt.connected()) {
                    DLOG_WARN("MQTT",
                              "Broker disconnected before event=%lu; reconnecting",
                              static_cast<unsigned long>(record.eventId));
                    if (!_connectBroker()) {
                        DLOG_WARN("MQTT",
                                  "Reconnect failed before publish event=%lu",
                                  static_cast<unsigned long>(record.eventId));
                        _lastFailed++;
                        _dumpCtx.phase = DUMP_PHASE_FAILED;
                        return true;
                    }
                    _serviceMqttLink();
                }

                const uint32_t publishT0 = millis();
                const bool publishOk = _publishPayloadQos1(record.topic,
                                                           record.payload,
                                                           record.payloadLen,
                                                           false);
                const uint32_t publishDt = millis() - publishT0;
                _dumpPublishMs += publishDt;
                if (!publishOk) {
                    const bool samePoisonRecord =
                        (_lastPoisonEventId == record.eventId &&
                         _lastPoisonSessionId == record.sessionId);
                    _lastPoisonEventId = record.eventId;
                    _lastPoisonSessionId = record.sessionId;
                    _lastPoisonEventFailures = samePoisonRecord
                        ? static_cast<uint8_t>(_lastPoisonEventFailures + 1)
                        : 1;
                    DLOG_WARN("MQTT",
                              "Publish failed for session=%s event=%lu"
                              " topic=%s payload=%u ms=%lu"
                              " mqttState=%d connected=%d poisonFails=%u/%u",
                              record.sessionId,
                              static_cast<unsigned long>(record.eventId),
                              record.topic,
                              static_cast<unsigned>(record.payloadLen),
                              static_cast<unsigned long>(publishDt),
                              _mqtt.state(),
                              _mqtt.connected() ? 1 : 0,
                              static_cast<unsigned>(_lastPoisonEventFailures),
                              static_cast<unsigned>(MQTT_POISON_FAIL_LIMIT));

                    if (_lastPoisonEventFailures < MQTT_POISON_FAIL_LIMIT) {
                        _dumpCtx.publishRetryAtMs =
                            millis() + kUploadPublishRetryDelayMs;
                        if (_mqtt.connected()) {
                            _serviceMqttLink();
                        }
                        DLOG_WARN("MQTT",
                                  "Publish retry deferred event=%lu nextIn=%lums",
                                  static_cast<unsigned long>(record.eventId),
                                  static_cast<unsigned long>(kUploadPublishRetryDelayMs));
                        return false;
                    }

                    DLOG_WARN("MQTT",
                              "Publish retry limit reached; retaining event session=%s event=%lu",
                              record.sessionId,
                              static_cast<unsigned long>(record.eventId));
                    _lastFailed++;
                    _dumpCtx.phase = DUMP_PHASE_FAILED;
                    return true;
                }

                if (!_dumpCtx.firstEventPublishEndLogged) {
                    DLOG_INFO("MQTT",
                              "First event publish end event=%lu stack=%luB",
                              static_cast<unsigned long>(record.eventId),
                              static_cast<unsigned long>(_currentTaskStackWatermarkBytes()));
                    _dumpCtx.firstEventPublishEndLogged = true;
                }
                _lastPoisonEventId = 0;
                _lastPoisonSessionId = "";
                _lastPoisonEventFailures = 0;

                if (_mqtt.connected()) {
                    _serviceMqttLink();
                }
                _dumpSlicePause();

                if (!STORAGE.markEventUploaded(record.eventId,
                                               record.sessionId,
                                               record.lane)) {
                    _lastFailed++;
                    DLOG_WARN("MQTT",
                              "Failed to mark uploaded event=%lu session=%s",
                              static_cast<unsigned long>(record.eventId),
                              record.sessionId);
                    _dumpCtx.phase = DUMP_PHASE_FAILED;
                    return true;
                }

                _dumpCtx.uploadBucketIndex++;
                _lastPublished++;
                recordsThisSlice++;
                _refreshUploadLease("mqtt_progress", _uploadLeaseHoldMs);

                _setUploadUiState(true, "UPLOADING",
                                  static_cast<uint32_t>(_lastPublished),
                                  _dumpCtx.maxEventsThisLease,
                                  true);

                // Post-record time yield: placed here so a slow fetch cannot
                // prevent processing at least one record per call.
                if ((millis() - sliceStartMs) >= kDumpSliceBudgetMs) {
                    _serviceMqttLink();
                    return false;
                }

                // Durable checkpoint: persist watermarks mid-upload so a crash
                // cannot roll back more than kDurableCheckpointEveryN events.
                if ((_lastPublished % kDurableCheckpointEveryN) == 0) {
                    DLOG_DEBUG("MQTT",
                              "Upload checkpoint deferred until radio quiet pub=%d",
                              _lastPublished);
                }

                if ((_lastPublished % kProgressLogEveryN) == 0) {
                    DLOG_INFO("MQTT",
                              "Dump progress pub=%d fail=%d session=%u/%u since=%lu leaseMaxEvents=%u",
                              _lastPublished,
                              _lastFailed,
                              static_cast<unsigned>(_dumpCtx.sessionIndex),
                              static_cast<unsigned>(_dumpCtx.sessionIds.size()),
                              static_cast<unsigned long>(_dumpCtx.sinceId),
                              static_cast<unsigned>(_dumpCtx.maxEventsThisLease));

                    // Instantaneous rate over the window since the last log,
                    // plus the cumulative average. The two diverging is the
                    // signal that the drain is degrading as it goes -- which is
                    // what an O(N^2) record lookup looks like from the outside.
                    const uint32_t nowMs = millis();
                    const uint32_t windowMs = nowMs - _dumpLastRateLogMs;
                    const int windowPub = _lastPublished - _dumpLastRateLogPublished;
                    const uint32_t totalMs = nowMs - _dumpStartMs;
                    DLOG_INFO("MQTT",
                              "Dump rate pub=%d window=%d/%lums (%lu.%02lu rec/s) "
                              "avg=%lu.%02lu rec/s fillMs=%lu pubMs=%lu",
                              _lastPublished,
                              windowPub,
                              static_cast<unsigned long>(windowMs),
                              windowMs ? static_cast<unsigned long>(
                                  (windowPub * 1000UL) / windowMs) : 0UL,
                              windowMs ? static_cast<unsigned long>(
                                  ((windowPub * 100000UL) / windowMs) % 100UL) : 0UL,
                              totalMs ? static_cast<unsigned long>(
                                  (_lastPublished * 1000UL) / totalMs) : 0UL,
                              totalMs ? static_cast<unsigned long>(
                                  ((_lastPublished * 100000UL) / totalMs) % 100UL) : 0UL,
                              static_cast<unsigned long>(_dumpFillMs),
                              static_cast<unsigned long>(_dumpPublishMs));
                    _dumpLastRateLogMs = nowMs;
                    _dumpLastRateLogPublished = _lastPublished;
                }
            }

            const uint32_t remainingAfterBucket =
                STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U;
            if (_lastFailed == 0 && remainingAfterBucket > 0) {
                    _setUploadUiState(true, "STREAM",
                                      static_cast<uint32_t>(_lastPublished),
                                      _dumpCtx.maxEventsThisLease,
                                      true);

                DLOG_DEBUG("MQTT",
                          "Upload stream buffer drained; refilling remaining=%lu complete=%d",
                          static_cast<unsigned long>(remainingAfterBucket),
                          _dumpCtx.uploadBucketComplete ? 1 : 0);

                // Keep WiFi+MQTT connected through the refill. The bucket
                // fill is now PSRAM-resident + uses a cached File handle, so
                // it doesn't need a radio-quiet window. Service the MQTT
                // client briefly so the broker keepalive doesn't fire.
                if (_mqtt.connected()) {
                    _serviceMqttLink();
                }

                if ((_lastPublished - _dumpCtx.lastCheckpointPublished) >=
                    kDurableCheckpointEveryN) {
                    if (!STORAGE.flushUploadCheckpoint()) {
                        _lastFailed++;
                        DLOG_WARN("MQTT",
                                  "Upload checkpoint failed during refill");
                        _dumpCtx.phase = DUMP_PHASE_FAILED;
                        return true;
                    }
                    _dumpCtx.lastCheckpointPublished =
                        static_cast<uint32_t>(_lastPublished);
                }

                const uint32_t refillT0 = millis();
                if (!_fillUploadBucketRadioQuiet(kUploadRamBucketRecords)) {
                        _lastFailed++;
                        DLOG_WARN("MQTT",
                                  "Upload stream refill failed remaining=%lu",
                                  static_cast<unsigned long>(remainingAfterBucket));
                        _dumpCtx.phase = DUMP_PHASE_FAILED;
                        return true;
                }

                DLOG_DEBUG("MQTT",
                          "Upload stream refilled bucket=%u records=%u remaining=%lu ms=%lu",
                          static_cast<unsigned>(_dumpCtx.bucketNumber),
                          static_cast<unsigned>(_dumpCtx.uploadBucket.size()),
                          static_cast<unsigned long>(remainingAfterBucket),
                          static_cast<unsigned long>(millis() - refillT0));

                if (_dumpCtx.uploadBucket.empty()) {
                    if (STORAGE.getPendingEventCount() == 0) {
                        _dumpCtx.phase = DUMP_PHASE_CENSUS;
                        _dumpCtx.phaseStarted = false;
                        return false;
                    }

                    if (_dumpCtx.uploadBucketComplete) {
                        const uint32_t pendingAfterRetry =
                            STORAGE.getPendingEventCount();
                        if (pendingAfterRetry > 0) {
                            DLOG_WARN("MQTT",
                                      "Upload stream exhausted sessions with pending=%lu; treating as maintenance-only remainder",
                                      static_cast<unsigned long>(pendingAfterRetry));
                        }
                        _dumpCtx.phase = DUMP_PHASE_CENSUS;
                        _dumpCtx.phaseStarted = false;
                        return false;
                    }

                    _lastFailed++;
                    DLOG_WARN("MQTT",
                              "Upload bucket produced no sessions with pending=%lu complete=%d",
                              static_cast<unsigned long>(STORAGE.getPendingEventCount()),
                              _dumpCtx.uploadBucketComplete ? 1 : 0);
                    _dumpCtx.phase = DUMP_PHASE_FAILED;
                    return true;
                }

                _refreshUploadLease("mqtt_bucket", _uploadLeaseHoldMs);
                // Continue publishing the freshly refilled stream buffer under
                // the same upload connection whenever possible.
                if (WiFi.status() != WL_CONNECTED || !_mqtt.connected()) {
                    _resumeDumpAfterReconnect = true;
                    _wifiConnectStarted = false;
                    _lastBrokerConnectAttemptMs = 0;
                    _brokerConnectSettleLogged = false;
                    _state = MQTT_CONNECTING_WIFI;
                    _stateEnteredMs = millis();
                    return false;
                }
                if (_mqtt.connected()) {
                    _serviceMqttLink();
                }
                return false;
            }

            if (_lastPublished == 0 && _dumpCtx.maxEventsThisLease > 0) {
                DLOG_WARN("MQTT",
                          "No events published despite pendingUpload=%u at dump start",
                          static_cast<unsigned>(_dumpCtx.maxEventsThisLease));
            }

            _dumpCtx.phase = DUMP_PHASE_CENSUS;
            _dumpCtx.phaseStarted = false;
            return false;
        }

        case DUMP_PHASE_CENSUS:
            DLOG_INFO("MQTT", "Census publish begin");
            _publishCensus();
            DLOG_INFO("MQTT", "Census publish end");
            _dumpCtx.phase = DUMP_PHASE_COMPACT;
            _dumpCtx.phaseStarted = false;
            return false;

        case DUMP_PHASE_COMPACT:
            if (_lastFailed == 0) {
                _setUploadUiState(true, "COMPACT",
                                  static_cast<uint32_t>(_lastPublished),
                                  _dumpCtx.maxEventsThisLease,
                                  true);
                _disconnectStaForUploadRefill();
                const int compactedSessions = STORAGE.compactAllUploadedEventFiles();
                DLOG_INFO("MQTT", "Compacted uploaded event files for %d session(s)",
                          compactedSessions);
            }
            _dumpCtx.phase = DUMP_PHASE_PURGE;
            _dumpCtx.phaseStarted = false;
            return false;

        case DUMP_PHASE_PURGE:
            _refreshPendingCount();

            if (_lastFailed == 0 && STORAGE.getPendingEventCount() == 0) {
                if (_purgeTransientFiles()) {
                    DLOG_INFO("MQTT", "Transient files purged after successful dump");
                    _refreshPendingCount();
                } else {
                    DLOG_WARN("MQTT", "Transient purge incomplete");
                }
                // Upload drained — radio will release shortly. This is the
                // best window to ask maintenance for an FS audit, since
                // capture and upload have just been quiet.
                STORAGE.requestMaintenance(STORAGE_MAINT_FS_AUDIT,
                                           "post_upload");
            }

            {
                const uint32_t drainMs = millis() - _dumpStartMs;
                DLOG_INFO("MQTT",
                          "upload_session_summary pub=%d acked=%lu failed=%d remain=%d "
                          "leaseMs=%lu drainMs=%lu rate=%lu.%02lu rec/s "
                          "fillMs=%lu pubMs=%lu otherMs=%lu",
                          _lastPublished,
                          static_cast<unsigned long>(_qos1AckedThisDump),
                          _lastFailed, _queuedRecords,
                          static_cast<unsigned long>(_uploadLeaseHoldMs),
                          static_cast<unsigned long>(drainMs),
                          drainMs ? static_cast<unsigned long>(
                              (_lastPublished * 1000UL) / drainMs) : 0UL,
                          drainMs ? static_cast<unsigned long>(
                              ((_lastPublished * 100000UL) / drainMs) % 100UL) : 0UL,
                          static_cast<unsigned long>(_dumpFillMs),
                          static_cast<unsigned long>(_dumpPublishMs),
                          static_cast<unsigned long>(
                              drainMs > (_dumpFillMs + _dumpPublishMs)
                                  ? drainMs - (_dumpFillMs + _dumpPublishMs) : 0UL));
            }

            _dumpCtx.phase = (_lastFailed == 0) ? DUMP_PHASE_DONE : DUMP_PHASE_FAILED;
            return true;

        case DUMP_PHASE_DONE:
        case DUMP_PHASE_FAILED:
            return true;
    }

    _dumpCtx.phase = DUMP_PHASE_FAILED;
    return true;
}

void MQTTManager::_disconnect() {
    if (_mqtt.connected()) _mqtt.disconnect();
    _wifiClient.stop();
    _wifiConnectStarted = false;
    // Leave WiFi up — WiFiManager owns its own state.
}

// ── Dump execution ────────────────────────────────────────────

void MQTTManager::_setUploadUiState(bool active,
                                    const char* phase,
                                    uint32_t published,
                                    uint32_t total,
                                    bool radioBusy) {
    uint16_t percent = 0;
    if (total > 0) {
        percent = static_cast<uint16_t>((published * 100UL) / total);
        if (percent > 100) percent = 100;
    }

    const char* nextPhase = phase ? phase : "";

    STATE_WRITE_BEGIN();

    g_state.uploadActive = active;
    g_state.radioBusy = radioBusy;
    g_state.uploadPublished = published;
    g_state.uploadTotal = total;
    g_state.uploadPercent = percent;
    strlcpy(g_state.uploadPhase, nextPhase, sizeof(g_state.uploadPhase));

    STATE_WRITE_END();
}

void MQTTManager::_appendFieldVaultUploadSummary(const char* result,
                                                 bool fieldOnly) {
    if (!FieldVault::isReady()) {
        return;
    }

    const uint32_t queued =
        _queuedRecords < 0 ? 0U : static_cast<uint32_t>(_queuedRecords);
    const uint32_t pending =
        STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U;

    if (!FieldVault::appendUploadSummary(result,
                                         static_cast<uint32_t>(_lastPublished),
                                         static_cast<uint32_t>(_lastFailed),
                                         queued,
                                         _uploadLeaseHoldMs,
                                         pending,
                                         fieldOnly)) {
        DLOG_WARN("FIELDVAULT", "upload summary append failed");
    }
}

void MQTTManager::_publishHealth() {
    JsonDocument doc;
    const RuntimeSettings* settings = _settingsView();
    const char* fwVersion =
        (settings && settings->deviceVersion[0]) ?
            settings->deviceVersion : SPECTRE_DEVICE_VERSION;
    const uint32_t now = millis();
    char ts[32];
    _timestamp(ts, sizeof(ts));

    bool     storageSummaryValid        = false;
    uint32_t storageSummaryAgeMs        = 0;
    uint8_t  storageSummaryStatus       = STORAGE_SUMMARY_UNKNOWN;
    uint32_t storageMission             = 0;
    uint32_t storageNoise               = 0;
    uint32_t storagePendingUploadMission = 0;
    uint32_t storagePendingUploadNoise   = 0;
    uint32_t storagePendingEnrichMission = 0;
    uint32_t storagePendingEnrichNoise   = 0;
    uint32_t storageEnrichmentDeltas    = 0;
    uint8_t  companionEnabled           = 0;
    uint8_t  companionPhone             = 0;
    uint8_t  companionWork              = 0;
    uint32_t companionPending           = 0;
    uint32_t companionLastSeenMs        = 0;

    STATE_READ_BEGIN();
    doc["sensor"]      = MQTT_SENSOR_ID;
    doc["status"]      = "ok";
    doc["ts"]          = ts;
    doc["time_valid"]  = g_state.timeValid;
    doc["time_source"] = g_state.timeSource;
    doc["time_iso"]    = g_state.timeISO;
    doc["time_local"]  = g_state.timeLocal;
    doc["uptime_ms"]   = g_state.uptimeMs;
    doc["batt_pct"]    = g_state.battPercent;
    doc["lora_ready"]  = g_state.loraReady;
    doc["session_id"]  = g_state.sessionId;
    doc["fw_version"]  = fwVersion;
    doc["export_last_ok"] = g_state.exportLastOk;
    doc["export_last_events"] = g_state.exportLastEvents;
    doc["export_last_files"] = g_state.exportLastFiles;
    doc["export_last_pending"] = g_state.exportLastPending;
    doc["export_last_iso"] = g_state.exportLastISO;
    doc["export_last_session"] = g_state.exportLastSessionId;
    storageSummaryValid        = g_state.storageSummaryValid;
    storageSummaryAgeMs        = spectreStorageSummaryAgeMs(now);
    storageSummaryStatus       =
        static_cast<uint8_t>(spectreStorageSummaryComputeStatus(now));
    storageMission             = g_state.storageMissionTotal;
    storageNoise               = g_state.storageNoiseTotal;
    storagePendingUploadMission = g_state.storagePendingUploadMission;
    storagePendingUploadNoise   = g_state.storagePendingUploadNoise;
    storagePendingEnrichMission = g_state.storagePendingEnrichMission;
    storagePendingEnrichNoise   = g_state.storagePendingEnrichNoise;
    storageEnrichmentDeltas    = g_state.storageEnrichmentDeltas;
    companionEnabled           = g_state.companionEnabled;
    companionPhone             = g_state.companionPhone;
    companionWork              = g_state.companionWork;
    companionPending           = g_state.companionPending;
    companionLastSeenMs        = g_state.companionLastSeenMs;
    STATE_READ_END();

    {
        JsonObject storage = doc["storage"].to<JsonObject>();
        storage["summary_valid"]          = storageSummaryValid;
        storage["summary_age_ms"]         = storageSummaryAgeMs;
        storage["summary_status"]         =
            spectreStorageSummaryStatusName(
                static_cast<StorageSummaryStatus>(storageSummaryStatus));
        storage["mission_events"]         = storageMission;
        storage["noise_events"]           = storageNoise;
        storage["pending_upload_mission"] = storagePendingUploadMission;
        storage["pending_upload_noise"]   = storagePendingUploadNoise;
        storage["pending_enrich_mission"] = storagePendingEnrichMission;
        storage["pending_enrich_noise"]   = storagePendingEnrichNoise;
        storage["enrichment_deltas"]      = storageEnrichmentDeltas;
    }

    {
        JsonObject companion = doc["companion"].to<JsonObject>();
        companion["enabled"]    = companionEnabled ? 1 : 0;
        companion["phone"]      = companionPhone;
        companion["work"]       = companionWork;
        companion["pending"]    = companionPending;
        companion["last_seen_s"] =
            (companionEnabled && companionLastSeenMs) ?
                (now - companionLastSeenMs) / 1000UL : 0;
    }

    const String topic = _mqttTopicFor("health");
    _publishJson(topic.c_str(), doc, true);  // retained
}

void MQTTManager::_publishCensus() {
    JsonDocument doc;
    const uint32_t now = millis();
    char ts[32];
    bool tagSet = false;
    char tag[32] = "";
    _timestamp(ts, sizeof(ts));

    uint32_t storageMission             = 0;
    uint32_t storageNoise               = 0;
    uint32_t storagePendingUploadMission = 0;
    uint32_t storagePendingUploadNoise   = 0;
    uint32_t storagePendingEnrichMission = 0;
    uint32_t storagePendingEnrichNoise   = 0;
    uint32_t storageEnrichmentDeltas    = 0;
    uint8_t  companionEnabled           = 0;
    uint8_t  companionPhone             = 0;
    uint8_t  companionWork              = 0;
    uint32_t companionPending           = 0;
    uint32_t companionLastSeenMs        = 0;

    STATE_READ_BEGIN();
    doc["sensor"]          = MQTT_SENSOR_ID;
    doc["ts"]              = ts;
    doc["time_valid"]      = g_state.timeValid;
    doc["time_source"]     = g_state.timeSource;
    doc["time_iso"]        = g_state.timeISO;
    doc["time_local"]      = g_state.timeLocal;
    doc["session_id"]      = g_state.sessionId;
    doc["session_networks"]= g_state.sessionNetworks;
    doc["session_devices"] = g_state.sessionDevices;
    doc["session_probes"]  = g_state.sessionProbes;
    doc["session_pmkids"]  = g_state.sessionPMKIDs;
    doc["session_drones"]  = g_state.sessionDrones;
    doc["uptime_ms"]       = g_state.uptimeMs;
    tagSet = g_state.sessionTagSet;
    strlcpy(tag, g_state.sessionTag, sizeof(tag));
    storageMission             = g_state.storageMissionTotal;
    storageNoise               = g_state.storageNoiseTotal;
    storagePendingUploadMission = g_state.storagePendingUploadMission;
    storagePendingUploadNoise   = g_state.storagePendingUploadNoise;
    storagePendingEnrichMission = g_state.storagePendingEnrichMission;
    storagePendingEnrichNoise   = g_state.storagePendingEnrichNoise;
    storageEnrichmentDeltas    = g_state.storageEnrichmentDeltas;
    companionEnabled           = g_state.companionEnabled;
    companionPhone             = g_state.companionPhone;
    companionWork              = g_state.companionWork;
    companionPending           = g_state.companionPending;
    companionLastSeenMs        = g_state.companionLastSeenMs;
    STATE_READ_END();

    if (tagSet) doc["session_tag"] = tag;

    {
        // Compact flat storage block — nested sub-objects and event ID range
        // are omitted here to keep census payload size conservative.
        // Full breakdown lives in the export manifest.
        JsonObject storage = doc["storage"].to<JsonObject>();
        storage["mission_events"]     = storageMission;
        storage["noise_events"]       = storageNoise;
        storage["pending_upload_m"]   = storagePendingUploadMission;
        storage["pending_upload_n"]   = storagePendingUploadNoise;
        storage["pending_enrich_m"]   = storagePendingEnrichMission;
        storage["pending_enrich_n"]   = storagePendingEnrichNoise;
        storage["enrichment_deltas"]  = storageEnrichmentDeltas;
    }

    {
        JsonObject companion = doc["companion"].to<JsonObject>();
        companion["enabled"]    = companionEnabled ? 1 : 0;
        companion["phone"]      = companionPhone;
        companion["work"]       = companionWork;
        companion["pending"]    = companionPending;
        companion["last_seen_s"] =
            (companionEnabled && companionLastSeenMs) ?
                (now - companionLastSeenMs) / 1000UL : 0;
    }

    const String topic = _mqttTopicFor("census");
    _publishJson(topic.c_str(), doc, false);
}

bool MQTTManager::_purgeTransientFiles() {
    bool ok = true;
    _setUploadUiState(true,
                      (_lastFailed == 0) ? "DONE" : "FAILED",
                      static_cast<uint32_t>(_lastPublished),
                      _dumpCtx.maxEventsThisLease,
                      true);
    auto shouldDelete = [](const char* dirPath, const String& name) -> bool {
        if (name.length() == 0) {
            return false;
        }

        if (strcmp(dirPath, PATH_EVENTS) == 0) {
            return name.endsWith(".upload.jsonl");
        }

        if (strcmp(dirPath, PATH_LOGS) == 0) {
            // Logs are offloaded over MQTT, so clear them locally
            return name == "debug.log" || name == "debug.log.1";
        }

        if (strcmp(dirPath, PATH_PMKID_DIR) == 0 ||
            strcmp(dirPath, "/pmkid") == 0) {
            return true;
        }

        if (strcmp(dirPath, PATH_EXPORTS) == 0) {
            // Session exports are operational artifacts, not vault data.
            return true;
        }

        return false;
    };

    auto purgeDir = [&](const char* dirPath) {
        if (!LittleFS.exists(dirPath)) {
            return;
        }

        File dir = LittleFS.open(dirPath);
        if (!dir || !dir.isDirectory()) {
            return;
        }

        File f = dir.openNextFile();
        while (f) {
            String fullPath = String(f.name());
            String nameOnly = fullPath;
            const bool isDir = f.isDirectory();

            const int slash = nameOnly.lastIndexOf('/');
            if (slash >= 0) {
                nameOnly = nameOnly.substring(slash + 1);
            }

            const bool deleteThis = (!isDir && shouldDelete(dirPath, nameOnly));

            if (isDir && strcmp(dirPath, PATH_EXPORTS) == 0) {
                // Normalise path: f.name() sometimes returns a bare name
                // with no directory prefix, which causes open() and rmdir()
                // to silently fail, leaving the directory non-empty.
                if (!fullPath.startsWith("/")) {
                    fullPath = String(dirPath);
                    if (!fullPath.endsWith("/")) fullPath += "/";
                    fullPath += nameOnly;
                }
                File exportDir = LittleFS.open(fullPath);
                File child = exportDir ? exportDir.openNextFile() : File();
                while (child) {
                    // Same normalisation as the parent loop: child.name() may
                    // return a bare filename, which makes LittleFS.remove()
                    // silently fail and leaves the directory non-empty.
                    String childPath(child.name());
                    if (!childPath.startsWith("/")) {
                        String prefix = fullPath;
                        if (!prefix.endsWith("/")) prefix += "/";
                        childPath = prefix + childPath;
                    }
                    child.close();
                    if (!LittleFS.remove(childPath)) {
                        DLOG_WARN("MQTT", "Failed to remove export artifact: %s",
                                  childPath.c_str());
                        ok = false;
                    }
                    child = exportDir.openNextFile();
                }
                if (exportDir) {
                    exportDir.close();
                }
                if (!LittleFS.rmdir(fullPath)) {
                    DLOG_WARN("MQTT", "Failed to remove export session dir: %s",
                              fullPath.c_str());
                    ok = false;
                }
                f = dir.openNextFile();
                continue;
            }

            f.close();

            if (!deleteThis) {
                f = dir.openNextFile();
                continue;
            }

            if (!fullPath.startsWith("/")) {
                fullPath = String(dirPath);
                if (!fullPath.endsWith("/")) {
                    fullPath += "/";
                }
                fullPath += nameOnly;
            }

            if (!LittleFS.remove(fullPath)) {
                DLOG_WARN("MQTT", "Failed to remove transient file: %s",
                          fullPath.c_str());
                ok = false;
            }
            f = dir.openNextFile();
        }
    };

    purgeDir(PATH_EVENTS);
    purgeDir(PATH_LOGS);
    purgeDir(PATH_PMKID_DIR);
    purgeDir("/pmkid");
    purgeDir(PATH_EXPORTS);

    LittleFS.mkdir(PATH_EVENTS);
    LittleFS.mkdir(PATH_LOGS);
    LittleFS.mkdir(PATH_PMKID_DIR);
    LittleFS.mkdir("/pmkid");
    LittleFS.mkdir(PATH_EXPORTS);

    return ok;
}

bool MQTTManager::_publishPayload(const char* topic,
                                  const char* payload,
                                  size_t payloadLen,
                                  bool retained) {
    if (!topic || !topic[0] || !payload || payloadLen == 0) {
        DLOG_WARN("MQTT", "Publish payload invalid topic=%s bytes=%u",
                  topic ? topic : "(null)",
                  static_cast<unsigned>(payloadLen));
        return false;
    }

    if (payloadLen > kMaxMqttPayloadBytes) {
        DLOG_WARN("MQTT",
                  "Payload exceeds publish buffer topic=%s bytes=%u buf=%u retained=%d",
                  topic,
                  static_cast<unsigned>(payloadLen),
                  static_cast<unsigned>(kMaxMqttPayloadBytes),
                  retained ? 1 : 0);
        return false;
    }

    auto publishOnce = [&]() -> bool {
        if (!_mqtt.connected()) {
            return false;
        }

        if (!_mqtt.beginPublish(topic,
                                static_cast<unsigned int>(payloadLen),
                                retained)) {
            return false;
        }

        const size_t written =
            _mqtt.write(reinterpret_cast<const uint8_t*>(payload), payloadLen);
        const bool ended = _mqtt.endPublish();
        if (written != payloadLen || !ended) {
            DLOG_WARN("MQTT",
                      "Stream payload mismatch topic=%s bytes=%u written=%u ended=%d retained=%d",
                      topic,
                      static_cast<unsigned>(payloadLen),
                      static_cast<unsigned>(written),
                      ended ? 1 : 0,
                      retained ? 1 : 0);
            return false;
        }

        return true;
    };

    if (_mqtt.connected()) {
        _serviceMqttLink();
    }
    bool ok = publishOnce();
    if (!ok && _mqtt.connected()) {
        _serviceMqttLink();
        _dumpSlicePause();
        ok = publishOnce();
    }
    if (!ok) {
        DLOG_WARN("MQTT",
                  "Broker rejected publish topic=%s bytes=%u retained=%d state=%d connected=%d",
                  topic,
                  static_cast<unsigned>(payloadLen),
                  retained ? 1 : 0,
                  _mqtt.state(),
                  _mqtt.connected() ? 1 : 0);
    }
    return ok;
}

void MQTTManager::_serviceMqttLink() {
    // PubSubClient and _publishPayloadQos1() share one TCP socket but each has
    // its own MQTT parser, and they cannot both read it.
    //
    // The QoS1 publisher writes a PUBLISH frame directly to the socket and then
    // reads until it sees the matching PUBACK, discarding every other packet it
    // encounters - including PubSubClient's PINGRESP. PubSubClient in turn
    // consumes whatever is pending when loop() runs, which can be the PUBACK the
    // publisher is still waiting for. Either way one side loses a packet it
    // needed: the publisher stalls for the full 5 s PUBACK timeout, or
    // PubSubClient never clears pingOutstanding and drops the link with stop().
    //
    // That is what limited the 2026-08-18 backlog drain to ~2.7 records/s, with
    // a 5 s timeout and reconnect every few publishes. While a dump is in
    // flight the raw publisher owns the socket exclusively.
    if (_state == MQTT_DUMPING) return;
    if (!_mqtt.connected()) return;
    _mqtt.loop();
}

void MQTTManager::_sendRawPingreq() {
    if (!_wifiClient.connected()) return;
    const uint8_t ping[2] = {0xC0U, 0x00U};
    if (_wifiClient.write(ping, sizeof(ping)) == sizeof(ping)) {
        _lastRawLinkActivityMs = millis();
    }
    // The PINGRESP is drained (and ignored) by the next QoS1 PUBACK read.
}

bool MQTTManager::_publishPayloadQos1(const char* topic,
                                      const char* payload,
                                      size_t payloadLen,
                                      bool retained) {
    if (!topic || !topic[0] || !payload || payloadLen == 0 ||
        payloadLen > kMaxMqttPayloadBytes || !_mqtt.connected()) {
        return false;
    }

    const size_t topicLen = strlen(topic);
    if (topicLen > 0xFFFFU) {
        return false;
    }

    uint16_t packetId = _nextQos1PacketId++;
    if (packetId == 0) {
        packetId = _nextQos1PacketId++;
    }

    const size_t remainingLength = 2U + topicLen + 2U + payloadLen;
    uint8_t header[5] = {
        static_cast<uint8_t>(0x32U | (retained ? 0x01U : 0x00U)),
        0, 0, 0, 0
    };
    size_t headerLen = 1;
    size_t encoded = remainingLength;
    do {
        uint8_t byte = static_cast<uint8_t>(encoded % 128U);
        encoded /= 128U;
        if (encoded > 0) byte |= 0x80U;
        header[headerLen++] = byte;
    } while (encoded > 0 && headerLen < sizeof(header));

    const uint8_t topicPrefix[2] = {
        static_cast<uint8_t>((topicLen >> 8) & 0xFFU),
        static_cast<uint8_t>(topicLen & 0xFFU)
    };
    const uint8_t packetIdBytes[2] = {
        static_cast<uint8_t>((packetId >> 8) & 0xFFU),
        static_cast<uint8_t>(packetId & 0xFFU)
    };

    const uint32_t deadline = millis() + kQos1PubackTimeoutMs;
    auto writeAll = [&](const uint8_t* data, size_t length) {
        size_t written = 0;
        while (written < length &&
               static_cast<int32_t>(millis() - deadline) < 0) {
            const size_t n = _wifiClient.write(data + written,
                                               length - written);
            if (n == 0) {
                if (!_wifiClient.connected()) return false;
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            written += n;
        }
        return written == length;
    };

    if (!writeAll(header, headerLen) ||
        !writeAll(topicPrefix, sizeof(topicPrefix)) ||
        !writeAll(reinterpret_cast<const uint8_t*>(topic), topicLen) ||
        !writeAll(packetIdBytes, sizeof(packetIdBytes)) ||
        !writeAll(reinterpret_cast<const uint8_t*>(payload), payloadLen)) {
        DLOG_WARN("MQTT", "QoS1 socket write failed packetId=%u",
                  static_cast<unsigned>(packetId));
        _wifiClient.stop();
        return false;
    }

    auto readByte = [&](uint8_t& out) {
        while (static_cast<int32_t>(millis() - deadline) < 0) {
            if (_wifiClient.available() > 0) {
                const int value = _wifiClient.read();
                if (value >= 0) {
                    out = static_cast<uint8_t>(value);
                    return true;
                }
            }
            if (!_wifiClient.connected()) return false;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        return false;
    };

    while (static_cast<int32_t>(millis() - deadline) < 0) {
        uint8_t packetType = 0;
        if (!readByte(packetType)) break;

        size_t incomingLength = 0;
        size_t multiplier = 1;
        bool lengthComplete = false;
        for (uint8_t i = 0; i < 4; ++i) {
            uint8_t byte = 0;
            if (!readByte(byte)) break;
            incomingLength += static_cast<size_t>(byte & 0x7FU) * multiplier;
            if ((byte & 0x80U) == 0) {
                lengthComplete = true;
                break;
            }
            multiplier *= 128U;
        }
        if (!lengthComplete) break;

        uint8_t first = 0;
        uint8_t second = 0;
        for (size_t i = 0; i < incomingLength; ++i) {
            uint8_t byte = 0;
            if (!readByte(byte)) {
                lengthComplete = false;
                break;
            }
            if (i == 0) first = byte;
            if (i == 1) second = byte;
        }
        if (!lengthComplete) break;

        if ((packetType & 0xF0U) == 0x40U && incomingLength == 2U) {
            const uint16_t ackId =
                static_cast<uint16_t>((static_cast<uint16_t>(first) << 8) |
                                      second);
            if (ackId == packetId) {
                _qos1AckedThisDump++;
                _lastRawLinkActivityMs = millis();
                if (!_qos1FirstAckLogged) {
                    _qos1FirstAckLogged = true;
                    DLOG_INFO("MQTT", "QoS1 PUBACK received packetId=%u",
                              static_cast<unsigned>(packetId));
                }
                return true;
            }
            DLOG_WARN("MQTT", "QoS1 PUBACK mismatch expected=%u got=%u",
                      static_cast<unsigned>(packetId),
                      static_cast<unsigned>(ackId));
        }
    }

    DLOG_WARN("MQTT", "QoS1 PUBACK timeout packetId=%u connected=%d",
              static_cast<unsigned>(packetId),
              _wifiClient.connected() ? 1 : 0);
    // A late PUBACK cannot be allowed to satisfy a later event. Reconnect from
    // a clean stream; the retained local watermark makes the event retryable.
    _wifiClient.stop();
    return false;
}

bool MQTTManager::_publishJson(const char* topic,
                                JsonDocument& doc,
                                bool retained,
                                uint32_t debugEventId) {
    const size_t measuredLen = measureJson(doc);
    if (measuredLen == 0) {
        DLOG_WARN("MQTT", "Serialize measure failed topic=%s retained=%d",
                  topic ? topic : "(null)",
                  retained ? 1 : 0);
        return false;
    }

    if (measuredLen > kMaxMqttPayloadBytes) {
        DLOG_WARN("MQTT",
                  "Payload exceeds publish buffer topic=%s bytes=%u buf=%u retained=%d",
                  topic ? topic : "(null)",
                  static_cast<unsigned>(measuredLen),
                  static_cast<unsigned>(kMaxMqttPayloadBytes),
                  retained ? 1 : 0);
        return false;
    }

    auto publishOnce = [&]() -> bool {
        if (!_mqtt.connected()) {
            return false;
        }

        if (!_mqtt.beginPublish(topic,
                                static_cast<unsigned int>(measuredLen),
                                retained)) {
            return false;
        }

        const size_t len = serializeJson(doc, _mqtt);
        const bool ended = _mqtt.endPublish();
        if (len == 0 || len != measuredLen || !ended) {
            DLOG_WARN("MQTT",
                      "Stream publish mismatch topic=%s measured=%u serialized=%u ended=%d retained=%d",
                      topic ? topic : "(null)",
                      static_cast<unsigned>(measuredLen),
                      static_cast<unsigned>(len),
                      ended ? 1 : 0,
                      retained ? 1 : 0);
            return false;
        }

        return true;
    };

    // Drain broker ACKs before writing so the TCP send buffer never backs up.
    if (_mqtt.connected()) {
        _serviceMqttLink();
    }
    bool ok = publishOnce();
    if (!ok && _mqtt.connected()) {
        _serviceMqttLink();
        _dumpSlicePause();
        ok = publishOnce();
    }
    if (!ok) {
        DLOG_WARN("MQTT",
                  "Broker rejected publish topic=%s bytes=%u retained=%d state=%d connected=%d",
                  topic ? topic : "(null)",
                  static_cast<unsigned>(measuredLen),
                  retained ? 1 : 0,
                  _mqtt.state(),
                  _mqtt.connected() ? 1 : 0);
    }
    return ok;
}

// ── Backlog helpers ───────────────────────────────────────────

void MQTTManager::_migrateLegacyQueueFiles() {
    if (LittleFS.exists(LEGACY_QUEUE_MIGRATION_MARKER)) {
        return;
    }

    if (!LittleFS.exists(LEGACY_QUEUE_DIR)) {
        File marker = LittleFS.open(LEGACY_QUEUE_MIGRATION_MARKER, "w");
        if (marker) marker.close();
        return;
    }

    File dir = LittleFS.open(LEGACY_QUEUE_DIR);
    if (!dir || !dir.isDirectory()) {
        File marker = LittleFS.open(LEGACY_QUEUE_MIGRATION_MARKER, "w");
        if (marker) marker.close();
        return;
    }

    int migrated = 0;
    int cleaned = 0;
    int failed = 0;

    File file = dir.openNextFile();
    while (file) {
        if (file.isDirectory()) {
            file.close();
            file = dir.openNextFile();
            continue;
        }

        const String name = String(file.name());
        const String path = String(LEGACY_QUEUE_DIR) + "/" + name;

        const uint32_t mirroredEventId = _extractLegacyEventIdFromQueueName(name);
        if (mirroredEventId > 0) {
            file.close();
            LittleFS.remove(path);
            migrated++;
            file = dir.openNextFile();
            continue;
        }

        const char* type = _legacyEventTypeFromQueueName(name);
        if (!type) {
            file.close();
            LittleFS.remove(path);   // stale unknown legacy artifact
            cleaned++;
            file = dir.openNextFile();
            continue;
        }

        JsonDocument legacyDoc;
        DeserializationError err = deserializeJson(legacyDoc, file);
        file.close();

        if (err || !legacyDoc.is<JsonObject>()) {
            LittleFS.remove(path);   // malformed legacy artifact
            cleaned++;
            continue;
        }

        const char* sessionId = legacyDoc["session_id"] | "";
        if (!sessionId[0] ||
            !STORAGE.appendEvent(type, legacyDoc.as<JsonObjectConst>(), sessionId)) {
            failed++;
            file = dir.openNextFile();
            continue;
        }

        LittleFS.remove(path);
        migrated++;
        file = dir.openNextFile();
    }

    DLOG_INFO("MQTT",
              "Legacy queue migration migrated=%d cleaned=%d failed=%d",
              migrated, cleaned, failed);

    // Write marker once no unhandled legacy files remain.
    // A failed append means storage had a real runtime problem.
    if (failed == 0) {
        File marker = LittleFS.open(LEGACY_QUEUE_MIGRATION_MARKER, "w");
        if (marker) marker.close();

        File checkDir = LittleFS.open(LEGACY_QUEUE_DIR);
        if (checkDir && checkDir.isDirectory()) {
            File leftover = checkDir.openNextFile();
            if (!leftover) {
                LittleFS.rmdir(LEGACY_QUEUE_DIR);
            } else {
                leftover.close();
            }
        }
    }
}

void MQTTManager::_refreshPendingCount(bool refreshDebriefMirror) {
    const uint32_t totalPending =
        STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0;
    const uint32_t sessionPending =
        STORAGE.isReady() ? STORAGE.getSessionPendingEventCount() : 0;
    const bool backlogTrusted = STORAGE.isPendingEventCountAuthoritative();

    _queuedRecords = static_cast<int>(totalPending);
    // Exit drain mode at the low-water mark (not just at empty) so uploads stay
    // BATCHED at the threshold: once the backlog is flushed below this, the next
    // upload only fires after pending re-accumulates to
    // MQTT_UPLOAD_READY_THRESHOLD. (At low water 0 this matches the old
    // drain-to-empty behavior, which streams continuously during active capture.)
    if (totalPending <= MQTT_UPLOAD_DRAIN_EXIT_RECORDS) {
        _continuousDrainActive = false;
    }
    STATE_WRITE_BEGIN();
    g_state.sessionFilesPending = static_cast<int>(totalPending);
    g_state.kaliSyncAvailable = backlogTrusted && (totalPending > 0);
    g_state.kaliSyncPending = backlogTrusted && (totalPending > 0);
    if (refreshDebriefMirror) {
        g_state.exportLastPending = sessionPending;
    }
    STATE_WRITE_END();
}

bool MQTTManager::dumpAvailable() {
    if (STORAGE.isReady() && !STORAGE.isPendingEventCountAuthoritative()) {
        return false;
    }
    return queueDepth() > 0;
}

int MQTTManager::queueDepth() {
    if (STORAGE.isReady() && !STORAGE.isPendingEventCountAuthoritative()) {
        return 0;
    }
    if (!STORAGE.isReady()) {
        return _queuedRecords;
    }
    return static_cast<int>(STORAGE.getAuthoritativePendingEventCount());
}

int MQTTManager::uploadReadyCount() const {
    if (STORAGE.isReady() && !STORAGE.isPendingEventCountAuthoritative()) {
        return 0;
    }
    if (!STORAGE.isReady()) {
        return _queuedRecords;
    }
    return static_cast<int>(STORAGE.getAuthoritativePendingEventCount());
}

// ── Capture enqueue / sync append helpers ─────────────────────

void MQTTManager::_prepareQueuedEvent(JsonDocument& doc) {
    char ts[32];
    bool tagSet = false;
    char tag[32] = "";
    String sessionId = SESS.getId();

    _timestamp(ts, sizeof(ts));
    STATE_READ_BEGIN();
    tagSet = g_state.sessionTagSet;
    strlcpy(tag, g_state.sessionTag, sizeof(tag));
    STATE_READ_END();

    doc["sensor"] = MQTT_SENSOR_ID;
    doc["ts"] = ts;
    doc["session_id"] = sessionId;
    if (tagSet) {
        doc["session_tag"] = tag;
    }
}

uint32_t MQTTManager::_appendSyncEvent(const char* eventType,
                                       JsonDocument& doc) {
    // Exceptional synchronous append path. Normal capture types are routed
    // through RAMSpool::enqueue() at their capture site and never hit LittleFS
    // here.
    const uint32_t appendStartMs = millis();
    const AppendEventResult result =
        STORAGE.appendEventDetailed(eventType, doc.as<JsonObjectConst>());
    const uint32_t appendMs = millis() - appendStartMs;
    if (appendMs >= 250UL) {
        DLOG_WARN("MQTT",
                  "sync append slow ms=%lu type=%s status=%u",
                  static_cast<unsigned long>(appendMs),
                  eventType ? eventType : "unknown",
                  static_cast<unsigned>(result.status));
    }

    if (!result.ok()) {
        switch (result.status) {
            case APPEND_SUPPRESSED_DUPLICATE:
                break;
            case APPEND_DROPPED_POLICY:
                DLOG_WARN("MQTT", "Dropped %s event by storage policy",
                          eventType ? eventType : "unknown");
                break;

            case APPEND_FAILED_PARSE:
                DLOG_WARN("MQTT", "Failed to sync append %s event: parse",
                          eventType ? eventType : "unknown");
                break;

            case APPEND_FAILED_NOT_READY:
                DLOG_WARN("MQTT", "Failed to sync append %s event: storage not ready",
                          eventType ? eventType : "unknown");
                break;

            case APPEND_FAILED_NO_SESSION:
                DLOG_WARN("MQTT", "Failed to sync append %s event: no session",
                          eventType ? eventType : "unknown");
                break;

            case APPEND_FAILED_IO:
                DLOG_WARN("MQTT", "Failed to sync append %s event: I/O",
                          eventType ? eventType : "unknown");
                break;

            case APPEND_FAILED_INVALID:
            default:
                DLOG_WARN("MQTT", "Failed to sync append %s event",
                          eventType ? eventType : "unknown");
                break;
        }
        return 0;
    }

    _noteQueuedRecord();
    return result.eventId;
}

void MQTTManager::_noteQueuedRecord() {
    const uint32_t totalPending =
        STORAGE.isReady() ? STORAGE.getAuthoritativePendingEventCount() : 0U;
    const bool backlogTrusted = STORAGE.isPendingEventCountAuthoritative();
    _queuedRecords = static_cast<int>(totalPending);
    STATE_WRITE_BEGIN();
    g_state.sessionFilesPending = static_cast<int>(totalPending);
    g_state.kaliSyncAvailable = backlogTrusted && (totalPending > 0);
    g_state.kaliSyncPending = backlogTrusted && (totalPending > 0);
    STATE_WRITE_END();
}

bool MQTTManager::queueProbe(const char* mac, const char* ssid,
                              int8_t rssi, uint8_t channel,
                              const char* ieFingerprint,
                              const char* trackId,
                              uint8_t physicalDeviceId,
                              uint16_t sampleSeq,
                              uint16_t sampleFrames,
                              int8_t rssiMin,
                              int8_t rssiMax,
                              const char* sampleReason,
                              int8_t noiseFloor) {
    JsonDocument doc;
    _prepareQueuedEvent(doc);
    doc["mac"]           = mac;
    doc["probed_ssid"]   = ssid ? ssid : "";
    doc["is_broadcast"]  = (!ssid || ssid[0] == '\0') ? 1 : 0;
    doc["rssi"]          = rssi;
    doc["channel"]       = channel;
    doc["ie_fingerprint"]= ieFingerprint ? ieFingerprint : "";
    doc["track_id"]       = trackId ? trackId : "";
    (void)physicalDeviceId;
    doc["localization_sample"] = true;
    doc["sample_seq"] = sampleSeq;
    doc["sample_frames"] = sampleFrames;
    doc["rssi_min"] = rssiMin;
    doc["rssi_max"] = rssiMax;
    // RF context. Without these an RSSI is a bare number: noise_floor gives it
    // an SNR, and ant_gain_q2 says which antenna it was referenced to. Both are
    // required to compare observations across time, place and hardware.
    if (noiseFloor != 0) doc["noise_floor"] = noiseFloor;
    doc["ant_gain_q2"] = SETTINGS.get().antennaGainQ2;
    doc["sample_reason"] = sampleReason ? sampleReason : "interval";
    const RAMSpool::CaptureClassification probeCls =
        RAMSpool::classify("probe", doc.as<JsonObjectConst>());
    const bool queued = RAMSpool::enqueue("probe",
                                          doc.as<JsonObjectConst>(),
                                          RAMSpool::SLOT_PROBE,
                                          probeCls);
    if (!queued) {
        DLOG_WARN("MQTT", "probe enqueue drop mac=%s ssid=%s",
                  mac ? mac : "",
                  ssid ? ssid : "");
        return false;
    }
    return true;
}

void MQTTManager::queueNetwork(const char* bssid, const char* ssid,
                               int8_t rssi, uint8_t channel,
                               const char* security, bool isHidden,
                               bool hasWPS, const char* trackId,
                               uint16_t sampleSeq, uint16_t sampleFrames,
                               int8_t rssiMin, int8_t rssiMax,
                               const char* sampleReason,
                               int8_t noiseFloor) {
    if (!bssid || !bssid[0]) return;

    JsonDocument doc;
    _prepareQueuedEvent(doc);
    doc["bssid"]     = bssid;
    doc["ssid"]      = ssid ? ssid : "";
    doc["rssi"]      = rssi;
    doc["channel"]   = channel;
    doc["security"]  = security ? security : "";
    doc["is_hidden"] = isHidden ? 1 : 0;
    doc["has_wps"]   = hasWPS ? 1 : 0;
    doc["source"]    = "spectre_field";
    doc["track_id"]  = trackId ? trackId : "";
    doc["localization_sample"] = true;
    doc["sample_seq"] = sampleSeq;
    doc["sample_frames"] = sampleFrames;
    doc["rssi_min"] = rssiMin;
    doc["rssi_max"] = rssiMax;
    // RF context. Without these an RSSI is a bare number: noise_floor gives it
    // an SNR, and ant_gain_q2 says which antenna it was referenced to. Both are
    // required to compare observations across time, place and hardware.
    if (noiseFloor != 0) doc["noise_floor"] = noiseFloor;
    doc["ant_gain_q2"] = SETTINGS.get().antennaGainQ2;
    doc["sample_reason"] = sampleReason ? sampleReason : "interval";
    const RAMSpool::CaptureClassification networkCls =
        RAMSpool::classify("network", doc.as<JsonObjectConst>());
    if (!RAMSpool::enqueue("network",
                           doc.as<JsonObjectConst>(),
                           RAMSpool::SLOT_DEVICE,
                           networkCls)) {
        DLOG_WARN("MQTT", "network enqueue drop bssid=%s ssid=%s",
                  bssid,
                  ssid ? ssid : "");
    }
}

void MQTTManager::queueDevice(const char* mac,
                               const char* ieFingerprint,
                               const char* probeSetHash,
                               int8_t rssi, bool isRandomMAC,
                               const char* trackId,
                               uint8_t physicalDeviceId) {
    JsonDocument doc;
    _prepareQueuedEvent(doc);
    doc["mac"]            = mac;
    doc["ie_fingerprint"] = ieFingerprint ? ieFingerprint : "";
    doc["probe_set_hash"] = probeSetHash  ? probeSetHash  : "";
    doc["rssi"]           = rssi;
    doc["is_random_mac"]  = isRandomMAC ? 1 : 0;
    doc["source"]         = "spectre_field";
    doc["track_id"]       = trackId ? trackId : "";
    doc["physical_device_id"] = physicalDeviceId;
    const RAMSpool::CaptureClassification deviceCls =
        RAMSpool::classify("device", doc.as<JsonObjectConst>());
    const bool queued = RAMSpool::enqueue("device",
                                          doc.as<JsonObjectConst>(),
                                          RAMSpool::SLOT_DEVICE,
                                          deviceCls);
    if (!queued) {
        DLOG_WARN("MQTT", "device enqueue drop mac=%s",
                  mac ? mac : "");
    }
}

void MQTTManager::queueDrone(const char* droneID,
                              float lat, float lon, float alt,
                              const char* mac, int8_t rssi,
                              uint8_t channel,
                              const char* protocol) {
    JsonDocument doc;
    _prepareQueuedEvent(doc);

    // Match EtherGuard drone_telemetry schema exactly
    doc["mac"]         = mac;
    doc["rssi"]        = rssi;
    doc["channel"]     = channel;
    doc["protocol"]    = protocol ? protocol : "unknown";
    doc["drone_id"]    = droneID ? droneID : "";
    doc["latitude"]    = lat;
    doc["longitude"]   = lon;
    doc["altitude_m"]  = alt;
    doc["source_frame"]= "spectre_field";
    doc["event_type"]  = "drone_remote_id";
    doc["severity"]    = "CRITICAL";
    doc["category"]    = "drone";
    _appendSyncEvent("drone", doc);
}

void MQTTManager::queuePMKID(const char* ssid,
                              const char* bssid,
                              const char* clientMAC,
                              const uint8_t* pmkid,
                              uint8_t eapolMask) {
    JsonDocument doc;
    _prepareQueuedEvent(doc);

    // Hashcat format: PMKID*BSSID*ClientMAC*SSID_hex
    char ssidHex[65];

    const size_t ssidLen = strnlen(ssid ? ssid : "", 32);
    for (size_t i = 0; i < ssidLen; i++) {
        snprintf(ssidHex + i*2, 3, "%02x", (uint8_t)ssid[i]);
    }
    ssidHex[ssidLen * 2] = '\0';

    char pmkidHex[33];
    for (int i = 0; i < 16; i++) {
        snprintf(pmkidHex + i*2, 3, "%02x", pmkid[i]);
    }
    pmkidHex[32] = '\0';

    char hashcatLine[160];
    snprintf(hashcatLine, sizeof(hashcatLine),
             "PMKID*%s*%s*%s", pmkidHex, bssid, clientMAC);

    // EAPOL mask: bit0=M1 bit1=M2 bit2=M3 bit3=M4. 0 = none observed.
    // Surfaces handshake-completion state to the server so triage can
    // distinguish PMKID-only vs M1+M2 vs fully-validated 4-way.
    const uint8_t m = eapolMask & 0x0F;
    const bool fullHandshake = (m == 0x0F);

    doc["ssid"]         = ssid;
    doc["bssid"]        = bssid;
    doc["client_mac"]   = clientMAC;
    doc["pmkid_hex"]    = pmkidHex;
    doc["hashcat_line"] = hashcatLine;
    doc["eapol_mask"]   = m;
    doc["eapol_m1"]     = (bool)(m & 0x01);
    doc["eapol_m2"]     = (bool)(m & 0x02);
    doc["eapol_m3"]     = (bool)(m & 0x04);
    doc["eapol_m4"]     = (bool)(m & 0x08);
    doc["eapol_full"]   = fullHandshake;
    doc["event_type"]   = "pmkid_captured";
    doc["severity"]     = "WARN";
    doc["category"]     = "capture";
    const uint32_t eventId =
        _appendSyncEvent("pmkid", doc);
    if (!eventId) {
        return;
    }

    STATE_READ_BEGIN();
    const bool gpsValid = g_state.gpsValid;
    const float gpsLat = g_state.gpsLat;
    const float gpsLon = g_state.gpsLon;
    const float gpsAlt = g_state.gpsAlt;
    const float gpsAcc = g_state.gpsAccuracy;
    const bool tagSet = g_state.sessionTagSet;
    char tagBuf[32] = {};
    strlcpy(tagBuf, g_state.sessionTag, sizeof(tagBuf));
    STATE_READ_END();

    if (gpsValid) {
        STORAGE.enrichEvent(eventId,
                            gpsLat, gpsLon, gpsAlt, gpsAcc,
                            tagSet ? tagBuf : "");
    }

    // Also write hashcat file to LittleFS for direct extraction
    char hcPath[48];
    snprintf(hcPath, sizeof(hcPath),
             PATH_PMKID_DIR "/%s.hc22000", bssid);
    LittleFS.mkdir(PATH_PMKID_DIR);
    File hcFile = LittleFS.open(hcPath, FILE_APPEND);
    if (hcFile) {
        hcFile.println(hashcatLine);
        hcFile.close();
    }

    char notifText[48];
    snprintf(notifText, sizeof(notifText), "PMKID: %s", ssid);
    _queueMqttNotification(NOTIF_PMKID, notifText);
}

void MQTTManager::queueEvent(const char* eventType,
                              const char* severity,
                              const char* mac,
                              const char* ssid,
                              const char* detail,
                              const char* category) {
    JsonDocument doc;
    _prepareQueuedEvent(doc);
    doc["event_type"] = eventType;
    doc["severity"]   = severity;
    doc["mac"]        = mac    ? mac    : "";
    doc["ssid"]       = ssid   ? ssid   : "";
    doc["detail"]     = detail ? detail : "";
    doc["category"]   = category ? category : "detection";
    _appendSyncEvent("event", doc);
}

void MQTTManager::noteExternalQueuedRecord() {
    _noteQueuedRecord();
}

// ── Timestamp ─────────────────────────────────────────────────

void MQTTManager::_timestamp(char* buf, int len) {
    if (!TIME_SVC.formatNowIso(buf, len)) {
        if (buf && len > 0) {
            buf[0] = '\0';
        }
    }
}
