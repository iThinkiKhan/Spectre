#include "WioNrfAccessory.h"

#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#include "../core/DebugLog.h"
#include "../core/PsramObject.h"
#include "../core/Session.h"
#include "CommandDispatcher.h"
#include "TimeService.h"

namespace {
constexpr const char* TAG = "WIO";

// Mirror the freshness/holdover windows used by the internal BLE GPS path.
constexpr uint32_t WIO_GPS_STALE_MS = 45000UL;
constexpr uint32_t WIO_GPS_TIME_HOLDOVER_MS = 1800000UL;

// Same envelope used by BLEManager::_validateGpsFix.
constexpr float WIO_GPS_MAX_ABS_LAT = 90.0f;
constexpr float WIO_GPS_MAX_ABS_LON = 180.0f;
constexpr float WIO_GPS_MIN_ALT_M = -500.0f;
constexpr float WIO_GPS_MAX_ALT_M = 9000.0f;
constexpr float WIO_GPS_MAX_ACCURACY_M = 250.0f;

constexpr uint32_t WIO_TEXT_INPUT_TIMEOUT_MIN_MS = 10000UL;
constexpr uint32_t WIO_TEXT_INPUT_TIMEOUT_MAX_MS = 300000UL;

bool startsWith(const char* text, const char* prefix) {
    if (!text || !prefix) {
        return false;
    }
    while (*prefix) {
        if (*text++ != *prefix++) {
            return false;
        }
    }
    return true;
}

// CRC-32 (zlib/IEEE poly 0xEDB88320, init 0xFFFFFFFF, final XOR). Must stay
// byte-for-byte identical to the nRF side (wio_nrf_firmware uartFrameCrc32) so
// the BLE_RX_BIN integrity check agrees across the UART link.
uint32_t uartFrameCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }
    return ~crc;
}

bool isPrintableTextByte(uint8_t b) {
    return b >= 0x20 && b < 0x7F;
}
}

WioNrfAccessory& getWioNrfAccessory() {
    static WioNrfAccessory* instance = []() {
        void* storage = allocateManagerStorage(sizeof(WioNrfAccessory));
        configASSERT(storage);
        return new (storage) WioNrfAccessory();
    }();
    return *instance;
}

bool WioNrfAccessory::begin(uint32_t detectTimeoutMs) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (_begun) {
        return _present;
    }

    _serial = _selectSerial();
    if (!_serial) {
        DLOG_WARN(TAG, "UART%u unavailable", static_cast<unsigned>(WIO_NRF_UART_NUM));
        return false;
    }

    _serial->setRxBufferSize(4096);
    _serial->begin(WIO_NRF_BAUD, SERIAL_8N1, WIO_NRF_UART_RX, WIO_NRF_UART_TX);
    _serial->setTimeout(0);
    while (_serial->available()) {
        _serial->read();
    }

    _begun = true;
    _present = false;
    _bleProxy = false;
    _uartCap = false;
    _bleWriteBin = false;
    _sx1262Present = false;
    _phoneConnected = false;
    _phoneRssi = 0;
    strlcpy(_phone, "unknown", sizeof(_phone));
    _lineLen = 0;
    _lineOverflow = false;
    _rxBinActive = false;
    _rxBinChr[0] = '\0';
    _rxBinExpected = 0;
    _rxBinLen = 0;
    _enrichmentFailed = false;
    _enrichmentReady = false;
    _dropAfterReady = false;
    _lastSeenMs = 0;
    _lastHelloMs = 0;
    _lastStatusRequestMs = 0;
    _lastRecoveryStatusMs = 0;
    _activeSeq = 0;
    _pendingBatchWriteSeq = 0;
    _stateStartedMs = 0;
    _readySinceMs = 0;
    _rxBinStartedMs = 0;
    _enrichmentExpectedCount = 0;
    _enrichmentRxLen = 0;
    _enrichmentAvailableCount = 0;
    _enrichmentSessionId = 0;
    _enrichmentBatchSeq = 0;
    _activeEnrichmentSessionId = 0;
    _activeEnrichmentBatchId = 0;
    _linkState = LINK_IDLE;
    _secureSession.reset();
    _clearEnrichmentExchangeState(false);
    _setGpsUnavailable(true);
    _textInputPending = false;
    _textInputReady = false;
    _textInputDeadlineMs = 0;
    _textInputToken = 0;
    memset(_promptBuf, 0, sizeof(_promptBuf));
    memset(_resultBuf, 0, sizeof(_resultBuf));

    DLOG_INFO(TAG, "UART%u begin tx=%d rx=%d baud=%lu",
              static_cast<unsigned>(WIO_NRF_UART_NUM),
              WIO_NRF_UART_TX,
              WIO_NRF_UART_RX,
              static_cast<unsigned long>(WIO_NRF_BAUD));

    if (sendLine("SPECTRE/1 HELLO")) {
        _lastHelloMs = millis();
    }

    const uint32_t start = millis();
    while (millis() - start < detectTimeoutMs) {
        tick();
        if (_present && (_bleProxy || _uartCap || _sx1262Present)) {
            break;
        }
        delay(10);
    }

    if (_present) {
        DLOG_INFO(TAG, "Accessory detected bleProxy=%d sx1262=%d uart=%d",
                  _bleProxy ? 1 : 0,
                  _sx1262Present ? 1 : 0,
                  _uartCap ? 1 : 0);
    } else {
        DLOG_INFO(TAG, "Accessory not detected within %lums",
                  static_cast<unsigned long>(detectTimeoutMs));
    }

    return _present;
#else
    (void)detectTimeoutMs;
    return false;
#endif
}

void WioNrfAccessory::tick() {
#if WIO_NRF_ACCESSORY_ENABLED
    if (!_begun || !_serial) {
        return;
    }

    char line[UART_LINE_MAX] = {};
    uint8_t lines = 0;
    while (lines < 4 && readLine(line, sizeof(line))) {
        _handleLine(line);
        ++lines;
    }
    if (lines > 0) {
        _rxFrameTotal += lines;
    }

    // Release credits: tell the nRF our cumulative drained-frame count so it can
    // send more without overrunning our RX buffer. One small line per tick that
    // actually consumed frames; the count is cumulative so a lost ack self-heals
    // on the next one. Only when the peer advertised the capability.
    if (_flowCtrl && _rxFrameTotal != _rxCreditAckedTotal) {
        char credit[48] = {};
        snprintf(credit, sizeof(credit), "SPECTRE/1 RXCREDIT total=%lu",
                 static_cast<unsigned long>(_rxFrameTotal));
        if (sendLine(credit)) {
            _rxCreditAckedTotal = _rxFrameTotal;
        }
    }

    const uint32_t now = millis();
    if (!_present && now - _lastHelloMs >= HELLO_RETRY_INTERVAL_MS) {
        if (sendLine("SPECTRE/1 HELLO")) {
            _lastHelloMs = now;
        }
        return;
    }

    if (_linkState == LINK_PROBING &&
        now - _stateStartedMs > PROBE_TIMEOUT_MS) {
        _fail("probe_timeout");
    } else if (_linkState == LINK_AUTHENTICATING) {
        if (now - _stateStartedMs > AUTH_TIMEOUT_MS) {
            _fail("auth_timeout");
        } else if (_authChallengeLen > 0 &&
                   _authReissues < AUTH_MAX_REISSUES &&
                   now - _authLastTxMs >= AUTH_REISSUE_INTERVAL_MS) {
            // No auth response yet — the phone answers in <1s, so the response
            // notify was lost on the WIO->S3 UART hop. Re-issue the challenge;
            // the phone rebuilds and re-notifies a fresh response.
            ++_authReissues;
            DLOG_WARN(TAG,
                      "Auth response not seen in %lums; reissuing challenge attempt=%u",
                      static_cast<unsigned long>(now - _authLastTxMs),
                      static_cast<unsigned>(_authReissues));
            _authLastTxMs = now;
            _sendBleWrite("auth", _authChallengeBuf, _authChallengeLen);
        }
    } else if ((_linkState == LINK_SENDING_BATCH ||
                _linkState == LINK_WAITING_ENRICHMENT) &&
               now - _stateStartedMs > ENRICHMENT_TIMEOUT_MS) {
        // A lost/corrupt response stalls one batch; drop it and re-request
        // rather than aborting the whole bulk drain (escalates after N drops).
        _dropEnrichmentBatch("enrichment_timeout");
    } else if (_linkState == LINK_READY &&
               _dropAfterReady &&
               _readySinceMs != 0 &&
               now - _readySinceMs > READY_IDLE_DROP_MS) {
        disconnectPhone("ready_idle");
    }

    if (_present && _bleProxy && _linkState == LINK_FAILED &&
        now - _lastRecoveryStatusMs >= RECOVERY_STATUS_INTERVAL_MS) {
        requestBleStatus();
        _lastRecoveryStatusMs = now;
    }

    if (_linkState == LINK_FAILED &&
        now - _stateStartedMs >= FAILED_IDLE_RECOVERY_MS) {
        _recoverFailedLink("failed_idle_recovery");
    }

    _checkTextInputTimeout(now);

    if (_gpsAvailable && (now - _lastGpsFixMs) > WIO_GPS_STALE_MS) {
        _setGpsUnavailable(false);
    }
#endif
}

bool WioNrfAccessory::requestCompanionLink(const char* reason, bool allowCachedReconnect) {
    (void)allowCachedReconnect;
#if WIO_NRF_ACCESSORY_ENABLED
    if (!_present || !_bleProxy) {
        DLOG_WARN(TAG, "External BLE probe skipped reason=%s present=%d bleProxy=%d",
                  reason ? reason : "-",
                  _present ? 1 : 0,
                  _bleProxy ? 1 : 0);
        return false;
    }
    if (isPhoneCompanionReady()) {
        _dropAfterReady = false;
        DLOG_INFO(TAG, "External BLE probe reused ready link reason=%s phone=%s rssi=%d",
                  reason ? reason : "-",
                  phoneState(),
                  _phoneRssi);
        return true;
    }
    if (_linkState != LINK_IDLE && _linkState != LINK_FAILED) {
        if (_linkState == LINK_SENDING_BATCH ||
            _linkState == LINK_WAITING_ENRICHMENT) {
            return true;
        }
        DLOG_INFO(TAG, "External BLE probe already active reason=%s state=%s seq=%lu ageMs=%lu",
                  reason ? reason : "-",
                  linkStateName(),
                  static_cast<unsigned long>(_activeSeq),
                  static_cast<unsigned long>(millis() - _stateStartedMs));
        return true;
    }
    if (_linkState == LINK_FAILED) {
        _recoverFailedLink("new_probe");
    }
    return _sendProbe(reason);
#else
    return false;
#endif
}

