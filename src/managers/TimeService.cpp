
#include "TimeService.h"

#include <WiFi.h>
#include <ctime>
#include <sys/time.h>

#include "../core/DebugLog.h"
#include "BLEManager.h"
#include "SettingsManager.h"
#include "MQTTManager.h"
#include "../core/SpectreState.h"

bool TimeService::begin() {
    if (!SETTINGS.isReady() && !SETTINGS.begin()) {
        DLOG_WARN("TIME", "Settings unavailable");
        return false;
    }

    applyTimezone(SETTINGS.get().timezone);
    _lastTickMs = 0;
    _lastNtpStartMs = 0;
    _ntpStarted = false;

    time_t now = time(nullptr);
    if (now >= static_cast<time_t>(MIN_VALID_EPOCH)) {
        syncFromEpoch(static_cast<uint32_t>(now), TIME_SOURCE_NTP);
    }

    _publishState(millis());
    DLOG_INFO("TIME", "Service ready");
    return true;
}

void TimeService::tick() {
    const uint32_t nowMs = millis();
    if (_lastTickMs != 0 && (nowMs - _lastTickMs) < TICK_INTERVAL_MS) return;
    _lastTickMs = nowMs;

    _syncFromGps(nowMs);

    const bool dumpActive = MQTT_MGR.isDumping();
    if (_source != TIME_SOURCE_GPS) {
        if (!dumpActive) {
            _syncFromNtp(nowMs);
        }
    }

    _publishState(nowMs);
}

const char* TimeService::sourceName() const {
    switch (_source) {
        case TIME_SOURCE_GPS: return "gps";
        case TIME_SOURCE_NTP: return "ntp";
        case TIME_SOURCE_NONE:
        default:              return "none";
    }
}

bool TimeService::applyTimezone(const char* timezone) {
    if (!timezone || !timezone[0]) return false;
    setenv("TZ", timezone, 1);
    tzset();
    DLOG_INFO("TIME", "Timezone applied: %s", timezone);
    return true;
}

bool TimeService::syncFromEpoch(uint32_t epochUtc, TimeSource source,
                                uint32_t referenceMs) {
    if (epochUtc < MIN_VALID_EPOCH) return false;

    const bool wasValid = _valid;
    const TimeSource prevSource = _source;
    const uint32_t prevEpochAtSync = _epochAtSync;
    const uint32_t prevMillisAtSync = _millisAtSync;

    if (source == TIME_SOURCE_GPS && wasValid) {
        const int64_t projectedEpoch =
            static_cast<int64_t>(prevEpochAtSync) +
            (static_cast<int64_t>(static_cast<int32_t>(referenceMs - prevMillisAtSync)) / 1000LL);
        if (static_cast<int64_t>(epochUtc) + GPS_BACKWARD_GUARD_S < projectedEpoch) {
            DLOG_WARN("TIME", "Reject stale GPS sync incoming=%lu current=%lu",
                      static_cast<unsigned long>(epochUtc),
                      static_cast<unsigned long>(projectedEpoch));
            return false;
        }
    }

    bool shouldLog = false;
    if (!wasValid || prevSource != source) {
        shouldLog = true;
    } else {
        const int64_t projectedEpoch =
            static_cast<int64_t>(prevEpochAtSync) +
            (static_cast<int64_t>(static_cast<int32_t>(referenceMs - prevMillisAtSync)) / 1000LL);
        const int64_t drift = static_cast<int64_t>(epochUtc) - projectedEpoch;
        if (drift > 2 || drift < -2) {
            shouldLog = true;
        }
    }

    timeval tv = {};
    tv.tv_sec = static_cast<time_t>(epochUtc);
    settimeofday(&tv, nullptr);

    _epochAtSync = epochUtc;
    _millisAtSync = referenceMs;
    _source = source;
    _valid = true;

    if (shouldLog) {
        char iso[24] = {};
        _formatIso8601(static_cast<time_t>(epochUtc), iso, sizeof(iso));
        DLOG_INFO("TIME", "Sync source=%s utc=%s", sourceName(), iso);
    }

    return true;
}

bool TimeService::formatNowIso(char* out, size_t len) const {
    return formatIsoForMillis(millis(), out, len);
}

bool TimeService::formatNowLocal(char* out, size_t len) const {
    return formatLocalForMillis(millis(), out, len);
}

bool TimeService::formatIsoForMillis(uint32_t monotonicMs, char* out, size_t len) const {
    if (!out || len == 0) return false;

    if (!_valid) {
        out[0] = '\0';
        return false;
    }

    const int64_t epoch = _epochForMillis(monotonicMs);
    if (epoch < 0) {
        out[0] = '\0';
        return false;
    }
    _formatIso8601(static_cast<time_t>(epoch), out, len);
    return true;
}

