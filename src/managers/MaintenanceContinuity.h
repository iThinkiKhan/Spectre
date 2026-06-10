
#pragma once

#include <Arduino.h>

// On-disk continuity log for storage maintenance. Written by maintenance
// passes so the next boot can short-circuit redundant repair work if the
// stored generation/counters match the live ones.
//
// Pure persistence: CRC, magic/version validation, atomic tmp+rename.
// The "build a record from current StorageManager state" and the
// "compare loaded record to current state" logic lives on StorageManager
// because it depends on many private fields there.

struct __attribute__((packed)) MaintenanceContinuityRecord {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t size = 0;
    uint32_t completedFlags = 0;
    uint32_t remainingFlags = 0;
    uint32_t storageGeneration = 0;
    uint32_t spoolGeneration = 0;
    uint32_t eventMetaGeneration = 0;
    uint32_t eventCounterGeneration = 0;
    uint32_t activeSegmentId = 0;
    uint32_t nextEventId = 0;
    uint32_t pendingTotal = 0;
    uint32_t segmentCount = 0;
    uint32_t fsUsedBytes = 0;
    uint32_t fsTotalBytes = 0;
    uint32_t lastFsAuditCompletedMs = 0;
    uint32_t writtenMs = 0;
    uint8_t  counterTrust = 0;
    uint8_t  clean = 0;
    uint16_t reserved = 0;
    uint32_t crc = 0;
};

static_assert(sizeof(MaintenanceContinuityRecord) == 72,
              "MaintenanceContinuityRecord on-disk size must stay fixed");

namespace MaintenanceContinuity {

static constexpr uint32_t LOG_MAGIC   = 0x314C544DUL; // "MTL1"
static constexpr uint16_t LOG_VERSION = 1;

// CRC over every byte of the record except the trailing crc field itself.
uint32_t computeCrc(const MaintenanceContinuityRecord& rec);

// Reads, validates magic/version/size/CRC. On any failure `out` is reset
// to a zero-initialized record and the function returns false.
bool load(MaintenanceContinuityRecord& out);

// Sets magic/version/size on `rec` if not already set, computes CRC into
// `rec.crc`, then performs the atomic tmp+rename write. Returns false on
// any I/O failure. Does NOT retry — caller owns the fsUsedBytes settle
// retry path since it reads StorageManager-adjacent state.
bool save(MaintenanceContinuityRecord& rec);

}  // namespace MaintenanceContinuity
