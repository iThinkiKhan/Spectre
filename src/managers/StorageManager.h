#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include "../core/Session.h"
#include "../core/EventBus.h"
#include "../core/SpectreState.h"
#include "../data/Schema.h"
#include "SpoolBinaryCodec.h"
#include "TimeService.h"

// OWNERSHIP CONTRACT
// - TaskHardware is the sole mutator of persistent storage state.
// - UI code may read mirrored state only; it must not write to LittleFS.
// - Upload watermark deferral is the only supported way to carry event-ack
//   progress through an active RADIO_WIFI_UPLOAD window.

struct DeviceConfig {
    String name;
    String owner;
    String version;

    long   loraFreq;
    int    loraNetworkId;
    int    loraAddress;
    int    loraSF;
    int    loraBW;
    int    loraCR;
    int    loraPreamble;

    String mqttBroker;
    int    mqttPort;
    String mqttUser;
    String mqttPassword;
    String mqttTopicBase;

    std::vector<std::pair<String,String>> wifiNetworks;
};

enum EventStatus : uint8_t {
    EVT_RAW      = 0,
    EVT_ENRICHED = 1,
    EVT_UPLOADED = 2
};

enum StorageEnrichmentState : uint8_t {
    STORAGE_ENRICH_NOT_ELIGIBLE = 0,
    STORAGE_ENRICH_PENDING = 1,
    STORAGE_ENRICH_DONE = 2
};

enum StoragePressureMode : uint8_t {
    STORAGE_MODE_NORMAL = 0,
    STORAGE_MODE_WATCH,
    STORAGE_MODE_FULL,
    STORAGE_MODE_OVERRUN
};

enum StorageRetentionPolicy : uint8_t {
    STORAGE_POLICY_NORMAL = 0,
    STORAGE_POLICY_REDUCED,
    STORAGE_POLICY_CRITICAL_ONLY
};

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

static constexpr uint16_t SPOOL_SEGMENT_SUMMARY_VERSION = 3;

struct StorageLaneCounts {
    uint32_t mission = 0;
    uint32_t noise = 0;

    uint32_t total() const {
        return mission + noise;
    }
};

struct StorageSessionSummary {
    uint32_t missionTotal = 0;
    uint32_t noiseTotal = 0;

    uint32_t p0Total = 0;
    uint32_t p1Total = 0;
    uint32_t p2Total = 0;
    uint32_t p3Total = 0;

    uint32_t pendingUploadMission = 0;
    uint32_t pendingUploadNoise = 0;

    uint32_t pendingEnrichmentMission = 0;
    uint32_t pendingEnrichmentNoise = 0;

    uint32_t enrichmentDeltas = 0;

    uint32_t firstEventId = 0;
    uint32_t lastEventId = 0;
};

struct DedupStats {
    uint32_t suppressed = 0;
    uint32_t dropped    = 0;
};

struct DedupWindowEntry {
    String   key;
    uint32_t firstSeenMs = 0;
    uint32_t lastSeenMs  = 0;
    uint32_t count       = 0;
};

struct HandshakeProgress {
    String   apMac;
    String   staMac;
    String   ssid;
    uint8_t  frameMask = 0;
    uint32_t lastUpdateMs = 0;
    bool     complete = false;
};

enum AppendEventStatus : uint8_t {
    APPEND_OK = 0,
    APPEND_SUPPRESSED_DUPLICATE,
    APPEND_DROPPED_POLICY,
    APPEND_FAILED_INVALID,
    APPEND_FAILED_PARSE,
    APPEND_FAILED_NOT_READY,
    APPEND_FAILED_NO_SESSION,
    APPEND_FAILED_IO
};

struct AppendEventResult {
    AppendEventStatus status = APPEND_FAILED_INVALID;
    uint32_t eventId = 0;

    bool ok() const { return status == APPEND_OK && eventId != 0; }
    bool suppressed() const { return status == APPEND_SUPPRESSED_DUPLICATE; }
    bool dropped() const { return status == APPEND_DROPPED_POLICY; }
    bool failed() const {
        return !(ok() || suppressed() || dropped());
    }
};

