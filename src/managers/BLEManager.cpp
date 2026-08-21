


#include "BLEManager.h"
#include "CommandDispatcher.h"
#include "PhoneOffloadManager.h"
#include "TimeService.h"

#include "protocol/CompanionProtocol.h"

#include <ctype.h>
#include <math.h>
#include <string>
#include <string.h>
#include <vector>
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_bt.h"
#include <esp_err.h>

extern void companionRequestCancel();
#include <esp_wifi.h>
#include <esp_heap_caps.h>

#include "../SecretsConfig.h"
#include "../core/DebugLog.h"
#include "../core/EventBus.h"
#include "../core/Session.h"
#include "../core/CrashBreadcrumb.h"
#include "RadioArbiter.h"

namespace {
constexpr const char* TAG = "BLE";

constexpr uint32_t SCAN_WINDOW_MS            = 8000UL;
constexpr uint32_t WEAK_SIGNAL_SCAN_WINDOW_MS = 15000UL;
constexpr uint32_t SCAN_GAP_MS               = 15000UL;
constexpr uint32_t SCAN_RESPONSE_TIMEOUT_MS  = 200UL;
constexpr uint32_t BLE_RADIO_SETTLE_MS       = 500UL;
constexpr uint32_t BLE_SCAN_STOP_SETTLE_MS   = 300UL;
constexpr uint32_t BLE_PRE_INIT_SETTLE_MS    = 75UL;
constexpr uint16_t CONNECT_SCAN_INTERVAL     = 16;  // 10 ms units used by NimBLE initiator
constexpr uint16_t CONNECT_SCAN_WINDOW       = 16;
constexpr uint32_t CONNECT_TIMEOUT_MS        = 8000UL;  // modest increase; was 6 s
// A peer has already been seen by the scanner before connect() starts. A
// 45-second blocking attempt outlived the automatic handoff window in field
// use and invited cross-task teardown. Fifteen seconds still tolerates a weak
// phone while bounding how long capture stays paused.
constexpr uint32_t CONNECT_TIMEOUT_PROBE_MS  = 15000UL;
constexpr uint16_t PHONE_CONN_SUPERVISION_TIMEOUT_10MS = 1500;
constexpr uint32_t CONNECT_WATCHDOG_MS       = 50000UL;
constexpr uint32_t GPS_POLL_MS               = 5000UL;
constexpr uint32_t PHONE_LINK_REQUEST_COOLDOWN_MS = 5000UL;
constexpr uint32_t CONTROL_POLL_MS           = 2000UL;
constexpr uint32_t GPS_STALE_MS              = 45000UL;
constexpr uint32_t GPS_TIME_HOLDOVER_MS      = 1800000UL;
constexpr uint32_t STATUS_REFRESH_MS         = 1000UL;
constexpr uint32_t WG_CONFIRM_TIMEOUT_MS     = 15000UL;
constexpr uint32_t WG_ARM_WINDOW_MS          = 8000UL;
constexpr uint32_t STACK_LOG_INTERVAL_MS     = 30000UL;
constexpr uint32_t ENRICHMENT_TIMEOUT_MS     = 25000UL;
constexpr uint32_t PHONE_PEER_CACHE_TTL_MS   = 300000UL;
constexpr int8_t   PHONE_WEAK_RSSI_DBM       = -85;

// Refresh on successful link transitions so the arbiter does not reclaim BLE
// mid-session.
constexpr uint32_t BLE_LINK_ACTIVE_LEASE_MS  = 60000UL;

constexpr float GPS_MAX_ABS_LAT              = 90.0f;
constexpr float GPS_MAX_ABS_LON              = 180.0f;
constexpr float GPS_MIN_ALT_M                = -500.0f;
constexpr float GPS_MAX_ALT_M                = 20000.0f;
constexpr float GPS_MAX_ACCURACY_M           = 1000.0f;

constexpr uint32_t RECONNECT_BACKOFF_MS[3]   = { 2000UL, 5000UL, 10000UL };

constexpr uint8_t NOTIF_INFO = 1;
constexpr uint8_t NOTIF_WARN = 2;

// Protects the small worker/disable handshake shared by TaskHardware (core 0)
// and BLEWorker (core 1). NimBLE calls themselves deliberately run outside the
// critical section.
portMUX_TYPE s_bleWorkerStateMux = portMUX_INITIALIZER_UNLOCKED;

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

const char* btControllerStatusName(esp_bt_controller_status_t status) {
    switch (status) {
        case ESP_BT_CONTROLLER_STATUS_IDLE:
            return "idle";
        case ESP_BT_CONTROLLER_STATUS_INITED:
            return "inited";
        case ESP_BT_CONTROLLER_STATUS_ENABLED:
            return "enabled";
        default:
            return "unknown";
    }
}

uint32_t currentTaskFreeStackBytes() {
    return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr) *
                                 sizeof(StackType_t));
}

uint8_t hexNibble(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<uint8_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<uint8_t>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<uint8_t>(c - 'A' + 10);
    }
    return 0xFF;
}

bool decodeHex(const char* hex, uint8_t* out, size_t outLen) {
    if (!hex || !out || strlen(hex) != outLen * 2U) {
        return false;
    }
    for (size_t i = 0; i < outLen; ++i) {
        const uint8_t hi = hexNibble(hex[i * 2U]);
        const uint8_t lo = hexNibble(hex[i * 2U + 1U]);
        if (hi > 0x0F || lo > 0x0F) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

void formatHexSnippet(const std::vector<uint8_t>& payload, char* out, size_t outLen) {
    if (!out || outLen == 0) {
        return;
    }

    out[0] = '\0';
    const size_t maxBytes = min(payload.size(), static_cast<size_t>(24));
    size_t used = 0;
    for (size_t i = 0; i < maxBytes && used + 3 < outLen; ++i) {
        const int written = snprintf(out + used, outLen - used,
                                     "%s%02X",
                                     (i == 0) ? "" : " ",
                                     static_cast<unsigned>(payload[i]));
        if (written <= 0) {
            break;
        }
        used += static_cast<size_t>(written);
    }

    if (payload.size() > maxBytes && used + 4 < outLen) {
        strlcat(out, "...", outLen);
    }
}

bool parseUuid128LittleEndian(const char* uuid, uint8_t out[16]) {
    if (!uuid || !out) {
        return false;
    }

    char hex[32];
    size_t count = 0;
    for (const char* p = uuid; *p; ++p) {
        if (*p == '-') {
            continue;
        }
        if (!isxdigit(static_cast<unsigned char>(*p)) || count >= sizeof(hex)) {
            return false;
        }
        hex[count++] = *p;
    }

    if (count != sizeof(hex)) {
        return false;
    }

    uint8_t bigEndian[16];
    for (size_t i = 0; i < sizeof(bigEndian); ++i) {
        const uint8_t hi = hexNibble(hex[i * 2]);
        const uint8_t lo = hexNibble(hex[i * 2 + 1]);
        if (hi > 0x0F || lo > 0x0F) {
            return false;
        }
        bigEndian[i] = static_cast<uint8_t>((hi << 4) | lo);
    }

    for (size_t i = 0; i < sizeof(bigEndian); ++i) {
        out[i] = bigEndian[sizeof(bigEndian) - 1 - i];
    }
    return true;
}

bool payloadAdvertisesUuid128(const std::vector<uint8_t>& payload, const char* uuid) {
    uint8_t target[16];
    if (!parseUuid128LittleEndian(uuid, target)) {
        return false;
    }

    size_t offset = 0;
    while (offset < payload.size()) {
        const uint8_t fieldLen = payload[offset];
        if (fieldLen == 0) {
            break;
        }

        const size_t fieldEnd = offset + 1U + fieldLen;
        if (fieldEnd > payload.size() || fieldLen < 1) {
            break;
        }

        const uint8_t type = payload[offset + 1U];
        const uint8_t* data = payload.data() + offset + 2U;
        const size_t dataLen = fieldLen - 1U;

        // Accept service-data UUIDs too; Android may advertise that shape.
        if (type == 0x06 || type == 0x07 || type == 0x21) {
            const size_t compareLen = (type == 0x21 && dataLen >= 16U) ? 16U : dataLen;
            for (size_t i = 0; i + 16U <= compareLen; i += 16U) {
                if (memcmp(data + i, target, 16U) == 0) {
                    return true;
                }
            }
        }

        offset = fieldEnd;
    }

    return false;
}

BLEManager* s_bleInstance = nullptr;

// Dedicated RTC marker for failures that reset USB CDC before the panic text
// can be captured.  Do not reuse the general breadcrumb ring here: boot
// maintenance can overwrite that ring before an operator reconnects serial.
RTC_NOINIT_ATTR volatile uint32_t s_bleRxDiagMagic;
RTC_NOINIT_ATTR volatile uint32_t s_bleRxDiagStage;
RTC_NOINIT_ATTR volatile uint32_t s_bleRxDiagLen;
RTC_NOINIT_ATTR volatile uint32_t s_bleRxDiagUptimeMs;
constexpr uint32_t BLE_RX_DIAG_MAGIC = 0xB1E0D1A6UL;

// A normal reboot starts ticking BLE again before an operator can reconnect
// USB, which would overwrite the last pre-panic marker above.  Snapshot that
// marker into a second RTC record only when boot observes a crash reset.  Host
// recovery resets and subsequent healthy boots intentionally leave it intact.
RTC_NOINIT_ATTR volatile uint32_t s_bleRxCrashMagic;
RTC_NOINIT_ATTR volatile uint32_t s_bleRxCrashStage;
RTC_NOINIT_ATTR volatile uint32_t s_bleRxCrashLen;
RTC_NOINIT_ATTR volatile uint32_t s_bleRxCrashUptimeMs;
RTC_NOINIT_ATTR volatile uint32_t s_bleWorkerDiagMagic;
RTC_NOINIT_ATTR volatile uint32_t s_bleWorkerDiagStage;
RTC_NOINIT_ATTR volatile uint32_t s_bleWorkerDiagDetail;
RTC_NOINIT_ATTR volatile uint32_t s_bleWorkerDiagUptimeMs;
RTC_NOINIT_ATTR volatile uint32_t s_bleWorkerCrashMagic;
RTC_NOINIT_ATTR volatile uint32_t s_bleWorkerCrashStage;
RTC_NOINIT_ATTR volatile uint32_t s_bleWorkerCrashDetail;
RTC_NOINIT_ATTR volatile uint32_t s_bleWorkerCrashUptimeMs;

inline void markBleRxDiag(uint32_t stage, size_t len = 0) {
    s_bleRxDiagMagic = BLE_RX_DIAG_MAGIC;
    s_bleRxDiagLen = static_cast<uint32_t>(len);
    s_bleRxDiagUptimeMs = millis();
    s_bleRxDiagStage = stage; // publish the stage last
}

inline void markBleWorkerDiag(uint32_t stage, uint32_t detail = 0) {
    s_bleWorkerDiagMagic = BLE_RX_DIAG_MAGIC;
    s_bleWorkerDiagDetail = detail;
    s_bleWorkerDiagUptimeMs = millis();
    s_bleWorkerDiagStage = stage;
}
}  // namespace

BLEManager BLE_MGR;

BLEManager::BLEManager()
    : _scanCallbacks(*this),
      _clientCallbacks(*this),
      _serverCallbacks(*this),
      _textInputCallbacks(*this) {
    memset(_sessionId, 0, sizeof(_sessionId));
    memset(_deviceName, 0, sizeof(_deviceName));
    memset(_targetDeviceName, 0, sizeof(_targetDeviceName));
    memset(_targetDeviceNameShort, 0, sizeof(_targetDeviceNameShort));
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
    strlcpy(_targetDeviceNameShort, SPECTRE_BLE_TARGET_DEVICE_NAME_SHORT, sizeof(_targetDeviceNameShort));
    strlcpy(_targetServiceUUID, PHONE_SERVICE_UUID, sizeof(_targetServiceUUID));
    strlcpy(_receiptBuf, "IDLE", sizeof(_receiptBuf));
}

bool BLEManager::begin() {
    if (!_ensurePayloadBuffers()) {
        return false;
    }

    if (_begun) {
        return _ensureWorkerTask();
    }

    // Create the RX mutex before any NimBLE callbacks can be registered.
    if (!_rxMutex) {
        _rxMutex = xSemaphoreCreateMutex();
        if (!_rxMutex) {
            DLOG_ERROR(TAG, "BLE RX mutex alloc failed");
            return false;
        }
    }

    auto failBegin = [this]() {
        _releaseWorkerTask("begin_fail");

        s_bleInstance = nullptr;
        _teardownSessionObjects();
        NimBLEDevice::deinit(false);
        _deinitBtController();
        _scan = nullptr;
        _client = nullptr;
        _remoteService = nullptr;
        _gpsRemoteChar = nullptr;
        _controlRemoteChar = nullptr;
        _metaRemoteChar = nullptr;
        _eventBatchRemoteChar = nullptr;
        _enrichmentRemoteChar = nullptr;
        _authRemoteChar = nullptr;
        _authWriteRemoteChar = nullptr;
        _server = nullptr;
        _textService = nullptr;
        _promptChar = nullptr;
        _inputChar = nullptr;
        _receiptChar = nullptr;
        _statusChar = nullptr;

        _resetState();
        _begun = false;
        _radioEnabled = false;
        _lastStackLogMs = 0;
        _workerMinFreeStackBytes = 0;

        if (_rxMutex) {
            vSemaphoreDelete(_rxMutex);
            _rxMutex = nullptr;
        }
    };

    s_bleInstance = this;
    _resetState();
    _buildDeviceName();

    DLOG_DEBUG(TAG, "begin phase=pre device=%s targetName=%s", _deviceName, _targetDeviceName);

    {
        wifi_mode_t mode = WIFI_MODE_NULL;
        const esp_err_t err = esp_wifi_get_mode(&mode);
        DLOG_DEBUG(TAG, "pre-nimble wifi_mode=%d err=%s",
                   static_cast<int>(mode),
                   esp_err_to_name(err));
    }

    if (BLE_PRE_INIT_SETTLE_MS > 0) {
        delay(BLE_PRE_INIT_SETTLE_MS);
    }

    const esp_bt_controller_status_t btStatus = esp_bt_controller_get_status();
    DLOG_INFO(TAG,
              "begin phase=nimble_pre bt=%s/%d heap=%u largest=%u stack=%u devLen=%u",
              btControllerStatusName(btStatus),
              static_cast<int>(btStatus),
              static_cast<unsigned>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
              static_cast<unsigned>(heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM)),
              static_cast<unsigned>(currentTaskFreeStackBytes()),
              static_cast<unsigned>(strlen(_deviceName)));

    DLOG_INFO(TAG, "begin phase=nimble_init");
    const bool nimbleOk = NimBLEDevice::init(std::string(_deviceName));
    if (!nimbleOk) {
        DLOG_ERROR(TAG,
                   "begin phase=nimble_init_fail bt=%s/%d heap=%u largest=%u stack=%u",
                   btControllerStatusName(esp_bt_controller_get_status()),
                   static_cast<int>(esp_bt_controller_get_status()),
                   static_cast<unsigned>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
                   static_cast<unsigned>(heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM)),
                   static_cast<unsigned>(currentTaskFreeStackBytes()));
        failBegin();
        return false;
    }
    DLOG_INFO(TAG,
              "begin phase=nimble_init_ok bt=%s/%d heap=%u largest=%u stack=%u",
              btControllerStatusName(esp_bt_controller_get_status()),
              static_cast<int>(esp_bt_controller_get_status()),
              static_cast<unsigned>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
              static_cast<unsigned>(heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM)),
              static_cast<unsigned>(currentTaskFreeStackBytes()));

    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setMTU(247);

    // BLE is transport only; app auth and AES-GCM own trust.
    NimBLEDevice::setSecurityAuth(false, false, false);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityInitKey(0);
    NimBLEDevice::setSecurityRespKey(0);
    DLOG_INFO(TAG, "BLE SM disabled; app-layer P-256/AES-GCM required");

    _setupServer();
    DLOG_INFO(TAG, "begin phase=server_ok");

    _setupScanner();
    DLOG_INFO(TAG, "begin phase=scanner_ok");

    if (!_ensureWorkerTask()) {
        failBegin();
        return false;
    }

    _publishBleState();
    _publishGpsState();
    _publishTextInputState();

    _nextActionMs = millis();
    _lastStackLogMs = _nextActionMs;
    _workerMinFreeStackBytes = 0;
    _radioEnabled = false;
    _lastBeginMs = millis();
    _begun = true;
    DLOG_INFO(TAG, "begin ready");
    return true;
}

