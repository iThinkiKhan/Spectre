


#pragma once

#include <Arduino.h>
#include <esp_heap_caps.h>

// True usable DRAM.
//
// MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT also counts the ~7.7 KB RTC slow-memory
// heap at 0x600FE000 (verified with `heap map`: internal free minus DMA free is
// a constant 7,664 B). That region is slow, is where these breadcrumbs live, and
// only ever receives last-resort spill allocations. Counting it made every
// "internal free" reading ~7.7 KB optimistic and — worse — held the capture heap
// guard above its own 12 KB threshold even when DRAM was genuinely exhausted:
// the 2026-08-18 overnight run reported min=6K while RTC alone had 7,668 B free,
// so real DRAM had hit zero and was spilling. Measure DRAM, not DRAM+RTC.
static constexpr uint32_t SPECTRE_CAP_DRAM = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;

// RTC slow-memory crash breadcrumbs survive software resets, panic resets,
// and task-watchdog resets. They do not survive full power loss.

static constexpr uint8_t  CRASH_LOG_DEPTH   = 10;
static constexpr uint32_t CRASH_LOG_MAGIC   = 0xC0DE0003UL;  // ring metadata sentinel
static constexpr uint32_t CRASH_ENTRY_MAGIC = 0xDEADB00BUL;  // per-entry sentinel

enum class CrashPhase : uint8_t {
    NONE = 0,
    WIFI_CAPTURE,    // radio arbiter granted RADIO_WIFI_CAPTURE
    MQTT_DUMPING,    // MQTT state machine entered MQTT_DUMPING
    UPLOAD_FLUSH,    // STORAGE.endUploadBatch() about to run
    BACKLOG_PROBE,   // BLE phone probe lease granted (no phone yet)
    BACKLOG_ENRICH,  // BLE enrichment exchange in progress
    BLE_AUTH,        // P-256 + AES-GCM handshake with phone underway
    STORAGE_APPEND,  // storage append / enrichment delta batch write
    RUNTIME_HEALTH,  // periodic health snapshot / serial diagnostics
    HEAP_INTEGRITY,  // heap_caps_check_integrity_all diagnostic walk
    RADIO_RESUME,    // idle fallback/radio owner resume after release
    STORAGE_BOOT,    // storage mount / fast boot reconcile / boot maintenance
    DISPLAY_WAIT,    // hardware task waiting for display layer readiness
};

struct CrashLogEntry {
    uint32_t magic;
    uint8_t  phase;      // CrashPhase cast to uint8_t
    uint8_t  owner;      // RadioOwner cast to uint8_t
    uint8_t  resolved;   // 1 = completed cleanly, 0 = was active at reset
    uint8_t  vaulted;    // 1 = already written to FieldVault by some boot
    uint32_t bootGen;    // boot generation that wrote the entry
    uint32_t pending;    // upload queue depth at checkpoint
    uint32_t heapMinFree;// internal/8-bit heap low-water mark
    uint32_t heapFree;   // current internal/8-bit free heap
    uint32_t heapLargest;// current largest internal/8-bit free block
    uint32_t uptimeMs;   // millis() at checkpoint
    uint32_t seqNum;     // monotonic across resets (for ordering)
    uint32_t crc;        // XOR of all other fields
};

// Ring sits entirely in RTC slow memory (~416 bytes of the 8 KB budget).
struct CrashLog {
    uint32_t      logMagic;           // CRASH_LOG_MAGIC when ring is initialised
    uint32_t      bootGen;            // increments once per boot
    uint32_t      nextSeq;            // next sequence number to assign
    uint8_t       head;               // index of next slot to write (oldest if full)
    uint8_t       _pad[3];
    CrashLogEntry entries[CRASH_LOG_DEPTH];
};

// Defined in CrashBreadcrumb.cpp with RTC_NOINIT_ATTR.
extern CrashLog g_crashLog;
bool crashBreadcrumbPersist();

// ── Internal helpers ──────────────────────────────────────────────────────────

inline uint32_t _entryCrc(const CrashLogEntry& e) {
    return e.magic
         ^ static_cast<uint32_t>(e.phase)
         ^ static_cast<uint32_t>(e.owner)
         ^ static_cast<uint32_t>(e.resolved)
         ^ (static_cast<uint32_t>(e.vaulted) << 8)
         ^ e.pending
         ^ e.heapMinFree
         ^ e.heapFree
         ^ e.heapLargest
         ^ e.uptimeMs
         ^ e.seqNum
         ^ e.bootGen;
}