enum SpoolSegmentFormat : uint8_t {
    SPOOL_SEGMENT_JSONL = 1,
    SPOOL_SEGMENT_BIN_V2 = 2
};

enum SpoolDecodedRecordType : uint8_t {
    SPOOL_REC_UNKNOWN = 0,
    SPOOL_REC_EVENT = 1,
    SPOOL_REC_ENRICH_DELTA = 2
};

struct DecodedSpoolRecord {
    SpoolDecodedRecordType recordType = SPOOL_REC_UNKNOWN;
    uint32_t eventId = 0;
    String sessionId;
    JsonDocument doc;
};

struct SpoolEnrichBatchEntry {
    uint32_t    eventId    = 0;
    const char* sessionId  = nullptr;
    float       lat        = 0.0f;
    float       lon        = 0.0f;
    float       alt        = 0.0f;
    float       acc        = 0.0f;
    const char* tag        = nullptr;
};

struct SpoolEnrichmentDelta {
    uint32_t id = 0;
    float lat = 0.0f;
    float lon = 0.0f;
    float alt = 0.0f;
    float acc = 0.0f;
    String tag;
    uint32_t ts = 0;
};

struct SpoolSegmentInfo {
    uint32_t segmentId = 0;
    uint32_t firstEventId = 0;
    uint32_t lastEventId = 0;
    uint16_t summaryVersion = 0;
    bool summaryValid = false;  // cache-only; fall back to scan when false

    uint32_t recordCount = 0;
    uint32_t eventCount = 0;
    uint32_t enrichDeltaCount = 0;

    uint32_t missionCount = 0;
    uint32_t noiseCount = 0;
    uint32_t pendingUploadMissionCount = 0;
    uint32_t pendingUploadNoiseCount = 0;

    uint32_t p0Count = 0;
    uint32_t p1Count = 0;
    uint32_t p2Count = 0;
    uint32_t p3Count = 0;

    // Events ORIGINATING in this segment that want enrichment but haven't
    // been enriched anywhere yet. Maintained live: incremented on append
    // when the event wants enrichment; decremented when a delta lands
    // anywhere referencing one of this segment's eventIds. Lets hot-path
    // getters skip drained segments instead of scanning every record.
    uint32_t pendingEnrichmentCount = 0;

    uint32_t minTimestampMs = 0;
    uint32_t maxTimestampMs = 0;

    uint32_t approxBytes = 0;
    uint8_t format = SPOOL_SEGMENT_JSONL;
};

struct PendingEventDescriptor {
    uint32_t eventId = 0;
    uint32_t timestampMs = 0;
    uint8_t type = 0;
    uint8_t status = 0;
    uint8_t lane = static_cast<uint8_t>(STORAGE_LANE_NOISE);
    uint8_t priority = static_cast<uint8_t>(STORAGE_PRIO_P3);
    uint16_t valueScore = 0;
};

static constexpr uint32_t UIX_MAGIC = 0x00584955UL; // "UIX\0"
static constexpr uint16_t UIX_RECORD_LEN_V1 = 68;

struct UploadIndexRecordV1 {
    uint32_t magic = UIX_MAGIC;
    uint8_t  version = 1;
    uint8_t  reserved0 = 0;
    uint16_t recordLen = UIX_RECORD_LEN_V1;
    uint32_t segmentId = 0;
    uint32_t offset = 0;
    uint32_t len = 0;
    uint32_t eventId = 0;
    char     sessionId[40] = "";
    uint32_t crc = 0;
};

static_assert(sizeof(UploadIndexRecordV1) == 68,
              "UploadIndexRecordV1 must stay fixed-size");

struct UploadIndexStats {
    uint32_t indexedEvents = 0;
    uint32_t sessions = 0;
};

static constexpr size_t UPLOAD_INDEX_PAGE_CAPACITY = 32;

struct UploadIndexPage {
    UploadIndexRecordV1 records[UPLOAD_INDEX_PAGE_CAPACITY];
    uint8_t count = 0;
};

struct UploadIndexPagedSession {
    std::vector<std::unique_ptr<UploadIndexPage>> pages;
    uint32_t count = 0;
};

