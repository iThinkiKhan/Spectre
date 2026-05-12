


#include "CrashBreadcrumb.h"
#include "DebugLog.h"
#include <Preferences.h>

constexpr const char* CRASH_PREF_NAMESPACE = "spectre_crash";
constexpr const char* CRASH_PREF_KEY = "log";

bool crashBreadcrumbPersist() {
    if (!_logReady()) return false;

    Preferences prefs;
    if (!prefs.begin(CRASH_PREF_NAMESPACE, false)) {
        return false;
    }

    const size_t written = prefs.putBytes(CRASH_PREF_KEY,
                                          &g_crashLog,
                                          sizeof(g_crashLog));
    prefs.end();
    return written == sizeof(g_crashLog);
}

static bool _restoreCrashLog() {
    Preferences prefs;
    if (!prefs.begin(CRASH_PREF_NAMESPACE, true)) {
        return false;
    }

    const size_t available = prefs.getBytesLength(CRASH_PREF_KEY);
    if (available != sizeof(g_crashLog)) {
        prefs.end();
        return false;
    }

    const size_t read = prefs.getBytes(CRASH_PREF_KEY,
                                       &g_crashLog,
                                       sizeof(g_crashLog));
    prefs.end();

    return read == sizeof(g_crashLog) && _logReady();
}

// RTC_NOINIT_ATTR places g_crashLog in RTC slow memory, which the ESP32
// preserves across software resets, panics, and watchdog resets.
// The memory is NOT zero-initialised — _logReady() and _entryValid() check
// magic + CRC before trusting any field.
RTC_NOINIT_ATTR CrashLog g_crashLog;

// ─────────────────────────────────────────────────────────────────────────────
// crashLogPrint
//
// Iterates the ring oldest→newest (starting at head, wrapping once), prints
// every valid entry to both Serial and DLOG.  Resolved entries are tagged
// [ok]; unresolved ones (= device reset while that phase was active) are
// tagged [CRASH?].
//
// Entries are NOT erased: they persist until overwritten by new checkpoints,
// so this can be called several boots after the crash and still show history.
// ─────────────────────────────────────────────────────────────────────────────

void crashLogPrint() {
    bool restoredFromNvs = false;

    if (!_logReady()) {
        restoredFromNvs = _restoreCrashLog();
    }

    if (!_logReady()) {
        Serial.printf("[BOOT] crash log: cold boot / power cycle (no RTC data)\n");
        return;
    }

    // Count valid entries for the header line.
    uint8_t validCount = 0;
    for (uint8_t i = 0; i < CRASH_LOG_DEPTH; i++) {
        if (_entryValid(g_crashLog.entries[i])) validCount++;
    }

    if (validCount == 0) {
        Serial.printf("[BOOT] crash log: ring present but all entries invalid (first run?)\n");
        return;
    }

    Serial.printf("[BOOT] crash log: %u entr%s, seq 0..%lu (oldest first)\n",
                  static_cast<unsigned>(validCount),
                  validCount == 1 ? "y" : "ies",
                  static_cast<unsigned long>(g_crashLog.nextSeq > 0 ? g_crashLog.nextSeq - 1 : 0));

    if (restoredFromNvs) {
        Serial.printf("[BOOT] crash log restored from persistent snapshot\n");
    }

    // Walk ring oldest→newest.  head points to the next write slot, so
    // iterating from head gives the oldest entry first.
    uint8_t printed = 0;
    for (uint8_t i = 0; i < CRASH_LOG_DEPTH; i++) {
        const uint8_t idx = (g_crashLog.head + i) % CRASH_LOG_DEPTH;
        const CrashLogEntry& e = g_crashLog.entries[idx];
        if (!_entryValid(e)) continue;

        printed++;
        const CrashPhase phase = static_cast<CrashPhase>(e.phase);
        const char* tag = e.resolved ? "[ok]    " : "[CRASH?]";

        Serial.printf("[BOOT]   #%u seq=%-4lu %s phase=%-16s owner=%u pendingUpload=%-5lu heapFree=%luK largest=%luK min=%luK uptime=%lus\n",
                      static_cast<unsigned>(printed),
                      static_cast<unsigned long>(e.seqNum),
                      tag,
                      crashPhaseName(phase),
                      static_cast<unsigned>(e.owner),
                      static_cast<unsigned long>(e.pending),
                      static_cast<unsigned long>(e.heapFree / 1024),
                      static_cast<unsigned long>(e.heapLargest / 1024),
                      static_cast<unsigned long>(e.heapMinFree / 1024),
                      static_cast<unsigned long>(e.uptimeMs / 1000));

        // Mirror crashes (but not routine [ok] entries) into DLOG so they
        // appear in the in-memory log ring that the companion can pull.
        if (!e.resolved) {
            DLOG_WARN("CORE",
                      "crash? seq=%lu phase=%s owner=%u pendingUpload=%lu heapFree=%luK largest=%luK min=%luK uptime=%lus",
                      static_cast<unsigned long>(e.seqNum),
                      crashPhaseName(phase),
                      static_cast<unsigned>(e.owner),
                      static_cast<unsigned long>(e.pending),
                      static_cast<unsigned long>(e.heapFree / 1024),
                      static_cast<unsigned long>(e.heapLargest / 1024),
                      static_cast<unsigned long>(e.heapMinFree / 1024),
                      static_cast<unsigned long>(e.uptimeMs / 1000));
        }
    }
}


