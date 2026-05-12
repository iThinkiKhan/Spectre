


#include "PowerManager.h"

#include <math.h>

#include "../config.h"
#include "../core/DebugLog.h"
#include "SettingsManager.h"

namespace {

constexpr float FILTER_ALPHA = 0.22f;
constexpr float CURRENT_EMA_ALPHA = 0.35f;
constexpr uint32_t USB_ENTER_DWELL_MS = 45000UL;
constexpr uint32_t USB_EXIT_DWELL_MS = 75000UL;
constexpr uint32_t USB_EXIT_FAST_DWELL_MS = 15000UL;
constexpr uint32_t TREND_WINDOW_MS = 30000UL;
constexpr uint32_t CURRENT_WINDOW_MS = 90000UL;
// Trend must span at least this long before it is trusted to cap runtime
// estimates or contribute to critical-reserve decisions. This prevents the
// steep voltage sag at boot-up (battery suddenly loaded) from producing a
// spuriously short runtime estimate and triggering a premature critical state.
constexpr uint32_t TREND_STABLE_MIN_MS = 120000UL;
constexpr float MIN_SOC_DELTA_FOR_CURRENT = 0.45f;
// Calibrated against an observed ~2 h runtime on the stock 1100 mAh cell —
// the device draws far more in promiscuous-heavy use than the original 135 mA
// guess implied. POWER_AGGRESSIVE_SAVING (CPU scaling, backlight floor) is
// expected to bring this down; revisit after a discharge run with the flag on.
constexpr float NOMINAL_ACTIVE_CURRENT_MA = 320.0f;
constexpr float NOMINAL_ECONOMY_CURRENT_MA = 220.0f;
constexpr float MIN_RUNTIME_CURRENT_MA = 40.0f;
constexpr float MAX_RUNTIME_CURRENT_MA = 800.0f;

struct SocPoint {
    uint16_t mv;
    float pct;
};

constexpr SocPoint SOC_CURVE[] = {
    {3300, 0.0f},
    {3420, 4.0f},
    {3520, 9.0f},
    {3600, 15.0f},
    {3680, 24.0f},
    {3750, 34.0f},
    {3820, 47.0f},
    {3890, 62.0f},
    {3960, 76.0f},
    {4040, 89.0f},
    {4120, 97.0f},
    {4200, 100.0f}
};

template <typename T>
T clampValue(T value, T minValue, T maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

float interpolate(float x0, float y0, float x1, float y1, float x) {
    if (fabsf(x1 - x0) < 0.001f) return y0;
    const float t = clampValue((x - x0) / (x1 - x0), 0.0f, 1.0f);
    return y0 + ((y1 - y0) * t);
}

const char* powerSourceName(PowerSource source) {
    switch (source) {
        case POWER_SOURCE_BATTERY: return "BATTERY";
        case POWER_SOURCE_USB:     return "USB";
        case POWER_SOURCE_UNKNOWN:
        default:                   return "UNKNOWN";
    }
}

const char* powerStateName(PowerState state) {
    switch (state) {
        case POWER_STATE_USB:              return "USB";
        case POWER_STATE_BATTERY_NORMAL:   return "BAT_NORMAL";
        case POWER_STATE_BATTERY_ECONOMY:  return "BAT_ECON";
        case POWER_STATE_BATTERY_CRITICAL: return "BAT_CRIT";
        default:                           return "UNKNOWN";
    }
}

}  // namespace

bool PowerManager::begin() {
    if (_ready) return true;

    pinMode(BAT_ADC, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(BAT_ADC, ADC_11db);

    if (POWER_USB_SENSE_PIN >= 0) {
        pinMode(POWER_USB_SENSE_PIN, INPUT);
    }
    if (POWER_CHARGE_SENSE_PIN >= 0) {
        pinMode(POWER_CHARGE_SENSE_PIN, INPUT);
    }

    _snapshot.batteryCapacityMah = POWER_BATTERY_CAPACITY_DEFAULT_MAH;
    _snapshot.source = POWER_SOURCE_UNKNOWN;
    _snapshot.state = POWER_STATE_BATTERY_NORMAL;

    _ready = true;
    tick(millis());
    DLOG_INFO("POWER", "Power manager ready");
    return true;
}

PowerSnapshot PowerManager::consumeSnapshot() {
    PowerSnapshot out = _snapshot;
    _snapshot.sourceChanged = false;
    _snapshot.stateChanged = false;
    _snapshot.criticalJustEntered = false;
    return out;
}

void PowerManager::tick(uint32_t nowMs) {
    if (!_ready && !begin()) return;
    if (nowMs == 0) nowMs = millis();
    if (_lastSampleMs != 0 && (nowMs - _lastSampleMs) < SAMPLE_INTERVAL_MS) {
        return;
    }
    _lastSampleMs = nowMs;

    uint16_t batteryCapacityMah = POWER_BATTERY_CAPACITY_DEFAULT_MAH;
    if (SETTINGS.isReady()) {
        batteryCapacityMah = clampValue<uint16_t>(
            SETTINGS.get().batteryCapacityMah,
            POWER_BATTERY_CAPACITY_MIN_MAH,
            POWER_BATTERY_CAPACITY_MAX_MAH);
    }

    const uint16_t sampleMv = _readBatteryMillivolts();
    if (_filteredVoltageMv <= 0.0f) {
        _filteredVoltageMv = static_cast<float>(sampleMv);
    } else {
        _filteredVoltageMv =
            (_filteredVoltageMv * (1.0f - FILTER_ALPHA)) +
            (static_cast<float>(sampleMv) * FILTER_ALPHA);
    }

    const uint16_t voltageMv =
        static_cast<uint16_t>(clampValue<int>(
            static_cast<int>(lroundf(_filteredVoltageMv)),
            0,
            5000));
    const float socPct = _estimateSocPercent(voltageMv);
    const int percent = clampValue<int>(
        static_cast<int>(lroundf(socPct)),
        0,
        100);

    _pushHistory(nowMs, voltageMv, socPct);
    const int16_t trendMvPerMin = _estimateTrendMvPerMinute(nowMs, voltageMv);

    const bool hardwareUsb =
        (POWER_USB_SENSE_PIN >= 0) &&
        (digitalRead(POWER_USB_SENSE_PIN) == POWER_USB_SENSE_ACTIVE);
    const bool hardwareCharging =
        (POWER_CHARGE_SENSE_PIN >= 0) &&
        (digitalRead(POWER_CHARGE_SENSE_PIN) == POWER_CHARGE_SENSE_ACTIVE);

    const PowerSource previousSource = _snapshot.source;
    const PowerState previousState = _snapshot.state;

    // _resolveSource uses the raw 30-second trend (intentional — that is what
    // detects USB rail voltage vs. a discharging battery).
    const PowerSource source = _resolveSource(nowMs,
                                              voltageMv,
                                              trendMvPerMin,
                                              socPct,
                                              hardwareUsb,
                                              hardwareCharging);

    // Stamp the source-change time as soon as the transition is accepted so
    // the trend gate below uses the correct anchor on the same tick. Also
    // stamp on the very first run, so initial boot starts a stable window.
    const bool sourceChangedNow =
        (previousSource != source) || (_sourceChangedAtMs == 0);
    if (sourceChangedNow) {
        _sourceChangedAtMs = nowMs;
    }

    // Gate the trend that feeds runtime/critical decisions: a USB->BATTERY
    // transition (or vice versa) leaves the 30s baseline anchored at the old
    // rail voltage, producing a spurious -400 mV/min slope. Suppress trend
    // pressure until we've been on the new source for TREND_STABLE_MIN_MS.
    // Note: this replaces the previous gate that only looked at absolute
    // history age — old history is exactly the problem here.
    const bool trendStable =
        ((nowMs - _sourceChangedAtMs) >= TREND_STABLE_MIN_MS);
    const int16_t trendForCritical = trendStable ? trendMvPerMin : 0;

    const bool charging = _resolveCharging(source,
                                           voltageMv,
                                           trendMvPerMin,
                                           socPct,
                                           hardwareCharging);

    _estimateDischargeCurrentMa(nowMs,
                                socPct,
                                batteryCapacityMah,
                                source,
                                charging);
    const uint16_t runtimeMin = _estimateRuntimeMinutes(socPct,
                                                        batteryCapacityMah,
                                                        voltageMv,
                                                        trendForCritical,
                                                        source,
                                                        charging);

    bool criticalJustEntered = false;
    const PowerState state = _resolveState(source,
                                           runtimeMin,
                                           socPct,
                                           voltageMv,
                                           trendForCritical,
                                           nowMs,
                                           criticalJustEntered);

    _snapshot.voltageMv = voltageMv;
    _snapshot.voltage = static_cast<float>(voltageMv) / 1000.0f;
    _snapshot.percent = percent;
    _snapshot.trendMvPerMin = trendMvPerMin;
    _snapshot.batteryCapacityMah = batteryCapacityMah;
    _snapshot.runtimeRemainingMin = runtimeMin;
    _snapshot.source = source;
    _snapshot.state = state;
    _snapshot.charging = charging;
    _snapshot.sourceChanged = (previousSource != source);
    _snapshot.stateChanged = (previousState != state);
    _snapshot.criticalJustEntered = criticalJustEntered;

    if (_snapshot.sourceChanged) {
        DLOG_INFO("POWER",
                  "Source %s -> %s (%umV trend=%d)",
                  powerSourceName(previousSource),
                  powerSourceName(source),
                  static_cast<unsigned>(voltageMv),
                  static_cast<int>(trendMvPerMin));

        // Drop the rolling history and the voltage filter on any real source
        // transition. Without this, the next ~3 minutes of trend computations
        // will mix pre-transition voltages (USB rail at 4.20 V or empty-load
        // battery resting voltage) with post-transition voltages, producing a
        // false -400 mV/min slope that makes _estimateRuntimeMinutes cap
        // runtime to ~2 minutes and falsely latch BATTERY_CRITICAL.
        // The trend gate (_sourceChangedAtMs / TREND_STABLE_MIN_MS) is the
        // primary defense; this reset is belt-and-suspenders so that even if
        // the gate were ever bypassed, the data fed to it is sane.
        _historyCount = 0;
        _historyHead = 0;
        _filteredVoltageMv = 0.0f;
        _estimatedCurrentMa = 0.0f;
    }

    if (_snapshot.stateChanged || criticalJustEntered) {
        DLOG_INFO("POWER",
                  "State %s -> %s (%d%% %umV runtime=%umin charge=%d)",
                  powerStateName(previousState),
                  powerStateName(state),
                  percent,
                  static_cast<unsigned>(voltageMv),
                  static_cast<unsigned>(runtimeMin),
                  charging ? 1 : 0);
    }
}

uint16_t PowerManager::_readBatteryMillivolts() const {
    uint32_t totalMv = 0;
    uint8_t validSamples = 0;

    for (int i = 0; i < 8; ++i) {
        const int sample = analogReadMilliVolts(BAT_ADC);
        if (sample > 0) {
            totalMv += static_cast<uint32_t>(sample);
            validSamples++;
        }
        delayMicroseconds(200);
    }

    if (validSamples == 0) {
        return 0;
    }

    const uint16_t adcMv = static_cast<uint16_t>(totalMv / validSamples);
    return _scaleBatteryMillivolts(adcMv);
}

uint16_t PowerManager::_scaleBatteryMillivolts(uint16_t adcMv) const {
    const uint32_t scaled =
        (static_cast<uint32_t>(adcMv) * POWER_BATTERY_DIVIDER_NUM) /
        max(POWER_BATTERY_DIVIDER_DEN, 1);
    return static_cast<uint16_t>(clampValue<uint32_t>(scaled, 0U, 5000U));
}

float PowerManager::_estimateSocPercent(uint16_t voltageMv) const {
    if (voltageMv <= SOC_CURVE[0].mv) {
        return SOC_CURVE[0].pct;
    }

    const size_t lastIndex = (sizeof(SOC_CURVE) / sizeof(SOC_CURVE[0])) - 1;
    if (voltageMv >= SOC_CURVE[lastIndex].mv) {
        return SOC_CURVE[lastIndex].pct;
    }

    for (size_t i = 1; i <= lastIndex; ++i) {
        if (voltageMv > SOC_CURVE[i].mv) continue;
        return interpolate(static_cast<float>(SOC_CURVE[i - 1].mv),
                           SOC_CURVE[i - 1].pct,
                           static_cast<float>(SOC_CURVE[i].mv),
                           SOC_CURVE[i].pct,
                           static_cast<float>(voltageMv));
    }

    return 0.0f;
}

void PowerManager::_pushHistory(uint32_t nowMs, uint16_t mv, float socPct) {
    _history[_historyHead].ms = nowMs;
    _history[_historyHead].mv = mv;
    _history[_historyHead].socPct = socPct;
    _historyHead = (_historyHead + 1U) % HISTORY_SIZE;
    if (_historyCount < HISTORY_SIZE) {
        _historyCount++;
    }
}

bool PowerManager::_historySampleForAge(uint32_t nowMs,
                                        uint32_t minAgeMs,
                                        HistoryPoint& out) const {
    if (_historyCount == 0) return false;

    const uint16_t oldestIndex =
        (_historyHead + HISTORY_SIZE - _historyCount) % HISTORY_SIZE;
    const HistoryPoint& oldest = _history[oldestIndex];
    if (oldest.ms == 0 || (nowMs - oldest.ms) < minAgeMs) {
        return false;
    }

    out = oldest;
    return true;
}

int16_t PowerManager::_estimateTrendMvPerMinute(uint32_t nowMs, uint16_t mv) const {
    HistoryPoint baseline = {};
    if (!_historySampleForAge(nowMs, TREND_WINDOW_MS, baseline)) {
        return 0;
    }

    const uint32_t dtMs = nowMs - baseline.ms;
    if (dtMs == 0) return 0;

    const float slope =
        (static_cast<float>(mv) - static_cast<float>(baseline.mv)) *
        (60000.0f / static_cast<float>(dtMs));
    return static_cast<int16_t>(
        clampValue<int>(static_cast<int>(lroundf(slope)), -400, 400));
}

float PowerManager::_estimateDischargeCurrentMa(uint32_t nowMs,
                                                float socPct,
                                                uint16_t batteryCapacityMah,
                                                PowerSource source,
                                                bool charging) {
    const bool economyLike =
        (_snapshot.state == POWER_STATE_BATTERY_ECONOMY) ||
        (_snapshot.state == POWER_STATE_BATTERY_CRITICAL);
    const float nominalMa =
        economyLike ? NOMINAL_ECONOMY_CURRENT_MA : NOMINAL_ACTIVE_CURRENT_MA;

    if (source != POWER_SOURCE_BATTERY || charging) {
        _estimatedCurrentMa = 0.0f;
        return 0.0f;
    }

    HistoryPoint baseline = {};
    if (_historySampleForAge(nowMs, CURRENT_WINDOW_MS, baseline)) {
        const uint32_t dtMs = nowMs - baseline.ms;
        const float socDelta = baseline.socPct - socPct;
        if (dtMs >= 60000UL && socDelta >= MIN_SOC_DELTA_FOR_CURRENT) {
            const float hours = static_cast<float>(dtMs) / 3600000.0f;
            if (hours > 0.0f) {
                const float consumedMah =
                    (socDelta / 100.0f) * static_cast<float>(batteryCapacityMah);
                const float measuredMa = consumedMah / hours;
                if (measuredMa >= MIN_RUNTIME_CURRENT_MA &&
                    measuredMa <= MAX_RUNTIME_CURRENT_MA) {
                    if (_estimatedCurrentMa <= 0.0f) {
                        _estimatedCurrentMa = measuredMa;
                    } else {
                        _estimatedCurrentMa =
                            (_estimatedCurrentMa * (1.0f - CURRENT_EMA_ALPHA)) +
                            (measuredMa * CURRENT_EMA_ALPHA);
                    }
                }
            }
        }
    }

    if (_estimatedCurrentMa <= 0.0f) {
        _estimatedCurrentMa = nominalMa;
    } else {
        _estimatedCurrentMa =
            (_estimatedCurrentMa * 0.85f) + (nominalMa * 0.15f);
    }

    _estimatedCurrentMa = clampValue(_estimatedCurrentMa,
                                     MIN_RUNTIME_CURRENT_MA,
                                     MAX_RUNTIME_CURRENT_MA);
    return _estimatedCurrentMa;
}

PowerSource PowerManager::_resolveSource(uint32_t nowMs,
                                         uint16_t voltageMv,
                                         int16_t trendMvPerMin,
                                         float socPct,
                                         bool hardwareUsb,
                                         bool hardwareCharging) {
    if (hardwareUsb || hardwareCharging) {
        _candidateSource = POWER_SOURCE_USB;
        _candidateSinceMs = nowMs;
        return POWER_SOURCE_USB;
    }

    bool usbCandidate = false;
    if (voltageMv >= 4195) {
        usbCandidate = true;
    } else if (trendMvPerMin >= 8 && socPct < 99.0f) {
        usbCandidate = true;
    } else if (_snapshot.source == POWER_SOURCE_UNKNOWN &&
               voltageMv >= 4150 &&
               trendMvPerMin >= 2) {
        usbCandidate = true;
    } else if (_snapshot.source == POWER_SOURCE_USB &&
               voltageMv >= 4080 &&
               trendMvPerMin >= -4) {
        usbCandidate = true;
    }

    const PowerSource candidate =
        usbCandidate ? POWER_SOURCE_USB : POWER_SOURCE_BATTERY;
    if (_candidateSource != candidate) {
        _candidateSource = candidate;
        _candidateSinceMs = nowMs;
    }

    if (_snapshot.source == POWER_SOURCE_UNKNOWN) {
        return candidate;
    }
    if (candidate == _snapshot.source) {
        return _snapshot.source;
    }

    uint32_t requiredDwellMs =
        (candidate == POWER_SOURCE_USB) ? USB_ENTER_DWELL_MS : USB_EXIT_DWELL_MS;
    if (candidate == POWER_SOURCE_BATTERY &&
        voltageMv < 3980 &&
        trendMvPerMin <= -8) {
        requiredDwellMs = USB_EXIT_FAST_DWELL_MS;
    }

    if ((nowMs - _candidateSinceMs) >= requiredDwellMs) {
        return candidate;
    }
    return _snapshot.source;
}

bool PowerManager::_resolveCharging(PowerSource source,
                                    uint16_t voltageMv,
                                    int16_t trendMvPerMin,
                                    float socPct,
                                    bool hardwareCharging) const {
    if (hardwareCharging) {
        return true;
    }
    if (source != POWER_SOURCE_USB) {
        return false;
    }

    if (socPct >= 98.0f && voltageMv >= 4180 && trendMvPerMin <= 2) {
        return false;
    }
    return (socPct < 98.0f && voltageMv < 4185) || trendMvPerMin >= 3;
}

uint16_t PowerManager::_estimateRuntimeMinutes(float socPct,
                                               uint16_t batteryCapacityMah,
                                               uint16_t voltageMv,
                                               int16_t trendMvPerMin,
                                               PowerSource source,
                                               bool charging) const {
    if (source != POWER_SOURCE_BATTERY || charging) {
        return 0;
    }

    const float currentMa = clampValue(
        _estimatedCurrentMa > 0.0f ? _estimatedCurrentMa : NOMINAL_ACTIVE_CURRENT_MA,
        MIN_RUNTIME_CURRENT_MA,
        MAX_RUNTIME_CURRENT_MA);
    const float remainingMah =
        (clampValue(socPct, 0.0f, 100.0f) / 100.0f) *
        static_cast<float>(batteryCapacityMah);
    float runtimeMin = (remainingMah / currentMa) * 60.0f;

    if (trendMvPerMin < -1 && voltageMv > (BAT_CRITICAL_MV + 30)) {
        const float voltageHeadroomMv =
            static_cast<float>(voltageMv - (BAT_CRITICAL_MV + 30));
        const float trendRuntimeMin =
            voltageHeadroomMv / fabsf(static_cast<float>(trendMvPerMin));
        if (trendRuntimeMin > 0.0f) {
            runtimeMin = min(runtimeMin, trendRuntimeMin);
        }
    }

    return static_cast<uint16_t>(
        clampValue<int>(static_cast<int>(lroundf(runtimeMin)), 1, 1440));
}

PowerState PowerManager::_resolveState(PowerSource source,
                                       uint16_t runtimeMin,
                                       float socPct,
                                       uint16_t voltageMv,
                                       int16_t trendMvPerMin,
                                       uint32_t nowMs,
                                       bool& criticalJustEntered) {
    criticalJustEntered = false;

    if (source == POWER_SOURCE_USB) {
        _criticalLatched = false;
        _snapshot.criticalSinceMs = 0;
        _snapshot.criticalSleepAtMs = 0;
        return POWER_STATE_USB;
    }

    // Voltage safety floor: never treat a "runtime <= 5 min" signal as
    // hard-critical while the cell is still measurably above warning. A
    // genuinely-empty battery is below ~3.7 V long before runtime would
    // estimate this low; a reading of "2 minutes left at 3.9 V" is the
    // signature of a stale-baseline trend, not real depletion.
    const bool voltageBelowSafeFloor =
        (voltageMv <= static_cast<uint16_t>(BAT_WARN_MV + 200));
    const bool hardRuntimeCritical =
        (runtimeMin > 0 && runtimeMin <= 5 && voltageBelowSafeFloor);
    const bool criticalReserveLow =
        socPct <= 18.0f ||
        voltageMv <= (BAT_WARN_MV + 120) ||
        trendMvPerMin <= -12;
    const bool shouldEnterCritical =
        hardRuntimeCritical ||
        (runtimeMin > 0 &&
         runtimeMin <= POWER_CRITICAL_RUNTIME_MIN &&
         criticalReserveLow);

    // Unlatch path: previously _criticalLatched only cleared on entering USB,
    // so a single transient false-critical (e.g. caused by the USB->BATTERY
    // history mixing) would persist until the user replugged. Clear the latch
    // when we've been on stable battery for >=60s AND the steady-state
    // signals all look healthy AND we wouldn't re-enter critical right now.
    if (_criticalLatched) {
        const bool sourceStableLong =
            (_sourceChangedAtMs != 0) &&
            ((nowMs - _sourceChangedAtMs) >= 60000UL);
        const bool voltageHealthy =
            voltageMv >= static_cast<uint16_t>(BAT_WARN_MV + 300);
        const bool trendHealthy = (trendMvPerMin >= -8);
        if (sourceStableLong && voltageHealthy && trendHealthy &&
            !shouldEnterCritical) {
            _criticalLatched = false;
            _snapshot.criticalSinceMs = 0;
            _snapshot.criticalSleepAtMs = 0;
        }
    }

    if (!_criticalLatched && shouldEnterCritical) {
        _criticalLatched = true;
        _snapshot.criticalSinceMs = nowMs;
        _snapshot.criticalSleepAtMs = nowMs + POWER_CRITICAL_SLEEP_COUNTDOWN_MS;
        criticalJustEntered = true;
    }

    if (_criticalLatched) {
        return POWER_STATE_BATTERY_CRITICAL;
    }

    _snapshot.criticalSinceMs = 0;
    _snapshot.criticalSleepAtMs = 0;

    const bool stayEconomy =
        _snapshot.state == POWER_STATE_BATTERY_ECONOMY &&
        socPct <= static_cast<float>(POWER_ECONOMY_PERCENT + 6);
    if (stayEconomy || socPct <= static_cast<float>(POWER_ECONOMY_PERCENT)) {
        return POWER_STATE_BATTERY_ECONOMY;
    }

    return POWER_STATE_BATTERY_NORMAL;
}




