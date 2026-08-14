#pragma once

#include <stddef.h>
#include <esp_heap_caps.h>

// Long-lived manager objects with multi-kilobyte rings/buffers should not
// consume the small internal heap needed to alternate the S3 BLE and WiFi
// controllers.  PSRAM is initialized by IDF before C++ local statics are
// constructed on this board.  Keep an internal fallback for degraded boots.
inline void* allocateManagerStorage(size_t bytes) {
    void* storage = heap_caps_calloc(
        1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage) {
        storage = heap_caps_calloc(
            1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return storage;
}