void BLEManager::printRxCrashDiag() const {
    bool printed = false;
    if (s_bleRxCrashMagic == BLE_RX_DIAG_MAGIC) {
        Serial.printf("[BLE] rxdiag crash stage=%lu len=%lu uptimeMs=%lu\n",
                      static_cast<unsigned long>(s_bleRxCrashStage),
                      static_cast<unsigned long>(s_bleRxCrashLen),
                      static_cast<unsigned long>(s_bleRxCrashUptimeMs));
        printed = true;
    } else if (s_bleRxDiagMagic == BLE_RX_DIAG_MAGIC) {
        Serial.printf("[BLE] rxdiag stage=%lu len=%lu uptimeMs=%lu\n",
                      static_cast<unsigned long>(s_bleRxDiagStage),
                      static_cast<unsigned long>(s_bleRxDiagLen),
                      static_cast<unsigned long>(s_bleRxDiagUptimeMs));
        printed = true;
    }

    if (s_bleWorkerCrashMagic == BLE_RX_DIAG_MAGIC) {
        Serial.printf("[BLE] workerdiag crash stage=%lu detail=%lu uptimeMs=%lu\n",
                      static_cast<unsigned long>(s_bleWorkerCrashStage),
                      static_cast<unsigned long>(s_bleWorkerCrashDetail),
                      static_cast<unsigned long>(s_bleWorkerCrashUptimeMs));
        printed = true;
    } else if (s_bleWorkerDiagMagic == BLE_RX_DIAG_MAGIC) {
        Serial.printf("[BLE] workerdiag stage=%lu detail=%lu uptimeMs=%lu\n",
                      static_cast<unsigned long>(s_bleWorkerDiagStage),
                      static_cast<unsigned long>(s_bleWorkerDiagDetail),
                      static_cast<unsigned long>(s_bleWorkerDiagUptimeMs));
        printed = true;
    }

    if (!printed) {
        Serial.println("[BLE] rxdiag unavailable");
    }
}

void BLEManager::latchRxCrashDiag() {
    const esp_reset_reason_t reason = esp_reset_reason();
    if (reason != ESP_RST_PANIC &&
        reason != ESP_RST_TASK_WDT &&
        reason != ESP_RST_INT_WDT &&
        reason != ESP_RST_WDT) {
        return;
    }
    if (s_bleRxDiagMagic == BLE_RX_DIAG_MAGIC) {
        s_bleRxCrashStage = s_bleRxDiagStage;
        s_bleRxCrashLen = s_bleRxDiagLen;
        s_bleRxCrashUptimeMs = s_bleRxDiagUptimeMs;
        s_bleRxCrashMagic = BLE_RX_DIAG_MAGIC;
    }

    if (s_bleWorkerDiagMagic == BLE_RX_DIAG_MAGIC) {
        s_bleWorkerCrashStage = s_bleWorkerDiagStage;
        s_bleWorkerCrashDetail = s_bleWorkerDiagDetail;
        s_bleWorkerCrashUptimeMs = s_bleWorkerDiagUptimeMs;
        s_bleWorkerCrashMagic = BLE_RX_DIAG_MAGIC;
    }
}

void BLEManager::shutdown() {
    if (!_begun) {
        return;
    }

    const bool scanWasActive = _scan && _scan->isScanning();
    DLOG_INFO(TAG, "shutdown");
    setRadioEnabled(false);
    if (scanWasActive) {
        DLOG_INFO(TAG, "shutdown scan stop settle=%lums",
                  static_cast<unsigned long>(BLE_SCAN_STOP_SETTLE_MS));
        delay(BLE_SCAN_STOP_SETTLE_MS);
    }

    if (!_releaseWorkerTask("shutdown")) {
        // The worker is still inside a blocking NimBLE call (connect/auth).
        // Tearing down underneath it corrupts host state. Leave the stack up;
        // the caller retries once the worker goes idle.
        DLOG_WARN(TAG, "shutdown aborted: worker busy, stack left initialised");
        return;
    }

    // Destroy the object graph before the host goes away, then take the host
    // down without letting NimBLE run destructors afterwards (clearAll=false).
    _teardownSessionObjects();

    DLOG_INFO(TAG, "shutdown phase=nimble_deinit");
    NimBLEDevice::deinit(false);
    DLOG_INFO(TAG, "shutdown phase=nimble_deinit_ok");

    // deinit() leaves the controller initialised in this build, so its ~33 KB
    // would stay pinned and the next init() would fail on an already
    // initialised controller.
    _deinitBtController();

    _resetState();
    _begun = false;
    _radioEnabled = false;
    _scanActive = false;
    _clientConnected = false;
    _serverConnected = false;
    _advertisingActive = false;
    _connectResultPending = false;
    _connectResultOk = false;
    _dirtyDisconnectCount = 0;
    _state = BLE_IDLE;
    _lastStackLogMs = 0;
    _workerMinFreeStackBytes = 0;
    s_bleInstance = nullptr;

    _publishBleState();
    _publishTextInputState();
}

void BLEManager::_teardownSessionObjects() {
    // Order matters. NimBLEDevice::deinit(true) tears the host down FIRST and
    // then runs these destructors, so they free attribute storage and mbufs
    // against pools nimble_port_deinit() has already handed back - which is the
    // "free() target pointer is outside heap areas" assert captured in the
    // 2026-08-18 coredump. Doing the destruction here, while the host is still
    // up, keeps every free matched to a live pool.

    if (_client) {
        if (_client->isConnected()) {
            _client->disconnect();
            delay(BLE_SCAN_STOP_SETTLE_MS);
        }
        NimBLEDevice::deleteClient(_client);
        _client = nullptr;
    }
    _remoteService        = nullptr;
    _gpsRemoteChar        = nullptr;
    _controlRemoteChar    = nullptr;
    _metaRemoteChar       = nullptr;
    _eventBatchRemoteChar = nullptr;
    _enrichmentRemoteChar = nullptr;
    _authRemoteChar       = nullptr;
    _authWriteRemoteChar  = nullptr;
    _storageRemoteChar    = nullptr;

    if (_server && _textService) {
        // removeService() is two-phase: the first call hides the service and
        // marks it removed, the second actually deletes it. It also drops the
        // UUID from the advertising payload for us.
        _server->removeService(_textService, true);
        _server->removeService(_textService, true);
        _textService = nullptr;
    }
    _promptChar      = nullptr;
    _inputChar       = nullptr;
    _receiptChar     = nullptr;
    _statusChar      = nullptr;
    _linkRequestChar = nullptr;

    if (NimBLEAdvertising* adv = NimBLEDevice::getAdvertising()) {
        adv->removeServices();
        adv->clearData();
    }
    _advertisingActive = false;

    // _server and _scan stay alive: NimBLEDevice owns them and hands the same
    // instances back from createServer()/getScan() on the next begin(), which
    // then rebuilds the services. Removing the services set m_svcChanged, so
    // NimBLEServer::start() will reset and re-register the GATT database
    // against the freshly initialised host instead of early-returning.
}

void BLEManager::_deinitBtController() {
    const uint32_t before =
        heap_caps_get_free_size(SPECTRE_CAP_DRAM);

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        const esp_err_t err = esp_bt_controller_disable();
        if (err != ESP_OK) {
            DLOG_WARN(TAG, "controller disable failed: %s", esp_err_to_name(err));
            return;
        }
    }

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        const esp_err_t err = esp_bt_controller_deinit();
        if (err != ESP_OK) {
            DLOG_WARN(TAG, "controller deinit failed: %s", esp_err_to_name(err));
            return;
        }
    }

    const uint32_t after =
        heap_caps_get_free_size(SPECTRE_CAP_DRAM);
    DLOG_INFO(TAG, "controller released %luB (%luB -> %luB) status=%s",
              static_cast<unsigned long>(after > before ? after - before : 0),
              static_cast<unsigned long>(before),
              static_cast<unsigned long>(after),
              btControllerStatusName(esp_bt_controller_get_status()));
}

void BLEManager::releaseProbeResources() {
    if (!_begun) {
        return;
    }

    DLOG_INFO(TAG, "probe resource release");
    portENTER_CRITICAL(&s_bleWorkerStateMux);
    _probeResetPending = true;
    portEXIT_CRITICAL(&s_bleWorkerStateMux);
    setRadioEnabled(false);

    // BLEWorker uses a PSRAM-backed stack, so retaining it costs no scarce
    // internal heap. More importantly, deleting it here used to kill the task
    // while client->connect()/authentication was still blocking, corrupting
    // NimBLE state and producing the field panic loop.
    if (!readyForWifiHandoff()) {
        DLOG_INFO(TAG, "probe release waiting for BLE handoff settle");
    }
}

void BLEManager::setRadioEnabled(bool enabled) {
    if (!_begun) {
        return;
    }

    if (!enabled) {
        bool workerBusy = false;
        bool needsCleanup = false;
        portENTER_CRITICAL(&s_bleWorkerStateMux);
        needsCleanup = _radioEnabled || _radioDisablePending ||
                       _probeResetPending;
        if (!needsCleanup) {
            portEXIT_CRITICAL(&s_bleWorkerStateMux);
            return;
        }
        _radioEnabled = false;
        _wifiHandoffReadyAtMs = 0;
        workerBusy = _workerBusy;
        if (workerBusy) {
            _radioDisablePending = true;
        }
        portEXIT_CRITICAL(&s_bleWorkerStateMux);

        if (workerBusy) {
            DLOG_INFO(TAG, "radio disable queued: worker busy");
            return;
        }

        _finishRadioDisable();
        return;
    }

    portENTER_CRITICAL(&s_bleWorkerStateMux);
    const bool canEnable = !_workerBusy && !_radioDisablePending;
    if (canEnable) {
        _radioEnabled = true;
        _wifiHandoffReadyAtMs = 0;
    }
    portEXIT_CRITICAL(&s_bleWorkerStateMux);

    if (!canEnable) {
        DLOG_WARN(TAG, "radio enable deferred: BLE cleanup still active");
        return;
    }

    _nextActionMs = millis() + BLE_RADIO_SETTLE_MS;
    if (_textInputPending && !_serverConnected) {
        _ensureAdvertising(true);
    }
    DLOG_INFO(TAG, "radio enabled settle=%lums",
              static_cast<unsigned long>(BLE_RADIO_SETTLE_MS));
}

void BLEManager::_finishRadioDisable() {
    const bool scanWasActive = _scan && _scan->isScanning();
    if (scanWasActive) {
        _scan->stop();
    }
    _scanActive = false;
    _nextActionMs = millis() + SCAN_GAP_MS;

    if (_advertisingActive) {
        _ensureAdvertising(false);
    }

    if (_server && _serverConnHandle != BLE_HS_CONN_HANDLE_NONE) {
        _server->disconnect(_serverConnHandle);
        _serverConnHandle = BLE_HS_CONN_HANDLE_NONE;
        _serverConnected = false;
    }

    // GPS/probe path can keep the client object for faster reconnect.
    // Enrichment is heavier and should leave no stale NimBLE client behind.
    if (_enrichmentInFlight ||
        _enrichmentReady ||
        _enrichmentRequestPending ||
        _enrichmentFailed ||
        _dirtyDisconnectCount >= 2) {
        _hardDropClient("radio_disabled");
    } else {
        _softDisconnectClient("radio_disabled");
    }

    _clearBleRxQueues();
    _manualProbeActive = false;
    _probeStartMs = 0;
    _connectResultPending = false;
    _connectResultOk = false;
    _connectStartedMs = 0;
    _state = BLE_IDLE;

    bool resetProbeState = false;
    portENTER_CRITICAL(&s_bleWorkerStateMux);
    resetProbeState = _probeResetPending;
    _probeResetPending = false;
    _radioDisablePending = false;
    _wifiHandoffReadyAtMs = millis() +
        (scanWasActive ? BLE_SCAN_STOP_SETTLE_MS : BLE_PRE_INIT_SETTLE_MS);
    portEXIT_CRITICAL(&s_bleWorkerStateMux);

    if (resetProbeState) {
        _resetState();
        _radioEnabled = false;
        _lastStackLogMs = 0;
        _workerMinFreeStackBytes = 0;
    }

    _publishBleState();
    _publishGpsState();
    _publishTextInputState();
    DLOG_INFO(TAG,
              "radio disabled handoffSettle=%lums workerRetained=1",
              static_cast<unsigned long>(scanWasActive
                                             ? BLE_SCAN_STOP_SETTLE_MS
                                             : BLE_PRE_INIT_SETTLE_MS));
}

bool BLEManager::readyForWifiHandoff() const {
    if (!_begun) {
        return true;
    }

    bool enabled = false;
    bool busy = false;
    bool cleanupPending = false;
    uint32_t readyAtMs = 0;
    portENTER_CRITICAL(&s_bleWorkerStateMux);
    enabled = _radioEnabled;
    busy = _workerBusy;
    cleanupPending = _radioDisablePending;
    readyAtMs = _wifiHandoffReadyAtMs;
    portEXIT_CRITICAL(&s_bleWorkerStateMux);

    return !enabled && !busy && !cleanupPending &&
           (readyAtMs == 0 ||
            static_cast<int32_t>(millis() - readyAtMs) >= 0);
}