bool WioNrfAccessory::requestEnrichmentBatch(const EventBatchRecord* records, size_t count) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (!records || count == 0 || count > PHONE_COMPANION_ENRICH_BATCH_MAX) {
        return false;
    }
    if (!isPhoneCompanionReady()) {
        return false;
    }
    if (_linkState == LINK_SENDING_BATCH || _linkState == LINK_WAITING_ENRICHMENT) {
        return false;
    }

    _clearEnrichmentExchangeState(false);

    ++_enrichmentBatchSeq;
    if (_enrichmentBatchSeq == 0) {
        ++_enrichmentBatchSeq;
    }
    if (_enrichmentSessionId == 0) {
        _enrichmentSessionId = 1;
    }
    _activeEnrichmentSessionId = _enrichmentSessionId;
    _activeEnrichmentBatchId = _enrichmentBatchSeq;

    PhoneEventBatchHeaderV2 header = {};
    header.magic = PHONE_EVENT_BATCH_V2_MAGIC;
    header.version = PHONE_BATCH_HEADER_VERSION;
    header.recordCount = static_cast<uint16_t>(count);
    header.sessionId = _activeEnrichmentSessionId;
    header.batchId = _activeEnrichmentBatchId;

    const size_t recordBytes = count * EVENT_BATCH_RECORD_SIZE;
    const size_t payloadLen = PHONE_EVENT_BATCH_HEADER_V2_SIZE + recordBytes;
    memcpy(_eventBatchTxBuf, &header, sizeof(header));
    memcpy(_eventBatchTxBuf + PHONE_EVENT_BATCH_HEADER_V2_SIZE, records, recordBytes);
    size_t secureLen = 0;
    if (!_secureSession.encrypt(PHONE_SECURE_CHANNEL_EVENT_BATCH,
                                _eventBatchTxBuf,
                                payloadLen,
                                _eventBatchSecureTxBuf,
                                sizeof(_eventBatchSecureTxBuf),
                                secureLen)) {
        _fail("batch_encrypt_failed");
        return false;
    }

    uint32_t writeSeq = 0;
    if (!_sendBleWrite("batch", _eventBatchSecureTxBuf, secureLen, &writeSeq)) {
        _fail("batch_uart_write_failed");
        return false;
    }

    _pendingBatchWriteSeq = writeSeq;
    _enrichmentExpectedCount = count;
    _enrichmentSendMs = millis();
    _setLinkState(LINK_SENDING_BATCH);
    _dropAfterReady = false;
    DLOG_DEBUG(TAG, "External enrichment batch queued count=%u bytes=%u session=%lu batch=%lu",
               static_cast<unsigned>(count),
               static_cast<unsigned>(payloadLen),
               static_cast<unsigned long>(_activeEnrichmentSessionId),
               static_cast<unsigned long>(_activeEnrichmentBatchId));
    return true;
#else
    (void)records;
    (void)count;
    return false;
#endif
}

bool WioNrfAccessory::consumeEnrichmentBatch(PendingEnrichment* out,
                                             size_t maxCount,
                                             size_t& outCount) {
    outCount = 0;
    if (!out || maxCount < _enrichmentAvailableCount || !_enrichmentReady) {
        return false;
    }

    for (size_t i = 0; i < _enrichmentAvailableCount; ++i) {
        out[i] = _enrichmentBatch[i];
    }
    outCount = _enrichmentAvailableCount;
    const uint32_t transferMs = _enrichmentXferMs;
    _clearEnrichmentExchangeState(false);
    _enrichmentXferMs = transferMs;
    _setLinkState(LINK_READY);
    _dropAfterReady = true;
    return true;
}

bool WioNrfAccessory::consumeEnrichmentFailure() {
    if (!_enrichmentFailed) {
        return false;
    }
    _enrichmentFailed = false;
    _setLinkState(LINK_FAILED);
    disconnectPhone("failure");
    return true;
}

bool WioNrfAccessory::consumeEnrichmentBatchDropped() {
    if (!_enrichmentBatchDropped) {
        return false;
    }
    _enrichmentBatchDropped = false;
    return true;
}

void WioNrfAccessory::_dropEnrichmentBatch(const char* reason) {
    // Bound retries: a few drops are transient UART corruption (recoverable),
    // but a sustained run means the link is genuinely bad — escalate to a hard
    // fail so the session ends instead of spinning.
    constexpr uint8_t kMaxConsecutiveDrops = 5;
    if (++_enrichmentConsecutiveDrops >= kMaxConsecutiveDrops) {
        DLOG_WARN(TAG,
                  "Enrichment batch drop limit reached (%u) reason=%s; failing link",
                  static_cast<unsigned>(_enrichmentConsecutiveDrops),
                  reason ? reason : "-");
        _fail(reason);
        return;
    }
    DLOG_WARN(TAG,
              "Dropping enrichment batch reason=%s session=%lu batch=%lu rxLen=%u drops=%u; will re-request",
              reason ? reason : "-",
              static_cast<unsigned long>(_activeEnrichmentSessionId),
              static_cast<unsigned long>(_activeEnrichmentBatchId),
              static_cast<unsigned>(_enrichmentRxLen),
              static_cast<unsigned>(_enrichmentConsecutiveDrops));
    // Reset only the in-flight batch reassembly; keep the secure session/link
    // up and return to READY so the host re-requests the next batch.
    _clearEnrichmentExchangeState(false);
    _enrichmentBatchDropped = true;
    if (_linkState == LINK_SENDING_BATCH || _linkState == LINK_WAITING_ENRICHMENT) {
        _setLinkState(LINK_READY);
    }
    _touchReadyActivity();
}

bool WioNrfAccessory::sendLine(const char* line) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (!_begun || !_serial || !line || !line[0]) {
        return false;
    }
    _serial->print(line);
    _serial->print("\r\n");
    return true;
#else
    (void)line;
    return false;
#endif
}

bool WioNrfAccessory::readLine(char* out, size_t outLen) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (!_serial || !out || outLen == 0) {
        return false;
    }

    if (_rxBinActive &&
        millis() - _rxBinStartedMs > RX_BIN_TIMEOUT_MS) {
        _abortRxBinary("rx_bin_timeout");
        return false;
    }

    while (_serial->available()) {
        if (_rxBinActive) {
            while (_serial->available() && _rxBinLen < _rxBinExpected) {
                const int raw = _serial->read();
                if (raw < 0) {
                    break;
                }
                _uartRxBuf[_rxBinLen++] = static_cast<uint8_t>(raw);
            }
            if (_rxBinLen < _rxBinExpected) {
                if (millis() - _rxBinStartedMs > RX_BIN_TIMEOUT_MS) {
                    _abortRxBinary("rx_bin_timeout");
                }
                return false;
            }

            _rxBinActive = false;
            _rxBinStartedMs = 0;

            if (_rxBinCrcPresent) {
                const uint32_t actual = uartFrameCrc32(_uartRxBuf, _rxBinLen);
                if (actual != _rxBinCrc) {
                    // The full frame arrived but the relay corrupted/dropped a
                    // byte. Drop just this frame and keep the link alive — do
                    // NOT _fail() the exchange (that is what _abortRxBinary
                    // would do). For enrichment, the device's _rxCounter is
                    // never advanced for an unseen frame, so the phone's resend
                    // (or the next frame) proceeds normally; a genuinely stuck
                    // wait still falls to ENRICHMENT_TIMEOUT_MS.
                    DLOG_WARN(TAG,
                              "UART frame CRC mismatch char=%s len=%u crc=%08lX expected=%08lX dropped=1",
                              _rxBinChr[0] ? _rxBinChr : "?",
                              static_cast<unsigned>(_rxBinLen),
                              static_cast<unsigned long>(actual),
                              static_cast<unsigned long>(_rxBinCrc));
                    _rxBinChr[0] = '\0';
                    _rxBinExpected = 0;
                    _rxBinLen = 0;
                    _rxBinCrc = 0;
                    _rxBinCrcPresent = false;
                    _rxBinIsSubghz = false;
                    return false;
                }
            }

            if (_rxBinIsSubghz) {
                snprintf(out, outLen,
                         "WIO/1 SUBGHZ_RX len=%u",
                         static_cast<unsigned>(_rxBinLen));
            } else {
                snprintf(out, outLen,
                         "WIO/1 BLE_RX_BIN char=%s len=%u",
                         _rxBinChr,
                         static_cast<unsigned>(_rxBinLen));
            }
            return true;
        }

        const int raw = _serial->read();
        if (raw < 0) {
            break;
        }
        const char c = static_cast<char>(raw);

        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            if (_lineOverflow) {
                _lineLen = 0;
                _lineOverflow = false;
                return false;
            }
            if (_lineLen == 0) {
                continue;
            }
            _line[_lineLen] = '\0';
            if (startsWith(_line, "WIO/1 BLE_RX_BIN ")) {
                char chr[16] = {};
                uint32_t expected = 0;
                const char* status = _line + strlen("WIO/1 BLE_RX_BIN ");
                if (!_readKeyValue(status, "char", chr, sizeof(chr)) ||
                    !_readUintValue(status, "len", expected) ||
                    expected > sizeof(_uartRxBuf)) {
                    _lineLen = 0;
                    continue;
                }
                uint32_t crc = 0;
                const bool crcPresent = _readUintValue(status, "crc", crc);
                strlcpy(_rxBinChr, chr, sizeof(_rxBinChr));
                _rxBinExpected = expected;
                _rxBinLen = 0;
                _rxBinCrc = crc;
                _rxBinCrcPresent = crcPresent;
                _rxBinActive = expected > 0;
                _rxBinStartedMs = _rxBinActive ? millis() : 0;
                _lineLen = 0;
                if (!_rxBinActive) {
                    snprintf(out, outLen,
                             "WIO/1 BLE_RX_BIN char=%s len=0",
                             _rxBinChr);
                    return true;
                }
                continue;
            }
            if (startsWith(_line, "WIO/1 SUBGHZ_RX ")) {
                uint32_t expected = 0;
                const char* status = _line + strlen("WIO/1 SUBGHZ_RX ");
                if (!_readUintValue(status, "len", expected) ||
                    expected > sizeof(_uartRxBuf)) {
                    _lineLen = 0;
                    continue;
                }
                uint32_t crc = 0;
                const bool crcPresent = _readUintValue(status, "crc", crc);
                // rssi/snr are signed (dBm / dB) so parse them as text+atoi;
                // _readUintValue would mangle the leading minus sign.
                char num[12] = {};
                int rssi = 0;
                int snr = 0;
                if (_readKeyValue(status, "rssi", num, sizeof(num))) {
                    rssi = atoi(num);
                }
                if (_readKeyValue(status, "snr", num, sizeof(num))) {
                    snr = atoi(num);
                }
                _rxBinChr[0] = '\0';
                _rxBinIsSubghz = true;
                _rxBinRssi = static_cast<int16_t>(rssi);
                _rxBinSnr = static_cast<int16_t>(snr);
                _rxBinExpected = expected;
                _rxBinLen = 0;
                _rxBinCrc = crc;
                _rxBinCrcPresent = crcPresent;
                _rxBinActive = expected > 0;
                _rxBinStartedMs = _rxBinActive ? millis() : 0;
                _lineLen = 0;
                if (!_rxBinActive) {
                    snprintf(out, outLen, "WIO/1 SUBGHZ_RX len=0");
                    return true;
                }
                continue;
            }
            strlcpy(out, _line, outLen);
            _lineLen = 0;
            return true;
        }

        if (static_cast<unsigned char>(c) < 0x20) {
            continue;
        }

        if (_lineLen + 1 >= sizeof(_line)) {
            _lineOverflow = true;
            _lineLen = 0;
            const uint32_t now = millis();
            if (now - _lastOverflowLogMs > 10000UL) {
                DLOG_WARN(TAG, "Dropping overlong UART line");
                _lastOverflowLogMs = now;
            }
            continue;
        }

        _line[_lineLen++] = c;
    }