struct SpoolIndex {
    uint32_t version = 1;
    uint8_t format = SPOOL_SEGMENT_JSONL;
    uint32_t nextSegmentId = 1;
    uint32_t activeSegmentId = 0;
    uint32_t oldestSegmentId = 0;
    uint32_t pendingTotal = 0;
    std::vector<String> sessions;
    std::vector<std::pair<String, uint32_t>> uploadedWatermarks;
    std::vector<SpoolSegmentInfo> segments;
};

struct BadUsbScriptInfo {
    char name[32];
    char file[40];
    char desc[48];
    uint16_t lineCount;
    bool valid;
};

enum class SpoolScanStatus : uint8_t {
    OK = 0,          // all records decoded cleanly
    OK_WITH_SKIPS,   // some records were bad but framing held
    FATAL            // segment unreadable / structurally broken
};

class StorageManager {
public:
    static StorageManager& getInstance() {
        static StorageManager instance;
        return instance;
    }

    bool begin();
    bool isReady() const { return _ready; }

    bool loadConfig();
    bool saveConfig();
    DeviceConfig& getConfig() { loadConfig(); return _config; }
    void setConfig(DeviceConfig& cfg) { _config = cfg; }

    bool addWifiNetwork(const String& ssid, const String& password);
    bool removeWifiNetwork(const String& ssid);
    std::vector<std::pair<String,String>> getWifiNetworks();

    bool logLoraPacket(int address, const String& payload, const String& payloadHex,
                       int rssi, int snr, long freq);

    bool logWifiScan(const String& ssid, const String& bssid, int rssi,
                     int channel, const String& encryption);

    bool logProbe(const String& mac, const String& ssid, int rssi);

    uint32_t appendEvent(const char* type, const char* payloadJson,
                         const char* sessionId = nullptr);
    uint32_t appendEvent(const char* type, JsonObjectConst payload,
                         const char* sessionId = nullptr);

    AppendEventResult appendEventDetailed(const char* type, const char* payloadJson,
                                         const char* sessionId = nullptr);
    AppendEventResult appendEventDetailed(const char* type, JsonObjectConst payload,
                                         const char* sessionId = nullptr);

    bool     getEventBatch(uint32_t sinceId, int maxCount, JsonDocument& out);
    bool     getEventBatchForSession(const char* sessionId, uint32_t sinceId,
                                     int maxCount, JsonDocument& out);
    StorageLaneCounts getPendingUploadCounts(const char* sessionId = nullptr);
    StorageLaneCounts getPendingEnrichmentCounts();
    StorageLaneCounts getPendingEnrichmentCountsForSession(const char* sessionId = nullptr);
    uint32_t getPendingEnrichmentCountForSession(const char* sessionId = nullptr);
    bool     getSessionStorageSummary(const char* sessionIdOverride,
                                      StorageSessionSummary& out);
    bool     getUploadEventBatchForSession(const char* sessionId,
                                           uint32_t sinceId,
                                           int maxCount,
                                           JsonDocument& out);
    bool     prepareUploadIndexForUpload(uint32_t budgetMs);
    bool     getNextResolvedEventForSession(const char* sessionId,
                                            uint32_t sinceId,
                                            JsonDocument& out);
    bool     getPendingEnrichmentBatchForSession(const char* sessionId,
                                                 PendingEventDescriptor* out,
                                                 size_t maxCount,
                                                 size_t& outCount);
    bool     getPendingEnrichmentBatch(PendingEventDescriptor* out,
                                       size_t maxCount,
                                       size_t& outCount);
    bool     getPendingEnrichmentBatchExcluding(const uint32_t* excludeIds,
                                                size_t excludeCount,
                                                PendingEventDescriptor* out,
                                                size_t maxCount,
                                                size_t& outCount);
    bool     forEachEventForSession(const char* sessionId,
                                    const std::function<bool(JsonObjectConst)>& cb);
    bool     markEventUploaded(uint32_t eventId,
                               const char* sessionId = nullptr,
                               uint8_t laneHint = 0xFF);
    bool     markEventsUploaded(uint32_t upToId);

