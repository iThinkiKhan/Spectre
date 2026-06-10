#include "DashboardStreamer.h"

#include <string.h>

#include "../core/DebugLog.h"
#include "CommandDispatcher.h"
#include "PhoneTransportRouter.h"

namespace {
constexpr const char* TAG = "DSTREAM";
}

DashboardStreamer DASHBOARD_STREAMER;

void DashboardStreamer::begin() {
    if (_begun) return;
    _begun = true;
    _streamId = 0;
    _nextStreamId = 1;
    _intervalMs = PHONE_DASHBOARD_STREAM_INTERVAL_DEFAULT_MS;
    _deadlineMs = 0;
    _nextEmitMs = 0;
    _nextSeq = 1;
}

DashboardStreamer::StartResult DashboardStreamer::start(
    uint32_t requestedIntervalMs, uint32_t requestedDurationMs) {
    if (!_begun) begin();

    if (_streamId != 0) {
        _emitFinalChunkIfActive("superseded");
    }

    uint32_t duration = requestedDurationMs;
    if (duration < PHONE_DASHBOARD_STREAM_LEASE_MIN_MS) {
        duration = PHONE_DASHBOARD_STREAM_LEASE_MIN_MS;
    } else if (duration > PHONE_DASHBOARD_STREAM_LEASE_MAX_MS) {
        duration = PHONE_DASHBOARD_STREAM_LEASE_MAX_MS;
    }

    uint32_t interval = requestedIntervalMs == 0
        ? PHONE_DASHBOARD_STREAM_INTERVAL_DEFAULT_MS
        : requestedIntervalMs;
    if (interval < PHONE_DASHBOARD_STREAM_INTERVAL_MIN_MS) {
        interval = PHONE_DASHBOARD_STREAM_INTERVAL_MIN_MS;
    } else if (interval > PHONE_DASHBOARD_STREAM_INTERVAL_MAX_MS) {
        interval = PHONE_DASHBOARD_STREAM_INTERVAL_MAX_MS;
    }

    if (_nextStreamId == 0) _nextStreamId = 1;
    _streamId          = _nextStreamId++;
    _intervalMs        = interval;
    _deadlineMs        = millis() + duration;
    _nextEmitMs        = millis();  // emit first snapshot immediately
    _nextSeq           = 1;
    _lastTransportKind = static_cast<uint8_t>(PHONE_XPORT.kind());

    DLOG_INFO(TAG,
              "stream start id=%u intervalMs=%lu durationMs=%lu transport=%s",
              static_cast<unsigned>(_streamId),
              static_cast<unsigned long>(interval),
              static_cast<unsigned long>(duration),
              PhoneTransportRouter::kindName(PHONE_XPORT.kind()));

    return StartResult{
        .ok                = true,
        .streamId          = _streamId,
        .grantedIntervalMs = interval,
        .grantedDurationMs = duration,
    };
}

bool DashboardStreamer::stop(uint8_t streamId, bool flushFinal, const char* reason) {
    if (_streamId == 0 || _streamId != streamId) return false;
    if (flushFinal) {
        _emitFinalChunkIfActive(reason ? reason : "stop");
    } else {
        _streamId = 0;
        DLOG_INFO(TAG, "stream stop id=%u reason=%s",
                  static_cast<unsigned>(streamId),
                  reason ? reason : "-");
    }
    return true;
}

void DashboardStreamer::_emitSnapshotChunk(bool ending) {
    if (_streamId == 0) return;

    CmdDashboardSnapshotV1 snap = {};
    if (!CommandDispatcher::populateDashboardSnapshot(snap)) {
        DLOG_WARN(TAG, "snapshot build failed id=%u",
                  static_cast<unsigned>(_streamId));
        return;
    }

    uint8_t plain[DASHBOARD_STREAM_CHUNK_FRAME_MAX];
    DashboardStreamChunkV1Header hdr = {};
    hdr.version  = DASHBOARD_STREAM_CHUNK_VERSION;
    hdr.streamId = _streamId;
    hdr.seq      = _nextSeq++;
    hdr.flags    = ending ? DASHBOARD_STREAM_FLAG_END : 0;
    hdr.reserved = 0;
    memcpy(plain, &hdr, sizeof(hdr));
    memcpy(plain + sizeof(hdr), &snap, sizeof(snap));
    const size_t plainLen = sizeof(hdr) + sizeof(snap);

    if (!PHONE_XPORT.publishDashboardStreamChunk(plain, plainLen)) {
        DLOG_WARN(TAG, "chunk write failed id=%u seq=%u",
                  static_cast<unsigned>(_streamId),
                  static_cast<unsigned>(hdr.seq));
    }
}

void DashboardStreamer::_emitFinalChunkIfActive(const char* reason) {
    if (_streamId == 0) return;
    const uint8_t endingId = _streamId;
    _emitSnapshotChunk(/*ending=*/true);
    _streamId = 0;
    DLOG_INFO(TAG, "stream end id=%u reason=%s",
              static_cast<unsigned>(endingId),
              reason ? reason : "-");
}

void DashboardStreamer::tick() {
    if (!_begun || _streamId == 0) return;

    const uint8_t currentKind = static_cast<uint8_t>(PHONE_XPORT.kind());
    if (currentKind != _lastTransportKind) {
        _emitFinalChunkIfActive("transport_flip");
        return;
    }

    const uint32_t now = millis();
    if (static_cast<int32_t>(now - _deadlineMs) >= 0) {
        _emitFinalChunkIfActive("lease_expired");
        return;
    }

    if (static_cast<int32_t>(now - _nextEmitMs) < 0) {
        return;
    }

    _emitSnapshotChunk(/*ending=*/false);
    _nextEmitMs = now + _intervalMs;
}
