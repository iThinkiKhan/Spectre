
#include "FieldVault.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <esp_system.h>
#include <stdio.h>
#include <string.h>

#include "Schema.h"
#include "../core/CrashBreadcrumb.h"
#include "../core/DebugLog.h"

namespace FieldVault {

namespace {

constexpr size_t   kLineMax      = 256;   // max per-record JSONL line
constexpr size_t   kRotateBytes  = 16384; // rotate when file exceeds this size

// NVS storage for the "last vaulted crash breadcrumb seq" watermark. Used to
// suppress duplicate crash records across reboots that all observe the same
// unresolved breadcrumb in the RTC ring.
constexpr const char* kPrefsNamespace = "spectre_fv";
constexpr const char* kPrefsKeyLvcs   = "lvcs";  // last vaulted crash bcrumb_seq
constexpr const char* kPrefsKeyLvup   = "lvup";  // last vaulted upload byte offset

bool     _ready                 = false;
uint32_t _nextSeq               = 0;
uint32_t _lastVaultedCrashSeq   = 0;
bool     _haveLvcs              = false;  // true once loaded from NVS
uint32_t _uploadCursor          = 0;
bool     _haveLvup              = false;
bool     _uploadCursorDirty     = false;

// Best-effort scan of the existing JSONL to recover the highest seq written
// previously. Done once at begin(). Tolerates truncated trailing lines
// because we read line-by-line and just skip lines without a parsable seq.
uint32_t _scanLastSeq(const char* path) {
    File f = LittleFS.open(path, "r");
    if (!f) return 0;

    uint32_t maxSeq = 0;
    char buf[kLineMax];
    while (f.available()) {
        size_t n = f.readBytesUntil('\n', buf, sizeof(buf) - 1);
        if (n == 0) break;
        buf[n] = '\0';

        const char* p = strstr(buf, "\"seq\":");
        if (!p) continue;
        p += 6;
        // Skip optional whitespace.
        while (*p == ' ') p++;
        char* end = nullptr;
        unsigned long s = strtoul(p, &end, 10);
        if (end != p && s > maxSeq) {
            maxSeq = static_cast<uint32_t>(s);
        }
    }
    f.close();
    return maxSeq;
}

void _ensureDir(const char* path) {
    if (!LittleFS.exists(path)) {
        LittleFS.mkdir(path);
    }
}

void _loadLvcs() {
    Preferences prefs;
    if (!prefs.begin(kPrefsNamespace, true)) {
        _lastVaultedCrashSeq = 0;
        _haveLvcs = false;
        return;
    }
    _haveLvcs = prefs.isKey(kPrefsKeyLvcs);
    _lastVaultedCrashSeq = prefs.getULong(kPrefsKeyLvcs, 0);
    prefs.end();
}

bool _persistLvcs(uint32_t seq) {
    Preferences prefs;
    if (!prefs.begin(kPrefsNamespace, false)) {
        return false;
    }
    const size_t wrote = prefs.putULong(kPrefsKeyLvcs, seq);
    prefs.end();
    return wrote > 0;
}

void _loadLvup() {
    Preferences prefs;
    if (!prefs.begin(kPrefsNamespace, true)) {
        _uploadCursor = 0;
        _haveLvup = true;
        return;
    }
    _uploadCursor = prefs.getULong(kPrefsKeyLvup, 0);
    prefs.end();
    _haveLvup = true;
}

bool _persistLvup(uint32_t cursor) {
    Preferences prefs;
    if (!prefs.begin(kPrefsNamespace, false)) {
        return false;
    }
    const size_t wrote = prefs.putULong(kPrefsKeyLvup, cursor);
    prefs.end();
    return wrote > 0;
}

size_t _liveFileSize() {
    File f = LittleFS.open(PATH_FIELDVAULT_LOG, "r");
    if (!f) return 0;
    const size_t sz = f.size();
    f.close();
    return sz;
}

// Short, conservative reason string built from reset reason + crash phase.
// Returns a pointer to a static buffer, valid until the next call. Safe
// because crash vaulting runs once at boot from a single task.
const char* _inferCrashReason(uint8_t resetReason, CrashPhase phase) {
    static char buf[80];
    const char* phName = crashPhaseName(phase);

    const char* rrAdj = nullptr;
    switch (static_cast<esp_reset_reason_t>(resetReason)) {
        case ESP_RST_TASK_WDT:  rrAdj = "task watchdog";       break;
        case ESP_RST_INT_WDT:   rrAdj = "interrupt watchdog";  break;
        case ESP_RST_WDT:       rrAdj = "watchdog";            break;
        case ESP_RST_PANIC:     rrAdj = "panic";               break;
        case ESP_RST_BROWNOUT:  rrAdj = "brownout";            break;
        case ESP_RST_DEEPSLEEP: rrAdj = "deep-sleep wake";     break;
        case ESP_RST_POWERON:   rrAdj = "power-on";            break;
        case ESP_RST_EXT:       rrAdj = "external reset";      break;
        case ESP_RST_SW:        rrAdj = "software reset";      break;
        case ESP_RST_SDIO:      rrAdj = "sdio reset";          break;
        default:                rrAdj = "reset";               break;
    }
    snprintf(buf, sizeof(buf), "%s during %s", rrAdj, phName);
    return buf;
}

// Walk the ring and return the index of the newest unresolved valid entry,
// or -1 if there are none.
int _findNewestUnresolvedIdx() {
    if (!_logReady()) return -1;

    int newestIdx = -1;
    uint32_t newestSeq = 0;
    bool any = false;
    for (uint8_t i = 0; i < CRASH_LOG_DEPTH; i++) {
        const CrashLogEntry& e = g_crashLog.entries[i];
        if (!_entryValid(e)) continue;
        if (e.resolved) continue;
        if (!any || e.seqNum > newestSeq) {
            newestSeq = e.seqNum;
            newestIdx = i;
            any = true;
        }
    }
    return newestIdx;
}

// Forward declaration so _rotateIfNeeded can reset the upload cursor.
bool _persistLvup(uint32_t cursor);

void _rotateIfNeeded() {
    File f = LittleFS.open(PATH_FIELDVAULT_LOG, "r");
    if (!f) return;
    const size_t sz = f.size();
    f.close();
    if (sz < kRotateBytes) return;

    // Records between the upload cursor and end-of-file get demoted to the
    // .bak rolling backup and will not be retried by the upload pipeline.
    // Surface the loss explicitly so it shows up in the log when uploads
    // are falling behind, rather than silently swallowing the tail.
    const uint32_t unsentBytes = (_uploadCursor < sz)
        ? static_cast<uint32_t>(sz - _uploadCursor)
        : 0U;
    if (unsentBytes > 0) {
        DLOG_WARN("FIELDVAULT",
                  "rotation demoting unsent tail bytes=%lu cursor=%lu size=%lu",
                  static_cast<unsigned long>(unsentBytes),
                  static_cast<unsigned long>(_uploadCursor),
                  static_cast<unsigned long>(sz));
    }

    if (LittleFS.exists(PATH_FIELDVAULT_BAK)) {
        LittleFS.remove(PATH_FIELDVAULT_BAK);
    }
    LittleFS.rename(PATH_FIELDVAULT_LOG, PATH_FIELDVAULT_BAK);

    // The new live file starts empty, so the upload cursor must reset to 0
    // or it would point past the new file's size and hasPending() would
    // never trip until the file grew past the stale offset.
    _uploadCursor = 0;
    _haveLvup = true;
    (void)_persistLvup(0);
}

bool _appendLine(const char* line) {
    _rotateIfNeeded();
    File f = LittleFS.open(PATH_FIELDVAULT_LOG, FILE_APPEND);
    if (!f) return false;
    size_t wrote = f.print(line);
    if (wrote == 0) {
        f.close();
        return false;
    }
    if (line[wrote - 1] != '\n') {
        f.print('\n');
    }
    f.close();
    return true;
}

bool _resetReasonLooksCrashLike(uint8_t resetReason) {
    switch (static_cast<esp_reset_reason_t>(resetReason)) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
            return true;
        default:
            return false;
    }
}

}  // namespace

