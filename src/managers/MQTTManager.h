

#pragma once
#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>
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

    // BLE WireGuard trigger — always allowed, confirms intent
    bool bleTriggeredDump();

    // Queue a record for next dump
    // These are called by WiFiManager as captures happen
    void queueProbe(const char* mac, const char* ssid,
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
    bool      dumpAvailable() { return _queuedRecords > 0; }
    int       queueDepth()    { return _queuedRecords; }
    uint32_t  lastDumpAge()   { return millis() - _lastDumpMs; }
    int       uploadReadyCount() const { return _queuedRecords; }
    bool      uploadLeaseReady(bool force = false) const;

private:
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
        DUMP_PHASE_FAILED
    };

    struct DumpContext {
        DumpPhase phase = DUMP_PHASE_IDLE;
        size_t sessionIndex = 0;
        uint32_t sinceId = 0;
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
        // Session list for DUMP_PHASE_EVENTS. Kept here (not static local) so
        // that _dumpCtx = {} at dump start properly destructs the Strings and
        // releases the vector's backing allocation. A static local would retain
        // peak capacity across cycles and fragment the heap.
        std::vector<String> sessionIds;
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
    uint32_t      _lastPoisonEventId = 0;
    String        _lastPoisonSessionId;
    uint8_t       _lastPoisonEventFailures = 0;

    // Internal state machine
    void _runStateMachine();
    bool _connectWiFi();
    bool _connectBroker();
    void _startDumpPlan();
    bool _runDumpSlice();
    void _disconnect();
    void _cleanup();
    void _setUploadUiState(bool active,
                       const char* phase,
                       uint32_t published,
                       uint32_t total,
                       bool radioBusy);
    bool _wifiConnectStarted = false;
                       
    // Individual publish methods
    void _publishHealth();
    void _publishCensus();
    bool _publishJson(const char* topic, JsonDocument& doc,
                      bool retained = false,
                      uint32_t debugEventId = 0);

    // Event backlog management
    void _migrateLegacyQueueFiles();
    void _refreshPendingCount(bool refreshDebriefMirror = false);
    void _prepareQueuedEvent(JsonDocument& doc);
    uint32_t _appendQueuedEvent(const char* eventType,
                            JsonDocument& doc,
                            QueueMetric metric);
    void _noteQueuedRecord(QueueMetric metric);
    bool _purgeTransientFiles();
    void _publishDebugLogs();
    bool _publishFileInChunks(const char* topicSuffix,
                              const char* filePath,
                              const char* fileName,
                              size_t chunkSize = 512);
    DumpContext _dumpCtx = {};

    // Timestamp helper — ISO8601 UTC
    void _timestamp(char* buf, int len);
};

extern MQTTManager MQTT_MGR;


