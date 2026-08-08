
#pragma once

#include <Arduino.h>

// Type definitions and pure helpers for the spool audit/repair subsystem.
// SpoolRepairJob lives in SpoolRepair.h (it depends on SpoolSegmentInfo).
// The state-machine methods that operate on a SpoolRepairJob still live
// on StorageManager — they reach deep into the spool index, counter, and
// persistence machinery, and will move once SpoolStore tightens that
// boundary.

enum SpoolRepairMode : uint8_t {
    REPAIR_BACKGROUND = 0,
    REPAIR_FAST_IDLE,
    REPAIR_EMERGENCY
};

enum class SpoolCorruptionReason : uint8_t {
    NONE = 0,
    SEGMENT_OPEN_FAILED,
    SEGMENT_HEADER_INVALID,
    RECORD_LENGTH_INVALID,
    RECORD_CRC_FAILED,
    RECORD_DECODE_FAILED,
    RECORD_SEMANTIC_INVALID,
    SCAN_FAILED,
    SUMMARY_MISMATCH,
    UNKNOWN
};

enum class SegmentPendingScanResult : uint8_t {
    HAS_PENDING,
    NO_PENDING,
    SCAN_FAILED
};

struct SpoolAuditResult {
    uint32_t scannedSegments    = 0;
    uint32_t scannedRecords     = 0;
    uint32_t validEventRecords  = 0;
    uint32_t validEnrichDeltas  = 0;
    uint32_t invalidRecords     = 0;
    uint32_t skippedRecords     = 0;
    uint32_t quarantinedSegments = 0;
    uint32_t unreadableSegments = 0;

    uint32_t rebuiltPendingTotal = 0;
    uint32_t maxEventIdSeen      = 0;

    uint32_t oldPendingTotal  = 0;
    uint32_t oldNextEventId   = 0;

    bool hadMismatch          = false;
    bool hadFatalSegmentError = false;
    bool repaired             = false;
    // True when the scan could not see the full on-disk picture — currently
    // only when the RAMSpool worker held an unflushed append file that could
    // not be flushed safely. The counts below are then a floor, not a total,
    // and callers must not reconcile counters downward from them.
    bool scanIncomplete       = false;

    uint32_t totalValidRecords() const {
        return validEventRecords + validEnrichDeltas;
    }
};

struct SpoolBootAuditResult {
    bool repairRequired = false;
    bool snapshotLagged = false;
    bool indexBehind = false;
    bool generationMatch = false;
    bool headerAuditOk = false;
    bool checkpointAuditOk = false;
    bool counterOk = false;
    uint32_t auditedSegments = 0;
    uint32_t indexedRecords = 0;
    uint32_t headerRecords = 0;
};

struct RepairProgressSnapshot {
    bool     active           = false;
    uint32_t scannedRecords   = 0;
    uint32_t scannedSegments  = 0;
    uint32_t totalSegments    = 0;
    uint32_t startedMs        = 0;
    const char* reason        = "";
};

inline const char* spoolCorruptionReasonText(SpoolCorruptionReason reason) {
    // NOTE: NONE is intentionally folded into UNKNOWN/"unknown" — matches the
    // original file-static helper. Do not change.
    switch (reason) {
        case SpoolCorruptionReason::SEGMENT_OPEN_FAILED:     return "segment_open_failed";
        case SpoolCorruptionReason::SEGMENT_HEADER_INVALID:  return "segment_header_invalid";
        case SpoolCorruptionReason::RECORD_LENGTH_INVALID:   return "record_length_invalid";
        case SpoolCorruptionReason::RECORD_CRC_FAILED:       return "record_crc_failed";
        case SpoolCorruptionReason::RECORD_DECODE_FAILED:    return "record_decode_failed";
        case SpoolCorruptionReason::RECORD_SEMANTIC_INVALID: return "record_semantic_invalid";
        case SpoolCorruptionReason::SCAN_FAILED:             return "scan_failed";
        case SpoolCorruptionReason::SUMMARY_MISMATCH:        return "summary_mismatch";
        case SpoolCorruptionReason::UNKNOWN:
        case SpoolCorruptionReason::NONE:
        default:                                              return "unknown";
    }
}

inline void repairBudgetsForMode(SpoolRepairMode mode,
                                  uint32_t& budgetMs,
                                  uint32_t& maxRecords) {
    switch (mode) {
        case REPAIR_FAST_IDLE:
            budgetMs = 1200UL;
            maxRecords = 2048U;
            break;
        case REPAIR_EMERGENCY:
            budgetMs = 50UL;
            maxRecords = 128U;
            break;
        case REPAIR_BACKGROUND:
        default:
            budgetMs = 1UL;
            maxRecords = 1U;
            break;
    }

    if (budgetMs == 0) {
        budgetMs = 1UL;
    }
    if (maxRecords == 0) {
        maxRecords = 1U;
    }
}