bool begin() {
    if (_ready) return true;

    // /config and /config/vault are provisioned by StorageManager::begin().
    // We add the field/ subdirectory here so this module owns its own path.
    _ensureDir(PATH_FIELDVAULT_DIR);

    _nextSeq = _scanLastSeq(PATH_FIELDVAULT_LOG) + 1;
    _loadLvcs();
    _loadLvup();

    // Defensive: if the persisted upload cursor sits past the current file
    // size (e.g. NVS survived but the file was wiped), clamp to file size so
    // hasPending() reports correctly.
    const size_t liveSize = _liveFileSize();
    if (_uploadCursor > liveSize) {
        _uploadCursor = static_cast<uint32_t>(liveSize);
        (void)_persistLvup(_uploadCursor);
    }

    _ready = true;
    return true;
}

bool isReady() { return _ready; }
uint32_t nextSeq() { return _nextSeq; }

bool appendBoot(uint8_t resetReason,
                const char* resetName,
                uint32_t freeHeapKb,
                uint32_t pendingUploads,
                const char* sessionId,
                const char* nowIsoOrEmpty,
                bool usbSerialAttached) {
    if (!_ready) return false;

    char line[kLineMax];
    const uint32_t seq    = _nextSeq;
    const uint32_t tsMs   = millis();
    const char* iso       = (nowIsoOrEmpty && nowIsoOrEmpty[0]) ? nowIsoOrEmpty : "";
    const char* sid       = (sessionId && sessionId[0]) ? sessionId : "";
    const char* rname     = (resetName && resetName[0]) ? resetName : "unknown";

    int n = snprintf(line, sizeof(line),
        "{\"type\":\"boot\",\"seq\":%lu,\"ts_ms\":%lu,\"ts_iso\":\"%s\","
        "\"reset\":\"%s\",\"reset_code\":%u,\"session\":\"%s\","
        "\"heap_kb\":%lu,\"pending\":%lu,\"serial\":%u}\n",
        static_cast<unsigned long>(seq),
        static_cast<unsigned long>(tsMs),
        iso,
        rname,
        static_cast<unsigned>(resetReason),
        sid,
        static_cast<unsigned long>(freeHeapKb),
        static_cast<unsigned long>(pendingUploads),
        usbSerialAttached ? 1u : 0u);

    if (n <= 0) return false;
    // snprintf truncated — line[kLineMax-1] is '\0' but the trailing newline
    // got dropped. Force terminator to keep the JSONL invariant.
    if (static_cast<size_t>(n) >= sizeof(line)) {
        line[sizeof(line) - 2] = '\n';
        line[sizeof(line) - 1] = '\0';
    }

    if (!_appendLine(line)) {
        DLOG_WARN("STOR", "FieldVault append failed (boot seq=%lu)",
                  static_cast<unsigned long>(seq));
        return false;
    }

    _nextSeq++;
    return true;
}

