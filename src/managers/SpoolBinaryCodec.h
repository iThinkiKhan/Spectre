


#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <stdint.h>

namespace SpoolBin {

static constexpr uint32_t SEGMENT_MAGIC = 0x00325053UL; // "SP2\0"
static constexpr uint32_t CHECKPOINT_MAGIC = 0x31435053UL; // "SPC1"

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
    // Physical offset of the end of the last committed record (including
    // checkpoint sidecars). 0 = unknown (segment written before this field
    // existed); readers/writers must then fall back to the physical file size.
    // Updated in the same header write that commits record counters, so a
    // record interrupted before the header rewrite is simply not committed
    // and the next append overwrites the torn bytes.
    uint32_t tailOffset = 0;
    uint32_t reserved1 = 0;
};

static_assert(sizeof(SegmentHeaderV2) == 44, "on-disk layout must not change");

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

static_assert(sizeof(RecordPrefix) == 4, "on-disk layout must not change");

// Compact segment-local summary checkpoint appended into binary segments.
// This is written as a regular record body behind REC_CHECKPOINT.
struct SpoolSegmentCheckpointV1 {
    uint32_t magic = CHECKPOINT_MAGIC;
    uint16_t version = 1;
    // Explicit padding: this struct is memcpy'd to/from disk and CRC'd as raw
    // bytes, so the 2 alignment bytes after `version` must be deterministic.
    uint16_t pad0 = 0;
    uint32_t segmentId = 0;
    uint32_t lastEventId = 0;
    uint32_t recordCount = 0;
    uint32_t eventCount = 0;
    uint32_t enrichDeltaCount = 0;

    uint32_t missionCount = 0;
    uint32_t noiseCount = 0;

    uint32_t pendingUploadMissionCount = 0;
    uint32_t pendingUploadNoiseCount = 0;

    uint32_t pendingEnrichmentCount = 0;

    uint32_t p0Count = 0;
    uint32_t p1Count = 0;
    uint32_t p2Count = 0;
    uint32_t p3Count = 0;

    uint32_t minTimestampMs = 0;
    uint32_t maxTimestampMs = 0;

    uint32_t bodyOffset = 0;
    uint32_t crc32 = 0;
};

static_assert(sizeof(SpoolSegmentCheckpointV1) == 80, "on-disk layout must not change");

bool writeBytes(fs::File& f, const void* data, size_t len);
bool readBytes(fs::File& f, void* data, size_t len);

bool writeUVarint(fs::File& f, uint32_t value);
bool readUVarint(fs::File& f, uint32_t& out);

bool writeVarintZigZag(fs::File& f, int32_t value);
bool readVarintZigZag(fs::File& f, int32_t& out);

// Reads and validates the segment header (magic, version, headerSize).
// Returns false on short reads or a corrupted/foreign header.
bool readSegmentHeaderV2(fs::File& f, SegmentHeaderV2& hdr);
bool writeSegmentHeaderV2(fs::File& f, const SegmentHeaderV2& hdr);

// Physical extent of committed data in the segment. Returns hdr.tailOffset
// when it is sane, otherwise falls back to the physical file size (legacy
// segments written before tailOffset existed). Bytes past this offset are a
// torn tail from an interrupted append and must be ignored/overwritten.
uint32_t committedSegmentEnd(fs::File& f, const SegmentHeaderV2& hdr);

struct AppendRecordLocation {
    uint32_t offset = 0;
    uint32_t len    = 0;
};

bool appendRecordV2(const String& path,
                    uint8_t recType,
                    const uint8_t* payload,
                    uint16_t length,
                    uint32_t eventId,
                    SegmentHeaderV2* outHeader = nullptr,
                    AppendRecordLocation* outLoc = nullptr);

// Append one record to an already-open file at its current write position.
// Updates hdr counters in memory but does NOT write hdr back to disk —
// the caller must call writeSegmentHeaderV2 once at the end of the batch.
bool appendRecordToOpen(fs::File& f,
                        uint8_t recType,
                        const uint8_t* payload,
                        uint16_t length,
                        uint32_t eventId,
                        SegmentHeaderV2& hdr);

bool appendCheckpointRecordV1(const String& path,
                              SpoolSegmentCheckpointV1& checkpoint,
                              AppendRecordLocation* outLoc = nullptr);

bool decodeCheckpointRecordV1(const uint8_t* data,
                              size_t length,
                              uint32_t bodyOffset,
                              SpoolSegmentCheckpointV1& out);

bool encodeFieldMapV1(JsonObjectConst doc,
                      uint8_t* out,
                      size_t capacity,
                      size_t& written);

bool decodeFieldMapV1(const uint8_t* data,
                      size_t length,
                      JsonObject out);

bool createEmptySegmentV2(const String& path, uint32_t segmentId, uint32_t createdMs);

} // namespace SpoolBin



