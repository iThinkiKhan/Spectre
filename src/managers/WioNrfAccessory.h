#pragma once

#include <Arduino.h>

#include "../config.h"
#include "protocol/CompanionProtocol.h"
#include "../security/BleSecureSession.h"
#include "BLEManager.h"

class WioNrfAccessory {
public:
    enum LinkState : uint8_t {
        LINK_IDLE = 0,
        LINK_PROBING,
        LINK_AUTHENTICATING,
        LINK_READY,
        LINK_SENDING_BATCH,
        LINK_WAITING_ENRICHMENT,
        LINK_FAILED
    };

    bool begin(uint32_t detectTimeoutMs = 750);
    void tick();

    bool available() const { return _present; }
    bool hasBleProxy() const { return _present && _bleProxy; }
    bool hasSx1262() const { return _present && _sx1262Present; }
    bool phoneConnected() const { return _phoneConnected; }
    int phoneRssi() const { return _phoneRssi; }
    const char* phoneState() const { return _phone[0] ? _phone : "unknown"; }
    uint32_t lastSeenMs() const { return _lastSeenMs; }
    LinkState linkState() const { return _linkState; }

    bool requestCompanionLink(const char* reason, bool allowCachedReconnect = true);
    bool isPhoneCompanionReady() const {
        return _phoneConnected && _linkState == LINK_READY && _secureSession.isReady();
    }
    bool isCompanionLinkBusy() const {
        return _linkState != LINK_IDLE &&
               _linkState != LINK_READY &&
               _linkState != LINK_FAILED;
    }
    bool isEnrichmentExchangeActive() const {
        return _linkState == LINK_SENDING_BATCH ||
               _linkState == LINK_WAITING_ENRICHMENT;
    }
    bool requestEnrichmentBatch(const EventBatchRecord* records, size_t count);
    bool consumeEnrichmentBatch(PendingEnrichment* out, size_t maxCount, size_t& outCount);
    bool consumeEnrichmentFailure();
    uint32_t getLastEnrichmentTransferMs() const { return _enrichmentXferMs; }

    // Live phone GPS — mirrors BLEManager.  Set by encrypted PhoneGpsFrameV1
    // frames the phone publishes; WIO forwards the raw envelope and we decrypt
    // here so the secret never leaves the ESP.
    bool hasFreshGpsFix() const;
    bool getBestTimeEpoch(uint32_t& epochUtc) const;
    bool isTimeTrusted() const { return _timeTrusted; }
    float gpsLat() const { return _gpsLat; }
    float gpsLon() const { return _gpsLon; }
    float gpsAlt() const { return _gpsAlt; }
    float gpsAccuracy() const { return _gpsAccuracy; }

    // Encrypts the storage frame and asks the WIO to write it to the phone's
    // storage characteristic.  Mirrors BLEManager::publishStorageSnapshot.
    bool publishStorageSnapshot(const PhoneStorageFrameV1& frame);

    // Encrypts a log-stream chunk plaintext and writes it to the phone's
    // log-stream characteristic over the WIO proxy (slice #3).
    bool publishLogStreamChunk(const uint8_t* plain, size_t plainLen);

    // Same shape for dashboard-stream chunks (slice #4).
    bool publishDashboardStreamChunk(const uint8_t* plain, size_t plainLen);

    // Pushes a throttled phone notification frame over the WIO proxy.
    bool publishNotification(const uint8_t* plain, size_t plainLen);

    // Text input over WIO.  The WIO firmware exposes the existing text
    // service to the phone; ESP drives the lifecycle.  Text bytes are not
    // encrypted with the companion secure session — they live on a separate
    // GATT service exactly like the internal BLE path.
    bool requestTextInput(const char* prompt, uint32_t timeoutMs = 120000UL);
    bool consumeTextInput(char* out, size_t outLen);
    void cancelTextInput(const char* reason = nullptr);
    bool isTextInputPending() const { return _textInputPending; }
    bool isTextInputReady() const { return _textInputReady; }
    const char* phonePromptText() const { return _promptBuf; }

    const char* linkStateName() const;