inline bool _entryValid(const CrashLogEntry& e) {
    return e.magic == CRASH_ENTRY_MAGIC && e.crc == _entryCrc(e);
}

inline bool _logReady() {
    return g_crashLog.logMagic == CRASH_LOG_MAGIC &&
           g_crashLog.head     <  CRASH_LOG_DEPTH;
}

inline void _initLog() {
    g_crashLog.logMagic = CRASH_LOG_MAGIC;
    g_crashLog.bootGen  = 0;
    g_crashLog.nextSeq  = 0;
    g_crashLog.head     = 0;
    for (uint8_t i = 0; i < CRASH_LOG_DEPTH; i++) {
        g_crashLog.entries[i].magic = 0;
        g_crashLog.entries[i].crc   = 0;
    }
}

// Choose the slot a new checkpoint should occupy.
//
// A plain ring lets high-frequency resolved checkpoints (RUNTIME_HEALTH fires
// every 30 s) evict the unresolved entry that is the actual crash evidence —
// the ring self-wiped within ~2.5 min, which is why the 2026-08-18 field
// panics recorded nothing but "[ok] runtime_health". Victim order is therefore
// invalid slot -> oldest resolved slot -> oldest unresolved slot, so an
// in-flight phase is only overwritten when every slot is in flight.
inline uint8_t _claimSlot() {
    uint8_t invalid  = CRASH_LOG_DEPTH;
    uint8_t resolved = CRASH_LOG_DEPTH;
    uint8_t stale    = CRASH_LOG_DEPTH;  // unresolved, but from an earlier boot
    uint8_t active   = CRASH_LOG_DEPTH;  // unresolved, this boot

    for (uint8_t i = 0; i < CRASH_LOG_DEPTH; i++) {
        const CrashLogEntry& e = g_crashLog.entries[i];
        if (!_entryValid(e)) {
            if (invalid == CRASH_LOG_DEPTH) invalid = i;
            continue;
        }

        uint8_t* slot = nullptr;
        if (e.resolved) {
            slot = &resolved;
        } else if (e.bootGen != g_crashLog.bootGen) {
            slot = &stale;
        } else {
            slot = &active;
        }

        if (*slot == CRASH_LOG_DEPTH ||
            e.seqNum < g_crashLog.entries[*slot].seqNum) {
            *slot = i;
        }
    }

    if (invalid  < CRASH_LOG_DEPTH) return invalid;
    if (resolved < CRASH_LOG_DEPTH) return resolved;
    // A phase left unresolved by an earlier boot has already been reported at
    // that boot's crash print, so it must not permanently occupy a slot and
    // starve the current boot of anywhere to record.
    if (stale    < CRASH_LOG_DEPTH) return stale;
    return active;
}

// Index of the highest-sequence valid entry, or CRASH_LOG_DEPTH if none.
inline uint8_t _newestSlot() {
    uint8_t newest = CRASH_LOG_DEPTH;
    for (uint8_t i = 0; i < CRASH_LOG_DEPTH; i++) {
        const CrashLogEntry& e = g_crashLog.entries[i];
        if (!_entryValid(e)) continue;
        if (newest == CRASH_LOG_DEPTH ||
            e.seqNum > g_crashLog.entries[newest].seqNum) {
            newest = i;
        }
    }
    return newest;
}

// ── Public API ────────────────────────────────────────────────────────────────

inline const char* crashPhaseName(CrashPhase p) {
    switch (p) {
        case CrashPhase::NONE:           return "none";
        case CrashPhase::WIFI_CAPTURE:   return "wifi_capture";
        case CrashPhase::MQTT_DUMPING:   return "mqtt_dumping";
        case CrashPhase::UPLOAD_FLUSH:   return "upload_flush";
        case CrashPhase::BACKLOG_PROBE:  return "backlog_probe";
        case CrashPhase::BACKLOG_ENRICH: return "backlog_enrich";
        case CrashPhase::BLE_AUTH:       return "ble_auth";
        case CrashPhase::STORAGE_APPEND: return "storage_append";
        case CrashPhase::RUNTIME_HEALTH: return "runtime_health";
        case CrashPhase::HEAP_INTEGRITY: return "heap_integrity";
        case CrashPhase::RADIO_RESUME:   return "radio_resume";
        case CrashPhase::STORAGE_BOOT:   return "storage_boot";
        case CrashPhase::DISPLAY_WAIT:   return "display_wait";
        default:                          return "?";
    }
}

