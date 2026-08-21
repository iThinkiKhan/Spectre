#include "PhoneOffloadManager.h"

#include <ArduinoJson.h>
#include <string.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include "../SecretsConfig.h"
#include "../config.h"
#include "../core/DebugLog.h"
#include "../core/StorageExclusiveWindow.h"
#include "../data/Schema.h"
#include "RadioArbiter.h"
#include "SettingsManager.h"
#include "StorageManager.h"

namespace {
constexpr const char* TAG = "OFFLOAD";
constexpr uint32_t OFFLOAD_STALE_MS = 120000UL;
constexpr uint32_t OFFLOAD_INDEX_BUDGET_MS = 45000UL;
constexpr uint32_t OFFLOAD_CHECKPOINT_MS = 60000UL;
constexpr uint16_t OFFLOAD_CHECKPOINT_RECORDS = 128;
constexpr uint32_t WIFI_BULK_MAGIC = 0x53504231UL;
constexpr uint32_t WIFI_BULK_ACK_MAGIC = 0x41434b31UL;
constexpr size_t WIFI_BULK_BATCH_MAX_BYTES = 64U * 1024U;
constexpr uint16_t WIFI_BULK_BATCH_MAX_RECORDS = 64;
WiFiClient s_wifiBulkClient;
bool s_wifiBulkEventsInstalled = false;
IPAddress s_wifiBulkPhoneIp;

constexpr uint32_t WIFI_BULK_RESUME_MAGIC = 0x53505231UL; // SPR1
struct WifiBulkResumeState {
    uint32_t magic;
    uint32_t checksum;
    uint8_t transferId;
    uint16_t port;
    char ssid[33];
    char password[64];
    uint8_t token[32];
};
RTC_NOINIT_ATTR WifiBulkResumeState s_wifiBulkResume;

uint32_t wifiBulkResumeChecksum(const WifiBulkResumeState& state) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state.transferId);
    const size_t len = sizeof(state) - offsetof(WifiBulkResumeState, transferId);
    uint32_t hash = 2166136261UL;
    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 16777619UL;
    }
    return hash;
}

bool wifiBulkResumeValid() {
    return s_wifiBulkResume.magic == WIFI_BULK_RESUME_MAGIC &&
           s_wifiBulkResume.ssid[0] != '\0' &&
           s_wifiBulkResume.password[0] != '\0' &&
           s_wifiBulkResume.port != 0 &&
           s_wifiBulkResume.checksum == wifiBulkResumeChecksum(s_wifiBulkResume);
}

void installWifiBulkEvents() {
    if (s_wifiBulkEventsInstalled) return;
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        if (event == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED) {
            s_wifiBulkPhoneIp = IPAddress(info.wifi_ap_staipassigned.ip.addr);
            DLOG_INFO(TAG, "wifi bulk phone address=%s",
                      s_wifiBulkPhoneIp.toString().c_str());
        } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
            DLOG_WARN(TAG, "wifi bulk phone disconnected reason=%u",
                      static_cast<unsigned>(info.wifi_ap_stadisconnected.reason));
            s_wifiBulkPhoneIp = IPAddress();
        }
    });
    s_wifiBulkEventsInstalled = true;
}

void appendBe16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8)); out.push_back(static_cast<uint8_t>(value));
}
void appendBe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24)); out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8)); out.push_back(static_cast<uint8_t>(value));
}
uint16_t readBe16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t readBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

String topicForEventType(const char* type) {
    const RuntimeSettings* settings =
        SETTINGS.isReady() ? &SETTINGS.get() : nullptr;
    const char* base =
        (settings && settings->mqttTopicBase[0])
            ? settings->mqttTopicBase
            : SPECTRE_MQTT_TOPIC_BASE;

    const char* suffix = "event";
    if (type && type[0]) {
        if (strcmp(type, "subghz") == 0) suffix = "subghz";
        else if (strcmp(type, "probe") == 0) suffix = "probe";
        else if (strcmp(type, "device") == 0) suffix = "device";
        else if (strcmp(type, "network") == 0) suffix = "network";
        else if (strcmp(type, "drone") == 0) suffix = "drone";
        else if (strcmp(type, "pmkid") == 0) suffix = "pmkid";
    }

    String topic(base);
    topic += "/";
    topic += SPECTRE_MQTT_SENSOR_ID;
    topic += "/";
    topic += suffix;
    return topic;
}

bool copyRecordForPublish(JsonObjectConst record, JsonDocument& out) {
    out.clear();
    JsonObject publish = out.to<JsonObject>();
    if (publish.isNull()) return false;

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
        publish[key].set(kv.value());
    }

    const char* isoTs = record[F_TIMESTAMP_ISO] | "";
    if (isoTs[0]) publish[F_TIMESTAMP] = isoTs;
    else publish["ts_unknown"] = 1;
    return !out.overflowed();
}
}

PhoneOffloadManager& PhoneOffloadManager::getInstance() {
    // This manager is touched by CommandDispatcher::tick() even while no
    // offload is active.  Keep the object in internal RAM: placing it in PSRAM
    // made the Samsung background-enrichment notify path panic immediately
    // after the encrypted request was sent.  Its large transfer body is cold,
    // but the manager's frequently accessed control state is not.
    static PhoneOffloadManager instance;
    return instance;
}