    bool sendLine(const char* line);
    bool readLine(char* out, size_t outLen);
    void requestBleStatus();
    void disconnectPhone(const char* reason = nullptr);

private:
    static constexpr size_t UART_LINE_MAX = 640;
    // Mirrors wio_nrf_firmware's UART_PAYLOAD_MAX. Every S3->nRF->phone
    // encrypted write must fit this single relay payload.
    static constexpr size_t WIO_RELAY_PAYLOAD_MAX = 256;
    static constexpr size_t BLE_WRITE_VALUE_MAX = WIO_RELAY_PAYLOAD_MAX;
    static constexpr size_t EVENT_BATCH_SECURE_FRAME_MAX =
        PHONE_COMPANION_ENRICH_BATCH_MAX * EVENT_BATCH_RECORD_SIZE +
        PHONE_SECURE_ENVELOPE_OVERHEAD;
    static_assert(PHONE_AUTH_FRAME_SIZE <= BLE_WRITE_VALUE_MAX,
                  "AUTH frame exceeds WIO relay payload");
    static_assert(EVENT_BATCH_SECURE_FRAME_MAX <= BLE_WRITE_VALUE_MAX,
                  "Event batch envelope exceeds WIO relay payload");
    static_assert(PHONE_COMMAND_RESP_FRAME_MAX + PHONE_SECURE_ENVELOPE_OVERHEAD <=
                      BLE_WRITE_VALUE_MAX,
                  "Command response envelope exceeds WIO relay payload");
    static_assert(LOG_STREAM_CHUNK_FRAME_MAX + PHONE_SECURE_ENVELOPE_OVERHEAD <=
                      BLE_WRITE_VALUE_MAX,
                  "Log stream envelope exceeds WIO relay payload");
    static constexpr uint32_t HELLO_RETRY_INTERVAL_MS = 1000UL;
    static constexpr uint32_t PROBE_TIMEOUT_MS = 12000UL;
    static constexpr uint32_t AUTH_TIMEOUT_MS = 8000UL;
    static constexpr uint32_t ENRICHMENT_TIMEOUT_MS = 25000UL;
    static constexpr uint32_t READY_IDLE_DROP_MS = 10000UL;
    static constexpr uint32_t RECOVERY_STATUS_INTERVAL_MS = 5000UL;
    static constexpr uint32_t RX_BIN_TIMEOUT_MS = 1500UL;
    static constexpr uint32_t FAILED_IDLE_RECOVERY_MS = 15000UL;

    HardwareSerial* _selectSerial();
    void _handleLine(const char* line);
    void _handleCapsLine(const char* caps);
    void _handleStatusLine(const char* status);
    void _handleProbeLine(const char* status);
    void _handleWriteLine(const char* status);
    void _handleRxLine(const char* status);
    void _handleRxBytes(const char* chr, const uint8_t* data, size_t len);
    void _markSeen();
    bool _tokenPresent(const char* text, const char* token) const;
    bool _readKeyValue(const char* text, const char* key, char* out, size_t outLen) const;
    bool _readUintValue(const char* text, const char* key, uint32_t& out) const;
    void _setLinkState(LinkState state);
    void _fail(const char* reason);
    void _recoverFailedLink(const char* reason);
    void _abortRxBinary(const char* reason);
    void _handlePhoneDisconnected(const char* reason, bool markFailure);
    uint32_t _nextSeq();
    bool _sendProbe(const char* reason);
    bool _sendBleWrite(const char* chr, const uint8_t* data, size_t len,
                       uint32_t* seqOut = nullptr);
    bool _sendBleWriteBinary(const char* chr, const uint8_t* data, size_t len,
                             uint32_t* seqOut = nullptr);
    bool _sendBleWriteHex(const char* chr, const uint8_t* data, size_t len,
                          uint32_t* seqOut = nullptr);
    void _handleAuthBytes(const uint8_t* data, size_t len);
    void _handleControlBytes(const uint8_t* data, size_t len);
    void _handleEnrichmentBytes(const uint8_t* data, size_t len);
    void _handleGpsBytes(const uint8_t* data, size_t len);
    void _handleTextInputBytes(const uint8_t* data, size_t len);
    void _handleCommandRequestBytes(const uint8_t* data, size_t len);
    void _touchReadyActivity();
    bool _validateGpsFix(float lat, float lon, float alt, float accuracy,
                         uint32_t epochUtc) const;
    void _setGpsUnavailable(bool clearCoordinates);
    void _clearTextInputState(const char* reason);
    void _checkTextInputTimeout(uint32_t now);
    bool _encodeHex(const uint8_t* data, size_t len, char* out, size_t outLen) const;
    bool _decodeHex(const char* hex, uint8_t* out, size_t outCap, size_t& outLen) const;
    uint8_t _hexNibble(char c) const;