// Write before any risky radio or storage transition.
// Entries are written in a ring; older entries are overwritten after DEPTH writes.
inline void crashCheckpoint(CrashPhase phase, uint8_t owner, uint32_t pending) {
    if (!_logReady()) _initLog();

    CrashLogEntry& e = g_crashLog.entries[_claimSlot()];
    e.magic       = CRASH_ENTRY_MAGIC;
    e.phase       = static_cast<uint8_t>(phase);
    e.owner       = owner;
    e.resolved    = 0;
    e.vaulted     = 0;
    e.pending     = pending;
    e.heapMinFree = heap_caps_get_minimum_free_size(SPECTRE_CAP_DRAM);
    e.heapFree    = heap_caps_get_free_size(SPECTRE_CAP_DRAM);
    e.heapLargest = heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM);
    e.uptimeMs    = static_cast<uint32_t>(millis());
    e.seqNum      = g_crashLog.nextSeq++;
    e.bootGen     = g_crashLog.bootGen;
    e.crc         = _entryCrc(e);

    g_crashLog.head = 0;  // ordering now comes from seqNum, not ring position
    (void)crashBreadcrumbPersist();
}

// RTC-only checkpoint for frequent diagnostics. Survives panic/watchdog resets
// without adding flash/NVS writes to hot paths.
inline void crashCheckpointVolatile(CrashPhase phase, uint8_t owner, uint32_t pending) {
    if (!_logReady()) _initLog();

    CrashLogEntry& e = g_crashLog.entries[_claimSlot()];
    e.magic       = CRASH_ENTRY_MAGIC;
    e.phase       = static_cast<uint8_t>(phase);
    e.owner       = owner;
    e.resolved    = 0;
    e.vaulted     = 0;
    e.pending     = pending;
    e.heapMinFree = heap_caps_get_minimum_free_size(SPECTRE_CAP_DRAM);
    e.heapFree    = heap_caps_get_free_size(SPECTRE_CAP_DRAM);
    e.heapLargest = heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM);
    e.uptimeMs    = static_cast<uint32_t>(millis());
    e.seqNum      = g_crashLog.nextSeq++;
    e.bootGen     = g_crashLog.bootGen;
    e.crc         = _entryCrc(e);

    g_crashLog.head = 0;  // ordering now comes from seqNum, not ring position
}

// Progress breadcrumb for a phase that emits many checkpoints in sequence and
// never clears them (storage boot steps encode the step number in `pending`).
// Each call overwrites this boot's existing unresolved entry for the phase, so
// the ring keeps only the furthest step reached rather than one slot per step.
inline void crashCheckpointStep(CrashPhase phase, uint8_t owner,
                                uint32_t detail, bool persist) {
    if (!_logReady()) _initLog();

    const uint8_t phaseValue = static_cast<uint8_t>(phase);
    uint8_t idx = CRASH_LOG_DEPTH;
    for (uint8_t i = 0; i < CRASH_LOG_DEPTH; i++) {
        const CrashLogEntry& e = g_crashLog.entries[i];
        if (_entryValid(e) && e.resolved == 0 &&
            e.phase == phaseValue && e.bootGen == g_crashLog.bootGen) {
            idx = i;
            break;
        }
    }
    if (idx == CRASH_LOG_DEPTH) idx = _claimSlot();

    CrashLogEntry& e = g_crashLog.entries[idx];
    e.magic       = CRASH_ENTRY_MAGIC;
    e.phase       = phaseValue;
    e.owner       = owner;
    e.resolved    = 0;
    e.vaulted     = 0;
    e.pending     = detail;
    e.heapMinFree = heap_caps_get_minimum_free_size(SPECTRE_CAP_DRAM);
    e.heapFree    = heap_caps_get_free_size(SPECTRE_CAP_DRAM);
    e.heapLargest = heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM);
    e.uptimeMs    = static_cast<uint32_t>(millis());
    e.seqNum      = g_crashLog.nextSeq++;
    e.bootGen     = g_crashLog.bootGen;
    e.crc         = _entryCrc(e);

    if (persist) (void)crashBreadcrumbPersist();
}

// Mark the most-recently written checkpoint as cleanly resolved.
// The entry stays in the ring so the boot log shows context around crashes.
inline void crashBreadcrumbClear() {
    if (!_logReady()) return;
    // Slots are no longer filled in ring order, so the most recent write is the
    // highest sequence number rather than the slot behind head.
    const uint8_t last = _newestSlot();
    if (last < CRASH_LOG_DEPTH) {
        CrashLogEntry& e = g_crashLog.entries[last];
        e.resolved = 1;
        e.crc = _entryCrc(e);
        (void)crashBreadcrumbPersist();
    }
}

