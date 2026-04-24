
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <NimBLEDevice.h>

#include "../core/SpectreState.h"
#include "ButtonHandler.h"

/*
    Spectre BLE sideband
    ====================

    This manager is intentionally hardware-only:
    - Core 0 only
    - no LVGL
    - no display calls
    - designed for begin() + tick() from TaskHardware every ~100 ms

    It runs BLE in two roles at once:

    1. Spectre as a GATT client to a phone companion peripheral
       Service UUID: 84f03a80-6d7b-4d4d-9a64-6b2d6f3a0001

       Characteristic: GPS fix (read + notify)
       UUID: 84f03a80-6d7b-4d4d-9a64-6b2d6f3a0002
       Binary payload, packed little-endian PhoneGpsFrameV1:

           struct PhoneGpsFrameV1 {
               uint8_t  version;      // must be 1
               int32_t  latE7;        // latitude  * 1e7
               int32_t  lonE7;        // longitude * 1e7
               int32_t  altCm;        // altitude in centimeters
               uint16_t accuracyDm;   // horizontal accuracy in decimeters
               uint32_t epochUtc;     // Unix UTC seconds
               uint8_t  flags;        // bit0 valid, bit1 trusted time
           };

       Characteristic: phone control (read + notify)
       UUID: 84f03a80-6d7b-4d4d-9a64-6b2d6f3a0003
       Binary payload, packed little-endian PhoneControlFrameV1:

           struct PhoneControlFrameV1 {
               uint8_t  version;      // must be 1
               uint8_t  flags;        // bit0 WireGuard active
                                       // bit1 immediate dump requested
                                       // bit2 cancel pending request
               uint16_t counter;      // monotonically increasing request id
           };

       Characteristic: companion metadata (read)
       UUID: 84f03a80-6d7b-4d4d-9a64-6b2d6f3a0004
       UTF-8 string, suggested format:
           "app=SpectrePhone;ver=1.0.0"

    2. Spectre as a GATT server for text entry from the phone app
       Service UUID: 84f03a80-6d7b-4d4d-9a64-6b2d6f3a1001

       Characteristic: prompt (read + notify)
       UUID: 84f03a80-6d7b-4d4d-9a64-6b2d6f3a1002
       UTF-8 reason string:
           "WiFi password for: Nothing But Net"

       Characteristic: input (write + write without response)
       UUID: 84f03a80-6d7b-4d4d-9a64-6b2d6f3a1003
       UTF-8 input payload, max 63 bytes plus null terminator on-device

       Characteristic: receipt (read + notify)
       UUID: 84f03a80-6d7b-4d4d-9a64-6b2d6f3a1004
       UTF-8 ack string:
           "IDLE", "PENDING", "RECEIVED", "CONSUMED",
           "BUSY", "REJECTED", "TIMEOUT", "CANCELLED"

       Characteristic: status (read + notify)
       UUID: 84f03a80-6d7b-4d4d-9a64-6b2d6f3a1005
       UTF-8 semicolon-delimited status string:
           "sess=<id>;state=SUBSCRIBED;gps=1;input=PENDING;wg=ARMED;tok=4"

    WireGuard confirmation flow
    ===========================

    The phone companion raises bit1 in PhoneControlFrameV1 to request
    an immediate MQTT dump. Spectre never fires that dump immediately.
    Instead it enters a guarded, two-step hardware-button confirm path:

    1. BTN_A_LONG arms the request for a short window.
    2. BTN_B_LONG while armed confirms the dump trigger.
    3. Any short press, timeout, or explicit cancel frame aborts it.

    This is deliberate enough for field use without requiring a display call.
    Integration should forward button events through handleButtonEvent() first.

    Required SpectreState additions before integrating this manager
    ===============================================================

    // GPS
    bool     gpsAvailable      = false;
    float    gpsLat            = 0.0f;
    float    gpsLon            = 0.0f;
    float    gpsAlt            = 0.0f;
    float    gpsAccuracy       = 0.0f;
    uint32_t gpsLastFix        = 0;
    char     gpsTimeISO[24]    = "";

    // BLE
    bool     bleConnected      = false;

    // Text input
    bool     textInputPending   = false;
    char     textInputPrompt[24] = "";
    char     textInputResult[64] = "";
    bool     textInputReady     = false;

    // WireGuard
    bool     wgDumpTriggered   = false;
*/

struct __attribute__((packed)) PhoneGpsFrameV1 {
    uint8_t  version;
    int32_t  latE7;
    int32_t  lonE7;
    int32_t  altCm;
    uint16_t accuracyDm;
    uint32_t epochUtc;
    uint8_t  flags;
};