    HardwareSerial* _serial = nullptr;
    bool _begun = false;
    bool _present = false;
    bool _bleProxy = false;
    bool _uartCap = false;
    bool _bleWriteBin = false;
    bool _sx1262Present = false;
    bool _phoneConnected = false;
    bool _lineOverflow = false;
    bool _rxBinActive = false;
    bool _enrichmentFailed = false;
    bool _enrichmentReady = false;
    bool _dropAfterReady = false;
    uint32_t _lastSeenMs = 0;
    uint32_t _lastHelloMs = 0;
    uint32_t _lastStatusRequestMs = 0;
    uint32_t _lastRecoveryStatusMs = 0;
    uint32_t _lastOverflowLogMs = 0;
    uint32_t _seq = 0;
    uint32_t _activeSeq = 0;
    uint32_t _pendingBatchWriteSeq = 0;
    uint32_t _stateStartedMs = 0;
    uint32_t _readySinceMs = 0;
    uint32_t _rxBinStartedMs = 0;
    uint32_t _enrichmentSendMs = 0;
    uint32_t _enrichmentWaitStartMs = 0;
    uint32_t _enrichmentXferMs = 0;
    size_t _enrichmentExpectedCount = 0;
    size_t _enrichmentRxLen = 0;
    size_t _enrichmentAvailableCount = 0;
    int _phoneRssi = 0;
    char _phone[16] = "unknown";
    char _line[UART_LINE_MAX] = {};
    size_t _lineLen = 0;
    char _rxBinChr[16] = {};
    size_t _rxBinExpected = 0;
    size_t _rxBinLen = 0;
    LinkState _linkState = LINK_IDLE;
    BleSecureSession _secureSession;
    uint8_t _authChallengeBuf[PHONE_AUTH_FRAME_SIZE] = {};
    uint8_t _uartRxBuf[256] = {};
    uint8_t _eventBatchTxBuf[PHONE_COMPANION_ENRICH_BATCH_MAX * EVENT_BATCH_RECORD_SIZE] = {};
    uint8_t _eventBatchSecureTxBuf[PHONE_COMPANION_ENRICH_BATCH_MAX * EVENT_BATCH_RECORD_SIZE +
                                   PHONE_SECURE_ENVELOPE_OVERHEAD] = {};
    uint8_t _enrichmentRxBuf[PHONE_COMPANION_ENRICH_BATCH_MAX * ENRICHMENT_RECORD_SIZE] = {};
    PendingEnrichment _enrichmentBatch[PHONE_COMPANION_ENRICH_BATCH_MAX] = {};

    // GPS state — mirrors BLEManager.  Updated when the phone publishes a
    // PhoneGpsFrameV1 over the (encrypted) GPS channel.
    bool      _gpsAvailable = false;
    bool      _timeTrusted = false;
    uint32_t  _lastGpsFixMs = 0;
    uint32_t  _gpsEpochAtFix = 0;
    float     _gpsLat = 0.0f;
    float     _gpsLon = 0.0f;
    float     _gpsAlt = 0.0f;
    float     _gpsAccuracy = 0.0f;

    // Text input state — mirrors BLEManager, unencrypted text service.
    bool      _textInputPending = false;
    bool      _textInputReady = false;
    uint32_t  _textInputDeadlineMs = 0;
    uint32_t  _textInputToken = 0;
    char      _promptBuf[24] = {};
    char      _resultBuf[64] = {};

    uint8_t   _storageSecureTxBuf[PHONE_STORAGE_FRAME_SIZE + PHONE_SECURE_ENVELOPE_OVERHEAD] = {};

    // Phone command/control buffers — mirror BLEManager's command-channel
    // storage so a WIO-routed request follows the same lifecycle.
    uint8_t   _commandRespPlainBuf[PHONE_COMMAND_RESP_FRAME_MAX] = {};
    uint8_t   _commandRespSecureBuf[PHONE_COMMAND_RESP_FRAME_MAX +
                                    PHONE_SECURE_ENVELOPE_OVERHEAD] = {};
    uint8_t   _logStreamSecureBuf[LOG_STREAM_CHUNK_FRAME_MAX +
                                  PHONE_SECURE_ENVELOPE_OVERHEAD] = {};
    uint8_t   _dashboardStreamSecureBuf[DASHBOARD_STREAM_CHUNK_FRAME_MAX +
                                        PHONE_SECURE_ENVELOPE_OVERHEAD] = {};
    uint8_t   _notificationSecureBuf[PHONE_NOTIFICATION_FRAME_MAX +
                                     PHONE_SECURE_ENVELOPE_OVERHEAD] = {};
};

extern WioNrfAccessory WIO_NRF;