#else
    (void)out;
    (void)outLen;
#endif
    return false;
}

void WioNrfAccessory::requestBleStatus() {
    if (sendLine("SPECTRE/1 BLE_STATUS")) {
        _lastStatusRequestMs = millis();
    }
}

void WioNrfAccessory::requestBleStart() {
    if (sendLine("SPECTRE/1 BLE_START")) {
        _lastStatusRequestMs = millis();
        DLOG_INFO(TAG, "External BLE start requested");
    }
}

void WioNrfAccessory::disconnectPhone(const char* reason) {
    (void)reason;
    if (sendLine("SPECTRE/1 BLE_DROP")) {
        DLOG_INFO(TAG, "External BLE link dropped reason=%s", reason ? reason : "-");
    }
    _handlePhoneDisconnected(reason ? reason : "local_drop", false);
}

HardwareSerial* WioNrfAccessory::_selectSerial() {
#if WIO_NRF_UART_NUM == 2
    return &Serial2;
#else
    return &Serial1;
#endif
}

void WioNrfAccessory::_handleLine(const char* line) {
    if (!line || !line[0]) {
        return;
    }

    if (startsWith(line, "WIO/1 CAPS ")) {
        _markSeen();
        _handleCapsLine(line + strlen("WIO/1 CAPS "));
        return;
    }

    if (startsWith(line, "WIO/1 STATUS ")) {
        _markSeen();
        _handleStatusLine(line + strlen("WIO/1 STATUS "));
        return;
    }

    if (startsWith(line, "WIO/1 BLE_PROBE ")) {
        _markSeen();
        _handleProbeLine(line + strlen("WIO/1 BLE_PROBE "));
        return;
    }

    if (startsWith(line, "WIO/1 BLE_WRITE ")) {
        _markSeen();
        _handleWriteLine(line + strlen("WIO/1 BLE_WRITE "));
        return;
    }

    if (startsWith(line, "WIO/1 BLE_RX ")) {
        _markSeen();
        _handleRxLine(line + strlen("WIO/1 BLE_RX "));
        return;
    }

    if (startsWith(line, "WIO/1 BLE_RX_BIN ")) {
        _markSeen();
        char chr[16] = {};
        uint32_t len = 0;
        const char* status = line + strlen("WIO/1 BLE_RX_BIN ");
        if (_readKeyValue(status, "char", chr, sizeof(chr)) &&
            _readUintValue(status, "len", len) &&
            len == _rxBinLen) {
            _handleRxBytes(chr, _uartRxBuf, _rxBinLen);
        }
        _rxBinChr[0] = '\0';
        _rxBinExpected = 0;
        _rxBinLen = 0;
        return;
    }

    if (startsWith(line, "WIO/1 SUBGHZ_RX ")) {
        _markSeen();
        uint32_t len = 0;
        const char* status = line + strlen("WIO/1 SUBGHZ_RX ");
        if (_readUintValue(status, "len", len) && len == _rxBinLen) {
            _handleSubghzRxBytes(_uartRxBuf, _rxBinLen, _rxBinRssi, _rxBinSnr);
        }
        _rxBinIsSubghz = false;
        _rxBinExpected = 0;
        _rxBinLen = 0;
        return;
    }

    if (startsWith(line, "WIO/1 SUBGHZ_TX ")) {
        _markSeen();
        _handleSubghzTxAck(line + strlen("WIO/1 SUBGHZ_TX "));
        return;
    }

    if (startsWith(line, "WIO/1 SUBGHZ_STATUS ")) {
        _markSeen();
        _handleSubghzStatusLine(line + strlen("WIO/1 SUBGHZ_STATUS "));
        return;
    }

    if (startsWith(line, "WIO/1 BLE_DROP ")) {
        _markSeen();
        const bool activeAttempt =
            _linkState == LINK_PROBING ||
            _linkState == LINK_AUTHENTICATING ||
            _linkState == LINK_READY ||
            _linkState == LINK_SENDING_BATCH ||
            _linkState == LINK_WAITING_ENRICHMENT;
        _handlePhoneDisconnected("phone_drop", activeAttempt);
        return;
    }

    if (startsWith(line, "WIO/1 PONG") ||
        startsWith(line, "WIO/1 HELLO") ||
        startsWith(line, "WIO/1 OK")) {
        _markSeen();
        DLOG_INFO(TAG, "Accessory response: %s", line);
        return;
    }

    DLOG_DEBUG(TAG, "Ignoring line: %s", line);
}

void WioNrfAccessory::_handleCapsLine(const char* caps) {
    _bleProxy = _tokenPresent(caps, "BLE_PROXY");
    _uartCap = _tokenPresent(caps, "UART");
    _bleWriteBin = _tokenPresent(caps, "BLE_WRITE_BIN");
    _sx1262Present = _tokenPresent(caps, "SX1262_PRESENT");
    _flowCtrl = _tokenPresent(caps, "UART_FLOWCTRL");

    // CAPS also arrives after a fresh nRF boot (its counters just reset), so
    // force the next tick to re-send our cumulative total. The nRF baselines off
    // that first RXCREDIT, so it does not matter that our total kept climbing.
    if (_flowCtrl) {
        _rxCreditAckedTotal = _rxFrameTotal - 1;
    }

    DLOG_INFO(TAG, "CAPS %s", caps ? caps : "");
    if (_bleProxy) {
        DLOG_INFO(TAG, "BLE proxy capability present");
    }
    if (_flowCtrl) {
        DLOG_INFO(TAG, "UART flow control capability present");
    }
}

void WioNrfAccessory::_handleStatusLine(const char* status) {
    const bool oldConnected = _phoneConnected;
    int oldRssi = _phoneRssi;
    char oldPhone[sizeof(_phone)] = {};
    strlcpy(oldPhone, _phone, sizeof(oldPhone));

    char value[16] = {};
    if (_readKeyValue(status, "phone", value, sizeof(value))) {
        strlcpy(_phone, value, sizeof(_phone));
    }
    bool sawConnected = false;
    if (_readKeyValue(status, "connected", value, sizeof(value))) {
        sawConnected = true;
        _phoneConnected = atoi(value) != 0;
    }
    if (_readKeyValue(status, "rssi", value, sizeof(value))) {
        _phoneRssi = atoi(value);
    }

    const bool forceProbeLog =
        _linkState == LINK_PROBING ||
        _linkState == LINK_AUTHENTICATING ||
        _linkState == LINK_FAILED;

    if (forceProbeLog ||
        oldConnected != _phoneConnected ||
        oldRssi != _phoneRssi ||
        strcmp(oldPhone, _phone) != 0) {
        DLOG_INFO(TAG, "BLE status phone=%s connected=%d rssi=%d state=%s seq=%lu ageMs=%lu raw=\"%s\"",
                  phoneState(),
                  _phoneConnected ? 1 : 0,
                  _phoneRssi,
                  linkStateName(),
                  static_cast<unsigned long>(_activeSeq),
                  static_cast<unsigned long>(millis() - _stateStartedMs),
                  status ? status : "");
    }

    if (sawConnected && !_phoneConnected) {
        const bool hadLiveLink =
            oldConnected ||
            _secureSession.isReady() ||
            _linkState == LINK_AUTHENTICATING ||
            _linkState == LINK_READY ||
            _linkState == LINK_SENDING_BATCH ||
            _linkState == LINK_WAITING_ENRICHMENT;
        if (hadLiveLink) {
            const bool activeAttempt =
                _linkState == LINK_AUTHENTICATING ||
                _linkState == LINK_READY ||
                _linkState == LINK_SENDING_BATCH ||
                _linkState == LINK_WAITING_ENRICHMENT;
            _handlePhoneDisconnected("status_disconnected", activeAttempt);
        }
    }
}

