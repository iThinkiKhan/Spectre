#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

#include "protocol/CompanionProtocol.h"

// Phone-requested log-tail stream; never blocks DebugLog or capture.
class LogStreamer {
public:
    struct StartResult {
        bool     ok;
        uint8_t  streamId;        // 0 if !ok
        uint16_t grantedLineCap;
        uint32_t grantedDurationMs;
    };

    void begin();

    // Begin a new lease.  Cancels any existing stream first; the phone
    // gets a final chunk on the OLD streamId before the new lease starts.
    StartResult start(uint16_t requestedLineCap, uint32_t requestedDurationMs);

    // End an active lease.  No-op if streamId doesn't match the active one.
    // `flushFinal=true` queues a final chunk with LOG_STREAM_FLAG_END.
    bool stop(uint8_t streamId, bool flushFinal, const char* reason);

    bool    isActive()       const { return _streamId != 0; }
    uint8_t activeStreamId() const { return _streamId; }

    // Producer side — called from inside DebugLog::log() (already on a task
    // context, never an ISR).  Cheap: portMUX_TYPE spin + bounded memcpy.
    void ingestLine(const char* line, size_t len);

    // Consumer side — called from the hardware task.  Emits at most one
    // chunk per call.  Services lease expiry and transport-flip cancellation.
    void tick();

private:
    static constexpr uint16_t SLOT_COUNT       = 32;
    static constexpr uint16_t LINE_MAX_BYTES   = 180;   // truncated past this
    static constexpr uint16_t DEFAULT_LINE_CAP = 16;

    struct LineSlot {
        uint16_t length;
        uint8_t  bytes[LINE_MAX_BYTES];
    };

    void   _resetLocked();
    void   _emitFinalChunkIfActive(const char* reason);
    size_t _packChunkPayload(uint8_t* out,
                             size_t outCap,
                             uint16_t lineCap,
                             uint16_t& outLineCount);
    void   _writeChunk(uint16_t lineCount,
                       uint16_t dropped,
                       const uint8_t* lineBuf,
                       size_t lineBufLen,
                       bool ending);

    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    bool         _begun = false;

    // Active lease state.  0 = inactive.
    uint8_t      _streamId          = 0;
    uint8_t      _nextStreamId      = 1;
    uint16_t     _lineCap           = DEFAULT_LINE_CAP;
    uint32_t     _deadlineMs        = 0;
    uint16_t     _nextSeq           = 1;
    uint16_t     _droppedSinceLast  = 0;
    uint8_t      _lastTransportKind = 0;

    // Ring of captured lines (~5.8 KB).  PSRAM-backed and allocated in begin()
    // rather than held as internal BSS; this is a debug/telemetry path, and
    // every consumer is gated behind _begun so a failed allocation simply
    // leaves the streamer disabled instead of dereferencing null.
    LineSlot* _slots = nullptr;
    uint16_t _head = 0;          // oldest populated slot
    uint16_t _depth = 0;         // populated slots
};

extern LogStreamer LOG_STREAMER;
