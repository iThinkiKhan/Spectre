
#include "MQTTManager.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <cstdlib>
#include <vector>
#include "../config.h"
#include "../data/Schema.h"
#include "../core/EventBus.h"
#include "../core/NotifTypes.h"
#include "../core/DebugLog.h"
#include "../core/RuntimeContracts.h"
#include "../core/CrashBreadcrumb.h"
#include "RadioArbiter.h"
#include "SettingsManager.h"
#include "StorageManager.h"
#include "TimeService.h"


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
static constexpr uint8_t  kFetchBatchSize          = MQTT_DUMP_FETCH_BATCH_SIZE;
static constexpr uint16_t kDurableCheckpointEveryN = MQTT_DUMP_CHECKPOINT_EVERY_N;

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
    _mqtt.setSocketTimeout(60);

    _migrateLegacyQueueFiles();
    _refreshPendingCount();

    // If absurdly large, log but do not clear automatically
    if (_queuedRecords > 450) {
        DLOG_WARN("MQTT", "Backlog large: %d records", _queuedRecords);
    } else {
        DLOG_INFO("MQTT", "Backlog loaded: %d records", _queuedRecords);
    }

}

void MQTTManager::tick() {
    // Dump slices now run inline on TaskHardware, so only service the MQTT
    // client from the idle/non-dumping path.
    if (_state != MQTT_DUMPING && _mqtt.connected()) {
        _mqtt.loop();
    }
    _runStateMachine();
}