void WioNrfAccessory::_handleProbeLine(const char* status) {
    uint32_t seq = 0;
    _readUintValue(status, "id", seq);
    if (_activeSeq != 0 && seq != 0 && seq != _activeSeq) {
        DLOG_WARN(TAG, "External BLE probe stale response activeSeq=%lu responseSeq=%lu raw=\"%s\"",
                  static_cast<unsigned long>(_activeSeq),
                  static_cast<unsigned long>(seq),
                  status ? status : "");
        return;
    }

    char value[16] = {};
    const bool found =
        _readKeyValue(status, "result", value, sizeof(value)) &&
        strcmp(value, "found") == 0;
    const bool connected =
        _readKeyValue(status, "connected", value, sizeof(value)) &&
        atoi(value) != 0;
    if (_readKeyValue(status, "rssi", value, sizeof(value))) {
        _phoneRssi = atoi(value);
    }
    uint32_t seen = 0;
    uint32_t uuidSeen = 0;
    uint32_t connectAttempts = 0;
    uint32_t connectFailures = 0;
    uint32_t textLink = 0;
    int bestRssi = -127;
    char disconnectReason[16] = "0x00";
    char err[32] = "none";
    char source[16] = "unknown";
    _readUintValue(status, "seen", seen);
    _readUintValue(status, "uuid", uuidSeen);
    _readUintValue(status, "conn", connectAttempts);
    _readUintValue(status, "fail", connectFailures);
    _readUintValue(status, "text", textLink);
    if (_readKeyValue(status, "best", value, sizeof(value))) {
        bestRssi = atoi(value);
    }
    _readKeyValue(status, "disc", disconnectReason, sizeof(disconnectReason));
    _readKeyValue(status, "err", err, sizeof(err));
    _readKeyValue(status, "source", source, sizeof(source));

    DLOG_INFO(TAG,
              "External BLE probe response seq=%lu found=%d connected=%d source=%s text=%lu err=%s rssi=%d best=%d seen=%lu uuid=%lu conn=%lu fail=%lu ageMs=%lu",
              static_cast<unsigned long>(seq),
              found ? 1 : 0,
              connected ? 1 : 0,
              source,
              static_cast<unsigned long>(textLink),
              err,
              _phoneRssi,
              bestRssi,
              static_cast<unsigned long>(seen),
              static_cast<unsigned long>(uuidSeen),
              static_cast<unsigned long>(connectAttempts),
              static_cast<unsigned long>(connectFailures),
              static_cast<unsigned long>(millis() - _stateStartedMs));

    if (!found || !connected) {
        const char* missClass = "phone_not_reachable";
        if (err[0] != '\0' && strcmp(err, "none") != 0) {
            missClass = err;
        } else if (seen == 0) {
            missClass = "no_ble_advertisements";
        } else if (uuidSeen == 0) {
            missClass = "phone_service_not_advertising";
        } else if (connectAttempts == 0) {
            missClass = "phone_service_seen_no_connect";
        } else if (connectFailures > 0) {
            missClass = "connect_start_failed";
        }
        DLOG_WARN(TAG,
                  "External BLE probe miss class=%s seq=%lu source=%s text=%lu seen=%lu uuid=%lu best=%d conn=%lu fail=%lu disc=%s",
                  missClass,
                  static_cast<unsigned long>(seq),
                  source,
                  static_cast<unsigned long>(textLink),
                  static_cast<unsigned long>(seen),
                  static_cast<unsigned long>(uuidSeen),
                  bestRssi,
                  static_cast<unsigned long>(connectAttempts),
                  static_cast<unsigned long>(connectFailures),
                  disconnectReason);
        _phoneConnected = false;
        _fail("phone_not_reachable");
        return;
    }

    _phoneConnected = true;
    strlcpy(_phone, "reachable", sizeof(_phone));
    DLOG_INFO(TAG, "external_probe_summary seen=1 rssi=%d transport=wio",
              _phoneRssi);

    size_t challengeLen = 0;
    _clearEnrichmentExchangeState(false);
    _secureSession.reset();
    if (!_secureSession.buildChallenge(_authChallengeBuf,
                                       sizeof(_authChallengeBuf),
                                       challengeLen)) {
        _fail("auth_challenge_failed");
        return;
    }

    // Retain the challenge so a NAK'd forward write (UART CRC mismatch on the
    // S3->nRF hop) can be re-sent on the same connection instead of dropping
    // the phone and eating the full AUTH_TIMEOUT_MS window.
    _authChallengeLen = challengeLen;
    _authWriteRetries = 0;
    _authReissues = 0;

    if (!_sendBleWrite("auth", _authChallengeBuf, challengeLen)) {
        _fail("auth_uart_write_failed");
        return;
    }

    _authLastTxMs = millis();
    _setLinkState(LINK_AUTHENTICATING);
}

void WioNrfAccessory::_handleWriteLine(const char* status) {
    uint32_t ok = 0;
    _readUintValue(status, "ok", ok);
    uint32_t seq = 0;
    _readUintValue(status, "id", seq);

    // Only the enrichment batch exchange advances state on the ack.  Other
    // writes (storage snapshot, text prompt, auth challenge) are best-effort:
    // a failure tells us the WIO->phone hop didn't land, but it shouldn't
    // tear down a ready link — those flows have their own timeouts.
    if (_linkState == LINK_SENDING_BATCH) {
        if (_pendingBatchWriteSeq != 0 && seq != 0 && seq != _pendingBatchWriteSeq) {
            DLOG_WARN(TAG, "Ignoring stale BLE_WRITE ack state=%s active=%lu ack=%lu",
                      linkStateName(),
                      static_cast<unsigned long>(_pendingBatchWriteSeq),
                      static_cast<unsigned long>(seq));
            return;
        }
        if (ok == 0) {
            _fail("batch_ble_write_failed");
            return;
        }
        _pendingBatchWriteSeq = 0;
        _setLinkState(LINK_WAITING_ENRICHMENT);
        _enrichmentWaitStartMs = millis();
        return;
    }

    if (_linkState == LINK_AUTHENTICATING) {
        // Ack for the auth-challenge write. A matching id with ok=0 means the
        // nRF rejected the forward frame on CRC (a corrupted/dropped byte on the
        // S3->nRF UART hop) and did NOT write it to the phone — so the phone
        // never sees a challenge and would otherwise silently stall to
        // auth_timeout. Re-send a bounded number of times before failing.
        if (seq != 0 && _activeSeq != 0 && seq != _activeSeq) {
            return;
        }
        if (ok == 0) {
            if (_authChallengeLen > 0 &&
                _authWriteRetries < AUTH_WRITE_MAX_RETRIES) {
                ++_authWriteRetries;
                DLOG_WARN(TAG,
                          "Auth challenge NAK'd (uart crc); resending attempt=%u",
                          static_cast<unsigned>(_authWriteRetries));
                if (_sendBleWrite("auth", _authChallengeBuf, _authChallengeLen)) {
                    _authLastTxMs = millis();
                    return;
                }
            }
            _fail("auth_uart_write_failed");
        }
        return;
    }

    if (_linkState == LINK_WAITING_ENRICHMENT && seq != 0 && seq != _activeSeq) {
        DLOG_WARN(TAG, "Ignoring stale BLE_WRITE ack state=%s active=%lu ack=%lu",
                  linkStateName(),
                  static_cast<unsigned long>(_activeSeq),
                  static_cast<unsigned long>(seq));
        return;
    }

    if (ok == 0) {
        DLOG_WARN(TAG, "BLE_WRITE ack ok=0 state=%s", linkStateName());
    } else {
        _touchReadyActivity();
    }
}

void WioNrfAccessory::_handleRxLine(const char* status) {
    char chr[16] = {};
    char hex[UART_LINE_MAX] = {};
    if (!_readKeyValue(status, "char", chr, sizeof(chr)) ||
        !_readKeyValue(status, "hex", hex, sizeof(hex))) {
        return;
    }

    size_t len = 0;
    if (!_decodeHex(hex, _uartRxBuf, sizeof(_uartRxBuf), len)) {
        _fail("uart_hex_decode_failed");
        return;
    }

    _handleRxBytes(chr, _uartRxBuf, len);
}

void WioNrfAccessory::_handleRxBytes(const char* chr, const uint8_t* data, size_t len) {
    if (!chr || (!data && len != 0)) {
        return;
    }

    _touchReadyActivity();

    if (strcmp(chr, "auth") == 0) {
        _handleAuthBytes(data, len);
    } else if (strcmp(chr, "control") == 0) {
        _handleControlBytes(data, len);
    } else if (strcmp(chr, "enrich") == 0) {
        _handleEnrichmentBytes(data, len);
    } else if (strcmp(chr, "gps") == 0) {
        _handleGpsBytes(data, len);
    } else if (strcmp(chr, "text_input") == 0) {
        _handleTextInputBytes(data, len);
    } else if (strcmp(chr, "command_req") == 0) {
        _handleCommandRequestBytes(data, len);
    } else {
        DLOG_DEBUG(TAG, "RX char=%s len=%u (ignored)", chr,
                   static_cast<unsigned>(len));
    }
}

// ---------------------------------------------------------------------------
// SX1262 sub-GHz modem bridge
// ---------------------------------------------------------------------------

void WioNrfAccessory::_handleSubghzRxBytes(const uint8_t* data, size_t len,
                                           int rssi, int snr) {
    if (!data || len == 0 || len > SUBGHZ_FRAME_MAX) {
        return;
    }
    _markSeen();
    _subghzLastRssi = rssi;
    _subghzLastSnr = snr;

    if (_subghzRxCount >= SUBGHZ_RX_RING) {
        // Ring full: drop the oldest frame so live RX always wins over a
        // backlog the consumer hasn't drained yet.
        _subghzRxTail = (_subghzRxTail + 1) % SUBGHZ_RX_RING;
        --_subghzRxCount;
    }
    SubGhzRxFrame& slot = _subghzRx[_subghzRxHead];
    slot.len = static_cast<uint16_t>(len);
    slot.rssi = static_cast<int16_t>(rssi);
    slot.snr = static_cast<int16_t>(snr);
    memcpy(slot.data, data, len);
    _subghzRxHead = (_subghzRxHead + 1) % SUBGHZ_RX_RING;
    ++_subghzRxCount;
}

void WioNrfAccessory::_handleSubghzTxAck(const char* status) {
    uint32_t ok = 0;
    _readUintValue(status, "ok", ok);
    if (ok) {
        ++_subghzTxOk;
    } else {
        ++_subghzTxFail;
    }
}

void WioNrfAccessory::_handleSubghzStatusLine(const char* status) {
    uint32_t mode = 0;
    if (_readUintValue(status, "mode", mode)) {
        _subghzMode = static_cast<SubGhzModemMode>(mode);
    }
    char num[12] = {};
    if (_readKeyValue(status, "rssi", num, sizeof(num))) {
        _subghzLastRssi = atoi(num);
    }
    if (_readKeyValue(status, "snr", num, sizeof(num))) {
        _subghzLastSnr = atoi(num);
    }
}

