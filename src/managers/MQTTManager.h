


#pragma once
#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <utility>
#include <esp_heap_caps.h>
#include "../SecretsConfig.h"
#include "../core/SpectreState.h"
#include "../core/Session.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// OWNERSHIP CONTRACT
// - TaskHardware is the only caller that may tick or mutate MQTT state.
// - Upload progress depends on a RADIO_WIFI_UPLOAD lease granted by
//   RadioArbiter.
// - Display code may read mirrored g_state fields only; it must not call into
//   MQTTManager directly.

#define MQTT_BROKER_HOST        SPECTRE_MQTT_BROKER
#define MQTT_BROKER_PORT        SPECTRE_MQTT_PORT
#define MQTT_SENSOR_ID          SPECTRE_MQTT_SENSOR_ID


// Topic constants — match EtherGuard schema exactly
#define TOPIC_EVENT     SPECTRE_MQTT_TOPIC_BASE "/" MQTT_SENSOR_ID "/event"
#define TOPIC_HEALTH    SPECTRE_MQTT_TOPIC_BASE "/" MQTT_SENSOR_ID "/health"
#define TOPIC_DRONE     SPECTRE_MQTT_TOPIC_BASE "/" MQTT_SENSOR_ID "/drone"
#define TOPIC_PMKID     SPECTRE_MQTT_TOPIC_BASE "/" MQTT_SENSOR_ID "/pmkid"
#define TOPIC_CENSUS    SPECTRE_MQTT_TOPIC_BASE "/" MQTT_SENSOR_ID "/census"
#define TOPIC_PROBE     SPECTRE_MQTT_TOPIC_BASE "/" MQTT_SENSOR_ID "/probe"
#define TOPIC_DEVICE    SPECTRE_MQTT_TOPIC_BASE "/" MQTT_SENSOR_ID "/device"
// FieldVault drain — high-value boot/crash/upload/enrichment summaries.
// Payload is a single JSONL record line (no trailing newline) with a "type"
// tag identifying the record kind.
#define TOPIC_FIELD     SPECTRE_MQTT_TOPIC_BASE "/" MQTT_SENSOR_ID "/field"

typedef enum {
    MQTT_IDLE,
    MQTT_CONNECTING_WIFI,
    MQTT_CONNECTING_BROKER,
    MQTT_DUMPING,
    MQTT_DONE,
    MQTT_FAILED
} MQTTState;

class MQTTManager {
public:
    void begin();
    void tick();

    // Trigger a dump through radio arbiter
    bool requestDump(bool force = false);
    bool requestFieldVaultDump();

    // BLE WireGuard trigger — always allowed, confirms intent
    bool bleTriggeredDump();

    // Queue a record for next dump
    // These are called by WiFiManager as captures happen
    bool queueProbe(const char* mac, const char* ssid,
                    int8_t rssi, uint8_t channel,
                    const char* ieFingerprint);
    void queueDevice(const char* mac, const char* ieFingerprint,
                     const char* probeSetHash, int8_t rssi,
                     bool isRandomMAC);
    void queueDrone(const char* droneID, float lat, float lon,
                    float alt, const char* mac, int8_t rssi,
                    uint8_t channel, const char* protocol);
    // eapolMask: bit0=M1 bit1=M2 bit2=M3 bit3=M4 at time of capture.
    // 0 is valid (PMKID from a stray M1 RSN IE with no handshake observed).
    void queuePMKID(const char* ssid, const char* bssid,
                    const char* clientMAC, const uint8_t* pmkid,
                    uint8_t eapolMask = 0);
    void queueEvent(const char* eventType, const char* severity,
                    const char* mac, const char* ssid,
                    const char* detail, const char* category);
    void noteExternalQueuedRecord();

    MQTTState getState()      { return _state; }
    bool      isDumping()     { return _state == MQTT_DUMPING; }
    bool      dumpAvailable();
    int       queueDepth();
    uint32_t  lastDumpAge()   { return millis() - _lastDumpMs; }
    int       uploadReadyCount() const;
    bool      uploadLeaseReady(bool force = false) const;
    bool      backlogDrainActive() const { return _continuousDrainActive; }

private:
    // TaskDisplay suspend/resume around the upload mission. Idempotent — safe
    // to call resume from every cleanup exit even if pause was never entered.
    void _pauseDisplayForUpload();
    void _resumeDisplayAfterUpload();

