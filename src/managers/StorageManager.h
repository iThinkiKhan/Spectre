
#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <new>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>
#include "../config.h"
#include "../core/Session.h"
#include "../core/EventBus.h"
#include "../core/SpectreState.h"
#include "../data/Schema.h"
#include "SpoolBinaryCodec.h"
#include "TimeService.h"
#include "BadUsbVault.h"
#include "MaintenanceContinuity.h"
#include "CounterTrust.h"
#include "StoragePressure.h"
#include "StorageLanes.h"
#include "DedupFilter.h"
#include "SpoolRepairTypes.h"
#include "SpoolSegment.h"
#include "SpoolRepair.h"
#include "BacklogIndex.h"
#include "../core/StorageUiMirror.h"

namespace RAMSpool {
struct EventSlot;
struct CaptureClassification;
}

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
    STORAGE_ENRICH_DONE = 2,
    // Event is enrichment-eligible but its captured timestamp is not a
    // trusted UTC epoch (e.g., captured before NTP/GPS time sync). The
    // phone has no way to look up GPS history for an unknown wall-clock
    // time, so we mark these records as terminally enriched-no-data
    // rather than leaving them pending forever. Distinct from DONE so
    // exporters/uploaders can surface the absence of GPS data instead
    // of treating zeros as a real fix.
    STORAGE_ENRICH_NO_DATA = 3
};

// StoragePressureMode + StorageRetentionPolicy moved to StoragePressure.h
// (included above). Live fields + side-effect paths stay on StorageManager.

// SpoolRepairMode moved to SpoolRepairTypes.h (included above).

enum StorageMaintenanceReason : uint32_t {
    STORAGE_MAINT_NONE = 0,
    STORAGE_MAINT_DIRTY_SPOOL_INDEX = 1UL << 0,
    STORAGE_MAINT_DIRTY_SUMMARY = 1UL << 1,
    STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST = 1UL << 2,
    STORAGE_MAINT_SNAPSHOT_LAGGED = 1UL << 3,
    STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY = 1UL << 4,
    STORAGE_MAINT_ACTIVE_SEGMENT_INVALID = 1UL << 5,
    STORAGE_MAINT_COUNTER_UNTRUSTED = 1UL << 6,
    STORAGE_MAINT_EMERGENCY_REPAIR = 1UL << 7,
    STORAGE_MAINT_ACTIVE_SEGMENT_NEAR_FULL = 1UL << 8,
    STORAGE_MAINT_SEGMENT_AUDIT = 1UL << 9,
    STORAGE_MAINT_DELETE_DRAINED = 1UL << 10,
    STORAGE_MAINT_CAPTURE_INDEX_DIRTY = 1UL << 11,
    STORAGE_MAINT_FS_AUDIT = 1UL << 12,
    STORAGE_MAINT_BINARY_CHECKPOINT = 1UL << 13,
    STORAGE_MAINT_CONTINUITY_PASS = 1UL << 14
};

// StoragePriority + StorageLane moved to StorageLanes.h (included above).

// SPOOL_SEGMENT_SUMMARY_VERSION moved to SpoolSegment.h (included above).

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

// DedupWindowEntry + HandshakeProgress moved to DedupFilter (private nested
// types of the DedupFilter class). DedupCounterEffect / DedupVerdict /
// HandshakeVerdict moved to DedupFilter.h.

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

// CounterTrust enum + STORAGE_COUNTER_* aliases moved to CounterTrust.h
// (included above). The state machine itself (transitions + maintenance
// side effects) lives in StorageManager because each transition fires
// maintenance-flag updates.

// SpoolSegmentTrustState moved to SpoolSegment.h.

struct QueuedAppendTiming {
    uint32_t parseMs = 0;
    uint32_t classifyMs = 0;
    uint32_t buildDocMs = 0;
    uint32_t appendSegmentMs = 0;
    uint32_t checkpointMs = 0;
    uint32_t indexPersistMs = 0;
    uint32_t counterMs = 0;
    uint32_t uiMs = 0;
    uint32_t totalMs = 0;
};

// SpoolSegmentFormat + SpoolSegmentLifecycle moved to SpoolSegment.h.

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