bool appendSeriallessResetCrashIfNeeded(uint8_t resetReason,
                                        const char* resetName,
                                        const char* sessionId,
                                        const char* nowIsoOrEmpty,
                                        const char* powerSource,
                                        uint32_t freeHeapKb,
                                        uint32_t pendingUploads,
                                        bool usbSerialAttached) {
    if (!_ready) return false;
    if (usbSerialAttached || !_resetReasonLooksCrashLike(resetReason)) {
        return false;
    }

    char line[kLineMax];
    const uint32_t seq  = _nextSeq;
    const uint32_t tsMs = millis();
    const char* iso     = (nowIsoOrEmpty && nowIsoOrEmpty[0]) ? nowIsoOrEmpty : "";
    const char* sid     = (sessionId && sessionId[0]) ? sessionId : "";
    const char* rname   = (resetName && resetName[0]) ? resetName : "unknown";
    const char* psrc    = (powerSource && powerSource[0]) ? powerSource : "unknown";

    int n = snprintf(line, sizeof(line),
        "{\"type\":\"reset_crash\",\"seq\":%lu,\"ts_ms\":%lu,\"ts_iso\":\"%s\","
        "\"session\":\"%s\",\"reset\":\"%s\",\"reset_code\":%u,"
        "\"power\":\"%s\",\"heap_kb\":%lu,\"pending\":%lu,\"serial\":0}\n",
        static_cast<unsigned long>(seq),
        static_cast<unsigned long>(tsMs),
        iso,
        sid,
        rname,
        static_cast<unsigned>(resetReason),
        psrc,
        static_cast<unsigned long>(freeHeapKb),
        static_cast<unsigned long>(pendingUploads));

    if (n <= 0) return false;
    if (static_cast<size_t>(n) >= sizeof(line)) {
        line[sizeof(line) - 2] = '\n';
        line[sizeof(line) - 1] = '\0';
    }

    if (!_appendLine(line)) {
        DLOG_WARN("STOR", "FieldVault append failed (reset_crash seq=%lu)",
                  static_cast<unsigned long>(seq));
        return false;
    }

    _nextSeq++;
    return true;
}