struct __attribute__((packed)) PhoneControlFrameV1 {
    uint8_t  version;
    uint8_t  flags;
    uint16_t counter;
};

struct __attribute__((packed)) EventBatchRecord {
    uint32_t eventId;
    uint32_t timestampMs;
    uint8_t  type;
    uint8_t  status;
};

struct PendingEnrichment {
    uint32_t eventId;
    float    lat;
    float    lon;
    float    alt;
    float    accuracy;
    char     tag[32];
};

class BLEManager {
public:
    static constexpr const char* PHONE_SERVICE_UUID       = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0001";
    static constexpr const char* PHONE_GPS_CHAR_UUID      = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0002";
    static constexpr const char* PHONE_CONTROL_CHAR_UUID  = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0003";
    static constexpr const char* PHONE_META_CHAR_UUID     = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0004";
    static constexpr const char* PHONE_EVENT_BATCH_UUID   = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0005";
    static constexpr const char* PHONE_ENRICHMENT_UUID    = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0006";

    static constexpr const char* TEXT_SERVICE_UUID        = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1001";
    static constexpr const char* TEXT_PROMPT_CHAR_UUID    = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1002";
    static constexpr const char* TEXT_INPUT_CHAR_UUID     = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1003";
    static constexpr const char* TEXT_RECEIPT_CHAR_UUID   = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1004";
    static constexpr const char* TEXT_STATUS_CHAR_UUID    = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1005";

    enum LinkState : uint8_t {
        BLE_IDLE = 0,
        BLE_SCANNING,
        BLE_CONNECTING,
        BLE_CONNECTED,
        BLE_SUBSCRIBED
    };

    BLEManager();

    void begin();
    void shutdown();
    void tick();
    void setRadioEnabled(bool enabled);
    bool isBegun() const { return _begun; }
    bool isRadioEnabled() const { return _radioEnabled; }

    void setTargetDeviceName(const char* deviceName);
    void setTargetServiceUUID(const char* serviceUuid);

    // Returns true if BLE consumed the event.
    bool handleButtonEvent(ButtonEvent evt);

    // Text input handoff
    bool requestTextInput(const char* reason, uint32_t timeoutMs = 120000UL);
    bool consumeTextInput(char* out, size_t outLen);
    void cancelTextInput();
    bool isTextInputPending() const { return _textInputPending; }
    bool isTextInputReady()   const { return _textInputReady; }

    // Enrichment exchange
    bool requestEnrichmentBatch(const EventBatchRecord* records, size_t count);
    bool consumeEnrichmentBatch(PendingEnrichment* out, size_t maxCount, size_t& outCount);
    bool isPhoneCompanionReady() const;

    // One-shot trigger for MQTT layer
    bool consumeWireGuardDumpTrigger();
    bool isDumpConfirmationPending() const { return _wgConfirmPending; }

    bool hasFreshGpsFix() const;
    bool formatBestTimeISO(char* out, size_t len);
    bool getBestTimeEpoch(uint32_t& epochUtc) const;

    LinkState getState() const { return _state; }

private:
    class ScanCallbacks : public NimBLEScanCallbacks {
    public:
        explicit ScanCallbacks(BLEManager& owner) : _owner(owner) {}
        void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override;
    private:
        BLEManager& _owner;
    };

    class ClientCallbacks : public NimBLEClientCallbacks {
    public:
        explicit ClientCallbacks(BLEManager& owner) : _owner(owner) {}
        void onConnect(NimBLEClient* pClient) override;
        void onDisconnect(NimBLEClient* pClient, int reason) override;
        bool onConnParamsUpdateRequest(NimBLEClient* pClient,
                                       const ble_gap_upd_params* params) override;
        void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;
    private:
        BLEManager& _owner;
    };

    class ServerCallbacks : public NimBLEServerCallbacks {
    public:
        explicit ServerCallbacks(BLEManager& owner) : _owner(owner) {}
        void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override;
        void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override;
    private:
        BLEManager& _owner;
    };

    class TextInputCallbacks : public NimBLECharacteristicCallbacks {
    public:
        explicit TextInputCallbacks(BLEManager& owner) : _owner(owner) {}
        void onWrite(NimBLECharacteristic* pCharacteristic,
                     NimBLEConnInfo& connInfo) override;
    private:
        BLEManager& _owner;
    };

    static constexpr uint8_t PHONE_GPS_FLAG_VALID        = 0x01;
    static constexpr uint8_t PHONE_GPS_FLAG_TIME_TRUSTED = 0x02;

