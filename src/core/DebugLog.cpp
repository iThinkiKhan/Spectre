

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

const char DebugLog::LOG_PATH[] = "/logs/debug.log";
const char DebugLog::LOG_BAK[]  = "/logs/debug.log.1";

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

    bool uploadActive = false;
    STATE_READ_BEGIN();
    uploadActive = g_state.uploadActive;
    STATE_READ_END();

    // Auto-flush every 30 seconds if ready, but keep LittleFS writes out of
    // the active upload window and risky WiFi radio ownership windows.
    // Manual/crash flushes still go through flush().
    uint32_t now = millis();
    if (_ready &&
        !uploadActive &&
        !debugLogFlushRisky() &&
        now - _lastFlush > 30000) {
        flush();
    }
}

void DebugLog::logCrash(const char* fmt, ...) {
    char msg[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    log('E', "CRASH", "%s", msg);

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

    return (_serialAreaMask & _areaMaskForTag(tag)) != 0;
}

int DebugLog::_levelRank(char level) {
    switch (sanitizeDebugLevel(level)) {
        case DEBUG_LEVEL_ERROR:
            return 3;
        case DEBUG_LEVEL_WARN:
            return 2;
        case DEBUG_LEVEL_INFO:
            return 1;
        case DEBUG_LEVEL_VERBOSE:
        default:
            return 0;
    }
}

uint32_t DebugLog::_areaMaskForTag(const char* tag) {
    if (!tag || !tag[0]) {
        return DEBUG_AREA_GENERAL;
    }

    if (strcmp(tag, "SYS") == 0 ||
        strcmp(tag, "CORE") == 0 ||
        strcmp(tag, "STACK") == 0 ||
        strcmp(tag, "HEAP") == 0 ||
        strcmp(tag, "BTN") == 0) {
        return DEBUG_AREA_CORE;
    }
    if (strcmp(tag, "SETTINGS") == 0) {
        return DEBUG_AREA_SETTINGS;
    }
    if (strcmp(tag, "STOR") == 0 ||
        strcmp(tag, "STORAGE") == 0) {
        return DEBUG_AREA_STORAGE;
    }
    if (strcmp(tag, "TIME") == 0) {
        return DEBUG_AREA_TIME;
    }
    if (strcmp(tag, "RADIO") == 0 ||
        strcmp(tag, "LORA") == 0) {
        return DEBUG_AREA_RADIO;
    }
    if (strcmp(tag, "WIFI") == 0 ||
        strcmp(tag, "ANT") == 0 ||
        strcmp(tag, "DRONE") == 0) {
        return DEBUG_AREA_WIFI;
    }
    if (strcmp(tag, "BLE") == 0) {
        return DEBUG_AREA_BLE;
    }
    if (strcmp(tag, "MQTT") == 0) {
        return DEBUG_AREA_MQTT;
    }
    if (strcmp(tag, "EXPORT") == 0) {
        return DEBUG_AREA_EXPORT;
    }
    if (strcmp(tag, "GPS") == 0) {
        return DEBUG_AREA_GPS;
    }
    if (strcmp(tag, "MODE") == 0) {
        return DEBUG_AREA_MODE;
    }

    return DEBUG_AREA_GENERAL;
}