// Mark the newest unresolved checkpoint for a phase as cleanly completed. This is
// used by nested flows that write a broader checkpoint first, then a narrower
// checkpoint later (for example MQTT_DUMPING followed by UPLOAD_FLUSH).
inline void crashBreadcrumbClear(CrashPhase phase) {
    if (!_logReady()) return;

    const uint8_t phaseValue = static_cast<uint8_t>(phase);
    CrashLogEntry* newest = nullptr;
    for (uint8_t i = 0; i < CRASH_LOG_DEPTH; i++) {
        CrashLogEntry& e = g_crashLog.entries[i];
        if (_entryValid(e) && e.phase == phaseValue && e.resolved == 0) {
            if (!newest || e.seqNum > newest->seqNum) {
                newest = &e;
            }
        }
    }

    if (newest) {
        newest->resolved = 1;
        newest->crc = _entryCrc(*newest);
        (void)crashBreadcrumbPersist();
    }
}

inline void crashBreadcrumbClearVolatile(CrashPhase phase) {
    if (!_logReady()) return;

    const uint8_t phaseValue = static_cast<uint8_t>(phase);
    CrashLogEntry* newest = nullptr;
    for (uint8_t i = 0; i < CRASH_LOG_DEPTH; i++) {
        CrashLogEntry& e = g_crashLog.entries[i];
        if (_entryValid(e) && e.phase == phaseValue && e.resolved == 0) {
            if (!newest || e.seqNum > newest->seqNum) {
                newest = &e;
            }
        }
    }

    if (newest) {
        newest->resolved = 1;
        newest->crc = _entryCrc(*newest);
    }
}

// Mark the entry carrying `seqNum` as already written to the field vault.
//
// The vault dedupes against an NVS watermark, but that write can fail (a full
// or busy NVS partition), and when it did the same breadcrumb was re-vaulted as
// a fresh crash on every subsequent boot — bcrumb_seq 5804 appeared three times
// in the 2026-08-19 run with identical uptime and heap. The consumed flag lives
// on the entry itself, in the same RTC memory that carries the entry across the
// reset, so dedup no longer depends on NVS succeeding. `resolved` is
// deliberately left alone: the ring dump must still show this as [CRASH?].
inline void crashBreadcrumbMarkVaulted(uint32_t seqNum) {
    if (!_logReady()) return;

    for (uint8_t i = 0; i < CRASH_LOG_DEPTH; i++) {
        CrashLogEntry& e = g_crashLog.entries[i];
        if (!_entryValid(e) || e.seqNum != seqNum) continue;
        if (e.vaulted) return;
        e.vaulted = 1;
        e.crc = _entryCrc(e);
        (void)crashBreadcrumbPersist();
        return;
    }
}

// Print all valid ring entries (oldest first) to Serial and DLOG.
// Call once at boot before any manager initialises.
// Defined in CrashBreadcrumb.cpp.
void crashLogPrint();

// Discard every ring entry, the NVS snapshot, and any armed allocation-failure
// record. Operator-invoked (`crash clear`) when starting a fresh test run so
// old breadcrumbs cannot be mistaken for new evidence.
void crashLogClear();

// ── Allocation-failure capture ───────────────────────────────────────────────
//
// The ESP-IDF heap calls a registered hook whenever an allocation cannot be
// satisfied. Without it a failed malloc just returns NULL and the firmware
// panics later on the resulting NULL dereference, which is what made the
// 2026-08-18 field panics unattributable. Recording the request that failed
// turns that into a named cause.

static constexpr uint32_t ALLOC_FAIL_MAGIC = 0xA110FA11UL;

struct AllocFailRecord {
    uint32_t magic;
    uint32_t size;      // bytes the failing caller asked for
    uint32_t caps;      // MALLOC_CAP_* mask requested
    uint32_t uptimeMs;  // millis() at the first failure
    uint32_t count;     // total failures seen since the record was armed
    char     task[16];  // FreeRTOS task name, or "isr"
};

// RTC_NOINIT so it survives the panic reset that the failure causes.
extern AllocFailRecord g_allocFail;

// Restore the ring (RTC, else the NVS snapshot) and open a new boot
// generation. Call once at the very start of setup(), before any checkpoint.
void crashLogBeginBoot();

// Register the heap hook. Call once, early in setup().
void crashAllocFailInstall();

// True when a failure record from this or a prior boot is present.
bool crashAllocFailValid();

// Print any surviving record, then disarm it so it is reported only once.
void crashAllocFailPrint();