    // Batch mode: while active, markEventUploaded/markEventsUploaded update
    // the in-memory watermark only — no LittleFS writes. The flush happens on
    // endUploadBatch() (or flushUploadCheckpoint() mid-upload).
    void     beginUploadBatch();
    bool     endUploadBatch();
    // Flush watermarks to flash while keeping the batch open and the radio
    // lease held. Safe to call mid-upload; does NOT violate the no-write-
    // during-radio contract because the caller accepts the brownout risk in
    // exchange for crash-safe upload progress.
    bool     flushUploadCheckpoint();
    bool     isUploadBatchActive() const { return _uploadBatchActive; }
    bool     isUploadBatchDirty() const { return _uploadBatchDirty; }
    bool     compactUploadedEventFiles(const char* sessionId = nullptr);
    int      compactAllUploadedEventFiles();

    // Hot-path accessor: returns the live in-memory counter without ever
    // scanning the spool. This is the only path the runtime should use.
    // The live counter is incremented on appendEvent and decremented on
    // markEventUploaded; passing forceRescan=true is for explicit repair
    // tooling and triggers a full O(spool) scan via recountPendingFromSpool().
    uint32_t getPendingEventCount(bool forceRescan = false);

    // Explicit, costly: walks every record in every segment to recompute
    // the pending counter and replace the live value. Reserve for boot
    // audit, manual repair, or post-quarantine recovery — never call from
    // a hot path. Returns the recounted value.
    uint32_t recountPendingFromSpool();
    uint32_t getPendingEventCountForSession(const char* sessionId);
    uint32_t getSessionPendingEventCount();
    uint32_t getLastUploadedEventId(const char* sessionId = nullptr);
    void     listEventSessions(std::vector<String>& sessionIds);
    uint32_t getNextEventId();

    bool     prepareForEnrichmentAppend(size_t expectedDeltaCount);
    bool     appendEnrichDeltasBatch(const SpoolEnrichBatchEntry* entries,
                                     size_t count,
                                     uint32_t* appliedOut = nullptr,
                                     uint32_t* failedOut  = nullptr);

    bool     enrichEvent(uint32_t eventId,
                         float lat, float lon,
                         float alt, float accuracy,
                         const char* tag);
    bool     enrichEventForSession(uint32_t eventId,
                                   const char* sessionId,
                                   float lat, float lon,
                                   float alt, float accuracy,
                                   const char* tag);
    bool     findEventSessions(const uint32_t* eventIds,
                               size_t count,
                               String* outSessionIds) const;

    bool beginSession();
    bool endSession();
    bool checkpointSessionState();

    bool ensureBadUsbVault();
    int  loadBadUsbScriptIndex(BadUsbScriptInfo* outScripts, int maxCount);
    bool readBadUsbScript(const char* fileName, String& outScript);
    bool writeBadUsbScript(const char* fileName, const char* scriptBody,
                           const char* displayName, const char* desc);

    void saveKnownLocations(SpectreState::KnownLocation* locs, int count);
    int  loadKnownLocations(SpectreState::KnownLocation* locs, int maxCount);

    size_t getTotalBytes();
    size_t getUsedBytes();
    size_t getFreeBytes();
    int    getUsedPercent();
    String getUsedString();
    String getCachedUsedString() const { return _cachedUsedString; }
    void   checkHealth();

    bool listLogFiles(std::vector<String>& files);
    bool readFile(const String& path, String& contents);
    bool deleteFile(const String& path);
    bool deleteOldestLog();
    bool wipeNonVaultStorage();

    StoragePressureMode getPressureMode() const { return _pressureMode; }
    StorageRetentionPolicy getRetentionPolicy() const { return _retentionPolicy; }
    uint32_t getDedupedCount() const { return _dedupStats.suppressed; }
    uint32_t getDroppedCount() const { return _dedupStats.dropped; }
    bool usingSpoolBackend() const { return true; }
    bool compactSpool();
    void refreshStorageUiState();
    void updateStoragePressure();
    bool hasStorageMaintenanceWork() const {
        return _repairRequested ||
               _repairJob.active ||
               _spoolAuditRepairRequired ||
               _spoolSummaryRebuildPending ||
               _pendingCountDirty;
    }
    bool requestSpoolRepair(const char* reason = nullptr);
    bool serviceStorageMaintenanceStep(uint32_t budgetMs, uint16_t maxRecords);
    bool repairStep(uint32_t budgetMs, uint16_t maxRecords);

