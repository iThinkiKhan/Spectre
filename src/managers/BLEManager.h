#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <NimBLEDevice.h>

#include "../config.h"
#include "../protocol/CompanionProtocol.h"
#include "../core/SpectreState.h"
#include "../security/BleSecureSession.h"
#include "ButtonHandler.h"

/*
    Spectre BLE sideband
    ====================

    This manager is intentionally hardware-only:
    - Core 0 only
    - no LVGL
    - no display calls
    - designed for begin() + tick() from TaskHardware every ~100 ms

    Shared wire protocol details live in protocol/CompanionProtocol.h.
    This manager owns the BLE transport, state publishing, and button routing.
*/

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
    enum LinkState : uint8_t {
        BLE_IDLE = 0,
        BLE_SCANNING,
        BLE_CONNECTING,
        BLE_CONNECTED,
        BLE_SUBSCRIBED
    };

    // Granular reason for the most recent app-layer BLE auth failure.
    // NONE means either auth succeeded or it was never attempted (e.g. scan
    // timeout — phone never found).  Non-NONE means the phone was reached at
    // the GATT level but the P-256 / AES-GCM handshake did not complete.
    enum class BleAuthFailReason : uint8_t {
        NONE = 0,
        AUTH_CHAR_NOT_FOUND,      // auth GATT characteristic absent or read-only
        AUTH_NOTIFY_MISSING,      // subscribe() to auth notification failed
        GATT_WRITE_FAILED,        // writeValue() for challenge frame rejected
        SECURE_RESPONSE_TIMEOUT,  // phone connected but no response before deadline
        AES_GCM_OPEN_FAILED,      // response received but session open failed
        PHONE_DISCONNECTED,       // link dropped while waiting for auth response
    };

    BLEManager();

    bool begin();
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

    // Manual probe — extended-timeout connection attempt for bench testing.
    // Uses a 15 s NimBLE connect timeout instead of the 6 s field default.
    // If already subscribed, returns true immediately (no-op).
    // Otherwise expedites the next scan/reconnect cycle with the probe timeout.
    bool requestManualProbe();
    bool requestCompanionLink(const char* reason, bool allowCachedReconnect = true);

    // Enrichment exchange
    bool requestEnrichmentBatch(const EventBatchRecord* records, size_t count);
    bool consumeEnrichmentBatch(PendingEnrichment* out, size_t maxCount, size_t& outCount);
    bool consumeEnrichmentFailure();
    uint32_t getLastEnrichmentTransferMs() const { return _enrichmentXferMs; }

    // Granular companion readiness checks.
    //
    // Use these instead of isPhoneCompanionReady() when only a subset of
    // capabilities is required, so that a partially-built phone app does not
    // make the device side look entirely broken.
    //
    //   isPhoneLinkReady()       — subscribed (all traffic is possible)
    //   isPhoneGpsReady()        — subscribed + GPS char bound
    //                              NOTE: also check hasFreshGpsFix() to
    //                              confirm the phone is actively publishing.
    //   isPhoneControlReady()    — subscribed + control char bound
    //   isPhoneEnrichmentReady() — subscribed + event-batch + enrichment chars
    //
    bool isPhoneLinkReady()       const { return _state == BLE_SUBSCRIBED; }
    bool isPhoneGpsReady()        const { return _state == BLE_SUBSCRIBED &&
                                                 _gpsRemoteChar != nullptr; }
    bool isPhoneControlReady()    const { return _state == BLE_SUBSCRIBED &&
                                                 _controlRemoteChar != nullptr; }
    bool isPhoneEnrichmentReady() const { return _state == BLE_SUBSCRIBED &&
                                                 _eventBatchRemoteChar != nullptr &&
                                                 _enrichmentRemoteChar != nullptr; }

    // Full-feature check (alias for isPhoneEnrichmentReady).
    // Kept for backward compatibility with existing call sites.
    bool isPhoneCompanionReady() const;

    // One-shot trigger for MQTT layer
    bool consumeWireGuardDumpTrigger();
    bool isDumpConfirmationPending() const { return _wgConfirmPending; }

    bool hasFreshGpsFix() const;
    bool formatBestTimeISO(char* out, size_t len);
    bool getBestTimeEpoch(uint32_t& epochUtc) const;

    LinkState getState() const { return _state; }

    // BLE health diagnostics (for companion status snapshot).
    uint32_t getLastBeginMs()          const { return _lastBeginMs; }
    uint32_t getLastScanStartMs()      const { return _lastScanStartMs; }
    int      getLastDisconnectReason() const { return _lastDisconnectReason; }
    BleAuthFailReason getLastAuthFailReason() const { return _lastAuthFailReason; }

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

    static constexpr uint32_t WORKER_JOB_CONNECT         = 0x00000001UL;
    static constexpr uint32_t WORKER_JOB_POLL_GPS        = 0x00000002UL;
    static constexpr uint32_t WORKER_JOB_POLL_CONTROL    = 0x00000004UL;
    static constexpr uint32_t WORKER_JOB_SEND_ENRICH     = 0x00000008UL;

    static constexpr uint32_t WORKER_STACK_WORDS         = 10240;

    // Lifecycle helpers
    void _buildDeviceName();
    bool _beginFrameworkPhase();
    bool _beginServicesPhase();
    bool _beginWorkerPhase();
    void _abortBegin(const char* phase);
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
    static void _authNotifyThunk(NimBLERemoteCharacteristic* chr,
                                 uint8_t* data,
                                 size_t len,
                                 bool isNotify);

    // Remote service handling
    bool _bindRemoteCharacteristics();
    bool _authenticateRemote();
    void _clearRemoteHandles();
    void _softDisconnectClient(const char* reason);
    void _hardDropClient(const char* reason);

    enum class TargetMatch : uint8_t {
        None         = 0,
        ServiceUuid  = 1,   // cacheable for short-lived reconnect
        NameFallback = 2,   // diagnostic only — never cached past one attempt
    };
    TargetMatch _matchesTarget(const NimBLEAdvertisedDevice* advertisedDevice);

    // Drop any cached peer (address + reconnect bookkeeping) and force a
    // fresh scan on the next idle window.  Always log the reason so the
    // bring-up log shows why a sticky reconnect was abandoned.
    void _clearTargetCache(const char* reason);

    // Renew the active radio lease while a BLE link is up so manual
    // bench testing isn't cut off when the lease expires.
    void _renewBleLeaseIfOwned(const char* reason);
    bool _shouldYieldToUpload() const;
    void _scheduleProbeSoonButNotNow(uint32_t now);

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

    // BLE RX handoff
    void _queueGpsFrameFromCallback(const uint8_t* data, size_t len);
    void _queueControlFrameFromCallback(const uint8_t* data, size_t len);
    void _queueEnrichmentChunkFromCallback(const uint8_t* data, size_t len);
    void _queueAuthFrameFromCallback(const uint8_t* data, size_t len);

    void _drainBleRxQueues();
    void _drainGpsRx();
    void _drainControlRx();
    void _drainEnrichmentRx();

    void _clearBleRxQueues();

    // GPS / control parsing
    void _handleGpsPayload(const uint8_t* data, size_t len);
    void _handleControlPayload(const uint8_t* data, size_t len);
    void _handleEnrichmentPayload(const uint8_t* data, size_t len);
    void _resetEnrichmentExchangeState(bool preserveFailure);
    void _failEnrichment(const char* reason);
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

    SemaphoreHandle_t _rxMutex = nullptr;

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
    uint8_t   _dirtyDisconnectCount = 0;
    bool      _gpsNotifyEnabled = false;
    bool      _controlNotifyEnabled = false;
    bool      _authNotifyEnabled = false;
    bool      _connectResultPending = false;
    bool      _connectResultOk = false;
    bool      _probeConnectPending = false;  // true → next connect uses 15 s timeout
    bool      _manualProbeActive   = false;  // true → fast scan retry until connect or lease expires
    uint32_t  _scanDiagUntilMs = 0;         // bench diag: log advertised devices until this ts
    uint8_t   _scanDiagSeen    = 0;         // bench diag: count capped at 20

    bool      _gpsAvailable = false;
    bool      _timeTrusted = false;
    bool      _wgActive = false;
    bool      _wgConfirmPending = false;
    bool      _wgArmed = false;
    bool      _wgDumpTriggerLatched = false;

    bool      _enrichmentRequestPending = false;
    bool      _enrichmentInFlight = false;
    bool      _enrichmentReady = false;
    bool      _enrichmentFailed = false;
    bool      _enrichmentNotifyEnabled = false;
    bool      _enrichmentSendQueued = false;
    bool      _enrichmentBatchAcked = false;
    uint32_t  _enrichmentSendMs = 0;
    uint32_t  _enrichmentXferMs = 0;
    uint32_t  _enrichmentWaitStartMs = 0;

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
    uint32_t _lastBeginMs = 0;
    uint32_t _lastLeaseRenewMs = 0;
    int      _lastDisconnectReason = 0;
    BleAuthFailReason _lastAuthFailReason = BleAuthFailReason::NONE;

    float     _gpsLat = 0.0f;
    float     _gpsLon = 0.0f;
    float     _gpsAlt = 0.0f;
    float     _gpsAccuracy = 0.0f;

    char      _sessionId[40];
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

    // ─────────────────────────────────────────────
    // BLE callback RX handoff
    // Callbacks may only copy into these buffers.
    // Parsing/state mutation happens from tick().
    // ─────────────────────────────────────────────

    static constexpr size_t GPS_RX_MAX =
        PHONE_GPS_FRAME_SIZE + PHONE_SECURE_ENVELOPE_OVERHEAD;
    static constexpr size_t CONTROL_RX_MAX =
        PHONE_CONTROL_FRAME_SIZE + PHONE_SECURE_ENVELOPE_OVERHEAD;

    // Keep this at one negotiated BLE payload chunk, not a whole batch.
    // 244 is the usual max payload after MTU 247 negotiation.
    // If your current companion code uses a smaller known max, use that instead.
    static constexpr size_t ENRICH_RX_CHUNK_MAX = 244;

    // Enough for several notify/write chunks without heap use.
    // 8 slots = 1952 bytes static RAM at 244 bytes each.
    static constexpr uint8_t ENRICH_RX_SLOTS = 8;

    struct BleRxChunk {
        uint16_t len = 0;
        uint8_t  data[ENRICH_RX_CHUNK_MAX] = {};
    };

    // GPS is latest-value. Dropping old GPS is acceptable.
    bool    _gpsRxPending = false;
    uint8_t _gpsRxBuf[GPS_RX_MAX] = {};
    size_t  _gpsRxLen = 0;
    uint16_t _gpsRxDrops = 0;

    // Control can be latest-value for now unless you need command ordering.
    // For first phone connection, latest command is good enough.
    bool    _controlRxPending = false;
    uint8_t _controlRxBuf[CONTROL_RX_MAX] = {};
    size_t  _controlRxLen = 0;
    uint16_t _controlRxDrops = 0;

    // Enrichment must preserve chunk order.
    BleRxChunk _enrichRx[ENRICH_RX_SLOTS];
    uint8_t _enrichRxHead = 0;
    uint8_t _enrichRxTail = 0;
    uint8_t _enrichRxCount = 0;
    uint16_t _enrichRxDrops = 0;

    bool    _authRxPending = false;
    uint8_t _authRxBuf[PHONE_AUTH_FRAME_SIZE] = {};
    size_t  _authRxLen = 0;
    uint16_t _authRxDrops = 0;

    // Scratch buffer used by tick-side drain.
    // Static member, not local stack.
    uint8_t _enrichRxScratch[ENRICH_RX_CHUNK_MAX] = {};

    static constexpr size_t ENRICHMENT_MAX_RECORDS = PHONE_COMPANION_ENRICH_BATCH_MAX;
    static_assert(ENRICHMENT_MAX_RECORDS > 0, "PHONE_COMPANION_ENRICH_BATCH_MAX must be > 0");
    static_assert(ENRICHMENT_MAX_RECORDS <= 64,
                  "PHONE_COMPANION_ENRICH_BATCH_MAX >64 needs a stack/buffer audit");

    uint8_t   _eventBatchTxBuf[ENRICHMENT_MAX_RECORDS * EVENT_BATCH_RECORD_SIZE];
    size_t    _eventBatchTxLen = 0;
    uint8_t   _eventBatchSecureTxBuf[ENRICHMENT_MAX_RECORDS * EVENT_BATCH_RECORD_SIZE +
                                      PHONE_SECURE_ENVELOPE_OVERHEAD];
    uint8_t   _enrichmentRxBuf[ENRICHMENT_MAX_RECORDS * ENRICHMENT_RECORD_SIZE];
    size_t    _enrichmentRxLen = 0;
    size_t    _enrichmentExpectedCount = 0;
    size_t    _enrichmentAvailableCount = 0;
    PendingEnrichment _enrichmentBatch[ENRICHMENT_MAX_RECORDS];

    NimBLEAddress               _targetAddress;
    bool                        _haveTargetAddress = false;
    // True only after GATT service discovery confirms PHONE_SERVICE_UUID on a
    // service-UUID advertisement match.  Name-fallback matches never set this.
    // Do NOT set this at advertisement time — Android RPA addresses are volatile
    // until the connection + service discovery succeeds.
    bool                        _targetCachedFromService      = false;
    uint32_t                    _targetCachedAtMs             = 0;
    uint32_t                    _lastTargetSeenMs             = 0;
    int8_t                      _lastTargetRssi               = 0;
    // Set when the advertisement was matched by service UUID; cleared once
    // _bindRemoteCharacteristics commits the cache (or _clearTargetCache runs).
    bool                        _pendingConnectIsServiceMatch = false;
    // Set in _onAdvertisedDevice after scan is stopped; cleared in tick() once
    // the 250 ms drain delay has elapsed and _startConnectAttempt fires.
    bool                        _connectPendingAfterScan      = false;

    NimBLEScan*                 _scan = nullptr;
    NimBLEClient*               _client = nullptr;
    NimBLERemoteService*        _remoteService = nullptr;
    NimBLERemoteCharacteristic* _gpsRemoteChar = nullptr;
    NimBLERemoteCharacteristic* _controlRemoteChar = nullptr;
    NimBLERemoteCharacteristic* _metaRemoteChar = nullptr;
    NimBLERemoteCharacteristic* _eventBatchRemoteChar = nullptr;
    NimBLERemoteCharacteristic* _enrichmentRemoteChar = nullptr;
    NimBLERemoteCharacteristic* _authRemoteChar = nullptr;
    BleSecureSession            _secureSession;
    uint8_t                     _authChallengeBuf[PHONE_AUTH_FRAME_SIZE] = {};
    uint8_t                     _authResponseBuf[PHONE_AUTH_FRAME_SIZE] = {};

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
