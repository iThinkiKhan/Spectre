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
    // Non-fatal batch drop: host should re-request, not end the session.
    bool consumeEnrichmentBatchDropped();
    uint32_t getLastEnrichmentTransferMs() const { return _enrichmentXferMs; }

    // Live phone GPS via encrypted WIO-forwarded frames.
    bool hasFreshGpsFix() const;
    bool getBestTimeEpoch(uint32_t& epochUtc) const;
    bool isTimeTrusted() const { return _timeTrusted; }
    float gpsLat() const { return _gpsLat; }
    float gpsLon() const { return _gpsLon; }
    float gpsAlt() const { return _gpsAlt; }
    float gpsAccuracy() const { return _gpsAccuracy; }

    bool publishStorageSnapshot(const PhoneStorageFrameV1& frame);

    bool publishLogStreamChunk(const uint8_t* plain, size_t plainLen);

    bool publishDashboardStreamChunk(const uint8_t* plain, size_t plainLen);

    bool publishNotification(const uint8_t* plain, size_t plainLen);

    // Text input lives on the same unencrypted service as the internal BLE path.
    bool requestTextInput(const char* prompt, uint32_t timeoutMs = 120000UL);
    bool consumeTextInput(char* out, size_t outLen);
    void cancelTextInput(const char* reason = nullptr);
    bool isTextInputPending() const { return _textInputPending; }
    bool isTextInputReady() const { return _textInputReady; }
    const char* phonePromptText() const { return _promptBuf; }

    const char* linkStateName() const;

    bool sendLine(const char* line);
    bool readLine(char* out, size_t outLen);
    void requestBleStart();
    void requestBleStatus();
    void disconnectPhone(const char* reason = nullptr);

    // Raw SX1262 modem bridge: nRF owns silicon, ESP owns protocol logic.
    enum SubGhzModemMode : uint8_t {
        SUBGHZ_MODEM_OFF     = 0,
        SUBGHZ_MODEM_STANDBY = 1,
        SUBGHZ_MODEM_RX      = 2,
    };
    // SX1262 has one active PHY profile; native and mesh consumers gate on this.
    enum SubGhzAppOwner : uint8_t {
        SUBGHZ_OWNER_NONE   = 0,
        SUBGHZ_OWNER_NATIVE = 1,
        SUBGHZ_OWNER_MESH   = 2,
    };
    static constexpr size_t SUBGHZ_FRAME_MAX = 256;

    struct SubGhzRxFrame {
        uint16_t len = 0;
        int16_t  rssi = 0;
        int16_t  snr = 0;
        uint8_t  data[SUBGHZ_FRAME_MAX] = {};
    };

    bool subghzAvailable() const { return _present && _sx1262Present; }
    bool subghzConfigure(uint32_t freqHz, uint32_t bwHz, uint8_t sf, uint8_t cr,
                         uint16_t preamble, uint8_t syncWord, int8_t powerDbm);
    bool subghzSetMode(SubGhzModemMode mode);
    bool subghzSendRaw(const uint8_t* data, size_t len);
    bool subghzConsumeRx(SubGhzRxFrame& out);
    void subghzRequestStatus();
    SubGhzModemMode subghzMode() const { return _subghzMode; }
    SubGhzAppOwner subghzAppOwner() const { return _subghzAppOwner; }
    void subghzSetAppOwner(SubGhzAppOwner owner) { _subghzAppOwner = owner; }
    uint32_t subghzTxOkCount() const { return _subghzTxOk; }
    uint32_t subghzTxFailCount() const { return _subghzTxFail; }
    int subghzLastRssi() const { return _subghzLastRssi; }
    int subghzLastSnr() const { return _subghzLastSnr; }

