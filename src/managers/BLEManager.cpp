
#include "BLEManager.h"

#include <ctype.h>
#include <math.h>
#include <string.h>
#include "esp_bt.h"
#include <esp_heap_caps.h>

#include "../SecretsConfig.h"
#include "../core/DebugLog.h"
#include "../core/EventBus.h"
#include "../core/Session.h"

namespace {
constexpr const char* TAG = "BLE";

constexpr uint32_t SCAN_WINDOW_MS            = 4000UL;
constexpr uint32_t SCAN_GAP_MS               = 15000UL;
constexpr uint32_t CONNECT_TIMEOUT_MS        = 6000UL;
constexpr uint32_t CONNECT_WATCHDOG_MS       = 12000UL;
constexpr uint32_t GPS_POLL_MS               = 5000UL;
constexpr uint32_t CONTROL_POLL_MS           = 2000UL;
constexpr uint32_t GPS_STALE_MS              = 45000UL;
constexpr uint32_t GPS_TIME_HOLDOVER_MS      = 1800000UL;
constexpr uint32_t STATUS_REFRESH_MS         = 1000UL;
constexpr uint32_t WG_CONFIRM_TIMEOUT_MS     = 15000UL;
constexpr uint32_t WG_ARM_WINDOW_MS          = 8000UL;
constexpr uint32_t STACK_LOG_INTERVAL_MS     = 30000UL;
constexpr uint32_t ENRICHMENT_TIMEOUT_MS     = 12000UL;

constexpr float GPS_MAX_ABS_LAT              = 90.0f;
constexpr float GPS_MAX_ABS_LON              = 180.0f;
constexpr float GPS_MIN_ALT_M                = -500.0f;
constexpr float GPS_MAX_ALT_M                = 20000.0f;
constexpr float GPS_MAX_ACCURACY_M           = 1000.0f;

constexpr uint32_t RECONNECT_BACKOFF_MS[3]   = { 2000UL, 5000UL, 10000UL };

constexpr uint8_t NOTIF_INFO = 1;
constexpr uint8_t NOTIF_WARN = 2;

struct __attribute__((packed)) EnrichmentRecordWire {
    uint32_t eventId;
    int32_t  latE7;
    int32_t  lonE7;
    int32_t  altCm;
    uint16_t accuracyDm;
    uint32_t epochUtc;
    uint8_t  flags;
    char     tag[24];
};

template <typename T>
T clampValue(T v, T lo, T hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

bool isPrintableTextByte(uint8_t c) {
    if (c >= 0x20) {
        return true;
    }
    return false;
}

bool equalsIgnoreCase(const char* a, const char* b) {
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (tolower(static_cast<unsigned char>(*a)) !=
            tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0');
}

bool startsWithIgnoreCase(const char* haystack, const char* prefix) {
    if (!haystack || !prefix) {
        return false;
    }
    while (*prefix) {
        if (*haystack == '\0') {
            return false;
        }
        if (tolower(static_cast<unsigned char>(*haystack)) !=
            tolower(static_cast<unsigned char>(*prefix))) {
            return false;
        }
        ++haystack;
        ++prefix;
    }
    return true;
}

BLEManager* s_bleInstance = nullptr;
}  // namespace

static_assert(sizeof(PhoneGpsFrameV1) == 20, "PhoneGpsFrameV1 size mismatch");
static_assert(sizeof(PhoneControlFrameV1) == 4, "PhoneControlFrameV1 size mismatch");
static_assert(sizeof(EventBatchRecord) == 10, "EventBatchRecord size mismatch");
static_assert(sizeof(EnrichmentRecordWire) == 47, "EnrichmentRecordWire size mismatch");

BLEManager BLE_MGR;

BLEManager::BLEManager()
    : _scanCallbacks(*this),
      _clientCallbacks(*this),
      _serverCallbacks(*this),
      _textInputCallbacks(*this) {
    memset(_sessionId, 0, sizeof(_sessionId));
    memset(_deviceName, 0, sizeof(_deviceName));
    memset(_targetDeviceName, 0, sizeof(_targetDeviceName));
    memset(_targetServiceUUID, 0, sizeof(_targetServiceUUID));
    memset(_connectedDeviceName, 0, sizeof(_connectedDeviceName));
    memset(_connectedPeerAddr, 0, sizeof(_connectedPeerAddr));
    memset(_gpsTimeIso, 0, sizeof(_gpsTimeIso));
    memset(_promptBuf, 0, sizeof(_promptBuf));
    memset(_resultBuf, 0, sizeof(_resultBuf));
    memset(_receiptBuf, 0, sizeof(_receiptBuf));
    memset(_statusBuf, 0, sizeof(_statusBuf));
    memset(_metadataBuf, 0, sizeof(_metadataBuf));

    strlcpy(_targetDeviceName, SPECTRE_BLE_TARGET_DEVICE_NAME, sizeof(_targetDeviceName));
    strlcpy(_targetServiceUUID, PHONE_SERVICE_UUID, sizeof(_targetServiceUUID));
    strlcpy(_receiptBuf, "IDLE", sizeof(_receiptBuf));
}

void BLEManager::begin() {
    if (_begun) {
        return;
    }

    s_bleInstance = this;
    _resetState();
    _buildDeviceName();

    _workerStack = static_cast<StackType_t*>(heap_caps_malloc(
        WORKER_STACK_WORDS * sizeof(StackType_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!_workerStack) {
        DLOG_ERROR(TAG, "worker stack alloc failed");
        return;
    }

    _workerTask = xTaskCreateStaticPinnedToCore(
        _workerTaskEntry,
        "BLEWorker",
        WORKER_STACK_WORDS,
        this,
        2,
        _workerStack,
        &_workerTaskBuffer,
        0
    );
    if (!_workerTask) {
        DLOG_ERROR(TAG, "worker task create failed");
        free(_workerStack);
        _workerStack = nullptr;
        return;
    }

    DLOG_INFO(TAG, "init device=%s targetName=%s", _deviceName, _targetDeviceName);

    NimBLEDevice::init(std::string(_deviceName));
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    _setupServer();
    _setupScanner();

    _publishBleState();
    _publishGpsState();
    _publishTextInputState();

    _nextActionMs = millis();
    _lastStackLogMs = _nextActionMs;
    _workerMinFreeStackBytes = 0;
    _radioEnabled = false;
    _begun = true;
}

void BLEManager::shutdown() {
    if (!_begun) {
        return;
    }

    DLOG_INFO(TAG, "shutdown");
    setRadioEnabled(false);

    if (_workerTask) {
        TaskHandle_t task = _workerTask;
        _workerTask = nullptr;
        vTaskDelete(task);
    }

    NimBLEDevice::deinit(true);

    if (_workerStack) {
        heap_caps_free(_workerStack);
        _workerStack = nullptr;
    }

    _scan = nullptr;
    _client = nullptr;
    _remoteService = nullptr;
    _gpsRemoteChar = nullptr;
    _controlRemoteChar = nullptr;
    _metaRemoteChar = nullptr;
    _eventBatchRemoteChar = nullptr;
    _enrichmentRemoteChar = nullptr;
    _server = nullptr;
    _textService = nullptr;
    _promptChar = nullptr;
    _inputChar = nullptr;
    _receiptChar = nullptr;
    _statusChar = nullptr;

    _begun = false;
    _radioEnabled = false;
    _scanActive = false;
    _clientConnected = false;
    _serverConnected = false;
    _advertisingActive = false;
    _connectResultPending = false;
    _connectResultOk = false;
    _state = BLE_IDLE;
    _lastStackLogMs = 0;
    _workerMinFreeStackBytes = 0;

    _publishBleState();
    _publishTextInputState();
}

void BLEManager::setRadioEnabled(bool enabled) {
    if (!_begun || _radioEnabled == enabled) {
        return;
    }

    _radioEnabled = enabled;

    if (!_radioEnabled) {
    if (_scan && _scan->isScanning()) {
        _scan->stop();
    }
    _scanActive = false;
    _nextActionMs = millis() + SCAN_GAP_MS;

    if (_advertisingActive) {
        _ensureAdvertising(false);
    }

    if (_client && _client->isConnected()) {
        _ignoreDisconnectOnce = true;
        _client->disconnect();
    }

    _clientConnected = false;
    _clearRemoteHandles();
    _state = BLE_IDLE;
    _publishBleState();
    DLOG_INFO(TAG, "radio disabled");
    return;
   }

    _nextActionMs = millis();
    if (_textInputPending && !_serverConnected) {
        _ensureAdvertising(true);
    }
    DLOG_INFO(TAG, "radio enabled");
}

void BLEManager::tick() {
    if (!_begun) {
        return;
    }

    const uint32_t now = millis();
    if (_workerTask && (now - _lastStackLogMs) >= STACK_LOG_INTERVAL_MS) {
        const uint32_t freeBytes =
            static_cast<uint32_t>(uxTaskGetStackHighWaterMark(_workerTask) *
                                  sizeof(StackType_t));
        if (_workerMinFreeStackBytes == 0 || freeBytes < _workerMinFreeStackBytes) {
            _workerMinFreeStackBytes = freeBytes;
        }
        DLOG_INFO(TAG, "worker stack free=%luB min=%luB",
                  (unsigned long)freeBytes,
                  (unsigned long)_workerMinFreeStackBytes);
        _lastStackLogMs = now;
    }

    _checkTimeouts();
    if (!_radioEnabled) {
        return;
    }

    _handleConnectOutcome();

    if (_state == BLE_SUBSCRIBED) {
        if (!_gpsNotifyEnabled && _gpsRemoteChar && (now - _lastGpsPollMs) >= GPS_POLL_MS) {
            _lastGpsPollMs = now;
            _queueWorker(WORKER_JOB_POLL_GPS);
        }

        if (!_controlNotifyEnabled && _controlRemoteChar &&
            (now - _lastControlPollMs) >= CONTROL_POLL_MS) {
            _lastControlPollMs = now;
            _queueWorker(WORKER_JOB_POLL_CONTROL);
        }

        if (_enrichmentRequestPending && !_enrichmentSendQueued) {
            _enrichmentSendQueued = true;
            _queueWorker(WORKER_JOB_SEND_ENRICH);
        }
    }

    if (_enrichmentInFlight && _enrichmentDeadlineMs != 0 && now >= _enrichmentDeadlineMs) {
        DLOG_WARN(TAG, "enrichment request timed out");
        _enrichmentInFlight = false;
        _enrichmentRequestPending = false;
        _enrichmentSendQueued = false;
        _enrichmentExpectedCount = 0;
        _enrichmentAvailableCount = 0;
        _enrichmentRxLen = 0;
        _eventBatchTxLen = 0;
        _enrichmentDeadlineMs = 0;
    }

    if (_gpsAvailable && (now - _lastGpsFixMs) > GPS_STALE_MS) {
        _setGpsUnavailable(false);
    }

    if (_state == BLE_SCANNING && (now - _scanStartedMs) >= SCAN_WINDOW_MS) {
        _stopScanWindow();
        _state = BLE_IDLE;
        _nextActionMs = now + SCAN_GAP_MS;
        DLOG_INFO(TAG, "scan window ended");
    }

    if (_state == BLE_CONNECTING && (now - _connectStartedMs) > CONNECT_WATCHDOG_MS) {
        DLOG_WARN(TAG, "connect watchdog expired");
        _scheduleReconnect("connect watchdog");
    }

    if (_state == BLE_IDLE && now >= _nextActionMs) {
        if (_directReconnectPending && _haveTargetAddress) {
            _startConnectAttempt();
        } else {
            _startScanWindow();
        }
    }

    if ((now - _lastStatusRefreshMs) >= STATUS_REFRESH_MS) {
        _lastStatusRefreshMs = now;
        _refreshStatusCharacteristic();
    }
    if (millis() < _nextScanAllowedMs) {
    return;
    }
}

void BLEManager::setTargetDeviceName(const char* deviceName) {
    if (!deviceName) {
        return;
    }
    strlcpy(_targetDeviceName, deviceName, sizeof(_targetDeviceName));
}

void BLEManager::setTargetServiceUUID(const char* serviceUuid) {
    if (!serviceUuid) {
        return;
    }
    strlcpy(_targetServiceUUID, serviceUuid, sizeof(_targetServiceUUID));
}

bool BLEManager::handleButtonEvent(ButtonEvent evt) {
    if (!_wgConfirmPending) {
        return false;
    }

    switch (evt) {
        case BTN_A_LONG:
            _armWireGuardConfirmation();
            return true;

        case BTN_B_LONG:
            if (_wgArmed) {
                _confirmWireGuardDump();
            } else {
                _cancelWireGuardConfirmation("WG dump canceled");
            }
            return true;

        case BTN_A_SHORT:
        case BTN_B_SHORT:
            _cancelWireGuardConfirmation("WG dump canceled");
            return true;

        case BTN_NONE:
        default:
            return false;
    }
}

bool BLEManager::requestTextInput(const char* reason, uint32_t timeoutMs) {
    if (!reason || !reason[0]) {
        return false;
    }

    if (!_begun || !_radioEnabled) {
        DLOG_WARN(TAG, "text input rejected: radio disabled");
        return false;
    }

    if (_textInputPending) {
        DLOG_WARN(TAG, "text input already pending");
        _setReceipt("BUSY");
        return false;
    }

    _textInputPending = true;
    _textInputReady = false;
    _textInputDeadlineMs = millis() + clampValue<uint32_t>(timeoutMs, 10000UL, 300000UL);
    _textInputToken++;
    memset(_resultBuf, 0, sizeof(_resultBuf));

    _setPrompt(reason);
    _setReceipt("PENDING");
    _publishTextInputState();
    _refreshStatusCharacteristic();
    _ensureAdvertising(true);

    DLOG_INFO(TAG, "text input requested token=%u prompt=%s",
              static_cast<unsigned>(_textInputToken),
              _promptBuf);
    _pushNotification(NOTIF_INFO, "BLE input requested");
    return true;
}

bool BLEManager::consumeTextInput(char* out, size_t outLen) {
    if (!out || outLen == 0 || !_textInputReady) {
        return false;
    }

    strlcpy(out, _resultBuf, outLen);
    DLOG_INFO(TAG, "text input consumed token=%u", static_cast<unsigned>(_textInputToken));

    _clearTextInputState(true, "CONSUMED");
    return true;
}

void BLEManager::cancelTextInput() {
    if (!_textInputPending) {
        return;
    }
    DLOG_WARN(TAG, "text input canceled");
    _clearTextInputState(true, "CANCELLED");
}

bool BLEManager::requestEnrichmentBatch(const EventBatchRecord* records, size_t count) {
    if (!records || count == 0) {
        return false;
    }

    if (!_begun || !_radioEnabled || _state != BLE_SUBSCRIBED) {
        DLOG_WARN(TAG, "enrichment request rejected: link not ready");
        return false;
    }

    if (!_eventBatchRemoteChar || !_enrichmentRemoteChar) {
        DLOG_WARN(TAG, "enrichment request rejected: missing chars");
        return false;
    }

    if (_enrichmentRequestPending || _enrichmentInFlight || _enrichmentReady) {
        DLOG_WARN(TAG, "enrichment request rejected: busy");
        return false;
    }

    if (count > ENRICHMENT_MAX_RECORDS) {
        DLOG_WARN(TAG, "enrichment request rejected: count=%u",
                  static_cast<unsigned>(count));
        return false;
    }

    const size_t payloadLen = count * sizeof(EventBatchRecord);
    memcpy(_eventBatchTxBuf, records, payloadLen);
    _eventBatchTxLen = payloadLen;
    _enrichmentExpectedCount = count;
    _enrichmentRequestPending = true;
    _enrichmentInFlight = false;
    _enrichmentReady = false;
    _enrichmentSendQueued = false;
    _enrichmentBatchAcked = false;
    _enrichmentRxLen = 0;
    _enrichmentAvailableCount = 0;
    _enrichmentDeadlineMs = millis() + ENRICHMENT_TIMEOUT_MS;

    DLOG_INFO(TAG, "enrichment request queued count=%u bytes=%u",
              static_cast<unsigned>(count),
              static_cast<unsigned>(payloadLen));
    return true;
}

bool BLEManager::consumeEnrichmentBatch(PendingEnrichment* out,
                                        size_t maxCount,
                                        size_t& outCount) {
    outCount = 0;
    if (!out || maxCount == 0) {
        return false;
    }

    if (!_enrichmentReady || _enrichmentAvailableCount == 0) {
        return false;
    }

    if (maxCount < _enrichmentAvailableCount) {
        DLOG_WARN(TAG, "enrichment consume rejected: maxCount=%u needed=%u",
                  static_cast<unsigned>(maxCount),
                  static_cast<unsigned>(_enrichmentAvailableCount));
        return false;
    }

    const size_t count = _enrichmentAvailableCount;
    for (size_t i = 0; i < count; ++i) {
        out[i] = _enrichmentBatch[i];
    }

    outCount = count;
    _enrichmentReady = false;
    _enrichmentAvailableCount = 0;
    _enrichmentExpectedCount = 0;
    _enrichmentRxLen = 0;
    _eventBatchTxLen = 0;
    _enrichmentDeadlineMs = 0;
    memset(_enrichmentRxBuf, 0, sizeof(_enrichmentRxBuf));
    memset(_enrichmentBatch, 0, sizeof(_enrichmentBatch));

    DLOG_INFO(TAG, "enrichment batch consumed count=%u",
              static_cast<unsigned>(count));
    return true;
}

bool BLEManager::isPhoneCompanionReady() const {
    return _state == BLE_SUBSCRIBED && _eventBatchRemoteChar && _enrichmentRemoteChar;
}

bool BLEManager::consumeWireGuardDumpTrigger() {
    if (!_wgDumpTriggerLatched) {
        return false;
    }

    _wgDumpTriggerLatched = false;

    STATE_WRITE_BEGIN();
    g_state.wgDumpTriggered = false;
    STATE_WRITE_END();

    return true;
}

bool BLEManager::hasFreshGpsFix() const {
    return _gpsAvailable && (millis() - _lastGpsFixMs) <= GPS_STALE_MS;
}

bool BLEManager::formatBestTimeISO(char* out, size_t len) {
    if (!out || len == 0 || !_timeTrusted) {
        return false;
    }

    const uint32_t ageMs = millis() - _lastGpsFixMs;
    if (ageMs > GPS_TIME_HOLDOVER_MS) {
        return false;
    }

    const uint32_t epoch = _gpsEpochAtFix + (ageMs / 1000UL);
    _formatIso8601(epoch, out, len);
    return true;
}

bool BLEManager::getBestTimeEpoch(uint32_t& epochUtc) const {
    if (!_timeTrusted) {
        return false;
    }

    const uint32_t ageMs = millis() - _lastGpsFixMs;
    if (ageMs > GPS_TIME_HOLDOVER_MS) {
        return false;
    }

    epochUtc = _gpsEpochAtFix + (ageMs / 1000UL);
    return true;
}

void BLEManager::_buildDeviceName() {
    String sid = SESS.getId();
    if (sid.length() == 0) {
        sid = "FIELD";
    }

    strlcpy(_sessionId, sid.c_str(), sizeof(_sessionId));

    const char* base = _sessionId;
    const size_t sidLen = strlen(_sessionId);
    if (sidLen > 6) {
        base = _sessionId + (sidLen - 6);
    }

    snprintf(_deviceName, sizeof(_deviceName), "Spectre-%s", base);
}

void BLEManager::_setupServer() {
    _server = NimBLEDevice::createServer();
    _server->setCallbacks(&_serverCallbacks);
    _server->advertiseOnDisconnect(true);

    _textService = _server->createService(TEXT_SERVICE_UUID);
    _promptChar = _textService->createCharacteristic(
        TEXT_PROMPT_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
        sizeof(_promptBuf)
    );
    _inputChar = _textService->createCharacteristic(
        TEXT_INPUT_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR,
        sizeof(_resultBuf)
    );
    _receiptChar = _textService->createCharacteristic(
        TEXT_RECEIPT_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
        sizeof(_receiptBuf)
    );
    _statusChar = _textService->createCharacteristic(
        TEXT_STATUS_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
        sizeof(_statusBuf)
    );

    _inputChar->setCallbacks(&_textInputCallbacks);
    _textService->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv) {
        adv->addServiceUUID(TEXT_SERVICE_UUID);
        adv->setName(std::string(_deviceName));
    }

    _setPrompt("");
    _setReceipt("IDLE");
    _refreshStatusCharacteristic(false);
}

void BLEManager::_setupScanner() {
    _scan = NimBLEDevice::getScan();
    _scan->setScanCallbacks(&_scanCallbacks, false);
    _scan->setActiveScan(true);
    _scan->setInterval(45);
    _scan->setWindow(30);
}

void BLEManager::_resetState() {
    _state = BLE_IDLE;
    _scanActive = false;
    _targetFound = false;
    _directReconnectPending = false;
    _reconnectAttempt = 0;
    _clientConnected = false;
    _serverConnected = false;
    _ignoreDisconnectOnce = false;
    _gpsNotifyEnabled = false;
    _controlNotifyEnabled = false;
    _connectResultPending = false;
    _connectResultOk = false;
    _gpsAvailable = false;
    _timeTrusted = false;
    _wgActive = false;
    _wgConfirmPending = false;
    _wgArmed = false;
    _wgDumpTriggerLatched = false;
    _enrichmentRequestPending = false;
    _enrichmentInFlight = false;
    _enrichmentReady = false;
    _enrichmentNotifyEnabled = false;
    _enrichmentSendQueued = false;
    _enrichmentBatchAcked = false;
    _textInputPending = false;
    _textInputReady = false;
    _advertisingActive = false;
    _textInputToken = 0;
    _lastWgCounter = 0;
    _nextActionMs = 0;
    _scanStartedMs = 0;
    _connectStartedMs = 0;
    _lastGpsPollMs = 0;
    _lastControlPollMs = 0;
    _lastStatusRefreshMs = 0;
    _lastGpsFixMs = 0;
    _gpsEpochAtFix = 0;
    _textInputDeadlineMs = 0;
    _wgConfirmDeadlineMs = 0;
    _wgArmDeadlineMs = 0;
    _enrichmentDeadlineMs = 0;
    _gpsLat = 0.0f;
    _gpsLon = 0.0f;
    _gpsAlt = 0.0f;
    _gpsAccuracy = 0.0f;
    memset(_connectedDeviceName, 0, sizeof(_connectedDeviceName));
    memset(_connectedPeerAddr, 0, sizeof(_connectedPeerAddr));
    memset(_gpsTimeIso, 0, sizeof(_gpsTimeIso));
    memset(_promptBuf, 0, sizeof(_promptBuf));
    memset(_resultBuf, 0, sizeof(_resultBuf));
    strlcpy(_receiptBuf, "IDLE", sizeof(_receiptBuf));
    memset(_statusBuf, 0, sizeof(_statusBuf));
    memset(_metadataBuf, 0, sizeof(_metadataBuf));
    memset(_eventBatchTxBuf, 0, sizeof(_eventBatchTxBuf));
    memset(_enrichmentRxBuf, 0, sizeof(_enrichmentRxBuf));
    memset(_enrichmentBatch, 0, sizeof(_enrichmentBatch));
    _eventBatchTxLen = 0;
    _enrichmentRxLen = 0;
    _enrichmentExpectedCount = 0;
    _enrichmentAvailableCount = 0;
    _haveTargetAddress = false;
    _targetAddress = NimBLEAddress();
}

void BLEManager::_startScanWindow() {
    if (!_scan || _scanActive) {
        return;
    }

    _targetFound = false;
    _scanStartedMs = millis();
    _state = BLE_SCANNING;
    _scanActive = _scan->start(SCAN_WINDOW_MS / 1000UL, false, false);
    _lastScanStartMs = millis();

    if (_scanActive) {
        DLOG_INFO(TAG, "scan start");
    } else {
        DLOG_WARN(TAG, "scan start failed");
        _state = BLE_IDLE;
        _nextActionMs = millis() + SCAN_GAP_MS;
    }
}

void BLEManager::_stopScanWindow() {
    if (_scan && _scan->isScanning()) {
        _scan->stop();
    }
    _scanActive = false;
}

void BLEManager::_startConnectAttempt() {
    if (!_haveTargetAddress) {
        _state = BLE_IDLE;
        _nextActionMs = millis() + SCAN_GAP_MS;
        return;
    }

    if (_scanActive) {
        _stopScanWindow();
    }

    _state = BLE_CONNECTING;
    _connectStartedMs = millis();
    _connectResultPending = false;
    _connectResultOk = false;
    DLOG_INFO(TAG, "connect attempt addr=%s name=%s",
              _connectedPeerAddr[0] ? _connectedPeerAddr : "unknown",
              _connectedDeviceName[0] ? _connectedDeviceName : "unknown");
    _queueWorker(WORKER_JOB_CONNECT);
}

void BLEManager::_scheduleReconnect(const char* reason) {
    _setGpsUnavailable(false);
    _publishBleState();
    _refreshStatusCharacteristic();

    if (_scan && _scan->isScanning()) {
        _scan->stop();
    }
    _scanActive = false;

    if (_reconnectAttempt < 3 && _haveTargetAddress) {
        const uint32_t delayMs = RECONNECT_BACKOFF_MS[_reconnectAttempt];
        _reconnectAttempt++;
        _directReconnectPending = true;
        _state = BLE_IDLE;
        _nextActionMs = millis() + delayMs;
        DLOG_WARN(TAG, "%s -> reconnect %u in %lu ms",
                  reason,
                  static_cast<unsigned>(_reconnectAttempt),
                  static_cast<unsigned long>(delayMs));
        return;
    }

    _directReconnectPending = false;
    _reconnectAttempt = 0;
    _state = BLE_IDLE;
    _nextActionMs = millis() + SCAN_GAP_MS;
    DLOG_WARN(TAG, "%s -> return to scanning", reason);
}

void BLEManager::_handleConnectOutcome() {
    if (!_connectResultPending) {
        return;
    }

    _connectResultPending = false;

    if (_connectResultOk) {
        _reconnectAttempt = 0;
        _directReconnectPending = false;
        DLOG_INFO(TAG, "link ready state=%u", static_cast<unsigned>(_state));
        _publishBleState();
        _refreshStatusCharacteristic();
        return;
    }

    _scheduleReconnect("connect failed");
}

void BLEManager::_checkTimeouts() {
    const uint32_t now = millis();

    if (_textInputPending && _textInputDeadlineMs != 0 && now >= _textInputDeadlineMs) {
        DLOG_WARN(TAG, "text input timed out");
        _clearTextInputState(true, "TIMEOUT");
    }

    if (_wgConfirmPending && now >= _wgConfirmDeadlineMs) {
        _cancelWireGuardConfirmation("WG confirm timeout");
    }

    if (_wgArmed && now >= _wgArmDeadlineMs) {
        _cancelWireGuardConfirmation("WG arm timeout");
    }
}

void BLEManager::_queueWorker(uint32_t bits) {
    if (_workerTask) {
        xTaskNotify(_workerTask, bits, eSetBits);
    }
}

void BLEManager::_ensureAdvertising(bool enable) {
    if (enable == _advertisingActive) {
        return;
    }

    if (enable) {
        if (NimBLEDevice::startAdvertising()) {
            _advertisingActive = true;
            DLOG_INFO(TAG, "advertising start");
        } else {
            DLOG_WARN(TAG, "advertising start failed");
        }
    } else {
        if (NimBLEDevice::stopAdvertising()) {
            _advertisingActive = false;
            DLOG_INFO(TAG, "advertising stop");
        } else {
            _advertisingActive = false;
        }
    }
}

void BLEManager::_workerTaskEntry(void* arg) {
    static_cast<BLEManager*>(arg)->_workerLoop();
}

void BLEManager::_workerLoop() {
    while (true) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, 0xFFFFFFFFUL, &bits, portMAX_DELAY);

        if (bits & WORKER_JOB_CONNECT) {
            _doConnectJob();
        }
        if (bits & WORKER_JOB_POLL_GPS) {
            _doGpsPollJob();
        }
        if (bits & WORKER_JOB_POLL_CONTROL) {
            _doControlPollJob();
        }
        if (bits & WORKER_JOB_SEND_ENRICH) {
            _doEnrichmentSendJob();
        }
    }
}

void BLEManager::_doConnectJob() {
    if (!_haveTargetAddress) {
        _connectResultOk = false;
        _connectResultPending = true;
        return;
    }

    if (!_client) {
        _client = NimBLEDevice::createClient();
        _client->setClientCallbacks(&_clientCallbacks, false);
        _client->setConnectTimeout(static_cast<uint8_t>(CONNECT_TIMEOUT_MS / 1000UL));
        _client->setConnectionParams(12, 24, 0, 60, 0, 0);
    }

    _clearRemoteHandles();

    const bool connected = _client->connect(_targetAddress, true);
    if (!connected) {
        _connectResultOk = false;
        _connectResultPending = true;
        return;
    }

    _state = BLE_CONNECTED;

    if (!_bindRemoteCharacteristics()) {
        _ignoreDisconnectOnce = true;
        if (_client->isConnected()) {
            _client->disconnect();
        }
        _connectResultOk = false;
        _connectResultPending = true;
        return;
    }

    _connectResultOk = true;
    _connectResultPending = true;
}

void BLEManager::_doGpsPollJob() {
    if (!_gpsRemoteChar || !_client || !_client->isConnected() || !_gpsRemoteChar->canRead()) {
        return;
    }

    NimBLEAttValue value = _gpsRemoteChar->readValue();
    if (value.size() == 0) {
        return;
    }
    _handleGpsPayload(value.data(), value.size());
}

void BLEManager::_doControlPollJob() {
    if (!_controlRemoteChar || !_client || !_client->isConnected() || !_controlRemoteChar->canRead()) {
        return;
    }

    NimBLEAttValue value = _controlRemoteChar->readValue();
    if (value.size() == 0) {
        return;
    }
    _handleControlPayload(value.data(), value.size());
}

void BLEManager::_doEnrichmentSendJob() {
    _enrichmentSendQueued = false;

    if (!_eventBatchRemoteChar || !_client || !_client->isConnected()) {
        DLOG_WARN(TAG, "enrichment send failed: link down");
        _enrichmentRequestPending = false;
        return;
    }

    if (!_eventBatchRemoteChar->canWrite() && !_eventBatchRemoteChar->canWriteNoResponse()) {
        DLOG_WARN(TAG, "enrichment send failed: batch char not writable");
        _enrichmentRequestPending = false;
        return;
    }

    if (_eventBatchTxLen == 0 || _enrichmentExpectedCount == 0) {
        DLOG_WARN(TAG, "enrichment send failed: empty payload");
        _enrichmentRequestPending = false;
        return;
    }

    const bool ok = _eventBatchRemoteChar->writeValue(_eventBatchTxBuf, _eventBatchTxLen, true);
    if (!ok) {
        DLOG_WARN(TAG, "enrichment send failed: write error");
        _enrichmentRequestPending = false;
        return;
    }

    _enrichmentRequestPending = false;
    _enrichmentInFlight = true;
    _enrichmentReady = false;
    _enrichmentRxLen = 0;
    _enrichmentAvailableCount = 0;
    _enrichmentBatchAcked = false;
    _enrichmentDeadlineMs = millis() + ENRICHMENT_TIMEOUT_MS;

    DLOG_INFO(TAG, "enrichment batch sent bytes=%u count=%u",
              static_cast<unsigned>(_eventBatchTxLen),
              static_cast<unsigned>(_enrichmentExpectedCount));

    if (!_enrichmentNotifyEnabled && _enrichmentRemoteChar &&
        _enrichmentRemoteChar->canRead()) {
        NimBLEAttValue value = _enrichmentRemoteChar->readValue();
        if (value.size() > 0) {
            _handleEnrichmentPayload(value.data(), value.size());
        }
    }
}

void BLEManager::_onAdvertisedDevice(const NimBLEAdvertisedDevice* advertisedDevice) {
    if (!advertisedDevice || _state != BLE_SCANNING) {
        return;
    }

    if (!_matchesTarget(advertisedDevice)) {
        return;
    }

    _targetAddress = advertisedDevice->getAddress();
    _haveTargetAddress = true;
    _targetFound = true;
    strlcpy(_connectedPeerAddr,
            _targetAddress.toString().c_str(),
            sizeof(_connectedPeerAddr));

    if (advertisedDevice->haveName()) {
        strlcpy(_connectedDeviceName,
                advertisedDevice->getName().c_str(),
                sizeof(_connectedDeviceName));
    } else {
        strlcpy(_connectedDeviceName,
                SPECTRE_BLE_TARGET_DEVICE_NAME,
                sizeof(_connectedDeviceName));
    }

    DLOG_INFO(TAG, "target found addr=%s name=%s",
              _connectedPeerAddr,
              _connectedDeviceName);

    _stopScanWindow();
    _startConnectAttempt();
}

void BLEManager::_onClientConnected(NimBLEClient* pClient) {
    (void)pClient;
    _clientConnected = true;
    _publishBleState();
    _refreshStatusCharacteristic();
}

void BLEManager::_onClientDisconnected(NimBLEClient* pClient, int reason) {
    (void)pClient;
    _clientConnected = false;
    _clearRemoteHandles();
    _publishBleState();

    if (_ignoreDisconnectOnce) {
        _ignoreDisconnectOnce = false;
        return;
    }

    DLOG_WARN(TAG, "peer disconnected reason=%d", reason);
    _scheduleReconnect("peer disconnected");
}

void BLEManager::_onServerConnected(const NimBLEConnInfo* connInfo) {
    _serverConnected = true;
    if (connInfo) {
        strlcpy(_connectedPeerAddr,
                connInfo->getAddress().toString().c_str(),
                sizeof(_connectedPeerAddr));
    }
    _publishBleState();
    _refreshStatusCharacteristic();
}

void BLEManager::_onServerDisconnected(const NimBLEConnInfo* connInfo, int reason) {
    (void)connInfo;
    _serverConnected = false;
    _publishBleState();
    _refreshStatusCharacteristic();
    DLOG_INFO(TAG, "server disconnect reason=%d", reason);

    if (_textInputPending) {
        _ensureAdvertising(true);
    }
}

void BLEManager::_onTextInputWrite(NimBLECharacteristic* pCharacteristic) {
    if (!pCharacteristic) {
        return;
    }

    NimBLEAttValue value = pCharacteristic->getValue();
    if (value.size() == 0) {
        _setReceipt("REJECTED");
        return;
    }

    _acceptTextPayload(value.data(), value.size());
}

void BLEManager::_gpsNotifyThunk(NimBLERemoteCharacteristic* chr,
                                 uint8_t* data,
                                 size_t len,
                                 bool isNotify) {
    (void)chr;
    (void)isNotify;
    if (s_bleInstance) {
        s_bleInstance->_handleGpsPayload(data, len);
    }
}

void BLEManager::_controlNotifyThunk(NimBLERemoteCharacteristic* chr,
                                     uint8_t* data,
                                     size_t len,
                                     bool isNotify) {
    (void)chr;
    (void)isNotify;
    if (s_bleInstance) {
        s_bleInstance->_handleControlPayload(data, len);
    }
}

void BLEManager::_enrichmentNotifyThunk(NimBLERemoteCharacteristic* chr,
                                        uint8_t* data,
                                        size_t len,
                                        bool isNotify) {
    (void)chr;
    (void)isNotify;
    if (s_bleInstance) {
        s_bleInstance->_handleEnrichmentPayload(data, len);
    }
}

bool BLEManager::_bindRemoteCharacteristics() {
    if (!_client || !_client->isConnected()) {
        return false;
    }

    _remoteService = _client->getService(_targetServiceUUID);
    if (!_remoteService) {
        DLOG_ERROR(TAG, "remote service missing %s", _targetServiceUUID);
        return false;
    }

    _gpsRemoteChar = _remoteService->getCharacteristic(PHONE_GPS_CHAR_UUID);
    _controlRemoteChar = _remoteService->getCharacteristic(PHONE_CONTROL_CHAR_UUID);
    _metaRemoteChar = _remoteService->getCharacteristic(PHONE_META_CHAR_UUID);
    _eventBatchRemoteChar = _remoteService->getCharacteristic(PHONE_EVENT_BATCH_UUID);
    _enrichmentRemoteChar = _remoteService->getCharacteristic(PHONE_ENRICHMENT_UUID);

    if (!_gpsRemoteChar && !_controlRemoteChar) {
        DLOG_ERROR(TAG, "remote service has no usable chars");
        return false;
    }

    _gpsNotifyEnabled = false;
    _controlNotifyEnabled = false;
    _enrichmentNotifyEnabled = false;

    if (_gpsRemoteChar && _gpsRemoteChar->canNotify()) {
        _gpsNotifyEnabled = _gpsRemoteChar->subscribe(true, _gpsNotifyThunk, false);
    }

    if (_controlRemoteChar && _controlRemoteChar->canNotify()) {
        _controlNotifyEnabled = _controlRemoteChar->subscribe(true, _controlNotifyThunk, false);
    }

    if (_enrichmentRemoteChar && _enrichmentRemoteChar->canNotify()) {
        _enrichmentNotifyEnabled = _enrichmentRemoteChar->subscribe(true, _enrichmentNotifyThunk, false);
    }

    if (_metaRemoteChar && _metaRemoteChar->canRead()) {
        NimBLEAttValue meta = _metaRemoteChar->readValue();
        const size_t n = clampValue<size_t>(meta.size(), 0, sizeof(_metadataBuf) - 1);
        memcpy(_metadataBuf, meta.data(), n);
        _metadataBuf[n] = '\0';
    }

    if (_gpsRemoteChar && _gpsRemoteChar->canRead()) {
        NimBLEAttValue gps = _gpsRemoteChar->readValue();
        if (gps.size() > 0) {
            _handleGpsPayload(gps.data(), gps.size());
        }
    }

    if (_controlRemoteChar && _controlRemoteChar->canRead()) {
        NimBLEAttValue ctrl = _controlRemoteChar->readValue();
        if (ctrl.size() > 0) {
            _handleControlPayload(ctrl.data(), ctrl.size());
        }
    }

    _state = BLE_SUBSCRIBED;
    DLOG_INFO(TAG, "remote bound gps=%d ctrl=%d meta=%d batch=%d enrich=%d notify(gps=%d ctrl=%d enrich=%d)",
              _gpsRemoteChar ? 1 : 0,
              _controlRemoteChar ? 1 : 0,
              _metaRemoteChar ? 1 : 0,
              _eventBatchRemoteChar ? 1 : 0,
              _enrichmentRemoteChar ? 1 : 0,
              _gpsNotifyEnabled ? 1 : 0,
              _controlNotifyEnabled ? 1 : 0,
              _enrichmentNotifyEnabled ? 1 : 0);
    return true;
}

void BLEManager::_clearRemoteHandles() {
    _remoteService = nullptr;
    _gpsRemoteChar = nullptr;
    _controlRemoteChar = nullptr;
    _metaRemoteChar = nullptr;
    _eventBatchRemoteChar = nullptr;
    _enrichmentRemoteChar = nullptr;
    _gpsNotifyEnabled = false;
    _controlNotifyEnabled = false;
    _enrichmentNotifyEnabled = false;
    _enrichmentRequestPending = false;
    _enrichmentInFlight = false;
    _enrichmentReady = false;
    _enrichmentSendQueued = false;
    _enrichmentBatchAcked = false;
    _enrichmentDeadlineMs = 0;
    _eventBatchTxLen = 0;
    _enrichmentRxLen = 0;
    _enrichmentExpectedCount = 0;
    _enrichmentAvailableCount = 0;
}

bool BLEManager::_matchesTarget(const NimBLEAdvertisedDevice* advertisedDevice) {
    bool nameMatch = false;
    bool serviceMatch = false;

    if (_targetDeviceName[0] != '\0' && advertisedDevice->haveName()) {
        const std::string advName = advertisedDevice->getName();
        nameMatch = equalsIgnoreCase(advName.c_str(), _targetDeviceName) ||
                    startsWithIgnoreCase(advName.c_str(), _targetDeviceName);
    }

    if (_targetServiceUUID[0] != '\0' && advertisedDevice->haveServiceUUID()) {
        serviceMatch = advertisedDevice->isAdvertisingService(NimBLEUUID(_targetServiceUUID));
    }

    return nameMatch || serviceMatch;
}

void BLEManager::_armWireGuardConfirmation() {
    _wgArmed = true;
    _wgArmDeadlineMs = millis() + WG_ARM_WINDOW_MS;
    _refreshStatusCharacteristic();
    _pushNotification(NOTIF_WARN, "WG armed: hold B");
    DLOG_WARN(TAG, "WG dump armed");
}

void BLEManager::_confirmWireGuardDump() {
    _wgConfirmPending = false;
    _wgArmed = false;
    _wgConfirmDeadlineMs = 0;
    _wgArmDeadlineMs = 0;
    _wgDumpTriggerLatched = true;

    STATE_WRITE_BEGIN();
    g_state.wgDumpTriggered = true;
    STATE_WRITE_END();

    _refreshStatusCharacteristic();
    _pushNotification(NOTIF_WARN, "WG dump confirmed");
    DLOG_WARN(TAG, "WG dump confirmed");
}

void BLEManager::_cancelWireGuardConfirmation(const char* reason) {
    _wgConfirmPending = false;
    _wgArmed = false;
    _wgConfirmDeadlineMs = 0;
    _wgArmDeadlineMs = 0;
    _refreshStatusCharacteristic();
    _pushNotification(NOTIF_INFO, reason);
    DLOG_INFO(TAG, "%s", reason);
}

void BLEManager::_setReceipt(const char* code, bool notify) {
    if (!code) {
        code = "IDLE";
    }

    strlcpy(_receiptBuf, code, sizeof(_receiptBuf));
    if (_receiptChar) {
        _receiptChar->setValue(reinterpret_cast<const uint8_t*>(_receiptBuf),
                               strlen(_receiptBuf));
        if (notify && _serverConnected) {
            _receiptChar->notify();
        }
    }
}

void BLEManager::_setPrompt(const char* prompt, bool notify) {
    if (!prompt) {
        prompt = "";
    }

    strlcpy(_promptBuf, prompt, sizeof(_promptBuf));
    if (_promptChar) {
        _promptChar->setValue(reinterpret_cast<const uint8_t*>(_promptBuf),
                              strlen(_promptBuf));
        if (notify && _serverConnected) {
            _promptChar->notify();
        }
    }
}

void BLEManager::_refreshStatusCharacteristic(bool notify) {
    const char* inputState = "IDLE";
    if (_textInputPending && !_textInputReady) {
        inputState = "PENDING";
    } else if (_textInputPending && _textInputReady) {
        inputState = "READY";
    }

    const char* wgState = "IDLE";
    if (_wgConfirmPending && !_wgArmed) {
        wgState = "PENDING";
    } else if (_wgArmed) {
        wgState = "ARMED";
    } else if (_wgActive) {
        wgState = "UP";
    }

    snprintf(_statusBuf,
             sizeof(_statusBuf),
             "sess=%s;state=%u;gps=%u;input=%s;wg=%s;tok=%u",
             _sessionId[0] ? _sessionId : "NONE",
             static_cast<unsigned>(_state),
             hasFreshGpsFix() ? 1U : 0U,
             inputState,
             wgState,
             static_cast<unsigned>(_textInputToken));

    if (_statusChar) {
        _statusChar->setValue(reinterpret_cast<const uint8_t*>(_statusBuf),
                              strlen(_statusBuf));
        if (notify && _serverConnected) {
            _statusChar->notify();
        }
    }
}

bool BLEManager::_acceptTextPayload(const uint8_t* data, size_t len) {
    if (!_textInputPending) {
        DLOG_WARN(TAG, "text write rejected: idle");
        _setReceipt("IDLE");
        return false;
    }

    if (_textInputReady) {
        DLOG_WARN(TAG, "text write rejected: busy");
        _setReceipt("BUSY");
        return false;
    }

    if (!data || len == 0 || len >= sizeof(_resultBuf)) {
        DLOG_WARN(TAG, "text write rejected: len=%u", static_cast<unsigned>(len));
        _setReceipt("REJECTED");
        return false;
    }

    size_t cleanLen = len;
    while (cleanLen > 0 && (data[cleanLen - 1] == '\r' || data[cleanLen - 1] == '\n')) {
        cleanLen--;
    }

    if (cleanLen == 0 || cleanLen >= sizeof(_resultBuf)) {
        _setReceipt("REJECTED");
        return false;
    }

    for (size_t i = 0; i < cleanLen; ++i) {
        if (!isPrintableTextByte(data[i]) && data[i] < 0x80) {
            DLOG_WARN(TAG, "text write rejected: non-printable");
            _setReceipt("REJECTED");
            return false;
        }
    }

    memcpy(_resultBuf, data, cleanLen);
    _resultBuf[cleanLen] = '\0';
    _textInputReady = true;
    _textInputDeadlineMs = 0;

    _setReceipt("RECEIVED");
    _publishTextInputState();
    _refreshStatusCharacteristic();

    DLOG_INFO(TAG, "text input received token=%u len=%u",
              static_cast<unsigned>(_textInputToken),
              static_cast<unsigned>(cleanLen));
    _pushNotification(NOTIF_INFO, "BLE input received");
    return true;
}

void BLEManager::_clearTextInputState(bool clearPrompt, const char* receiptCode) {
    _textInputPending = false;
    _textInputReady = false;
    _textInputDeadlineMs = 0;

    if (clearPrompt) {
        memset(_promptBuf, 0, sizeof(_promptBuf));
        _setPrompt("", false);
    }

    memset(_resultBuf, 0, sizeof(_resultBuf));
    _setReceipt(receiptCode ? receiptCode : "IDLE");
    _publishTextInputState();
    _refreshStatusCharacteristic();

    if (!_serverConnected) {
        _ensureAdvertising(false);
    }
}

void BLEManager::_handleGpsPayload(const uint8_t* data, size_t len) {
    if (!data || len < sizeof(PhoneGpsFrameV1)) {
        DLOG_WARN(TAG, "gps frame too short");
        return;
    }

    PhoneGpsFrameV1 frame;
    memcpy(&frame, data, sizeof(frame));

    if (frame.version != 1) {
        DLOG_WARN(TAG, "gps frame version=%u", static_cast<unsigned>(frame.version));
        return;
    }

    if ((frame.flags & PHONE_GPS_FLAG_VALID) == 0) {
        _setGpsUnavailable(false);
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
    _formatIso8601(frame.epochUtc, _gpsTimeIso, sizeof(_gpsTimeIso));
    _publishGpsState();
    _refreshStatusCharacteristic();

    GPSFix fix;
    fix.lat = lat;
    fix.lon = lon;
    fix.accuracy = acc;
    fix.valid = true;
    fix.timestamp = _lastGpsFixMs;
    SESS.updateGPS(fix);
}

void BLEManager::_handleControlPayload(const uint8_t* data, size_t len) {
    if (!data || len < sizeof(PhoneControlFrameV1)) {
        DLOG_WARN(TAG, "control frame too short");
        return;
    }

    PhoneControlFrameV1 frame;
    memcpy(&frame, data, sizeof(frame));

    if (frame.version != 1) {
        DLOG_WARN(TAG, "control frame version=%u", static_cast<unsigned>(frame.version));
        return;
    }

    _wgActive = (frame.flags & PHONE_CTRL_FLAG_WG_ACTIVE) != 0;

    if ((frame.flags & PHONE_CTRL_FLAG_CANCEL) != 0) {
        _cancelWireGuardConfirmation("WG remote cancel");
        return;
    }

    if ((frame.flags & PHONE_CTRL_FLAG_BATCH_RX) != 0) {
        _enrichmentBatchAcked = true;
    }

    if ((frame.flags & PHONE_CTRL_FLAG_DUMP_REQ) != 0 && frame.counter != _lastWgCounter) {
        _lastWgCounter = frame.counter;
        _wgConfirmPending = true;
        _wgArmed = false;
        _wgConfirmDeadlineMs = millis() + WG_CONFIRM_TIMEOUT_MS;
        _wgArmDeadlineMs = 0;
        _refreshStatusCharacteristic();
        _pushNotification(NOTIF_WARN, "WG dump request: hold A then B");
        DLOG_WARN(TAG, "WG dump request counter=%u", static_cast<unsigned>(frame.counter));
        return;
    }

    _refreshStatusCharacteristic();
}

void BLEManager::_handleEnrichmentPayload(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return;
    }

    if (!_enrichmentInFlight || _enrichmentExpectedCount == 0) {
        DLOG_WARN(TAG, "enrichment payload ignored: not waiting");
        return;
    }

    const size_t expectedBytes = _enrichmentExpectedCount * ENRICHMENT_RECORD_SIZE;
    if (_enrichmentRxLen >= expectedBytes) {
        return;
    }

    size_t copyLen = len;
    if (_enrichmentRxLen + copyLen > expectedBytes) {
        copyLen = expectedBytes - _enrichmentRxLen;
        DLOG_WARN(TAG, "enrichment payload truncated");
    }

    if (_enrichmentRxLen + copyLen > sizeof(_enrichmentRxBuf)) {
        DLOG_WARN(TAG, "enrichment payload overflow");
        _enrichmentInFlight = false;
        _enrichmentExpectedCount = 0;
        _enrichmentRxLen = 0;
        return;
    }

    memcpy(_enrichmentRxBuf + _enrichmentRxLen, data, copyLen);
    _enrichmentRxLen += copyLen;

    if (_enrichmentRxLen < expectedBytes) {
        return;
    }

    const size_t count = _enrichmentExpectedCount;
    for (size_t i = 0; i < count; ++i) {
        EnrichmentRecordWire record;
        memcpy(&record,
               _enrichmentRxBuf + (i * ENRICHMENT_RECORD_SIZE),
               sizeof(record));

        PendingEnrichment& out = _enrichmentBatch[i];
        out.eventId = record.eventId;
        out.lat = static_cast<float>(record.latE7) / 10000000.0f;
        out.lon = static_cast<float>(record.lonE7) / 10000000.0f;
        out.alt = static_cast<float>(record.altCm) / 100.0f;
        out.accuracy = static_cast<float>(record.accuracyDm) / 10.0f;

        char tagBuf[sizeof(record.tag)];
        memcpy(tagBuf, record.tag, sizeof(tagBuf));
        tagBuf[sizeof(tagBuf) - 1] = '\0';
        strlcpy(out.tag, tagBuf, sizeof(out.tag));
    }

    _enrichmentAvailableCount = count;
    _enrichmentReady = true;
    _enrichmentInFlight = false;
    _enrichmentDeadlineMs = 0;

    DLOG_INFO(TAG, "enrichment batch received count=%u bytes=%u",
              static_cast<unsigned>(count),
              static_cast<unsigned>(_enrichmentRxLen));
}

bool BLEManager::_validateGpsFix(float lat, float lon, float alt, float accuracy,
                                 uint32_t epochUtc) const {
    if (!isfinite(lat) || !isfinite(lon) || !isfinite(alt) || !isfinite(accuracy)) {
        return false;
    }
    if (fabsf(lat) > GPS_MAX_ABS_LAT || fabsf(lon) > GPS_MAX_ABS_LON) {
        return false;
    }
    if (alt < GPS_MIN_ALT_M || alt > GPS_MAX_ALT_M) {
        return false;
    }
    if (accuracy < 0.0f || accuracy > GPS_MAX_ACCURACY_M) {
        return false;
    }
    if (epochUtc < 1609459200UL) {  // 2021-01-01 UTC
        return false;
    }
    return true;
}

void BLEManager::_setGpsUnavailable(bool clearCoordinates) {
    _gpsAvailable = false;
    if (clearCoordinates) {
        _gpsLat = 0.0f;
        _gpsLon = 0.0f;
        _gpsAlt = 0.0f;
        _gpsAccuracy = 0.0f;
        _gpsEpochAtFix = 0;
        _timeTrusted = false;
        memset(_gpsTimeIso, 0, sizeof(_gpsTimeIso));
    }
    _publishGpsState();
    _refreshStatusCharacteristic();
}

void BLEManager::_publishBleState() {
    const bool blePresent = _clientConnected || _serverConnected;

    STATE_WRITE_BEGIN();
    g_state.bleConnected = blePresent;
    STATE_WRITE_END();
}

void BLEManager::_publishGpsState() {
    STATE_WRITE_BEGIN();
    g_state.gpsAvailable = _gpsAvailable;
    g_state.gpsLat = _gpsLat;
    g_state.gpsLon = _gpsLon;
    g_state.gpsAlt = _gpsAlt;
    g_state.gpsAccuracy = _gpsAccuracy;
    g_state.gpsLastFix = _lastGpsFixMs;
    strlcpy(g_state.gpsTimeISO, _gpsTimeIso, sizeof(g_state.gpsTimeISO));
    STATE_WRITE_END();
}

void BLEManager::_publishTextInputState() {
    STATE_WRITE_BEGIN();
    g_state.textInputPending = _textInputPending;
    g_state.textInputReady = _textInputReady;
    strlcpy(g_state.textInputPrompt, _promptBuf, sizeof(g_state.textInputPrompt));
    STATE_WRITE_END();
}

void BLEManager::_pushNotification(uint8_t type, const char* text) {
    if (!text || !text[0]) {
        return;
    }

    if (!BUS.publishNotification(type, text)) {
        DLOG_WARN("BLE", "Notification queue full, dropped type=%u",
                  static_cast<unsigned>(type));
    }
}

bool BLEManager::_parseIso8601(const char* iso, uint32_t& epochUtc) const {
    if (!iso || strlen(iso) < 20) {
        return false;
    }

    int year = 0;
    int mon = 0;
    int day = 0;
    int hour = 0;
    int min = 0;
    int sec = 0;

    if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2dZ",
               &year, &mon, &day, &hour, &min, &sec) != 6) {
        return false;
    }

    const int32_t days = _daysFromCivil(year, mon, day);
    if (days < 0) {
        return false;
    }

    epochUtc = static_cast<uint32_t>(days) * 86400UL +
               static_cast<uint32_t>(hour) * 3600UL +
               static_cast<uint32_t>(min) * 60UL +
               static_cast<uint32_t>(sec);
    return true;
}

