#pragma once

#include <Arduino.h>

#include "protocol/CompanionProtocol.h"

// Phone-requested dashboard stream lease, serviced from the hardware task.
class DashboardStreamer {
public:
    struct StartResult {
        bool     ok;
        uint8_t  streamId;         // 0 on failure
        uint32_t grantedIntervalMs;
        uint32_t grantedDurationMs;
    };

    void begin();

    // Begin a new lease.  Cancels any existing stream first; the phone
    // gets a final END chunk on the OLD streamId before the new one starts.
    StartResult start(uint32_t requestedIntervalMs, uint32_t requestedDurationMs);

    // End an active lease.  No-op if streamId doesn't match the active one.
    // flushFinal=true queues a final chunk with DASHBOARD_STREAM_FLAG_END.
    bool stop(uint8_t streamId, bool flushFinal, const char* reason);

    bool    isActive()       const { return _streamId != 0; }
    uint8_t activeStreamId() const { return _streamId; }

    // Called from the hardware task.  Emits at most one chunk per call when
    // the interval has elapsed; also handles lease expiry + transport flip.
    void tick();

private:
    void _emitFinalChunkIfActive(const char* reason);
    void _emitSnapshotChunk(bool ending);

    bool     _begun = false;

    // Active lease state.  0 = inactive.
    uint8_t  _streamId          = 0;
    uint8_t  _nextStreamId      = 1;
    uint32_t _intervalMs        = PHONE_DASHBOARD_STREAM_INTERVAL_DEFAULT_MS;
    uint32_t _deadlineMs        = 0;
    uint32_t _nextEmitMs        = 0;
    uint16_t _nextSeq           = 1;
    uint8_t  _lastTransportKind = 0;
};

extern DashboardStreamer DASHBOARD_STREAMER;
