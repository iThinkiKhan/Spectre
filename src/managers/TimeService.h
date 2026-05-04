

#pragma once

#include <Arduino.h>
#include <ctime>

enum TimeSource : uint8_t {
    TIME_SOURCE_NONE = 0,
    TIME_SOURCE_GPS,
    TIME_SOURCE_NTP
};

class TimeService {
public:
    static TimeService& getInstance() {
        static TimeService instance;
        return instance;
    }

    bool begin();
    void tick();

    bool isTimeValid() const { return _valid; }
    TimeSource source() const { return _source; }
    const char* sourceName() const;

    bool applyTimezone(const char* timezone);
    bool syncFromEpoch(uint32_t epochUtc, TimeSource source,
                       uint32_t referenceMs = millis());

    bool formatNowIso(char* out, size_t len) const;
    bool formatNowLocal(char* out, size_t len) const;
    bool formatIsoForMillis(uint32_t monotonicMs, char* out, size_t len) const;
    bool formatLocalForMillis(uint32_t monotonicMs, char* out, size_t len) const;
    String dayStampForMillis(uint32_t monotonicMs) const;

private:
    TimeService() = default;

    static constexpr uint32_t MIN_VALID_EPOCH = 1704067200UL;
    static constexpr uint32_t TICK_INTERVAL_MS = 1000UL;
    static constexpr uint32_t NTP_RESTART_MS = 300000UL;

    void _syncFromGps(uint32_t nowMs);
    void _syncFromNtp(uint32_t nowMs);
    void _publishState(uint32_t referenceMs) const;
    int64_t _epochForMillis(uint32_t monotonicMs) const;
    static void _formatIso8601(time_t epochUtc, char* out, size_t len);
    static void _formatLocalClock(time_t epochUtc, char* out, size_t len);

    bool _valid = false;
    TimeSource _source = TIME_SOURCE_NONE;
    uint32_t _epochAtSync = 0;
    uint32_t _millisAtSync = 0;
    uint32_t _lastTickMs = 0;
    uint32_t _lastNtpStartMs = 0;
    bool _ntpStarted = false;
};

#define TIME_SVC TimeService::getInstance()



