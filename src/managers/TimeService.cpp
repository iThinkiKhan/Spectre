

#include "TimeService.h"

#include <WiFi.h>
#include <ctime>
#include <sys/time.h>
#include <algorithm>

#include "../core/DebugLog.h"
#include "BLEManager.h"
#include "PhoneTransportRouter.h"
#include "SettingsManager.h"
#include "MQTTManager.h"
#include "WioNrfAccessory.h"
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
    _utcAccurate = false;

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
    if (!_utcAccurate && _source != TIME_SOURCE_GPS) {
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
    _utcAccurate = true;

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

bool TimeService::epochForMillis(uint32_t monotonicMs, uint32_t& epochUtc) const {
    const int64_t epoch = _epochForMillis(monotonicMs);
    if (epoch < static_cast<int64_t>(MIN_VALID_EPOCH) ||
        epoch > static_cast<int64_t>(UINT32_MAX)) {
        return false;
    }

    epochUtc = static_cast<uint32_t>(epoch);
    return true;
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

bool TimeService::acquireUtcFromSavedWiFi(uint32_t totalTimeoutMs) {
    if (_utcAccurate) {
        return true;
    }
    if (!SETTINGS.isReady() && !SETTINGS.begin()) {
        DLOG_WARN("TIME", "Quick NTP skipped: settings unavailable");
        return false;
    }

    const RuntimeSettings settings = SETTINGS.snapshot();
    if (settings.wifiNetworkCount == 0) {
        DLOG_WARN("TIME", "Quick NTP skipped: no saved WiFi networks");
        return false;
    }

    applyTimezone(settings.timezone);

    const uint32_t startMs = millis();
    const uint32_t deadlineMs = startMs + totalTimeoutMs;

    auto timeRemaining = [&]() -> uint32_t {
        const int32_t remaining =
            static_cast<int32_t>(deadlineMs - millis());
        return remaining > 0 ? static_cast<uint32_t>(remaining) : 0U;
    };

    auto waitForNtp = [&](uint32_t waitMs) -> bool {
        configTzTime(settings.timezone, settings.ntpServer1, settings.ntpServer2);
        _ntpStarted = true;
        _lastNtpStartMs = millis();
        DLOG_INFO("TIME", "Quick NTP requested via %s / %s",
                  settings.ntpServer1,
                  settings.ntpServer2);

        const uint32_t ntpDeadline = millis() + waitMs;
        while (static_cast<int32_t>(millis() - ntpDeadline) < 0) {
            const time_t now = time(nullptr);
            if (now >= static_cast<time_t>(MIN_VALID_EPOCH)) {
                return syncFromEpoch(static_cast<uint32_t>(now),
                                     TIME_SOURCE_NTP,
                                     millis());
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return false;
    };

    if (WiFi.status() == WL_CONNECTED) {
        const uint32_t waitMs = std::min(QUICK_NTP_WAIT_MS, timeRemaining());
        if (waitMs > 0 && waitForNtp(waitMs)) {
            WiFi.disconnect(false, false);
            _publishState(millis());
            return true;
        }
    }

    for (uint8_t i = 0; i < settings.wifiNetworkCount && timeRemaining() > 0; ++i) {
        const WiFiCredential& network = settings.wifiNetworks[i];
        if (!network.ssid[0]) {
            continue;
        }

        WiFi.disconnect(false, false);
        vTaskDelay(pdMS_TO_TICKS(40));
        WiFi.setSleep(false);
        WiFi.begin(network.ssid, network.password);

        const uint32_t connectBudget =
            std::min(QUICK_WIFI_CONNECT_SLICE_MS, timeRemaining());
        const uint32_t connectDeadline = millis() + connectBudget;
        DLOG_INFO("TIME", "Quick NTP WiFi connect ssid=%s timeout=%lums",
                  network.ssid,
                  static_cast<unsigned long>(connectBudget));

        while (WiFi.status() != WL_CONNECTED &&
               static_cast<int32_t>(millis() - connectDeadline) < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (WiFi.status() != WL_CONNECTED) {
            DLOG_WARN("TIME", "Quick NTP WiFi connect failed ssid=%s",
                      network.ssid);
            continue;
        }

        const uint32_t waitMs = std::min(QUICK_NTP_WAIT_MS, timeRemaining());
        if (waitMs > 0 && waitForNtp(waitMs)) {
            WiFi.disconnect(false, false);
            _publishState(millis());
            DLOG_INFO("TIME", "Quick NTP UTC acquired ssid=%s",
                      network.ssid);
            return true;
        }

        DLOG_WARN("TIME", "Quick NTP wait expired ssid=%s", network.ssid);
    }

    WiFi.disconnect(false, false);
    _publishState(millis());
    DLOG_WARN("TIME", "Quick NTP UTC unavailable within %lums",
              static_cast<unsigned long>(totalTimeoutMs));
    return false;
}

void TimeService::_syncFromGps(uint32_t nowMs) {
    uint32_t gpsEpoch = 0;
    if (!PHONE_XPORT.getBestTimeEpoch(gpsEpoch)) return;

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
    g_state.utcAccurate = _utcAccurate;
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
