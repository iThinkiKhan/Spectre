#pragma once

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Task-stack placement audit.
//
// A stack in PSRAM is a latent DoubleException: spi_flash disables the cache
// for a write, PSRAM goes with it, and the next register-window spill faults
// inside the exception handler. The panic lands hours after the mistake, with
// no usable backtrace on the app CDC — the 2026-08-20 and 2026-08-21 overnight
// panics were both this, confirmed from the coredump:
//
//   exccause 0x42 (DoubleException)  excvaddr 0xffffffe0
//   epc6 -> _WindowOverflow8         epc1 -> esp_psram_check_ptr_addr
//
// Nobody has to ask for a PSRAM stack to get one. pvPortMallocStack resolves to
// pvPortMalloc -> MALLOC_CAP_8BIT, which matches internal AND PSRAM, and the
// prebuilt Arduino IDF sets CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y — so a
// plain xTaskCreate() silently falls back to PSRAM whenever internal DRAM is
// too tight or too fragmented to satisfy it. That makes it pressure-dependent
// and effectively unreproducible on a quiet bench.
//
// Lives in a header so both the boot path and BLEManager can run it: BLE brings
// up nimble_host and btController at the tightest moment in the whole duty
// cycle, which is precisely when the fallback fires.

namespace TaskStackAudit {

struct Result {
    uint32_t offenders = 0;   // task stacks found outside internal RAM
    uint32_t scanned   = 0;
};

// Walks every task and reports which stacks are not in internal RAM.
// `verbose` also prints the clean ones. `sink` may be null for a silent check.
inline Result run(bool verbose, Print* sink = &Serial) {
    Result out;
    const UBaseType_t count = uxTaskGetNumberOfTasks();
    TaskStatus_t* tasks = static_cast<TaskStatus_t*>(
        heap_caps_calloc(count, sizeof(TaskStatus_t),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!tasks) {
        if (sink) sink->println("[STACK] audit skipped (no internal memory for snapshot)");
        return out;
    }

    const UBaseType_t got = uxTaskGetSystemState(tasks, count, nullptr);
    out.scanned = static_cast<uint32_t>(got);
    for (UBaseType_t i = 0; i < got; i++) {
        const void* base = static_cast<const void*>(tasks[i].pxStackBase);
        const bool internal = esp_ptr_internal(base);
        if (!internal) out.offenders++;
        if (sink && (!internal || verbose)) {
            sink->printf("[STACK] %-16s base=%p %s\r\n",
                         tasks[i].pcTaskName ? tasks[i].pcTaskName : "?",
                         base,
                         internal ? "internal" : "*** EXTERNAL (PSRAM) ***");
        }
    }

    if (sink) {
        if (out.offenders > 0) {
            sink->printf("[STACK] *** %lu task stack(s) in PSRAM - DoubleException risk ***\r\n",
                         static_cast<unsigned long>(out.offenders));
        } else if (verbose) {
            sink->printf("[STACK] all %lu task stacks internal\r\n",
                         static_cast<unsigned long>(out.scanned));
        }
    }

    heap_caps_free(tasks);
    return out;
}

}  // namespace TaskStackAudit