bool vaultUnresolvedCrashIfNew(uint8_t resetReason,
                               const char* resetName,
                               const char* sessionId,
                               const char* nowIsoOrEmpty,
                               bool usbSerialAttached) {
    if (!_ready) return false;

    const int idx = _findNewestUnresolvedIdx();
    if (idx < 0) {
        return false;  // ring empty, all clean, or not loaded
    }

    const CrashLogEntry& e = g_crashLog.entries[idx];
    const uint32_t bcrumbSeq = e.seqNum;

    // Dedup against prior boots that already vaulted this same breadcrumb.
    // _haveLvcs distinguishes "no persisted watermark yet" from a real
    // persisted watermark of 0, so the first crash entry is not re-vaulted
    // forever if its breadcrumb seqNum is 0.
    if (_haveLvcs && bcrumbSeq <= _lastVaultedCrashSeq) {
        return false;
    }

    char line[kLineMax];
    const uint32_t seq    = _nextSeq;
    const uint32_t tsMs   = millis();
    const char* iso       = (nowIsoOrEmpty && nowIsoOrEmpty[0]) ? nowIsoOrEmpty : "";
    const char* sid       = (sessionId && sessionId[0]) ? sessionId : "";
    const char* rname     = (resetName && resetName[0]) ? resetName : "unknown";
    const CrashPhase ph   = static_cast<CrashPhase>(e.phase);
    const char* phName    = crashPhaseName(ph);
    const char* reason    = _inferCrashReason(resetReason, ph);

    int n = snprintf(line, sizeof(line),
        "{\"type\":\"crash\",\"seq\":%lu,\"bcrumb_seq\":%lu,\"ts_ms\":%lu,"
        "\"ts_iso\":\"%s\",\"session\":\"%s\",\"phase\":\"%s\",\"reason\":\"%s\","
        "\"owner\":%u,\"pending\":%lu,\"heap_kb\":%lu,\"uptime_s\":%lu,"
        "\"reset\":\"%s\",\"reset_code\":%u,\"serial\":%u}\n",
        static_cast<unsigned long>(seq),
        static_cast<unsigned long>(bcrumbSeq),
        static_cast<unsigned long>(tsMs),
        iso,
        sid,
        phName,
        reason,
        static_cast<unsigned>(e.owner),
        static_cast<unsigned long>(e.pending),
        static_cast<unsigned long>(e.heapMinFree / 1024),
        static_cast<unsigned long>(e.uptimeMs / 1000),
        rname,
        static_cast<unsigned>(resetReason),
        usbSerialAttached ? 1u : 0u);

    if (n <= 0) return false;
    if (static_cast<size_t>(n) >= sizeof(line)) {
        line[sizeof(line) - 2] = '\n';
        line[sizeof(line) - 1] = '\0';
    }

    if (!_appendLine(line)) {
        DLOG_WARN("STOR", "FieldVault append failed (crash bcrumb_seq=%lu)",
                  static_cast<unsigned long>(bcrumbSeq));
        return false;
    }

    _nextSeq++;
    _lastVaultedCrashSeq = bcrumbSeq;
    _haveLvcs = true;
    if (!_persistLvcs(bcrumbSeq)) {
        // Persist failure leaves lvcs in RAM only; next boot may re-vault. We
        // accept that over swallowing a real crash record for the user.
        DLOG_WARN("STOR", "FieldVault lvcs persist failed (seq=%lu)",
                  static_cast<unsigned long>(bcrumbSeq));
    }
    return true;
}

