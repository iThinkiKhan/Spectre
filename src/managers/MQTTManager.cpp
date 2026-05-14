

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
#define LEGACY_QUEUE_MIGRATION_MARKER "/events/mqtt_legacy_migrated.flag"

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
    if (strcmp(type, "drone") == 0)  return _mqttTopicFor("drone");
    if (strcmp(type, "pmkid") == 0)  return _mqttTopicFor("pmkid");
    return _mqttTopicFor("event");
}

const char* _legacyEventTypeFromQueueName(const String& name) {
    if (name.startsWith("subghz_")) return "subghz";
    if (name.startsWith("probe_"))  return "probe";
    if (name.startsWith("device_")) return "device";
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

void _copyEventRecordForPublish(JsonObjectConst record, JsonDocument& out) {
    out.clear();
    JsonObject publishDoc = out.to<JsonObject>();

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
    }
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
    vTaskDelay(pdMS_TO_TICKS(2));
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
static constexpr uint32_t kUploadPublishSettleMs   = 10UL;
static constexpr size_t   kMaxMqttPayloadBytes     = 1535U;
// Per-record retention is ~24 B internal (three PSRAM allocations'
// bookkeeping); the payload itself lives in PSRAM. 256 records × ~24 B ≈
// 6 KB internal, well within the ~33 KB free at upload time.
static constexpr uint16_t kUploadRamBucketRecords  = 256U;
static constexpr int8_t   kUploadWifiTxPowerQdbm   = 32; // 8 dBm, quarter-dBm units.

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
        _mqtt.loop();
    }
    // Opportunistic one-shot: if FieldVault has pending records and the boot
    // grace has elapsed, fire a single short field-only dump. Sets the latch
    // so we never retry within this boot.
    _maybeStartStartupFieldDump();
    _runStateMachine();
}

bool MQTTManager::uploadLeaseReady(bool force) const {
    if (_uploadPausedByMission()) {
        return false;
    }
    if (STORAGE.isReady() && !STORAGE.isPendingEventCountAuthoritative()) {
        return false;
    }
    const uint32_t pendingRecords =
        STORAGE.isReady() ? STORAGE.getAuthoritativePendingEventCount() : 0U;
    return force ||
           (_continuousDrainActive &&
            pendingRecords > 0 &&
            millis() >= _uploadBackoffUntilMs) ||
           (pendingRecords >= MQTT_UPLOAD_READY_THRESHOLD &&
            millis() >= _uploadBackoffUntilMs);
}
// ── Dump request ──────────────────────────────────────────────