void BLEManager::tick() {
    if (!_begun) {
        return;
    }

    const uint32_t now = millis();
    if (_workerTask && (now - _lastStackLogMs) >= STACK_LOG_INTERVAL_MS) {
        const uint32_t freeBytes =
            static_cast<uint32_t>(uxTaskGetStackHighWaterMark(_workerTask));
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

    if (_phoneLinkRequestPending) {
        // The phone first connects as a central to Spectre's text service and
        // writes LINK1. This build deliberately permits one NimBLE connection,
        // so release that short-lived inbound control link before Spectre scans
        // back to the phone's authenticated companion peripheral. Keep the
        // request latched until the asynchronous server-disconnect callback has
        // confirmed the slot is free.
        if (_serverConnected &&
            _server &&
            _serverConnHandle != BLE_HS_CONN_HANDLE_NONE) {
            DLOG_INFO(TAG,
                      "phone-side link handoff: releasing inbound conn=%u",
                      static_cast<unsigned>(_serverConnHandle));
            _server->disconnect(_serverConnHandle);
            return;
        }
        _phoneLinkRequestPending = false;
        if (_state != BLE_SUBSCRIBED) {
            DLOG_INFO(TAG, "phone-side companion link request accepted");
            requestCompanionLink("phone_gatt_request", true);
        }
    }

    _handleConnectOutcome();

    // All BLE notify/write callback payloads are applied here,
    // not inside NimBLE callback context.
    _drainBleRxQueues();

    if (_state == BLE_SUBSCRIBED) {
        // Keep the radio lease alive while we have a working BLE link
        // so a manual bench session isn't cut mid-test by a probe-sized
        // lease expiring under us.  Cadence stays well inside the
        // BLE_LINK_ACTIVE_LEASE_MS window so the deadline never slips.
        constexpr uint32_t LEASE_RENEW_CADENCE_MS = 7000UL;
        if (_lastLeaseRenewMs == 0 ||
            (now - _lastLeaseRenewMs) >= LEASE_RENEW_CADENCE_MS) {
            _renewBleLeaseIfOwned("ble link active");
            _lastLeaseRenewMs = now;
        }

        // Do not fall back to periodic GATT reads when the phone omits notify
        // support. Field traces consistently panic inside the NimBLE host about
        // 100 ms after a completed GPS read (the third read, roughly ten seconds
        // into each secure session). Command responses, enrichment, and offload
        // use separate characteristics and remain fully available. A phone build
        // that exposes GPS/control notifications will resume those feeds through
        // the already-subscribed callback path without polling.

        if (_enrichmentRequestPending && !_enrichmentSendQueued) {
            _enrichmentSendQueued = true;
            _queueWorker(WORKER_JOB_SEND_ENRICH);
        }
    }

    if (_enrichmentInFlight && _enrichmentDeadlineMs != 0 &&
        static_cast<int32_t>(now - _enrichmentDeadlineMs) >= 0) {
        DLOG_WARN(TAG, "enrichment request timed out ack=%d rxBytes=%u expected=%u",
                  _enrichmentBatchAcked ? 1 : 0,
                  static_cast<unsigned>(_enrichmentRxLen),
                  static_cast<unsigned>(_enrichmentExpectedCount * ENRICHMENT_RECORD_SIZE));
        _failEnrichment("enrichment_timeout");
    }

    if (_gpsAvailable && (now - _lastGpsFixMs) > GPS_STALE_MS) {
        _setGpsUnavailable(false);
    }

    const bool weakRecentTarget =
        _lastTargetSeenMs != 0 &&
        (now - _lastTargetSeenMs) <= PHONE_PEER_CACHE_TTL_MS &&
        _lastTargetRssi <= PHONE_WEAK_RSSI_DBM;
    const uint32_t scanWindowMs =
        weakRecentTarget ? WEAK_SIGNAL_SCAN_WINDOW_MS : SCAN_WINDOW_MS;

    if (_state == BLE_SCANNING && _targetFound) {
        _stopScanWindow();
        DLOG_INFO(TAG, "scan stop; connect pending addr=%s", _connectedPeerAddr);
        _state                   = BLE_IDLE;
        _connectPendingAfterScan = true;
        _nextActionMs            = now + 250UL;
    }

    if (_state == BLE_SCANNING && (now - _scanStartedMs) >= scanWindowMs) {
        _stopScanWindow();
        _state = BLE_IDLE;

        if (_shouldYieldToUpload()) {
            const bool manualProbeWasActive = _manualProbeActive;
            DLOG_WARN(TAG, "yielding scan lease to pending upload");
            RADIO_ARB.release(RADIO_BLE_GPS, "yield_upload");
            _manualProbeActive = manualProbeWasActive;
            _scheduleProbeSoonButNotNow(now);
            return;
        }

        if (_manualProbeActive) {
            _nextActionMs = now + 250UL;   // fast bench retry
        } else {
            _nextActionMs = now + SCAN_GAP_MS;
        }

        DLOG_INFO(TAG, "scan window ended window=%lums weakRecent=%u lastRssi=%d",
                  static_cast<unsigned long>(scanWindowMs),
                  weakRecentTarget ? 1u : 0u,
                  static_cast<int>(_lastTargetRssi));
    }

    if (_state == BLE_CONNECTING && (now - _connectStartedMs) > CONNECT_WATCHDOG_MS) {
        DLOG_WARN(TAG, "connect watchdog expired");
        // Treat watchdog as a client->connect failure: don't retry the
        // same address, drop the cache and force a fresh scan.
        _clearTargetCache("connect_watchdog_expired");
        _scheduleReconnect("connect watchdog");
    }

    // Probe timeout: phone not seen within CONNECT_TIMEOUT_PROBE_MS. If the
    // timeout lands mid-scan, let the active scan stop normally first; tearing
    // NimBLE down immediately after an in-flight stop is fragile on the S3.
    const bool scanStopSettled =
        _scanStartedMs == 0 ||
        (now - _scanStartedMs) >= (scanWindowMs + BLE_SCAN_STOP_SETTLE_MS);
    if (_manualProbeActive && _radioEnabled && _probeStartMs != 0 &&
        !_haveTargetAddress &&
        _state == BLE_IDLE &&
        scanStopSettled &&
        (now - _probeStartMs) >= CONNECT_TIMEOUT_PROBE_MS) {
        DLOG_WARN(TAG, "probe timeout: phone not seen in %lums — releasing radio",
                  static_cast<unsigned long>(CONNECT_TIMEOUT_PROBE_MS));
        RADIO_ARB.release(RADIO_BLE_GPS, "probe_timeout");
        return;
    }

    if (_state == BLE_IDLE && now >= _nextActionMs) {
        if (_shouldYieldToUpload()) {
            const bool manualProbeWasActive = _manualProbeActive;
            DLOG_WARN(TAG, "yielding idle BLE lease to pending upload");
            RADIO_ARB.release(RADIO_BLE_GPS, "yield_upload");
            _manualProbeActive = manualProbeWasActive;
            _scheduleProbeSoonButNotNow(now);
            return;
        }

        if (_connectPendingAfterScan && _haveTargetAddress) {
            // 250 ms drain after scan stop has elapsed; safe to connect.
            _connectPendingAfterScan = false;
            DLOG_INFO(TAG, "connect pending settled addr=%s", _connectedPeerAddr);
            _startConnectAttempt();
        } else if (_directReconnectPending && _haveTargetAddress) {
            _startConnectAttempt();
        } else if (now >= _nextScanAllowedMs) {
            _startScanWindow();
        }
    }

    if ((now - _lastStatusRefreshMs) >= STATUS_REFRESH_MS) {
        _lastStatusRefreshMs = now;
        _refreshStatusCharacteristic();
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

bool BLEManager::requestCompanionLink(const char* reason, bool allowCachedReconnect) {
    if (!_begun || !_radioEnabled) {
        return false;
    }

    if (RADIO_ARB.currentOwner() == RADIO_WIFI_UPLOAD) {
        DLOG_WARN(TAG, "companion link rejected: upload active");
        return false;
    }

    if (_state == BLE_SUBSCRIBED) {
        return true;
    }

    const char* requestReason = reason ? reason : "companion_link_requested";
    const uint32_t now = millis();
    const bool cacheFresh =
        _targetCachedAtMs != 0 &&
        (now - _targetCachedAtMs) <= PHONE_PEER_CACHE_TTL_MS;
    if (_targetCachedFromService && !cacheFresh) {
        _clearTargetCache("cached_peer_expired");
    }

    const bool useCachedReconnect =
        allowCachedReconnect && _haveTargetAddress && _targetCachedFromService && cacheFresh;

    // Fresh-scan callers drop any cached peer so stale Android private
    // addresses cannot short-circuit discovery. Enrichment may try a
    // service-confirmed cached peer first, then falls back to scanning if
    // connect fails.
    if (!useCachedReconnect) {
        _clearTargetCache(requestReason);
    }

    // If a stale connect attempt is already in flight, abort it cleanly
    // so the new link request starts from a known idle state.
    if (_state == BLE_CONNECTING) {
        _hardDropClient(requestReason);
        _state = BLE_IDLE;
    }
    _stopScanWindow();

    _manualProbeActive   = true;
    _probeConnectPending = true;
    _probeStartMs        = 0;   // latched when first scan window opens, after settle
    _nextScanAllowedMs   = 0;
    _nextActionMs        = now + BLE_RADIO_SETTLE_MS;
    _scanDiagUntilMs     = now + (useCachedReconnect ? 12000UL : 45000UL);
    _scanDiagSeen        = 0;

    // A manual link window is bidirectional. While Spectre scans for the
    // phone's companion peripheral, also advertise Spectre's text service so
    // the phone can initiate its Text Link from the other direction.
    _ensureAdvertising(true);

    if (useCachedReconnect) {
        _directReconnectPending = true;
        _connectPendingAfterScan = false;
    }

    DLOG_INFO(TAG,
              "companion link requested reason=%s mode=%s timeout=15s settle=%lums cacheAge=%lums rssi=%d",
              requestReason,
              useCachedReconnect ? "cached" : "fresh_scan",
              static_cast<unsigned long>(BLE_RADIO_SETTLE_MS),
              static_cast<unsigned long>(_targetCachedAtMs ? now - _targetCachedAtMs : 0),
              static_cast<int>(_lastTargetRssi));
    return true;
}

bool BLEManager::requestManualProbe() {
    const bool ok = requestCompanionLink("btcon_manual", false);
    if (ok) {
        DLOG_INFO(TAG,
                  "btcon probe requested timeout=15s settle=%lums (cache cleared, fresh scan)",
                  static_cast<unsigned long>(BLE_RADIO_SETTLE_MS));
    }
    return ok;
}

bool BLEManager::requestEnrichmentBatch(const EventBatchRecord* records, size_t count) {
    if (!records || count == 0) {
        return false;
    }

    if (RADIO_ARB.currentOwner() == RADIO_WIFI_UPLOAD) {
        DLOG_WARN(TAG, "enrichment request rejected: upload active");
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

    // Flush any stale chunks from a prior failed request before starting fresh.
    _clearBleRxQueues();

    const size_t payloadLen = count * EVENT_BATCH_RECORD_SIZE;
    memcpy(_payload->eventBatchTx, records, payloadLen);
    _eventBatchTxLen = payloadLen;
    _enrichmentExpectedCount = count;
    _enrichmentRequestPending = true;
    _enrichmentInFlight = false;
    _enrichmentReady = false;
    _enrichmentFailed = false;
    _enrichmentSendQueued = false;
    _enrichmentBatchAcked = false;
    _enrichmentSendMs = 0;
    _enrichmentXferMs = 0;
    _enrichmentWaitStartMs = 0;
    _enrichmentRxLen = 0;
    _enrichmentAvailableCount = 0;
    _enrichmentDeadlineMs = millis() + ENRICHMENT_TIMEOUT_MS;

    DLOG_INFO(TAG, "enrichment request queued count=%u bytes=%u",
              static_cast<unsigned>(count),
              static_cast<unsigned>(payloadLen));
    return true;
}

bool BLEManager::publishStorageSnapshot(const PhoneStorageFrameV1& frame) {
    if (RADIO_ARB.currentOwner() == RADIO_WIFI_UPLOAD) {
        DLOG_WARN(TAG, "storage snapshot skipped: upload active");
        return false;
    }

    if (!_begun || !_radioEnabled || _state != BLE_SUBSCRIBED) {
        return false;
    }

    if (!_storageRemoteChar) {
        return false;
    }

    if (!_storageRemoteChar->canWrite() && !_storageRemoteChar->canWriteNoResponse()) {
        DLOG_WARN(TAG, "storage snapshot skipped: storage char not writable");
        return false;
    }

    size_t secureTxLen = 0;
    if (!_secureSession.encrypt(PHONE_SECURE_CHANNEL_STORAGE,
                                reinterpret_cast<const uint8_t*>(&frame),
                                sizeof(frame),
                                _payload->storageSecureTx,
                                sizeof(_payload->storageSecureTx),
                                secureTxLen)) {
        DLOG_WARN(TAG, "storage snapshot encrypt failed: %s",
                  _secureSession.lastError() ? _secureSession.lastError() : "-");
        return false;
    }

    const bool ok = _storageRemoteChar->writeValue(_payload->storageSecureTx, secureTxLen, true);
    if (!ok) {
        DLOG_WARN(TAG, "storage snapshot write failed sealed=%u",
                  static_cast<unsigned>(secureTxLen));
        return false;
    }

    DLOG_INFO(TAG,
              "storage snapshot sent pendingUp=%lu/%lu pendingEnrich=%lu/%lu usedPct=%u",
              static_cast<unsigned long>(frame.pendingUploadMission),
              static_cast<unsigned long>(frame.pendingUploadNoise),
              static_cast<unsigned long>(frame.pendingEnrichMission),
              static_cast<unsigned long>(frame.pendingEnrichNoise),
              static_cast<unsigned>(frame.usedPct));
    return true;
}

bool BLEManager::publishLogStreamChunk(const uint8_t* plain, size_t plainLen) {
    if (!plain || plainLen == 0 || plainLen > LOG_STREAM_CHUNK_FRAME_MAX) {
        return false;
    }
    if (!_begun || !_radioEnabled || _state != BLE_SUBSCRIBED) {
        return false;
    }
    if (!_logStreamRemoteChar) {
        return false;
    }
    if (!_logStreamRemoteChar->canWrite() &&
        !_logStreamRemoteChar->canWriteNoResponse()) {
        return false;
    }

    size_t secureLen = 0;
    if (!_secureSession.encrypt(PHONE_SECURE_CHANNEL_LOG_STREAM,
                                plain, plainLen,
                                _payload->logStreamSecure, sizeof(_payload->logStreamSecure),
                                secureLen)) {
        return false;
    }

    // Prefer write-without-response: chunks are lossy by design and the
    // round-trip ack would cap our chunk rate well below what we need.
    const bool needsResponse = !_logStreamRemoteChar->canWriteNoResponse();
    return _logStreamRemoteChar->writeValue(_payload->logStreamSecure, secureLen,
                                            needsResponse);
}

bool BLEManager::publishDashboardStreamChunk(const uint8_t* plain, size_t plainLen) {
    if (!plain || plainLen == 0 || plainLen > DASHBOARD_STREAM_CHUNK_FRAME_MAX) {
        return false;
    }
    if (!_begun || !_radioEnabled || _state != BLE_SUBSCRIBED) {
        return false;
    }
    if (!_dashboardStreamRemoteChar) {
        return false;
    }
    if (!_dashboardStreamRemoteChar->canWrite() &&
        !_dashboardStreamRemoteChar->canWriteNoResponse()) {
        return false;
    }

    size_t secureLen = 0;
    if (!_secureSession.encrypt(PHONE_SECURE_CHANNEL_DASHBOARD_STREAM,
                                plain, plainLen,
                                _payload->dashboardStreamSecure,
                                sizeof(_payload->dashboardStreamSecure),
                                secureLen)) {
        return false;
    }

    const bool needsResponse = !_dashboardStreamRemoteChar->canWriteNoResponse();
    return _dashboardStreamRemoteChar->writeValue(
        _payload->dashboardStreamSecure, secureLen, needsResponse);
}

bool BLEManager::publishNotification(const uint8_t* plain, size_t plainLen) {
    if (!plain || plainLen == 0 || plainLen > PHONE_NOTIFICATION_FRAME_MAX) {
        return false;
    }
    if (!_begun || !_radioEnabled || _state != BLE_SUBSCRIBED) {
        return false;
    }
    if (!_notificationRemoteChar) {
        return false;
    }
    if (!_notificationRemoteChar->canWrite() &&
        !_notificationRemoteChar->canWriteNoResponse()) {
        return false;
    }

    size_t secureLen = 0;
    if (!_secureSession.encrypt(PHONE_SECURE_CHANNEL_NOTIFICATION,
                                plain, plainLen,
                                _payload->notificationSecure,
                                sizeof(_payload->notificationSecure),
                                secureLen)) {
        return false;
    }

    const bool needsResponse = !_notificationRemoteChar->canWriteNoResponse();
    return _notificationRemoteChar->writeValue(
        _payload->notificationSecure, secureLen, needsResponse);
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
        out[i] = _payload->enrichmentBatch[i];
    }

    outCount = count;
    _enrichmentReady = false;
    _enrichmentAvailableCount = 0;
    _enrichmentExpectedCount = 0;
    _enrichmentRxLen = 0;
    _enrichmentSendMs = 0;
    _enrichmentWaitStartMs = 0;
    _eventBatchTxLen = 0;
    _enrichmentDeadlineMs = 0;
    memset(_payload->enrichmentRx, 0, sizeof(_payload->enrichmentRx));
    memset(_payload->enrichmentBatch, 0, sizeof(_payload->enrichmentBatch));

    DLOG_INFO(TAG, "enrichment batch consumed count=%u",
              static_cast<unsigned>(count));
    return true;
}

bool BLEManager::consumeEnrichmentFailure() {
    if (!_enrichmentFailed) {
        return false;
    }

    // A timeout/failure means the phone may still emit a late ACK or response.
    // Tear down the client before releasing the BLE lease so those callbacks
    // cannot land in the next radio owner or the next enrichment exchange.
    _resetEnrichmentExchangeState(false);
    _hardDropClient("enrichment_failure");
    return true;
}

bool BLEManager::isPhoneCompanionReady() const {
    return isPhoneEnrichmentReady();
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
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC,
        sizeof(_promptBuf)
    );
    _inputChar = _textService->createCharacteristic(
        TEXT_INPUT_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC,
        sizeof(_resultBuf)
    );
    _receiptChar = _textService->createCharacteristic(
        TEXT_RECEIPT_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC,
        sizeof(_receiptBuf)
    );
    _statusChar = _textService->createCharacteristic(
        TEXT_STATUS_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC,
        sizeof(_statusBuf)
    );
    _linkRequestChar = _textService->createCharacteristic(
        TEXT_LINK_REQUEST_CHAR_UUID,
        // This is deliberately only a discovery trigger. It grants no trust
        // and carries no data; the outbound signed P-256 handshake remains
        // mandatory before control, logs, enrichment, or offload are usable.
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR,
        8
    );

    _inputChar->setCallbacks(&_textInputCallbacks);
    _linkRequestChar->setCallbacks(&_textInputCallbacks);

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv) {
        // Primary advertisement carries flags + the 128-bit text service UUID so
        // the companion app's UUID-filtered scan ("search for Spectre") always
        // matches. A 128-bit UUID (18B) + flags (3B) already fills most of the
        // 31-byte primary budget, so the device name goes in the scan response
        // instead — otherwise NimBLE drops one of them on overflow and the app
        // sees a nameless or undiscoverable device even while advertising.
        adv->addServiceUUID(TEXT_SERVICE_UUID);
        NimBLEAdvertisementData scanResponse;
        scanResponse.setName(std::string(_deviceName));
        adv->setScanResponseData(scanResponse);
        adv->enableScanResponse(true);
    }

    _setPrompt("");
    _setReceipt("IDLE");
    _refreshStatusCharacteristic(false);
}

void BLEManager::_setupScanner() {
    _scan = NimBLEDevice::getScan();
    _scan->setScanCallbacks(&_scanCallbacks, false);

    // The companion UUID is in the primary advertisement, so scan passively.
    // This avoids depending on a peer scan response before service matching.
    _scan->setActiveScan(false);
    _scan->setScanResponseTimeout(SCAN_RESPONSE_TIMEOUT_MS);

    // 100% duty cycle while a scan window is open — interval == window.
    // 30 ms is the NimBLE-recommended floor for Android-friendly
    // discovery without provoking the controller's "scan too aggressive"
    // backoff.  Keep them equal so a slot can never collide with a
    // listen gap.
    _scan->setInterval(30);
    _scan->setWindow(30);

    DLOG_INFO(TAG,
              "scanner configured legacy=1 active=0 interval=%lu window=%lu srTimeout=%lu",
              30UL,
              30UL,
              static_cast<unsigned long>(SCAN_RESPONSE_TIMEOUT_MS));
}

void BLEManager::_resetState() {
    _state = BLE_IDLE;
    _scanActive = false;
    _targetFound = false;
    _directReconnectPending = false;
    _reconnectAttempt = 0;
    _clientConnected = false;
    _serverConnected = false;
    _serverConnHandle = BLE_HS_CONN_HANDLE_NONE;
    _ignoreDisconnectOnce = false;
    _gpsNotifyEnabled = false;
    _controlNotifyEnabled = false;
    _authNotifyEnabled = false;
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
    _enrichmentFailed = false;
    _enrichmentNotifyEnabled = false;
    _enrichmentSendQueued = false;
    _enrichmentBatchAcked = false;
    _enrichmentSendMs = 0;
    _enrichmentXferMs = 0;
    _enrichmentWaitStartMs = 0;
    _textInputPending = false;
    _textInputReady = false;
    _phoneLinkRequestPending = false;
    _lastPhoneLinkRequestMs = 0;
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
    memset(_payload->eventBatchTx, 0, sizeof(_payload->eventBatchTx));
    memset(_payload->enrichmentRx, 0, sizeof(_payload->enrichmentRx));
    memset(_payload->enrichmentBatch, 0, sizeof(_payload->enrichmentBatch));
    _eventBatchTxLen = 0;
    _enrichmentRxLen = 0;
    _enrichmentExpectedCount = 0;
    _enrichmentAvailableCount = 0;
    _clearBleRxQueues();
    _haveTargetAddress            = false;
    _targetAddress                = NimBLEAddress();
    _targetCachedFromService      = false;
    _targetCachedAtMs             = 0;
    _lastTargetSeenMs             = 0;
    _lastTargetRssi               = 0;
    _pendingConnectIsServiceMatch = false;
    _connectPendingAfterScan      = false;
    _lastLeaseRenewMs = 0;
    _lastDisconnectReason = 0;
    _lastAuthFailReason = BleAuthFailReason::NONE;
    _secureRxFailures = 0;
    _authRxPending = false;
    _authRxLen = 0;
    _authRxDrops = 0;
    memset(_payload->authRx, 0, sizeof(_payload->authRx));
    _secureSession.reset();
}

void BLEManager::_startScanWindow() {
    if (!_scan || _scanActive) {
        return;
    }

    if (RADIO_ARB.currentOwner() == RADIO_WIFI_UPLOAD) {
        DLOG_WARN(TAG, "scan skipped: upload active");
        _state = BLE_IDLE;
        _nextActionMs = millis() + SCAN_GAP_MS;
        return;
    }

    const uint32_t now = millis();
    const bool diagActive =
        (_scanDiagUntilMs != 0 &&
         static_cast<int32_t>(now - _scanDiagUntilMs) < 0);
    const bool weakRecentTarget =
        _lastTargetSeenMs != 0 &&
        (now - _lastTargetSeenMs) <= PHONE_PEER_CACHE_TTL_MS &&
        _lastTargetRssi <= PHONE_WEAK_RSSI_DBM;
    const uint32_t scanWindowMs =
        weakRecentTarget ? WEAK_SIGNAL_SCAN_WINDOW_MS : SCAN_WINDOW_MS;

    _targetFound = false;
    _scanStartedMs = now;
    if (_manualProbeActive && _probeStartMs == 0) {
        _probeStartMs = now;   // start probe window clock on first scan
    }
    _state = BLE_SCANNING;
    _scan->clearResults();
    _scan->setScanCallbacks(&_scanCallbacks, diagActive);
    // Keep NimBLE's controller-side duration infinite and let the application
    // timer below own the stop.  Giving both layers the same finite deadline
    // creates an intermittent race between the natural GAP completion event
    // and ble_gap_disc_cancel(), observed as an ESP_RST_PANIC exactly at an
    // 8-second scan boundary during repeated field probes.
    _scanActive = _scan->start(0, false, false);
    _lastScanStartMs = millis();

    if (_scanActive) {
        DLOG_INFO(TAG, "scan start");
        DLOG_INFO(TAG,
                  "scan start diag=%d deliverDuplicates=%d diagLeft=%ld state=%u window=%lums weakRecent=%u lastRssi=%d",
                  diagActive ? 1 : 0,
                  diagActive ? 1 : 0,
                  (long)(_scanDiagUntilMs > millis() ? _scanDiagUntilMs - millis() : 0),
                  static_cast<unsigned>(_state),
                  static_cast<unsigned long>(scanWindowMs),
                  weakRecentTarget ? 1u : 0u,
                  static_cast<int>(_lastTargetRssi));
    } else {
        DLOG_WARN(TAG, "scan start failed");
        _scan->clearResults();
        _state = BLE_IDLE;
        _nextActionMs = millis() + SCAN_GAP_MS;
    }
}

void BLEManager::_stopScanWindow() {
    if (_scan && _scan->isScanning()) {
        _scan->stop();
    }
    if (_scan) {
        _scan->clearResults();
        _scan->setScanCallbacks(&_scanCallbacks, false);
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
    _renewBleLeaseIfOwned("ble connect attempt");
    DLOG_INFO(TAG, "connect attempt addr=%s name=%s",
              _connectedPeerAddr[0] ? _connectedPeerAddr : "unknown",
              _connectedDeviceName[0] ? _connectedDeviceName : "unknown");
    _queueWorker(WORKER_JOB_CONNECT);
}

void BLEManager::_scheduleReconnect(const char* reason) {
    _setGpsUnavailable(false);
    _publishBleState();
    _refreshStatusCharacteristic();

    _stopScanWindow();

    // Only service-UUID cached peers are eligible for short-lived
    // reconnect.  Name fallback or already-cleared caches always go
    // back to a fresh scan.  After any direct reconnect we still want
    // to clear the cache on the next failure (handled in _doConnectJob
    // and _handleConnectOutcome paths).
    if (_reconnectAttempt < 3 &&
        _haveTargetAddress &&
        _targetCachedFromService) {
        const uint32_t delayMs = RECONNECT_BACKOFF_MS[_reconnectAttempt];
        _reconnectAttempt++;
        _directReconnectPending = true;
        _state = BLE_IDLE;
        _nextActionMs = millis() + delayMs;
        DLOG_WARN(TAG, "%s -> reconnect %u in %lu ms (service-cached)",
                  reason,
                  static_cast<unsigned>(_reconnectAttempt),
                  static_cast<unsigned long>(delayMs));
        return;
    }

    // Either we never had a service-cached peer, the cache was already
    // cleared by a connect failure, or we've exhausted our reconnect
    // budget — drop everything and let a fresh scan find the peer again.
    _clearTargetCache(reason ? reason : "schedule_reconnect_fallback");
    _state = BLE_IDLE;
    _nextActionMs = millis() + (_manualProbeActive ? 250UL : SCAN_GAP_MS);
    DLOG_WARN(TAG, "%s -> return to scanning", reason);
}

void BLEManager::_handleConnectOutcome() {
    if (!_connectResultPending) {
        return;
    }

    _connectResultPending = false;

    if (_connectResultOk) {
        _reconnectAttempt = 0;
        _dirtyDisconnectCount = 0;
        _directReconnectPending = false;

        // Name-fallback matches are diagnostic only — the address must
        // not survive into a later reconnect cycle, even on a successful
        // bind.  Service-UUID cached peers stay until disconnect/failure.
        if (!_targetCachedFromService) {
            _clearTargetCache("name_fallback_post_connect");
        }

        // Bench testing: keep the lease alive so a successful link isn't
        // killed by an expiring probe lease while we're still poking at
        // the GATT table.  refreshLease no-ops if BLE doesn't currently
        // own the radio (e.g. server-only text input flow).
        _renewBleLeaseIfOwned("ble link ready");

        DLOG_INFO(TAG, "link ready state=%u", static_cast<unsigned>(_state));
        _publishBleState();
        _refreshStatusCharacteristic();
        return;
    }

    // _doConnectJob already cleared the cache on a hard connect/bind
    // failure; this call still runs _scheduleReconnect to drive the
    // state machine back into BLE_IDLE → fresh scan.
    _scheduleReconnect("connect failed");
}

void BLEManager::_checkTimeouts() {
    const uint32_t now = millis();

    // Signed-delta deadline compares so timeouts still fire correctly across
    // a millis() wraparound (every ~49.7 days).
    if (_textInputPending && _textInputDeadlineMs != 0 &&
        static_cast<int32_t>(now - _textInputDeadlineMs) >= 0) {
        DLOG_WARN(TAG, "text input timed out");
        _clearTextInputState(true, "TIMEOUT");
    }

    if (_wgConfirmPending &&
        static_cast<int32_t>(now - _wgConfirmDeadlineMs) >= 0) {
        _cancelWireGuardConfirmation("WG confirm timeout");
    }

    if (_wgArmed &&
        static_cast<int32_t>(now - _wgArmDeadlineMs) >= 0) {
        _cancelWireGuardConfirmation("WG arm timeout");
    }
}

void BLEManager::_queueWorker(uint32_t bits) {
    if (_workerTask) {
        xTaskNotify(_workerTask, bits, eSetBits);
    }
}

bool BLEManager::_ensurePayloadBuffers() {
    if (_payload) {
        return true;
    }

    _payload = static_cast<PayloadBuffers*>(
        heap_caps_calloc(1, sizeof(PayloadBuffers), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_payload) {
        DLOG_ERROR(TAG, "BLE payload PSRAM alloc failed bytes=%u free=%u largest=%u",
                   static_cast<unsigned>(sizeof(PayloadBuffers)),
                   static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                   static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        return false;
    }

    DLOG_INFO(TAG, "BLE payload buffers in PSRAM bytes=%u",
              static_cast<unsigned>(sizeof(PayloadBuffers)));
    return true;
}

bool BLEManager::_ensureWorkerTask() {
    if (_workerTask) {
        return true;
    }

    // The stack MUST be internal. It was MALLOC_CAP_SPIRAM, which IDF permits
    // (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y) but which is unsafe for any
    // task that reaches a flash write: spi_flash disables the cache for the
    // duration, PSRAM goes with it, and the next register-window spill writes
    // to a stack that is no longer addressable. The fault lands inside the
    // exception handler, so it is a DoubleException — unrecoverable, and it
    // takes the device down with no usable backtrace on the app CDC.
    //
    // That is the 2026-08-20 overnight panic, confirmed from the coredump:
    //   exccause 0x42 (DoubleException)  excvaddr 0xffffffe0
    //   epc6 -> _WindowOverflow8         epc1 -> esp_psram_check_ptr_addr
    //   a1   -> spiflash_start_core      (the cache-disable path)
    // All five panics recorded phase=backlog_probe, which runs here. This is
    // the only task in the firmware with a non-internal stack.
    //
    // Costs WORKER_STACK_BYTES of DRAM while BLE is up; the teardown path
    // reclaims it along with the NimBLE object graph.
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        _workerTaskEntry,
        "BLEWorker",
        WORKER_STACK_BYTES,
        this,
        2,
        &_workerTask,
        1,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );
    if (created != pdPASS || !_workerTask) {
        _workerTask = nullptr;
        // Report the pool the stack actually comes from, not PSRAM.
        DLOG_ERROR(TAG,
                   "worker task create failed rc=%ld need=%u internalFree=%u largest=%u",
                   static_cast<long>(created),
                   static_cast<unsigned>(WORKER_STACK_BYTES),
                   static_cast<unsigned>(
                       heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                   static_cast<unsigned>(
                       heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        return false;
    }

    _lastStackLogMs = millis();
    _workerMinFreeStackBytes = 0;
    DLOG_INFO(TAG, "begin phase=worker_ok");
    return true;
}

bool BLEManager::_releaseWorkerTask(const char* phase) {
    if (_workerTask) {
        portENTER_CRITICAL(&s_bleWorkerStateMux);
        const bool workerBusy = _workerBusy;
        portEXIT_CRITICAL(&s_bleWorkerStateMux);
        if (workerBusy) {
            DLOG_WARN(TAG, "%s phase=worker_delete_deferred busy=1",
                      phase ? phase : "ble");
            return false;
        }
        DLOG_INFO(TAG, "%s phase=worker_delete", phase ? phase : "ble");
        TaskHandle_t task = _workerTask;
        _workerTask = nullptr;
        vTaskDeleteWithCaps(task);
    }
    return true;
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

void BLEManager::_softDisconnectClient(const char* reason) {
    if (_client && _client->isConnected()) {
        _ignoreDisconnectOnce = true;
        _client->disconnect();
    }

    _clientConnected = false;
    _clearRemoteHandles();

    DLOG_INFO(TAG, "client soft drop reason=%s",
              reason ? reason : "-");
}

void BLEManager::_hardDropClient(const char* reason) {
    if (_client) {
        if (_client->isConnected()) {
            _ignoreDisconnectOnce = true;
            _client->disconnect();
        }

        NimBLEDevice::deleteClient(_client);
        _client = nullptr;
    }

    _clientConnected = false;
    _clearRemoteHandles();
    _clearBleRxQueues();
    _dirtyDisconnectCount = 0;

    DLOG_INFO(TAG, "client hard drop reason=%s",
              reason ? reason : "-");
}

void BLEManager::_workerTaskEntry(void* arg) {
    static_cast<BLEManager*>(arg)->_workerLoop();
}

void BLEManager::_workerLoop() {
    while (true) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, 0xFFFFFFFFUL, &bits, portMAX_DELAY);
        markBleWorkerDiag(160, bits);

        portENTER_CRITICAL(&s_bleWorkerStateMux);
        const bool runJobs = _radioEnabled;
        if (runJobs) {
            _workerBusy = true;
        }
        portEXIT_CRITICAL(&s_bleWorkerStateMux);

        if (!runJobs) {
            markBleWorkerDiag(161, bits);
            continue;
        }

        if (bits & WORKER_JOB_CONNECT) {
            markBleWorkerDiag(162, bits);
            _doConnectJob();
            markBleWorkerDiag(163, bits);
        }
        if (_radioEnabled && (bits & WORKER_JOB_POLL_GPS)) {
            markBleWorkerDiag(164, bits);
            _doGpsPollJob();
            markBleWorkerDiag(165, bits);
        }
        if (_radioEnabled && (bits & WORKER_JOB_POLL_CONTROL)) {
            markBleWorkerDiag(166, bits);
            _doControlPollJob();
            markBleWorkerDiag(167, bits);
        }
        if (_radioEnabled && (bits & WORKER_JOB_SEND_ENRICH)) {
            markBleWorkerDiag(168, bits);
            _doEnrichmentSendJob();
            markBleWorkerDiag(169, bits);
        }

        bool finishDisable = false;
        portENTER_CRITICAL(&s_bleWorkerStateMux);
        if (_radioDisablePending) {
            // Keep busy asserted until cleanup completes so RadioArbiter cannot
            // start WiFi between the blocking job and its disconnect cleanup.
            _radioDisablePending = false;
            finishDisable = true;
        } else {
            _workerBusy = false;
        }
        portEXIT_CRITICAL(&s_bleWorkerStateMux);

        if (finishDisable) {
            markBleWorkerDiag(170, bits);
            _finishRadioDisable();
            markBleWorkerDiag(171, bits);
            portENTER_CRITICAL(&s_bleWorkerStateMux);
            _workerBusy = false;
            portEXIT_CRITICAL(&s_bleWorkerStateMux);
        }
        markBleWorkerDiag(172, bits);
    }
}

void BLEManager::_doConnectJob() {
    if (!_haveTargetAddress) {
        _connectResultOk = false;
        _connectResultPending = true;
        return;
    }

    // Consume probe flag before any early-return paths below.
    const bool isProbe = _probeConnectPending || _manualProbeActive;
    _probeConnectPending = false;
    const uint32_t connectTimeoutMs = isProbe ? CONNECT_TIMEOUT_PROBE_MS
                                               : CONNECT_TIMEOUT_MS;

    if (!_client) {
        _client = NimBLEDevice::createClient();
        _client->setClientCallbacks(&_clientCallbacks, false);
        _client->setConnectTimeout(connectTimeoutMs);
        _client->setConnectionParams(6,
                                     12,
                                     0,
                                     PHONE_CONN_SUPERVISION_TIMEOUT_10MS,
                                     CONNECT_SCAN_INTERVAL,
                                     CONNECT_SCAN_WINDOW);
    } else if (isProbe) {
        // Client object already exists from a prior attempt; update its timeout.
        _client->setConnectTimeout(connectTimeoutMs);
    }

    _clearRemoteHandles();

    DLOG_INFO(TAG,
              "connect params timeoutMs=%lu scanItvl=%u scanWin=%u addrType=%u",
              static_cast<unsigned long>(connectTimeoutMs),
              static_cast<unsigned>(CONNECT_SCAN_INTERVAL),
              static_cast<unsigned>(CONNECT_SCAN_WINDOW),
              static_cast<unsigned>(_targetAddress.getType()));

    const bool connected = _client->connect(_targetAddress, true);
    const int connectErr = _client ? _client->getLastError() : 0;
    DLOG_INFO(TAG,
              "connect result ok=%u err=%d addr=%s",
              connected ? 1u : 0u,
              connectErr,
              _connectedPeerAddr);
    if (!connected) {
        DLOG_WARN(TAG,
                  "connect failed at client->connect err=%d addr=%s",
                  connectErr,
                  _connectedPeerAddr);
        // Delete the client object so any NimBLE-internal per-client allocations
        // from the failed attempt are released before the next reconnect cycle.
        // _doConnectJob will create a fresh one when called again.
        NimBLEDevice::deleteClient(_client);
        _client = nullptr;
        // Per the BLE bring-up rules: a client->connect failure must drop
        // the cached peer and force a fresh scan rather than retry the
        // same address.  _handleConnectOutcome will handle the rescan.
        DLOG_WARN(TAG, "cleanup: dropping client and peer cache reason=connect_failed_client_connect");
        _clearTargetCache("connect_failed_client_connect");
        _connectResultOk = false;
        _connectResultPending = true;
        return;
    }

    _connectStartedMs = millis();
    _renewBleLeaseIfOwned("app auth");
    DLOG_INFO(TAG, "BLE link connected; starting app-layer auth watchdog=%lums",
              static_cast<unsigned long>(CONNECT_WATCHDOG_MS));

    _state = BLE_CONNECTED;

    if (!_bindRemoteCharacteristics()) {
        DLOG_WARN(TAG, "connect failed at characteristic bind");
        _ignoreDisconnectOnce = true;
        if (_client->isConnected()) {
            _client->disconnect();
        }
        // Same cleanup: drop the client object after a bind failure so we start
        // fresh on retry rather than reusing a client in an uncertain state.
        NimBLEDevice::deleteClient(_client);
        _client = nullptr;
        // Bind failure also counts as a connect-failure for cache purposes.
        DLOG_WARN(TAG, "cleanup: dropping client and peer cache reason=connect_failed_bind");
        _clearTargetCache("connect_failed_bind");
        _connectResultOk = false;
        _connectResultPending = true;
        return;
    }

    _connectResultOk = true;
    _connectResultPending = true;
}

void BLEManager::_doGpsPollJob() {
    markBleWorkerDiag(180);
    if (!_gpsRemoteChar || !_client || !_client->isConnected() || !_gpsRemoteChar->canRead()) {
        markBleWorkerDiag(181);
        return;
    }

    markBleWorkerDiag(182);
    NimBLEAttValue value = _gpsRemoteChar->readValue();
    markBleWorkerDiag(183, value.size());
    if (value.size() == 0) {
        return;
    }
    _queueGpsFrameFromCallback(value.data(), value.size());
    markBleWorkerDiag(184, value.size());
}

void BLEManager::_doControlPollJob() {
    markBleWorkerDiag(190);
    if (!_controlRemoteChar || !_client || !_client->isConnected() || !_controlRemoteChar->canRead()) {
        markBleWorkerDiag(191);
        return;
    }

    markBleWorkerDiag(192);
    NimBLEAttValue value = _controlRemoteChar->readValue();
    markBleWorkerDiag(193, value.size());
    if (value.size() == 0) {
        return;
    }
    _queueControlFrameFromCallback(value.data(), value.size());
    markBleWorkerDiag(194, value.size());
}

void BLEManager::_doEnrichmentSendJob() {
    // Keep the queued latch set until the worker has finished the blocking
    // GATT write.  Clearing it on entry lets tick() observe
    // pending=true/queued=false and enqueue the same batch a second time.
    if (RADIO_ARB.currentOwner() == RADIO_WIFI_UPLOAD) {
        DLOG_WARN(TAG, "enrichment send failed: upload active");
        _failEnrichment("upload_active");
        return;
    }

    if (!_eventBatchRemoteChar || !_client || !_client->isConnected()) {
        DLOG_WARN(TAG, "enrichment send failed: link down");
        _failEnrichment("send_link_down");
        return;
    }

    if (!_eventBatchRemoteChar->canWrite() && !_eventBatchRemoteChar->canWriteNoResponse()) {
        DLOG_WARN(TAG, "enrichment send failed: batch char not writable");
        _failEnrichment("batch_char_not_writable");
        return;
    }

    if (_eventBatchTxLen == 0 || _enrichmentExpectedCount == 0) {
        DLOG_WARN(TAG, "enrichment send failed: empty payload");
        _failEnrichment("empty_payload");
        return;
    }

    const uint32_t tSendStart = millis();
    const uint16_t mtu = _client->getMTU();
    const uint16_t attPayload = (mtu > 3) ? static_cast<uint16_t>(mtu - 3) : 20;
    DLOG_INFO(TAG, "enrichment send mtu=%u attPayload=%u bytes=%u count=%u",
              static_cast<unsigned>(mtu),
              static_cast<unsigned>(attPayload),
              static_cast<unsigned>(_eventBatchTxLen),
              static_cast<unsigned>(_enrichmentExpectedCount));

    size_t secureTxLen = 0;
    if (!_secureSession.encrypt(PHONE_SECURE_CHANNEL_EVENT_BATCH,
                                _payload->eventBatchTx,
                                _eventBatchTxLen,
                                _payload->eventBatchSecureTx,
                                sizeof(_payload->eventBatchSecureTx),
                                secureTxLen)) {
        DLOG_WARN(TAG, "enrichment send failed: encrypt: %s",
                  _secureSession.lastError() ? _secureSession.lastError() : "-");
        _failEnrichment("batch_encrypt_failed");
        return;
    }

    const bool ok = _eventBatchRemoteChar->writeValue(_payload->eventBatchSecureTx, secureTxLen, true);
    if (!ok) {
        // Android/Samsung can still deliver the write even when NimBLE reports
        // a false return; the phone may acknowledge BATCH_RX shortly after.
        // Arm the normal response timeout and let the ACK/notify path decide.
        DLOG_WARN(TAG, "enrichment write returned false; waiting for ack/response");
    }

    _enrichmentRequestPending = false;
    _enrichmentInFlight = true;
    _enrichmentSendQueued = false;
    _enrichmentReady = false;
    _enrichmentRxLen = 0;
    _enrichmentAvailableCount = 0;
    _enrichmentBatchAcked = false;
    _enrichmentSendMs = millis() - tSendStart;
    _enrichmentXferMs = 0;
    _enrichmentWaitStartMs = millis();
    _enrichmentDeadlineMs = millis() + ENRICHMENT_TIMEOUT_MS;

    DLOG_INFO(TAG, "enrichment batch sent plain=%u sealed=%u count=%u writeOk=%d",
              static_cast<unsigned>(_eventBatchTxLen),
              static_cast<unsigned>(secureTxLen),
              static_cast<unsigned>(_enrichmentExpectedCount),
              ok ? 1 : 0);

    if (!_enrichmentNotifyEnabled && _enrichmentRemoteChar &&
        _enrichmentRemoteChar->canRead()) {
        NimBLEAttValue value = _enrichmentRemoteChar->readValue();
        if (value.size() > 0) {
            _queueEnrichmentChunkFromCallback(value.data(), value.size());
        }
    }
}

void BLEManager::_onAdvertisedDevice(const NimBLEAdvertisedDevice* advertisedDevice) {
    if (!advertisedDevice) {
        return;
    }

    const uint32_t now = millis();

    if (_scanDiagUntilMs != 0 && now < _scanDiagUntilMs) {
        // ── Compact check line (every advert, no cap) ──────────────────────
        // Tells us immediately whether the ESP32 is seeing the advert at all
        // and whether the service UUID is being parsed correctly, without
        // flooding serial with full payload for every background device.
        const std::string addr = advertisedDevice->getAddress().toString();
        const uint8_t addrType =
            static_cast<uint8_t>(advertisedDevice->getAddressType());
        const bool hasParsedTargetSvc =
            advertisedDevice->isAdvertisingService(NimBLEUUID(_targetServiceUUID));
        const bool hasRawTargetSvc =
            payloadAdvertisesUuid128(advertisedDevice->getPayload(), _targetServiceUUID);
        const bool hasTargetSvc = hasParsedTargetSvc || hasRawTargetSvc;
        const TargetMatch matchKind = _matchesTarget(advertisedDevice);
        const char* matchedBy =
            (matchKind == TargetMatch::ServiceUuid)  ? "service" :
            (matchKind == TargetMatch::NameFallback) ? "name"    :
                                                       "none";

        DLOG_DEBUG(TAG,
                  "adv check addr=%s addrType=%u targetSvc=%d matchedBy=%s rssi=%d",
                  addr.c_str(),
                  static_cast<unsigned>(addrType),
                  hasTargetSvc ? 1 : 0,
                  matchedBy,
                  advertisedDevice->getRSSI());

        // ── Full detail (target always; non-target capped at 20) ───────────
        // Once the cap is reached non-target background devices are silenced,
        // but target/Spectre adverts still get the full payload dump.
        const bool isNonTarget = !hasTargetSvc;
        const bool fullDetail  = !isNonTarget || (_scanDiagSeen < 20);
        if (isNonTarget && _scanDiagSeen < 20) {
            _scanDiagSeen++;
        }

        if (fullDetail) {
            const std::string name = advertisedDevice->getName();
            const std::vector<uint8_t>& payload = advertisedDevice->getPayload();
            const uint8_t svcCount = advertisedDevice->getServiceUUIDCount();
            char payloadHex[80];
            formatHexSnippet(payload, payloadHex, sizeof(payloadHex));

            DLOG_DEBUG(TAG,
                      "adv raw name='%s' addr=%s addrType=%u rssi=%d hasName=%d hasSvc=%d targetSvc=%d rawTargetSvc=%d svcCount=%u matchedBy=%s",
                      name.c_str(),
                      addr.c_str(),
                      static_cast<unsigned>(addrType),
                      advertisedDevice->getRSSI(),
                      advertisedDevice->haveName()        ? 1 : 0,
                      advertisedDevice->haveServiceUUID() ? 1 : 0,
                      hasTargetSvc                        ? 1 : 0,
                      hasRawTargetSvc                     ? 1 : 0,
                      static_cast<unsigned>(svcCount),
                      matchedBy);
            DLOG_DEBUG(TAG,
                      "adv raw payload len=%u hex=%s",
                      static_cast<unsigned>(payload.size()),
                      payloadHex);

            if (advertisedDevice->haveServiceUUID()) {
                DLOG_DEBUG(TAG, "adv svc=%s",
                          advertisedDevice->getServiceUUID().toString().c_str());
            }

            // Spectre-name spotlight: dump full UUID list for any device whose
            // name contains "Spectre" even if targetSvc=0, so we notice it
            // regardless of which PDU the service UUID arrived in (or didn't).
            if (name.find("Spectre") != std::string::npos) {
                char svcBuf[128];
                if (svcCount == 0) {
                    strlcpy(svcBuf, "(none)", sizeof(svcBuf));
                } else {
                    svcBuf[0] = '\0';
                    for (uint8_t i = 0; i < svcCount; i++) {
                        std::string uuidStr =
                            advertisedDevice->getServiceUUID(i).toString();
                        if (i > 0) { strlcat(svcBuf, ",", sizeof(svcBuf)); }
                        strlcat(svcBuf, uuidStr.c_str(), sizeof(svcBuf));
                    }
                }
                DLOG_DEBUG(TAG,
                          "adv SPECTRE-NAME name='%s' addr=%s svcs=%u targetSvc=%d [%s]",
                          name.c_str(),
                          addr.c_str(),
                          static_cast<unsigned>(svcCount),
                          hasTargetSvc ? 1 : 0,
                          svcBuf);
            }
        }
    }

    if (_state != BLE_SCANNING) {
        return;
    }

    const TargetMatch matchKind = _matchesTarget(advertisedDevice);
    if (matchKind == TargetMatch::None) {
        return;
    }

    const bool isServiceMatch = (matchKind == TargetMatch::ServiceUuid);
    const char* matchedBy = isServiceMatch ? "service" : "name";
    _lastTargetSeenMs = millis();
    _lastTargetRssi = advertisedDevice->getRSSI();

    // Copy peer data from the advertisement — address and name are snapshotted
    // here while we still have the advertised-device pointer.
    _targetAddress = advertisedDevice->getAddress();

    // Do NOT cache the peer address yet.  Android random/private addresses
    // (addrType=1) are volatile and may rotate before a reconnect.
    // _pendingConnectIsServiceMatch remembers the match type so
    // _bindRemoteCharacteristics can commit the cache once PHONE_SERVICE_UUID
    // is confirmed via GATT service discovery.
    _haveTargetAddress            = true;
    _targetCachedFromService      = false;      // committed later in _bindRemoteCharacteristics
    _pendingConnectIsServiceMatch = isServiceMatch;
    _targetFound                  = true;

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

    DLOG_INFO(TAG,
              "target found matchedBy=%s cachePeer=0 addr=%s name=%s rssi=%d",
              matchedBy,
              _connectedPeerAddr,
              _connectedDeviceName,
              static_cast<int>(_lastTargetRssi));

    // tick() stops the scanner after this callback returns, then connects.
}

void BLEManager::_onClientConnected(NimBLEClient* pClient) {
    (void)pClient;
    _clientConnected = true;
    _publishBleState();
    _refreshStatusCharacteristic();
}

void BLEManager::_onClientDisconnected(NimBLEClient* pClient, int reason) {
    (void)pClient;
    const bool enrichmentActive =
        _enrichmentRequestPending || _enrichmentInFlight || _enrichmentSendQueued;
    const bool radioReleased =
        !_radioEnabled || !RADIO_ARB.isBleOwner();
    _lastDisconnectReason = reason;
    _clientConnected = false;
    _lastLeaseRenewMs = 0;
    // A Wi-Fi bulk transfer deliberately releases the BLE radio after the
    // authenticated endpoint command is accepted. Its index and watermarks
    // must remain owned by PhoneOffloadManager until the TCP batch protocol
    // completes or times out.
    if (!PHONE_OFFLOAD.wifiBulkActive()) {
        PHONE_OFFLOAD.end(0, "ble_disconnect");
    } else {
        DLOG_INFO(TAG, "BLE disconnect preserved active Wi-Fi bulk transfer");
    }
    _clearRemoteHandles();
    _clearBleRxQueues();

    if (_ignoreDisconnectOnce || radioReleased) {
        _ignoreDisconnectOnce = false;
        if (radioReleased) {
            DLOG_INFO(TAG, "peer disconnect ignored after radio release reason=%d",
                      reason);
        }
        _publishBleState();
        return;
    }

    if (enrichmentActive) {
        _enrichmentFailed = true;
        DLOG_WARN(TAG, "enrichment failed reason=peer_disconnected");
    }
    _publishBleState();

    if (_dirtyDisconnectCount < 255) {
        _dirtyDisconnectCount++;
    }

    DLOG_WARN(TAG, "peer disconnected reason=%d dirty=%u",
              reason,
              static_cast<unsigned>(_dirtyDisconnectCount));

    if (_dirtyDisconnectCount >= 2) {
        _hardDropClient("repeated_dirty_disconnect");
    }

    _scheduleReconnect("peer disconnected");
}

void BLEManager::_onServerConnected(const NimBLEConnInfo* connInfo) {
    _serverConnected = true;
    _advertisingActive = false;
    if (connInfo) {
        _serverConnHandle = connInfo->getConnHandle();
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
    _serverConnHandle = BLE_HS_CONN_HANDLE_NONE;
    _publishBleState();
    _refreshStatusCharacteristic();
    DLOG_INFO(TAG, "server disconnect reason=%d", reason);

    if (_radioEnabled && (_textInputPending || _manualProbeActive)) {
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

    if (pCharacteristic == _linkRequestChar) {
        static constexpr char LINK_REQUEST[] = "LINK1";
        const uint32_t now = millis();
        const bool cooledDown = _lastPhoneLinkRequestMs == 0 ||
            now - _lastPhoneLinkRequestMs >= PHONE_LINK_REQUEST_COOLDOWN_MS;
        if (cooledDown &&
            value.size() == sizeof(LINK_REQUEST) - 1U &&
            memcmp(value.data(), LINK_REQUEST, sizeof(LINK_REQUEST) - 1U) == 0) {
            _lastPhoneLinkRequestMs = now;
            _phoneLinkRequestPending = true;
            DLOG_INFO(TAG, "phone-side link request queued");
        } else {
            DLOG_WARN(TAG, "phone-side link request rejected bytes=%u cooldown=%u",
                      static_cast<unsigned>(value.size()),
                      cooledDown ? 0U : 1U);
        }
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
        s_bleInstance->_queueGpsFrameFromCallback(data, len);
    }
}

void BLEManager::_controlNotifyThunk(NimBLERemoteCharacteristic* chr,
                                     uint8_t* data,
                                     size_t len,
                                     bool isNotify) {
    (void)chr;
    (void)isNotify;
    if (s_bleInstance) {
        s_bleInstance->_queueControlFrameFromCallback(data, len);
    }
}

void BLEManager::_enrichmentNotifyThunk(NimBLERemoteCharacteristic* chr,
                                        uint8_t* data,
                                        size_t len,
                                        bool isNotify) {
    (void)chr;
    (void)isNotify;
    markBleRxDiag(110, len);
    if (s_bleInstance) {
        s_bleInstance->_queueEnrichmentChunkFromCallback(data, len);
    }
    markBleRxDiag(118, len);
}

void BLEManager::_commandReqNotifyThunk(NimBLERemoteCharacteristic* chr,
                                        uint8_t* data,
                                        size_t len,
                                        bool isNotify) {
    (void)chr;
    (void)isNotify;
    if (s_bleInstance) {
        s_bleInstance->_queueCommandRequestFromCallback(data, len);
    }
}

void BLEManager::_authNotifyThunk(NimBLERemoteCharacteristic* chr,
                                  uint8_t* data,
                                  size_t len,
                                  bool isNotify) {
    (void)chr;
    (void)isNotify;
    if (s_bleInstance) {
        s_bleInstance->_queueAuthFrameFromCallback(data, len);
    }
}

// ── Enrichment failure helper ──

void BLEManager::_resetEnrichmentExchangeState(bool preserveFailure) {
    _enrichmentRequestPending = false;
    _enrichmentInFlight = false;
    _enrichmentReady = false;
    _enrichmentSendQueued = false;
    _enrichmentBatchAcked = false;
    _enrichmentSendMs = 0;
    _enrichmentXferMs = 0;
    _enrichmentWaitStartMs = 0;
    _enrichmentDeadlineMs = 0;
    _eventBatchTxLen = 0;
    _enrichmentRxLen = 0;
    _enrichmentExpectedCount = 0;
    _enrichmentAvailableCount = 0;

    if (!preserveFailure) {
        _enrichmentFailed = false;
    }

    memset(_payload->enrichmentRx, 0, sizeof(_payload->enrichmentRx));
    memset(_payload->enrichmentBatch, 0, sizeof(_payload->enrichmentBatch));
    _clearBleRxQueues();
}

void BLEManager::_failEnrichment(const char* reason) {
    _resetEnrichmentExchangeState(true);
    _enrichmentFailed = true;

    DLOG_WARN(TAG, "enrichment failed reason=%s", reason ? reason : "-");

    // Keep the event batch and pending event list intact so the caller can retry.
    _publishBleState();
    _refreshStatusCharacteristic();
}

bool BLEManager::_bindRemoteCharacteristics() {
    if (!_client || !_client->isConnected()) {
        return false;
    }

    DLOG_INFO(TAG, "remote service lookup begin core=%d internalFree=%u largest=%u",
              xPortGetCoreID(),
              static_cast<unsigned>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
              static_cast<unsigned>(heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM)));
    _remoteService = _client->getService(_targetServiceUUID);
    DLOG_INFO(TAG, "remote service lookup end found=%u err=%d",
              _remoteService ? 1u : 0u,
              _client ? _client->getLastError() : 0);
    if (!_remoteService) {
        DLOG_ERROR(TAG, "remote service missing %s", _targetServiceUUID);
        return false;
    }

    DLOG_INFO(TAG, "remote characteristic discovery begin");
    const auto& discoveredChars = _remoteService->getCharacteristics(true);
    DLOG_INFO(TAG, "remote characteristic discovery end count=%u err=%d",
              static_cast<unsigned>(discoveredChars.size()),
              _client ? _client->getLastError() : 0);
    if (discoveredChars.empty()) {
        DLOG_ERROR(TAG, "remote service has no characteristics");
        return false;
    }

    _gpsRemoteChar = _remoteService->getCharacteristic(PHONE_GPS_CHAR_UUID);
    _controlRemoteChar = _remoteService->getCharacteristic(PHONE_CONTROL_CHAR_UUID);
    _metaRemoteChar = _remoteService->getCharacteristic(PHONE_META_CHAR_UUID);
    _eventBatchRemoteChar = _remoteService->getCharacteristic(PHONE_EVENT_BATCH_UUID);
    _enrichmentRemoteChar = _remoteService->getCharacteristic(PHONE_ENRICHMENT_UUID);
    _authRemoteChar = _remoteService->getCharacteristic(PHONE_AUTH_CHAR_UUID);
    _authWriteRemoteChar = _remoteService->getCharacteristic(PHONE_AUTH_REQUEST_CHAR_UUID);
    _storageRemoteChar = _remoteService->getCharacteristic(PHONE_STORAGE_CHAR_UUID);
    _commandReqRemoteChar  = _remoteService->getCharacteristic(PHONE_COMMAND_REQ_CHAR_UUID);
    _commandRespRemoteChar = _remoteService->getCharacteristic(PHONE_COMMAND_RESP_CHAR_UUID);
    _logStreamRemoteChar   = _remoteService->getCharacteristic(PHONE_LOG_STREAM_CHAR_UUID);
    _dashboardStreamRemoteChar = _remoteService->getCharacteristic(PHONE_DASHBOARD_STREAM_CHAR_UUID);
    _notificationRemoteChar = _remoteService->getCharacteristic(PHONE_NOTIFICATION_CHAR_UUID);

    if (!_authRemoteChar || !_authRemoteChar->canNotify() ||
        !_authWriteRemoteChar ||
        (!_authWriteRemoteChar->canWrite() && !_authWriteRemoteChar->canWriteNoResponse())) {
        DLOG_ERROR(TAG, "remote service missing directional auth chars");
        _lastAuthFailReason = BleAuthFailReason::AUTH_CHAR_NOT_FOUND;
        return false;
    }

    if (!_gpsRemoteChar && !_controlRemoteChar) {
        DLOG_ERROR(TAG, "remote service has no usable chars");
        return false;
    }

    const auto discoverNotifyDescriptors = [this](NimBLERemoteCharacteristic* chr,
                                                   const char* label) {
        if (!chr || !chr->canNotify()) {
            return true;
        }
        const auto& descriptors = chr->getDescriptors(true);
        DLOG_INFO(TAG, "remote descriptor discovery char=%s count=%u err=%d",
                  label,
                  static_cast<unsigned>(descriptors.size()),
                  _client ? _client->getLastError() : 0);
        return !descriptors.empty();
    };

    if (!discoverNotifyDescriptors(_authRemoteChar, "auth")) {
        DLOG_ERROR(TAG, "remote auth descriptor discovery failed");
        return false;
    }

    _gpsNotifyEnabled = false;
    _controlNotifyEnabled = false;
    _enrichmentNotifyEnabled = false;
    _authNotifyEnabled = false;
    _commandReqNotifyEnabled = false;

    if (!_authenticateRemote()) {
        DLOG_ERROR(TAG, "remote auth failed: %s",
                   _secureSession.lastError() ? _secureSession.lastError() : "-");
        return false;
    }

    // Service discovery plus signed app-layer auth confirmed the peer.  Safe
    // to cache service-UUID matches for short reconnects.
    if (_pendingConnectIsServiceMatch) {
        _targetCachedFromService      = true;
        _targetCachedAtMs             = millis();
        _lastTargetSeenMs             = _targetCachedAtMs;
        _pendingConnectIsServiceMatch = false;
        DLOG_INFO(TAG, "phone auth confirmed; caching peer addr=%s ttl=%lums rssi=%d",
                  _connectedPeerAddr,
                  static_cast<unsigned long>(PHONE_PEER_CACHE_TTL_MS),
                  static_cast<int>(_lastTargetRssi));
    }

    if (_enrichmentRemoteChar && _enrichmentRemoteChar->canNotify() &&
        discoverNotifyDescriptors(_enrichmentRemoteChar, "enrichment")) {
        _enrichmentNotifyEnabled = _enrichmentRemoteChar->subscribe(true, _enrichmentNotifyThunk, true);
    }
    if (!_enrichmentNotifyEnabled) {
        DLOG_ERROR(TAG, "remote enrichment subscription failed");
        return false;
    }

    // Phone-originated commands (screen changes, status/log requests, durable
    // field offload) travel phone -> Spectre on this notify characteristic.
    // Merely discovering it is not enough: without the CCCD subscription the
    // app can show a secure session while every control request disappears.
    if (_commandReqRemoteChar && _commandReqRemoteChar->canNotify() &&
        discoverNotifyDescriptors(_commandReqRemoteChar, "command_req")) {
        _commandReqNotifyEnabled = _commandReqRemoteChar->subscribe(
            true, _commandReqNotifyThunk, true);
    }
    if (_commandReqRemoteChar && !_commandReqNotifyEnabled) {
        DLOG_ERROR(TAG, "remote command request subscription failed");
        return false;
    }

    if (_metaRemoteChar && _metaRemoteChar->canRead()) {
        NimBLEAttValue meta = _metaRemoteChar->readValue();
        uint8_t plain[sizeof(_metadataBuf)] = {};
        size_t plainLen = 0;
        if (meta.size() > 0 &&
            _secureSession.decrypt(PHONE_SECURE_CHANNEL_META,
                                   meta.data(),
                                   meta.size(),
                                   plain,
                                   sizeof(plain) - 1,
                                   plainLen)) {
            const size_t n = clampValue<size_t>(plainLen, 0, sizeof(_metadataBuf) - 1);
            memcpy(_metadataBuf, plain, n);
            _metadataBuf[n] = '\0';
        } else if (meta.size() > 0) {
            DLOG_WARN(TAG, "metadata decrypt failed: %s",
                      _secureSession.lastError() ? _secureSession.lastError() : "-");
        }
    }

    if (_gpsRemoteChar && _gpsRemoteChar->canRead()) {
        NimBLEAttValue gps = _gpsRemoteChar->readValue();
        if (gps.size() > 0) {
            _queueGpsFrameFromCallback(gps.data(), gps.size());
        }
    }

    if (_controlRemoteChar && _controlRemoteChar->canRead()) {
        NimBLEAttValue ctrl = _controlRemoteChar->readValue();
        if (ctrl.size() > 0) {
            _queueControlFrameFromCallback(ctrl.data(), ctrl.size());
        }
    }

    _manualProbeActive = false;
    _state = BLE_SUBSCRIBED;
    const uint16_t mtu = (_client && _client->isConnected()) ? _client->getMTU() : 0;
    const uint16_t attPayload = (mtu > 3) ? static_cast<uint16_t>(mtu - 3) : 0;
    DLOG_INFO(TAG, "remote bound gps=%d ctrl=%d meta=%d batch=%d enrich=%d auth=%d authReq=%d storage=%d cmdReq=%d cmdResp=%d logStream=%d dashStream=%d notification=%d notify(gps=%d ctrl=%d enrich=%d auth=%d cmdReq=%d) mtu=%u attPayload=%u",
              _gpsRemoteChar ? 1 : 0,
              _controlRemoteChar ? 1 : 0,
              _metaRemoteChar ? 1 : 0,
              _eventBatchRemoteChar ? 1 : 0,
              _enrichmentRemoteChar ? 1 : 0,
              _authRemoteChar ? 1 : 0,
              _authWriteRemoteChar ? 1 : 0,
              _storageRemoteChar ? 1 : 0,
              _commandReqRemoteChar ? 1 : 0,
              _commandRespRemoteChar ? 1 : 0,
              _logStreamRemoteChar ? 1 : 0,
              _dashboardStreamRemoteChar ? 1 : 0,
              _notificationRemoteChar ? 1 : 0,
              _gpsNotifyEnabled ? 1 : 0,
              _controlNotifyEnabled ? 1 : 0,
              _enrichmentNotifyEnabled ? 1 : 0,
              _authNotifyEnabled ? 1 : 0,
              _commandReqNotifyEnabled ? 1 : 0,
              static_cast<unsigned>(mtu),
              static_cast<unsigned>(attPayload));

    // Compact one-line summaries — keep these; they are the field-readable record.
    const uint32_t scanMs = (_probeStartMs != 0 && _lastTargetSeenMs > _probeStartMs)
                            ? (_lastTargetSeenMs - _probeStartMs)
                            : 0;
    DLOG_INFO(TAG, "probe_summary seen=1 matchedBy=%s rssi=%d scanMs=%lu",
              _targetCachedFromService ? "service" : "name",
              static_cast<int>(_lastTargetRssi),
              static_cast<unsigned long>(scanMs));
    DLOG_INFO(TAG, "phone_session_summary auth=ok mtu=%u gps=%d ctrl=%d enrich=%d storage=%d",
              static_cast<unsigned>(mtu),
              _gpsRemoteChar ? 1 : 0,
              _controlRemoteChar ? 1 : 0,
              _enrichmentRemoteChar ? 1 : 0,
              _storageRemoteChar ? 1 : 0);
    return true;
}

bool BLEManager::_authenticateRemote() {
    if (!_authRemoteChar || !_authWriteRemoteChar || !_client || !_client->isConnected()) {
        // Caller (_bindRemoteCharacteristics) already set AUTH_CHAR_NOT_FOUND
        // for the char-missing case; leave reason as-is.
        return false;
    }

    _lastAuthFailReason = BleAuthFailReason::NONE;

    // Keep the live GATT path flash-free. RTC breadcrumbs still survive
    // panic/watchdog resets without taking the NVS lock from this worker.
    crashCheckpointVolatile(CrashPhase::BLE_AUTH,
                            static_cast<uint8_t>(RADIO_ARB.currentOwner()),
                            0 /* pending not available here */);

    auto failAuth = []() {
        crashBreadcrumbClearVolatile(CrashPhase::BLE_AUTH);
        return false;
    };

    xSemaphoreTake(_rxMutex, portMAX_DELAY);
    _authRxPending = false;
    _authRxLen = 0;
    _authRxDrops = 0;
    xSemaphoreGive(_rxMutex);

    _secureSession.reset();

    DLOG_INFO(TAG, "auth notify subscribe begin");
    _authNotifyEnabled = _authRemoteChar->subscribe(true, _authNotifyThunk, true);
    DLOG_INFO(TAG, "auth notify subscribe end ok=%u err=%d",
              _authNotifyEnabled ? 1u : 0u,
              _client ? _client->getLastError() : 0);
    if (!_authNotifyEnabled) {
        DLOG_WARN(TAG, "auth notify subscribe failed");
        _lastAuthFailReason = BleAuthFailReason::AUTH_NOTIFY_MISSING;
        return failAuth();
    }

    size_t challengeLen = 0;
    if (!_secureSession.buildChallenge(_payload->authChallenge,
                                       sizeof(_payload->authChallenge),
                                       challengeLen)) {
        // buildChallenge failure is a local crypto init problem; no specific
        // auth-fail reason is set — _secureSession.lastError() covers it.
        return failAuth();
    }

    DLOG_INFO(TAG,
              "auth challenge write bytes=%u mtu=%u attPayload=%u mode=response",
              static_cast<unsigned>(challengeLen),
              static_cast<unsigned>(_client ? _client->getMTU() : 0),
              static_cast<unsigned>(_client && _client->getMTU() > 3
                                        ? _client->getMTU() - 3
                                        : 0));
    // Authentication runs after MTU negotiation. A 120-byte write remains far
    // below the 244-byte ATT payload we request and avoids five response-mode
    // writes plus artificial gaps on every field connection.
    constexpr size_t AUTH_WRITE_CHUNK_SIZE = 120;
    for (size_t offset = 0; offset < challengeLen; offset += AUTH_WRITE_CHUNK_SIZE) {
        const size_t chunkLen = min(AUTH_WRITE_CHUNK_SIZE, challengeLen - offset);
        if (!_authWriteRemoteChar->writeValue(_payload->authChallenge + offset, chunkLen, true)) {
            DLOG_WARN(TAG,
                      "auth challenge write failed offset=%u bytes=%u",
                      static_cast<unsigned>(offset),
                      static_cast<unsigned>(chunkLen));
            _lastAuthFailReason = BleAuthFailReason::GATT_WRITE_FAILED;
            return failAuth();
        }
        if (offset + chunkLen < challengeLen) vTaskDelay(pdMS_TO_TICKS(5));
    }

    const uint32_t deadline = millis() + 6000UL;
    size_t responseLen = 0;

    while (millis() < deadline && _client && _client->isConnected()) {
        bool haveResponse = false;
        xSemaphoreTake(_rxMutex, portMAX_DELAY);
        if (_authRxPending && _authRxLen > 0) {
            responseLen = min(_authRxLen, sizeof(_payload->authResponse));
            memcpy(_payload->authResponse, _payload->authRx, responseLen);
            _authRxPending = false;
            _authRxLen = 0;
            haveResponse = true;
        }
        xSemaphoreGive(_rxMutex);

        if (haveResponse) {
            const bool ok = _secureSession.completeFromResponse(_payload->authResponse, responseLen);
            memset(_payload->authResponse, 0, sizeof(_payload->authResponse));
            if (ok) {
                DLOG_INFO(TAG, "app secure session ready alg=P-256+ECDH+AES-256-GCM");
                crashBreadcrumbClearVolatile(CrashPhase::BLE_AUTH);
            } else {
                _lastAuthFailReason = BleAuthFailReason::AES_GCM_OPEN_FAILED;
                crashBreadcrumbClearVolatile(CrashPhase::BLE_AUTH);
            }
            return ok;
        }

        vTaskDelay(pdMS_TO_TICKS(25));
    }

    // Distinguish clean timeout from mid-auth disconnect.
    if (_client && _client->isConnected()) {
        DLOG_WARN(TAG, "auth response timeout");
        _lastAuthFailReason = BleAuthFailReason::SECURE_RESPONSE_TIMEOUT;
    } else {
        DLOG_WARN(TAG, "auth response timeout: phone disconnected");
        _lastAuthFailReason = BleAuthFailReason::PHONE_DISCONNECTED;
    }
    return failAuth();
}

void BLEManager::_clearTargetCache(const char* reason) {
    // Always log when we abandon a cached peer so the bring-up log makes
    // it obvious why the next cycle had to fall back to a fresh scan.
    if (_haveTargetAddress || _directReconnectPending || _reconnectAttempt != 0) {
        DLOG_INFO(TAG,
                  "clearing cached peer reason=%s addr=%s svcCached=%u attempts=%u",
                  reason ? reason : "-",
                  _connectedPeerAddr[0] ? _connectedPeerAddr : "none",
                  _targetCachedFromService ? 1u : 0u,
                  static_cast<unsigned>(_reconnectAttempt));
    }
    _haveTargetAddress            = false;
    _targetAddress                = NimBLEAddress();
    _targetCachedFromService      = false;
    _pendingConnectIsServiceMatch = false;
    _connectPendingAfterScan      = false;
    _directReconnectPending       = false;
    _reconnectAttempt             = 0;
    _targetFound                  = false;
    _connectedPeerAddr[0]         = '\0';
    _connectedDeviceName[0]       = '\0';
}

void BLEManager::_renewBleLeaseIfOwned(const char* reason) {
    // refreshLease is a no-op unless the arbiter is currently owned by
    // the requested role, so this is safe even if BLE was started by a
    // path that didn't take the arbiter (e.g. server-only text input).
    const RadioOwner owner = RADIO_ARB.currentOwner();
    if (owner == RADIO_BLE_GPS || owner == RADIO_BLE_TEXT) {
        RADIO_ARB.refreshLease(owner,
                               BLE_LINK_ACTIVE_LEASE_MS,
                               reason ? reason : "ble link active");
    }
}

bool BLEManager::_shouldYieldToUpload() const {
    if (!RADIO_ARB.hasPendingOwner(RADIO_WIFI_UPLOAD)) {
        return false;
    }

    // Keep text-input BLE and any live link/auth/enrichment work intact.
    if (RADIO_ARB.currentOwner() != RADIO_BLE_GPS) {
        return false;
    }

    if (_state != BLE_IDLE && _state != BLE_SCANNING) {
        return false;
    }

    if (_enrichmentInFlight ||
        _connectResultPending ||
        _connectPendingAfterScan) {
        return false;
    }

    return true;
}

void BLEManager::_scheduleProbeSoonButNotNow(uint32_t now) {
    // Hold off long enough for the upload lease to make progress, but keep
    // the probe warm so we can resume as soon as BLE is granted again.
    _nextActionMs = now + 250UL;
}

void BLEManager::_clearRemoteHandles() {
    _remoteService = nullptr;
    _gpsRemoteChar = nullptr;
    _controlRemoteChar = nullptr;
    _metaRemoteChar = nullptr;
    _eventBatchRemoteChar = nullptr;
    _enrichmentRemoteChar = nullptr;
    _authRemoteChar = nullptr;
    _authWriteRemoteChar = nullptr;
    _storageRemoteChar = nullptr;
    _commandReqRemoteChar = nullptr;
    _commandRespRemoteChar = nullptr;
    _logStreamRemoteChar = nullptr;
    _dashboardStreamRemoteChar = nullptr;
    _gpsNotifyEnabled = false;
    _controlNotifyEnabled = false;
    _enrichmentNotifyEnabled = false;
    _authNotifyEnabled = false;
    _commandReqNotifyEnabled = false;
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
    _secureRxFailures = 0;
    _secureSession.reset();
    _authRxPending = false;
    _authRxLen = 0;
    _authRxDrops = 0;
    _clearBleRxQueues();
}

BLEManager::TargetMatch
BLEManager::_matchesTarget(const NimBLEAdvertisedDevice* advertisedDevice) {
    if (_targetServiceUUID[0] == '\0') {
        return TargetMatch::None;
    }

    const bool serviceMatch =
        (advertisedDevice->haveServiceUUID() &&
         advertisedDevice->isAdvertisingService(NimBLEUUID(_targetServiceUUID))) ||
        payloadAdvertisesUuid128(advertisedDevice->getPayload(), _targetServiceUUID);
    if (serviceMatch) {
        return TargetMatch::ServiceUuid;
    }

    // Name fallback covers the bench condition where Android shortens the
    // local name under payload pressure (advertises "SPHONE" instead of
    // "SpectrePhone") and where the 128-bit service UUID isn't surviving the
    // primary AD payload. Accept either the full target name or the short
    // alias so production discovery can link until UUID packing is resolved.
    if (advertisedDevice->haveName()) {
        const std::string name = advertisedDevice->getName();
        const char* matched = nullptr;
        if (_targetDeviceName[0] != '\0' &&
            (equalsIgnoreCase(name.c_str(), _targetDeviceName) ||
             startsWithIgnoreCase(name.c_str(), _targetDeviceName))) {
            matched = _targetDeviceName;
        } else if (_targetDeviceNameShort[0] != '\0' &&
                   equalsIgnoreCase(name.c_str(), _targetDeviceNameShort)) {
            matched = _targetDeviceNameShort;
        }
        if (matched) {
            DLOG_WARN(TAG,
                      "name fallback match name=%s target=%s",
                      name.c_str(),
                      matched);
            return TargetMatch::NameFallback;
        }
    }

    return TargetMatch::None;
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
    while (cleanLen > 0 &&
           (data[cleanLen - 1] == '\r' ||
            data[cleanLen - 1] == '\n' ||
            data[cleanLen - 1] == '\0')) {
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

// ── BLE RX queue — enqueue side (called from NimBLE callback context) ──

void BLEManager::_queueGpsFrameFromCallback(const uint8_t* data, size_t len) {
    if (!data || len < PHONE_GPS_FRAME_SIZE) {
        xSemaphoreTake(_rxMutex, portMAX_DELAY);
        _gpsRxDrops++;
        xSemaphoreGive(_rxMutex);
        return;
    }

    const size_t copyLen = min(len, GPS_RX_MAX);

    xSemaphoreTake(_rxMutex, portMAX_DELAY);

    if (_gpsRxPending) {
        // Latest GPS wins. Count this so we can prove whether callback pressure exists.
        _gpsRxDrops++;
    }

    memcpy(_payload->gpsRx, data, copyLen);
    _gpsRxLen = copyLen;
    _gpsRxPending = true;

    xSemaphoreGive(_rxMutex);
}

void BLEManager::_queueControlFrameFromCallback(const uint8_t* data, size_t len) {
    if (!data || len < PHONE_CONTROL_FRAME_SIZE) {
        xSemaphoreTake(_rxMutex, portMAX_DELAY);
        _controlRxDrops++;
        xSemaphoreGive(_rxMutex);
        return;
    }

    const size_t copyLen = min(len, CONTROL_RX_MAX);

    xSemaphoreTake(_rxMutex, portMAX_DELAY);

    if (_controlRxPending) {
        // Latest command wins for now.
        _controlRxDrops++;
    }

    memcpy(_payload->controlRx, data, copyLen);
    _controlRxLen = copyLen;
    _controlRxPending = true;

    xSemaphoreGive(_rxMutex);
}

void BLEManager::_queueEnrichmentChunkFromCallback(const uint8_t* data, size_t len) {
    markBleRxDiag(111, len);
    if (!data || len == 0 || len > ENRICH_RX_CHUNK_MAX) {
        xSemaphoreTake(_rxMutex, portMAX_DELAY);
        _enrichRxDrops++;
        xSemaphoreGive(_rxMutex);
        return;
    }

    markBleRxDiag(112, len);
    xSemaphoreTake(_rxMutex, portMAX_DELAY);
    markBleRxDiag(113, len);

    if (_enrichRxCount >= ENRICH_RX_SLOTS) {
        _enrichRxDrops++;
        xSemaphoreGive(_rxMutex);
        return;
    }

    BleRxChunk& slot = _payload->enrichRx[_enrichRxTail];
    markBleRxDiag(114, len);
    slot.len = static_cast<uint16_t>(len);
    memcpy(slot.data, data, len);
    markBleRxDiag(115, len);

    _enrichRxTail = (_enrichRxTail + 1) % ENRICH_RX_SLOTS;
    _enrichRxCount++;
    markBleRxDiag(116, len);

    xSemaphoreGive(_rxMutex);
    markBleRxDiag(117, len);
}

void BLEManager::_queueAuthFrameFromCallback(const uint8_t* data, size_t len) {
    if (!data || len != PHONE_AUTH_FRAME_SIZE) {
        xSemaphoreTake(_rxMutex, portMAX_DELAY);
        _authRxDrops++;
        xSemaphoreGive(_rxMutex);
        return;
    }

    xSemaphoreTake(_rxMutex, portMAX_DELAY);
    if (_authRxPending) {
        _authRxDrops++;
    }
    memcpy(_payload->authRx, data, len);
    _authRxLen = len;
    _authRxPending = true;
    xSemaphoreGive(_rxMutex);
}

void BLEManager::_queueCommandRequestFromCallback(const uint8_t* data, size_t len) {
    markBleRxDiag(128, len);
    if (!data || len == 0 || len > COMMAND_REQ_RX_MAX) {
        xSemaphoreTake(_rxMutex, portMAX_DELAY);
        _commandReqRxDrops++;
        xSemaphoreGive(_rxMutex);
        return;
    }

    xSemaphoreTake(_rxMutex, portMAX_DELAY);
    if (_commandReqRxPending) {
        // Latest-wins.  Phone retries via its own request timeout if it
        // submitted a different opcode and the earlier one was dropped.
        _commandReqRxDrops++;
    }
    memcpy(_payload->commandReqRx, data, len);
    _commandReqRxLen = len;
    _commandReqRxPending = true;
    xSemaphoreGive(_rxMutex);
    markBleRxDiag(129, len);
}

// ── BLE RX queue — drain side (called from tick() context only) ──

void BLEManager::_drainBleRxQueues() {
    _drainGpsRx();
    _drainControlRx();
    _drainEnrichmentRx();
    _drainCommandRx();
}

void BLEManager::_drainGpsRx() {
    markBleRxDiag(200);
    uint8_t snapshot[GPS_RX_MAX];
    size_t snapLen = 0;
    bool hadFrame = false;

    xSemaphoreTake(_rxMutex, portMAX_DELAY);
    markBleRxDiag(201);
    if (_gpsRxPending && _gpsRxLen > 0) {
        snapLen = _gpsRxLen;
        if (snapLen > GPS_RX_MAX) {
            snapLen = GPS_RX_MAX;
        }
        memcpy(snapshot, _payload->gpsRx, snapLen);
        _gpsRxPending = false;
        _gpsRxLen = 0;
        hadFrame = true;
        markBleRxDiag(202, snapLen);
    }
    xSemaphoreGive(_rxMutex);
    markBleRxDiag(203, snapLen);

    if (hadFrame) {
        uint8_t plain[PHONE_GPS_FRAME_SIZE];
        size_t plainLen = 0;
        markBleRxDiag(204, snapLen);
        if (_secureSession.decrypt(PHONE_SECURE_CHANNEL_GPS,
                                   snapshot,
                                   snapLen,
                                   plain,
                                   sizeof(plain),
                                   plainLen)) {
            markBleRxDiag(205, plainLen);
            _noteSecureRxSuccess();
            markBleRxDiag(206, plainLen);
            _handleGpsPayload(plain, plainLen);
            markBleRxDiag(207, plainLen);
        } else {
            markBleRxDiag(208, snapLen);
            const char* err = _secureSession.lastError();
            if (err && strstr(err, "replay/stale counter")) {
                DLOG_DEBUG(TAG, "Ignoring stale GPS frame: %s", err);
            } else {
                DLOG_WARN(TAG, "gps decrypt failed: %s", err ? err : "-");
                _noteSecureRxFailure("gps", err);
            }
        }
    }
}

void BLEManager::_drainControlRx() {
    markBleRxDiag(210);
    uint8_t snapshot[CONTROL_RX_MAX];
    size_t snapLen = 0;
    bool hadFrame = false;

    xSemaphoreTake(_rxMutex, portMAX_DELAY);
    markBleRxDiag(211);
    if (_controlRxPending && _controlRxLen > 0) {
        snapLen = _controlRxLen;
        if (snapLen > CONTROL_RX_MAX) {
            snapLen = CONTROL_RX_MAX;
        }
        memcpy(snapshot, _payload->controlRx, snapLen);
        _controlRxPending = false;
        _controlRxLen = 0;
        hadFrame = true;
        markBleRxDiag(212, snapLen);
    }
    xSemaphoreGive(_rxMutex);
    markBleRxDiag(213, snapLen);

    if (hadFrame) {
        uint8_t plain[PHONE_CONTROL_FRAME_SIZE];
        size_t plainLen = 0;
        markBleRxDiag(214, snapLen);
        if (_secureSession.decrypt(PHONE_SECURE_CHANNEL_CONTROL,
                                   snapshot,
                                   snapLen,
                                   plain,
                                   sizeof(plain),
                                   plainLen)) {
            markBleRxDiag(215, plainLen);
            _noteSecureRxSuccess();
            markBleRxDiag(216, plainLen);
            _handleControlPayload(plain, plainLen);
            markBleRxDiag(217, plainLen);
        } else {
            markBleRxDiag(218, snapLen);
            const char* err = _secureSession.lastError();
            if (err && strstr(err, "replay/stale counter")) {
                DLOG_DEBUG(TAG, "Ignoring stale control frame: %s", err);
            } else {
                DLOG_WARN(TAG, "control decrypt failed: %s", err ? err : "-");
                _noteSecureRxFailure("control", err);
            }
        }
    }
}

void BLEManager::_noteSecureRxSuccess() {
    _secureRxFailures = 0;
}

void BLEManager::_noteSecureRxFailure(const char* channel, const char* error) {
    if (_state != BLE_SUBSCRIBED || !_client || !_client->isConnected()) {
        return;
    }
    if (_secureRxFailures < 0xFF) {
        _secureRxFailures++;
    }
    if (_secureRxFailures < 3) {
        return;
    }

    DLOG_WARN(TAG,
              "secure session desync channel=%s failures=%u error=%s; reconnecting",
              channel ? channel : "-",
              static_cast<unsigned>(_secureRxFailures),
              error ? error : "-");
    _softDisconnectClient("secure_session_desync");
    _state = BLE_IDLE;
    _scheduleReconnect("secure session desync");
}

void BLEManager::_drainEnrichmentRx() {
    markBleRxDiag(120);
    while (true) {
        size_t snapLen = 0;
        uint8_t queuedAfterPop = 0;
        bool overflowed = false;

        xSemaphoreTake(_rxMutex, portMAX_DELAY);
        markBleRxDiag(121);
        if (_enrichRxDrops > 0) {
            _enrichRxDrops = 0;
            _enrichRxHead = 0;
            _enrichRxTail = 0;
            _enrichRxCount = 0;
            overflowed = true;
        }

        if (_enrichRxCount == 0) {
            xSemaphoreGive(_rxMutex);
            if (overflowed && _enrichmentInFlight) {
                _failEnrichment("enrich_rx_overflow");
            }
            break;
        }

        BleRxChunk& slot = _payload->enrichRx[_enrichRxHead];
        snapLen = slot.len;
        if (snapLen > ENRICH_RX_CHUNK_MAX) {
            snapLen = ENRICH_RX_CHUNK_MAX;
        }

        if (snapLen > 0) {
            markBleRxDiag(122, snapLen);
            memcpy(_payload->enrichRxScratch, slot.data, snapLen);
            markBleRxDiag(123, snapLen);
        }

        _enrichRxHead = (_enrichRxHead + 1) % ENRICH_RX_SLOTS;
        _enrichRxCount--;
        queuedAfterPop = _enrichRxCount;
        xSemaphoreGive(_rxMutex);
        markBleRxDiag(124, snapLen);

        if (overflowed && _enrichmentInFlight) {
            _failEnrichment("enrich_rx_overflow");
            break;
        }

        if (snapLen > 0) {
            DLOG_INFO(TAG, "enrichment rx chunk cipher=%u queued=%u",
                      static_cast<unsigned>(snapLen),
                      static_cast<unsigned>(queuedAfterPop));
            uint8_t plain[ENRICH_RX_CHUNK_MAX];
            size_t plainLen = 0;
            markBleRxDiag(125, snapLen);
            if (_secureSession.decrypt(PHONE_SECURE_CHANNEL_ENRICHMENT,
                                       _payload->enrichRxScratch,
                                       snapLen,
                                       plain,
                                       sizeof(plain),
                                       plainLen)) {
                markBleRxDiag(126, plainLen);
                _handleEnrichmentPayload(plain, plainLen);
                markBleRxDiag(127, plainLen);
            } else {
                DLOG_WARN(TAG, "enrichment decrypt failed: %s",
                          _secureSession.lastError() ? _secureSession.lastError() : "-");
                if (_enrichmentInFlight) {
                    _failEnrichment("enrichment_decrypt_failed");
                }
                break;
            }
        }
    }
}

void BLEManager::_drainCommandRx() {
    markBleRxDiag(130);
    uint8_t snapshot[COMMAND_REQ_RX_MAX];
    size_t snapLen = 0;
    bool hadFrame = false;

    xSemaphoreTake(_rxMutex, portMAX_DELAY);
    markBleRxDiag(131);
    if (_commandReqRxPending && _commandReqRxLen > 0) {
        snapLen = _commandReqRxLen;
        if (snapLen > COMMAND_REQ_RX_MAX) {
            snapLen = COMMAND_REQ_RX_MAX;
        }
        memcpy(snapshot, _payload->commandReqRx, snapLen);
        _commandReqRxPending = false;
        _commandReqRxLen = 0;
        hadFrame = true;
        markBleRxDiag(132, snapLen);
    }
    xSemaphoreGive(_rxMutex);
    markBleRxDiag(133, snapLen);

    if (!hadFrame) {
        markBleRxDiag(138);
        return;
    }

    uint8_t plain[PHONE_COMMAND_REQ_FRAME_MAX];
    size_t plainLen = 0;
    markBleRxDiag(134, snapLen);
    if (!_secureSession.decrypt(PHONE_SECURE_CHANNEL_COMMAND,
                                snapshot, snapLen,
                                plain, sizeof(plain),
                                plainLen)) {
        DLOG_WARN(TAG, "command decrypt failed: %s",
                  _secureSession.lastError() ? _secureSession.lastError() : "-");
        markBleRxDiag(139, snapLen);
        return;
    }

    markBleRxDiag(135, plainLen);
    _handleCommandRequestPayload(plain, plainLen);
    markBleRxDiag(137, plainLen);
}

void BLEManager::_handleCommandRequestPayload(const uint8_t* data, size_t len) {
    markBleRxDiag(140, len);
    if (!_commandRespRemoteChar || !_commandRespRemoteChar->canWrite()) {
        DLOG_WARN(TAG, "command response char not writable; dropping reply");
        return;
    }

    size_t plainRespLen = 0;
    markBleRxDiag(141, len);
    if (!CommandDispatcher::dispatch(data, len,
                                     _payload->commandRespPlain,
                                     sizeof(_payload->commandRespPlain),
                                     plainRespLen)) {
        DLOG_WARN(TAG, "command dispatcher refused response (buffer too small)");
        return;
    }

    markBleRxDiag(142, plainRespLen);
    size_t secureLen = 0;
    markBleRxDiag(143, plainRespLen);
    if (!_secureSession.encrypt(PHONE_SECURE_CHANNEL_COMMAND,
                                _payload->commandRespPlain, plainRespLen,
                                _payload->commandRespSecure,
                                sizeof(_payload->commandRespSecure),
                                secureLen)) {
        DLOG_WARN(TAG, "command encrypt failed: %s",
                  _secureSession.lastError() ? _secureSession.lastError() : "-");
        return;
    }

    markBleRxDiag(144, secureLen);
    // The command envelope already carries an authenticated monotonic counter
    // plus opcode/requestId, and offload chunks are stateless by offset. Avoid
    // an ATT write-response round trip for every field-drain fragment; Android
    // exposes WRITE_NO_RESPONSE on this characteristic and the phone still
    // durably commits each record before Spectre advances its watermark.
    markBleRxDiag(145, secureLen);
    if (!_commandRespRemoteChar->writeValue(_payload->commandRespSecure, secureLen, false)) {
        DLOG_WARN(TAG, "command response write failed");
    }
    markBleRxDiag(146, secureLen);
}

void BLEManager::_clearBleRxQueues() {
    xSemaphoreTake(_rxMutex, portMAX_DELAY);
    _gpsRxPending = false;
    _gpsRxLen = 0;
    _gpsRxDrops = 0;
    _controlRxPending = false;
    _controlRxLen = 0;
    _controlRxDrops = 0;
    _enrichRxHead = 0;
    _enrichRxTail = 0;
    _enrichRxCount = 0;
    _enrichRxDrops = 0;
    _authRxPending = false;
    _authRxLen = 0;
    _authRxDrops = 0;
    _commandReqRxPending = false;
    _commandReqRxLen = 0;
    _commandReqRxDrops = 0;
    xSemaphoreGive(_rxMutex);
}

void BLEManager::_handleGpsPayload(const uint8_t* data, size_t len) {
    if (!data || len < PHONE_GPS_FRAME_SIZE) {
        DLOG_WARN(TAG, "gps frame too short");
        return;
    }

    PhoneGpsFrameV1 frame;
    memcpy(&frame, data, sizeof(frame));

    if (frame.version != COMPANION_PROTOCOL_VERSION) {
        DLOG_WARN(TAG, "gps frame version=%u", static_cast<unsigned>(frame.version));
        return;
    }

    if ((frame.flags & PHONE_GPS_FLAG_VALID) == 0) {
        // Time-only frames keep capture timestamps enrichable indoors.
        if ((frame.flags & PHONE_GPS_FLAG_TIME_TRUSTED) != 0 &&
            frame.epochUtc >= 1609459200UL) {  // >= 2021-01-01 UTC
            _lastGpsFixMs = millis();
            _gpsEpochAtFix = frame.epochUtc;
            _timeTrusted = true;
            _formatIso8601(frame.epochUtc, _gpsTimeIso, sizeof(_gpsTimeIso));
            // Adopt the phone's UTC immediately (see WIO path note).
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
    if (!data || len < PHONE_CONTROL_FRAME_SIZE) {
        DLOG_WARN(TAG, "control frame too short");
        return;
    }

    PhoneControlFrameV1 frame;
    memcpy(&frame, data, sizeof(frame));

    if (frame.version != COMPANION_PROTOCOL_VERSION) {
        DLOG_WARN(TAG, "control frame version=%u", static_cast<unsigned>(frame.version));
        return;
    }

    _wgActive = (frame.flags & PHONE_CTRL_FLAG_WG_ACTIVE) != 0;

    if ((frame.flags & PHONE_CTRL_FLAG_CANCEL) != 0) {
        if (_enrichmentRequestPending || _enrichmentInFlight) {
            DLOG_WARN(TAG, "enrichment cancelled by phone");
            _failEnrichment("phone_cancel");
        }

        companionRequestCancel();
        _cancelWireGuardConfirmation("WG remote cancel");
        return;
    }

    if ((frame.flags & PHONE_CTRL_FLAG_BATCH_RX) != 0) {
        _enrichmentBatchAcked = true;
        DLOG_INFO(TAG, "enrichment batch acknowledged by phone");
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

    if (_enrichmentRxLen + copyLen > sizeof(_payload->enrichmentRx)) {
        DLOG_WARN(TAG, "enrichment payload overflow");
        _failEnrichment("enrichment_payload_overflow");
        return;
    }

    memcpy(_payload->enrichmentRx + _enrichmentRxLen, data, copyLen);
    _enrichmentRxLen += copyLen;

    if (_enrichmentRxLen < expectedBytes) {
        return;
    }

    const size_t count = _enrichmentExpectedCount;
    for (size_t i = 0; i < count; ++i) {
        EnrichmentRecordWire record;
        memcpy(&record,
               _payload->enrichmentRx + (i * ENRICHMENT_RECORD_SIZE),
               ENRICHMENT_RECORD_SIZE);

        PendingEnrichment& out = _payload->enrichmentBatch[i];
        out.eventId = record.eventId;
        out.lat = static_cast<float>(record.latE7) / 10000000.0f;
        out.lon = static_cast<float>(record.lonE7) / 10000000.0f;
        out.alt = static_cast<float>(record.altCm) / 100.0f;
        out.accuracy = static_cast<float>(record.accuracyDm) / 10.0f;
        out.gpsEpochUtc = record.epochUtc;
        out.noData = (record.flags & PHONE_ENRICH_FLAG_NO_DATA) != 0;

        char tagBuf[sizeof(record.tag)];
        memcpy(tagBuf, record.tag, sizeof(tagBuf));
        tagBuf[sizeof(tagBuf) - 1] = '\0';
        strlcpy(out.tag, tagBuf, sizeof(out.tag));
    }

    _enrichmentAvailableCount = count;
    _enrichmentReady = true;
    _enrichmentInFlight = false;
    _enrichmentDeadlineMs = 0;

    const uint32_t waitMs =
        _enrichmentWaitStartMs ? (millis() - _enrichmentWaitStartMs) : 0U;
    _enrichmentXferMs = _enrichmentSendMs + waitMs;
    DLOG_INFO(TAG,
              "enrich_xfer_perf count=%u sendMs=%lu waitMs=%lu rxBytes=%u",
              static_cast<unsigned>(_enrichmentExpectedCount),
              static_cast<unsigned long>(_enrichmentSendMs),
              static_cast<unsigned long>(waitMs),
              static_cast<unsigned>(_enrichmentRxLen));

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

void BLEManager::ScanCallbacks::onDiscovered(
    const NimBLEAdvertisedDevice* advertisedDevice) {
    if (advertisedDevice &&
        _owner._matchesTarget(advertisedDevice) != TargetMatch::None) {
        _owner._onAdvertisedDevice(advertisedDevice);
    }
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