// Lightweight per-record header view for hot scan paths that don't need a
// full JsonDocument. Decoded directly into local fields by the binary
// scanner — no heap allocation per record. Callers that need payload-
// specific fields can request a full decode by recording the eventId/
// session and re-fetching only the records they care about.
//
// String fields are real Strings (sessionId/typeString) because the
// underlying binary decoders allocate them anyway via _readStringFromBytes.
// We don't double-allocate by pushing them into a JsonDocument on top.
struct DecodedSpoolRecordHeader {
    SpoolDecodedRecordType recordType = SPOOL_REC_UNKNOWN;
    uint32_t eventId = 0;
    uint32_t timestampMs = 0;
    String sessionId;
    String typeString;  // event type ("probe", "device", "pmkid", "drone", etc.)
    uint8_t eventFlags = 0;
    uint8_t payloadFamily = 0;
    // For ENRICH_DELTA records: the event being enriched.
    uint32_t targetEventId = 0;
};

// SpoolEnrichBatchEntry, SpoolEnrichmentDelta, PendingEventDescriptor
// moved to BacklogIndex.h. SpoolSegmentInfo moved to SpoolSegment.h.

// Upload-index types (UIX_MAGIC, UIX_RECORD_LEN_V1, UploadIndexRecordV1,
// UploadIndexStats, UPLOAD_INDEX_PAGE_CAPACITY, UploadIndexPage,
// UploadIndexPagedSession) moved to BacklogIndex.h (included above).
// Methods that build/query the index still live on StorageManager and will
// move in a follow-up cut.

struct SpoolIndex {
    uint32_t version = 1;
    uint32_t generation = 0;
    uint8_t format = SPOOL_SEGMENT_JSONL;
    uint32_t nextSegmentId = 1;
    uint32_t activeSegmentId = 0;
    uint32_t oldestSegmentId = 0;
    uint32_t pendingTotal = 0;
    uint32_t nextEventId = 1;
    std::vector<String> sessions;
    std::vector<std::pair<String, uint32_t>> uploadedWatermarks;
    std::vector<SpoolSegmentInfo> segments;
};