bool MQTTManager::requestDump(bool force) {
    if (_state != MQTT_IDLE) return false;
    if (_uploadPausedByMission()) {
        DLOG_INFO("MQTT", "Upload deferred while non-uplink mission is active");
        return false;
    }
    if (!uploadLeaseReady(force)) return false;

    const uint32_t pendingRecords =
        STORAGE.isReady() ? STORAGE.getAuthoritativePendingEventCount() : 0U;
    const uint32_t uploadWindowRecords =
        std::min<uint32_t>(pendingRecords, STORAGE_UPLOAD_WINDOW_RECORDS);
    _uploadLeaseHoldMs = _calcUploadLeaseMs(static_cast<int>(uploadWindowRecords));
    if (force ||
        pendingRecords >= MQTT_UPLOAD_READY_THRESHOLD ||
        pendingRecords > uploadWindowRecords) {
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
        _dumpCtx = DumpContext{};
        _uploadStartStackWatermarkBytes = _currentTaskStackWatermarkBytes();

        // Capture pauses for the duration of the upload mission. The radio
        // arbiter has already stopped new captures (we hold RADIO_WIFI_UPLOAD);
        // drainAndPauseWorker waits for any in-flight RAMSpool slots to finish
        // flushing to flash, then gates the worker. Eliminates the concurrent
        // LittleFS R/W race that was triple-faulting the chip during bucket
        // fill — and matches the product intent: offload quickly, don't
        // continue capturing.
        RAMSpool::drainAndPauseWorker(5000);

        // Render one last UI frame announcing the upload, then suspend
        // TaskDisplay for the whole mission. On ESP32-S3 with OPI PSRAM, flash
        // and PSRAM share SPI0 cache; an LVGL render that touches PSRAM-backed
        // fonts/framebuffer overlapping a LittleFS.open() flash op causes a
        // silent triple-fault. With TaskDisplay suspended, core 1 is quiet
        // (RAMSpool worker is already paused too).
        _setUploadUiState(true, "UPLOADING", 0, pendingRecords, true);
        _pauseDisplayForUpload();

        if (!STORAGE.prepareUploadIndexForUpload(_uploadLeaseHoldMs)) {
            DLOG_WARN("MQTT", "Upload index not ready; dump deferred");
            _uploadBackoffUntilMs = millis() + MQTT_FAILED_BACKOFF_MS;
            _resumeDisplayAfterUpload();
            _setUploadUiState(false, "", 0, 0, false);
            RAMSpool::resumeWorker();
            RADIO_ARB.release(RADIO_WIFI_UPLOAD, "upload_index_unavailable", false);
            RADIO_ARB.ensureDefaultCapture("upload_index_unavailable");
            _logUploadStackWatermark("index_unavailable");
            return false;
        }

        _startDumpPlan();
        if (!_fillUploadBucketRadioQuiet(kUploadRamBucketRecords)) {
            DLOG_WARN("MQTT", "Upload RAM bucket prefill failed; dump deferred");
            _uploadBackoffUntilMs = millis() + MQTT_FAILED_BACKOFF_MS;
            _resumeDisplayAfterUpload();
            _setUploadUiState(false, "", 0, 0, false);
            RAMSpool::resumeWorker();
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
                  "Upload lease granted — pending=%lu window=%lu lease=%lus",
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

bool MQTTManager::requestFieldVaultDump() {
    if (_state != MQTT_IDLE) return false;
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
    if (millis() < _bootGraceUntilMs) return false;

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
    _dumpCtx = DumpContext{};
    _uploadStartStackWatermarkBytes = _currentTaskStackWatermarkBytes();

    STORAGE.beginUploadBatch();

    _fieldOnlyMode = true;
    _fieldOnlyPublishedThisDump = 0;
    _fieldOnlyClearAfterRelease = false;
    _state = MQTT_CONNECTING_WIFI;
    _stateEnteredMs = millis();
    _bleTriggered = false;
    _setUploadUiState(false, "", 0, 0, true);

    DLOG_INFO("MQTT",
              "Startup field upload — pending=%lu lease=%lus",
              static_cast<unsigned long>(FieldVault::uploadedThrough()),
              static_cast<unsigned long>(_uploadLeaseHoldMs / 1000UL));
    return true;
}

void MQTTManager::_prefetchFirstUploadEvent() {
    (void)_fillUploadBucketRadioQuiet(1);
}

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

    const uint32_t fillStartMs = millis();
    if (_dumpCtx.uploadBucket.capacity() < maxRecords) {
        _dumpCtx.uploadBucket.reserve(maxRecords);
    }

    DLOG_INFO("MQTT",
              "Upload RAM bucket fill begin bucket=%u max=%u sessions=%u sessionIndex=%u radioQuiet=1",
              static_cast<unsigned>(_dumpCtx.bucketNumber),
              static_cast<unsigned>(maxRecords),
              static_cast<unsigned>(sessionIds.size()),
              static_cast<unsigned>(_dumpCtx.sessionIndex));

    while (_dumpCtx.sessionIndex < sessionIds.size() &&
           _dumpCtx.uploadBucket.size() < maxRecords) {
        const String& sessionId = sessionIds[_dumpCtx.sessionIndex];
        if (!_dumpCtx.sinceIdInitialized) {
            DLOG_INFO("MQTT",
                      "Upload bucket session start idx=%u sess=%s bucket=%u",
                      static_cast<unsigned>(_dumpCtx.sessionIndex),
                      sessionId.c_str(),
                      static_cast<unsigned>(_dumpCtx.uploadBucket.size()));
            _dumpCtx.sinceId = STORAGE.getLastUploadedEventId(sessionId.c_str());
            _dumpCtx.sinceIdInitialized = true;
        }

        JsonDocument recordDoc;
        bool foundRecord = false;
        const uint32_t fetchT0 = millis();
        if (!_dumpCtx.firstEventFetchLogged) {
            DLOG_INFO("MQTT",
                      "First event batch fetch session=%s since=%lu radioQuiet=1",
                      sessionId.c_str(),
                      static_cast<unsigned long>(_dumpCtx.sinceId));
            _dumpCtx.firstEventFetchLogged = true;
        }

        const bool fetchOk = STORAGE.getNextUploadEventForSession(
            sessionId.c_str(), _dumpCtx.sinceId,
            recordDoc, foundRecord);
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

        if (!foundRecord) {
            _dumpCtx.sessionIndex++;
            _dumpCtx.sinceId = 0;
            _dumpCtx.sinceIdInitialized = false;
            continue;
        }

        JsonObjectConst record = recordDoc.as<JsonObjectConst>();
        const uint32_t eventId = record["id"] | 0U;
        if (eventId == 0) {
            DLOG_WARN("MQTT",
                      "Upload RAM bucket record missing id session=%s since=%lu",
                      sessionId.c_str(),
                      static_cast<unsigned long>(_dumpCtx.sinceId));
            _dumpCtx.uploadBucket.clear();
            return false;
        }

        const char* type = record["type"] | "event";
        JsonDocument publishDoc;
        _copyEventRecordForPublish(record, publishDoc);

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

        if (!_dumpCtx.firstEventFetchedOkLogged) {
            DLOG_INFO("MQTT",
                      "First event batch fetched ok=1 count=1 ms=%lu radioQuiet=1",
                      static_cast<unsigned long>(fetchDt));
            _dumpCtx.firstEventFetchedOkLogged = true;
        }

        if (fetchDt >= kUploadFetchSlowWarnMs) {
            DLOG_WARN("MQTT",
                      "Upload RAM bucket fetch slow session=%s event=%lu ms=%lu",
                      sessionId.c_str(),
                      static_cast<unsigned long>(eventId),
                      static_cast<unsigned long>(fetchDt));
        }
    }

    _dumpCtx.uploadBucketComplete =
        _dumpCtx.sessionIndex >= sessionIds.size();

    DLOG_INFO("MQTT",
              "Upload RAM bucket fill done bucket=%u records=%u complete=%d ms=%lu",
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
                              std::min<uint32_t>(static_cast<uint32_t>(_queuedRecords),
                                                 STORAGE_UPLOAD_WINDOW_RECORDS),
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
            if (_mqtt.connected()) {
                _mqtt.loop();
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
            _refreshPendingCount(true);
            if (!wasFieldOnlyMode &&
                STORAGE.isReady() &&
                STORAGE.getAuthoritativePendingEventCount() == 0) {
                _setUploadUiState(true, "CLEANUP",
                                  static_cast<uint32_t>(_lastPublished),
                                  _dumpCtx.maxEventsThisLease,
                                  false);
                if (!STORAGE.compactSpool()) {
                    DLOG_WARN("MQTT", "Final spool compact failed after dump");
                } else {
                    _refreshPendingCount(true);
                }
            }
            crashBreadcrumbClear(CrashPhase::UPLOAD_FLUSH);
            crashBreadcrumbClear(CrashPhase::MQTT_DUMPING);
            RAMSpool::resumeWorker();
            _resumeDisplayAfterUpload();
            RADIO_ARB.ensureDefaultCapture("mqtt_done");
            _logUploadStackWatermark("done");
            _state = MQTT_IDLE;

            // Note: session data NOT cleared here
            // Only cleared on explicit debrief screen long press
            break;
        }

        case MQTT_FAILED:
            _uploadBackoffUntilMs = millis() + MQTT_FAILED_BACKOFF_MS;
            _setUploadUiState(true, "FAILED",
                  static_cast<uint32_t>(_lastPublished),
                  _dumpCtx.maxEventsThisLease > 0
                      ? _dumpCtx.maxEventsThisLease
                      : std::min<uint32_t>(static_cast<uint32_t>(_queuedRecords),
                                           STORAGE_UPLOAD_WINDOW_RECORDS),
                  false);
            RADIO_ARB.release(RADIO_WIFI_UPLOAD, "dump_failed", false);
            DLOG_WARN("MQTT",
                      _fieldOnlyMode ? "Startup field dump failed, backoff %lu ms"
                                     : "Dump failed, backoff %lu ms",
                      static_cast<unsigned long>(MQTT_FAILED_BACKOFF_MS));
            FieldVault::flushUploadCursor();
            _fieldOnlyClearAfterRelease = false;
            _fieldOnlyMode = false;
            _fieldOnlyPublishedThisDump = 0;
            _disconnect();
            // Flush whatever partial progress we already acked before capture
            // fallback is resumed.
            crashCheckpoint(CrashPhase::UPLOAD_FLUSH,
                            static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                            static_cast<uint32_t>(_queuedRecords));
            STORAGE.endUploadBatch();
            _refreshPendingCount(true);
            crashBreadcrumbClear(CrashPhase::UPLOAD_FLUSH);
            crashBreadcrumbClear(CrashPhase::MQTT_DUMPING);
            RAMSpool::resumeWorker();
            _resumeDisplayAfterUpload();
            RADIO_ARB.ensureDefaultCapture("mqtt_failed");
            _logUploadStackWatermark("failed");
            _state = MQTT_IDLE;
            break;
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
    DLOG_INFO("MQTT", "Broker connect result ok=%d state=%d ms=%lu",
              ok ? 1 : 0,
              _mqtt.state(),
              static_cast<unsigned long>(millis() - t0));
    return ok;
}

void MQTTManager::_startDumpPlan() {
    _lastPublished = 0;
    _lastFailed = 0;

    _dumpCtx = DumpContext{};
    _fieldOnlyClearAfterRelease = false;
    // FieldVault reads LittleFS, so regular event upload skips that phase:
    // event payloads are already RAM-staged before STA comes up. Field-only
    // startup/manual uploads still use the dedicated drain path.
    _dumpCtx.phase = _fieldOnlyMode ? DUMP_PHASE_FIELD : DUMP_PHASE_HEALTH;
    _dumpCtx.sessionIndex = 0;
    _dumpCtx.sinceId = 0;
    _dumpCtx.phaseStarted = false;
    // Snapshot for diagnostics/UI: the upload owner bails repeated 5k storage
    // buckets until the backlog is empty or the dump fails.
    _dumpCtx.maxEventsThisLease =
        STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U;

    crashCheckpoint(CrashPhase::MQTT_DUMPING,
                    static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                    static_cast<uint32_t>(_queuedRecords));

    DLOG_INFO("MQTT", "Dump start — pendingUpload=%d lease=%lus leaseMaxEvents=%u",
              _queuedRecords,
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
                              "Startup field upload drained — published=%u",
                              static_cast<unsigned>(_fieldOnlyPublishedThisDump));
                } else {
                    DLOG_INFO("MQTT",
                              "Startup field upload ended early — published=%u",
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

                char line[256];
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
                const bool ok = _mqtt.publish(
                    TOPIC_FIELD,
                    reinterpret_cast<const uint8_t*>(line),
                    static_cast<unsigned int>(len),
                    /*retained=*/false);

                if (!ok) {
                    DLOG_WARN("MQTT",
                              "FieldVault publish failed (off=%lu len=%u) — "
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
                    _mqtt.loop();
                    return false;
                }

                if (recordsThisSlice >= kDumpMaxRecordsPerSlice) {
                    _mqtt.loop();
                    return false;
                }

                const DumpContext::UploadPublishRecord& record =
                    _dumpCtx.uploadBucket[_dumpCtx.uploadBucketIndex];

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
                vTaskDelay(pdMS_TO_TICKS(kUploadPublishSettleMs));

                if (!_mqtt.connected()) {
                    DLOG_WARN("MQTT",
                              "Broker disconnected before event=%lu — reconnecting",
                              static_cast<unsigned long>(record.eventId));
                    if (!_connectBroker()) {
                        DLOG_WARN("MQTT",
                                  "Reconnect failed before publish event=%lu",
                                  static_cast<unsigned long>(record.eventId));
                        _lastFailed++;
                        _dumpCtx.phase = DUMP_PHASE_FAILED;
                        return true;
                    }
                    _mqtt.loop();
                }

                if (!_publishPayload(record.topic,
                                     record.payload,
                                     record.payloadLen,
                                     false)) {
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
                              " topic=%s payload=%u"
                              " mqttState=%d connected=%d poisonFails=%u/%u",
                              record.sessionId,
                              static_cast<unsigned long>(record.eventId),
                              record.topic,
                              static_cast<unsigned>(record.payloadLen),
                              _mqtt.state(),
                              _mqtt.connected() ? 1 : 0,
                              static_cast<unsigned>(_lastPoisonEventFailures),
                              static_cast<unsigned>(MQTT_POISON_FAIL_LIMIT));

                    if (_lastPoisonEventFailures >= MQTT_POISON_FAIL_LIMIT) {
                        if (STORAGE.markEventUploaded(record.eventId,
                                                      record.sessionId,
                                                      record.lane)) {
                            DLOG_WARN("MQTT",
                                      "Quarantined poison event session=%s event=%lu after %u failures",
                                      record.sessionId,
                                      static_cast<unsigned long>(record.eventId),
                                      static_cast<unsigned>(_lastPoisonEventFailures));
                            _dumpCtx.uploadBucketIndex++;
                            _lastPoisonEventId = 0;
                            _lastPoisonSessionId = "";
                            _lastPoisonEventFailures = 0;
                            continue;
                        }

                        DLOG_WARN("MQTT",
                                  "Quarantine mark failed for session=%s event=%lu",
                                  record.sessionId,
                                  static_cast<unsigned long>(record.eventId));
                    }

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

                if (_mqtt.connected()) {
                    _mqtt.loop();
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
                    _mqtt.loop();
                    return false;
                }

                // Durable checkpoint: persist watermarks mid-upload so a crash
                // cannot roll back more than kDurableCheckpointEveryN events.
                if ((_lastPublished % kDurableCheckpointEveryN) == 0) {
                    DLOG_INFO("MQTT",
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
                }
            }

            const uint32_t remainingAfterBucket =
                STORAGE.isReady() ? STORAGE.getPendingEventCount() : 0U;
            if (_lastFailed == 0 && remainingAfterBucket > 0) {
                _setUploadUiState(true, "INDEX",
                                  static_cast<uint32_t>(_lastPublished),
                                  _dumpCtx.maxEventsThisLease,
                                  true);

                DLOG_INFO("MQTT",
                          "Upload RAM bucket drained; refilling without disconnect remaining=%lu complete=%d",
                          static_cast<unsigned long>(remainingAfterBucket),
                          _dumpCtx.uploadBucketComplete ? 1 : 0);

                // Keep WiFi+MQTT connected through the refill. The bucket
                // fill is now PSRAM-resident + uses a cached File handle, so
                // it doesn't need a radio-quiet window. Service the MQTT
                // client briefly so the broker keepalive doesn't fire.
                if (_mqtt.connected()) {
                    _mqtt.loop();
                }

                if (!STORAGE.flushUploadCheckpoint()) {
                    _lastFailed++;
                    DLOG_WARN("MQTT",
                              "Upload checkpoint failed during refill");
                    _dumpCtx.phase = DUMP_PHASE_FAILED;
                    return true;
                }

                if (_dumpCtx.uploadBucketComplete) {
                    _dumpCtx.eventsPrefetched = false;
                    _dumpCtx.sessionIds.clear();
                    _dumpCtx.sessionIndex = 0;
                    _dumpCtx.sinceId = 0;
                    _dumpCtx.sinceIdInitialized = false;
                    if (!STORAGE.prepareUploadIndexForUpload(_uploadLeaseHoldMs)) {
                        _lastFailed++;
                        DLOG_WARN("MQTT",
                                  "Upload index rebuild failed during refill remaining=%lu",
                                  static_cast<unsigned long>(remainingAfterBucket));
                        _dumpCtx.phase = DUMP_PHASE_FAILED;
                        return true;
                    }
                    if (_mqtt.connected()) {
                        _mqtt.loop();
                    }
                }

                const uint32_t refillT0 = millis();
                if (!_fillUploadBucketRadioQuiet(kUploadRamBucketRecords)) {
                    _lastFailed++;
                    DLOG_WARN("MQTT",
                              "Upload RAM bucket refill failed remaining=%lu",
                              static_cast<unsigned long>(remainingAfterBucket));
                    _dumpCtx.phase = DUMP_PHASE_FAILED;
                    return true;
                }

                DLOG_INFO("MQTT",
                          "Upload RAM bucket refilled bucket=%u records=%u remaining=%lu ms=%lu",
                          static_cast<unsigned>(_dumpCtx.bucketNumber),
                          static_cast<unsigned>(_dumpCtx.uploadBucket.size()),
                          static_cast<unsigned long>(remainingAfterBucket),
                          static_cast<unsigned long>(millis() - refillT0));

                if (_dumpCtx.uploadBucket.empty()) {
                    if (STORAGE.getPendingEventCount() == 0) {
                        _dumpCtx.phase = DUMP_PHASE_CENSUS;
                        _dumpCtx.phaseStarted = false;
                        return false;
                    } else {
                        _lastFailed++;
                        DLOG_WARN("MQTT",
                                  "Upload bucket produced no sessions with pending=%lu",
                                  static_cast<unsigned long>(STORAGE.getPendingEventCount()));
                        _dumpCtx.phase = DUMP_PHASE_FAILED;
                    }
                    return true;
                }

                _refreshUploadLease("mqtt_bucket", _uploadLeaseHoldMs);
                // Stay in MQTT_DUMPING — connection is still alive, just
                // continue publishing the freshly refilled bucket on the
                // next state-machine tick.
                if (_mqtt.connected()) {
                    _mqtt.loop();
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
            }

            DLOG_INFO("MQTT", "upload_session_summary pub=%d failed=%d remain=%d leaseMs=%lu",
                      _lastPublished, _lastFailed, _queuedRecords,
                      static_cast<unsigned long>(_uploadLeaseHoldMs));

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
    storageSummaryAgeMs        = now - g_state.storageSummaryUpdatedMs;
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
        _mqtt.loop();
    }
    bool ok = publishOnce();
    if (!ok && _mqtt.connected()) {
        _mqtt.loop();
        _dumpSlicePause();
        ok = publishOnce();
    }
    if (!ok) {
        const int mqttState = _mqtt.state();
        const bool wasConnected = _mqtt.connected();
        DLOG_WARN("MQTT",
                  "Publish retry path topic=%s state=%d connected=%d",
                  topic,
                  mqttState,
                  wasConnected ? 1 : 0);

        _mqtt.disconnect();
        _wifiClient.stop();
        _dumpSlicePause();

        if (_connectBroker()) {
            DLOG_INFO("MQTT", "Broker reconnected during dump");
            _mqtt.loop();
            _dumpSlicePause();
            ok = publishOnce();
        } else {
            DLOG_WARN("MQTT",
                      "Broker reconnect failed during dump topic=%s",
                      topic);
        }
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
        _mqtt.loop();
    }
    bool ok = publishOnce();
    if (!ok && _mqtt.connected()) {
        _mqtt.loop();
        _dumpSlicePause();
        ok = publishOnce();
    }
    if (!ok) {
        const int mqttState = _mqtt.state();
        const bool wasConnected = _mqtt.connected();
        DLOG_WARN("MQTT",
                  "Publish retry path topic=%s state=%d connected=%d",
                  topic ? topic : "(null)",
                  mqttState,
                  wasConnected ? 1 : 0);

        _mqtt.disconnect();
        _wifiClient.stop();
        _dumpSlicePause();

        if (_connectBroker()) {
            DLOG_INFO("MQTT", "Broker reconnected during dump");
            _mqtt.loop();
            _dumpSlicePause();
            ok = publishOnce();
        } else {
            DLOG_WARN("MQTT",
                      "Broker reconnect failed during dump topic=%s",
                      topic ? topic : "(null)");
        }
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
        STORAGE.isReady() ? STORAGE.getAuthoritativePendingEventCount() : 0;
    const uint32_t sessionPending =
        STORAGE.isReady() ? STORAGE.getSessionPendingEventCount() : 0;
    const bool backlogTrusted = STORAGE.isPendingEventCountAuthoritative();

    _queuedRecords = static_cast<int>(totalPending);
    if (totalPending == 0) {
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
                                       JsonDocument& doc,
                                       QueueMetric metric) {
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

    _noteQueuedRecord(metric);
    return result.eventId;
}

void MQTTManager::_noteQueuedRecord(QueueMetric metric) {
    const uint32_t totalPending =
        STORAGE.isReady() ? STORAGE.getAuthoritativePendingEventCount() : 0U;
    const bool backlogTrusted = STORAGE.isPendingEventCountAuthoritative();
    _queuedRecords = static_cast<int>(totalPending);
    STATE_WRITE_BEGIN();
    g_state.sessionFilesPending = static_cast<int>(totalPending);
    g_state.kaliSyncAvailable = backlogTrusted && (totalPending > 0);
    g_state.kaliSyncPending = backlogTrusted && (totalPending > 0);
    switch (metric) {
        case QUEUE_METRIC_PROBES:
            g_state.sessionProbes++;
            break;
        case QUEUE_METRIC_DEVICES:
            g_state.sessionDevices++;
            break;
        case QUEUE_METRIC_DRONES:
            g_state.sessionDrones++;
            break;
        case QUEUE_METRIC_PMKIDS:
            g_state.sessionPMKIDs++;
            break;
        case QUEUE_METRIC_NONE:
        default:
            break;
    }
    STATE_WRITE_END();
}

bool MQTTManager::queueProbe(const char* mac, const char* ssid,
                              int8_t rssi, uint8_t channel,
                              const char* ieFingerprint) {
    JsonDocument doc;
    _prepareQueuedEvent(doc);
    doc["mac"]           = mac;
    doc["probed_ssid"]   = ssid ? ssid : "";
    doc["is_broadcast"]  = (!ssid || ssid[0] == '\0') ? 1 : 0;
    doc["rssi"]          = rssi;
    doc["channel"]       = channel;
    doc["ie_fingerprint"]= ieFingerprint ? ieFingerprint : "";
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

void MQTTManager::queueDevice(const char* mac,
                               const char* ieFingerprint,
                               const char* probeSetHash,
                               int8_t rssi, bool isRandomMAC) {
    JsonDocument doc;
    _prepareQueuedEvent(doc);
    doc["mac"]            = mac;
    doc["ie_fingerprint"] = ieFingerprint ? ieFingerprint : "";
    doc["probe_set_hash"] = probeSetHash  ? probeSetHash  : "";
    doc["rssi"]           = rssi;
    doc["is_random_mac"]  = isRandomMAC ? 1 : 0;
    doc["source"]         = "spectre_field";
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
    _appendSyncEvent("drone", doc, QUEUE_METRIC_DRONES);
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
        _appendSyncEvent("pmkid", doc, QUEUE_METRIC_PMKIDS);
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
    _appendSyncEvent("event", doc, QUEUE_METRIC_NONE);
}

void MQTTManager::noteExternalQueuedRecord() {
    _noteQueuedRecord(QUEUE_METRIC_NONE);
}

// ── Timestamp ─────────────────────────────────────────────────

void MQTTManager::_timestamp(char* buf, int len) {
    if (!TIME_SVC.formatNowIso(buf, len)) {
        if (buf && len > 0) {
            buf[0] = '\0';
        }
    }
}