    void beginHotPathDiagnosticsSuppressed();
    void endHotPathDiagnosticsSuppressed();
    bool shouldStoreByPriority(StoragePriority priority) const;
    bool shouldAcceptHandshakeFrame(const char* apMac,
                                    const char* staMac,
                                    const char* ssid,
                                    uint8_t messageNumber);

    void spoolAuditToSerial(bool repair);
    void spoolDiagToSerial();
    void spoolQuarantineListToSerial();
    void spoolQuarantineMetaToSerial();
    bool spoolQuarantineClear();

    // Public so file-scope helpers in the .cpp can reference without friend declarations.
    enum class SpoolCorruptionReason : uint8_t {
        NONE = 0,
        SEGMENT_OPEN_FAILED,
        SEGMENT_HEADER_INVALID,
        RECORD_LENGTH_INVALID,
        RECORD_CRC_FAILED,
        RECORD_DECODE_FAILED,
        RECORD_SEMANTIC_INVALID,
        SCAN_FAILED,
        SUMMARY_MISMATCH,
        UNKNOWN
    };

    struct SpoolAuditResult {
        uint32_t scannedSegments    = 0;
        uint32_t scannedRecords     = 0;
        uint32_t validEventRecords  = 0;
        uint32_t validEnrichDeltas  = 0;
        uint32_t invalidRecords     = 0;
        uint32_t skippedRecords     = 0;
        uint32_t quarantinedSegments = 0;
        uint32_t unreadableSegments = 0;

        uint32_t rebuiltPendingTotal = 0;
        uint32_t maxEventIdSeen      = 0;

        uint32_t oldPendingTotal  = 0;
        uint32_t oldNextEventId   = 0;

        bool hadMismatch          = false;
        bool hadFatalSegmentError = false;
        bool repaired             = false;
    };

private:
    StorageManager() {}
    enum class SegmentPendingScanResult : uint8_t {
        HAS_PENDING,
        NO_PENDING,
        SCAN_FAILED
    };

    enum class SpoolRepairAction : uint8_t {
        NONE = 0,
        SKIP_RECORD,
        QUARANTINE_SEGMENT
    };

    struct SpoolRepairJob {
        bool active = false;
        String reason;
        SpoolAuditResult audit;
        std::vector<SpoolSegmentInfo> repairedSegments;
        std::vector<String> rebuiltSessions;
        std::vector<String> segmentSessions;
        SpoolSegmentInfo originalSegment;
        SpoolSegmentInfo rebuiltSegment;
        size_t segmentIndex = 0;
        uint32_t fileOffset = 0;
        uint32_t startMs = 0;
        uint32_t segmentValidEventRecords = 0;
        uint32_t segmentValidEnrichDeltas = 0;
        String lastSession;
        bool scanningSegment = false;
        bool segmentChanged = false;
    };

    bool        _ready = false;
    DeviceConfig _config;
    String      _currentLoraLog;
    String      _currentWifiLog;
    String      _currentProbeLog;
    String      _cachedUsedString = "0KB / 0KB";
    uint32_t    _nextEventId = 1;
    uint32_t    _lastCounterSaveMs = 0;
    uint16_t    _eventCounterPendingWrites = 0;
    bool        _eventCounterDirty = false;
    uint32_t    _pendingEventCount = 0;
    bool        _pendingCountDirty = true;
    uint32_t    _lastMetaSaveMs = 0;
    uint32_t    _lastSpoolIndexSaveMs = 0;
    uint16_t    _spoolIndexPendingWrites = 0;
    bool        _spoolIndexDirty = false;
    SpoolIndex  _spoolIndex;

    // Upload-batch deferral: when _uploadBatchActive, watermark updates skip
    // flash writes; _uploadBatchDirty tracks whether endUploadBatch needs to
    // persist the spool index + event meta.
    bool        _uploadBatchActive = false;
    bool        _uploadBatchDirty  = false;
    bool        _uploadIndexResident = false;
    // When summary metadata is observed invalid during a busy radio window,
    // defer rebuilding until a quiet maintenance pass can service it.
    bool        _spoolSummaryRebuildPending = false;
    bool        _spoolAuditRepairRequired = false;
    bool        _repairRequested = false;
    bool        _suppressHotPathDiagnostics = false;
    SpoolRepairJob _repairJob;