// BadUsbScriptInfo moved to BadUsbVault.h (included above) so the vault
// is self-contained. StorageManager keeps the BadUSB methods below as
// thin facades for now.

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

    // Synchronous exceptional append only. Normal capture must go through
    // RAMSpool::enqueue() and be persisted by appendQueuedRecord().
    AppendEventResult appendEventDetailed(const char* type, const char* payloadJson,
                                         const char* sessionId = nullptr,
                                         bool deferMetadataFlush = false,
                                         bool deferUiRefresh = false);
    AppendEventResult appendEventDetailed(const char* type, JsonObjectConst payload,
                                         const char* sessionId = nullptr,
                                         bool deferMetadataFlush = false,
                                         bool deferUiRefresh = false);
    AppendEventResult appendQueuedRecord(const RAMSpool::EventSlot& slot,
                                         QueuedAppendTiming* timing = nullptr);

    void     beginWorkerAppendBatch(const char* reason = nullptr);
    bool     endWorkerAppendBatch(const char* reason = nullptr,
                                  bool forceFlush = false);
    bool     hasOpenWorkerAppendBatch() const;

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
    bool     getNextUploadEventForSession(const char* sessionId,
                                          uint32_t sinceId,
                                          JsonDocument& out,
                                          bool& found);
    bool     prepareUploadIndexForUpload(uint32_t budgetMs);
    // Close the cached upload segment-read file handle. Safe to call at any
    // time. Useful between bucket fills to avoid carrying a long-lived File
    // object across phases that may invalidate LittleFS internal state.
    void     closeUploadReadFile();
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
    bool     prepareEnrichmentIndexForWindow(size_t maxRecords,
                                             uint32_t budgetMs);
    bool     getNextPendingEnrichmentRecord(PendingEventDescriptor& out,
                                            bool& found);
    void     releaseEnrichmentIndexMemory(const char* reason = nullptr);
    bool     markEnriched(uint32_t eventId,
                          float lat, float lon,
                          float alt, float accuracy,
                          const char* tag);
    // Permanently retire an event from the enrichment pending pool because
    // its captured timestamp is not a usable UTC epoch (e.g., pre-time-sync
    // capture). Writes an ENRICH_DELTA record with ENRICH_FLAG_NO_DATA set
    // and all coordinate fields zero. The event becomes STORAGE_ENRICH_NO_DATA
    // for downstream readers; the live pending-enrichment counter drops by 1.
    bool     markEnrichmentNoData(uint32_t eventId);
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
    bool     isUploadIndexResident() const { return _backlog.uploadIndexResident; }
    bool     compactUploadedEventFiles(const char* sessionId = nullptr);
    int      compactAllUploadedEventFiles();

    // Hot-path accessor: returns the live in-memory counter without ever
    // scanning the spool. This is the only path the runtime should use.
    // The live counter is incremented on appendEvent and decremented on
    // markEventUploaded; passing forceRescan=true is for explicit repair
    // tooling and triggers a full O(spool) scan via recountPendingFromSpool().
    uint32_t getPendingEventCount(bool forceRescan = false);
    uint32_t getAuthoritativePendingEventCount() const { return _pendingEventCount; }
    enum PendingBacklogTrustState : uint8_t {
        BACKLOG_TRUSTED = 0,
        BACKLOG_DEGRADED = 1,
        BACKLOG_UNKNOWN = 2
    };
    PendingBacklogTrustState getBacklogTrustState() const {
        switch (_counterTrustState) {
            case CounterTrust::Trusted:
            case CounterTrust::TrustedSnapshotLagged:
                return BACKLOG_TRUSTED;
            case CounterTrust::Degraded:
                return BACKLOG_DEGRADED;
            case CounterTrust::RepairRequired:
            case CounterTrust::EmergencyOnly:
            default:
                return BACKLOG_UNKNOWN;
        }
    }
    bool isPendingEventCountAuthoritative() const {
        return counterTrustIsAuthoritative(_counterTrustState);
    }

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
    uint32_t getStoredRecordCount() const;
    uint32_t getStoredEventCount() const;
    uint32_t getDisplayRecordCount() const;
    uint32_t getDisplayEventCount() const;
    bool     refreshStoredRecordCountCache(uint32_t nowMs, bool force = false);

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
    bool wipeVaultStorage();
    bool wipeNonVaultStorage();

    StoragePressureMode getPressureMode() const { return _pressureMode; }
    StorageRetentionPolicy getRetentionPolicy() const { return _retentionPolicy; }
    uint32_t getDedupedCount() const { return _dedupStats.suppressed; }
    uint32_t getDroppedCount() const { return _dedupStats.dropped; }
    CounterTrust getCounterTrustState() const {
        return _counterTrustState;
    }
    const char* getCounterTrustReason() const {
        return _counterTrustReason.c_str();
    }
    uint32_t getCounterTrustSinceMs() const {
        return _counterTrustSinceMs;
    }
    bool usingSpoolBackend() const { return true; }
    bool compactSpool();
    void refreshStorageUiState(bool defer = false, bool recalcPressure = true);
    bool flushWorkerMetadataBatch(const char* reason = nullptr,
                                  bool force = false,
                                  bool allowDuringCapture = false);
    void updateStoragePressure(bool allowSideEffects = true);
    bool hasWorkerMetadataFlushWork() const {
        return _workerMetadataDirtyPending;
    }
    bool hasWorkerUiRefreshWork() const {
        return _storageUiRefreshPending && _storageUiRefreshDue();
    }
    bool hasMaintenanceWork() const;
    bool hasStorageMaintenanceWork() const { return hasMaintenanceWork(); }
    uint32_t lastFsAuditCompletedMs() const { return _lastFsAuditCompletedMs; }
    bool shouldRunMaintenanceNow() const;
    bool needsMaintenanceBeforeCapture() const;
    bool isCaptureSafeToResume() const;
    // True while a manual or automatic enrichment session has a prepared
    // window resident in memory. Used by the idle storage maintenance
    // trigger to avoid grabbing a maintenance lease mid-session — which
    // would release the resident window and force a full re-scan.
    bool isEnrichmentWindowResident() const {
        return _backlog.enrichmentIndexResident;
    }
    uint32_t maintenanceFlags() const;
    const char* maintenanceFlagsText() const;
    void requestMaintenance(StorageMaintenanceReason reason,
                            const char* source = nullptr);
    void completeExternalMaintenance(StorageMaintenanceReason reason,
                                     const char* source = nullptr);
    bool runMaintenanceWindow(uint32_t budgetMs,
                              const char* reason = nullptr);
    bool runCaptureMaintenanceSlice(uint32_t budgetMs,
                                    const char* reason = nullptr);
    bool hasSpoolRepairWork() const {
        return _repairRequested ||
               _spoolAuditRepairRequired ||
               _repairJob.active;
    }
    bool requestSpoolRepair(const char* reason = nullptr);
    bool serviceStorageMaintenanceStep(uint32_t budgetMs, uint32_t maxRecords);
    bool repairStep(SpoolRepairMode mode, uint32_t budgetMs, uint32_t maxRecords);

    // RepairProgressSnapshot moved to SpoolRepairTypes.h.
    // StorageUiSnapshot moved to core/StorageUiMirror.h (included above).

    RepairProgressSnapshot getRepairProgress() const {
        RepairProgressSnapshot s;
        s.active          = _repairJob.active;
        s.scannedRecords  = _repairJob.audit.scannedRecords;
        s.scannedSegments = _repairJob.audit.scannedSegments;
        s.totalSegments   = static_cast<uint32_t>(_spoolIndex.segments.size());
        s.startedMs       = _repairJob.startMs;
        s.reason          = _repairJob.reason.c_str();
        return s;
    }

    void beginHotPathDiagnosticsSuppressed();
    void endHotPathDiagnosticsSuppressed();
    bool shouldStoreByPriority(StoragePriority priority) const;
    bool shouldAcceptHandshakeFrame(const char* apMac,
                                    const char* staMac,
                                    const char* ssid,
                                    uint8_t messageNumber,
                                    bool deferUiRefresh = false);

    void spoolAuditToSerial(bool repair);
    void spoolCountToSerial();
    void spoolEnrichToSerial();
    void spoolDiagToSerial();
    void spoolQuarantineListToSerial();
    void spoolQuarantineMetaToSerial();
    bool spoolQuarantineClear();

    // SpoolCorruptionReason / SpoolAuditResult / SpoolBootAuditResult moved
    // to SpoolRepairTypes.h (included above).

