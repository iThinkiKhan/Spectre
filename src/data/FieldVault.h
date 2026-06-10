
#pragma once

#include <Arduino.h>

// FieldVault — compact persistent record of high-value field-test evidence.
//
// Records are appended to a single JSONL file under /config/vault/field/. Each
// line is one tiny JSON object with a "type" tag (boot/crash/upload_*/etc.).
// FieldVault is intentionally NOT a debug log: routine DLOG_* chatter must
// continue to go through DebugLog and obey the debug profile. FieldVault
// captures only events worth preserving across reboots and uploads.
//
// Design constraints (see plan):
//   - No dynamic allocation. Records serialize with snprintf into a stack
//     buffer (capped at kLineMax bytes).
//   - No long debug strings. Crash and boot records are short, fixed-shape.
//   - Writes are append-only. Power loss may leave a partial trailing line;
//     readers must tolerate truncated lines.
//   - Size-based rotation at kRotateBytes; one backup retained.
//
namespace FieldVault {

bool begin();
bool isReady();
uint32_t nextSeq();

// Write one {"type":"boot",...} record. Pass empty string for nowIsoOrEmpty
// when wall-clock time is not yet available.
bool appendBoot(uint8_t resetReason,
                const char* resetName,
                uint32_t freeHeapKb,
                uint32_t pendingUploads,
                const char* sessionId,
                const char* nowIsoOrEmpty,
                bool usbSerialAttached);

// Write one {"type":"reset_crash",...} record when the previous reset looks
// crash-like and USB serial was not attached at boot. This is a cold boot-time
// safety net for battery/field runs that crash outside a named checkpoint.
// Returns true iff a record was written this call.
bool appendSeriallessResetCrashIfNeeded(uint8_t resetReason,
                                        const char* resetName,
                                        const char* sessionId,
                                        const char* nowIsoOrEmpty,
                                        const char* powerSource,
                                        uint32_t freeHeapKb,
                                        uint32_t pendingUploads,
                                        bool usbSerialAttached);

// Inspect the in-RAM crash breadcrumb ring (must be loaded before this call;
// crashLogPrint() in setup() does that) and write at most one {"type":"crash"}
// record describing the newest unresolved breadcrumb. Idempotent across re-runs
// of the same boot — and across reboots within the same crash sequence — via
// an NVS-stored "last vaulted breadcrumb seq" watermark.
//
// Returns true iff a record was written this call.
bool vaultUnresolvedCrashIfNew(uint8_t resetReason,
                               const char* resetName,
                               const char* sessionId,
                               const char* nowIsoOrEmpty,
                               bool usbSerialAttached);

// ── FsAudit hooks ──────────────────────────────────────────────────────────
//
// Records emitted by the maintenance-owner filesystem audit (FsAudit). These
// describe inventory and validation results — they are not crash/boot
// breadcrumbs and may legitimately be informational.

// One record per unknown path encountered during a sweep. Caller is
// responsible for rate-limiting (per-pass cap lives in FsAudit::FsAuditLimits).
bool appendFsAuditUnknown(const char* path, uint32_t sizeBytes);

// One record per known-but-invalid file (header check failed) or
// tmp/orphan file. `reason` is a short stable token (e.g. "header_invalid",
// "tmp_orphan").
bool appendFsAuditInvalid(const char* path,
                          uint32_t sizeBytes,
                          const char* reason);

// One record per recovery action (delete/quarantine/rebuild request) taken
// or attempted by FsAudit. `action` is the verb (e.g. "delete_tmp",
// "quarantine", "delete_failed"); `detail` is action-specific context (the
// new path for a quarantine, the failure reason otherwise).
bool appendFsAuditAction(const char* path,
                         const char* action,
                         const char* detail);

// One record per audit pass with the per-class totals. Always emitted, even
// when the pass aborts under the budget/heap guard.
bool appendFsAuditSummary(uint16_t totalFiles,
                          uint16_t knownValid,
                          uint16_t knownInvalid,
                          uint16_t legacy,
                          uint16_t tmpOrphan,
                          uint16_t unknown,
                          uint32_t bytesScanned,
                          uint32_t durationMs,
                          bool     completed);

// ── Drain API (Phase 4: MQTT upload of pending vault records) ──────────────
//
// MQTTManager owns the publish path. FieldVault exposes a byte-offset cursor
// stored in NVS so reboots resume without re-publishing. Failed publishes
// must NOT call markUploadedThrough(); the next call to peekNext() will
// return the same record.
//
// Note on rotation: when the live JSONL exceeds kRotateBytes and gets
// rotated to .1, the cursor is reset to 0. Records still in the .1 backup
// are NOT drained — they're treated as already past, since field records
// are tiny and rotation should be rare. If this becomes a problem in
// practice, add a backup-file drain mode in a later phase.

// Copy the next pending JSONL record (without its trailing newline) into
// outLine[0..size-1]. Sets *outRecordEnd to the byte offset just past the
// record's newline; the caller passes that back to markUploadedThrough()
// after a successful publish.
//
// Returns false when there are no pending records, when the file is missing,
// or when a single record exceeds size bytes (in which case the cursor is
// advanced past the oversize record to avoid getting stuck on it; an audit
// note is logged).
bool peekNext(char* outLine, size_t size, uint32_t* outRecordEnd);

// Persist the upload watermark. Call after a successful publish with the
// outRecordEnd value returned by peekNext(). Idempotent.
bool markUploadedThrough(uint32_t recordEnd);

// Advance the in-RAM upload watermark without touching flash. Use while the
// upload radio is active; call flushUploadCursor() after the radio lease is
// released to persist the final cursor.
bool markUploadedThroughVolatile(uint32_t recordEnd);
bool flushUploadCursor();

// True if there are pending bytes past the current upload watermark.
bool hasPending();

// Print retained FieldVault JSONL records to USB serial. Does not advance the
// upload cursor or mutate the vault.
void dumpToSerial();

// Current upload watermark (last persisted byte offset). 0 = nothing
// uploaded yet.
uint32_t uploadedThrough();

// Truncate the live JSONL file and reset the upload cursor to 0. Caller's
// contract: only call this after peekNext() has returned false (i.e. the
// live file is fully drained and every pending record has been
// successfully published). Records still in the .1 backup are left alone.
// Idempotent and safe to call when the file does not exist.
bool clearLive();

// Clear every retained FieldVault file after a successful serial dump. This is
// intentionally broader than clearLive(): it removes both live and .1 backup
// files and resets the upload cursor.
bool clearRetained();

}  // namespace FieldVault