namespace {

// Sanitize an arbitrary path for embedding in a JSON string field. Replaces
// characters JSON can't carry inline (control bytes, backslash, quote) with
// '?' so we never produce invalid JSONL.
void _sanitizeForJson(const char* in, char* out, size_t outSize) {
    if (outSize == 0) return;
    size_t o = 0;
    if (in) {
        for (size_t i = 0; in[i] && o + 1 < outSize; ++i) {
            const unsigned char c = static_cast<unsigned char>(in[i]);
            if (c < 0x20 || c == '"' || c == '\\' || c == 0x7F) {
                out[o++] = '?';
            } else {
                out[o++] = static_cast<char>(c);
            }
        }
    }
    out[o] = '\0';
}

}  // namespace

bool appendFsAuditUnknown(const char* path, uint32_t sizeBytes) {
    if (!_ready) return false;
    char safePath[160];
    _sanitizeForJson(path, safePath, sizeof(safePath));

    char line[kLineMax];
    const uint32_t seq = _nextSeq;
    int n = snprintf(line, sizeof(line),
        "{\"type\":\"fs_audit_unknown\",\"seq\":%lu,\"ts_ms\":%lu,"
        "\"path\":\"%s\",\"size\":%lu}\n",
        static_cast<unsigned long>(seq),
        static_cast<unsigned long>(millis()),
        safePath,
        static_cast<unsigned long>(sizeBytes));
    if (n <= 0) return false;
    if (static_cast<size_t>(n) >= sizeof(line)) {
        line[sizeof(line) - 2] = '\n';
        line[sizeof(line) - 1] = '\0';
    }
    if (!_appendLine(line)) return false;
    _nextSeq++;
    return true;
}

bool appendFsAuditInvalid(const char* path,
                          uint32_t sizeBytes,
                          const char* reason) {
    if (!_ready) return false;
    char safePath[160];
    char safeReason[32];
    _sanitizeForJson(path, safePath, sizeof(safePath));
    _sanitizeForJson(reason && reason[0] ? reason : "unknown",
                     safeReason, sizeof(safeReason));

    char line[kLineMax];
    const uint32_t seq = _nextSeq;
    int n = snprintf(line, sizeof(line),
        "{\"type\":\"fs_audit_invalid\",\"seq\":%lu,\"ts_ms\":%lu,"
        "\"path\":\"%s\",\"size\":%lu,\"reason\":\"%s\"}\n",
        static_cast<unsigned long>(seq),
        static_cast<unsigned long>(millis()),
        safePath,
        static_cast<unsigned long>(sizeBytes),
        safeReason);
    if (n <= 0) return false;
    if (static_cast<size_t>(n) >= sizeof(line)) {
        line[sizeof(line) - 2] = '\n';
        line[sizeof(line) - 1] = '\0';
    }
    if (!_appendLine(line)) return false;
    _nextSeq++;
    return true;
}

bool appendFsAuditAction(const char* path,
                         const char* action,
                         const char* detail) {
    if (!_ready) return false;
    char safePath[160];
    char safeAction[24];
    char safeDetail[64];
    _sanitizeForJson(path, safePath, sizeof(safePath));
    _sanitizeForJson(action && action[0] ? action : "unknown",
                     safeAction, sizeof(safeAction));
    _sanitizeForJson(detail ? detail : "", safeDetail, sizeof(safeDetail));

    char line[kLineMax];
    const uint32_t seq = _nextSeq;
    int n = snprintf(line, sizeof(line),
        "{\"type\":\"fs_audit_action\",\"seq\":%lu,\"ts_ms\":%lu,"
        "\"path\":\"%s\",\"action\":\"%s\",\"detail\":\"%s\"}\n",
        static_cast<unsigned long>(seq),
        static_cast<unsigned long>(millis()),
        safePath,
        safeAction,
        safeDetail);
    if (n <= 0) return false;
    if (static_cast<size_t>(n) >= sizeof(line)) {
        line[sizeof(line) - 2] = '\n';
        line[sizeof(line) - 1] = '\0';
    }
    if (!_appendLine(line)) return false;
    _nextSeq++;
    return true;
}

