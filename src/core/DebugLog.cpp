

#include "DebugLog.h"
#include "SpectreState.h"
#include "../managers/RadioArbiter.h"
#include <LittleFS.h>
#include <stdarg.h>
#include <esp_heap_caps.h>

namespace {
bool debugLogFlushRisky() {
    switch (RADIO_ARB.currentOwner()) {
        case RADIO_WIFI_CAPTURE:
        case RADIO_WIFI_SCAN:
        case RADIO_WIFI_PMKID:
        case RADIO_WIFI_UPLOAD:
            return true;
        case RADIO_NONE:
        case RADIO_BLE_TEXT:
        case RADIO_BLE_GPS:
        default:
            return false;
    }
}
}

// ── Static member definitions ─────────────────────────────────
char     DebugLog::_fallbackBuf[DebugLog::FALLBACK_BUF_SIZE] = {};
char*    DebugLog::_buf         = nullptr;
int      DebugLog::_bufSize     = DebugLog::BUF_SIZE;
int      DebugLog::_head        = 0;
int      DebugLog::_tail        = 0;
int      DebugLog::_used        = 0;
int      DebugLog::_lineCount   = 0;
bool     DebugLog::_ready       = false;
bool     DebugLog::_serialEnabled = false;
char     DebugLog::_serialMinLevel = DEBUG_LEVEL_INFO;
uint32_t DebugLog::_serialAreaMask = DEBUG_AREA_OPERATORS;
uint32_t DebugLog::_lastFlush   = 0;
portMUX_TYPE DebugLog::_mux     = portMUX_INITIALIZER_UNLOCKED;

// Default to OFF until applyProfile() runs. _minLevelRank=99 makes enabled()
// reject every level even before the profile branch is reached, so anything
// that logs before init produces no churn.
DebugProfile DebugLog::_profile             = DEBUG_PROFILE_OFF;
int          DebugLog::_minLevelRank        = 99;
uint32_t     DebugLog::_subsystemMask       = 0;
bool         DebugLog::_areaMaskAll         = false;
uint32_t     DebugLog::_autoFlushIntervalMs = 0;

const char DebugLog::LOG_PATH[] = "/logs/debug.log";
const char DebugLog::LOG_BAK[]  = "/logs/debug.log.1";

void DebugLog::applyProfile(DebugProfile profile, uint32_t subsystemMask) {
    _profile = profile;
    _subsystemMask = subsystemMask;
    switch (profile) {
        case DEBUG_PROFILE_OFF:
            _minLevelRank = 99;          // reject everything
            _areaMaskAll = false;
            _autoFlushIntervalMs = 0;    // never auto-flush
            break;
        case DEBUG_PROFILE_RUN:
            _minLevelRank = 2;           // WARN+
            _areaMaskAll = true;         // any subsystem
            _autoFlushIntervalMs = 0;    // no auto-flush in field
            break;
        case DEBUG_PROFILE_DEBUG:
            _minLevelRank = 1;           // INFO+
            _areaMaskAll = false;
            _autoFlushIntervalMs = 120000UL;
            break;
        case DEBUG_PROFILE_DEV:
            _minLevelRank = 0;           // VERBOSE+
            _areaMaskAll = false;
            _autoFlushIntervalMs = 30000UL;
            break;
    }
}