bool WioNrfAccessory::subghzConfigure(uint32_t freqHz, uint32_t bwHz, uint8_t sf,
                                      uint8_t cr, uint16_t preamble,
                                      uint8_t syncWord, int8_t powerDbm) {
    if (!subghzAvailable()) {
        return false;
    }
    char line[160] = {};
    snprintf(line, sizeof(line),
             "SPECTRE/1 SUBGHZ_CONFIG freq=%lu bw=%lu sf=%u cr=%u preamble=%u "
             "sync=0x%02X power=%d",
             static_cast<unsigned long>(freqHz),
             static_cast<unsigned long>(bwHz),
             static_cast<unsigned>(sf),
             static_cast<unsigned>(cr),
             static_cast<unsigned>(preamble),
             static_cast<unsigned>(syncWord),
             static_cast<int>(powerDbm));
    return sendLine(line);
}

bool WioNrfAccessory::subghzSetMode(SubGhzModemMode mode) {
    if (!subghzAvailable()) {
        return false;
    }
    const char* name = (mode == SUBGHZ_MODEM_RX)      ? "rx" :
                       (mode == SUBGHZ_MODEM_STANDBY) ? "standby" : "off";
    char line[48] = {};
    snprintf(line, sizeof(line), "SPECTRE/1 SUBGHZ_MODE mode=%s", name);
    if (!sendLine(line)) {
        return false;
    }
    _subghzMode = mode;  // optimistic; the nRF confirms via SUBGHZ_STATUS
    return true;
}

bool WioNrfAccessory::subghzSendRaw(const uint8_t* data, size_t len) {
    if (!subghzAvailable() || !data || len == 0 || len > SUBGHZ_FRAME_MAX) {
        return false;
    }
#if WIO_NRF_ACCESSORY_ENABLED
    if (!_serial) {
        return false;
    }
    const uint32_t id = _nextSeq();
    char header[64] = {};
    snprintf(header, sizeof(header),
             "SPECTRE/1 SUBGHZ_TX id=%lu len=%u",
             static_cast<unsigned long>(id),
             static_cast<unsigned>(len));
    if (!sendLine(header)) {
        return false;
    }
    if (_serial->write(data, len) != len) {
        return false;
    }
    _serial->print("\r\n");
    return true;
#else
    (void)data;
    (void)len;
    return false;
#endif
}

bool WioNrfAccessory::subghzConsumeRx(SubGhzRxFrame& out) {
    if (_subghzRxCount == 0) {
        return false;
    }
    out = _subghzRx[_subghzRxTail];
    _subghzRxTail = (_subghzRxTail + 1) % SUBGHZ_RX_RING;
    --_subghzRxCount;
    return true;
}

void WioNrfAccessory::subghzRequestStatus() {
    if (subghzAvailable()) {
        sendLine("SPECTRE/1 SUBGHZ_STATUS");
    }
}

void WioNrfAccessory::_markSeen() {
    _present = true;
    _lastSeenMs = millis();
}

bool WioNrfAccessory::_tokenPresent(const char* text, const char* token) const {
    if (!text || !token || !token[0]) {
        return false;
    }

    const size_t tokenLen = strlen(token);
    const char* p = text;
    while (*p) {
        while (*p == ' ') {
            ++p;
        }
        const char* start = p;
        while (*p && *p != ' ') {
            ++p;
        }
        if (static_cast<size_t>(p - start) == tokenLen &&
            strncmp(start, token, tokenLen) == 0) {
            return true;
        }
    }
    return false;
}

bool WioNrfAccessory::_readKeyValue(const char* text,
                                    const char* key,
                                    char* out,
                                    size_t outLen) const {
    if (!text || !key || !out || outLen == 0) {
        return false;
    }

    const size_t keyLen = strlen(key);
    const char* p = text;
    while (*p) {
        while (*p == ' ') {
            ++p;
        }
        const char* start = p;
        while (*p && *p != ' ') {
            ++p;
        }
        const char* eq = static_cast<const char*>(memchr(start, '=', p - start));
        if (eq && static_cast<size_t>(eq - start) == keyLen &&
            strncmp(start, key, keyLen) == 0) {
            const size_t valueLen = static_cast<size_t>(p - eq - 1);
            const size_t copyLen = min(valueLen, outLen - 1);
            memcpy(out, eq + 1, copyLen);
            out[copyLen] = '\0';
            return true;
        }
    }

    return false;
}

bool WioNrfAccessory::_readUintValue(const char* text, const char* key, uint32_t& out) const {
    char value[16] = {};
    if (!_readKeyValue(text, key, value, sizeof(value))) {
        return false;
    }
    out = static_cast<uint32_t>(strtoul(value, nullptr, 10));
    return true;
}

void WioNrfAccessory::_setLinkState(LinkState state) {
    if (_linkState == state) {
        return;
    }
    _linkState = state;
    _stateStartedMs = millis();
    if (state == LINK_READY) {
        _readySinceMs = _stateStartedMs;
    }
}

void WioNrfAccessory::_clearEnrichmentExchangeState(bool preserveFailure) {
    _pendingBatchWriteSeq = 0;
    _enrichmentExpectedCount = 0;
    _enrichmentRxLen = 0;
    _enrichmentAvailableCount = 0;
    _enrichmentReady = false;
    _enrichmentSendMs = 0;
    _enrichmentWaitStartMs = 0;
    _enrichmentXferMs = 0;
    _activeEnrichmentSessionId = 0;
    _activeEnrichmentBatchId = 0;
    if (!preserveFailure) {
        _enrichmentFailed = false;
    }
    memset(_eventBatchTxBuf, 0, sizeof(_eventBatchTxBuf));
    memset(_eventBatchSecureTxBuf, 0, sizeof(_eventBatchSecureTxBuf));
    memset(_enrichmentRxBuf, 0, sizeof(_enrichmentRxBuf));
    memset(_enrichmentBatch, 0, sizeof(_enrichmentBatch));
}

bool WioNrfAccessory::_secureEnvelopeHeaderLooksValid(uint8_t channel,
                                                      const uint8_t* data,
                                                      size_t len,
                                                      uint32_t& counter) const {
    counter = 0;
    if (!data || len < PHONE_SECURE_ENVELOPE_OVERHEAD) {
        return false;
    }
    counter = static_cast<uint32_t>(data[2]) |
              (static_cast<uint32_t>(data[3]) << 8) |
              (static_cast<uint32_t>(data[4]) << 16) |
              (static_cast<uint32_t>(data[5]) << 24);
    return data[0] == COMPANION_PROTOCOL_VERSION &&
           data[1] == channel &&
           counter != 0;
}

bool WioNrfAccessory::_extractEnrichmentRecords(const uint8_t* plain,
                                                size_t plainLen,
                                                const uint8_t*& recordBytes,
                                                size_t& recordLen) {
    recordBytes = nullptr;
    recordLen = 0;
    if (!plain || plainLen < PHONE_ENRICHMENT_RESPONSE_HEADER_V2_SIZE) {
        DLOG_WARN(TAG, "Ignoring enrichment frame without v2 response header len=%u session=%lu batch=%lu",
                  static_cast<unsigned>(plainLen),
                  static_cast<unsigned long>(_activeEnrichmentSessionId),
                  static_cast<unsigned long>(_activeEnrichmentBatchId));
        return false;
    }

    PhoneEnrichmentResponseHeaderV2 header = {};
    memcpy(&header, plain, sizeof(header));
    if (header.magic != PHONE_ENRICHMENT_RESPONSE_V2_MAGIC ||
        header.version != PHONE_BATCH_HEADER_VERSION) {
        DLOG_WARN(TAG, "Ignoring enrichment frame with invalid response header magic=0x%08lx version=%u",
                  static_cast<unsigned long>(header.magic),
                  static_cast<unsigned>(header.version));
        return false;
    }
    if (header.sessionId != _activeEnrichmentSessionId ||
        header.batchId != _activeEnrichmentBatchId) {
        DLOG_WARN(TAG, "Ignoring stale enrichment response session=%lu/%lu batch=%lu/%lu offset=%u",
                  static_cast<unsigned long>(header.sessionId),
                  static_cast<unsigned long>(_activeEnrichmentSessionId),
                  static_cast<unsigned long>(header.batchId),
                  static_cast<unsigned long>(_activeEnrichmentBatchId),
                  static_cast<unsigned>(header.payloadOffset));
        return false;
    }
    if (header.recordCount != _enrichmentExpectedCount) {
        DLOG_WARN(TAG, "Enrichment response count mismatch got=%u expected=%u",
                  static_cast<unsigned>(header.recordCount),
                  static_cast<unsigned>(_enrichmentExpectedCount));
        _dropEnrichmentBatch("enrichment_count_mismatch");
        return false;
    }
    if (header.payloadLen > plainLen - PHONE_ENRICHMENT_RESPONSE_HEADER_V2_SIZE) {
        _dropEnrichmentBatch("enrichment_header_len_invalid");
        return false;
    }
    if (header.payloadOffset != _enrichmentRxLen) {
        DLOG_WARN(TAG, "Enrichment response offset mismatch got=%u expected=%u session=%lu batch=%lu",
                  static_cast<unsigned>(header.payloadOffset),
                  static_cast<unsigned>(_enrichmentRxLen),
                  static_cast<unsigned long>(header.sessionId),
                  static_cast<unsigned long>(header.batchId));
        _dropEnrichmentBatch("enrichment_offset_mismatch");
        return false;
    }

    recordBytes = plain + PHONE_ENRICHMENT_RESPONSE_HEADER_V2_SIZE;
    recordLen = header.payloadLen;
    return true;
}

void WioNrfAccessory::_touchReadyActivity() {
    if (_linkState == LINK_READY && _dropAfterReady) {
        _readySinceMs = millis();
    }
}