private:
    static constexpr size_t UART_LINE_MAX = 640;
    // Mirrors wio_nrf_firmware UART_PAYLOAD_MAX.
    static constexpr size_t WIO_RELAY_PAYLOAD_MAX = 256;
    static constexpr size_t BLE_WRITE_VALUE_MAX = WIO_RELAY_PAYLOAD_MAX;
    static constexpr size_t EVENT_BATCH_SECURE_FRAME_MAX =
        PHONE_EVENT_BATCH_HEADER_V2_SIZE +
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
    static constexpr uint8_t AUTH_WRITE_MAX_RETRIES = 2;
    // Bounded challenge reissues recover lost response notifications.
    static constexpr uint32_t AUTH_REISSUE_INTERVAL_MS = 2000UL;
    static constexpr uint8_t AUTH_MAX_REISSUES = 3;

    HardwareSerial* _selectSerial();
    void _handleLine(const char* line);
    void _handleCapsLine(const char* caps);
    void _handleStatusLine(const char* status);
    void _handleProbeLine(const char* status);
    void _handleWriteLine(const char* status);
    void _handleRxLine(const char* status);
    void _handleRxBytes(const char* chr, const uint8_t* data, size_t len);
    void _handleSubghzRxBytes(const uint8_t* data, size_t len, int rssi, int snr);
    void _handleSubghzTxAck(const char* status);
    void _handleSubghzStatusLine(const char* status);
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
    void _clearEnrichmentExchangeState(bool preserveFailure);
    // Drop one corrupted batch; escalate only after repeated drops.
    void _dropEnrichmentBatch(const char* reason);
    bool _secureEnvelopeHeaderLooksValid(uint8_t channel,
                                         const uint8_t* data,
                                         size_t len,
                                         uint32_t& counter) const;
    bool _extractEnrichmentRecords(const uint8_t* plain,
                                   size_t plainLen,
                                   const uint8_t*& recordBytes,
                                   size_t& recordLen);
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
    // Optional UART flow control advertised by newer nRF firmware.
    bool _flowCtrl = false;
    bool _phoneConnected = false;
    bool _lineOverflow = false;
    bool _rxBinActive = false;
    bool _enrichmentFailed = false;
    bool _enrichmentBatchDropped = false;
    uint8_t _enrichmentConsecutiveDrops = 0;
    bool _enrichmentReady = false;
    bool _dropAfterReady = false;
    uint32_t _lastSeenMs = 0;
    uint32_t _lastHelloMs = 0;
    uint32_t _lastStatusRequestMs = 0;
    uint32_t _lastRecoveryStatusMs = 0;
    uint32_t _lastOverflowLogMs = 0;
    // Cumulative drained frames and last acked total.
    uint32_t _rxFrameTotal = 0;
    uint32_t _rxCreditAckedTotal = 0;
    uint32_t _seq = 0;
    uint32_t _activeSeq = 0;
    uint32_t _pendingBatchWriteSeq = 0;
    uint32_t _stateStartedMs = 0;
    uint32_t _readySinceMs = 0;
    uint32_t _rxBinStartedMs = 0;
    uint32_t _enrichmentSendMs = 0;
    uint32_t _enrichmentWaitStartMs = 0;
    uint32_t _enrichmentXferMs = 0;
    uint32_t _enrichmentSessionId = 0;
    uint32_t _enrichmentBatchSeq = 0;
    uint32_t _activeEnrichmentSessionId = 0;
    uint32_t _activeEnrichmentBatchId = 0;
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
    // Optional raw-binary UART CRC; absent on older nRF firmware.
    uint32_t _rxBinCrc = 0;
    bool _rxBinCrcPresent = false;
    // Shared binary RX path also carries SUBGHZ_RX.
    bool _rxBinIsSubghz = false;
    int16_t _rxBinRssi = 0;
    int16_t _rxBinSnr = 0;
    LinkState _linkState = LINK_IDLE;
    BleSecureSession _secureSession;
    uint8_t _authChallengeBuf[PHONE_AUTH_FRAME_SIZE] = {};
    size_t _authChallengeLen = 0;
    uint8_t _authWriteRetries = 0;
    uint32_t _authLastTxMs = 0;
    uint8_t _authReissues = 0;
    uint8_t _uartRxBuf[256] = {};
    uint8_t _eventBatchTxBuf[PHONE_EVENT_BATCH_HEADER_V2_SIZE +
                             PHONE_COMPANION_ENRICH_BATCH_MAX * EVENT_BATCH_RECORD_SIZE] = {};
    uint8_t _eventBatchSecureTxBuf[PHONE_EVENT_BATCH_HEADER_V2_SIZE +
                                   PHONE_COMPANION_ENRICH_BATCH_MAX * EVENT_BATCH_RECORD_SIZE +
                                   PHONE_SECURE_ENVELOPE_OVERHEAD] = {};
    uint8_t _enrichmentRxBuf[PHONE_COMPANION_ENRICH_BATCH_MAX * ENRICHMENT_RECORD_SIZE] = {};
    PendingEnrichment _enrichmentBatch[PHONE_COMPANION_ENRICH_BATCH_MAX] = {};

    // GPS state mirrors BLEManager.
    bool      _gpsAvailable = false;
    bool      _timeTrusted = false;
    uint32_t  _lastGpsFixMs = 0;
    uint32_t  _gpsEpochAtFix = 0;
    float     _gpsLat = 0.0f;
    float     _gpsLon = 0.0f;
    float     _gpsAlt = 0.0f;
    float     _gpsAccuracy = 0.0f;

    // Text input state mirrors BLEManager.
    bool      _textInputPending = false;
    bool      _textInputReady = false;
    uint32_t  _textInputDeadlineMs = 0;
    uint32_t  _textInputToken = 0;
    char      _promptBuf[24] = {};
    char      _resultBuf[64] = {};

    uint8_t   _storageSecureTxBuf[PHONE_STORAGE_FRAME_SIZE + PHONE_SECURE_ENVELOPE_OVERHEAD] = {};

    // Command/control buffers mirror BLEManager.
    uint8_t   _commandRespPlainBuf[PHONE_COMMAND_RESP_FRAME_MAX] = {};
    uint8_t   _commandRespSecureBuf[PHONE_COMMAND_RESP_FRAME_MAX +
                                    PHONE_SECURE_ENVELOPE_OVERHEAD] = {};
    uint8_t   _logStreamSecureBuf[LOG_STREAM_CHUNK_FRAME_MAX +
                                  PHONE_SECURE_ENVELOPE_OVERHEAD] = {};
    uint8_t   _dashboardStreamSecureBuf[DASHBOARD_STREAM_CHUNK_FRAME_MAX +
                                        PHONE_SECURE_ENVELOPE_OVERHEAD] = {};
    uint8_t   _notificationSecureBuf[PHONE_NOTIFICATION_FRAME_MAX +
                                     PHONE_SECURE_ENVELOPE_OVERHEAD] = {};

    static constexpr size_t SUBGHZ_RX_RING = 4;
    SubGhzModemMode _subghzMode = SUBGHZ_MODEM_OFF;
    SubGhzAppOwner  _subghzAppOwner = SUBGHZ_OWNER_NONE;
    SubGhzRxFrame   _subghzRx[SUBGHZ_RX_RING] = {};
    size_t          _subghzRxHead = 0;   // next slot to write
    size_t          _subghzRxTail = 0;   // next slot to read
    size_t          _subghzRxCount = 0;  // queued frames
    uint32_t        _subghzTxOk = 0;
    uint32_t        _subghzTxFail = 0;
    int             _subghzLastRssi = 0;
    int             _subghzLastSnr = 0;
};

extern WioNrfAccessory WIO_NRF;