void DebugLog::begin() {
    if (_ready) {
        return;
    }

    if (!LittleFS.exists("/logs")) {
        LittleFS.mkdir("/logs");
    }

    _ensureBuffer();
    char prior[FALLBACK_BUF_SIZE] = {};
    int priorLen = 0;
    if (_used > 0 && _buf != nullptr) {
        const int priorCapacity = _bufSize - 1;
        priorLen = _used;
        if (priorCapacity > 0 && priorLen > priorCapacity) {
            priorLen = priorCapacity;
        }
        if (priorLen > static_cast<int>(sizeof(prior) - 1)) {
            priorLen = sizeof(prior) - 1;
        }
        if (priorLen > 0 && priorCapacity > 0) {
            for (int i = 0; i < priorLen; ++i) {
                prior[i] = _buf[(_head + i) % priorCapacity];
            }
        }
    }

    char* nextBuf = (char*)ps_malloc(BUF_SIZE);
    if (!nextBuf) {
        Serial.println("[DLOG] PSRAM alloc failed, using fallback");
        _buf = _fallbackBuf;
        _bufSize = sizeof(_fallbackBuf);
    } else {
        _buf = nextBuf;
        _bufSize = BUF_SIZE;
        _head = 0;
        _tail = 0;
        _used = 0;
        if (priorLen > 0) {
            memcpy(_buf, prior, priorLen);
            _head = 0;
            _tail = priorLen % (_bufSize - 1);
            _used = priorLen;
            if (priorLen < _bufSize - 1) {
                _buf[_tail] = '\0';
            }
        } else {
            _buf[0] = '\0';
        }
    }
    _ready = true;
    log(DEBUG_LEVEL_INFO, "DLOG", "DebugLog started - build %s %s",
        __DATE__, __TIME__);
}

void DebugLog::configureUsbSerial(bool enabled, char minLevel, uint32_t areaMask) {
    _serialEnabled = enabled;
    _serialMinLevel = sanitizeDebugLevel(minLevel);
    _serialAreaMask = areaMask;
}

void DebugLog::setUsbSerialEnabled(bool enabled) {
    _serialEnabled = enabled;
}

void DebugLog::setUsbSerialMinLevel(char minLevel) {
    _serialMinLevel = sanitizeDebugLevel(minLevel);
}

void DebugLog::setUsbSerialAreaMask(uint32_t areaMask) {
    _serialAreaMask = areaMask;
}

void DebugLog::log(char level, const char* tag,
                   const char* fmt, ...) {
    // Defensive recheck: callers that bypass DLOG_* macros still get gated.
    if (!enabled(level, tag)) {
        return;
    }

    _ensureBuffer();
    char line[256];
    char msg[192];

    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    uint32_t ms = millis();
    snprintf(line, sizeof(line), "[%lu][%c][%s] %s\r\n",
             ms, level, tag, msg);

    if (_shouldMirrorToUsbSerial(level, tag)) {
        Serial.print(line);
    }

    portENTER_CRITICAL(&_mux);
    const int capacity = _bufSize - 1;
    int len = strlen(line);
    if (len > capacity) {
        len = capacity;
    }

    // Drop oldest bytes until there is room.
    while (_used + len > capacity) {
        _head = (_head + 1) % capacity;
        _used--;
    }

    for (int i = 0; i < len; ++i) {
        _buf[_tail] = line[i];
        _tail = (_tail + 1) % capacity;
    }

    _used += len;

    // Keep a terminator for simple debugging only when there is spare room.
    // Do not rely on _buf being linear anymore.
    if (_used < capacity) {
        _buf[_tail] = '\0';
    }

    _lineCount++;
    portEXIT_CRITICAL(&_mux);

    // Profile-controlled auto-flush. OFF/RUN have _autoFlushIntervalMs=0 so
    // no LittleFS writes happen on the routine log path. DEBUG/DEV throttle
    // and skip while an upload is active or a risky radio owner holds the
    // bus, keeping file IO out of the upload/capture windows.
    if (_autoFlushIntervalMs == 0) {
        return;
    }

    bool uploadActive = false;
    STATE_READ_BEGIN();
    uploadActive = g_state.uploadActive;
    STATE_READ_END();

    uint32_t now = millis();
    if (_ready &&
        !uploadActive &&
        !debugLogFlushRisky() &&
        now - _lastFlush > _autoFlushIntervalMs) {
        flush();
    }
}

