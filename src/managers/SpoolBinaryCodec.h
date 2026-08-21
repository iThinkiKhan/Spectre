


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
    // Absolute UTC epoch (seconds) corresponding to createdMs, stamped when a
    // trusted clock is available at segment creation or backfilled later in the
    // same boot. 0 means "unknown" — records in this segment cannot be mapped
    // to wall-clock time and are therefore unenrichable. Occupies what used to
    // be reserved0, so the on-disk layout is unchanged (old segments read 0).
    uint32_t createdEpochUtc = 0;
    uint32_t reserved1 = 0;
};

enum RecordType : uint8_t {
    REC_EVENT = 1,
    REC_ENRICH_DELTA = 2,
    REC_DICT_ADD = 3,
    REC_CHECKPOINT = 4,
    // Delta-coded enrichment record. Same one-record-per-enrichment cardinality
    // as REC_ENRICH_DELTA -- every segment counter, event-id range and audit
    // path is unchanged -- but each numeric field is stored as a delta from the
    // previous enrichment record in the same segment. Safe because enrich
    // deltas are only ever read by sequential scan; the random-access decoder
    // rejects any record that is not REC_EVENT.
    REC_ENRICH_DELTA_V2 = 5
};

struct RecordPrefix {
    uint8_t type = 0;
    uint8_t flags = 0;
    uint16_t length = 0;
};

// Compact segment-local summary checkpoint appended into binary segments.
// This is written as a regular record body behind REC_CHECKPOINT.
struct SpoolSegmentCheckpointV1 {
    uint32_t magic = CHECKPOINT_MAGIC;
    uint16_t version = 1;
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

bool writeBytes(fs::File& f, const void* data, size_t len);
bool readBytes(fs::File& f, void* data, size_t len);

bool writeUVarint(fs::File& f, uint32_t value);
bool readUVarint(fs::File& f, uint32_t& out);

bool writeVarintZigZag(fs::File& f, int32_t value);
bool readVarintZigZag(fs::File& f, int32_t& out);

bool readSegmentHeaderV2(fs::File& f, SegmentHeaderV2& hdr);
bool writeSegmentHeaderV2(fs::File& f, const SegmentHeaderV2& hdr);

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

// Append a checkpoint sidecar to an already-open segment file at its current
// write position. Checkpoints do not advance SegmentHeaderV2 counters.
bool appendCheckpointRecordToOpen(fs::File& f,
                                  SpoolSegmentCheckpointV1& checkpoint,
                                  AppendRecordLocation* outLoc = nullptr);

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

// =====================================================================
// Zero-allocation field map reader. Mirrors decodeFieldMapV1's wire
// format but never inflates the body into a JsonDocument — readers walk
// the byte stream in place. Use this for hot scan paths (pending event
// classification, enrichment delta extraction) where the per-record
// allocation cost of JsonDocument is prohibitive at backlog scale.
// =====================================================================

enum FieldMapValueType : uint8_t {
    FMV_NULL   = 0,
    FMV_STRING = 1,
    FMV_INT    = 2,
    FMV_UINT   = 3,
    FMV_FLOAT  = 4,
    FMV_BOOL   = 5
};

// Lightweight value view. String values point into the source buffer —
// they are NOT null-terminated and are valid only for the duration of
// the visit/find call. Callers that need to retain a string must copy.
struct FieldValueView {
    FieldMapValueType type = FMV_NULL;
    int32_t i32 = 0;
    uint32_t u32 = 0;
    float f32 = 0.0f;
    bool b = false;
    const char* strData = nullptr;
    uint32_t strLen = 0;
};

// Visit each (key, value) in a v1 field map. Visitor returns false to
// stop iteration early (returns true for "continue"). Key bytes are
// not null-terminated; compare with keyMatches() or strncmp+length.
// Returns true iff the whole map parsed cleanly (or visitor stopped
// without an error).
using FieldMapVisitFn = bool (*)(void* userCtx,
                                 const char* keyData,
                                 uint32_t keyLen,
                                 const FieldValueView& value);
bool visitFieldMapV1(const uint8_t* data,
                     size_t length,
                     FieldMapVisitFn visitor,
                     void* userCtx);

// Find a single field by name (null-terminated keyName). Stops at first
// hit. Returns true if found.
bool findFieldV1(const uint8_t* data,
                 size_t length,
                 const char* keyName,
                 FieldValueView& out);

// Convenience accessors — each does a single linear scan and returns
// fallback if the field is missing or the wrong type. For hot paths
// where multiple fields are needed, prefer visitFieldMapV1 to avoid
// re-walking the buffer per field.
uint32_t getFieldUInt32V1(const uint8_t* data,
                          size_t length,
                          const char* keyName,
                          uint32_t fallback);
int32_t getFieldInt32V1(const uint8_t* data,
                        size_t length,
                        const char* keyName,
                        int32_t fallback);
float getFieldFloatV1(const uint8_t* data,
                     size_t length,
                     const char* keyName,
                     float fallback);
bool getFieldBoolV1(const uint8_t* data,
                    size_t length,
                    const char* keyName,
                    bool fallback);
// Returns true if found AND type is FMV_STRING. outPtr is set into the
// source buffer (not null-terminated).
bool getFieldStringV1(const uint8_t* data,
                      size_t length,
                      const char* keyName,
                      const char*& outPtr,
                      uint32_t& outLen);

// Compare a non-null-terminated field key against a C string literal.
inline bool fieldKeyEquals(const char* keyData,
                           uint32_t keyLen,
                           const char* keyLiteral) {
    if (!keyLiteral) return false;
    const size_t litLen = strlen(keyLiteral);
    if (litLen != keyLen) return false;
    return keyLen == 0 || memcmp(keyData, keyLiteral, keyLen) == 0;
}

bool createEmptySegmentV2(const String& path, uint32_t segmentId, uint32_t createdMs,
                          uint32_t createdEpochUtc = 0);

} // namespace SpoolBin