bool TimeService::formatLocalForMillis(uint32_t monotonicMs, char* out, size_t len) const {
    if (!out || len == 0) return false;
    if (!_valid) {
        strlcpy(out, "--:--:--", len);
        return false;
    }

    const int64_t epoch = _epochForMillis(monotonicMs);
    if (epoch < 0) {
        strlcpy(out, "--:--:--", len);
        return false;
    }

    _formatLocalClock(static_cast<time_t>(epoch), out, len);
    return true;
}

String TimeService::dayStampForMillis(uint32_t monotonicMs) const {
    if (!_valid) {
        return String((monotonicMs / 1000UL) / 86400UL);
    }

    const int64_t epoch = _epochForMillis(monotonicMs);
    if (epoch < 0) return String("0");

    time_t raw = static_cast<time_t>(epoch);
    struct tm localTm = {};
    localtime_r(&raw, &localTm);

    char buf[16] = {};
    strftime(buf, sizeof(buf), "%Y%m%d", &localTm);
    return String(buf);
}

void TimeService::_syncFromGps(uint32_t nowMs) {
    uint32_t gpsEpoch = 0;
    if (!BLE_MGR.getBestTimeEpoch(gpsEpoch)) return;

    if (!_valid || _source != TIME_SOURCE_GPS) {
        syncFromEpoch(gpsEpoch, TIME_SOURCE_GPS, nowMs);
        return;
    }

    const int64_t currentEpoch = _epochForMillis(nowMs);
    if (currentEpoch < 0) {
        syncFromEpoch(gpsEpoch, TIME_SOURCE_GPS, nowMs);
        return;
    }

    const int64_t drift = currentEpoch - static_cast<int64_t>(gpsEpoch);
    if (drift > 2 || drift < -2) {
        syncFromEpoch(gpsEpoch, TIME_SOURCE_GPS, nowMs);
    }
}

void TimeService::_syncFromNtp(uint32_t nowMs) {
    if (WiFi.status() != WL_CONNECTED) {
        _ntpStarted = false;
        return;
    }

    const RuntimeSettings& settings = SETTINGS.get();
    if (!_ntpStarted || (nowMs - _lastNtpStartMs) > NTP_RESTART_MS) {
        configTzTime(settings.timezone, settings.ntpServer1, settings.ntpServer2);
        _ntpStarted = true;
        _lastNtpStartMs = nowMs;
        DLOG_INFO("TIME", "NTP requested via %s / %s",
                  settings.ntpServer1, settings.ntpServer2);
    }

    const time_t now = time(nullptr);
    if (now >= static_cast<time_t>(MIN_VALID_EPOCH)) {
        syncFromEpoch(static_cast<uint32_t>(now), TIME_SOURCE_NTP, nowMs);
    }
}

void TimeService::_publishState(uint32_t referenceMs) const {
    char iso[24] = {};
    char local[24] = {};
    const bool isoOk = formatIsoForMillis(referenceMs, iso, sizeof(iso));
    const bool localOk = formatLocalForMillis(referenceMs, local, sizeof(local));
    if (!isoOk) {
        iso[0] = '\0';
    }
    if (!localOk) {
        strlcpy(local, "--:--:--", sizeof(local));
    }

    STATE_WRITE_BEGIN();
    g_state.timeValid = _valid;
    strlcpy(g_state.timeSource, sourceName(), sizeof(g_state.timeSource));
    strlcpy(g_state.timeISO, iso, sizeof(g_state.timeISO));
    strlcpy(g_state.timeLocal, local, sizeof(g_state.timeLocal));
    STATE_WRITE_END();
}

int64_t TimeService::_epochForMillis(uint32_t monotonicMs) const {
    if (!_valid) return -1;
    const int64_t deltaMs =
        static_cast<int64_t>(static_cast<int32_t>(monotonicMs - _millisAtSync));
    return static_cast<int64_t>(_epochAtSync) + (deltaMs / 1000LL);
}

void TimeService::_formatIso8601(time_t epochUtc, char* out, size_t len) {
    if (!out || len == 0) return;
    struct tm utc = {};
    gmtime_r(&epochUtc, &utc);
    strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

void TimeService::_formatLocalClock(time_t epochUtc, char* out, size_t len) {
    if (!out || len == 0) return;
    struct tm localTm = {};
    localtime_r(&epochUtc, &localTm);
    strftime(out, len, "%Y-%m-%d %H:%M:%S", &localTm);
}