void WioNrfAccessory::_fail(const char* reason) {
    const uint32_t now = millis();
    DLOG_WARN(TAG, "External BLE failed reason=%s state=%s seq=%lu ageMs=%lu phone=%s rssi=%d",
              reason ? reason : "-",
              linkStateName(),
              static_cast<unsigned long>(_activeSeq),
              static_cast<unsigned long>(now - _stateStartedMs),
              phoneState(),
              _phoneRssi);
    _enrichmentFailed = true;
    _phoneConnected = false;
    _dropAfterReady = false;
    _secureSession.reset();
    _activeSeq = 0;
    _clearEnrichmentExchangeState(true);
    _enrichmentFailed = true;

    // Phone-level failures are not accessory disappearance.  The WIO nRF can
    // remain alive while it is scanning, reconnecting, or in its own LED error
    // indication, so keep the negotiated CAPS and let the scheduler retry the
    // WIO path instead of immediately enabling the ESP32 internal BLE radio.
    if (_present && _bleProxy) {
        requestBleStatus();
        _lastRecoveryStatusMs = millis();
    }
    _setLinkState(LINK_FAILED);
}

void WioNrfAccessory::_recoverFailedLink(const char* reason) {
    DLOG_INFO(TAG, "External BLE recovery to idle reason=%s state=%s phone=%s rssi=%d",
              reason ? reason : "-",
              linkStateName(),
              phoneState(),
              _phoneRssi);
    _secureSession.reset();
    _phoneConnected = false;
    _dropAfterReady = false;
    _activeSeq = 0;
    _clearEnrichmentExchangeState(false);
    if (_present && _bleProxy) {
        sendLine("SPECTRE/1 BLE_DROP");
        requestBleStatus();
    }
    _setLinkState(LINK_IDLE);
}

void WioNrfAccessory::_abortRxBinary(const char* reason) {
    DLOG_WARN(TAG, "Aborting UART binary frame reason=%s char=%s got=%u expected=%u",
              reason ? reason : "-",
              _rxBinChr[0] ? _rxBinChr : "?",
              static_cast<unsigned>(_rxBinLen),
              static_cast<unsigned>(_rxBinExpected));

    // GPS and control frames are incidental to an enrichment exchange (the
    // device clock is already synced; these are periodic refreshes). A dropped
    // one must NOT tear down an in-flight bulk enrich session — over a long
    // single-connection drain the periodic GPS/time frames are the most common
    // UART hiccup, and failing on them would abort the whole run. Only auth and
    // enrichment frames are critical to the active exchange.
    const bool criticalFrame =
        (strcmp(_rxBinChr, "auth") == 0) ||
        (strcmp(_rxBinChr, "enrich") == 0);

    _rxBinActive = false;
    _rxBinChr[0] = '\0';
    _rxBinExpected = 0;
    _rxBinLen = 0;
    _rxBinStartedMs = 0;
    _rxBinIsSubghz = false;
    _lineLen = 0;
    _lineOverflow = false;

    if (criticalFrame &&
        (_linkState == LINK_AUTHENTICATING ||
         _linkState == LINK_SENDING_BATCH ||
         _linkState == LINK_WAITING_ENRICHMENT)) {
        _fail(reason);
    } else if (_present && _bleProxy) {
        // Drop the frame and recover; the exchange continues.
        requestBleStatus();
    }
}

void WioNrfAccessory::_handlePhoneDisconnected(const char* reason, bool markFailure) {
    const LinkState previous = _linkState;
    const bool hadSession = _secureSession.isReady();

    if (previous != LINK_IDLE || hadSession || _phoneConnected) {
        DLOG_INFO(TAG,
                  "External BLE phone disconnected reason=%s previous=%s fail=%d phone=%s rssi=%d",
                  reason ? reason : "-",
                  linkStateName(),
                  markFailure ? 1 : 0,
                  phoneState(),
                  _phoneRssi);
    }

    _phoneConnected = false;
    _secureSession.reset();
    _setGpsUnavailable(true);
    _dropAfterReady = false;
    _activeSeq = 0;
    _clearEnrichmentExchangeState(markFailure);
    _clearTextInputState(reason ? reason : "phone_disconnect");

    if (markFailure) {
        _enrichmentFailed = true;
    }

    _setLinkState(LINK_IDLE);
}

uint32_t WioNrfAccessory::_nextSeq() {
    ++_seq;
    if (_seq == 0) {
        ++_seq;
    }
    return _seq;
}

bool WioNrfAccessory::_sendProbe(const char* reason) {
    _clearEnrichmentExchangeState(false);
    _activeSeq = _nextSeq();
    char line[80] = {};
    snprintf(line, sizeof(line),
             "SPECTRE/1 BLE_PROBE id=%lu timeout=%lu",
             static_cast<unsigned long>(_activeSeq),
             static_cast<unsigned long>(PROBE_TIMEOUT_MS));
    if (!sendLine(line)) {
        DLOG_WARN(TAG, "External BLE probe uart send failed reason=%s seq=%lu",
                  reason ? reason : "-",
                  static_cast<unsigned long>(_activeSeq));
        return false;
    }
    _setLinkState(LINK_PROBING);
    DLOG_INFO(TAG, "External BLE probe start reason=%s seq=%lu timeoutMs=%lu phone=%s rssi=%d",
              reason ? reason : "-",
              static_cast<unsigned long>(_activeSeq),
              static_cast<unsigned long>(PROBE_TIMEOUT_MS),
              phoneState(),
              _phoneRssi);
    _enrichmentFailed = false;
    _dropAfterReady = false;
    return true;
}

bool WioNrfAccessory::_sendBleWrite(const char* chr,
                                    const uint8_t* data,
                                    size_t len,
                                    uint32_t* seqOut) {
    return _bleWriteBin
               ? _sendBleWriteBinary(chr, data, len, seqOut)
               : _sendBleWriteHex(chr, data, len, seqOut);
}

bool WioNrfAccessory::_sendBleWriteBinary(const char* chr,
                                          const uint8_t* data,
                                          size_t len,
                                          uint32_t* seqOut) {
    if (!chr || (!_serial && WIO_NRF_ACCESSORY_ENABLED) || (!data && len != 0) ||
        len > BLE_WRITE_VALUE_MAX) {
        return false;
    }
#if WIO_NRF_ACCESSORY_ENABLED
    _activeSeq = _nextSeq();
    if (seqOut) {
        *seqOut = _activeSeq;
    }
    // CRC covers the raw payload so the nRF can reject a corrupted/dropped byte
    // on this forward hop before writing garbage to the phone (mirrors the
    // BLE_RX_BIN crc on the return path). Older nRF firmware ignores the extra
    // token; newer firmware validates and NAKs on mismatch.
    const uint32_t crc = (len > 0) ? uartFrameCrc32(data, len) : 0;
    char header[112] = {};
    snprintf(header, sizeof(header),
             "SPECTRE/1 BLE_WRITE_BIN id=%lu char=%s len=%u crc=%lu",
             static_cast<unsigned long>(_activeSeq),
             chr,
             static_cast<unsigned>(len),
             static_cast<unsigned long>(crc));
    if (!sendLine(header)) {
        return false;
    }
    if (len > 0 && _serial->write(data, len) != len) {
        return false;
    }
    _serial->print("\r\n");
    return true;
#else
    (void)chr;
    (void)data;
    (void)len;
    (void)seqOut;
    return false;
#endif
}

bool WioNrfAccessory::_sendBleWriteHex(const char* chr,
                                       const uint8_t* data,
                                       size_t len,
                                       uint32_t* seqOut) {
    if (!chr || (!data && len != 0)) {
        return false;
    }
    char hex[BLE_WRITE_VALUE_MAX * 2 + 1] = {};
    if (len > (sizeof(hex) - 1) / 2) {
        return false;
    }
    if (len > 0 && !_encodeHex(data, len, hex, sizeof(hex))) {
        return false;
    }

    _activeSeq = _nextSeq();
    if (seqOut) {
        *seqOut = _activeSeq;
    }
    char line[UART_LINE_MAX] = {};
    snprintf(line, sizeof(line),
             "SPECTRE/1 BLE_WRITE id=%lu char=%s len=%u hex=%s",
             static_cast<unsigned long>(_activeSeq),
             chr,
             static_cast<unsigned>(len),
             hex);
    return sendLine(line);
}

void WioNrfAccessory::_handleAuthBytes(const uint8_t* data, size_t len) {
    if (_linkState != LINK_AUTHENTICATING) {
        return;
    }
    if (!_secureSession.completeFromResponse(data, len)) {
        _fail("auth_response_failed");
        return;
    }
    ++_enrichmentSessionId;
    if (_enrichmentSessionId == 0) {
        ++_enrichmentSessionId;
    }
    _clearEnrichmentExchangeState(false);
    _setLinkState(LINK_READY);
    _dropAfterReady = true;
    DLOG_INFO(TAG, "External BLE companion authenticated session=%lu",
              static_cast<unsigned long>(_enrichmentSessionId));
}

void WioNrfAccessory::_handleControlBytes(const uint8_t* data, size_t len) {
    uint8_t plain[PHONE_CONTROL_FRAME_SIZE] = {};
    size_t plainLen = 0;
    if (!_secureSession.decrypt(PHONE_SECURE_CHANNEL_CONTROL,
                                data,
                                len,
                                plain,
                                sizeof(plain),
                                plainLen)) {
        return;
    }
    if (plainLen < PHONE_CONTROL_FRAME_SIZE) {
        return;
    }
    PhoneControlFrameV1 frame;
    memcpy(&frame, plain, sizeof(frame));
    if ((frame.flags & PHONE_CTRL_FLAG_CANCEL) != 0) {
        _fail("phone_cancel");
    }
}

void WioNrfAccessory::_handleCommandRequestBytes(const uint8_t* data, size_t len) {
    uint8_t plainReq[PHONE_COMMAND_REQ_FRAME_MAX] = {};
    size_t plainReqLen = 0;
    if (!_secureSession.decrypt(PHONE_SECURE_CHANNEL_COMMAND,
                                data, len,
                                plainReq, sizeof(plainReq),
                                plainReqLen)) {
        DLOG_WARN(TAG, "command decrypt failed: %s",
                  _secureSession.lastError() ? _secureSession.lastError() : "-");
        return;
    }

    size_t plainRespLen = 0;
    if (!CommandDispatcher::dispatch(plainReq, plainReqLen,
                                     _commandRespPlainBuf,
                                     sizeof(_commandRespPlainBuf),
                                     plainRespLen)) {
        DLOG_WARN(TAG, "command dispatcher refused response");
        return;
    }

    size_t secureLen = 0;
    if (!_secureSession.encrypt(PHONE_SECURE_CHANNEL_COMMAND,
                                _commandRespPlainBuf, plainRespLen,
                                _commandRespSecureBuf,
                                sizeof(_commandRespSecureBuf),
                                secureLen)) {
        DLOG_WARN(TAG, "command encrypt failed: %s",
                  _secureSession.lastError() ? _secureSession.lastError() : "-");
        return;
    }

    if (!_sendBleWrite("command_resp", _commandRespSecureBuf, secureLen)) {
        DLOG_WARN(TAG, "command response BLE_WRITE failed");
    }
}

