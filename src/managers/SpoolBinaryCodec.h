

#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <stdint.h>

namespace SpoolBin {

static constexpr uint32_t SEGMENT_MAGIC = 0x00325053UL; // "SP2\0"

struct SegmentHeaderV2 {
    uint32_t magic = SEGMENT_MAGIC;
    uint8_t version = 2;
    uint8_t flags = 0;
    uint16_t headerSize = sizeof(SegmentHeaderV2);
    uint32_t segmentId = 0;
    uint32_t createdMs = 0;
    uint32_t firstEventId = 0;
    uint32_t lastEventId = 0;
    uint32_t recordCount = 0;
    uint32_t bodyBytes = 0;
    uint32_t dictOffset = 0;
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
};

enum RecordType : uint8_t {
    REC_EVENT = 1,
    REC_ENRICH_DELTA = 2,
    REC_DICT_ADD = 3,
    REC_CHECKPOINT = 4
};

struct RecordPrefix {
    uint8_t type = 0;
    uint8_t flags = 0;
    uint16_t length = 0;
};

bool writeBytes(File& f, const void* data, size_t len);
bool readBytes(File& f, void* data, size_t len);

bool writeUVarint(File& f, uint32_t value);
bool readUVarint(File& f, uint32_t& out);

bool writeVarintZigZag(File& f, int32_t value);
bool readVarintZigZag(File& f, int32_t& out);

bool readSegmentHeaderV2(File& f, SegmentHeaderV2& hdr);
bool writeSegmentHeaderV2(File& f, const SegmentHeaderV2& hdr);

bool appendRecordV2(const String& path,
                    uint8_t recType,
                    const uint8_t* payload,
                    uint16_t length,
                    uint32_t eventId,
                    SegmentHeaderV2* outHeader = nullptr);

// Append one record to an already-open file at its current write position.
// Updates hdr counters in memory but does NOT write hdr back to disk —
// the caller must call writeSegmentHeaderV2 once at the end of the batch.
bool appendRecordToOpen(File& f,
                        uint8_t recType,
                        const uint8_t* payload,
                        uint16_t length,
                        uint32_t eventId,
                        SegmentHeaderV2& hdr);

bool createEmptySegmentV2(const String& path, uint32_t segmentId, uint32_t createdMs);

} // namespace SpoolBin