    StoragePressureMode    _pressureMode = STORAGE_MODE_NORMAL;
    StorageRetentionPolicy _retentionPolicy = STORAGE_POLICY_NORMAL;
    DedupStats             _dedupStats = {};
    std::vector<DedupWindowEntry> _dedupWindow;
    std::vector<HandshakeProgress> _handshakeWindow;
    std::map<uint32_t, String> _binaryLastSessionBySegment;
    std::map<String, UploadIndexPagedSession> _uploadIndexBySession;
    std::map<String, std::map<uint32_t, SpoolEnrichmentDelta>> _uploadEnrichBySession;
    std::vector<String> _uploadIndexSessions;
    UploadIndexStats _uploadIndexStats;

    static constexpr uint32_t DEDUP_WINDOW_MS = 5UL * 60UL * 1000UL;
    static constexpr uint32_t HANDSHAKE_WINDOW_MS = 15UL * 60UL * 1000UL;
    static constexpr size_t   DEDUP_WINDOW_MAX = 128;
    static constexpr uint8_t  STORAGE_WATCH_PCT = 80;
    static constexpr uint8_t  STORAGE_FULL_PCT = 92;
    static constexpr uint8_t  STORAGE_OVERRUN_PCT = 97;

    void _logSpoolDiagnostics(const char* reason,
                              const String& sessionId = String(),
                              bool includeRuntimeCounters = true) const;
    uint32_t _activeSessionWatermark(const String& sessionId) const;
    bool   _ensureDir(String path);
    bool   _removeTree(const String& path);
    bool   _removePathWithRetry(const String& path);
    bool   _rmdirWithRetry(const String& path);
    bool   _applyOneShotNonVaultReset();
    bool   _trimJsonLinesFile(const char* path, size_t keepLastLines);
    String _today();
    String _gpsJson();
    bool   _appendToLog(String path, String key, JsonObject& entry);
    bool   _appendJsonLine(const String& path, JsonDocument& doc);
    uint32_t _lastUploadedEventIdForSession(const String& sessionId);
    uint32_t _pendingEventCountForSession(const String& sessionId);
    uint32_t _pendingEventCountForSessionFromSpool(const String& sessionId) const;
    bool   _loadEventCounter();
    bool   _persistEventCounter(bool force = false);
    bool   _loadEventMeta();
    bool   _persistEventMeta(bool force = false);
    void   _initDefaultConfig();
    String _spoolDir() const;
    String _spoolIndexPath() const;
    String _spoolSegmentPath(uint32_t segmentId) const;
    String _spoolBinarySegmentPath(uint32_t segmentId) const;
    String _uploadIndexPath(uint32_t segmentId) const;
    String _spoolSegmentPathForFormat(uint32_t segmentId, uint8_t format) const;
    bool _ensureSpoolReady();
    bool _loadSpoolIndex();
    bool _rebuildUploadIndex();
    bool _rebuildUploadIndexSegment(const SpoolSegmentInfo& seg);
    bool _validateUploadIndexRecord(const UploadIndexRecordV1& rec,
                                    uint32_t expectedSegmentId) const;
    bool _addUploadPtrToMemory(const UploadIndexRecordV1& rec);
    void _releaseUploadIndexMemory(const char* reason);
    // force=true guarantees an immediate write. force=false is the hot-path
    // mode — it throttles to ~5s and defers while an upload batch is active.
    bool _persistSpoolIndex(bool force = false);
    bool _openNewSpoolSegment();
    SpoolSegmentInfo* _findSegmentInfo(uint32_t segmentId);
    const SpoolSegmentInfo* _findSegmentInfo(uint32_t segmentId) const;
    void _rememberSession(const String& sessionId);
    uint32_t _uploadedWatermarkForSession(const String& sessionId) const;
    void _setUploadedWatermarkForSession(const String& sessionId, uint32_t eventId);
    bool _appendSpoolRecord(JsonDocument& doc, uint32_t* outEventId = nullptr);
    bool _appendSpoolEnrichmentDelta(const String& sessionId,
                                     uint32_t eventId,
                                     float lat, float lon,
                                     float alt, float accuracy,
                                     const char* tag);
    bool _loadSpoolEnrichmentsForSession(const String& sessionId,
                                         std::vector<SpoolEnrichmentDelta>& enrichments) const;
    bool _loadSpoolEnrichmentIds(const String& sessionId,
                                 bool filterBySession,
                                 std::map<uint32_t, bool>& enrichedById) const;
    StorageLaneCounts _getPendingEnrichmentCounts(const String& sessionId,
                                                  bool filterBySession);
    bool _getPendingEnrichmentBatch(const String& sessionId,
                                    bool filterBySession,
                                    PendingEventDescriptor* out,
                                    size_t maxCount,
                                    size_t& outCount,
                                    const uint32_t* excludeIds = nullptr,
                                    size_t excludeCount = 0);
    void _clearSegmentSummary(SpoolSegmentInfo& seg);
    void _updateSegmentSummaryFromDecodedRecord(SpoolSegmentInfo& seg,
                                                const DecodedSpoolRecord& rec);
    void _updateSegmentSummaryFromEventDoc(SpoolSegmentInfo& seg,
                                           JsonObjectConst doc,
                                           uint32_t eventId,
                                           uint32_t timestampMs,
                                           bool pendingUpload = true);
    void _markSegmentEnrichmentDelta(SpoolSegmentInfo& seg,
                                     uint32_t recordId,
                                     uint32_t timestampMs);
    // Find the segment whose [firstEventId, lastEventId] contains eventId
    // and decrement its pendingEnrichmentCount. Bounded O(num_segments),
    // never opens a record file.
    //
    // CLAMP/RECOUNT semantics — conservative because a duplicate delta
    // against an already-enriched event would double-decrement the live
    // counter, and the runtime does not maintain an in-memory enriched-
    // set (memory rejected at 2k+ event scale):
    //   * stale segment summary           → flag _pendingCountDirty
    //   * counter already 0 (likely dup)  → flag _pendingCountDirty
    //   * counter > 0                     → decrement once
    // Audit/rebuild paths dedupe at the call site (per-eventId map) so
    // duplicate deltas in the spool decrement the gross count exactly
    // once during repair.
    void _decrementPendingEnrichmentForEvent(uint32_t eventId);
    void _decrementPendingUploadForEvent(uint32_t eventId,
                                         uint8_t laneHint,
                                         const char* reason);
    bool _rebuildSegmentSummary(SpoolSegmentInfo& seg);
    bool _rebuildInvalidSegmentSummaries(bool force = false);
    bool _hasInvalidSpoolSummaries() const;
    bool _canRebuildSpoolSummariesNow() const;
    bool _servicePendingSpoolSummaryRebuild();
    bool _segmentContainsRecords(uint32_t segmentId) const;
    bool _segmentFullyUploaded(uint32_t segmentId) const;
    void _rebuildSessionListFromSpool();
    uint32_t _rescanPendingEventCountFromSpool();
    bool _getEventBatchForSessionFromSpool(const String& sessionId,
                                           uint32_t sinceId,
                                           int maxCount,
                                           JsonDocument& out);
    bool _getUploadEventBatchForSessionFromIndex(const String& sessionId,
                                                 uint32_t sinceId,
                                                 int maxCount,
                                                 JsonDocument& out);
    bool _decodeBinarySpoolRecordBody(uint32_t segmentId,
                                      uint8_t recordType,
                                      const uint8_t* data,
                                      size_t len,
                                      uint32_t tsBase,
                                      const String& sessionSeed,
                                      DecodedSpoolRecord& out) const;
    bool _forEachResolvedEventForSession(const String& sessionId,
                                         uint32_t sinceId,
                                         int maxCount,
                                         const std::function<bool(JsonObjectConst)>& cb) const;
    SegmentPendingScanResult _segmentPendingScanResult(uint32_t segmentId) const;
    bool _segmentScanReadable(uint32_t segmentId) const;
    bool _pruneUploadedSessionState();
    bool _removeSpoolSegmentFile(uint32_t segmentId);
    bool _resyncSpoolIndexFromFilesystem();
    int _cleanupLegacyUploadSidecars();
    int _cleanupLegacyRawSessionFiles();
    int _cleanupLegacyEnrichSidecars();
    String _makeDedupKey(const char* type, JsonObjectConst payload) const;
    StoragePriority _priorityForEventType(const char* type,
                                          JsonObjectConst payload) const;
    StorageLane _laneForEventType(const char* type,
                                  JsonObjectConst payload) const;
    StorageLane _eventRecordLane(JsonObjectConst doc) const;
    StoragePriority _eventRecordPriority(JsonObjectConst doc) const;
    StoragePriority _finalPriorityForStoredEvent(JsonObjectConst doc) const;
    uint16_t _eventRecordValueScore(JsonObjectConst doc) const;
    bool _eventRecordWantsEnrichment(JsonObjectConst doc) const;
    bool _eventRecordPendingEnrichment(JsonObjectConst doc) const;
    bool _findEventSession(uint32_t eventId, String& outSessionId) const;
    const char* _laneText(StorageLane lane) const;
    bool _eventRecordIsMission(JsonObjectConst doc) const;
    bool _shouldSuppressDuplicate(const char* type,
                                  JsonObjectConst payload,
                                  StoragePriority priority);
    void _trimDedupWindows();
    const char* _policyText() const;
    void _publishStorageEventIfNeeded(StoragePressureMode oldMode,
                                      StoragePressureMode newMode);
    void _maybeCompactForPressure(StoragePressureMode oldMode,
                                  StoragePressureMode newMode);
    bool _appendSegmentRecord(SpoolSegmentInfo& seg,
                              JsonDocument& doc,
                              uint32_t* outEventId = nullptr,
                              SpoolBin::AppendRecordLocation* outLoc = nullptr);
    bool _scanSegmentRecords(uint32_t segmentId,
                         std::function<bool(const DecodedSpoolRecord&)> cb) const;
    bool _scanJsonlSegmentRecords(uint32_t segmentId,
                              std::function<bool(const DecodedSpoolRecord&)> cb) const;
    bool _scanBinarySegmentRecords(uint32_t segmentId,
                               std::function<bool(const DecodedSpoolRecord&)> cb) const;
    const char* _segmentFormatText(uint8_t format) const;

