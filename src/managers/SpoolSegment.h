
#pragma once

#include <Arduino.h>

// Spool segment data types — the on-disk + in-index shape of one segment.
// Moved out of StorageManager.h so SpoolRepair, the eventual SpoolStore,
// and any future spool-adjacent extraction can share the type without
// pulling the god class.

enum SpoolSegmentTrustState : uint8_t {
    SPOOL_SEGMENT_TRUSTED   = 0,
    SPOOL_SEGMENT_UNTRUSTED = 1,
    SPOOL_SEGMENT_INVALID   = 2
};

enum SpoolSegmentFormat : uint8_t {
    SPOOL_SEGMENT_JSONL  = 1,
    SPOOL_SEGMENT_BIN_V2 = 2
};

enum SpoolSegmentLifecycle : uint8_t {
    SPOOL_SEGMENT_ACTIVE = 0,
    SPOOL_SEGMENT_SEALED,
    SPOOL_SEGMENT_INDEXED,
    SPOOL_SEGMENT_PARTIALLY_UPLOADED,
    SPOOL_SEGMENT_FULLY_UPLOADED,
    SPOOL_SEGMENT_SALVAGED,
    SPOOL_SEGMENT_DRAINED,
    SPOOL_SEGMENT_DELETE_PENDING,
    SPOOL_SEGMENT_QUARANTINED
};

// Bump when the in-memory SpoolSegmentInfo "summary" cache format changes
// in a way that requires re-scanning live segments to repopulate.
static constexpr uint16_t SPOOL_SEGMENT_SUMMARY_VERSION = 4;

struct SpoolSegmentInfo {
    uint32_t segmentId = 0;
    uint32_t firstEventId = 0;
    uint32_t lastEventId = 0;
    uint16_t summaryVersion = 0;
    bool summaryValid = false;  // cache-only; fall back to scan when false
    uint8_t trustState = SPOOL_SEGMENT_TRUSTED;
    uint8_t lifecycle = SPOOL_SEGMENT_ACTIVE;

    uint32_t recordCount = 0;
    uint32_t eventCount = 0;
    uint32_t enrichDeltaCount = 0;

    uint32_t missionCount = 0;
    uint32_t noiseCount = 0;
    uint32_t pendingUploadMissionCount = 0;
    uint32_t pendingUploadNoiseCount = 0;

    uint32_t p0Count = 0;
    uint32_t p1Count = 0;
    uint32_t p2Count = 0;
    uint32_t p3Count = 0;

    // Events ORIGINATING in this segment that want enrichment but haven't
    // been enriched anywhere yet. Maintained live: incremented on append
    // when the event wants enrichment; decremented when a delta lands
    // anywhere referencing one of this segment's eventIds. Lets hot-path
    // getters skip drained segments instead of scanning every record.
    uint32_t pendingEnrichmentCount = 0;

    uint32_t minTimestampMs = 0;
    uint32_t maxTimestampMs = 0;

    uint32_t approxBytes = 0;
    uint8_t format = SPOOL_SEGMENT_JSONL;
};
