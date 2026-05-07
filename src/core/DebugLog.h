#pragma once
#include <Arduino.h>
#include "DebugConfig.h"

// DLOG_* macros wrap calls in a cheap profile/subsystem gate. When the gate
// rejects, the format args are never evaluated, so disabled callsites cost
// only an inlined level/mask compare — no vsnprintf, ring write, serial
// write, or auto-flush side effects.
#define DLOG_DEBUG(tag, fmt, ...) \
    do { if (DebugLog::enabled('D', tag)) DebugLog::log('D', tag, fmt, ##__VA_ARGS__); } while (0)
#define DLOG_INFO(tag, fmt, ...) \
    do { if (DebugLog::enabled('I', tag)) DebugLog::log('I', tag, fmt, ##__VA_ARGS__); } while (0)
#define DLOG_WARN(tag, fmt, ...) \
    do { if (DebugLog::enabled('W', tag)) DebugLog::log('W', tag, fmt, ##__VA_ARGS__); } while (0)
#define DLOG_ERROR(tag, fmt, ...) \
    do { if (DebugLog::enabled('E', tag)) DebugLog::log('E', tag, fmt, ##__VA_ARGS__); } while (0)
#define DLOG_CRASH(fmt, ...)      DebugLog::logCrash(fmt, ##__VA_ARGS__)

class DebugLog {
public:
    static void begin();
    static void configureUsbSerial(bool enabled,
                                   char minLevel = DEBUG_LEVEL_INFO,
                                   uint32_t areaMask = DEBUG_AREA_OPERATORS);
    static void setUsbSerialEnabled(bool enabled);
    static void setUsbSerialMinLevel(char minLevel);
    static void setUsbSerialAreaMask(uint32_t areaMask);
    static bool usbSerialEnabled() { return _serialEnabled; }
    static char usbSerialMinLevel() { return _serialMinLevel; }
    static uint32_t usbSerialAreaMask() { return _serialAreaMask; }

    // Apply a debug profile and the compile-time-resolved subsystem mask.
    // Profile policy wins: OFF rejects all, RUN passes WARN/ERROR for any
    // subsystem regardless of mask, DEBUG/DEV honor the mask. Safe to call
    // before begin(); the gate is consulted on every log() call.
    static void applyProfile(DebugProfile profile, uint32_t subsystemMask);
    static DebugProfile profile() { return _profile; }
    static uint32_t subsystemMask() { return _subsystemMask; }

    // Cheap early gate. Inline so disabled callsites cost only a few loads
    // and compares. Returns false before applyProfile() runs (default OFF).
    static inline bool enabled(char level, const char* tag) {
        if (_profile == DEBUG_PROFILE_OFF) return false;
        if (debugLevelRank(level) < _minLevelRank) return false;
        if (_areaMaskAll) return true;
        return (_subsystemMask & debugAreaMaskForTag(tag)) != 0;
    }

    static void log(char level, const char* tag,
                    const char* fmt, ...);
    static void logCrash(const char* fmt, ...);
    static void flush();
    static void dumpToSerial();
    static int  getLineCount() { return _lineCount; }

private:
    static const int  BUF_SIZE    = 2048;
    static const int  FALLBACK_BUF_SIZE = 512;
    static const int  MAX_LOG_KB  = 64;
    static const char LOG_PATH[];
    static const char LOG_BAK[];

    static char     _fallbackBuf[FALLBACK_BUF_SIZE];
    static char*    _buf;
    static int      _bufSize;
    static int      _head;   // oldest byte
    static int      _tail;   // next write byte
    static int      _used;   // bytes used
    static int      _lineCount;
    static bool     _ready;
    static bool     _serialEnabled;
    static char     _serialMinLevel;
    static uint32_t _serialAreaMask;
    static uint32_t _lastFlush;
    static portMUX_TYPE _mux;

    static DebugProfile _profile;
    static int          _minLevelRank;
    static uint32_t     _subsystemMask;
    static bool         _areaMaskAll;
    static uint32_t     _autoFlushIntervalMs;

    static void _ensureBuffer();
    static bool _shouldMirrorToUsbSerial(char level, const char* tag);
    static int _levelRank(char level);
    static void _writeToFile(const char* line);
    static void _rotateLogs();
};