    bool _auditAndRepairSpool(const char* reason, bool repair, SpoolAuditResult* out);
    void _startRepairJob(const char* reason);
    void _resetRepairJob();
    bool _beginRepairSegment();
    bool _repairJsonlSlice(uint32_t startMs,
                           uint32_t budgetMs,
                           uint16_t maxRecords,
                           uint16_t& recordsScanned);
    bool _repairBinaryMetaSlice(uint32_t startMs,
                                uint32_t budgetMs,
                                uint16_t maxRecords,
                                uint16_t& recordsScanned);
    void _finishRepairSegment();
    bool _finalizeRepairJob();
    void _rememberRepairSession(const String& sessionId);
    bool _scanSegmentForAudit(SpoolSegmentInfo& rebuilt,
                              SpoolAuditResult& audit,
                              std::vector<String>& rebuiltSessions,
                              bool repair);
    // Does NOT remove the segment from _spoolIndex.segments; caller excludes it.
    bool _quarantineSpoolSegment(uint32_t segmentId,
                                 SpoolCorruptionReason reason,
                                 const char* detail);
    bool _writeQuarantineMeta(uint32_t segmentId,
                              SpoolCorruptionReason reason,
                              const char* detail,
                              const String& originalPath,
                              const String& quarantinePath,
                              const String& metaPath);
    bool _isSegmentQuarantined(uint32_t segmentId) const;
    void _logSpoolAuditResult(const char* reason, const SpoolAuditResult& audit);
    // No I/O, no scan. Invariant failures only degrade counter trust and queue
    // explicit maintenance; they never run a spool scan inline.
    bool _checkSpoolInvariants(const char* reason, bool repairIfBad);
};

#define STORAGE StorageManager::getInstance()
