

#pragma once

#include <Arduino.h>

// ─────────────────────────────────────────────────────────────────────────────
// Crash breadcrumb ring — RTC slow memory
//
// Survives software reset, panic, and task-watchdog resets.
// Does NOT survive a full power-off (use NVS for that).
//
// Keeps the last CRASH_LOG_DEPTH checkpoints across consecutive reboots.
// Entries are never erased on boot — only overwritten by new checkpoints —
// so you can plug in a serial monitor several boots after a crash and still
// see what was happening.
//
// Usage:
//   crashCheckpoint(phase, owner, pending)  — call before any risky transition
//   crashBreadcrumbClear()                  — call after a transition completes
//                                             cleanly (marks entry as [ok],
//                                             does not erase it)
//   crashLogPrint()                         — call once at boot (declared below,
//                                             defined in CrashBreadcrumb.cpp)
//
// RadioOwner is stored as uint8_t to avoid a circular include; callers cast.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint8_t  CRASH_LOG_DEPTH   = 5;
static constexpr uint32_t CRASH_LOG_MAGIC   = 0xC0DE0001UL;  // ring metadata sentinel
static constexpr uint32_t CRASH_ENTRY_MAGIC = 0xDEADB00BUL;  // per-entry sentinel

enum class CrashPhase : uint8_t {
    NONE = 0,
    WIFI_CAPTURE,    // radio arbiter granted RADIO_WIFI_CAPTURE
    MQTT_DUMPING,    // MQTT state machine entered MQTT_DUMPING
    UPLOAD_FLUSH,    // STORAGE.endUploadBatch() about to run
    BACKLOG_PROBE,   // BLE phone probe lease granted (no phone yet)
    BACKLOG_ENRICH,  // BLE enrichment exchange in progress
    BLE_AUTH,        // P-256 + AES-GCM handshake with phone underway
};

struct CrashLogEntry {
    uint32_t magic;
    uint8_t  phase;      // CrashPhase cast to uint8_t
    uint8_t  owner;      // RadioOwner cast to uint8_t
    uint8_t  resolved;   // 1 = completed cleanly, 0 = was active at reset
    uint8_t  _pad;
    uint32_t pending;    // upload queue depth at checkpoint
    uint32_t heapMinFree;// ESP.getMinFreeHeap() at checkpoint
    uint32_t uptimeMs;   // millis() at checkpoint
    uint32_t seqNum;     // monotonic across resets (for ordering)
    uint32_t crc;        // XOR of all other fields
};

// Ring sits entirely in RTC slow memory (~152 bytes of the 8 KB budget).
struct CrashLog {
    uint32_t      logMagic;           // CRASH_LOG_MAGIC when ring is initialised
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
         ^ e.pending
         ^ e.heapMinFree
         ^ e.uptimeMs
         ^ e.seqNum;
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
    g_crashLog.nextSeq  = 0;
    g_crashLog.head     = 0;
    for (uint8_t i = 0; i < CRASH_LOG_DEPTH; i++) {
        g_crashLog.entries[i].magic = 0;
        g_crashLog.entries[i].crc   = 0;
    }
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
        default:                          return "?";
    }
}

// Write before any risky radio or storage transition.
// Entries are written in a ring; older entries are overwritten after DEPTH writes.
inline void crashCheckpoint(CrashPhase phase, uint8_t owner, uint32_t pending) {
    if (!_logReady()) _initLog();

    CrashLogEntry& e = g_crashLog.entries[g_crashLog.head];
    e.magic       = CRASH_ENTRY_MAGIC;
    e.phase       = static_cast<uint8_t>(phase);
    e.owner       = owner;
    e.resolved    = 0;
    e._pad        = 0;
    e.pending     = pending;
    e.heapMinFree = static_cast<uint32_t>(ESP.getMinFreeHeap());
    e.uptimeMs    = static_cast<uint32_t>(millis());
    e.seqNum      = g_crashLog.nextSeq++;
    e.crc         = _entryCrc(e);

    g_crashLog.head = (g_crashLog.head + 1) % CRASH_LOG_DEPTH;
    (void)crashBreadcrumbPersist();
}

// Mark the most-recently written checkpoint as cleanly resolved.
// The entry stays in the ring so the boot log shows context around crashes.
inline void crashBreadcrumbClear() {
    if (!_logReady()) return;
    // head points to the next slot to write; the most recent write is one behind it.
    const uint8_t last = (g_crashLog.head + CRASH_LOG_DEPTH - 1) % CRASH_LOG_DEPTH;
    CrashLogEntry& e = g_crashLog.entries[last];
    if (_entryValid(e)) {
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

// Print all valid ring entries (oldest first) to Serial and DLOG.
// Call once at boot before any manager initialises.
// Defined in CrashBreadcrumb.cpp.
void crashLogPrint();

