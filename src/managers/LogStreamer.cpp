#include "LogStreamer.h"

#include <string.h>
#include <stdlib.h>
#include <esp_heap_caps.h>

#include "../core/DebugLog.h"
#include "PhoneTransportRouter.h"

namespace {
constexpr const char* TAG = "LSTREAM";
}

LogStreamer LOG_STREAMER;

void LogStreamer::begin() {
    if (_begun) {
        return;
    }
    // Allocate the line ring from PSRAM before arming _begun — every producer
    // and consumer checks _begun, so if this fails the streamer stays inert
    // rather than writing through a null pointer.
    if (!_slots) {
        _slots = static_cast<LineSlot*>(
            heap_caps_calloc(SLOT_COUNT, sizeof(LineSlot),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!_slots) {
            _slots = static_cast<LineSlot*>(calloc(SLOT_COUNT, sizeof(LineSlot)));
        }
        if (!_slots) {
            return;
        }
    }
    _begun = true;
    portENTER_CRITICAL(&_mux);
    _resetLocked();
    portEXIT_CRITICAL(&_mux);
}

void LogStreamer::_resetLocked() {
    _streamId         = 0;
    _lineCap          = DEFAULT_LINE_CAP;
    _deadlineMs       = 0;
    _nextSeq          = 1;
    _droppedSinceLast = 0;
    _head             = 0;
    _depth            = 0;
}

LogStreamer::StartResult LogStreamer::start(uint16_t requestedLineCap,
                                            uint32_t requestedDurationMs) {
    if (!_begun) {
        begin();
    }

    // If a stream is already live, send a final END chunk on the old
    // streamId before taking over.  Outside the lock to avoid recursion.
    if (_streamId != 0) {
        _emitFinalChunkIfActive("superseded");
    }

    uint32_t duration = requestedDurationMs;
    if (duration < PHONE_LOG_STREAM_LEASE_MIN_MS) {
        duration = PHONE_LOG_STREAM_LEASE_MIN_MS;
    } else if (duration > PHONE_LOG_STREAM_LEASE_MAX_MS) {
        duration = PHONE_LOG_STREAM_LEASE_MAX_MS;
    }

    uint16_t lineCap = requestedLineCap == 0 ? DEFAULT_LINE_CAP : requestedLineCap;
    if (lineCap > SLOT_COUNT) {
        lineCap = SLOT_COUNT;
    }

    portENTER_CRITICAL(&_mux);
    _resetLocked();
    if (_nextStreamId == 0) {
        _nextStreamId = 1;
    }
    _streamId          = _nextStreamId++;
    _lineCap           = lineCap;
    _deadlineMs        = millis() + duration;
    _nextSeq           = 1;
    _droppedSinceLast  = 0;
    _lastTransportKind = static_cast<uint8_t>(PHONE_XPORT.kind());
    portEXIT_CRITICAL(&_mux);

    DLOG_INFO(TAG, "stream start id=%u cap=%u durationMs=%lu transport=%s",
              static_cast<unsigned>(_streamId),
              static_cast<unsigned>(lineCap),
              static_cast<unsigned long>(duration),
              PhoneTransportRouter::kindName(PHONE_XPORT.kind()));

    return StartResult{
        .ok                = true,
        .streamId          = _streamId,
        .grantedLineCap    = lineCap,
        .grantedDurationMs = duration,
    };
}

bool LogStreamer::stop(uint8_t streamId, bool flushFinal, const char* reason) {
    portENTER_CRITICAL(&_mux);
    const bool matches = (_streamId != 0 && _streamId == streamId);
    portEXIT_CRITICAL(&_mux);
    if (!matches) {
        return false;
    }

    if (flushFinal) {
        _emitFinalChunkIfActive(reason ? reason : "stop");
    } else {
        portENTER_CRITICAL(&_mux);
        _resetLocked();
        portEXIT_CRITICAL(&_mux);
        DLOG_INFO(TAG, "stream stop id=%u reason=%s",
                  static_cast<unsigned>(streamId),
                  reason ? reason : "-");
    }
    return true;
}

void LogStreamer::ingestLine(const char* line, size_t len) {
    if (!_begun || !line || len == 0) {
        return;
    }

    // Strip trailing CR/LF so the phone gets clean lines.  DebugLog::log()
    // always appends "\r\n" to its format string.
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        len--;
    }
    if (len == 0) {
        return;
    }
    if (len > LINE_MAX_BYTES) {
        len = LINE_MAX_BYTES;
    }

    portENTER_CRITICAL(&_mux);
    if (_streamId == 0) {
        portEXIT_CRITICAL(&_mux);
        return;
    }

    if (_depth >= SLOT_COUNT) {
        // Ring full — drop oldest, count it.
        _head = (_head + 1) % SLOT_COUNT;
        _depth--;
        if (_droppedSinceLast != 0xFFFF) {
            _droppedSinceLast++;
        }
    }

    const uint16_t writeIdx = (_head + _depth) % SLOT_COUNT;
    LineSlot& slot = _slots[writeIdx];
    slot.length = static_cast<uint16_t>(len);
    memcpy(slot.bytes, line, len);
    _depth++;
    portEXIT_CRITICAL(&_mux);
}