void DebugLog::logCrash(const char* fmt, ...) {
    char msg[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    // Crash records bypass the profile gate (OFF/RUN must still capture
    // them) so we cannot route through log(). Hand-roll the same ring
    // append + serial mirror that log() does, then force a flush.
    char line[256];
    uint32_t ms = millis();
    snprintf(line, sizeof(line), "[%lu][E][CRASH] %s\r\n", ms, msg);

    if (_serialEnabled) {
        Serial.print(line);
    }

    _ensureBuffer();
    portENTER_CRITICAL(&_mux);
    const int capacity = _bufSize - 1;
    int len = strlen(line);
    if (len > capacity) {
        len = capacity;
    }
    while (_used + len > capacity) {
        _head = (_head + 1) % capacity;
        _used--;
    }
    for (int i = 0; i < len; ++i) {
        _buf[_tail] = line[i];
        _tail = (_tail + 1) % capacity;
    }
    _used += len;
    if (_used < capacity) {
        _buf[_tail] = '\0';
    }
    _lineCount++;
    portEXIT_CRITICAL(&_mux);

    // Force immediate flush on crash
    flush();
}

void DebugLog::flush() {
    if (!_ready || _used == 0) return;

    char snapshot[BUF_SIZE];
    int snapLen = _used;

    portENTER_CRITICAL(&_mux);
    const int capacity = _bufSize - 1;
    snapLen = _used;
    for (int i = 0; i < snapLen; ++i) {
        snapshot[i] = _buf[(_head + i) % capacity];
    }
    snapshot[snapLen] = '\0';
    _head = 0;
    _tail = 0;
    _used = 0;
    portEXIT_CRITICAL(&_mux);

    _lastFlush = millis();

    // Check if rotation needed
    File f = LittleFS.open(LOG_PATH, "r");
    if (f) {
        size_t sz = f.size();
        f.close();
        if (sz > MAX_LOG_KB * 1024) {
            _rotateLogs();
        }
    }

    // Append to log file
    _writeToFile(snapshot);
}

void DebugLog::_writeToFile(const char* data) {
    File f = LittleFS.open(LOG_PATH, FILE_APPEND);
    if (!f) return;
    f.print(data);
    f.close();
}

void DebugLog::_rotateLogs() {
    // Remove old backup
    if (LittleFS.exists(LOG_BAK)) {
        LittleFS.remove(LOG_BAK);
    }
    // Rename current to backup
    LittleFS.rename(LOG_PATH, LOG_BAK);
}

void DebugLog::dumpToSerial() {
    Serial.println("=== DEBUG LOG ===");
    File f = LittleFS.open(LOG_PATH, "r");
    if (f) {
        while (f.available()) {
            Serial.write(f.read());
        }
        f.close();
    }
    // Also dump in-memory buffer
    if (_used > 0) {
        Serial.println("--- PENDING BUFFER ---");

        char snapshot[BUF_SIZE];
        int snapLen = 0;

        portENTER_CRITICAL(&_mux);
        const int capacity = _bufSize - 1;
        snapLen = _used;
        for (int i = 0; i < snapLen; ++i) {
            snapshot[i] = _buf[(_head + i) % capacity];
        }
        snapshot[snapLen] = '\0';
        portEXIT_CRITICAL(&_mux);

        Serial.print(snapshot);
    }
    Serial.println("=== END LOG ===");
}

void DebugLog::_ensureBuffer() {
    if (_buf != nullptr) {
        return;
    }

    _buf = _fallbackBuf;
    _bufSize = sizeof(_fallbackBuf);
    _head = 0;
    _tail = 0;
    _used = 0;
    _buf[0] = '\0';
}

bool DebugLog::_shouldMirrorToUsbSerial(char level, const char* tag) {
    if (!_serialEnabled) {
        return false;
    }

    if (_levelRank(level) < _levelRank(_serialMinLevel)) {
        return false;
    }

    return (_serialAreaMask & debugAreaMaskForTag(tag)) != 0;
}

int DebugLog::_levelRank(char level) {
    return debugLevelRank(level);
}