void WioNrfAccessory::_handleEnrichmentBytes(const uint8_t* data, size_t len) {
    if (_linkState != LINK_WAITING_ENRICHMENT || _enrichmentExpectedCount == 0) {
        return;
    }

    uint32_t counter = 0;
    if (!_secureEnvelopeHeaderLooksValid(PHONE_SECURE_CHANNEL_ENRICHMENT,
                                         data,
                                         len,
                                         counter)) {
        DLOG_WARN(TAG,
                  "Ignoring invalid enrichment envelope len=%u hdr=%02X/%02X ctr=%lu rxLen=%u expected=%u",
                  static_cast<unsigned>(len),
                  len > 0 && data ? data[0] : 0,
                  len > 1 && data ? data[1] : 0,
                  static_cast<unsigned long>(counter),
                  static_cast<unsigned>(_enrichmentRxLen),
                  static_cast<unsigned>(_enrichmentExpectedCount));
        return;
    }

    uint8_t plain[256] = {};
    size_t plainLen = 0;
    if (!_secureSession.decrypt(PHONE_SECURE_CHANNEL_ENRICHMENT,
                                data,
                                len,
                                plain,
                                sizeof(plain),
                                plainLen)) {
        const char* err = _secureSession.lastError();
        if (err && strstr(err, "replay/stale counter")) {
            DLOG_DEBUG(TAG, "Ignoring stale enrichment frame: %s", err);
            return;
        }
        DLOG_WARN(TAG,
                  "enrichment decrypt failed err=%s len=%u hdr=%02X/%02X ctr=%lu rxLen=%u expected=%u dropped=1",
                  err ? err : "-",
                  static_cast<unsigned>(len),
                  len > 0 ? data[0] : 0,
                  len > 1 ? data[1] : 0,
                  static_cast<unsigned long>(counter),
                  static_cast<unsigned>(_enrichmentRxLen),
                  static_cast<unsigned>(_enrichmentExpectedCount));
        // Drop just this frame instead of tearing down the whole exchange. The
        // BLE_RX_BIN CRC check now catches the common cause (UART relay
        // corruption) upstream, so reaching here is rare; when it does, the
        // device's _rxCounter is not advanced (decrypt returned before line
        // _rxCounter[channel]=counter), so the phone may resend the same-counter
        // frame and it will be accepted. A genuinely stuck wait still ends at
        // ENRICHMENT_TIMEOUT_MS.
        return;
    }

    const uint8_t* recordBytes = nullptr;
    size_t recordLen = 0;
    if (!_extractEnrichmentRecords(plain, plainLen, recordBytes, recordLen)) {
        return;
    }

    const size_t expectedBytes = _enrichmentExpectedCount * ENRICHMENT_RECORD_SIZE;
    if (_enrichmentRxLen + recordLen > expectedBytes ||
        _enrichmentRxLen + recordLen > sizeof(_enrichmentRxBuf)) {
        _dropEnrichmentBatch("enrichment_overflow");
        return;
    }

    if (recordLen > 0) {
        memcpy(_enrichmentRxBuf + _enrichmentRxLen, recordBytes, recordLen);
        _enrichmentRxLen += recordLen;
    }
    if (_enrichmentRxLen < expectedBytes) {
        return;
    }

    for (size_t i = 0; i < _enrichmentExpectedCount; ++i) {
        EnrichmentRecordWire record;
        memcpy(&record,
               _enrichmentRxBuf + i * ENRICHMENT_RECORD_SIZE,
               sizeof(record));
        PendingEnrichment& out = _enrichmentBatch[i];
        out.eventId = record.eventId;
        out.lat = static_cast<float>(record.latE7) / 10000000.0f;
        out.lon = static_cast<float>(record.lonE7) / 10000000.0f;
        out.alt = static_cast<float>(record.altCm) / 100.0f;
        out.accuracy = static_cast<float>(record.accuracyDm) / 10.0f;
        out.gpsEpochUtc = record.epochUtc;
        out.noData = (record.flags & PHONE_ENRICH_FLAG_NO_DATA) != 0;
        char tagBuf[sizeof(record.tag)] = {};
        memcpy(tagBuf, record.tag, sizeof(record.tag));
        tagBuf[sizeof(tagBuf) - 1] = '\0';
        strlcpy(out.tag, tagBuf, sizeof(out.tag));
    }

    _enrichmentAvailableCount = _enrichmentExpectedCount;
    _enrichmentReady = true;
    _enrichmentConsecutiveDrops = 0;  // clean batch — reset drop escalation
    _enrichmentXferMs = millis() - _enrichmentSendMs;
    _setLinkState(LINK_READY);
    DLOG_DEBUG(TAG, "External enrichment received count=%u bytes=%u session=%lu batch=%lu",
               static_cast<unsigned>(_enrichmentAvailableCount),
               static_cast<unsigned>(_enrichmentRxLen),
               static_cast<unsigned long>(_activeEnrichmentSessionId),
               static_cast<unsigned long>(_activeEnrichmentBatchId));
}

bool WioNrfAccessory::_encodeHex(const uint8_t* data, size_t len, char* out, size_t outLen) const {
    static const char* digits = "0123456789ABCDEF";
    if (!data || !out || outLen < len * 2 + 1) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = digits[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = digits[data[i] & 0x0F];
    }
    out[len * 2] = '\0';
    return true;
}

bool WioNrfAccessory::_decodeHex(const char* hex, uint8_t* out, size_t outCap, size_t& outLen) const {
    outLen = 0;
    if (!hex || !out) {
        return false;
    }
    const size_t hexLen = strlen(hex);
    if ((hexLen & 1U) != 0 || hexLen / 2 > outCap) {
        return false;
    }
    for (size_t i = 0; i < hexLen; i += 2) {
        const uint8_t hi = _hexNibble(hex[i]);
        const uint8_t lo = _hexNibble(hex[i + 1]);
        if (hi > 0x0F || lo > 0x0F) {
            return false;
        }
        out[outLen++] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

uint8_t WioNrfAccessory::_hexNibble(char c) const {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    return 0xFF;
}

bool WioNrfAccessory::hasFreshGpsFix() const {
    return _gpsAvailable && (millis() - _lastGpsFixMs) <= WIO_GPS_STALE_MS;
}

bool WioNrfAccessory::getBestTimeEpoch(uint32_t& epochUtc) const {
    if (!_timeTrusted) {
        return false;
    }
    const uint32_t ageMs = millis() - _lastGpsFixMs;
    if (ageMs > WIO_GPS_TIME_HOLDOVER_MS) {
        return false;
    }
    epochUtc = _gpsEpochAtFix + (ageMs / 1000UL);
    return true;
}

bool WioNrfAccessory::_validateGpsFix(float lat, float lon, float alt, float accuracy,
                                      uint32_t epochUtc) const {
    if (!isfinite(lat) || !isfinite(lon) || !isfinite(alt) || !isfinite(accuracy)) {
        return false;
    }
    if (fabsf(lat) > WIO_GPS_MAX_ABS_LAT || fabsf(lon) > WIO_GPS_MAX_ABS_LON) {
        return false;
    }
    if (alt < WIO_GPS_MIN_ALT_M || alt > WIO_GPS_MAX_ALT_M) {
        return false;
    }
    if (accuracy < 0.0f || accuracy > WIO_GPS_MAX_ACCURACY_M) {
        return false;
    }
    if (epochUtc < 1609459200UL) {  // 2021-01-01 UTC
        return false;
    }
    return true;
}

void WioNrfAccessory::_setGpsUnavailable(bool clearCoordinates) {
    _gpsAvailable = false;
    if (clearCoordinates) {
        _gpsLat = 0.0f;
        _gpsLon = 0.0f;
        _gpsAlt = 0.0f;
        _gpsAccuracy = 0.0f;
        _gpsEpochAtFix = 0;
        _timeTrusted = false;
    }
}

void WioNrfAccessory::_handleGpsBytes(const uint8_t* data, size_t len) {
    if (!_secureSession.isReady()) {
        return;
    }

    uint8_t plain[PHONE_GPS_FRAME_SIZE] = {};
    size_t plainLen = 0;
    if (!_secureSession.decrypt(PHONE_SECURE_CHANNEL_GPS,
                                data,
                                len,
                                plain,
                                sizeof(plain),
                                plainLen)) {
        DLOG_WARN(TAG, "gps frame decrypt failed");
        return;
    }
    if (plainLen < PHONE_GPS_FRAME_SIZE) {
        DLOG_WARN(TAG, "gps frame too short len=%u",
                  static_cast<unsigned>(plainLen));
        return;
    }

    PhoneGpsFrameV1 frame;
    memcpy(&frame, plain, sizeof(frame));

    if (frame.version != COMPANION_PROTOCOL_VERSION) {
        DLOG_WARN(TAG, "gps frame version=%u",
                  static_cast<unsigned>(frame.version));
        return;
    }

    if ((frame.flags & PHONE_GPS_FLAG_VALID) == 0) {
        // No live location fix, but the phone still knows the wall-clock time
        // (its NTP clock is valid even without a GPS lock). Accept a time-only
        // frame so the device can establish trusted UTC before/without a fix —
        // this is what lets records be dated and later enriched.
        if ((frame.flags & PHONE_GPS_FLAG_TIME_TRUSTED) != 0 &&
            frame.epochUtc >= 1609459200UL) {  // >= 2021-01-01 UTC
            _lastGpsFixMs = millis();
            _gpsEpochAtFix = frame.epochUtc;
            _timeTrusted = true;
            // Adopt the phone's UTC immediately rather than waiting for the
            // 1s TimeService tick — the enrich window opens within ~200ms of
            // auth, so the clock must be valid before the record walk runs.
            TIME_SVC.syncFromEpoch(frame.epochUtc, TIME_SOURCE_GPS, _lastGpsFixMs);
        }
        _setGpsUnavailable(false);  // location absent; preserves trusted time
        return;
    }

    const float lat = static_cast<float>(frame.latE7) / 10000000.0f;
    const float lon = static_cast<float>(frame.lonE7) / 10000000.0f;
    const float alt = static_cast<float>(frame.altCm) / 100.0f;
    const float acc = static_cast<float>(frame.accuracyDm) / 10.0f;

    if (!_validateGpsFix(lat, lon, alt, acc, frame.epochUtc)) {
        DLOG_WARN(TAG, "gps frame rejected lat=%.6f lon=%.6f acc=%.1f",
                  lat, lon, acc);
        return;
    }

    _gpsLat = lat;
    _gpsLon = lon;
    _gpsAlt = alt;
    _gpsAccuracy = acc;
    _gpsAvailable = true;
    _lastGpsFixMs = millis();
    _gpsEpochAtFix = frame.epochUtc;
    _timeTrusted = (frame.flags & PHONE_GPS_FLAG_TIME_TRUSTED) != 0;
    if (_timeTrusted) {
        TIME_SVC.syncFromEpoch(frame.epochUtc, TIME_SOURCE_GPS, _lastGpsFixMs);
    }

    GPSFix fix;
    fix.lat = lat;
    fix.lon = lon;
    fix.accuracy = acc;
    fix.valid = true;
    fix.timestamp = _lastGpsFixMs;
    SESS.updateGPS(fix);
}

bool WioNrfAccessory::publishStorageSnapshot(const PhoneStorageFrameV1& frame) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (!isPhoneCompanionReady()) {
        return false;
    }

    size_t secureLen = 0;
    if (!_secureSession.encrypt(PHONE_SECURE_CHANNEL_STORAGE,
                                reinterpret_cast<const uint8_t*>(&frame),
                                sizeof(frame),
                                _storageSecureTxBuf,
                                sizeof(_storageSecureTxBuf),
                                secureLen)) {
        DLOG_WARN(TAG, "storage snapshot encrypt failed");
        return false;
    }

    if (!_sendBleWrite("storage", _storageSecureTxBuf, secureLen)) {
        DLOG_WARN(TAG, "storage snapshot uart write failed");
        return false;
    }

    DLOG_DEBUG(TAG,
              "storage snapshot sent pendingUp=%lu/%lu pendingEnrich=%lu/%lu usedPct=%u",
              static_cast<unsigned long>(frame.pendingUploadMission),
              static_cast<unsigned long>(frame.pendingUploadNoise),
              static_cast<unsigned long>(frame.pendingEnrichMission),
              static_cast<unsigned long>(frame.pendingEnrichNoise),
              static_cast<unsigned>(frame.usedPct));
    return true;
#else
    (void)frame;
    return false;
#endif
}

bool WioNrfAccessory::publishLogStreamChunk(const uint8_t* plain, size_t plainLen) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (!plain || plainLen == 0 || plainLen > LOG_STREAM_CHUNK_FRAME_MAX) {
        return false;
    }
    if (!isPhoneCompanionReady()) {
        return false;
    }

    size_t secureLen = 0;
    if (!_secureSession.encrypt(PHONE_SECURE_CHANNEL_LOG_STREAM,
                                plain, plainLen,
                                _logStreamSecureBuf, sizeof(_logStreamSecureBuf),
                                secureLen)) {
        return false;
    }

    return _sendBleWrite("log_stream", _logStreamSecureBuf, secureLen);
#else
    (void)plain;
    (void)plainLen;
    return false;
#endif
}