bool PhoneOffloadManager::begin(CmdOffloadBeginResponseV1& out) {
    out = {};
    expireIfStale();

    if (!STORAGE.isReady() || !RADIO_ARB.isOwner(RADIO_BLE_GPS)) {
        DLOG_WARN(TAG, "begin refused ready=%u owner=%s",
                  STORAGE.isReady() ? 1U : 0U,
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
        return false;
    }

    // BEGIN is idempotent. A lost response must not discard an already-built
    // index or rewind an in-progress, phone-durable transfer.
    if (_active) {
        out.version = PHONE_OFFLOAD_VERSION;
        out.transferId = _transferId;
        out.flags = _beginFlags;
        out.pendingTotal = _beginPendingTotal;
        out.indexedTotal = _beginIndexedTotal;
        _lastActivityMs = millis();
        return true;
    }

    const uint8_t prep = __atomic_load_n(&_prepState, __ATOMIC_ACQUIRE);
    DLOG_INFO(TAG,
              "begin request prep=%u resident=%u active=%u",
              static_cast<unsigned>(prep),
              STORAGE.isUploadIndexResident() ? 1U : 0U,
              _active ? 1U : 0U);
    if (prep == PREP_IDLE || prep == PREP_FAILED) {
        if (!startPreparation()) {
            return false;
        }
        DLOG_INFO(TAG, "begin preparing index asynchronously");
        return false;
    }
    if (prep == PREP_RUNNING) {
        return false;
    }
    if (prep != PREP_READY || !STORAGE.isUploadIndexResident()) {
        __atomic_store_n(&_prepState, PREP_IDLE, __ATOMIC_RELEASE);
        return false;
    }

    _sessions.clear();
    STORAGE.listEventSessions(_sessions);
    _sessionIndex = 0;
    _sinceId = 0;
    _sinceInitialized = false;
    clearRecord();

    _transferId = static_cast<uint8_t>(_transferId + 1U);
    if (_transferId == 0) _transferId = 1;
    _active = true;
    _lastActivityMs = millis();
    _lastAckEventId = 0;
    _acksSinceCheckpoint = 0;
    _lastCheckpointMs = _lastActivityMs;
    _transferStartMs = _lastActivityMs;
    _transferAckedBytes = 0;
    _transferAckedRecords = 0;

    // beginUploadBatch flushes/closes the RAM-spool worker's append handle.
    // The preparation window has already resumed that worker, so activation
    // needs its own short exclusive window. Calling this naked raced a Core 1
    // append with the Android BEGIN retry immediately after index completion.
    StorageExclusiveWindow activationWindow;
    _storageBatchOpen = activationWindow.begin(
        STORAGE_WINDOW_ENRICHMENT, "phone_offload_activate");
    if (_storageBatchOpen) {
        STORAGE.beginUploadBatch();
        _storageBatchOpen = STORAGE.isUploadBatchActive();
        activationWindow.end(_storageBatchOpen
                                 ? "batch_open"
                                 : "batch_open_failed");
    }
    if (!_storageBatchOpen) {
        (void)reset("batch_open_failed");
        return false;
    }

    out.version = PHONE_OFFLOAD_VERSION;
    out.transferId = _transferId;
    out.flags = STORAGE.isUploadIndexWindowTruncated()
                    ? PHONE_OFFLOAD_FLAG_INDEX_TRUNCATED
                    : 0;
    out.pendingTotal = STORAGE.getAuthoritativePendingEventCount();
    out.indexedTotal = STORAGE.getUploadIndexResidentEventCount();
    _beginFlags = out.flags;
    _beginPendingTotal = out.pendingTotal;
    _beginIndexedTotal = out.indexedTotal;

    DLOG_INFO(TAG,
              "begin transfer=%u pending=%lu indexed=%lu sessions=%u truncated=%u",
              static_cast<unsigned>(_transferId),
              static_cast<unsigned long>(out.pendingTotal),
              static_cast<unsigned long>(out.indexedTotal),
              static_cast<unsigned>(_sessions.size()),
              (out.flags & PHONE_OFFLOAD_FLAG_INDEX_TRUNCATED) ? 1U : 0U);
    return true;
}

bool PhoneOffloadManager::startPreparation() {
    if (__atomic_load_n(&_prepState, __ATOMIC_ACQUIRE) == PREP_RUNNING) {
        return true;
    }
    __atomic_store_n(&_prepAbandonRequested, false, __ATOMIC_RELEASE);
    __atomic_store_n(&_prepState, PREP_RUNNING, __ATOMIC_RELEASE);
    DLOG_INFO(TAG,
              "index preparation queued on TaskHardware internalFree=%lu largest=%lu",
              static_cast<unsigned long>(
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
              static_cast<unsigned long>(
                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    return true;
}

void PhoneOffloadManager::servicePreparation() {
    if (__atomic_load_n(&_prepState, __ATOMIC_ACQUIRE) != PREP_RUNNING) {
        return;
    }
    runPreparation();
}

void PhoneOffloadManager::runPreparation() {
    if (__atomic_exchange_n(&_prepAbandonRequested,
                            false,
                            __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&_prepState, PREP_IDLE, __ATOMIC_RELEASE);
        DLOG_INFO(TAG, "queued index preparation abandoned before start");
        return;
    }

    DLOG_INFO(TAG,
              "index preparation started on TaskHardware stackFree=%lu",
              static_cast<unsigned long>(
                  uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    StorageExclusiveWindow window;
    bool ok = window.begin(STORAGE_WINDOW_ENRICHMENT, "phone_offload_prepare");
    if (ok) {
        ok = STORAGE.prepareUploadIndexForUpload(OFFLOAD_INDEX_BUDGET_MS);
        window.end(ok ? "index_ready" : "index_failed");
    }
    bool abandoned =
        __atomic_exchange_n(&_prepAbandonRequested, false, __ATOMIC_ACQ_REL);
    if (abandoned) {
        if (ok || STORAGE.isUploadIndexResident()) {
            STORAGE.releaseUploadIndexMemory("offload_prepare_abandoned");
        }
        __atomic_store_n(&_prepState, PREP_IDLE, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&_prepState,
                         ok ? PREP_READY : PREP_FAILED,
                         __ATOMIC_RELEASE);
        // Close the narrow race where cancellation arrives after the first
        // exchange but before PREP_READY becomes visible.
        abandoned = __atomic_exchange_n(&_prepAbandonRequested,
                                        false,
                                        __ATOMIC_ACQ_REL);
        if (abandoned) {
            if (ok || STORAGE.isUploadIndexResident()) {
                STORAGE.releaseUploadIndexMemory("offload_prepare_abandoned");
            }
            __atomic_store_n(&_prepState, PREP_IDLE, __ATOMIC_RELEASE);
        }
    }
    DLOG_INFO(TAG, "index preparation complete ok=%u resident=%u",
              ok && !abandoned ? 1U : 0U,
              STORAGE.isUploadIndexResident() ? 1U : 0U);
    DLOG_INFO("STACK", "TaskHardware offload preparation watermark=%luB",
              static_cast<unsigned long>(
                  uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
}

bool PhoneOffloadManager::next(const CmdOffloadNextRequestV1& req,
                               uint8_t* out,
                               size_t cap,
                               size_t& outLen) {
    outLen = 0;
    expireIfStale();
    if (!_active || req.transferId != _transferId || !out ||
        cap < sizeof(CmdOffloadChunkV1)) {
        return false;
    }
    _lastActivityMs = millis();

    // Collapse phone commit -> device ACK -> next fetch into one round trip.
    // ACK is processed first, so Spectre never moves past data Android has not
    // durably committed.
    if (req.offset == 0 && req.ackEventId != 0) {
        CmdOffloadAckRequestV1 ackReq = {};
        CmdOffloadAckResponseV1 ackOut = {};
        ackReq.transferId = req.transferId;
        ackReq.eventId = req.ackEventId;
        if (!ack(ackReq, ackOut)) return false;
        _lastActivityMs = millis();
    } else if (req.offset != 0 && req.ackEventId != 0) {
        return false;
    }

    if (!_recordReady) {
        if (req.offset != 0) return false;
        if (!loadNextRecord()) return false;
    }

    CmdOffloadChunkV1 hdr = {};
    hdr.version = PHONE_OFFLOAD_VERSION;
    hdr.transferId = _transferId;
    hdr.flags = STORAGE.isUploadIndexWindowTruncated()
                    ? PHONE_OFFLOAD_FLAG_INDEX_TRUNCATED
                    : 0;

    if (!_recordReady) {
        hdr.flags |= PHONE_OFFLOAD_FLAG_END;
        memcpy(out, &hdr, sizeof(hdr));
        outLen = sizeof(hdr);
        return true;
    }

    if (req.offset > _bodyLen) return false;
    const size_t chunkCap = cap - sizeof(hdr);
    const size_t remaining = static_cast<size_t>(_bodyLen - req.offset);
    const size_t chunkLen = remaining < chunkCap ? remaining : chunkCap;

    hdr.flags |= PHONE_OFFLOAD_FLAG_RECORD;
    hdr.lane = _lane;
    hdr.eventId = _eventId;
    hdr.totalLen = _bodyLen;
    hdr.offset = req.offset;
    hdr.chunkLen = static_cast<uint16_t>(chunkLen);
    hdr.sessionLen = _sessionLen;
    hdr.topicLen = _topicLen;
    memcpy(out, &hdr, sizeof(hdr));
    if (chunkLen > 0) {
        memcpy(out + sizeof(hdr), _body + req.offset, chunkLen);
    }
    outLen = sizeof(hdr) + chunkLen;
    return true;
}

bool PhoneOffloadManager::ack(const CmdOffloadAckRequestV1& req,
                              CmdOffloadAckResponseV1& out) {
    out = {};
    expireIfStale();
    if (!_active || req.transferId != _transferId || req.eventId == 0) {
        return false;
    }
    _lastActivityMs = millis();

    // If the phone retried after losing the ACK response, accept it without
    // writing another checkpoint.
    if (req.eventId == _lastAckEventId) {
        out.pendingTotal = STORAGE.getAuthoritativePendingEventCount();
        return true;
    }
    if (!_recordReady || req.eventId != _eventId) return false;

    const uint16_t acknowledgedBytes = _bodyLen;
    const bool marked = STORAGE.markEventUploaded(_eventId, _sessionId, _lane);
    if (!marked) return false;

    _sinceId = _eventId;
    _lastAckEventId = _eventId;
    clearRecord();
    _transferAckedRecords++;
    _transferAckedBytes += acknowledgedBytes;
    _acksSinceCheckpoint++;
    if (!flushCheckpointIfDue(false)) return false;
    out.pendingTotal = STORAGE.getAuthoritativePendingEventCount();
    // Per-record serial logging becomes measurable at field-transfer rates.
    // Retain periodic progress and the terminal edge for diagnostics.
    if ((_transferAckedRecords % 32U) == 0U || out.pendingTotal == 0U) {
        DLOG_INFO(TAG, "ack transfer=%u records=%lu event=%lu pending=%lu",
                  static_cast<unsigned>(_transferId),
                  static_cast<unsigned long>(_transferAckedRecords),
                  static_cast<unsigned long>(_lastAckEventId),
                  static_cast<unsigned long>(out.pendingTotal));
    }
    return true;
}

bool PhoneOffloadManager::flushCheckpointIfDue(bool force,
                                               bool callerOwnsStorageWindow) {
    if (!_storageBatchOpen) return true;
    const uint32_t now = millis();
    if (!force && _acksSinceCheckpoint < OFFLOAD_CHECKPOINT_RECORDS &&
        now - _lastCheckpointMs < OFFLOAD_CHECKPOINT_MS) {
        return true;
    }

    StorageExclusiveWindow window;
    if (!callerOwnsStorageWindow) {
        const StorageWindowKind kind = RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD)
            ? STORAGE_WINDOW_UPLOAD : STORAGE_WINDOW_ENRICHMENT;
        if (!window.begin(kind,
                          force ? "phone_offload_finish" : "phone_offload_checkpoint")) {
            return false;
        }
    }
    bool ok = false;
    if (force) {
        // END is the durable handoff boundary. Persist the acknowledged
        // cursors before closing the batch; endUploadBatch intentionally only
        // queues non-critical compaction/sidecar maintenance for later.
        const bool checkpointOk = STORAGE.flushUploadCheckpoint();
        const bool closeOk = STORAGE.endUploadBatch();
        ok = checkpointOk && closeOk;
    } else {
        ok = STORAGE.flushUploadCheckpoint();
    }
    if (!callerOwnsStorageWindow) {
        window.end(ok ? "checkpointed" : "checkpoint_failed");
    }
    if (force) _storageBatchOpen = false;
    if (ok) {
        _acksSinceCheckpoint = 0;
        _lastCheckpointMs = now;
    }
    return ok;
}

bool PhoneOffloadManager::end(uint8_t transferId, const char* reason) {
    if (!_active) {
        abandonPreparation(reason ? reason : "phone_end_idle");
        return true;
    }
    if (transferId != 0 && transferId != _transferId) return false;
    return reset(reason ? reason : "phone_end");
}

void PhoneOffloadManager::abandonPreparation(const char* reason) {
    if (_active) return;
    const uint8_t prep = __atomic_load_n(&_prepState, __ATOMIC_ACQUIRE);
    if (prep == PREP_RUNNING) {
        __atomic_store_n(&_prepAbandonRequested, true, __ATOMIC_RELEASE);
        DLOG_INFO(TAG, "index preparation marked abandoned reason=%s",
                  reason ? reason : "-");
        return;
    }
    if (prep == PREP_READY || prep == PREP_FAILED) {
        if (STORAGE.isUploadIndexResident()) {
            STORAGE.releaseUploadIndexMemory(
                reason ? reason : "offload_prepare_abandoned");
        }
        __atomic_store_n(&_prepState, PREP_IDLE, __ATOMIC_RELEASE);
        DLOG_INFO(TAG, "idle index preparation released reason=%s",
                  reason ? reason : "-");
    }
}

void PhoneOffloadManager::expireIfStale() {
    if (_active && _lastActivityMs != 0 &&
        millis() - _lastActivityMs > OFFLOAD_STALE_MS) {
        (void)reset("stale");
    }
}

bool PhoneOffloadManager::preparationPending() const {
    const uint8_t prep = __atomic_load_n(&_prepState, __ATOMIC_ACQUIRE);
    return prep == PREP_RUNNING || prep == PREP_READY;
}

bool PhoneOffloadManager::loadNextRecord(bool callerOwnsStorageWindow) {
    while (_sessionIndex < _sessions.size()) {
        const String& session = _sessions[_sessionIndex];
        if (!_sinceInitialized) {
            _sinceId = STORAGE.getLastUploadedEventId(session.c_str());
            _sinceInitialized = true;
        }

        JsonDocument record;
        bool found = false;
        bool ok = false;
        if (callerOwnsStorageWindow) {
            ok = STORAGE.getNextUploadEventForSession(
                session.c_str(), _sinceId, record, found);
        } else {
            StorageExclusiveWindow window;
            const StorageWindowKind kind = RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD)
                ? STORAGE_WINDOW_UPLOAD : STORAGE_WINDOW_ENRICHMENT;
            if (!window.begin(kind, "phone_offload_fetch")) return false;
            ok = STORAGE.getNextUploadEventForSession(
                session.c_str(), _sinceId, record, found);
            window.end(ok ? "fetched" : "fetch_failed");
        }
        if (!ok) return false;
        if (!found) {
            _sessionIndex++;
            _sinceId = 0;
            _sinceInitialized = false;
            continue;
        }

        JsonObjectConst obj = record.as<JsonObjectConst>();
        if (obj.isNull()) return false;
        const uint32_t eventId = obj["id"] | 0U;
        const uint8_t status = obj["status"] | static_cast<uint8_t>(EVT_RAW);
        if (eventId == 0) return false;
        if (status == static_cast<uint8_t>(EVT_UPLOADED)) {
            _sinceId = eventId;
            continue;
        }
        return buildRecordBody(obj, session);
    }
    clearRecord();
    return true;
}

bool PhoneOffloadManager::startWifiBulk(const uint8_t* payload, size_t len) {
#if !PHONE_WIFI_BULK_ENABLED
    (void)payload;
    (void)len;
    DLOG_WARN(TAG,
              "wifi bulk refused by field stability profile; use BLE offload");
    return false;
#else
    if (!_active || !payload || len < sizeof(CmdWifiOffloadBeginHeaderV1) ||
        _wifiBulkState != WIFI_BULK_IDLE) return false;
    CmdWifiOffloadBeginHeaderV1 hdr = {};
    memcpy(&hdr, payload, sizeof(hdr));
    const size_t expected = sizeof(hdr) + hdr.ssidLen + hdr.passwordLen + hdr.tokenLen;
    if (hdr.version != 1 || hdr.transferId != _transferId || hdr.ssidLen < 1 ||
        hdr.ssidLen > 32 || hdr.passwordLen < 8 || hdr.passwordLen > 63 ||
        hdr.tokenLen != sizeof(_wifiBulkToken) || hdr.port == 0 || expected != len) return false;
    size_t offset = sizeof(hdr);
    memcpy(_wifiBulkSsid, payload + offset, hdr.ssidLen); _wifiBulkSsid[hdr.ssidLen] = 0; offset += hdr.ssidLen;
    memcpy(_wifiBulkPassword, payload + offset, hdr.passwordLen); _wifiBulkPassword[hdr.passwordLen] = 0; offset += hdr.passwordLen;
    memcpy(_wifiBulkToken, payload + offset, sizeof(_wifiBulkToken));
    _wifiBulkPort = hdr.port;
    _wifiBulkState = WIFI_BULK_DELAY;
    _wifiBulkNextActionMs = millis() + 750UL;
    _wifiBulkDeadlineMs = millis() + 300000UL;
    _wifiBatch.clear(); _wifiBatch.reserve(WIFI_BULK_BATCH_MAX_BYTES);
    installWifiBulkEvents();
    DLOG_INFO(TAG, "wifi bulk armed transfer=%u pending=%lu ssidLen=%u port=%u",
              _transferId, static_cast<unsigned long>(_beginPendingTotal), hdr.ssidLen, hdr.port);
    return true;
#endif
}

bool PhoneOffloadManager::resumeWifiBulkEarly() {
    if (!wifiBulkResumeValid()) return false;

    // Consume the RTC handoff before touching heap or the Wi-Fi driver. If any
    // early-resume step faults, the next boot must take the normal recovery
    // path instead of replaying the same risky operation forever.
    const WifiBulkResumeState retained = s_wifiBulkResume;
    memset(&s_wifiBulkResume, 0, sizeof(s_wifiBulkResume));

    strlcpy(_wifiBulkSsid, retained.ssid, sizeof(_wifiBulkSsid));
    strlcpy(_wifiBulkPassword,
            retained.password,
            sizeof(_wifiBulkPassword));
    memcpy(_wifiBulkToken, retained.token, sizeof(_wifiBulkToken));
    _wifiBulkPort = retained.port;
    _transferId = retained.transferId;
    _wifiBulkDeadlineMs = millis() + 300000UL;
    _wifiBulkState = WIFI_BULK_BOOT_PREP;
    _wifiBatch.clear();
    _wifiBatch.reserve(WIFI_BULK_BATCH_MAX_BYTES);
    s_wifiBulkPhoneIp = IPAddress();
    installWifiBulkEvents();

    const bool modeOk = WiFi.mode(WIFI_AP);
    const bool apStarted = modeOk && WiFi.softAP(
        _wifiBulkSsid, _wifiBulkPassword, 1, 0, 1, false,
        WIFI_AUTH_WPA2_PSK, WIFI_CIPHER_TYPE_CCMP);
    const bool stagingReady = apStarted && ensureWifiStaging();
    DLOG_INFO(TAG,
              "wifi bulk early resume mode=%u ap=%u staging=%u ssid=%s ip=%s",
              modeOk ? 1U : 0U,
              apStarted ? 1U : 0U,
              stagingReady ? 1U : 0U,
              _wifiBulkSsid,
              WiFi.softAPIP().toString().c_str());
    if (!stagingReady) {
        if (apStarted) WiFi.softAPdisconnect(true);
        _wifiBulkState = WIFI_BULK_IDLE;
    }
    return stagingReady;
}

void PhoneOffloadManager::discardRetainedWifiBulkResume() {
    memset(&s_wifiBulkResume, 0, sizeof(s_wifiBulkResume));
    DLOG_WARN(TAG, "discarded retained wifi bulk resume for recovery boot");
}

void PhoneOffloadManager::tickWifiBulk() {
    if (_wifiBulkState == WIFI_BULK_IDLE) return;
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - _wifiBulkDeadlineMs) >= 0) {
        finishWifiBulk(false, "wifi_bulk_timeout"); return;
    }
    if (_wifiBulkState == WIFI_BULK_DELAY && static_cast<int32_t>(now - _wifiBulkNextActionMs) >= 0) {
        _wifiBulkState = WIFI_BULK_LEASE;
    }
    if (_wifiBulkState == WIFI_BULK_LEASE) {
        // Live BLE -> Wi-Fi driver swaps reset this S3 even after both stacks
        // are quiesced. Preserve the one-time field-link material in RTC RAM
        // and perform a deliberate reboot; setup() starts the AP before BLE or
        // capture can allocate the shared radio memory.
        WifiBulkResumeState retained = {};
        retained.magic = WIFI_BULK_RESUME_MAGIC;
        retained.transferId = _transferId;
        retained.port = _wifiBulkPort;
        strlcpy(retained.ssid, _wifiBulkSsid, sizeof(retained.ssid));
        strlcpy(retained.password,
                _wifiBulkPassword,
                sizeof(retained.password));
        memcpy(retained.token, _wifiBulkToken, sizeof(retained.token));
        retained.checksum = wifiBulkResumeChecksum(retained);
        s_wifiBulkResume = retained;
        DLOG_INFO(TAG, "wifi bulk state retained; clean radio reboot");
        Serial.flush();
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
        return;
    }
    if (_wifiBulkState == WIFI_BULK_BOOT_PREP) {
        if (!STORAGE.isReady() || !RADIO_ARB.isReady()) return;
        if (!RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD) &&
            !RADIO_ARB.adoptPrestartedUploadLease(
                300000UL, "phone_wifi_bulk_resume")) {
            finishWifiBulk(false, "wifi_resume_lease_failed");
            return;
        }

        StorageExclusiveWindow prepWindow;
        bool prepared = prepWindow.begin(
            STORAGE_WINDOW_UPLOAD, "phone_wifi_resume_prepare");
        if (prepared) {
            prepared = STORAGE.prepareUploadIndexForUpload(
                OFFLOAD_INDEX_BUDGET_MS);
            prepWindow.end(prepared ? "index_ready" : "index_failed");
        }
        if (!prepared) {
            finishWifiBulk(false, "wifi_resume_index_failed");
            return;
        }

        _sessions.clear();
        STORAGE.listEventSessions(_sessions);
        _sessionIndex = 0;
        _sinceId = 0;
        _sinceInitialized = false;
        clearRecord();
        _active = true;
        _lastActivityMs = millis();
        _lastAckEventId = 0;
        _acksSinceCheckpoint = 0;
        _lastCheckpointMs = _lastActivityMs;
        _transferStartMs = _lastActivityMs;
        _transferAckedBytes = 0;
        _transferAckedRecords = 0;
        _beginPendingTotal = STORAGE.getAuthoritativePendingEventCount();
        _beginIndexedTotal = STORAGE.getUploadIndexResidentEventCount();
        _beginFlags = STORAGE.isUploadIndexWindowTruncated()
                          ? PHONE_OFFLOAD_FLAG_INDEX_TRUNCATED
                          : 0;
        STORAGE.beginUploadBatch();
        _storageBatchOpen = STORAGE.isUploadBatchActive();
        if (!_storageBatchOpen) {
            finishWifiBulk(false, "wifi_resume_batch_failed");
            return;
        }
        _wifiBulkNextActionMs = now + 120000UL;
        _wifiBulkState = WIFI_BULK_CONNECT_WIFI;
        DLOG_INFO(TAG,
                  "wifi bulk resumed pending=%lu indexed=%lu phone=%s",
                  static_cast<unsigned long>(_beginPendingTotal),
                  static_cast<unsigned long>(_beginIndexedTotal),
                  s_wifiBulkPhoneIp.toString().c_str());
        return;
    }
    if (_wifiBulkState == WIFI_BULK_CONNECT_WIFI) {
        if (static_cast<uint32_t>(s_wifiBulkPhoneIp) == 0U) {
            if (static_cast<int32_t>(now - _wifiBulkNextActionMs) >= 0) {
                finishWifiBulk(false, "wifi_phone_join_timeout");
            }
            return;
        }
        _wifiBulkState = WIFI_BULK_CONNECT_TCP;
    }
    if (_wifiBulkState == WIFI_BULK_CONNECT_TCP) {
        if (!s_wifiBulkClient.connect(s_wifiBulkPhoneIp, _wifiBulkPort, 10000)) {
            finishWifiBulk(false, "wifi_tcp_failed"); return;
        }
        s_wifiBulkClient.setNoDelay(true);
        uint8_t hello[36] = {};
        hello[0] = 'S'; hello[1] = 'P'; hello[2] = 'B'; hello[3] = '1';
        memcpy(hello + 4, _wifiBulkToken, sizeof(_wifiBulkToken));
        if (s_wifiBulkClient.write(hello, sizeof(hello)) != sizeof(hello)) {
            finishWifiBulk(false, "wifi_hello_failed"); return;
        }
        // The session summary should measure the data plane. AP approval and
        // the controlled radio reboot are setup latency, not transfer speed.
        _transferStartMs = millis();
        _wifiBulkState = WIFI_BULK_TRANSFER;
        DLOG_INFO(TAG, "wifi bulk connected ip=%s port=%u",
                  s_wifiBulkPhoneIp.toString().c_str(), _wifiBulkPort);
    }
    if (_wifiBulkState == WIFI_BULK_TRANSFER) {
        if (!sendWifiBatch()) finishWifiBulk(false, "wifi_batch_failed");
    }
}

bool PhoneOffloadManager::sendWifiBatch() {
    if (!_wifiStaged) return false;
    _wifiBatch.clear(); _wifiStagedCount = 0;
    bool reachedEnd = false;
    StorageExclusiveWindow readWindow;
    if (!readWindow.begin(STORAGE_WINDOW_UPLOAD, "phone_wifi_batch_read")) return false;
    bool buildOk = true;
    while (_wifiStagedCount < WIFI_BULK_BATCH_MAX_RECORDS) {
        if (!loadNextRecord(true)) { buildOk = false; break; }
        if (!_recordReady) { reachedEnd = true; break; }
        const size_t needed = 10U + _bodyLen;
        if (!_wifiBatch.empty() && _wifiBatch.size() + needed > WIFI_BULK_BATCH_MAX_BYTES) break;
        if (needed > WIFI_BULK_BATCH_MAX_BYTES) { buildOk = false; break; }
        appendBe32(_wifiBatch, _eventId);
        _wifiBatch.push_back(_lane); _wifiBatch.push_back(_sessionLen); _wifiBatch.push_back(_topicLen); _wifiBatch.push_back(0);
        appendBe16(_wifiBatch, _bodyLen);
        _wifiBatch.insert(_wifiBatch.end(), _body, _body + _bodyLen);
        WifiStagedRecord& staged = _wifiStaged[_wifiStagedCount++];
        staged.eventId = _eventId; staged.lane = _lane; staged.bytes = _bodyLen;
        strlcpy(staged.sessionId, _sessionId, sizeof(staged.sessionId));
        _sinceId = _eventId;
        clearRecord();
    }
    readWindow.end(buildOk ? "batch_ready" : "batch_read_failed");
    if (!buildOk) return false;

    // An upload index is deliberately capped at 8192 records. Keep the same
    // private Wi-Fi/TCP session alive while rolling to the next bounded index
    // so a 10k+ field backlog pays the BLE/reboot/approval cost only once.
    if (_wifiStagedCount == 0 && reachedEnd &&
        STORAGE.getAuthoritativePendingEventCount() > 0) {
        if (!flushCheckpointIfDue(true)) return false;

        StorageExclusiveWindow nextWindow;
        bool nextReady = nextWindow.begin(
            STORAGE_WINDOW_UPLOAD, "phone_wifi_next_window");
        if (nextReady) {
            nextReady = STORAGE.prepareUploadIndexForUpload(
                OFFLOAD_INDEX_BUDGET_MS);
            nextWindow.end(nextReady ? "index_ready" : "index_failed");
        }
        if (!nextReady) return false;

        _sessions.clear();
        STORAGE.listEventSessions(_sessions);
        _sessionIndex = 0;
        _sinceId = 0;
        _sinceInitialized = false;
        clearRecord();
        STORAGE.beginUploadBatch();
        _storageBatchOpen = STORAGE.isUploadBatchActive();
        if (!_storageBatchOpen) return false;
        _beginIndexedTotal = STORAGE.getUploadIndexResidentEventCount();
        _beginFlags = STORAGE.isUploadIndexWindowTruncated()
                          ? PHONE_OFFLOAD_FLAG_INDEX_TRUNCATED
                          : 0;
        DLOG_INFO(TAG,
                  "wifi bulk next window indexed=%lu pending=%lu",
                  static_cast<unsigned long>(_beginIndexedTotal),
                  static_cast<unsigned long>(
                      STORAGE.getAuthoritativePendingEventCount()));
        return true;
    }

    uint8_t countBytes[2] = { static_cast<uint8_t>(_wifiStagedCount >> 8), static_cast<uint8_t>(_wifiStagedCount) };
    if (s_wifiBulkClient.write(countBytes, 2) != 2) return false;
    if (!_wifiBatch.empty() && s_wifiBulkClient.write(_wifiBatch.data(), _wifiBatch.size()) != _wifiBatch.size()) return false;
    s_wifiBulkClient.flush();
    uint8_t ack[12] = {};
    const uint32_t deadline = millis() + 30000UL;
    size_t received = 0;
    while (received < sizeof(ack) && static_cast<int32_t>(millis() - deadline) < 0) {
        const int available = s_wifiBulkClient.available();
        if (available > 0) received += s_wifiBulkClient.read(ack + received, sizeof(ack) - received);
        else vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (received != sizeof(ack) || readBe32(ack) != WIFI_BULK_ACK_MAGIC ||
        readBe16(ack + 4) != _wifiStagedCount || readBe16(ack + 6) != 0) return false;
    if (_wifiStagedCount == 0) {
        if (!reachedEnd) return false;
        finishWifiBulk(true, "wifi_bulk_complete");
        return true;
    }
    if (readBe32(ack + 8) != _wifiStaged[_wifiStagedCount - 1].eventId) return false;
    StorageExclusiveWindow commitWindow;
    if (!commitWindow.begin(STORAGE_WINDOW_UPLOAD, "phone_wifi_batch_commit")) return false;
    bool commitOk = true;
    for (uint16_t i = 0; i < _wifiStagedCount; ++i) {
        const WifiStagedRecord& staged = _wifiStaged[i];
        if (!STORAGE.markEventUploaded(staged.eventId, staged.sessionId, staged.lane)) {
            commitOk = false; break;
        }
        _transferAckedRecords++; _transferAckedBytes += staged.bytes;
    }
    if (commitOk) {
        _acksSinceCheckpoint += _wifiStagedCount;
        commitOk = flushCheckpointIfDue(false, true);
    }
    commitWindow.end(commitOk ? "batch_committed" : "batch_commit_failed");
    if (!commitOk) return false;
    DLOG_INFO(TAG, "wifi bulk progress records=%lu pending=%lu batch=%u bytes=%u",
              static_cast<unsigned long>(_transferAckedRecords),
              static_cast<unsigned long>(STORAGE.getAuthoritativePendingEventCount()),
              _wifiStagedCount, static_cast<unsigned>(_wifiBatch.size()));
    return true;
}

void PhoneOffloadManager::finishWifiBulk(bool success, const char* reason) {
    s_wifiBulkClient.stop();
    WiFi.softAPdisconnect(true);
    s_wifiBulkPhoneIp = IPAddress();
    s_wifiBulkResume.magic = 0;
    _wifiBulkState = WIFI_BULK_IDLE;
    memset(_wifiBulkPassword, 0, sizeof(_wifiBulkPassword));
    memset(_wifiBulkToken, 0, sizeof(_wifiBulkToken));
    (void)end(_transferId, reason);
    releaseWifiStaging();
    if (RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD)) RADIO_ARB.release(RADIO_WIFI_UPLOAD, reason);
    DLOG_INFO(TAG, "wifi bulk finished success=%u reason=%s", success ? 1U : 0U, reason ? reason : "-");
}

bool PhoneOffloadManager::ensureWifiStaging() {
    if (_wifiStaged) return true;
    _wifiStaged = static_cast<WifiStagedRecord*>(heap_caps_calloc(
        WIFI_BULK_BATCH_MAX_RECORDS,
        sizeof(WifiStagedRecord),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_wifiStaged) {
        DLOG_ERROR(TAG,
                   "wifi staging PSRAM alloc failed bytes=%u free=%lu largest=%lu",
                   static_cast<unsigned>(WIFI_BULK_BATCH_MAX_RECORDS *
                                         sizeof(WifiStagedRecord)),
                   static_cast<unsigned long>(
                       heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                   static_cast<unsigned long>(
                       heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        return false;
    }
    return true;
}

bool PhoneOffloadManager::ensureRecordBody() {
    if (_body) return true;
    _body = static_cast<uint8_t*>(heap_caps_calloc(
        PHONE_OFFLOAD_RECORD_MAX,
        sizeof(uint8_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_body) {
        DLOG_ERROR(TAG,
                   "record body PSRAM alloc failed bytes=%u free=%lu largest=%lu",
                   static_cast<unsigned>(PHONE_OFFLOAD_RECORD_MAX),
                   static_cast<unsigned long>(
                       heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                   static_cast<unsigned long>(
                       heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        return false;
    }
    return true;
}

void PhoneOffloadManager::releaseWifiStaging() {
    if (!_wifiStaged) return;
    heap_caps_free(_wifiStaged);
    _wifiStaged = nullptr;
    _wifiStagedCount = 0;
}

bool PhoneOffloadManager::buildRecordBody(JsonObjectConst record,
                                          const String& sessionId) {
    if (!ensureRecordBody()) return false;
    JsonDocument publish;
    if (!copyRecordForPublish(record, publish)) return false;

    const char* type = record["type"] | "event";
    const String topic = topicForEventType(type);
    const size_t sessionLen = sessionId.length();
    const size_t topicLen = topic.length();
    const size_t payloadLen = measureJson(publish);
    const size_t totalLen = sessionLen + topicLen + payloadLen;
    if (sessionLen == 0 || sessionLen > 255 || topicLen == 0 || topicLen > 255 ||
        payloadLen == 0 || totalLen > PHONE_OFFLOAD_RECORD_MAX ||
        totalLen > 0xFFFFU) {
        DLOG_WARN(TAG,
                  "record too large event=%lu session=%u topic=%u payload=%u total=%u",
                  static_cast<unsigned long>(record["id"] | 0U),
                  static_cast<unsigned>(sessionLen),
                  static_cast<unsigned>(topicLen),
                  static_cast<unsigned>(payloadLen),
                  static_cast<unsigned>(totalLen));
        return false;
    }

    memcpy(_body, sessionId.c_str(), sessionLen);
    memcpy(_body + sessionLen, topic.c_str(), topicLen);
    const size_t written = serializeJson(
        publish, _body + sessionLen + topicLen, payloadLen);
    if (written != payloadLen) return false;

    _eventId = record["id"] | 0U;
    _lane = record["lane"] | static_cast<uint8_t>(STORAGE_LANE_NOISE);
    _sessionLen = static_cast<uint8_t>(sessionLen);
    _topicLen = static_cast<uint8_t>(topicLen);
    _bodyLen = static_cast<uint16_t>(totalLen);
    strlcpy(_sessionId, sessionId.c_str(), sizeof(_sessionId));
    _recordReady = true;
    return true;
}

void PhoneOffloadManager::clearRecord() {
    _recordReady = false;
    _eventId = 0;
    _lane = 0xFF;
    _sessionLen = 0;
    _topicLen = 0;
    _bodyLen = 0;
    _sessionId[0] = '\0';
}

bool PhoneOffloadManager::reset(const char* reason) {
    const uint8_t oldTransfer = _transferId;
    const uint32_t elapsedMs = _transferStartMs ? millis() - _transferStartMs : 0;
    const uint32_t ackedRecords = _transferAckedRecords;
    const uint32_t ackedBytes = _transferAckedBytes;
    const bool checkpointOk = flushCheckpointIfDue(true);
    _active = false;
    _lastActivityMs = 0;
    _sessions.clear();
    _sessionIndex = 0;
    _sinceId = 0;
    _sinceInitialized = false;
    _beginPendingTotal = 0;
    _beginIndexedTotal = 0;
    _beginFlags = 0;
    _acksSinceCheckpoint = 0;
    _lastCheckpointMs = 0;
    _transferStartMs = 0;
    _transferAckedBytes = 0;
    _transferAckedRecords = 0;
    clearRecord();
    STORAGE.closeUploadReadFile();
    STORAGE.releaseUploadIndexMemory(reason ? reason : "phone_offload_end");
    __atomic_store_n(&_prepAbandonRequested, false, __ATOMIC_RELEASE);
    __atomic_store_n(&_prepState, PREP_IDLE, __ATOMIC_RELEASE);
    DLOG_INFO(TAG, "end transfer=%u reason=%s checkpoint=%u",
              static_cast<unsigned>(oldTransfer),
              reason ? reason : "-",
              checkpointOk ? 1U : 0U);
    if (ackedRecords > 0 && elapsedMs > 0) {
        const uint32_t recordsPerSecondX10 =
            static_cast<uint32_t>((static_cast<uint64_t>(ackedRecords) * 10000ULL) /
                                  elapsedMs);
        DLOG_INFO(TAG,
                  "offload_session_summary records=%lu bytes=%lu totalMs=%lu rate=%lu.%lu records_s",
                  static_cast<unsigned long>(ackedRecords),
                  static_cast<unsigned long>(ackedBytes),
                  static_cast<unsigned long>(elapsedMs),
                  static_cast<unsigned long>(recordsPerSecondX10 / 10U),
                  static_cast<unsigned long>(recordsPerSecondX10 % 10U));
    }
    return checkpointOk;
}