void BLEManager::_formatIso8601(uint32_t epochUtc, char* out, size_t len) const {
    if (!out || len == 0) {
        return;
    }

    const uint32_t days = epochUtc / 86400UL;
    const uint32_t rem = epochUtc % 86400UL;

    int32_t year = 1970;
    uint32_t month = 1;
    uint32_t day = 1;
    _civilFromDays(static_cast<int32_t>(days), year, month, day);

    const uint32_t hour = rem / 3600UL;
    const uint32_t minute = (rem % 3600UL) / 60UL;
    const uint32_t second = rem % 60UL;

    snprintf(out, len, "%04ld-%02lu-%02luT%02lu:%02lu:%02luZ",
             static_cast<long>(year),
             static_cast<unsigned long>(month),
             static_cast<unsigned long>(day),
             static_cast<unsigned long>(hour),
             static_cast<unsigned long>(minute),
             static_cast<unsigned long>(second));
}

int32_t BLEManager::_daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
    y -= (m <= 2);
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

void BLEManager::_civilFromDays(int32_t z, int32_t& y, uint32_t& m, uint32_t& d) {
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int32_t>(yoe) + era * 400;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : static_cast<uint32_t>(-9));
    y += (m <= 2);
}

void BLEManager::ScanCallbacks::onResult(const NimBLEAdvertisedDevice* advertisedDevice) {
    _owner._onAdvertisedDevice(advertisedDevice);
}

