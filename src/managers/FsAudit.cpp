
#include "FsAudit.h"

#include <LittleFS.h>
#include <esp_heap_caps.h>

#include "../core/DebugLog.h"
#include "../data/FieldVault.h"
#include "../data/Schema.h"
#include "StorageManager.h"

namespace FsAudit {

namespace {

constexpr uint8_t kHeaderProbeBytes = 8;
constexpr uint16_t kSmallFileMaxBytes = 8192;  // structural-validate ceiling

constexpr const char* kQuarantineDir = "/spool_bad/quarantine";
constexpr uint8_t kMaxPendingActions = 24;  // bound deferred action queue
constexpr uint8_t kMaxPathLen = 96;

// ── Path matching helpers ────────────────────────────────────────────────────

inline bool _eq(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}

inline bool _startsWith(const char* path, const char* prefix) {
    while (*prefix) {
        if (*path++ != *prefix++) return false;
    }
    return true;
}

inline bool _endsWith(const char* path, const char* suffix) {
    const size_t pl = strlen(path);
    const size_t sl = strlen(suffix);
    if (sl > pl) return false;
    return strcmp(path + (pl - sl), suffix) == 0;
}

// True if `path` lies directly under `dir` (no intermediate slash).
inline bool _isDirectChildOf(const char* path, const char* dir) {
    const size_t dl = strlen(dir);
    if (!_startsWith(path, dir)) return false;
    if (path[dl] != '/') return false;
    return strchr(path + dl + 1, '/') == nullptr;
}

// `seg_NNNNNN` — six-digit numeric segment id under a known dir. Returns true
// if the basename after `prefix` is exactly six decimal digits before the
// extension.
inline bool _matchSixDigit(const char* basename,
                           const char* prefix,
                           const char* extension) {
    if (!_startsWith(basename, prefix)) return false;
    const char* p = basename + strlen(prefix);
    for (int i = 0; i < 6; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
    }
    return strcmp(p + 6, extension) == 0;
}

inline const char* _basename(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// ── Cheap header validation ─────────────────────────────────────────────────

bool _peekHeader(const char* path, uint8_t* buf, size_t bufLen, size_t* outRead) {
    *outRead = 0;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    const size_t want = bufLen;
    const size_t got = f.read(buf, want);
    *outRead = got;
    f.close();
    return got > 0;
}

bool _looksLikeJson(const char* path, size_t fileSize) {
    if (fileSize == 0) return false;  // empty json is invalid
    uint8_t hdr[kHeaderProbeBytes] = {};
    size_t got = 0;
    if (!_peekHeader(path, hdr, sizeof(hdr), &got)) return false;
    // Skip leading whitespace
    size_t i = 0;
    while (i < got && (hdr[i] == ' ' || hdr[i] == '\t' ||
                       hdr[i] == '\r' || hdr[i] == '\n')) {
        ++i;
    }
    if (i >= got) return false;
    return hdr[i] == '{' || hdr[i] == '[';
}

bool _looksLikeJsonl(const char* path, size_t fileSize) {
    // JSONL: empty file is OK (just no records yet). If non-empty, first
    // non-whitespace byte should be '{'.
    if (fileSize == 0) return true;
    return _looksLikeJson(path, fileSize);
}

bool _looksLikeText(const char* path, size_t fileSize) {
    if (fileSize == 0) return true;
    uint8_t hdr[kHeaderProbeBytes] = {};
    size_t got = 0;
    if (!_peekHeader(path, hdr, sizeof(hdr), &got)) return false;
    for (size_t i = 0; i < got; ++i) {
        const uint8_t b = hdr[i];
        // Allow printable ASCII + common whitespace
        if (b >= 0x20 && b <= 0x7E) continue;
        if (b == '\t' || b == '\r' || b == '\n') continue;
        return false;
    }
    return true;
}

bool _looksLikeBinary(const char* /*path*/, size_t fileSize) {
    // Bin segments / pmkid_index — we only check non-empty here; format-aware
    // validation lives in StorageManager's spool audit.
    return fileSize > 0;
}

// ── Registry: classify path + optional cheap structural check ───────────────

// Paths the audit must never delete or quarantine, even on a failed validator.
// FieldVault is forensic evidence (and our own logging path); /config/vault is
// the persistent vault that survives wipes; the quarantine dir itself must
// not get re-quarantined on a future pass.
bool _isProtectedFromAction(const char* path) {
    if (!path) return true;
    if (_startsWith(path, "/config/vault/")) return true;
    if (_startsWith(path, "/spool_bad/")) return true;
    return false;
}

struct ClassifyResult {
    FsClass cls;
    bool    needsHeaderCheck;   // true → we should validate header
    bool    isSmallStructured;  // true → caller may run JSON/text check
};

ClassifyResult _matchSpec(const char* path) {
    // Forensic / quarantine — never touch
    if (_startsWith(path, "/spool_bad/")) {
        return {FS_CLASS_FORENSIC, false, false};
    }

    // Tmp/partial orphans — name-based classification
    if (_endsWith(path, ".tmp") || _endsWith(path, ".partial") ||
        _endsWith(path, ".bak")) {
        return {FS_CLASS_TMP_ORPHAN, false, false};
    }

    // Spool live tree
    if (_eq(path, "/spool/index.json")) {
        return {FS_CLASS_KNOWN_VALID, true, true};
    }
    if (_isDirectChildOf(path, "/spool")) {
        const char* base = _basename(path);
        if (_matchSixDigit(base, "seg_", ".bin") ||
            _matchSixDigit(base, "seg_", ".jsonl")) {
            return {FS_CLASS_KNOWN_VALID, true, false};
        }
        if (_matchSixDigit(base, "idx_", ".uix")) {
            return {FS_CLASS_KNOWN_VALID, false, false};
        }
        if (_endsWith(base, ".meta.json") &&
            _startsWith(base, "seg_")) {
            return {FS_CLASS_KNOWN_VALID, true, true};
        }
    }

    // Events
    if (_eq(path, PATH_EVENT_COUNTER)) {
        return {FS_CLASS_KNOWN_VALID, true, true};
    }
    if (_eq(path, PATH_EVENT_META)) {
        return {FS_CLASS_KNOWN_VALID, true, true};
    }
    if (_eq(path, PATH_STORE_VAULT_RESET_TAG)) {
        return {FS_CLASS_KNOWN_VALID, true, true};
    }
    if (_eq(path, PATH_STORE_NON_VAULT_RESET_TAG)) {
        return {FS_CLASS_KNOWN_VALID, true, true};
    }
    if (_eq(path, PATH_STORAGE_MAINT_LOG)) {
        return {FS_CLASS_KNOWN_VALID, true, false};
    }
    if (_eq(path, PATH_MQTT_LEGACY_MIGRATED_FLAG)) {
        return {FS_CLASS_KNOWN_VALID, false, true};
    }

    // Logs
    if (_eq(path, PATH_SESSIONS)) {
        return {FS_CLASS_KNOWN_VALID, true, true};
    }
    if (_isDirectChildOf(path, PATH_LOGS)) {
        const char* base = _basename(path);
        if (_eq(base, "debug.log") || _eq(base, "debug.log.1")) {
            return {FS_CLASS_KNOWN_VALID, false, false};
        }
        if ((_startsWith(base, "lora_") || _startsWith(base, "wifi_") ||
             _startsWith(base, "probe_")) && _endsWith(base, ".json")) {
            return {FS_CLASS_KNOWN_VALID, true, true};
        }
    }

    // Exports — known shape: /exports/index.jsonl, /exports/captures.hc22000,
    // and /exports/<sessionId>/... opaque files
    if (_eq(path, PATH_EXPORT_INDEX)) {
        return {FS_CLASS_KNOWN_VALID, true, true};
    }
    if (_eq(path, PATH_HC22000)) {
        return {FS_CLASS_KNOWN_VALID, true, false};
    }
    if (_startsWith(path, "/exports/")) {
        // Anything under a session dir is opaque to the audit (treated as
        // known-valid by virtue of being inside the export tree).
        return {FS_CLASS_KNOWN_VALID, false, false};
    }

    // PMKID artifacts
    if (_eq(path, PATH_PMKID_INDEX)) {
        return {FS_CLASS_KNOWN_VALID, true, false};
    }
    if (_isDirectChildOf(path, PATH_PMKID_DIR)) {
        const char* base = _basename(path);
        if (_endsWith(base, ".hc22000")) {
            return {FS_CLASS_KNOWN_VALID, true, false};
        }
    }

    // Vault — config/vault tree (persistent, survives wipes)
    if (_eq(path, "/config/vault/known_locations.json")) {
        return {FS_CLASS_KNOWN_VALID, true, true};
    }
    if (_eq(path, PATH_BADUSB_INDEX)) {
        return {FS_CLASS_KNOWN_VALID, true, true};
    }
    if (_isDirectChildOf(path, PATH_BADUSB_DIR)) {
        // Imported scripts — opaque text
        return {FS_CLASS_KNOWN_VALID, false, false};
    }
    if (_eq(path, PATH_FIELDVAULT_LOG) ||
        _eq(path, PATH_FIELDVAULT_BAK)) {
        return {FS_CLASS_KNOWN_VALID, true, true};
    }

    // Legacy paths — known shape, scheduled for removal in a future phase
    if (_eq(path, "/config/locations.json") ||
        _startsWith(path, "/mqtt_queue/") ||
        _startsWith(path, "/pmkid/") ||
        _startsWith(path, "/vault/")) {
        return {FS_CLASS_LEGACY, false, false};
    }

    return {FS_CLASS_UNKNOWN, false, false};
}

// ── Deferred-action queue ───────────────────────────────────────────────────
//
// We can't safely delete/rename while an enclosing dir iterator is open, so
// the walker enqueues actions and the executor drains the queue after the
// walk finishes. The queue is fixed-size to keep allocation off the heap.

enum DeferredKind : uint8_t {
    DEF_DELETE_TMP,
    DEF_QUARANTINE,
    DEF_REQUEST_REBUILD,    // path is descriptive only; flag is in `flag`
    DEF_REQUEST_FLAG,       // generic flag-set for missing essentials
    DEF_SALVAGE_JSONL       // line-by-line JSONL recovery
};

struct DeferredAction {
    DeferredKind kind;
    char         path[kMaxPathLen];
    uint32_t     sizeBytes;
    uint32_t     flag;       // STORAGE_MAINT_* bit when kind == DEF_REQUEST_*
    char         reason[24];
};

struct ActionQueue {
    DeferredAction items[kMaxPendingActions];
    uint8_t        count = 0;

    bool push(DeferredKind kind, const char* path,
              uint32_t sizeBytes, const char* reason,
              uint32_t flag = 0) {
        if (count >= kMaxPendingActions) return false;
        DeferredAction& a = items[count++];
        a.kind = kind;
        a.sizeBytes = sizeBytes;
        a.flag = flag;
        strlcpy(a.path, path ? path : "", sizeof(a.path));
        strlcpy(a.reason, reason ? reason : "", sizeof(a.reason));
        return true;
    }
};

// ── Must-exist registry ─────────────────────────────────────────────────────
//
// Files whose absence is a real problem. We delegate recreation to the data
// owner via the existing maintenance flags rather than guessing schemas. The
// owner subsystems already know how to write these files atomically.
//
// Files NOT in this list (e.g. /config/vault/known_locations.json,
// /config/vault/badusb/index.json) are intentionally absent until first
// write — their loaders treat absence as "no entries" and recreate empties
// on demand. Adding them here would create false-positive missing reports.

struct MustExistEntry {
    const char* path;
    uint32_t    rebuildFlag;     // STORAGE_MAINT_* bit to request when missing
    const char* reason;          // FieldVault detail string
};

const MustExistEntry kMustExist[] = {
    {"/spool/index.json",  STORAGE_MAINT_DIRTY_SPOOL_INDEX,
     "spool_index_missing"},
    {PATH_EVENT_COUNTER,   STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST,
     "event_counter_missing"},
    {PATH_EVENT_META,      STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST,
     "event_meta_missing"},
};
constexpr uint8_t kMustExistCount =
    sizeof(kMustExist) / sizeof(kMustExist[0]);

// Match a path against the must-exist table. Returns -1 if not in the table.
int8_t _mustExistIndex(const char* path) {
    if (!path) return -1;
    for (uint8_t i = 0; i < kMustExistCount; ++i) {
        if (strcmp(path, kMustExist[i].path) == 0) {
            return static_cast<int8_t>(i);
        }
    }
    return -1;
}

// ── Recursive walker ────────────────────────────────────────────────────────

struct WalkCtx {
    const FsAuditLimits* limits;
    FsAuditReport*       report;
    ActionQueue*         queue;
    uint32_t             startMs;
    uint16_t             queuedDeletes;      // delete actions enqueued so far
    uint16_t             queuedQuarantines;  // quarantine actions enqueued so far
    uint16_t             queuedRebuildReqs;  // flag-request actions enqueued
    uint16_t             queuedSalvages;     // salvage actions enqueued
    uint16_t             seenMustExistMask;  // bit per kMustExist[] entry
    bool                 budgetExpired;
    bool                 limitReached;
};

bool _heapStillSafe(const FsAuditLimits& limits) {
    const uint32_t freeInternal =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return freeInternal >= limits.minFreeInternal;
}

bool _budgetLeft(const WalkCtx& ctx) {
    if (ctx.limitReached || ctx.budgetExpired) return false;
    return (millis() - ctx.startMs) < ctx.limits->budgetMs;
}

void _classifyAndAccount(const char* path,
                         size_t fileSize,
                         WalkCtx& ctx) {
    ctx.report->totalFiles++;
    ctx.report->bytesScanned += fileSize;

    ClassifyResult cr = _matchSpec(path);

    // Cheap structural validation for small known files
    if (cr.cls == FS_CLASS_KNOWN_VALID && cr.needsHeaderCheck) {
        bool valid = true;
        if (cr.isSmallStructured && fileSize <= kSmallFileMaxBytes) {
            // JSON / JSONL / text — peek + format-by-extension
            if (_endsWith(path, ".json") || _endsWith(path, ".meta.json")) {
                valid = _looksLikeJson(path, fileSize);
            } else if (_endsWith(path, ".jsonl")) {
                valid = _looksLikeJsonl(path, fileSize);
            } else if (_endsWith(path, ".txt")) {
                valid = _looksLikeText(path, fileSize);
            }
        } else {
            // Big file — just confirm non-empty existence (already opened)
            valid = _looksLikeBinary(path, fileSize);
        }
        if (!valid) {
            cr.cls = FS_CLASS_KNOWN_INVALID;
        }
    }

    // Over-rotation safety net: /exports/index.jsonl is JSONL with no
    // upstream rotation. If it grows past the salvage input cap, attempt a
    // bounded line-keep salvage so future writes stay healthy. Triggered as
    // a known-valid file, not as known-invalid.
    if (cr.cls == FS_CLASS_KNOWN_VALID &&
        !ctx.limits->readOnly &&
        _eq(path, PATH_EXPORT_INDEX) &&
        fileSize > ctx.limits->maxSalvageInputBytes &&
        fileSize <= 2U * ctx.limits->maxSalvageInputBytes &&
        ctx.queuedSalvages < ctx.limits->maxSalvages) {
        if (ctx.queue->push(DEF_SALVAGE_JSONL, path, fileSize,
                            "over_rotation")) {
            ctx.queuedSalvages++;
        }
    }

    // Cross-reference signals — counters the caller compares against its
    // own indices. Only valid known files contribute; we don't want a
    // quarantined segment to look like a live one.
    if (cr.cls == FS_CLASS_KNOWN_VALID) {
        if (_isDirectChildOf(path, "/spool")) {
            const char* base = _basename(path);
            if (_matchSixDigit(base, "seg_", ".bin") ||
                _matchSixDigit(base, "seg_", ".jsonl")) {
                ctx.report->spoolSegmentFilesSeen++;
                // Parse the 6-digit segment id directly from the basename.
                uint32_t segId = 0;
                const char* digits = base + 4;  // past "seg_"
                for (int i = 0; i < 6; ++i) {
                    segId = segId * 10U + static_cast<uint32_t>(digits[i] - '0');
                }
                if (segId > ctx.report->spoolMaxSegmentIdSeen) {
                    ctx.report->spoolMaxSegmentIdSeen = segId;
                }
            }
        }
        // Mark must-exist as seen so we don't enqueue a missing-file action.
        const int8_t mei = _mustExistIndex(path);
        if (mei >= 0) {
            ctx.seenMustExistMask |= static_cast<uint16_t>(1U << mei);
        }
    }

    switch (cr.cls) {
        case FS_CLASS_KNOWN_VALID:    ctx.report->knownValid++;   break;
        case FS_CLASS_KNOWN_INVALID:
            ctx.report->knownInvalid++;
            DLOG_WARN("FSAUDIT",
                      "known-invalid path=%s size=%lu",
                      path,
                      static_cast<unsigned long>(fileSize));
            (void)FieldVault::appendFsAuditInvalid(path, fileSize, "header_invalid");
            // /spool/index.json gets handed off to the existing rebuild path
            // rather than quarantined; its content is reconstructible from
            // the segment files.
            if (!ctx.limits->readOnly) {
                if (_isProtectedFromAction(path)) {
                    // Vault / forensic / quarantine — log only, never act.
                    ctx.report->actionsSkipped++;
                } else if (_eq(path, "/spool/index.json")) {
                    if (ctx.queuedRebuildReqs < ctx.limits->maxRebuildRequests &&
                        ctx.queue->push(DEF_REQUEST_REBUILD, path, fileSize,
                                        "spool_index_invalid",
                                        STORAGE_MAINT_DIRTY_SPOOL_INDEX)) {
                        ctx.queuedRebuildReqs++;
                    } else {
                        ctx.report->actionsSkipped++;
                    }
                } else if (_eq(path, PATH_EXPORT_INDEX) &&
                           fileSize <= ctx.limits->maxSalvageInputBytes &&
                           ctx.queuedSalvages < ctx.limits->maxSalvages) {
                    // JSONL where corruption may be local to a few lines —
                    // try line-by-line salvage before resorting to quarantine.
                    if (ctx.queue->push(DEF_SALVAGE_JSONL, path, fileSize,
                                        "header_invalid_jsonl")) {
                        ctx.queuedSalvages++;
                    } else {
                        ctx.report->actionsSkipped++;
                    }
                } else if (ctx.queuedQuarantines < ctx.limits->maxQuarantines) {
                    if (ctx.queue->push(DEF_QUARANTINE, path, fileSize,
                                        "header_invalid")) {
                        ctx.queuedQuarantines++;
                    } else {
                        ctx.report->actionsSkipped++;
                    }
                } else {
                    ctx.report->actionsSkipped++;
                }
            }
            break;
        case FS_CLASS_LEGACY:         ctx.report->legacy++;       break;
        case FS_CLASS_FORENSIC:       ctx.report->forensic++;     break;
        case FS_CLASS_TMP_ORPHAN:
            ctx.report->tmpOrphan++;
            DLOG_WARN("FSAUDIT",
                      "tmp/orphan path=%s size=%lu",
                      path,
                      static_cast<unsigned long>(fileSize));
            (void)FieldVault::appendFsAuditInvalid(path, fileSize, "tmp_orphan");
            if (!ctx.limits->readOnly) {
                if (_isProtectedFromAction(path)) {
                    // Vault tmp files are owned by FieldVault rotation; do not
                    // delete out from under it.
                    ctx.report->actionsSkipped++;
                } else if (ctx.queuedDeletes < ctx.limits->maxDeletes) {
                    if (ctx.queue->push(DEF_DELETE_TMP, path, fileSize,
                                        "tmp_orphan")) {
                        ctx.queuedDeletes++;
                    } else {
                        ctx.report->actionsSkipped++;
                    }
                } else {
                    ctx.report->actionsSkipped++;
                }
            }
            break;
        case FS_CLASS_UNKNOWN:
            ctx.report->unknown++;
            if (ctx.report->unknownVaulted < ctx.limits->maxUnknownVaulted) {
                if (FieldVault::appendFsAuditUnknown(path, fileSize)) {
                    ctx.report->unknownVaulted++;
                }
            }
            DLOG_WARN("FSAUDIT",
                      "unknown path=%s size=%lu",
                      path,
                      static_cast<unsigned long>(fileSize));
            break;
        case FS_CLASS_UNCLASSIFIED:
        default:                      break;
    }
}

void _walk(const char* dirPath, uint8_t depth, WalkCtx& ctx) {
    if (!_budgetLeft(ctx)) return;
    if (depth > ctx.limits->maxDepth) return;
    if (!_heapStillSafe(*ctx.limits)) {
        ctx.budgetExpired = true;
        DLOG_WARN("FSAUDIT", "abort dir=%s reason=heap_guard", dirPath);
        return;
    }

    File dir = LittleFS.open(dirPath);
    if (!dir) return;
    if (!dir.isDirectory()) {
        dir.close();
        return;
    }
    ctx.report->totalDirs++;

    File entry = dir.openNextFile();
    while (entry) {
        // entry.name() returns a basename on some LittleFS builds and a full
        // path on others. Normalize to an absolute path so the spec matcher
        // sees consistent input.
        const char* rawName = entry.name();
        String namePath;
        if (rawName && rawName[0] == '/') {
            namePath = rawName;
        } else {
            namePath = String(dirPath);
            if (namePath.length() == 0 || namePath[namePath.length() - 1] != '/') {
                namePath += '/';
            }
            namePath += (rawName ? rawName : "");
        }
        const bool isDir = entry.isDirectory();
        const size_t sz = isDir ? 0 : entry.size();
        entry.close();

        if (!_budgetLeft(ctx)) {
            ctx.report->skippedBudget++;
            entry = File();
            break;
        }
        if (ctx.report->totalFiles >= ctx.limits->maxFiles) {
            ctx.limitReached = true;
            ctx.report->skippedBudget++;
            entry = File();
            break;
        }

        if (isDir) {
            _walk(namePath.c_str(), depth + 1, ctx);
        } else {
            _classifyAndAccount(namePath.c_str(), sz, ctx);
        }

        if (!_budgetLeft(ctx)) {
            entry = File();
            break;
        }
        entry = dir.openNextFile();
    }
    dir.close();
}

// ── Action executor ─────────────────────────────────────────────────────────
//
// Runs after the walk so we never mutate while a parent dir iterator is open.
// Each action emits a fs_audit_action FieldVault record describing the
// outcome. Free-space / heap guards block destructive actions when the FS or
// heap is in trouble.

// ── JSONL salvager ─────────────────────────────────────────────────────────
//
// Reads the source file line by line, keeps lines that look like complete
// JSON objects (start with '{', end with '}' before newline), drops the rest.
// Writes to a tmp; on success, original goes to .bak (forensic copy) and
// tmp renames over the original. Bounded by stack buffer size; lines longer
// than the buffer are dropped (they were already malformed).
//
// Returns true if salvage produced a file (even an empty one). The caller
// inspects the kept-line count to decide whether the salvage was useful.

constexpr size_t kSalvageLineMax = 512;  // longer lines are dropped

bool _salvageJsonlFile(const char* srcPath,
                       uint16_t& keptOut,
                       uint16_t& droppedOut,
                       const char*& failReasonOut) {
    keptOut = 0;
    droppedOut = 0;
    failReasonOut = nullptr;

    String src(srcPath);
    String tmp = src + ".salv";
    String bak = src + ".bak";

    File in = LittleFS.open(src, "r");
    if (!in) {
        failReasonOut = "src_open";
        return false;
    }
    if (LittleFS.exists(tmp)) LittleFS.remove(tmp);
    File out = LittleFS.open(tmp, "w");
    if (!out) {
        in.close();
        failReasonOut = "tmp_open";
        return false;
    }

    char buf[kSalvageLineMax];
    while (in.available()) {
        const size_t n = in.readBytesUntil('\n', buf, sizeof(buf) - 1);
        if (n == 0) break;
        buf[n] = '\0';

        // readBytesUntil returns short of `sizeof(buf) - 1` only when it
        // consumed the delimiter; if it filled the buffer, the newline was
        // never reached. Drop the entire oversized record (drain through
        // its terminator) instead of letting the leading chunk pass the
        // `{`/`}` shape check while subsequent chunks of the same record
        // get counted as separate lines.
        if (n == sizeof(buf) - 1) {
            droppedOut++;
            int c;
            while ((c = in.read()) >= 0 && c != '\n') {
                // discard
            }
            continue;
        }

        // Strip trailing \r if present (CRLF tolerance).
        size_t len = n;
        if (len > 0 && buf[len - 1] == '\r') {
            buf[--len] = '\0';
        }

        // Cheap shape check: must open with '{' and close with '}'.
        if (len >= 2 && buf[0] == '{' && buf[len - 1] == '}') {
            const size_t wrote =
                out.write(reinterpret_cast<const uint8_t*>(buf), len);
            const size_t wroteNl = out.write(static_cast<uint8_t>('\n'));
            if (wrote != len || wroteNl != 1) {
                // Partial write — tmp is now corrupt. Abort: leave original
                // untouched so the next pass can try again.
                out.close();
                in.close();
                LittleFS.remove(tmp);
                failReasonOut = "write_short";
                return false;
            }
            keptOut++;
        } else {
            droppedOut++;
        }
    }
    out.flush();
    out.close();
    in.close();

    // Promote .bak → backup of original; tmp → live name.
    if (LittleFS.exists(bak)) LittleFS.remove(bak);
    if (!LittleFS.rename(src, bak)) {
        LittleFS.remove(tmp);
        failReasonOut = "bak_rename";
        return false;
    }
    if (!LittleFS.rename(tmp, src)) {
        // Try to undo the bak rename so the live file survives.
        (void)LittleFS.rename(bak, src);
        LittleFS.remove(tmp);
        failReasonOut = "live_rename";
        return false;
    }
    return true;
}

bool _removePathRetry(const char* path) {
    for (int i = 0; i < 4; ++i) {
        if (LittleFS.remove(path)) return true;
        delay(2);
    }
    return false;
}

bool _ensureDirChain(const char* path) {
    // Create intermediate dirs walking left to right. /spool_bad/quarantine
    // for example: ensure /spool_bad exists, then quarantine.
    if (!path || !path[0] || strcmp(path, "/") == 0) return true;
    String p(path);
    int slash = 0;
    while ((slash = p.indexOf('/', slash + 1)) >= 0) {
        String parent = p.substring(0, slash);
        if (parent.length() == 0) continue;
        if (!LittleFS.exists(parent)) {
            (void)LittleFS.mkdir(parent);
        }
    }
    if (!LittleFS.exists(p)) {
        return LittleFS.mkdir(p);
    }
    return true;
}

// Translate "/spool/seg_000123.bin" →
//   "/spool_bad/quarantine/<seq8hex>_<idx2hex>__spool__seg_000123.bin"
//
// The 32-bit hex prefix is FieldVault::nextSeq() (monotonic across reboots
// because FieldVault scans its log on boot to recover seq). The 2-hex
// per-pass index disambiguates multiple quarantines within the same
// FieldVault seq window. Lex sort over the destination dir = age sort,
// which the evictor uses to pick oldest-first.
void _quarantineDestPath(const char* src,
                         uint32_t seq,
                         uint8_t idxInPass,
                         char* out,
                         size_t outSize) {
    if (outSize == 0) return;
    out[0] = '\0';
    if (!src) return;
    size_t o = 0;
    const char* prefix = kQuarantineDir;
    const size_t pl = strlen(prefix);
    if (pl + 14 >= outSize) return;
    memcpy(out, prefix, pl);
    o = pl;
    out[o++] = '/';
    const int wrote = snprintf(out + o, outSize - o,
                               "%08lx_%02x__",
                               static_cast<unsigned long>(seq),
                               static_cast<unsigned>(idxInPass));
    if (wrote <= 0 || static_cast<size_t>(wrote) >= outSize - o) {
        out[o] = '\0';
        return;
    }
    o += static_cast<size_t>(wrote);

    // Skip leading slash on src
    const char* p = (src[0] == '/') ? src + 1 : src;
    while (*p && o + 2 < outSize) {
        if (*p == '/') {
            out[o++] = '_';
            if (o + 2 >= outSize) break;
            out[o++] = '_';
        } else {
            out[o++] = *p;
        }
        ++p;
    }
    out[o] = '\0';
}

// If destination already exists, append .1, .2, ... up to .9 to find a
// free slot. Returns true if `out` ends up as a path that does not exist.
bool _resolveQuarantineCollision(char* path, size_t pathSize) {
    if (!LittleFS.exists(path)) return true;
    const size_t baseLen = strlen(path);
    if (baseLen + 3 >= pathSize) return false;
    for (char suffix = '1'; suffix <= '9'; ++suffix) {
        path[baseLen]     = '.';
        path[baseLen + 1] = suffix;
        path[baseLen + 2] = '\0';
        if (!LittleFS.exists(path)) return true;
    }
    path[baseLen] = '\0';
    return false;
}

bool _fsHasFreeBytes(uint32_t minBytes) {
    const uint64_t total = LittleFS.totalBytes();
    const uint64_t used = LittleFS.usedBytes();
    if (used > total) return false;
    return (total - used) >= minBytes;
}

bool _heapStillSafeStrict(const FsAuditLimits& limits) {
    const uint32_t freeInternal =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return freeInternal >= limits.minFreeInternal;
}

void _executeActions(const FsAuditLimits& limits,
                     const ActionQueue& queue,
                     FsAuditReport& report,
                     uint32_t startMs) {
    uint8_t quarantineIdx = 0;  // disambiguates multiple quarantines in one pass
    for (uint8_t i = 0; i < queue.count; ++i) {
        const DeferredAction& a = queue.items[i];

        // Guards re-checked per action — heap, free space, and elapsed
        // budget can all change between actions (especially after a
        // quarantine that doubled a file's footprint).
        if (!_heapStillSafeStrict(limits)) {
            DLOG_WARN("FSAUDIT",
                      "executor abort reason=heap_guard pending=%u",
                      static_cast<unsigned>(queue.count - i));
            report.actionsSkipped += (queue.count - i);
            (void)FieldVault::appendFsAuditAction(a.path, "abort", "heap_guard");
            return;
        }
        if ((millis() - startMs) >= limits.budgetMs) {
            DLOG_WARN("FSAUDIT",
                      "executor abort reason=budget pending=%u",
                      static_cast<unsigned>(queue.count - i));
            report.actionsSkipped += (queue.count - i);
            (void)FieldVault::appendFsAuditAction(a.path, "abort", "budget");
            return;
        }

        // Defense in depth: classifier should already have skipped these,
        // but reject any destructive action on protected paths here too.
        if ((a.kind == DEF_DELETE_TMP || a.kind == DEF_QUARANTINE) &&
            _isProtectedFromAction(a.path)) {
            report.actionsSkipped++;
            (void)FieldVault::appendFsAuditAction(a.path, "skip", "protected_path");
            continue;
        }

        switch (a.kind) {
            case DEF_DELETE_TMP: {
                if (_removePathRetry(a.path)) {
                    report.deletedTmp++;
                    (void)FieldVault::appendFsAuditAction(a.path, "delete_tmp", a.reason);
                } else {
                    report.actionsFailed++;
                    (void)FieldVault::appendFsAuditAction(a.path, "delete_failed", a.reason);
                }
                break;
            }

            case DEF_QUARANTINE: {
                if (!_fsHasFreeBytes(limits.minFreeFsBytes + a.sizeBytes)) {
                    report.actionsSkipped++;
                    (void)FieldVault::appendFsAuditAction(a.path, "quarantine_skip", "fs_low");
                    break;
                }
                if (!_ensureDirChain(kQuarantineDir)) {
                    report.actionsFailed++;
                    (void)FieldVault::appendFsAuditAction(a.path, "quarantine_failed", "mkdir");
                    break;
                }
                char dest[kMaxPathLen];
                _quarantineDestPath(a.path, FieldVault::nextSeq(),
                                    quarantineIdx++, dest, sizeof(dest));
                if (dest[0] == '\0' || !_resolveQuarantineCollision(dest, sizeof(dest))) {
                    report.actionsFailed++;
                    (void)FieldVault::appendFsAuditAction(a.path, "quarantine_failed", "name_collision");
                    break;
                }
                if (LittleFS.rename(a.path, dest)) {
                    report.quarantined++;
                    (void)FieldVault::appendFsAuditAction(a.path, "quarantine", dest);
                } else {
                    report.actionsFailed++;
                    (void)FieldVault::appendFsAuditAction(a.path, "quarantine_failed", "rename");
                }
                break;
            }

            case DEF_REQUEST_REBUILD: {
                STORAGE.requestMaintenance(
                    static_cast<StorageMaintenanceReason>(
                        a.flag ? a.flag : STORAGE_MAINT_DIRTY_SPOOL_INDEX),
                    "fs_audit_rebuild");
                report.rebuildRequests++;
                (void)FieldVault::appendFsAuditAction(a.path, "request_rebuild", a.reason);
                break;
            }

            case DEF_REQUEST_FLAG: {
                // Generic flag-set used for missing essentials. The owner
                // subsystem will recreate the file with its current schema
                // when the flag is consumed in a later maintenance window.
                if (a.flag != 0U) {
                    STORAGE.requestMaintenance(
                        static_cast<StorageMaintenanceReason>(a.flag),
                        a.reason);
                    report.rebuildRequests++;
                    (void)FieldVault::appendFsAuditAction(a.path, "request_flag", a.reason);
                } else {
                    report.actionsFailed++;
                    (void)FieldVault::appendFsAuditAction(a.path, "request_flag_failed", "no_flag");
                }
                break;
            }

            case DEF_SALVAGE_JSONL: {
                if (!_fsHasFreeBytes(limits.minFreeFsBytes + a.sizeBytes)) {
                    report.actionsSkipped++;
                    (void)FieldVault::appendFsAuditAction(a.path, "salvage_skip", "fs_low");
                    break;
                }
                uint16_t kept = 0;
                uint16_t dropped = 0;
                const char* failReason = nullptr;
                const bool ok = _salvageJsonlFile(a.path, kept, dropped, failReason);
                if (ok) {
                    report.salvaged++;
                    char detail[40];
                    snprintf(detail, sizeof(detail), "kept=%u drop=%u",
                             static_cast<unsigned>(kept),
                             static_cast<unsigned>(dropped));
                    (void)FieldVault::appendFsAuditAction(a.path, "salvage", detail);
                } else {
                    report.actionsFailed++;
                    (void)FieldVault::appendFsAuditAction(a.path, "salvage_failed",
                                                          failReason ? failReason : "unknown");
                }
                break;
            }
        }
    }
}

// ── Quarantine evictor ──────────────────────────────────────────────────────
//
// Walks /spool_bad/quarantine/ once, totals sizes. When over the byte cap,
// removes oldest entries (lex sort = age sort due to monotonic seq prefix in
// quarantine names). Bounded by limits.maxEvictions and a fixed-size
// in-memory entry list so heap is not at risk.

constexpr uint8_t kEvictorMaxEntries = 16;

struct EvictEntry {
    char     name[kMaxPathLen];   // basename only
    uint32_t sizeBytes;
};

void _evictQuarantineIfOverCap(const FsAuditLimits& limits,
                               FsAuditReport& report,
                               uint32_t startMs) {
    if ((millis() - startMs) >= limits.budgetMs) return;
    if (limits.maxEvictions == 0) return;

    File dir = LittleFS.open(kQuarantineDir);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }

    EvictEntry entries[kEvictorMaxEntries];
    uint8_t entryCount = 0;
    uint32_t totalBytes = 0;

    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            const size_t sz = entry.size();
            totalBytes += sz;
            if (entryCount < kEvictorMaxEntries) {
                const char* rawName = entry.name();
                const char* base = rawName;
                if (const char* slash = strrchr(rawName, '/')) {
                    base = slash + 1;
                }
                strlcpy(entries[entryCount].name, base,
                        sizeof(entries[entryCount].name));
                entries[entryCount].sizeBytes = sz;
                entryCount++;
            }
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();

    report.quarantineBytesSeen = totalBytes;

    if (totalBytes <= limits.maxQuarantineBytes) return;
    if (entryCount == 0) return;

    // Insertion-sort entries by name ascending (lex = age oldest first).
    for (uint8_t i = 1; i < entryCount; ++i) {
        EvictEntry tmp = entries[i];
        int8_t j = static_cast<int8_t>(i) - 1;
        while (j >= 0 && strcmp(entries[j].name, tmp.name) > 0) {
            entries[j + 1] = entries[j];
            --j;
        }
        entries[j + 1] = tmp;
    }

    uint16_t evicted = 0;
    for (uint8_t i = 0;
         i < entryCount &&
         evicted < limits.maxEvictions &&
         totalBytes > limits.maxQuarantineBytes;
         ++i) {
        if ((millis() - startMs) >= limits.budgetMs) break;
        if (!_heapStillSafeStrict(limits)) break;

        char fullPath[kMaxPathLen];
        snprintf(fullPath, sizeof(fullPath), "%s/%s",
                 kQuarantineDir, entries[i].name);
        if (_removePathRetry(fullPath)) {
            totalBytes = (totalBytes >= entries[i].sizeBytes)
                             ? totalBytes - entries[i].sizeBytes
                             : 0U;
            evicted++;
            report.evicted++;
            (void)FieldVault::appendFsAuditAction(fullPath, "evict", "size_cap");
        } else {
            report.actionsFailed++;
            (void)FieldVault::appendFsAuditAction(fullPath, "evict_failed", "rm");
        }
    }
}

// ── Missing-essential enqueueing ────────────────────────────────────────────
//
// After the walk, compare seenMustExistMask against kMustExist. For each
// unseen entry, enqueue a flag-request action that lets the owner subsystem
// recreate the file with its current schema. Bounded by maxRebuildRequests.

void _enqueueMissingEssentials(WalkCtx& ctx) {
    for (uint8_t i = 0; i < kMustExistCount; ++i) {
        const uint16_t bit = static_cast<uint16_t>(1U << i);
        if (ctx.seenMustExistMask & bit) continue;
        ctx.report->missingEssential++;
        DLOG_WARN("FSAUDIT",
                  "must-exist missing path=%s flag=0x%lx",
                  kMustExist[i].path,
                  static_cast<unsigned long>(kMustExist[i].rebuildFlag));
        (void)FieldVault::appendFsAuditInvalid(kMustExist[i].path, 0,
                                               kMustExist[i].reason);
        if (ctx.limits->readOnly) continue;
        if (ctx.queuedRebuildReqs >= ctx.limits->maxRebuildRequests) {
            ctx.report->actionsSkipped++;
            continue;
        }
        if (ctx.queue->push(DEF_REQUEST_FLAG, kMustExist[i].path, 0,
                            kMustExist[i].reason,
                            kMustExist[i].rebuildFlag)) {
            ctx.queuedRebuildReqs++;
        } else {
            ctx.report->actionsSkipped++;
        }
    }
}

}  // namespace

FsAuditLimits limitsForMode(FsAuditMode mode) {
    FsAuditLimits l{};
    // Defaults above match NORMAL; only tweak the per-mode dials. Safety
    // guards (minFreeInternal, minFreeFsBytes, maxDepth) stay constant — the
    // ladder scales work, never safety.
    switch (mode) {
        case FS_AUDIT_BACKGROUND:
            l.budgetMs            = 200UL;
            l.maxFiles            = 64;
            l.maxUnknownVaulted   = 4;
            l.maxDeletes          = 2;
            l.maxQuarantines      = 1;
            l.maxRebuildRequests  = 2;
            l.maxSalvages         = 0;     // skip salvage under pressure
            l.maxEvictions        = 1;
            break;
        case FS_AUDIT_EMERGENCY:
            l.budgetMs            = 8000UL;
            l.maxFiles            = 2048;
            l.maxUnknownVaulted   = 32;
            l.maxDeletes          = 64;
            l.maxQuarantines      = 32;
            l.maxRebuildRequests  = 8;
            l.maxSalvages         = 2;
            l.maxEvictions        = 16;
            break;
        case FS_AUDIT_NORMAL:
        default:
            // already-default values
            break;
    }
    return l;
}

const char* classText(FsClass c) {
    switch (c) {
        case FS_CLASS_KNOWN_VALID:   return "known_valid";
        case FS_CLASS_KNOWN_INVALID: return "known_invalid";
        case FS_CLASS_LEGACY:        return "legacy";
        case FS_CLASS_FORENSIC:      return "forensic";
        case FS_CLASS_TMP_ORPHAN:    return "tmp_orphan";
        case FS_CLASS_UNKNOWN:       return "unknown";
        case FS_CLASS_UNCLASSIFIED:
        default:                     return "unclassified";
    }
}

FsClass classifyPath(const char* path) {
    if (!path || !path[0]) return FS_CLASS_UNCLASSIFIED;
    return _matchSpec(path).cls;
}

bool runWindow(const FsAuditLimits& limits, FsAuditReport& report) {
    report = FsAuditReport{};
    if (!_heapStillSafe(limits)) {
        DLOG_WARN("FSAUDIT", "skip pass reason=heap_guard");
        return false;
    }

    ActionQueue queue;
    WalkCtx ctx{};
    ctx.limits = &limits;
    ctx.report = &report;
    ctx.queue = &queue;
    ctx.startMs = millis();
    ctx.queuedDeletes = 0;
    ctx.queuedQuarantines = 0;
    ctx.queuedRebuildReqs = 0;
    ctx.queuedSalvages = 0;
    ctx.seenMustExistMask = 0;
    ctx.budgetExpired = false;
    ctx.limitReached = false;

    _walk("/", 0, ctx);

    // Post-walk: enqueue flag-requests for any must-exist file we did not see.
    // Done before the executor so missing-file requests share the same budget
    // and FieldVault audit ordering as walk-discovered actions.
    _enqueueMissingEssentials(ctx);

    if (!limits.readOnly && queue.count > 0) {
        _executeActions(limits, queue, report, ctx.startMs);
    }

    // Quarantine eviction is independent of the action queue — it only walks
    // /spool_bad/quarantine/. Runs only when budget remains and never under
    // readOnly.
    if (!limits.readOnly) {
        _evictQuarantineIfOverCap(limits, report, ctx.startMs);
    }

    report.durationMs = millis() - ctx.startMs;
    report.completed = !ctx.budgetExpired && !ctx.limitReached;

    DLOG_INFO("FSAUDIT",
              "pass done files=%u dirs=%u valid=%u invalid=%u legacy=%u "
              "forensic=%u tmp=%u unknown=%u vaulted=%u missing=%u skipped=%u "
              "actions del=%u quar=%u rebuild=%u salv=%u evict=%u skip=%u fail=%u "
              "spool_seg=%u maxSegId=%lu quarBytes=%lu "
              "bytes=%lu ms=%lu complete=%d",
              report.totalFiles, report.totalDirs,
              report.knownValid, report.knownInvalid,
              report.legacy, report.forensic, report.tmpOrphan,
              report.unknown, report.unknownVaulted, report.missingEssential,
              report.skippedBudget,
              report.deletedTmp, report.quarantined, report.rebuildRequests,
              report.salvaged, report.evicted,
              report.actionsSkipped, report.actionsFailed,
              report.spoolSegmentFilesSeen,
              static_cast<unsigned long>(report.spoolMaxSegmentIdSeen),
              static_cast<unsigned long>(report.quarantineBytesSeen),
              static_cast<unsigned long>(report.bytesScanned),
              static_cast<unsigned long>(report.durationMs),
              report.completed ? 1 : 0);

    (void)FieldVault::appendFsAuditSummary(report.totalFiles,
                                           report.knownValid,
                                           report.knownInvalid,
                                           report.legacy,
                                           report.tmpOrphan,
                                           report.unknown,
                                           report.bytesScanned,
                                           report.durationMs,
                                           report.completed);

    return report.completed;
}

}  // namespace FsAudit