bool WioNrfAccessory::publishDashboardStreamChunk(const uint8_t* plain, size_t plainLen) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (!plain || plainLen == 0 || plainLen > DASHBOARD_STREAM_CHUNK_FRAME_MAX) {
        return false;
    }
    if (!isPhoneCompanionReady()) {
        return false;
    }

    size_t secureLen = 0;
    if (!_secureSession.encrypt(PHONE_SECURE_CHANNEL_DASHBOARD_STREAM,
                                plain, plainLen,
                                _dashboardStreamSecureBuf,
                                sizeof(_dashboardStreamSecureBuf),
                                secureLen)) {
        return false;
    }

    return _sendBleWrite("dashboard_stream",
                         _dashboardStreamSecureBuf, secureLen);
#else
    (void)plain;
    (void)plainLen;
    return false;
#endif
}

bool WioNrfAccessory::publishNotification(const uint8_t* plain, size_t plainLen) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (!plain || plainLen == 0 || plainLen > PHONE_NOTIFICATION_FRAME_MAX) {
        return false;
    }
    if (!isPhoneCompanionReady()) {
        return false;
    }

    size_t secureLen = 0;
    if (!_secureSession.encrypt(PHONE_SECURE_CHANNEL_NOTIFICATION,
                                plain, plainLen,
                                _notificationSecureBuf,
                                sizeof(_notificationSecureBuf),
                                secureLen)) {
        return false;
    }

    return _sendBleWrite("notification",
                         _notificationSecureBuf, secureLen);
#else
    (void)plain;
    (void)plainLen;
    return false;
#endif
}

bool WioNrfAccessory::requestTextInput(const char* prompt, uint32_t timeoutMs) {
#if WIO_NRF_ACCESSORY_ENABLED
    if (!prompt || !prompt[0]) {
        return false;
    }
    if (!_present || !_bleProxy) {
        return false;
    }
    if (_textInputPending) {
        DLOG_WARN(TAG, "text input already pending");
        return false;
    }

    if (timeoutMs < WIO_TEXT_INPUT_TIMEOUT_MIN_MS) {
        timeoutMs = WIO_TEXT_INPUT_TIMEOUT_MIN_MS;
    } else if (timeoutMs > WIO_TEXT_INPUT_TIMEOUT_MAX_MS) {
        timeoutMs = WIO_TEXT_INPUT_TIMEOUT_MAX_MS;
    }

    strlcpy(_promptBuf, prompt, sizeof(_promptBuf));
    memset(_resultBuf, 0, sizeof(_resultBuf));
    _textInputPending = true;
    _textInputReady = false;
    _textInputDeadlineMs = millis() + timeoutMs;
    _textInputToken++;

    const size_t plen = strlen(_promptBuf);
    if (!_sendBleWrite("text_prompt",
                       reinterpret_cast<const uint8_t*>(_promptBuf),
                       plen)) {
        DLOG_WARN(TAG, "text input uart write failed");
        _clearTextInputState("uart_write_failed");
        return false;
    }

    DLOG_INFO(TAG, "text input requested token=%u prompt=%s",
              static_cast<unsigned>(_textInputToken),
              _promptBuf);
    return true;
#else
    (void)prompt;
    (void)timeoutMs;
    return false;
#endif
}

bool WioNrfAccessory::consumeTextInput(char* out, size_t outLen) {
    if (!out || outLen == 0 || !_textInputReady) {
        return false;
    }
    strlcpy(out, _resultBuf, outLen);
    DLOG_INFO(TAG, "text input consumed token=%u",
              static_cast<unsigned>(_textInputToken));
    _clearTextInputState("consumed");
    return true;
}

void WioNrfAccessory::cancelTextInput(const char* reason) {
    if (!_textInputPending) {
        return;
    }
    DLOG_WARN(TAG, "text input cancelled reason=%s",
              reason ? reason : "-");
    _clearTextInputState(reason ? reason : "cancelled");
}

void WioNrfAccessory::_handleTextInputBytes(const uint8_t* data, size_t len) {
    if (!_textInputPending) {
        return;
    }
    if (_textInputReady) {
        return;
    }
    if (!data || len == 0 || len >= sizeof(_resultBuf)) {
        return;
    }

    size_t cleanLen = len;
    while (cleanLen > 0 &&
           (data[cleanLen - 1] == '\r' ||
            data[cleanLen - 1] == '\n' ||
            data[cleanLen - 1] == '\0')) {
        cleanLen--;
    }
    if (cleanLen == 0 || cleanLen >= sizeof(_resultBuf)) {
        return;
    }
    for (size_t i = 0; i < cleanLen; ++i) {
        if (!isPrintableTextByte(data[i]) && data[i] < 0x80) {
            DLOG_WARN(TAG, "text input rejected: non-printable");
            return;
        }
    }

    memcpy(_resultBuf, data, cleanLen);
    _resultBuf[cleanLen] = '\0';
    _textInputReady = true;
    _textInputDeadlineMs = 0;
    DLOG_INFO(TAG, "text input received token=%u len=%u",
              static_cast<unsigned>(_textInputToken),
              static_cast<unsigned>(cleanLen));
}

void WioNrfAccessory::_clearTextInputState(const char* reason) {
    const bool wasPending = _textInputPending;
    _textInputPending = false;
    _textInputReady = false;
    _textInputDeadlineMs = 0;
    memset(_resultBuf, 0, sizeof(_resultBuf));
    // Only push a clear-prompt to the WIO if the phone link is still up.
    // On phone disconnect there's no one to clear for, and the seq churn
    // would interfere with the next probe handshake.
    if (wasPending && _phoneConnected && _bleProxy) {
        _sendBleWrite("text_prompt",
                      reinterpret_cast<const uint8_t*>(""),
                      0);
    }
    memset(_promptBuf, 0, sizeof(_promptBuf));
    (void)reason;
}

void WioNrfAccessory::_checkTextInputTimeout(uint32_t now) {
    if (!_textInputPending || _textInputReady || _textInputDeadlineMs == 0) {
        return;
    }
    if (static_cast<int32_t>(now - _textInputDeadlineMs) < 0) {
        return;
    }
    DLOG_WARN(TAG, "text input timeout token=%u",
              static_cast<unsigned>(_textInputToken));
    _clearTextInputState("timeout");
}

const char* WioNrfAccessory::linkStateName() const {
    switch (_linkState) {
        case LINK_IDLE:               return "idle";
        case LINK_PROBING:            return "probing";
        case LINK_AUTHENTICATING:     return "auth";
        case LINK_READY:              return "ready";
        case LINK_SENDING_BATCH:      return "send_batch";
        case LINK_WAITING_ENRICHMENT: return "wait_enrich";
        case LINK_FAILED:             return "failed";
        default:                      return "?";
    }
}