    static constexpr uint8_t PHONE_CTRL_FLAG_WG_ACTIVE   = 0x01;
    static constexpr uint8_t PHONE_CTRL_FLAG_DUMP_REQ    = 0x02;
    static constexpr uint8_t PHONE_CTRL_FLAG_CANCEL      = 0x04;
    static constexpr uint8_t PHONE_CTRL_FLAG_BATCH_RX    = 0x08;

    static constexpr uint32_t WORKER_JOB_CONNECT         = 0x00000001UL;
    static constexpr uint32_t WORKER_JOB_POLL_GPS        = 0x00000002UL;
    static constexpr uint32_t WORKER_JOB_POLL_CONTROL    = 0x00000004UL;
    static constexpr uint32_t WORKER_JOB_SEND_ENRICH     = 0x00000008UL;

    static constexpr uint32_t WORKER_STACK_WORDS         = 1536;

    // Lifecycle helpers
    void _buildDeviceName();
    void _setupServer();
    void _setupScanner();
    void _resetState();

    // Tick helpers
    void _startScanWindow();
    void _stopScanWindow();
    void _startConnectAttempt();
    void _scheduleReconnect(const char* reason);
    void _handleConnectOutcome();
    void _checkTimeouts();
    void _queueWorker(uint32_t bits);
    void _ensureAdvertising(bool enable);

    // Worker
    static void _workerTaskEntry(void* arg);
    void _workerLoop();
    void _doConnectJob();
    void _doGpsPollJob();
    void _doControlPollJob();
    void _doEnrichmentSendJob();

    // NimBLE callbacks
    void _onAdvertisedDevice(const NimBLEAdvertisedDevice* advertisedDevice);
    void _onClientConnected(NimBLEClient* pClient);
    void _onClientDisconnected(NimBLEClient* pClient, int reason);
    void _onServerConnected(const NimBLEConnInfo* connInfo);
    void _onServerDisconnected(const NimBLEConnInfo* connInfo, int reason);
    void _onTextInputWrite(NimBLECharacteristic* pCharacteristic);

    static void _gpsNotifyThunk(NimBLERemoteCharacteristic* chr,
                                uint8_t* data,
                                size_t len,
                                bool isNotify);
    static void _controlNotifyThunk(NimBLERemoteCharacteristic* chr,
                                    uint8_t* data,
                                    size_t len,
                                    bool isNotify);
    static void _enrichmentNotifyThunk(NimBLERemoteCharacteristic* chr,
                                       uint8_t* data,
                                       size_t len,
                                       bool isNotify);

    // Remote service handling
    bool _bindRemoteCharacteristics();
    void _clearRemoteHandles();
    bool _matchesTarget(const NimBLEAdvertisedDevice* advertisedDevice);

    // WireGuard confirm flow
    void _armWireGuardConfirmation();
    void _confirmWireGuardDump();
    void _cancelWireGuardConfirmation(const char* reason);

    // Text input flow
    void _setReceipt(const char* code, bool notify = true);
    void _setPrompt(const char* prompt, bool notify = true);
    void _refreshStatusCharacteristic(bool notify = true);
    bool _acceptTextPayload(const uint8_t* data, size_t len);
    void _clearTextInputState(bool clearPrompt, const char* receiptCode);

    // GPS / control parsing
    void _handleGpsPayload(const uint8_t* data, size_t len);
    void _handleControlPayload(const uint8_t* data, size_t len);
    void _handleEnrichmentPayload(const uint8_t* data, size_t len);
    bool _validateGpsFix(float lat, float lon, float alt, float accuracy,
                         uint32_t epochUtc) const;
    void _setGpsUnavailable(bool clearCoordinates);

    // Shared-state publishing
    void _publishBleState();
    void _publishGpsState();
    void _publishTextInputState();
    void _pushNotification(uint8_t type, const char* text);

    // Time helpers
    bool _parseIso8601(const char* iso, uint32_t& epochUtc) const;
    void _formatIso8601(uint32_t epochUtc, char* out, size_t len) const;
    static int32_t _daysFromCivil(int32_t y, uint32_t m, uint32_t d);
    static void _civilFromDays(int32_t z, int32_t& y, uint32_t& m, uint32_t& d);

    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

    LinkState _state = BLE_IDLE;
    bool      _begun = false;
    bool      _radioEnabled = false;

    bool      _scanActive = false;
    bool      _targetFound = false;
    bool      _directReconnectPending = false;
    uint8_t   _reconnectAttempt = 0;

    bool      _clientConnected = false;
    bool      _serverConnected = false;
    bool      _ignoreDisconnectOnce = false;
    bool      _gpsNotifyEnabled = false;
    bool      _controlNotifyEnabled = false;
    bool      _connectResultPending = false;
    bool      _connectResultOk = false;