void BLEManager::ClientCallbacks::onConnect(NimBLEClient* pClient) {
    _owner._onClientConnected(pClient);
}

void BLEManager::ClientCallbacks::onDisconnect(NimBLEClient* pClient, int reason) {
    _owner._onClientDisconnected(pClient, reason);
}

bool BLEManager::ClientCallbacks::onConnParamsUpdateRequest(
    NimBLEClient* pClient,
    const ble_gap_upd_params* params
) {
    (void)pClient;
    return (params->itvl_min >= 6 && params->itvl_max <= 60);
}

void BLEManager::ClientCallbacks::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
    if (!connInfo.isEncrypted()) {
        DLOG_WARN(TAG, "BLE auth incomplete");
    }
}

void BLEManager::ServerCallbacks::onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
    (void)pServer;
    _owner._onServerConnected(&connInfo);
}

void BLEManager::ServerCallbacks::onDisconnect(NimBLEServer* pServer,
                                               NimBLEConnInfo& connInfo,
                                               int reason) {
    (void)pServer;
    _owner._onServerDisconnected(&connInfo, reason);
}

void BLEManager::TextInputCallbacks::onWrite(NimBLECharacteristic* pCharacteristic,
                                             NimBLEConnInfo& connInfo) {
    (void)connInfo;
    _owner._onTextInputWrite(pCharacteristic);
}