bool appendFsAuditSummary(uint16_t totalFiles,
                          uint16_t knownValid,
                          uint16_t knownInvalid,
                          uint16_t legacy,
                          uint16_t tmpOrphan,
                          uint16_t unknown,
                          uint32_t bytesScanned,
                          uint32_t durationMs,
                          bool     completed) {
    if (!_ready) return false;
    char line[kLineMax];
    const uint32_t seq = _nextSeq;
    int n = snprintf(line, sizeof(line),
        "{\"type\":\"fs_audit_summary\",\"seq\":%lu,\"ts_ms\":%lu,"
        "\"files\":%u,\"valid\":%u,\"invalid\":%u,\"legacy\":%u,"
        "\"tmp\":%u,\"unknown\":%u,\"bytes\":%lu,\"ms\":%lu,\"done\":%u}\n",
        static_cast<unsigned long>(seq),
        static_cast<unsigned long>(millis()),
        totalFiles, knownValid, knownInvalid, legacy,
        tmpOrphan, unknown,
        static_cast<unsigned long>(bytesScanned),
        static_cast<unsigned long>(durationMs),
        completed ? 1u : 0u);
    if (n <= 0) return false;
    if (static_cast<size_t>(n) >= sizeof(line)) {
        line[sizeof(line) - 2] = '\n';
        line[sizeof(line) - 1] = '\0';
    }
    if (!_appendLine(line)) return false;
    _nextSeq++;
    return true;
}

uint32_t uploadedThrough() {
    return _uploadCursor;
}

bool hasPending() {
    if (!_ready) return false;
    return _liveFileSize() > _uploadCursor;
}

void dumpToSerial() {
    if (!_ready) {
        Serial.println("[FIELD] not ready");
        return;
    }

    auto dumpFile = [](const char* label, const char* path) -> uint32_t {
        File f = LittleFS.open(path, "r");
        if (!f) {
            Serial.printf("[FIELD] %s missing path=%s\r\n", label, path);
            return 0;
        }

        const size_t sz = f.size();
        Serial.printf("[FIELD] --- %s path=%s bytes=%u ---\r\n",
                      label, path, static_cast<unsigned>(sz));

        uint32_t lines = 0;
        char buf[kLineMax];
        while (f.available()) {
            const size_t n = f.readBytesUntil('\n', buf, sizeof(buf) - 1);
            buf[n] = '\0';
            if (n > 0) {
                Serial.println(buf);
                lines++;
            }
            if ((lines & 0x0FU) == 0U) {
                Serial.flush();
                delay(1);
            }
        }
        f.close();

        Serial.printf("[FIELD] --- end %s lines=%lu ---\r\n",
                      label, static_cast<unsigned long>(lines));
        return lines;
    };

    Serial.printf("[FIELD] cursor=%lu pending=%d nextSeq=%lu\r\n",
                  static_cast<unsigned long>(_uploadCursor),
                  hasPending() ? 1 : 0,
                  static_cast<unsigned long>(_nextSeq));
    dumpFile("backup", PATH_FIELDVAULT_BAK);
    dumpFile("live", PATH_FIELDVAULT_LOG);
    Serial.println("[FIELD] dump complete");
}

