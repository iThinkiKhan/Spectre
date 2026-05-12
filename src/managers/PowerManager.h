


#pragma once

#include <Arduino.h>

#include "../core/SpectreState.h"

struct PowerSnapshot {
    uint16_t voltageMv = 0;
    float voltage = 0.0f;
    int percent = 0;
    int16_t trendMvPerMin = 0;
    uint16_t batteryCapacityMah = 0;
    uint16_t runtimeRemainingMin = 0;
    PowerSource source = POWER_SOURCE_UNKNOWN;
    PowerState state = POWER_STATE_BATTERY_NORMAL;
    bool charging = false;
    bool sourceChanged = false;
    bool stateChanged = false;
    bool criticalJustEntered = false;
    uint32_t criticalSinceMs = 0;
    uint32_t criticalSleepAtMs = 0;
};

class PowerManager {
public:
    static PowerManager& getInstance() {
        static PowerManager instance;
        return instance;
    }

    bool begin();
    void tick(uint32_t nowMs = 0);

    PowerSnapshot snapshot() const { return _snapshot; }
    PowerSnapshot consumeSnapshot();

private:
    PowerManager() = default;

    struct HistoryPoint {
        uint32_t ms = 0;
        uint16_t mv = 0;
        float socPct = 0.0f;
    };

    static constexpr uint32_t SAMPLE_INTERVAL_MS = 1000UL;
    static constexpr uint16_t HISTORY_SIZE = 180;

    uint16_t _readBatteryMillivolts() const;
    uint16_t _scaleBatteryMillivolts(uint16_t adcMv) const;
    float _estimateSocPercent(uint16_t voltageMv) const;
    void _pushHistory(uint32_t nowMs, uint16_t mv, float socPct);
    bool _historySampleForAge(uint32_t nowMs,
                              uint32_t minAgeMs,
                              HistoryPoint& out) const;
    int16_t _estimateTrendMvPerMinute(uint32_t nowMs, uint16_t mv) const;
    float _estimateDischargeCurrentMa(uint32_t nowMs,
                                      float socPct,
                                      uint16_t batteryCapacityMah,
                                      PowerSource source,
                                      bool charging);
    PowerSource _resolveSource(uint32_t nowMs,
                               uint16_t voltageMv,
                               int16_t trendMvPerMin,
                               float socPct,
                               bool hardwareUsb,
                               bool hardwareCharging);
    bool _resolveCharging(PowerSource source,
                          uint16_t voltageMv,
                          int16_t trendMvPerMin,
                          float socPct,
                          bool hardwareCharging) const;
    uint16_t _estimateRuntimeMinutes(float socPct,
                                     uint16_t batteryCapacityMah,
                                     uint16_t voltageMv,
                                     int16_t trendMvPerMin,
                                     PowerSource source,
                                     bool charging) const;
    PowerState _resolveState(PowerSource source,
                             uint16_t runtimeMin,
                             float socPct,
                             uint16_t voltageMv,
                             int16_t trendMvPerMin,
                             uint32_t nowMs,
                             bool& criticalJustEntered);

    bool _ready = false;
    PowerSnapshot _snapshot = {};
    float _filteredVoltageMv = 0.0f;
    float _estimatedCurrentMa = 0.0f;
    uint32_t _lastSampleMs = 0;
    PowerSource _candidateSource = POWER_SOURCE_UNKNOWN;
    uint32_t _candidateSinceMs = 0;
    // Wall-clock timestamp of the last accepted source transition (USB<->BATTERY,
    // or initial source detection). Trend gating for runtime/critical decisions
    // is anchored to this rather than to absolute history age — history that
    // crosses a source boundary mixes USB-rail voltages with battery-resting
    // voltages and produces a false catastrophic discharge trend.
    uint32_t _sourceChangedAtMs = 0;
    HistoryPoint _history[HISTORY_SIZE] = {};
    uint16_t _historyHead = 0;
    uint16_t _historyCount = 0;
    bool _criticalLatched = false;
};

#define POWER_MGR PowerManager::getInstance()