bool MQTTManager::uploadLeaseReady(bool force) const {
    if (_uploadPausedByMission()) {
        return false;
    }
    return force ||
           (_queuedRecords >= MQTT_UPLOAD_READY_THRESHOLD &&
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

    _uploadLeaseHoldMs = _calcUploadLeaseMs(_queuedRecords);

    bool granted = RADIO_ARB.requestUploadLease(
        _uploadLeaseHoldMs,
        force ? "forced_dump" : "threshold_dump",
        force);

    if (granted) {
        _wifiConnectStarted = false;
        _dumpCtx = DumpContext{};
        _uploadStartStackWatermarkBytes = _currentTaskStackWatermarkBytes();

        if (!STORAGE.prepareUploadIndexForUpload(_uploadLeaseHoldMs)) {
            DLOG_WARN("MQTT", "Upload index not ready; using spool scan fallback");
        }

        // Defer LittleFS watermark flushes until the radio is paused at
        // end-of-dump. Per-ack "w" opens during an active-radio window
        // have been observed to brown the rail.
        STORAGE.beginUploadBatch();

        _state = MQTT_CONNECTING_WIFI;
        _stateEnteredMs = millis();
        _bleTriggered = force;
        _setUploadUiState(true, "WIFI", 0, static_cast<uint32_t>(_queuedRecords), true);

        DLOG_INFO("MQTT", "Upload lease granted — %d records, lease=%lus",
                  _queuedRecords,
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
            _setUploadUiState(true, "WIFI", 0, static_cast<uint32_t>(_queuedRecords), true);

            if (WiFi.status() == WL_CONNECTED) {
                DLOG_INFO("MQTT", "WiFi connected, connecting broker");
                _state = MQTT_CONNECTING_BROKER;
                _stateEnteredMs = millis();
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

            if (elapsed > MQTT_CONNECT_TIMEOUT_MS) {
                DLOG_WARN("MQTT", "WiFi timeout");
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
            _setUploadUiState(true, "BROKER", 0, static_cast<uint32_t>(_queuedRecords), true);
            if (_connectBroker()) {
                DLOG_INFO("MQTT", "Broker connected, dumping");
                _startDumpPlan();
                _state = MQTT_DUMPING;
                _stateEnteredMs = millis();
            } else if (elapsed > MQTT_CONNECT_TIMEOUT_MS) {
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
                              static_cast<uint32_t>(_queuedRecords),
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

        case MQTT_DONE:
            _setUploadUiState(false, "", 0, 0, false);
            RADIO_ARB.release(RADIO_WIFI_UPLOAD, "dump_complete", false);
            DLOG_INFO("MQTT", "Dump complete");
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
            crashBreadcrumbClear(CrashPhase::UPLOAD_FLUSH);
            crashBreadcrumbClear(CrashPhase::MQTT_DUMPING);
            // Return to promiscuous after sync
            RADIO_ARB.ensureDefaultCapture("mqtt_done");
            _logUploadStackWatermark("done");
            _state = MQTT_IDLE;

            // Note: session data NOT cleared here
            // Only cleared on explicit debrief screen long press
            break;

        case MQTT_FAILED:
            _uploadBackoffUntilMs = millis() + MQTT_FAILED_BACKOFF_MS;
            _setUploadUiState(true, "FAILED",
                  static_cast<uint32_t>(_lastPublished),
                  static_cast<uint32_t>(_queuedRecords),
                  false);
            RADIO_ARB.release(RADIO_WIFI_UPLOAD, "dump_failed", false);
            DLOG_WARN("MQTT", "Dump failed, backoff %lu ms",
                      static_cast<unsigned long>(MQTT_FAILED_BACKOFF_MS));
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
            // Return to promiscuous after sync
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
    WiFi.begin(network->ssid, network->password);
    DLOG_INFO("MQTT", "Connecting to upload network: %s",
              network->ssid);
    return true;
}

bool MQTTManager::_connectBroker() {
    if (_mqtt.connected()) return true;

    // PubSubClient may believe the MQTT session is gone while the underlying
    // WiFiClient still holds a half-open TCP socket. Start each broker connect
    // attempt from a clean transport so reconnects do not accumulate state.
    _wifiClient.stop();

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

    if (brokerUser[0]) {
        return _mqtt.connect(clientID, brokerUser, brokerPass);
    }

    return _mqtt.connect(clientID);
}

void MQTTManager::_startDumpPlan() {
    _lastPublished = 0;
    _lastFailed = 0;

    _dumpCtx = DumpContext{};
    _dumpCtx.phase = DUMP_PHASE_HEALTH;
    _dumpCtx.sessionIndex = 0;
    _dumpCtx.sinceId = 0;
    _dumpCtx.phaseStarted = false;
    // Snapshot for diagnostics — no hard cap; upload is bounded by lease time.
    _dumpCtx.maxEventsThisLease =
        STORAGE.isReady() ? static_cast<uint32_t>(STORAGE.getPendingEventCount()) : 0U;

    crashCheckpoint(CrashPhase::MQTT_DUMPING,
                    static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                    static_cast<uint32_t>(_queuedRecords));

    DLOG_INFO("MQTT", "Dump start — pendingUpload=%d lease=%lus leaseMaxEvents=%u",
              _queuedRecords,
              static_cast<unsigned long>(_uploadLeaseHoldMs / 1000UL),
              static_cast<unsigned>(_dumpCtx.maxEventsThisLease));

    _setUploadUiState(true, "UPLOADING", 0,
                      static_cast<uint32_t>(_queuedRecords), true);
}

bool MQTTManager::_runDumpSlice() {
    switch (_dumpCtx.phase) {
        case DUMP_PHASE_IDLE:
            return true;

        case DUMP_PHASE_HEALTH:
            _dumpCtx.phaseStarted = true;
            DLOG_INFO("MQTT", "Health publish begin");
            _publishHealth();
            DLOG_INFO("MQTT", "Health publish end");
            _dumpCtx.phase = DUMP_PHASE_EVENTS;
            _dumpCtx.phaseStarted = false;
            return false;

        case DUMP_PHASE_EVENTS: {
            // sessionIds lives in _dumpCtx so _dumpCtx = {} at dump start
            // destructs the Strings and frees the vector's backing allocation.
            std::vector<String>& sessionIds = _dumpCtx.sessionIds;

            if (!_dumpCtx.phaseStarted) {
                sessionIds.clear();
                const uint32_t listT0 = millis();
                STORAGE.listEventSessions(sessionIds);
                const uint32_t listDt = millis() - listT0;
                DLOG_WARN("MQTT", "Upload sessions listed=%u ms=%lu",
                          static_cast<unsigned>(sessionIds.size()),
                          static_cast<unsigned long>(listDt));
                _dumpCtx.sessionIndex = 0;
                _dumpCtx.sinceId = 0;
                _dumpCtx.phaseStarted = true;
            }

            const uint32_t sliceStartMs = millis();
            uint8_t recordsThisSlice = 0;
            JsonDocument publishDoc;

            while (_dumpCtx.sessionIndex < sessionIds.size()) {
                // Time budget
                if ((millis() - sliceStartMs) >= kDumpSliceBudgetMs) {
                    _mqtt.loop();
                    return false;
                }

                // Record limit per slice
                if (recordsThisSlice >= kDumpMaxRecordsPerSlice) {
                    _mqtt.loop();
                    return false;
                }

                const String& sessionId = sessionIds[_dumpCtx.sessionIndex];

                // ── FETCH PHASE ───────────────────────────────────────────
                // When the cache is empty scan storage once and yield.
                // Subsequent calls drain the cached records without re-scanning.
                if (_dumpCtx.cachedBatchCount == 0) {
                    if (!_dumpCtx.sinceIdInitialized) {
                        _dumpCtx.sinceId =
                            STORAGE.getLastUploadedEventId(sessionId.c_str());
                        _dumpCtx.sinceIdInitialized = true;
                    }

                    _refreshUploadLease("mqtt_batch", _uploadLeaseHoldMs);
                    if (_mqtt.connected()) {
                        _mqtt.loop();
                        _dumpSlicePause();
                    } else {
                        DLOG_WARN("MQTT",
                                  "Broker disconnected before batch fetch — reconnecting");
                        if (!_connectBroker()) {
                            DLOG_WARN("MQTT",
                                      "Mid-dump reconnect failed at batch boundary");
                            _lastFailed++;
                            _dumpCtx.phase = DUMP_PHASE_FAILED;
                            return true;
                        }
                    }

                    if (!_dumpCtx.firstEventFetchLogged) {
                        DLOG_INFO("MQTT",
                                  "First event batch fetch session=%s since=%lu",
                                  sessionId.c_str(),
                                  static_cast<unsigned long>(_dumpCtx.sinceId));
                        _dumpCtx.firstEventFetchLogged = true;
                    }

                    _dumpCtx.cachedBatch.clear();
                    const uint32_t fetchT0 = millis();
                    const bool fetchOk = STORAGE.getUploadEventBatchForSession(
                        sessionId.c_str(), _dumpCtx.sinceId,
                        kFetchBatchSize, _dumpCtx.cachedBatch);
                    const uint32_t fetchDt = millis() - fetchT0;

                    if (fetchDt >= kUploadFetchSlowWarnMs) {
                        DLOG_WARN("MQTT",
                                  "Upload batch fetch slow session=%s since=%lu ms=%lu",
                                  sessionId.c_str(),
                                  static_cast<unsigned long>(_dumpCtx.sinceId),
                                  static_cast<unsigned long>(fetchDt));
                    } else if (fetchDt >= kUploadFetchSlowInfoMs) {
                        DLOG_INFO("MQTT",
                                  "Upload batch fetch slower than usual session=%s since=%lu ms=%lu",
                                  sessionId.c_str(),
                                  static_cast<unsigned long>(_dumpCtx.sinceId),
                                  static_cast<unsigned long>(fetchDt));
                    }

                    if (!fetchOk) {
                        _lastFailed++;
                        DLOG_WARN("MQTT", "Failed to read upload batch for session %s",
                                  sessionId.c_str());
                        _dumpCtx.phase = DUMP_PHASE_FAILED;
                        return true;
                    }

                    JsonArray arr = _dumpCtx.cachedBatch.as<JsonArray>();
                    if (arr.isNull() || arr.size() == 0) {
                        // Session exhausted — advance and yield
                        _dumpCtx.sessionIndex++;
                        _dumpCtx.sinceId = 0;
                        _dumpCtx.sinceIdInitialized = false;
                        _dumpCtx.cachedBatchIndex = 0;
                        _dumpCtx.cachedBatchCount = 0;
                        _dumpSlicePause();
                        return false;
                    }

                    _dumpCtx.cachedBatchIndex = 0;
                    _dumpCtx.cachedBatchCount =
                        static_cast<uint16_t>(arr.size());

                    // Yield after the expensive scan; next call drains cache
                    return false;
                }

                // ── PROCESS PHASE ─────────────────────────────────────────
                // Drain one record from the cached batch per while-iteration.
                JsonArray batch = _dumpCtx.cachedBatch.as<JsonArray>();
                if (batch.isNull() ||
                    _dumpCtx.cachedBatchIndex >= _dumpCtx.cachedBatchCount) {
                    // Defensive: stale cache — reset and re-fetch next call
                    _dumpCtx.cachedBatchIndex = 0;
                    _dumpCtx.cachedBatchCount = 0;
                    continue;
                }

                JsonObject record =
                    batch[_dumpCtx.cachedBatchIndex].as<JsonObject>();
                if (record.isNull()) {
                    _lastFailed++;
                    DLOG_WARN("MQTT",
                              "Upload batch record malformed session=%s since=%lu",
                              sessionId.c_str(),
                              static_cast<unsigned long>(_dumpCtx.sinceId));
                    _dumpCtx.phase = DUMP_PHASE_FAILED;
                    return true;
                }

                publishDoc.clear();

                const uint32_t eventId = record["id"] | 0U;
                if (eventId == 0) {
                    _lastFailed++;
                    DLOG_WARN("MQTT",
                              "Upload batch record missing event id session=%s since=%lu",
                              sessionId.c_str(),
                              static_cast<unsigned long>(_dumpCtx.sinceId));
                    _dumpCtx.phase = DUMP_PHASE_FAILED;
                    return true;
                }

                const char* type = record["type"] | "event";
                const String topic = _mqttTopicForEventType(type);

                if (!_dumpCtx.firstEventFetchedOkLogged) {
                    DLOG_INFO("MQTT",
                              "First event fetched ok eventId=%lu type=%s",
                              static_cast<unsigned long>(eventId),
                              type);
                    _dumpCtx.firstEventFetchedOkLogged = true;
                }

                if (!_dumpCtx.firstEventValidatedLogged) {
                    DLOG_INFO("MQTT", "First event validated");
                    _dumpCtx.firstEventValidatedLogged = true;
                }

                _copyEventRecordForPublish(record, publishDoc);

                if (!_dumpCtx.firstEventDocBuiltLogged) {
                    DLOG_INFO("MQTT", "First event doc built");
                    _dumpCtx.firstEventDocBuiltLogged = true;
                }

                if (!_dumpCtx.firstEventPublishBeginLogged) {
                    DLOG_INFO("MQTT", "First event publish begin");
                    _dumpCtx.firstEventPublishBeginLogged = true;
                }

                _refreshUploadLease("mqtt_publish", _uploadLeaseHoldMs);

                if (!_mqtt.connected()) {
                    DLOG_WARN("MQTT",
                              "Broker disconnected before event=%lu — reconnecting",
                              static_cast<unsigned long>(eventId));
                    if (!_connectBroker()) {
                        DLOG_WARN("MQTT",
                                  "Reconnect failed before publish event=%lu",
                                  static_cast<unsigned long>(eventId));
                        _lastFailed++;
                        _dumpCtx.phase = DUMP_PHASE_FAILED;
                        return true;
                    }
                    _mqtt.loop();
                }

                if (!_publishJson(topic.c_str(), publishDoc, false, eventId)) {
                    const bool samePoisonRecord =
                        (_lastPoisonEventId == eventId &&
                         _lastPoisonSessionId == sessionId);
                    _lastPoisonEventId = eventId;
                    _lastPoisonSessionId = sessionId;
                    _lastPoisonEventFailures = samePoisonRecord
                        ? static_cast<uint8_t>(_lastPoisonEventFailures + 1)
                        : 1;

                    const size_t payloadBytes = measureJson(publishDoc);
                    const char* payloadSessionId = publishDoc["session_id"] | "";
                    const char* payloadTs = publishDoc["ts"] | "";
                    const char* payloadSensor = publishDoc["sensor"] | "";
                    DLOG_WARN("MQTT",
                              "Publish failed for session=%s event=%lu type=%s"
                              " topic=%s payload=%u sessionField=%d tsField=%d sensorField=%d"
                              " mqttState=%d connected=%d poisonFails=%u/%u",
                              sessionId.c_str(),
                              static_cast<unsigned long>(eventId),
                              type,
                              topic.c_str(),
                              static_cast<unsigned>(payloadBytes),
                              payloadSessionId[0] ? 1 : 0,
                              payloadTs[0] ? 1 : 0,
                              payloadSensor[0] ? 1 : 0,
                              _mqtt.state(),
                              _mqtt.connected() ? 1 : 0,
                              static_cast<unsigned>(_lastPoisonEventFailures),
                              static_cast<unsigned>(MQTT_POISON_FAIL_LIMIT));

                    if (_lastPoisonEventFailures >= MQTT_POISON_FAIL_LIMIT) {
                        if (STORAGE.markEventUploaded(eventId, sessionId.c_str())) {
                            DLOG_WARN("MQTT",
                                      "Quarantined poison event session=%s event=%lu after %u failures",
                                      sessionId.c_str(),
                                      static_cast<unsigned long>(eventId),
                                      static_cast<unsigned>(_lastPoisonEventFailures));
                            _dumpCtx.sinceId = eventId;
                            _dumpCtx.cachedBatchIndex++;
                            if (_dumpCtx.cachedBatchIndex >= _dumpCtx.cachedBatchCount) {
                                _dumpCtx.cachedBatchIndex = 0;
                                _dumpCtx.cachedBatchCount = 0;
                            }
                            _lastPoisonEventId = 0;
                            _lastPoisonSessionId = "";
                            _lastPoisonEventFailures = 0;
                            continue;
                        }

                        DLOG_WARN("MQTT",
                                  "Quarantine mark failed for session=%s event=%lu",
                                  sessionId.c_str(),
                                  static_cast<unsigned long>(eventId));
                    }

                    _lastFailed++;
                    _dumpCtx.phase = DUMP_PHASE_FAILED;
                    return true;
                }

                if (!_dumpCtx.firstEventPublishEndLogged) {
                    DLOG_INFO("MQTT", "First event publish end");
                    _dumpCtx.firstEventPublishEndLogged = true;
                }

                if (_mqtt.connected()) {
                    _mqtt.loop();
                }
                _dumpSlicePause();

                if (!STORAGE.markEventUploaded(eventId, sessionId.c_str())) {
                    _lastFailed++;
                    DLOG_WARN("MQTT",
                              "Failed to mark uploaded event=%lu session=%s",
                              static_cast<unsigned long>(eventId),
                              sessionId.c_str());
                    _dumpCtx.phase = DUMP_PHASE_FAILED;
                    return true;
                }

                _dumpCtx.sinceId = eventId;
                _dumpCtx.cachedBatchIndex++;
                if (_dumpCtx.cachedBatchIndex >= _dumpCtx.cachedBatchCount) {
                    // Cache exhausted: next iteration fetches the next batch
                    _dumpCtx.cachedBatchIndex = 0;
                    _dumpCtx.cachedBatchCount = 0;
                }
                _lastPublished++;
                recordsThisSlice++;
                _refreshUploadLease("mqtt_progress", _uploadLeaseHoldMs);

                _setUploadUiState(true, "UPLOADING",
                                  static_cast<uint32_t>(_lastPublished),
                                  static_cast<uint32_t>(_queuedRecords),
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
                    STORAGE.flushUploadCheckpoint();
                }

                if ((_lastPublished % 25) == 0) {
                    DLOG_INFO("MQTT",
                      "Dump progress pub=%d fail=%d session=%u/%u since=%lu leaseMaxEvents=%u",
                              _lastPublished,
                              _lastFailed,
                              static_cast<unsigned>(_dumpCtx.sessionIndex),
                              static_cast<unsigned>(sessionIds.size()),
                              static_cast<unsigned long>(_dumpCtx.sinceId),
                              static_cast<unsigned>(_dumpCtx.maxEventsThisLease));
                }
            }

            STORAGE.refreshStorageUiState();
            DLOG_INFO("MQTT", "Published=%d Failed=%d",
                      _lastPublished, _lastFailed);

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
                                  static_cast<uint32_t>(_queuedRecords),
                                  true);
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

            DLOG_INFO("MQTT", "Dump done — pub=%d fail=%d remain=%d",
                      _lastPublished, _lastFailed, _queuedRecords);

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
    // Don't disconnect WiFi — WiFiManager may need it
    // WiFiManager handles its own WiFi state
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

void MQTTManager::_publishDebugLogs() {
    // Flush RAM buffer first so latest lines make it to disk.
    DebugLog::flush();

    int failed = 0;
    if (!_publishFileInChunks("log", "/logs/debug.log", "debug.log", 512)) {
        failed++;
    }
    if (LittleFS.exists("/logs/debug.log.1")) {
        if (!_publishFileInChunks("log", "/logs/debug.log.1", "debug.log.1", 512)) {
            failed++;
        }
    }

    if (failed > 0) {
        _lastFailed += failed;
        DLOG_WARN("MQTT", "Debug log publish failures=%d", failed);
    }
}

bool MQTTManager::_publishFileInChunks(const char* topicSuffix,
                                       const char* filePath,
                                       const char* fileName,
                                       size_t chunkSize) {
    if (!_mqtt.connected() || !filePath || !fileName || chunkSize == 0) {
        return false;
    }

    if (!LittleFS.exists(filePath)) {
        DLOG_INFO("MQTT", "Log file missing: %s", filePath);
        return true;  // not an upload failure
    }

    File f = LittleFS.open(filePath, "r");
    if (!f || f.isDirectory()) {
        DLOG_WARN("MQTT", "Failed to open log file: %s", filePath);
        return false;
    }

    const size_t fileSize = f.size();
    const size_t totalChunks = (fileSize == 0) ? 1 : ((fileSize + chunkSize - 1) / chunkSize);
    const String topic = _mqttTopicFor(topicSuffix);

    if (fileSize == 0) {
        JsonDocument emptyDoc;
        emptyDoc["sensor"] = MQTT_SENSOR_ID;
        emptyDoc["file"] = fileName;
        emptyDoc["chunk_index"] = 0;
        emptyDoc["chunk_count"] = 1;
        emptyDoc["size_bytes"] = 0;
        emptyDoc["content"] = "";
        bool ok = _publishJson(topic.c_str(), emptyDoc, false);
        f.close();
        return ok;
    }

    std::unique_ptr<char[]> buf(new char[chunkSize + 1]);
    if (!buf) {
        f.close();
        DLOG_ERROR("MQTT", "Log chunk alloc failed");
        return false;
    }

    size_t chunkIndex = 0;
    while (f.available() && _mqtt.connected()) {
        const size_t n = f.readBytes(buf.get(), chunkSize);
        buf[n] = '\0';

        JsonDocument doc;
        doc["sensor"] = MQTT_SENSOR_ID;
        doc["file"] = fileName;
        doc["chunk_index"] = chunkIndex;
        doc["chunk_count"] = totalChunks;
        doc["size_bytes"] = fileSize;
        doc["content"] = buf.get();

        if (!_publishJson(topic.c_str(), doc, false)) {
            DLOG_WARN("MQTT",
                      "Log chunk publish failed file=%s chunk=%u/%u",
                      fileName,
                      static_cast<unsigned>(chunkIndex + 1),
                      static_cast<unsigned>(totalChunks));
            f.close();
            return false;
        }

        chunkIndex++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    f.close();

    DLOG_INFO("MQTT",
              "Published log file=%s chunks=%u size=%u",
              fileName,
              static_cast<unsigned>(chunkIndex),
              static_cast<unsigned>(fileSize));
    return true;
}

bool MQTTManager::_purgeTransientFiles() {
    bool ok = true;
    _setUploadUiState(true,
                      (_lastFailed == 0) ? "DONE" : "FAILED",
                      static_cast<uint32_t>(_lastPublished),
                      static_cast<uint32_t>(_queuedRecords),
                      true);
    auto shouldDelete = [](const char* dirPath, const String& name) -> bool {
        if (name.length() == 0) {
            return false;
        }

        if (strcmp(dirPath, PATH_EVENTS) == 0) {
            // Remove only transient upload snapshots
            return name.endsWith(".upload.jsonl");
        }

        if (strcmp(dirPath, PATH_LOGS) == 0) {
            // Logs are offloaded over MQTT, so clear them locally
            return name == "debug.log" || name == "debug.log.1";
        }

        if (strcmp(dirPath, PATH_PMKID_DIR) == 0 ||
            strcmp(dirPath, "/pmkid") == 0) {
            // Treat PMKID artifacts as transient
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

bool MQTTManager::_publishJson(const char* topic,
                                JsonDocument& doc,
                                bool retained,
                                uint32_t debugEventId) {
    char buf[1536];
    const size_t measuredLen = measureJson(doc);
    if (measuredLen == 0) {
        DLOG_WARN("MQTT", "Serialize measure failed topic=%s retained=%d",
                  topic ? topic : "(null)",
                  retained ? 1 : 0);
        return false;
    }

    if (measuredLen >= sizeof(buf)) {
        DLOG_WARN("MQTT",
                  "Payload exceeds publish buffer topic=%s bytes=%u buf=%u retained=%d",
                  topic ? topic : "(null)",
                  static_cast<unsigned>(measuredLen),
                  static_cast<unsigned>(sizeof(buf) - 1),
                  retained ? 1 : 0);
        return false;
    }

    size_t len = serializeJson(doc, buf, sizeof(buf));
    if (len == 0 || len != measuredLen) {
        DLOG_WARN("MQTT",
                  "Serialize mismatch topic=%s measured=%u serialized=%u retained=%d",
                  topic ? topic : "(null)",
                  static_cast<unsigned>(measuredLen),
                  static_cast<unsigned>(len),
                  retained ? 1 : 0);
        return false;
    }

    // Drain broker ACKs before writing so the TCP send buffer never backs up.
    if (_mqtt.connected()) {
        _mqtt.loop();
    }
    bool ok = _mqtt.publish(topic, reinterpret_cast<uint8_t*>(buf), len, retained);
    if (!ok && _mqtt.connected()) {
        _mqtt.loop();
        _dumpSlicePause();
        ok = _mqtt.publish(topic, reinterpret_cast<uint8_t*>(buf), len, retained);
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
            ok = _mqtt.publish(topic, reinterpret_cast<uint8_t*>(buf), len, retained);
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
                  static_cast<unsigned>(len),
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

        // Optional: remove empty legacy dir
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

    _queuedRecords = static_cast<int>(totalPending);
    STATE_WRITE_BEGIN();
    g_state.sessionFilesPending = static_cast<int>(totalPending);
    g_state.kaliSyncAvailable = (totalPending > 0);
    g_state.kaliSyncPending = (totalPending > 0);
    if (refreshDebriefMirror) {
        g_state.exportLastPending = sessionPending;
    }
    STATE_WRITE_END();
}

// ── Queue write helpers ───────────────────────────────────────

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

uint32_t MQTTManager::_appendQueuedEvent(const char* eventType,
                                         JsonDocument& doc,
                                         QueueMetric metric) {
    static uint32_t s_suppressedDuplicateCount = 0;
    static uint32_t s_lastSuppressedLogMs = 0;

    const AppendEventResult result =
        STORAGE.appendEventDetailed(eventType, doc.as<JsonObjectConst>());

    if (!result.ok()) {
        switch (result.status) {
            case APPEND_SUPPRESSED_DUPLICATE: {
                s_suppressedDuplicateCount++;
                const uint32_t now = millis();
                if ((now - s_lastSuppressedLogMs) >= 10000UL) {
                    DLOG_DEBUG("MQTT", "append duplicate suppressed count=%lu",
                               static_cast<unsigned long>(s_suppressedDuplicateCount));
                    s_suppressedDuplicateCount = 0;
                    s_lastSuppressedLogMs = now;
                }
                break;
            }

            case APPEND_DROPPED_POLICY:
                DLOG_WARN("MQTT", "Dropped %s event by storage policy",
                          eventType ? eventType : "unknown");
                break;

            case APPEND_FAILED_PARSE:
                DLOG_WARN("MQTT", "Failed to append %s event: parse",
                          eventType ? eventType : "unknown");
                break;

            case APPEND_FAILED_NOT_READY:
                DLOG_WARN("MQTT", "Failed to append %s event: storage not ready",
                          eventType ? eventType : "unknown");
                break;

            case APPEND_FAILED_NO_SESSION:
                DLOG_WARN("MQTT", "Failed to append %s event: no session",
                          eventType ? eventType : "unknown");
                break;

            case APPEND_FAILED_IO:
                DLOG_WARN("MQTT", "Failed to append %s event: I/O",
                          eventType ? eventType : "unknown");
                break;

            case APPEND_FAILED_INVALID:
            default:
                DLOG_WARN("MQTT", "Failed to append %s event",
                          eventType ? eventType : "unknown");
                break;
        }
        return 0;
    }

    _noteQueuedRecord(metric);
    return result.eventId;
}

void MQTTManager::_noteQueuedRecord(QueueMetric metric) {
    _queuedRecords++;
    STATE_WRITE_BEGIN();
    g_state.sessionFilesPending = _queuedRecords;
    g_state.kaliSyncAvailable = true;
    g_state.kaliSyncPending = true;
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

void MQTTManager::queueProbe(const char* mac, const char* ssid,
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
    _appendQueuedEvent("probe", doc, QUEUE_METRIC_PROBES);
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
    _appendQueuedEvent("device", doc, QUEUE_METRIC_DEVICES);
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
    _appendQueuedEvent("drone", doc, QUEUE_METRIC_DRONES);
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

    // Convert SSID to hex
    const size_t ssidLen = strnlen(ssid ? ssid : "", 32);
    for (size_t i = 0; i < ssidLen; i++) {
        snprintf(ssidHex + i*2, 3, "%02x", (uint8_t)ssid[i]);
    }
    ssidHex[ssidLen * 2] = '\0';

    // Format PMKID as hex
    char pmkidHex[33];
    for (int i = 0; i < 16; i++) {
        snprintf(pmkidHex + i*2, 3, "%02x", pmkid[i]);
    }
    pmkidHex[32] = '\0';

    // Hashcat format line
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
        _appendQueuedEvent("pmkid", doc, QUEUE_METRIC_PMKIDS);
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
    _appendQueuedEvent("event", doc, QUEUE_METRIC_NONE);
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

