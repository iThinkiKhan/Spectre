
#pragma once

#include <Arduino.h>
#include <vector>

#include "SpoolSegment.h"
#include "SpoolRepairTypes.h"

// Per-job state for an in-progress spool audit/repair pass.
//
// The state-machine methods that operate on this struct (requestSpoolRepair,
// repairStep, _beginRepairSegment, _repairJsonlSlice, _repairBinaryMetaSlice,
// _finishRepairSegment, _finalizeRepairJob, ...) remain on StorageManager
// for now — they reach deep into the spool index, counter, and persistence
// machinery. A future extraction will move them once SpoolStore tightens
// that boundary.

struct SpoolRepairJob {
    bool active = false;
    String reason;
    SpoolAuditResult audit;
    std::vector<SpoolSegmentInfo> repairedSegments;
    std::vector<String> rebuiltSessions;
    std::vector<String> segmentSessions;
    SpoolSegmentInfo originalSegment;
    SpoolSegmentInfo rebuiltSegment;
    size_t segmentIndex = 0;
    uint32_t fileOffset = 0;
    uint32_t startMs = 0;
    uint32_t segmentValidEventRecords = 0;
    uint32_t segmentValidEnrichDeltas = 0;
    String lastSession;
    bool scanningSegment = false;
    bool segmentChanged = false;
};