private:
    StorageManager() {}
    // SegmentPendingScanResult moved to SpoolRepairTypes.h.
    // SpoolRepairAction enum was unused; removed.

    // SpoolRepairJob moved to SpoolRepair.h. The `_repairJob` member field
    // below now refers to the global type.

    // MaintenanceContinuityRecord moved to managers/MaintenanceContinuity.h
    // (included above). The persistence layer lives there; this class still
    // owns the live record state and the "is record current" comparison.

    bool        _ready = false;
    DeviceConfig _config;
    String      _currentLoraLog;
    String      _currentWifiLog;
    String      _currentProbeLog;
    String      _cachedUsedString = "0KB / 0KB";
    size_t      _cachedTotalBytes = 0;
    size_t      _cachedUsedBytes = 0;
    size_t      _cachedFreeBytes = 0;
    int         _cachedUsedPct = 0;
    uint32_t    _lastFsStatsRefreshMs = 0;
    uint32_t    _nextEventId = 1;
    uint32_t    _storageMetaGeneration = 1;
    bool        _storedRecordCountCacheValid = false;
    uint32_t    _storedRecordCountExactBase = 0;
    uint32_t    _storedRecordCountIndexBase = 0;
    uint32_t    _storedRecordCountUpdatedMs = 0;
    uint32_t    _lastCounterSaveMs = 0;
    uint16_t    _eventCounterPendingWrites = 0;
    bool        _eventCounterDirty = false;
    uint32_t    _eventCounterGeneration = 0;
    bool        _eventCounterLoaded = false;
    uint32_t    _pendingEventCount = 0;
    bool        _pendingCountDirty = true;
    uint32_t    _eventMetaGeneration = 0;
    bool        _eventMetaLoaded = false;
    uint32_t    _lastMetaSaveMs = 0;
    uint32_t    _lastSpoolIndexSaveMs = 0;
    uint16_t    _spoolIndexPendingWrites = 0;
    bool        _spoolIndexDirty = false;
    bool        _spoolIndexLoadFailed = false;
    uint16_t    _workerMetadataPendingWrites = 0;
    uint32_t    _workerMetadataDirtySinceMs = 0;
    bool        _workerMetadataDirtyPending = false;
    uint32_t    _metadataFlushCount = 0;
    uint32_t    _metadataFlushSlowMs = 0;
    uint32_t    _storageUiRefreshDeferred = 0;
    uint32_t    _storageUiRefreshFlush = 0;
    uint32_t    _storageUiRefreshServiceCount = 0;
    uint32_t    _storageUiRefreshDirtySinceMs = 0;
    bool        _storageUiRefreshPending = false;
    StorageUiSnapshot _storageUiSnapshot = {};
    bool        _storageUiSnapshotValid = false;
    char        _workerBatchFlushReason[32] = {};
    SpoolIndex  _spoolIndex;
    SemaphoreHandle_t _appendMutex = nullptr;
    bool        _workerAppendBatchActive = false;
    fs::File    _workerAppendFile;
    uint32_t    _workerAppendSegmentId = 0;
    uint32_t    _workerAppendWriteOffset = 0;
    uint32_t    _workerAppendRecordsSinceFlush = 0;
    SpoolBin::SegmentHeaderV2 _workerAppendHeader{};
    bool        _workerAppendHeaderOk = false;
    bool        _workerAppendHeaderDirty = false;
    CounterTrust _counterTrustState = CounterTrust::Trusted;
    String      _counterTrustReason;
    uint32_t    _counterTrustSinceMs = 0;

    // Upload-batch deferral: when _uploadBatchActive, watermark updates skip
    // flash writes; _uploadBatchDirty tracks whether endUploadBatch needs to
    // persist the spool index + event meta.
    bool        _uploadBatchActive = false;
    bool        _uploadBatchDirty  = false;
    // _uploadIndexResident + _enrichmentIndexResident moved to _backlog.
    // When summary metadata is observed invalid during a busy radio window,
    // defer rebuilding until a quiet maintenance pass can service it.
    bool        _spoolSummaryRebuildPending = false;
    bool        _spoolAuditRepairRequired = false;
    bool        _repairRequested = false;
    bool        _binaryCheckpointDeferred = false;
    uint32_t    _maintenanceRequestedFlags = STORAGE_MAINT_NONE;
    bool        _maintenanceCaptureGate = false;
    bool        _maintenanceFullRebuildAttempted = false;
    uint32_t    _maintenanceRetryAfterMs = 0;
    uint32_t    _lastFsAuditCompletedMs = 0;  // millis() of last completed FS audit
    MaintenanceContinuityRecord _maintenanceContinuity = {};
    bool        _maintenanceContinuityValid = false;
    mutable char _maintenanceFlagsTextBuf[192] = {};
    bool        _suppressHotPathDiagnostics = false;
    SpoolRepairJob _repairJob;

    StoragePressureMode    _pressureMode = STORAGE_MODE_NORMAL;
    StorageRetentionPolicy _retentionPolicy = STORAGE_POLICY_NORMAL;
    DedupStats             _dedupStats = {};
    DedupFilter            _dedupFilter;
    std::map<uint32_t, String> _binaryLastSessionBySegment;
    std::map<uint32_t, SpoolBin::SpoolSegmentCheckpointV1> _binaryCheckpointBySegment;

    // All upload + enrichment paged index state, plus the cached
    // upload-read file handle, moved into BacklogIndex.h. Methods that
    // operate on it still live on StorageManager; they reach via _backlog.
    BacklogIndex _backlog;

    // DEDUP_WINDOW_MS / DEDUP_WINDOW_MAX / HANDSHAKE_WINDOW_MS and the
    // DEDUP_PROFILE selector come from config.h.
    static constexpr uint32_t STORAGE_UI_REFRESH_COALESCE_MS = 250UL;
    // Pressure thresholds moved to StoragePressure::WATCH_PCT / FULL_PCT / OVERRUN_PCT.

    void _logSpoolDiagnostics(const char* reason,
                              const String& sessionId = String(),
                              bool includeRuntimeCounters = true) const;
    uint32_t _activeSessionWatermark(const String& sessionId) const;
    bool   _ensureDir(String path);
    bool   _removeTree(const String& path);
    bool   _removePathWithRetry(const String& path);
    bool   _rmdirWithRetry(const String& path);
    bool   _resetTagMatches(const char* path, const char* expectedTag);
    bool   _writeResetTag(const char* path, const char* tag);
    bool   _applyOneShotVaultReset();
    bool   _applyOneShotNonVaultReset();
    bool   _trimJsonLinesFile(const char* path, size_t keepLastLines);
    String _today();
    String _gpsJson();
    bool   _appendToLog(String path, String key, JsonObject& entry);
    bool   _appendJsonLine(const String& path, JsonDocument& doc);
    void   _bumpStorageMetaGeneration();
    uint32_t _lastUploadedEventIdForSession(const String& sessionId);
    uint32_t _pendingEventCountForSession(const String& sessionId);
    uint32_t _pendingEventCountForSessionFromSpool(const String& sessionId) const;
    bool   _loadEventCounter();
    bool   _persistEventCounter(bool force = false, const char* reason = nullptr);
    bool   _loadEventMeta();
    bool   _persistEventMeta(bool force = false, const char* reason = nullptr);
    bool   _atomicWriteFile(const String& path,
                            std::function<bool(fs::File&)> writer,
                            bool keepBackup);
    bool   _reconcileStorageMetadata();
    bool   _activeSegmentConfirmsNextEventId(uint32_t nextEventId) const;
    bool   _activeSegmentNearRotateThreshold() const;
    bool   _shouldRotateSegmentAfterAppend(const SpoolSegmentInfo& seg) const;
    bool   _preRotateActiveSegmentIfNearFull(const char* reason);
    uint32_t _scanNextEventIdFromSpool() const;
    AppendEventResult _appendEventDetailedInternal(
        const char* type,
        JsonObjectConst payload,
        const char* sessionIdOverride,
        const RAMSpool::CaptureClassification* classification,
        bool deferMetadataFlush,
        bool deferUiRefresh,
        bool deferPressureRefresh,
        QueuedAppendTiming* timing = nullptr);
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
    bool _loadUploadIndexSidecarSegment(const SpoolSegmentInfo& seg);
    bool _validateUploadIndexRecord(const UploadIndexRecordV1& rec,
                                    uint32_t expectedSegmentId) const;
    bool _addUploadPtrToMemory(const UploadIndexRecordV1& rec);
    bool _applyExactUploadedMarks(const String& sessionId,
                                  const std::vector<UploadIndexRecordV1>& selected,
                                  uint32_t before,
                                  uint32_t upToId);
    void _releaseUploadIndexMemory(const char* reason);
    bool _ensureUploadSegmentFileOpen(uint32_t segmentId, uint8_t format,
                                      const String& path);
    void _closeUploadSegmentFile(const char* reason);
    // force=true guarantees an immediate write. force=false is the hot-path
    // mode — it throttles to ~5s and defers while an upload batch is active.
    bool _persistSpoolIndex(bool force = false, const char* reason = nullptr);
    bool _openNewSpoolSegment();
    bool _ensureWorkerAppendFileOpen(SpoolSegmentInfo& seg);
    bool _flushWorkerAppendFile(const char* reason, bool closeFile);
    void _closeWorkerAppendFile(const char* reason);
    SpoolSegmentInfo* _findSegmentInfo(uint32_t segmentId);
    const SpoolSegmentInfo* _findSegmentInfo(uint32_t segmentId) const;
    void _rememberSession(const String& sessionId);
    uint32_t _uploadedWatermarkForSession(const String& sessionId) const;
    void _setUploadedWatermarkForSession(const String& sessionId, uint32_t eventId);
    void _restoreUploadedWatermarkForSession(const String& sessionId, uint32_t eventId);
    bool _appendSpoolRecord(JsonDocument& doc,
                            uint32_t* outEventId = nullptr,
                            bool deferIndexPersist = false,
                            QueuedAppendTiming* timing = nullptr);
    bool _appendSpoolEnrichmentDelta(const String& sessionId,
                                     uint32_t eventId,
                                     float lat, float lon,
                                     float alt, float accuracy,
                                     const char* tag,
                                     bool noData = false);
    bool _maybeWriteBinarySegmentCheckpoint(SpoolSegmentInfo& seg,
                                            const char* reason,
                                            bool force = false,
                                            bool* outWrote = nullptr);
    bool _serviceDeferredBinaryCheckpoint(const char* reason,
                                          bool allowDuringCapture = false);
    bool _loadSpoolEnrichmentsForSession(const String& sessionId,
                                         std::vector<SpoolEnrichmentDelta>& enrichments) const;
    // Build a sorted list of enriched event IDs across the spool. Single
    // contiguous allocation (4 bytes per entry) — replaces an earlier
    // std::map<uint32_t,bool> which fragmented internal heap at 2k+ scale.
    // Callers query via std::binary_search.
    bool _loadSpoolEnrichmentIds(const String& sessionId,
                                 bool filterBySession,
                                 std::vector<uint32_t>& enrichedIds) const;
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
    bool _adjustPendingUploadForEvent(uint32_t eventId,
                                      uint8_t laneHint,
                                      int32_t delta,
                                      const char* reason);
    bool _decrementPendingUploadForEvent(uint32_t eventId,
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
    bool _getNextUploadEventForSessionFromIndex(const String& sessionId,
                                                uint32_t sinceId,
                                                JsonDocument& out,
                                                bool& found);
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
    StoragePriority _priorityForEventType(const char* type,
                                          JsonObjectConst payload) const;
    StorageLane _laneForEventType(const char* type,
                                  JsonObjectConst payload) const;
    StorageLane _eventRecordLane(JsonObjectConst doc) const;
    StoragePriority _eventRecordPriority(JsonObjectConst doc) const;
    uint16_t _eventRecordValueScore(JsonObjectConst doc) const;
    bool _eventRecordWantsEnrichment(JsonObjectConst doc) const;
    bool _eventRecordPendingEnrichment(JsonObjectConst doc) const;
    bool _findEventSession(uint32_t eventId, String& outSessionId) const;
    const char* _laneText(StorageLane lane) const;
    bool _eventRecordIsMission(JsonObjectConst doc) const;

    // Dedup classifiers + window state moved to DedupFilter (member
    // `_dedupFilter`). _shouldSuppressDuplicate is now a thin wrapper that
    // delegates to the filter and applies any counter side effect.
    void _applyDedupCounterEffect(const DedupCounterEffect& effect,
                                  bool deferUiRefresh);

    bool _shouldSuppressDuplicate(const char* type,
                                  JsonObjectConst payload,
                                  StoragePriority priority,
                                  bool deferUiRefresh = false);
    const char* _policyText() const;
    const char* _counterTrustText() const;
    void _setCounterTrustState(CounterTrust state,
                               const char* reason = nullptr);
    void _applyCounterDelta(const char* reason,
                            int32_t pendingUploadDelta,
                            int32_t pendingEnrichDelta,
                            int32_t droppedDelta,
                            int32_t suppressedDelta,
                            StorageLane lane,
                            StoragePriority priority);
    void _applyPendingEventCountDelta(int32_t delta,
                                      const char* reason,
                                      bool markMetadataDirty,
                                      bool markUiRefresh);
    void _publishStorageEventIfNeeded(StoragePressureMode oldMode,
                                      StoragePressureMode newMode);
    void _maybeCompactForPressure(StoragePressureMode oldMode,
                                  StoragePressureMode newMode);
    void _refreshFsStats(bool force = false);
    void _queueStorageUiRefresh(bool defer);
    bool _storageUiRefreshDue() const;
    StorageUiSnapshot _buildStorageUiSnapshot(size_t freeBytes,
                                              int usedPct) const;
    bool _storageUiSnapshotChanged(const StorageUiSnapshot& next) const;
    void _setStoredRecordCountCache(uint32_t exactTotal, uint32_t nowMs);
    bool _appendSegmentRecord(SpoolSegmentInfo& seg,
                              JsonDocument& doc,
                              uint32_t* outEventId = nullptr,
                              SpoolBin::AppendRecordLocation* outLoc = nullptr,
                              QueuedAppendTiming* timing = nullptr);
    bool _scanSegmentRecords(uint32_t segmentId,
                         std::function<bool(const DecodedSpoolRecord&)> cb) const;
    bool _scanJsonlSegmentRecords(uint32_t segmentId,
                              std::function<bool(const DecodedSpoolRecord&)> cb) const;
    bool _scanBinarySegmentRecords(uint32_t segmentId,
                               std::function<bool(const DecodedSpoolRecord&)> cb) const;

    // Hot-scan variant — decodes only record headers (id, ts, session, type,
    // flags, payloadFamily) and skips the payload body entirely. Allocates no
    // JsonDocument per record. Use for paths that need to classify records
    // without payload-specific fields: pending-enrichment scan, enrich-id
    // collection, etc. Returns false on decode failure or I/O error.
    bool _scanSegmentRecordHeaders(uint32_t segmentId,
                              std::function<bool(const DecodedSpoolRecordHeader&)> cb) const;
    bool _scanBinarySegmentRecordHeaders(uint32_t segmentId,
                              std::function<bool(const DecodedSpoolRecordHeader&)> cb) const;
    bool _scanJsonlSegmentRecordHeaders(uint32_t segmentId,
                              std::function<bool(const DecodedSpoolRecordHeader&)> cb) const;
    const char* _segmentFormatText(uint8_t format) const;

    bool _auditAndRepairSpool(const char* reason, bool repair, SpoolAuditResult* out);
    bool _auditSpoolBoot(SpoolBootAuditResult& audit) const;
    bool _auditSpoolBinaryHeader(const SpoolSegmentInfo& seg,
                                 SpoolBin::SegmentHeaderV2& hdr) const;
    bool _auditSpoolBinaryCheckpointTail(const SpoolSegmentInfo& seg,
                                         const SpoolBin::SegmentHeaderV2& hdr) const;
    SpoolRepairMode _selectRepairMode() const;
    // _repairBudgetsForMode moved to SpoolRepairTypes.h as inline free function.
    bool _shouldDeferWorkerMetadataFlush() const;
    void _startRepairJob(const char* reason);
    void _resetRepairJob();
    bool _beginRepairSegment();
    bool _repairJsonlSlice(uint32_t startMs,
                           uint32_t budgetMs,
                           uint32_t maxRecords,
                           uint32_t& recordsScanned);
    bool _repairBinaryMetaSlice(uint32_t startMs,
                                uint32_t budgetMs,
                                uint32_t maxRecords,
                                uint32_t& recordsScanned);
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
    uint32_t _derivedMaintenanceFlags() const;
    SpoolSegmentLifecycle _deriveSegmentLifecycle(const SpoolSegmentInfo& seg) const;
    void _refreshSegmentLifecycle(SpoolSegmentInfo& seg);
    const char* _segmentLifecycleText(uint8_t lifecycle) const;
    void _clearMaintenanceFlags(uint32_t flags);
    const char* _maintenanceReasonText(StorageMaintenanceReason reason) const;
    const char* _maintenanceFlagsText(uint32_t flags) const;
    bool   _loadMaintenanceContinuityLog();
    bool   _writeMaintenanceContinuityLog(uint32_t completedFlags,
                                          uint32_t remainingFlags,
                                          bool clean,
                                          const char* reason);
    bool   _maintenanceContinuityCurrent() const;
    bool   _maintenanceOwnsFilesystem(const char* reason) const;
    // No I/O, no scan. Invariant failures only degrade counter trust and queue
    // explicit maintenance; they never run a spool scan inline.
    bool _checkSpoolInvariants(const char* reason, bool repairIfBad);
};

#define STORAGE StorageManager::getInstance()

