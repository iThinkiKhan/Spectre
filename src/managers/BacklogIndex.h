
#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <map>
#include <memory>
#include <new>
#include <vector>

#include "../core/SpiramAllocator.h"
#include "SpoolBinaryCodec.h"
#include "StorageLanes.h"

// On-disk + in-memory data shapes for the upload backlog index.
//
// Phase-A foundation for the BacklogIndex extraction. The methods that
// build, query, and persist these structures still live on StorageManager
// — they currently call back into the spool/segment machinery during
// pagination, which a future cut will sever once the upload module owns
// its own state.

static constexpr uint32_t UIX_MAGIC = 0x00584955UL; // "UIX\0"
static constexpr uint16_t UIX_RECORD_LEN_V1 = 72;

struct UploadIndexRecordV1 {
    uint32_t magic = UIX_MAGIC;
    uint8_t  version = 1;
    uint8_t  reserved0 = 0;
    uint16_t recordLen = UIX_RECORD_LEN_V1;
    uint32_t segmentId = 0;
    uint32_t offset = 0;
    uint32_t len = 0;
    uint32_t eventId = 0;
    uint8_t  lane = static_cast<uint8_t>(STORAGE_LANE_NOISE);
    uint8_t  priority = static_cast<uint8_t>(STORAGE_PRIO_P3);
    uint16_t reserved1 = 0;
    char     sessionId[40] = "";
    uint32_t crc = 0;
};

static_assert(sizeof(UploadIndexRecordV1) == 72,
              "UploadIndexRecordV1 must stay fixed-size");

struct UploadIndexStats {
    uint32_t indexedEvents = 0;
    uint32_t sessions = 0;
    uint32_t skippedRecords = 0;
    uint32_t failedSegments = 0;
};

static constexpr size_t UPLOAD_INDEX_PAGE_CAPACITY = 32;

struct UploadIndexPage {
    UploadIndexRecordV1 records[UPLOAD_INDEX_PAGE_CAPACITY];
    uint8_t count = 0;

    // Force pages into PSRAM; full-backlog upload can need hundreds.
    static void* operator new(size_t size, const std::nothrow_t&) noexcept {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }
    static void* operator new(size_t size) {
        // Non-throwing build (Arduino-ESP32 has -fno-exceptions): on PSRAM
        // exhaustion this returns nullptr and the caller will crash on the
        // first deref. The nothrow form above is the path used by callers
        // that check, which is what _addUploadPtrToMemory() does.
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }
    static void operator delete(void* p) noexcept {
        heap_caps_free(p);
    }
};

struct UploadIndexPagedSession {
    std::vector<std::unique_ptr<UploadIndexPage>> pages;
    uint32_t count = 0;
};

// Enrichment side: one batched-write entry (in-memory, hot-path input to
// appendEnrichDeltasBatch), one on-spool delta record, and the descriptor
// used by the enrichment-window iterator.

struct SpoolEnrichBatchEntry {
    uint32_t    eventId    = 0;
    const char* sessionId  = nullptr;
    float       lat        = 0.0f;
    float       lon        = 0.0f;
    float       alt        = 0.0f;
    float       acc        = 0.0f;
    const char* tag        = nullptr;
    uint32_t    gpsEpochUtc = 0;
    bool        noData     = false;
};

struct SpoolEnrichmentDelta {
    uint32_t id = 0;
    float lat = 0.0f;
    float lon = 0.0f;
    float alt = 0.0f;
    float acc = 0.0f;
    String tag;
    uint32_t ts = 0;
    uint32_t gpsTs = 0;
    // When true, this delta records that the event is terminally
    // unenrichable (timestamp wasn't a usable UTC epoch at capture).
    // Coordinate fields are sentinel zeros — readers should NOT surface
    // them as a real fix. Downstream consumers branch on this to mark the
    // event STORAGE_ENRICH_NO_DATA instead of STORAGE_ENRICH_DONE.
    bool noData = false;
};

struct PendingEventDescriptor {
    uint32_t eventId = 0;
    uint32_t timestampMs = 0;
    // Absolute capture UTC (seconds). 0 => no trusted time at capture, the
    // phone has nothing to match against (terminally no-data).
    uint32_t epochUtc = 0;
    uint8_t type = 0;
    uint8_t status = 0;
    uint8_t lane = static_cast<uint8_t>(STORAGE_LANE_NOISE);
    uint8_t priority = static_cast<uint8_t>(STORAGE_PRIO_P3);
    uint16_t valueScore = 0;
};

// Resident upload enrichment-delta index: sessionId -> (eventId -> delta), node
// storage forced to PSRAM (see SpiramStlAllocator).
using UploadEnrichDeltaMap =
    std::map<uint32_t, SpoolEnrichmentDelta, std::less<uint32_t>,
             SpiramStlAllocator<std::pair<const uint32_t, SpoolEnrichmentDelta>>>;
using UploadEnrichBySessionMap =
    std::map<String, UploadEnrichDeltaMap, std::less<String>,
             SpiramStlAllocator<std::pair<const String, UploadEnrichDeltaMap>>>;

// Backlog index — paged upload index + enrichment iterator window + the
// cached upload-read file handle. Lives as a data struct for Phase C; the
// methods that build and walk it (prepareUploadIndexForUpload,
// getNextUploadEventForSession, markEventUploaded, the enrichment-window
// iterator, etc.) still live on StorageManager and reach in by name. A
// follow-up cut will move those methods onto this type so it becomes the
// real BacklogIndex class.
struct BacklogIndex {
    // Resident state — set when pages are populated, cleared at teardown.
    bool uploadIndexResident = false;
    bool enrichmentIndexResident = false;

    // Upload paged index.
    std::map<String, UploadIndexPagedSession> uploadIndexBySession;
    UploadEnrichBySessionMap uploadEnrichBySession;
    std::vector<String> uploadIndexSessions;
    UploadIndexStats uploadIndexStats;
    uint32_t uploadIndexWindowLimit = 0;
    bool     uploadIndexWindowTruncated = false;

    // Enrichment iterator window.
    SpiramVector<PendingEventDescriptor> enrichmentWindow;
    SpiramVector<uint32_t> enrichmentKnownIds;
    size_t   enrichmentWindowCursor = 0;
    size_t   enrichmentWindowLimit = 0;
    bool     enrichmentWindowTruncated = false;
    size_t   enrichmentBuildIdSegmentCursor = 0;
    size_t   enrichmentBuildSegmentCursor = 0;
    size_t   enrichmentBuildCandidateCount = 0;
    uint32_t enrichmentBuildStartedMs = 0;
    bool     enrichmentBuildActive = false;
    bool     enrichmentBuildIdsReady = false;
    bool     enrichmentBuildHeapReady = false;
    bool     enrichmentBuildSawOverflow = false;

    // Cached segment-file handle for upload reads. Repeated LittleFS.open()
    // of the same .bin file across many records in a bucket fill is fragile
    // on this board (cumulative LittleFS internal state, ESP32-S3 + OPI
    // PSRAM). Keep the file open while consecutive records map to the same
    // segment; close on segment change, read failure, or upload index
    // teardown. Also explicitly closed by closeUploadReadFile() at the
    // start of each bucket fill to avoid carrying the handle across the
    // publish phase, which can churn LittleFS state via watermark writes.
    fs::File uploadReadFile;
    uint32_t uploadReadSegmentId = 0;
    uint32_t uploadReadFileSize = 0;
    uint8_t  uploadReadFormat = 0;
    SpoolBin::SegmentHeaderV2 uploadReadHeader{};
    bool     uploadReadHeaderOk = false;
};
