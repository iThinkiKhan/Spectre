


#include "CrashBreadcrumb.h"
#include "DebugLog.h"
#include "../config.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

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

// RTC slow memory survives software/panic/watchdog resets; validate before use.
RTC_NOINIT_ATTR CrashLog g_crashLog;
RTC_NOINIT_ATTR AllocFailRecord g_allocFail;

// Runs inside the failing allocation's context, which may be an ISR and may
// hold heap locks. It therefore touches only RTC memory: no heap calls, no
// Serial, no flash. Heap sizes at the time of failure are already carried by
// the crash-breadcrumb ring entry for the active phase.
static void _onAllocFailed(size_t size, uint32_t caps, const char* fnName) {
    (void)fnName;

    if (g_allocFail.magic != ALLOC_FAIL_MAGIC) {
        g_allocFail.magic = ALLOC_FAIL_MAGIC;
        g_allocFail.count = 0;
    }
    g_allocFail.count++;

    // Keep the first failure of the boot; it is the one that starts the
    // cascade. Later failures only bump the counter.
    if (g_allocFail.count > 1) return;

    g_allocFail.size     = static_cast<uint32_t>(size);
    g_allocFail.caps     = caps;
    g_allocFail.uptimeMs = static_cast<uint32_t>(millis());

    const char* task = "isr";
    if (!xPortInIsrContext()) {
        const char* name = pcTaskGetName(nullptr);
        if (name) task = name;
    }
    strlcpy(g_allocFail.task, task, sizeof(g_allocFail.task));
}

void crashLogBeginBoot() {
    // RTC slow memory survives resets but not power loss; fall back to the
    // NVS snapshot so a battery-pull still reports the last crash.
    if (!_logReady()) {
        (void)_restoreCrashLog();
    }
    if (!_logReady()) {
        _initLog();
    }
    g_crashLog.bootGen++;
    (void)crashBreadcrumbPersist();
}

void crashAllocFailInstall() {
    // Arm a fresh record only if one is not already waiting to be reported.
    if (g_allocFail.magic != ALLOC_FAIL_MAGIC) {
        memset(&g_allocFail, 0, sizeof(g_allocFail));
    }
    const esp_err_t err = heap_caps_register_failed_alloc_callback(_onAllocFailed);
    if (err != ESP_OK) {
        DLOG_WARN("CORE", "alloc-fail hook not installed (%d)", static_cast<int>(err));
    }
}

bool crashAllocFailValid() {
    return g_allocFail.magic == ALLOC_FAIL_MAGIC && g_allocFail.count > 0;
}

void crashAllocFailPrint() {
    if (!crashAllocFailValid()) return;

    Serial.printf("[BOOT] ALLOC FAIL: %lu bytes caps=0x%lx task=%s uptime=%lus (%lu total)\r\n",
                  static_cast<unsigned long>(g_allocFail.size),
                  static_cast<unsigned long>(g_allocFail.caps),
                  g_allocFail.task,
                  static_cast<unsigned long>(g_allocFail.uptimeMs / 1000),
                  static_cast<unsigned long>(g_allocFail.count));
    DLOG_WARN("CORE",
              "alloc fail %lu bytes caps=0x%lx task=%s uptime=%lus count=%lu",
              static_cast<unsigned long>(g_allocFail.size),
              static_cast<unsigned long>(g_allocFail.caps),
              g_allocFail.task,
              static_cast<unsigned long>(g_allocFail.uptimeMs / 1000),
              static_cast<unsigned long>(g_allocFail.count));

    // Disarm so the next boot does not re-report a stale failure.
    memset(&g_allocFail, 0, sizeof(g_allocFail));
}

void crashLogClear() {
    // Wipe the RTC ring, the NVS snapshot that would otherwise restore it on
    // the next cold boot, and any armed allocation-failure record. Keeps the
    // current boot generation so entries written after this call still sort
    // and age correctly.
    const uint32_t bootGen = _logReady() ? g_crashLog.bootGen : 0;
    _initLog();
    g_crashLog.bootGen = bootGen;
    memset(&g_allocFail, 0, sizeof(g_allocFail));

    Preferences prefs;
    if (prefs.begin(CRASH_PREF_NAMESPACE, false)) {
        prefs.remove(CRASH_PREF_KEY);
        prefs.end();
    }
    // Re-seed the snapshot so a restore finds an empty-but-valid ring rather
    // than falling back to whatever a stale key held.
    (void)crashBreadcrumbPersist();

    Serial.println("[BOOT] crash ring cleared (RTC + NVS snapshot + alloc-fail)");
    DLOG_WARN("CORE", "crash ring cleared by operator");
}

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

    // Slots are claimed by age/resolved-state rather than in ring order, so
    // walk them in ascending seqNum to print oldest→newest.
    uint8_t printed = 0;
    bool    emitted[CRASH_LOG_DEPTH] = {false};
    for (uint8_t n = 0; n < CRASH_LOG_DEPTH; n++) {
        uint8_t idx = CRASH_LOG_DEPTH;
        for (uint8_t i = 0; i < CRASH_LOG_DEPTH; i++) {
            if (emitted[i] || !_entryValid(g_crashLog.entries[i])) continue;
            if (idx == CRASH_LOG_DEPTH ||
                g_crashLog.entries[i].seqNum < g_crashLog.entries[idx].seqNum) {
                idx = i;
            }
        }
        if (idx == CRASH_LOG_DEPTH) break;
        emitted[idx] = true;
        const CrashLogEntry& e = g_crashLog.entries[idx];

        printed++;
        const CrashPhase phase = static_cast<CrashPhase>(e.phase);
        const char* tag = e.resolved ? "[ok]" : "[CRASH?]";

        Serial.printf("[BOOT] #%u seq=%lu %s phase=%s owner=%u pendingUpload=%lu heapFree=%luK largest=%luK min=%luK uptime=%lus\r\n",
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


