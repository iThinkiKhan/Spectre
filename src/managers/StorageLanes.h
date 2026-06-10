
#pragma once

#include <Arduino.h>

// Capture-classification enums shared by StorageManager, RAMSpool, DedupFilter,
// and other consumers. Moved out of StorageManager.h so DedupFilter can use
// them without pulling the god class in.

enum StoragePriority : uint8_t {
    STORAGE_PRIO_P0 = 0,
    STORAGE_PRIO_P1 = 1,
    STORAGE_PRIO_P2 = 2,
    STORAGE_PRIO_P3 = 3
};

enum StorageLane : uint8_t {
    STORAGE_LANE_MISSION = 0,
    STORAGE_LANE_NOISE   = 1
};