bool peekNext(char* outLine, size_t size, uint32_t* outRecordEnd) {
    if (!_ready || outLine == nullptr || size < 2 || outRecordEnd == nullptr) {
        return false;
    }

    File f = LittleFS.open(PATH_FIELDVAULT_LOG, "r");
    if (!f) return false;

    const size_t fileSize = f.size();
    if (_uploadCursor >= fileSize) {
        f.close();
        return false;
    }

    if (!f.seek(_uploadCursor)) {
        f.close();
        return false;
    }

    // Read up to size-1 bytes or until newline. readBytesUntil consumes
    // (but does not store) the delimiter. We separately track how many bytes
    // we advanced so the caller's recordEnd cursor includes the newline.
    const size_t maxRead = size - 1;
    const size_t got = f.readBytesUntil('\n', outLine, maxRead);
    outLine[got] = '\0';

    // Determine the post-record file position. Reading the delimiter advances
    // the file pointer by one extra byte when a newline was found; absent any
    // delimiter (truncated trailing line at EOF) the pointer sits at file end.
    const size_t newPos = static_cast<size_t>(f.position());
    f.close();

    if (got == 0 && newPos <= _uploadCursor) {
        // Nothing read and pointer didn't advance; treat as nothing pending.
        return false;
    }

    // Oversize record: line larger than caller's buffer. Skip past it so we
    // don't get permanently stuck. The caller still won't see this record.
    if (got == maxRead && newPos < fileSize) {
        DLOG_WARN("STOR",
                  "FieldVault peek skipped oversize record at off=%lu (len>=%u)",
                  static_cast<unsigned long>(_uploadCursor),
                  static_cast<unsigned>(maxRead));
        // Walk forward to the next newline.
        File f2 = LittleFS.open(PATH_FIELDVAULT_LOG, "r");
        if (!f2) return false;
        f2.seek(static_cast<uint32_t>(newPos));
        char throwaway[64];
        while (f2.available()) {
            const size_t n = f2.readBytesUntil('\n', throwaway, sizeof(throwaway));
            if (n < sizeof(throwaway)) break;  // hit newline or EOF
        }
        const size_t skipTo = static_cast<size_t>(f2.position());
        f2.close();
        markUploadedThroughVolatile(static_cast<uint32_t>(skipTo));
        return false;
    }

    *outRecordEnd = static_cast<uint32_t>(newPos);
    return true;
}

bool markUploadedThrough(uint32_t recordEnd) {
    if (!_ready) return false;
    if (recordEnd <= _uploadCursor) return true;  // idempotent / stale
    _uploadCursor = recordEnd;
    _haveLvup = true;
    _uploadCursorDirty = false;
    return _persistLvup(recordEnd);
}

bool markUploadedThroughVolatile(uint32_t recordEnd) {
    if (!_ready) return false;
    if (recordEnd <= _uploadCursor) return true;  // idempotent / stale
    _uploadCursor = recordEnd;
    _haveLvup = true;
    _uploadCursorDirty = true;
    return true;
}

bool flushUploadCursor() {
    if (!_ready) return false;
    if (!_uploadCursorDirty) return true;
    if (!_persistLvup(_uploadCursor)) return false;
    _uploadCursorDirty = false;
    return true;
}

bool clearLive() {
    if (!_ready) return false;
    if (LittleFS.exists(PATH_FIELDVAULT_LOG)) {
        if (!LittleFS.remove(PATH_FIELDVAULT_LOG)) {
            DLOG_WARN("STOR", "FieldVault clearLive: remove failed");
            return false;
        }
    }
    _uploadCursor = 0;
    _haveLvup = true;
    _uploadCursorDirty = false;
    return _persistLvup(0);
}

bool clearRetained() {
    if (!_ready) return false;

    bool ok = true;
    if (LittleFS.exists(PATH_FIELDVAULT_LOG) &&
        !LittleFS.remove(PATH_FIELDVAULT_LOG)) {
        DLOG_WARN("STOR", "FieldVault clearRetained: live remove failed");
        ok = false;
    }
    if (LittleFS.exists(PATH_FIELDVAULT_BAK) &&
        !LittleFS.remove(PATH_FIELDVAULT_BAK)) {
        DLOG_WARN("STOR", "FieldVault clearRetained: backup remove failed");
        ok = false;
    }

    if (!ok) return false;
    _uploadCursor = 0;
    _haveLvup = true;
    _uploadCursorDirty = false;
    return _persistLvup(0);
}

}  // namespace FieldVault