size_t LogStreamer::_packChunkPayload(uint8_t* out,
                                      size_t outCap,
                                      uint16_t lineCap,
                                      uint16_t& outLineCount) {
    // Caller has _mux held.
    size_t written = 0;
    outLineCount = 0;
    while (outLineCount < lineCap && _depth > 0) {
        const LineSlot& slot = _slots[_head];
        const size_t need = static_cast<size_t>(slot.length) + 1;  // + null
        if (written + need > outCap) {
            break;
        }
        if (slot.length > 0) {
            memcpy(out + written, slot.bytes, slot.length);
        }
        out[written + slot.length] = 0;
        written += need;
        outLineCount++;

        _head = (_head + 1) % SLOT_COUNT;
        _depth--;
    }
    return written;
}

void LogStreamer::_writeChunk(uint16_t lineCount,
                              uint16_t dropped,
                              const uint8_t* lineBuf,
                              size_t lineBufLen,
                              bool ending) {
    if (_streamId == 0) {
        return;
    }

    uint8_t plain[LOG_STREAM_CHUNK_FRAME_MAX];
    LogStreamChunkV1Header hdr = {};
    hdr.version    = LOG_STREAM_CHUNK_VERSION;
    hdr.streamId   = _streamId;
    hdr.seq        = _nextSeq++;
    hdr.dropped    = dropped;
    hdr.lineCount  = lineCount;
    hdr.totalBytes = static_cast<uint16_t>(lineBufLen);
    hdr.flags      = ending ? LOG_STREAM_FLAG_END : 0;
    hdr.reserved   = 0;
    memcpy(plain, &hdr, sizeof(hdr));
    if (lineBufLen > 0) {
        memcpy(plain + sizeof(hdr), lineBuf, lineBufLen);
    }
    const size_t plainLen = sizeof(hdr) + lineBufLen;

    if (!PHONE_XPORT.publishLogStreamChunk(plain, plainLen)) {
        // Best-effort — drop the chunk if the transport can't carry it.
        DLOG_WARN(TAG, "chunk write failed id=%u seq=%u",
                  static_cast<unsigned>(_streamId),
                  static_cast<unsigned>(hdr.seq));
    }
}

void LogStreamer::_emitFinalChunkIfActive(const char* reason) {
    if (_streamId == 0) {
        return;
    }

    uint8_t lineBuf[LOG_STREAM_CHUNK_PAYLOAD_MAX];
    uint16_t lineCount = 0;
    uint16_t dropped = 0;
    uint8_t  endingStreamId = 0;

    portENTER_CRITICAL(&_mux);
    if (_streamId == 0) {
        portEXIT_CRITICAL(&_mux);
        return;
    }
    const size_t lineBufLen =
        _packChunkPayload(lineBuf, sizeof(lineBuf), _lineCap, lineCount);
    dropped = _droppedSinceLast;
    _droppedSinceLast = 0;
    endingStreamId = _streamId;
    portEXIT_CRITICAL(&_mux);

    _writeChunk(lineCount, dropped, lineBuf, lineBufLen, /*ending=*/true);

    portENTER_CRITICAL(&_mux);
    _resetLocked();
    portEXIT_CRITICAL(&_mux);

    DLOG_INFO(TAG, "stream end id=%u reason=%s",
              static_cast<unsigned>(endingStreamId),
              reason ? reason : "-");
}

void LogStreamer::tick() {
    if (!_begun || _streamId == 0) {
        return;
    }

    // Auto-cancel on transport flip — the response char on the old transport
    // may be gone, and the phone is no longer subscribed there.
    const uint8_t currentKind = static_cast<uint8_t>(PHONE_XPORT.kind());
    if (currentKind != _lastTransportKind) {
        _emitFinalChunkIfActive("transport_flip");
        return;
    }

    // Lease expiry.
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - _deadlineMs) >= 0) {
        _emitFinalChunkIfActive("lease_expired");
        return;
    }

    // Drain one chunk's worth of lines (or skip if nothing to send and no
    // dropped lines to report).
    portENTER_CRITICAL(&_mux);
    if (_depth == 0 && _droppedSinceLast == 0) {
        portEXIT_CRITICAL(&_mux);
        return;
    }
    uint8_t lineBuf[LOG_STREAM_CHUNK_PAYLOAD_MAX];
    uint16_t lineCount = 0;
    const size_t lineBufLen =
        _packChunkPayload(lineBuf, sizeof(lineBuf), _lineCap, lineCount);
    const uint16_t dropped = _droppedSinceLast;
    _droppedSinceLast = 0;
    portEXIT_CRITICAL(&_mux);

    _writeChunk(lineCount, dropped, lineBuf, lineBufLen, /*ending=*/false);
}
