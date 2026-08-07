

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
    bool hasAccurateUtc() const { return _utcAccurate; }
    TimeSource source() const { return _source; }
    const char* sourceName() const;

    bool applyTimezone(const char* timezone);
    bool syncFromEpoch(uint32_t epochUtc, TimeSource source,
                       uint32_t referenceMs = millis());

    bool formatNowIso(char* out, size_t len) const;
    bool formatNowLocal(char* out, size_t len) const;
    bool epochForMillis(uint32_t monotonicMs, uint32_t& epochUtc) const;
    bool formatIsoForMillis(uint32_t monotonicMs, char* out, size_t len) const;
    // Format an ABSOLUTE UTC epoch (seconds) to ISO-8601. Unlike
    // formatIsoForMillis, this does NOT project through the live clock, so it
    // stays correct across reboots — use it for the reboot-safe per-record
    // epoch (SegmentHeaderV2.createdEpochUtc + delta) rather than a millis().
    bool formatIsoForEpoch(uint32_t epochUtc, char* out, size_t len) const;
    bool formatLocalForMillis(uint32_t monotonicMs, char* out, size_t len) const;
    String dayStampForMillis(uint32_t monotonicMs) const;
    bool acquireUtcFromSavedWiFi(uint32_t totalTimeoutMs);

    // Observability for trusted-clock acquisition. A boot that never gets a
    // clock leaves every captured record un-enrichable, and the failure was
    // historically invisible (TIME logs are debug-area gated). These expose the
    // most recent NTP-acquisition outcome so the always-on [HEALTH] block, the
    // `time` serial command, and the BOOT_SUMMARY screen can surface it.
    const char* lastAttemptReason() const { return _lastAttemptReason; }
    bool everAttempted() const { return _lastAttemptMs != 0; }
    uint32_t lastAttemptAgeMs() const {
        return _lastAttemptMs ? (millis() - _lastAttemptMs) : 0U;
    }
    // Record an acquisition outcome/skip from callers that gate NTP before it
    // reaches acquireUtcFromSavedWiFi (e.g. a capture-safety or wedge skip), so
    // the reason still shows up in the health heartbeat instead of vanishing.
    void recordAttempt(const char* reason);

private:
    TimeService() = default;

    static constexpr uint32_t MIN_VALID_EPOCH = 1704067200UL;
    static constexpr uint32_t TICK_INTERVAL_MS = 1000UL;
    static constexpr uint32_t NTP_RESTART_MS = 300000UL;
    static constexpr uint32_t GPS_BACKWARD_GUARD_S = 2UL;
    static constexpr uint32_t QUICK_WIFI_CONNECT_SLICE_MS = 4500UL;
    static constexpr uint32_t QUICK_NTP_WAIT_MS = 3500UL;

    void _syncFromGps(uint32_t nowMs);
    void _syncFromNtp(uint32_t nowMs);
    void _publishState(uint32_t referenceMs) const;
    int64_t _epochForMillis(uint32_t monotonicMs) const;
    static void _formatIso8601(time_t epochUtc, char* out, size_t len);
    static void _formatLocalClock(time_t epochUtc, char* out, size_t len);

    bool _valid = false;
    bool _utcAccurate = false;
    TimeSource _source = TIME_SOURCE_NONE;
    uint32_t _epochAtSync = 0;
    uint32_t _millisAtSync = 0;
    uint32_t _lastTickMs = 0;
    uint32_t _lastNtpStartMs = 0;
    bool _ntpStarted = false;

    char _lastAttemptReason[48] = "none";
    uint32_t _lastAttemptMs = 0;
};

#define TIME_SVC TimeService::getInstance()

