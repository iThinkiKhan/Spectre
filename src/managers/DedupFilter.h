
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

#include "StorageLanes.h"

// Time-windowed dedup + 4-way-handshake-frame tracking.
//
// Stateful: owns two sliding windows. Pure with respect to StorageManager —
// each classifier mutates its window and returns a verdict; the caller
// applies any counter delta / UI refresh side effect implied by the verdict.

struct DedupCounterEffect {
    bool apply = false;
    const char* reason = nullptr;
    int32_t droppedDelta = 0;
    int32_t suppressedDelta = 0;
    StorageLane lane = STORAGE_LANE_NOISE;
    StoragePriority priority = STORAGE_PRIO_P3;
};

struct DedupVerdict {
    bool suppress = false;
    DedupCounterEffect counter;
};

struct HandshakeVerdict {
    bool accept = true;
    DedupCounterEffect counter;
};

class DedupFilter {
public:
    // Classify a non-handshake capture candidate. May mutate the dedup
    // window (refresh entry timestamp, insert new entry, or expire-and-reset).
    DedupVerdict classifyDedupCandidate(const char* type,
                                        JsonObjectConst payload,
                                        StoragePriority priority);

    // Classify one frame of a 4-way handshake. May mutate the handshake
    // window (update mask, insert new progress record). messageNumber is
    // 1..4; out-of-range returns accept=false without side effects.
    HandshakeVerdict classifyHandshakeFrame(const char* apMac,
                                            const char* staMac,
                                            const char* ssid,
                                            uint8_t messageNumber);

    // Drop entries older than DEDUP_WINDOW_MS / HANDSHAKE_WINDOW_MS, then
    // trim the dedup window to DEDUP_WINDOW_MAX entries (oldest first).
    void trimWindows();

private:
    struct DedupWindowEntry {
        uint64_t key         = 0;
        uint32_t firstSeenMs = 0;
        uint32_t lastSeenMs  = 0;
        uint32_t count       = 0;
    };
    struct HandshakeProgress {
        uint64_t key         = 0;
        uint8_t  frameMask   = 0;
        uint32_t lastUpdateMs = 0;
        bool     complete    = false;
    };

    std::vector<DedupWindowEntry> _dedupWindow;
    std::vector<HandshakeProgress> _handshakeWindow;

    uint64_t _makeDedupKey(const char* type, JsonObjectConst payload) const;
};
