#pragma once
#include <Arduino.h>
#include "DebugConfig.h"

#define DLOG_DEBUG(tag, fmt, ...) DebugLog::log('D', tag, fmt, ##__VA_ARGS__)
#define DLOG_INFO(tag, fmt, ...)  DebugLog::log('I', tag, fmt, ##__VA_ARGS__)
#define DLOG_WARN(tag, fmt, ...)  DebugLog::log('W', tag, fmt, ##__VA_ARGS__)
#define DLOG_ERROR(tag, fmt, ...) DebugLog::log('E', tag, fmt, ##__VA_ARGS__)
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

    static void _ensureBuffer();
    static bool _shouldMirrorToUsbSerial(char level, const char* tag);
    static int _levelRank(char level);
    static uint32_t _areaMaskForTag(const char* tag);
    static void _writeToFile(const char* line);
    static void _rotateLogs();
};