    bool      _gpsAvailable = false;
    bool      _timeTrusted = false;
    bool      _wgActive = false;
    bool      _wgConfirmPending = false;
    bool      _wgArmed = false;
    bool      _wgDumpTriggerLatched = false;

    bool      _enrichmentRequestPending = false;
    bool      _enrichmentInFlight = false;
    bool      _enrichmentReady = false;
    bool      _enrichmentNotifyEnabled = false;
    bool      _enrichmentSendQueued = false;
    bool      _enrichmentBatchAcked = false;

    bool      _textInputPending = false;
    bool      _textInputReady = false;
    bool      _advertisingActive = false;

    uint16_t  _textInputToken = 0;
    uint16_t  _lastWgCounter = 0;

    uint32_t  _nextActionMs = 0;
    uint32_t  _scanStartedMs = 0;
    uint32_t  _connectStartedMs = 0;
    uint32_t  _lastGpsPollMs = 0;
    uint32_t  _lastControlPollMs = 0;
    uint32_t  _lastStatusRefreshMs = 0;
    uint32_t  _lastGpsFixMs = 0;
    uint32_t  _gpsEpochAtFix = 0;
    uint32_t  _textInputDeadlineMs = 0;
    uint32_t  _wgConfirmDeadlineMs = 0;
    uint32_t  _wgArmDeadlineMs = 0;
    uint32_t  _enrichmentDeadlineMs = 0;
    uint32_t  _lastStackLogMs = 0;
    uint32_t  _workerMinFreeStackBytes = 0;
    uint32_t _lastScanStartMs = 0;
    uint32_t _nextScanAllowedMs = 0;

    float     _gpsLat = 0.0f;
    float     _gpsLon = 0.0f;
    float     _gpsAlt = 0.0f;
    float     _gpsAccuracy = 0.0f;

    char      _sessionId[20];
    char      _deviceName[24];
    char      _targetDeviceName[24];
    char      _targetServiceUUID[40];
    char      _connectedDeviceName[24];
    char      _connectedPeerAddr[24];
    char      _gpsTimeIso[24];
    char      _promptBuf[24];
    char      _resultBuf[64];
    char      _receiptBuf[24];
    char      _statusBuf[72];
    char      _metadataBuf[24];

    static constexpr size_t ENRICHMENT_MAX_RECORDS = 24;
    static constexpr size_t EVENT_BATCH_RECORD_SIZE = sizeof(EventBatchRecord);
    static constexpr size_t ENRICHMENT_RECORD_SIZE = 47;

    uint8_t   _eventBatchTxBuf[ENRICHMENT_MAX_RECORDS * EVENT_BATCH_RECORD_SIZE];
    size_t    _eventBatchTxLen = 0;
    uint8_t   _enrichmentRxBuf[ENRICHMENT_MAX_RECORDS * ENRICHMENT_RECORD_SIZE];
    size_t    _enrichmentRxLen = 0;
    size_t    _enrichmentExpectedCount = 0;
    size_t    _enrichmentAvailableCount = 0;
    PendingEnrichment _enrichmentBatch[ENRICHMENT_MAX_RECORDS];

    NimBLEAddress               _targetAddress;
    bool                        _haveTargetAddress = false;

    NimBLEScan*                 _scan = nullptr;
    NimBLEClient*               _client = nullptr;
    NimBLERemoteService*        _remoteService = nullptr;
    NimBLERemoteCharacteristic* _gpsRemoteChar = nullptr;
    NimBLERemoteCharacteristic* _controlRemoteChar = nullptr;
    NimBLERemoteCharacteristic* _metaRemoteChar = nullptr;
    NimBLERemoteCharacteristic* _eventBatchRemoteChar = nullptr;
    NimBLERemoteCharacteristic* _enrichmentRemoteChar = nullptr;

    NimBLEServer*               _server = nullptr;
    NimBLEService*              _textService = nullptr;
    NimBLECharacteristic*       _promptChar = nullptr;
    NimBLECharacteristic*       _inputChar = nullptr;
    NimBLECharacteristic*       _receiptChar = nullptr;
    NimBLECharacteristic*       _statusChar = nullptr;

    TaskHandle_t                _workerTask = nullptr;
    StaticTask_t                _workerTaskBuffer;
    StackType_t*                _workerStack = nullptr;

    ScanCallbacks               _scanCallbacks;
    ClientCallbacks             _clientCallbacks;
    ServerCallbacks             _serverCallbacks;
    TextInputCallbacks          _textInputCallbacks;
};

extern BLEManager BLE_MGR;