    enum QueueMetric : uint8_t {
        QUEUE_METRIC_NONE = 0,
        QUEUE_METRIC_PROBES,
        QUEUE_METRIC_DEVICES,
        QUEUE_METRIC_DRONES,
        QUEUE_METRIC_PMKIDS
    };
        enum DumpPhase : uint8_t {
        DUMP_PHASE_IDLE = 0,
        DUMP_PHASE_HEALTH,
        DUMP_PHASE_EVENTS,
        DUMP_PHASE_CENSUS,
        DUMP_PHASE_COMPACT,
        DUMP_PHASE_PURGE,
        DUMP_PHASE_DONE,
        DUMP_PHASE_FAILED,
        // Field vault drain. Runs first so high-value tiny boot/crash records
        // publish before bulky event/census traffic. Appended to keep the
        // existing ordinals stable.
        DUMP_PHASE_FIELD,
    };

    struct DumpContext {
        // Per-record payload + identifiers live in PSRAM. Internal heap is
        // tight at upload time (~33 KB free); keeping Arduino Strings here
        // costs ~70 B internal per record and fragments the heap once the
        // bucket approaches its limit. PSRAM has ~7 MB free; per-record
        // internal cost reduces to the three-alloc bookkeeping overhead
        // (~24 B). Non-copyable + move-only so we can't double-free the
        // PSRAM buffers.
        struct UploadPublishRecord {
            char* sessionId = nullptr;
            char* topic = nullptr;
            char* payload = nullptr;
            size_t payloadLen = 0;
            uint32_t eventId = 0;
            uint8_t lane = 0xFF;

            UploadPublishRecord() = default;
            UploadPublishRecord(const UploadPublishRecord&) = delete;
            UploadPublishRecord& operator=(const UploadPublishRecord&) = delete;
            UploadPublishRecord(UploadPublishRecord&& o) noexcept
                : sessionId(o.sessionId),
                  topic(o.topic),
                  payload(o.payload),
                  payloadLen(o.payloadLen),
                  eventId(o.eventId),
                  lane(o.lane) {
                o.sessionId = nullptr;
                o.topic = nullptr;
                o.payload = nullptr;
                o.payloadLen = 0;
            }
            UploadPublishRecord& operator=(UploadPublishRecord&& o) noexcept {
                if (this != &o) {
                    heap_caps_free(sessionId);
                    heap_caps_free(topic);
                    heap_caps_free(payload);
                    sessionId = o.sessionId; o.sessionId = nullptr;
                    topic = o.topic; o.topic = nullptr;
                    payload = o.payload; o.payload = nullptr;
                    payloadLen = o.payloadLen; o.payloadLen = 0;
                    eventId = o.eventId;
                    lane = o.lane;
                }
                return *this;
            }
            ~UploadPublishRecord() {
                heap_caps_free(sessionId);
                heap_caps_free(topic);
                heap_caps_free(payload);
            }
        };

        DumpContext()
            : phase(DUMP_PHASE_IDLE),
              sessionIndex(0),
              sinceId(0),
              maxEventsThisLease(0),
              phaseStarted(false),
              healthDone(false),
              censusDone(false),
              compactDone(false),
              purgeDone(false),
              firstEventFetchLogged(false),
              firstEventFetchedOkLogged(false),
              firstEventValidatedLogged(false),
              firstEventDocBuiltLogged(false),
              firstEventPublishBeginLogged(false),
              firstEventPublishEndLogged(false),
              sinceIdInitialized(false),
              eventsPrefetched(false),
              bucketNumber(0),
              sessionIds(),
              cachedBatch(),
              uploadBucket(),
              uploadBucketIndex(0),
              uploadBucketComplete(false),
              cachedBatchIndex(0),
              cachedBatchCount(0) {}

        DumpPhase phase = DUMP_PHASE_IDLE;
        size_t sessionIndex = 0;
        uint32_t sinceId = 0;
        uint32_t maxEventsThisLease = 0;
        bool phaseStarted = false;
        bool healthDone = false;
        bool censusDone = false;
        bool compactDone = false;
        bool purgeDone = false;
        bool firstEventFetchLogged = false;
        bool firstEventFetchedOkLogged = false;
        bool firstEventValidatedLogged = false;
        bool firstEventDocBuiltLogged = false;
        bool firstEventPublishBeginLogged = false;
        bool firstEventPublishEndLogged = false;
        bool sinceIdInitialized = false;
        bool eventsPrefetched = false;
        uint16_t bucketNumber = 0;
        // Session list for DUMP_PHASE_EVENTS. Kept here (not static local) so
        // that _dumpCtx = {} at dump start properly destructs the Strings and
        // releases the vector's backing allocation. A static local would retain
        // peak capacity across cycles and fragment the heap.
        std::vector<String> sessionIds;
        // Cached upload record: fetched from the binary upload index, then
        // published on a later slice so storage reads and MQTT writes stay
        // separated during the hot upload path.
        JsonDocument cachedBatch;
        std::vector<UploadPublishRecord> uploadBucket;
        size_t       uploadBucketIndex = 0;
        bool         uploadBucketComplete = false;
        uint16_t     cachedBatchIndex = 0;
        uint16_t     cachedBatchCount = 0;
    };

