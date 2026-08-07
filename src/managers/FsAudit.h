
#pragma once

#include <Arduino.h>

// Bounded LittleFS audit/recovery pass. Caller must hold STORAGE_MAINTENANCE.
namespace FsAudit {

enum FsClass : uint8_t {
    FS_CLASS_UNCLASSIFIED = 0,
    FS_CLASS_KNOWN_VALID,
    FS_CLASS_KNOWN_INVALID,   // matched a spec but failed cheap validation
    FS_CLASS_LEGACY,          // matched a deprecated-but-known path
    FS_CLASS_FORENSIC,        // /spool_bad/* — leave alone, never touch
    FS_CLASS_TMP_ORPHAN,      // *.tmp / *.partial — likely interrupted writer
    FS_CLASS_UNKNOWN          // no spec matched; FieldVault-logged
};

enum FsAction : uint8_t {
    FS_ACT_NONE = 0,
    FS_ACT_LOGGED,            // FieldVault record only (unknown / known_invalid)
    FS_ACT_DELETED_TMP,       // .tmp/.partial/.bak orphan removed
    FS_ACT_QUARANTINED,       // moved to /spool_bad/quarantine/
    FS_ACT_REQUESTED_REBUILD, // delegated to existing rebuild machinery
    FS_ACT_SALVAGED,          // JSONL salvage rewrote the file keeping good lines
    FS_ACT_EVICTED,           // old quarantine entry removed under size cap
    FS_ACT_SKIPPED_LIMIT,     // would have acted but per-pass cap reached
    FS_ACT_FAILED             // attempted action but FS rejected it
};

struct FsAuditReport {
    uint16_t totalFiles      = 0;
    uint16_t totalDirs       = 0;
    uint16_t knownValid      = 0;
    uint16_t knownInvalid    = 0;
    uint16_t legacy          = 0;
    uint16_t forensic        = 0;
    uint16_t tmpOrphan       = 0;
    uint16_t unknown         = 0;
    uint16_t unknownVaulted  = 0;   // unknowns we actually emitted records for
    uint16_t skippedBudget   = 0;   // entries skipped because budget expired

    // Recovery action counters
    uint16_t deletedTmp      = 0;
    uint16_t quarantined     = 0;
    uint16_t rebuildRequests = 0;
    uint16_t salvaged        = 0;
    uint16_t evicted         = 0;
    uint16_t missingEssential = 0;   // must-exist files we did not see
    uint16_t actionsSkipped  = 0;    // hit per-pass action cap or guard
    uint16_t actionsFailed   = 0;    // FS rejected the action

    // Cross-reference signals — caller compares against its own indices.
    uint16_t spoolSegmentFilesSeen = 0;
    uint32_t spoolMaxSegmentIdSeen = 0;
    uint32_t quarantineBytesSeen   = 0;

    uint32_t bytesScanned    = 0;
    uint32_t durationMs      = 0;
    bool     completed       = false;  // true iff sweep walked the entire tree
};

// Mode ladder — mirrors SpoolRepairMode so the same global pressure signal
// drives both. Per-mode limits scale entry/action caps and time budget; the
// safety guards (heap floor, FS free-space floor, recursion depth) are
// constant across modes — emergency mode does *more* work, never less safe.
enum FsAuditMode : uint8_t {
    FS_AUDIT_BACKGROUND = 0,   // writer or heap pressure — minimal slice
    FS_AUDIT_NORMAL,           // idle / scheduled boot pass — modest slice
    FS_AUDIT_EMERGENCY         // recovery mode — largest slice
};

struct FsAuditLimits {
    uint32_t budgetMs            = 3000UL;
    uint16_t maxFiles            = 512;     // hard cap on entries visited
    uint16_t maxUnknownVaulted   = 16;      // FieldVault entries per pass
    uint16_t maxDeletes          = 16;      // tmp orphans removed per pass
    uint16_t maxQuarantines      = 8;       // bad files moved per pass
    uint16_t maxRebuildRequests  = 4;       // STORAGE_MAINT flag-set actions
    uint16_t maxSalvages         = 1;       // expensive — at most one per pass
    uint16_t maxEvictions        = 4;       // quarantine eviction cap per pass
    uint8_t  maxDepth            = 6;       // recursion guard (constant)
    uint32_t minFreeInternal     = 32UL * 1024UL;  // abort if heap drops below
    uint32_t minFreeFsBytes      = 64UL * 1024UL;  // skip writes when fs near full
    uint32_t maxQuarantineBytes  = 128UL * 1024UL; // /spool_bad/quarantine cap
    uint32_t maxSalvageInputBytes = 16UL * 1024UL; // skip salvage on bigger files
    bool     readOnly            = false;   // true → observe only (Phase 1 behavior)
};

// Mode → limits factory. Per-mode profile (recovery scales work, never safety):
//   BACKGROUND  budget=200ms  maxFiles=64   del=2  quar=1  vaulted=4   evict=1 salvage=0
//   NORMAL      budget=3000ms maxFiles=512  del=16 quar=8  vaulted=16  evict=4 salvage=1
//   EMERGENCY   budget=8000ms maxFiles=2048 del=64 quar=32 vaulted=32  evict=16 salvage=2
// Safety guards (heap floor, fs free, recursion depth, quarantine cap) constant.
FsAuditLimits limitsForMode(FsAuditMode mode);

// Walk LittleFS once with the given limits and fill `report`. Returns true if
// the sweep ran to completion (no budget/heap/limit cutoff). Idempotent: each
// call is independent — no state is carried between passes.
bool runWindow(const FsAuditLimits& limits, FsAuditReport& report);

// Helpers exposed for tests and for the maintenance owner that want to
// inspect a path without doing a full sweep.
FsClass classifyPath(const char* path);
const char* classText(FsClass c);

}  // namespace FsAudit