    WiFiClient    _wifiClient;
    PubSubClient  _mqtt;
    MQTTState     _state         = MQTT_IDLE;
    uint32_t      _lastDumpMs    = 0;
    uint32_t      _stateEnteredMs = 0;
    bool          _bleTriggered  = false;
    int           _queuedRecords = 0;
    bool          _sessionCleared = false;
    int           _lastPublished = 0;
    int           _lastFailed    = 0;
    uint32_t      _uploadBackoffUntilMs = 0;
    uint32_t      _uploadLeaseHoldMs   = 0;  // computed at requestDump(), scales with backlog
    bool          _continuousDrainActive = false;
    uint32_t      _lastPoisonEventId = 0;
    String        _lastPoisonSessionId;
    uint8_t       _lastPoisonEventFailures = 0;
    uint32_t      _uploadStartStackWatermarkBytes = 0;
    uint32_t      _lastBrokerConnectAttemptMs = 0;
    bool          _brokerConnectSettleLogged = false;
    bool          _resumeDumpAfterReconnect = false;
    // TaskDisplay is suspended for the duration of the upload mission. With
    // OPI PSRAM on ESP32-S3 the flash and PSRAM caches share SPI0, so any
    // LVGL render on core 1 (which reads PSRAM-backed fonts/framebuffer)
    // overlapping a LittleFS.open() flash op on core 0 can silently
    // triple-fault the chip. Suspending TaskDisplay for the upload-mission
    // window eliminates the race; UI freezes on the last-rendered upload
    // status frame and resumes at MQTT_DONE/MQTT_FAILED.
    bool          _displayPausedForUpload = false;

    // One-shot startup FieldVault upload state. _bootGraceUntilMs is set in
    // begin() and never moves; _startupFieldDumpDone latches true on the
    // single attempt and is never cleared (no periodic retry loop).
    // _fieldOnlyMode and _fieldOnlyPublishedThisDump bound the dump so it
    // does not drain the regular event backlog.
    uint32_t      _bootGraceUntilMs       = 0;
    bool          _startupFieldDumpDone   = false;
    bool          _fieldOnlyMode          = false;
    uint8_t       _fieldOnlyPublishedThisDump = 0;
    bool          _fieldOnlyClearAfterRelease = false;

    // Internal state machine
    void _runStateMachine();
    bool _connectWiFi();
    bool _connectBroker();
    void _startDumpPlan();
    void _prefetchFirstUploadEvent();
    bool _fillUploadBucketRadioQuiet(uint16_t maxRecords);
    bool _disconnectStaForUploadRefill();
    bool _runDumpSlice();
    void _disconnect();
    void _cleanup();
    void _setUploadUiState(bool active,
                       const char* phase,
                       uint32_t published,
                       uint32_t total,
                       bool radioBusy);
    void _logUploadStackWatermark(const char* result);
    bool _wifiConnectStarted = false;
                       
    // Individual publish methods
    void _publishHealth();
    void _publishCensus();

    bool _maybeStartStartupFieldDump();
    bool _startStartupFieldDump();
    bool _publishJson(const char* topic, JsonDocument& doc,
                      bool retained = false,
                      uint32_t debugEventId = 0);
    bool _publishPayload(const char* topic,
                         const char* payload,
                         size_t payloadLen,
                         bool retained = false);

    // Event backlog management
    void _migrateLegacyQueueFiles();
    void _refreshPendingCount(bool refreshDebriefMirror = false);
    void _prepareQueuedEvent(JsonDocument& doc);
    uint32_t _appendSyncEvent(const char* eventType,
                              JsonDocument& doc,
                              QueueMetric metric);
    void _noteQueuedRecord(QueueMetric metric);
    bool _purgeTransientFiles();
    DumpContext _dumpCtx{};

    // Timestamp helper — ISO8601 UTC
    void _timestamp(char* buf, int len);
};

extern MQTTManager MQTT_MGR;



