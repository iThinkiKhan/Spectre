


#include "StorageManager.h"
#include <esp_system.h>
#include "../config.h"
#include "../core/DebugLog.h"
#include "../core/CrashBreadcrumb.h"
#include "../core/MissionRuntime.h"
#include "../core/RuntimeContracts.h"
#include "RadioArbiter.h"
#include "SettingsManager.h"
#include "RAMSpool.h"
#include "SpoolBinaryCodec.h"
#include "FsAudit.h"
#include "KnownLocationsStore.h"
#include "EntityManager.h"
#include "BadUsbVault.h"
#include "StorageFsUtil.h"
#include "SpoolPaths.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <functional>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <new>

// Upload runs with the WiFi radio active and has shown sensitivity to even
// serial/ring-buffer formatting in the storage fetch path. Keep these traces
// compile-time dark unless chasing a very specific local fault.
#define DLOG_UPLOAD_TRACE(tag, fmt, ...) \
    do { if (false) DLOG_DEBUG(tag, fmt, ##__VA_ARGS__); } while (0)

namespace {

static constexpr uint8_t ENRICH_FLAG_TAG = 0x01;
static constexpr uint8_t ENRICH_FLAG_GPS_TS = 0x02;
// Set on enrichment delta records that record the absence of GPS data
// rather than a real fix. Used to retire records whose captured timestamp
// is not a trusted UTC epoch (so the phone could never look up history).
// Coordinate fields on such records are all zero.
static constexpr uint8_t ENRICH_FLAG_NO_DATA = 0x04;
// v2 only. Set when this record's numeric fields are absolute rather than
// deltas, and the reader must reset its running context. The writer sets it
// whenever it has no in-memory context for the segment -- which is exactly
// what happens on the first enrichment after a reboot, mid-segment. Without a
// self-describing marker the writer would emit absolutes while a reader
// scanning from the segment start still expected deltas, and every following
// record would decode to garbage.
static constexpr uint8_t ENRICH_FLAG_ABSOLUTE = 0x08;
static constexpr uint32_t MIN_ENRICH_GPS_EPOCH = 1609459200UL;

bool _isReservedEventKey(const char* key) {
    return strcmp(key, "id") == 0 ||
           strcmp(key, "ts") == 0 ||
           strcmp(key, "type") == 0 ||
           strcmp(key, F_ENRICH_STATE) == 0 ||
           strcmp(key, F_GPS_TS) == 0 ||
           strcmp(key, "status") == 0;
}

const SpoolEnrichmentDelta* _findSpoolEnrichment(
    const std::vector<SpoolEnrichmentDelta>& enrichments, uint32_t id) {
    for (const auto& enrichment : enrichments) {
        if (enrichment.id == id) return &enrichment;
    }
    return nullptr;
}

static RAMSpool::CaptureClassification _captureClassification(JsonObjectConst doc) {
    return RAMSpool::classify(doc["type"] | "", doc);
}

static void _normalizeCapturedEvent(
    JsonObject doc,
    const RAMSpool::CaptureClassification* clsOverride = nullptr) {
    const RAMSpool::CaptureClassification cls =
        clsOverride ? *clsOverride : _captureClassification(doc);
    doc["prio"] = static_cast<uint8_t>(cls.priority);
    doc["lane"] = static_cast<uint8_t>(cls.lane);
    doc["lane_name"] =
        (cls.lane == RAMSpool::LANE_MISSION) ? "MISSION" : "NOISE";

    JsonVariantConst enrichMarker = doc[F_ENRICH_STATE];
    if (!cls.enrichEligible) {
        doc[F_ENRICH_STATE] =
            static_cast<uint8_t>(STORAGE_ENRICH_NOT_ELIGIBLE);
    } else if (enrichMarker.isNull()) {
        doc[F_ENRICH_STATE] =
            static_cast<uint8_t>(STORAGE_ENRICH_PENDING);
    }
}

static bool _isSyncAppendHotPathViolationType(const char* type) {
    return type &&
           (strcmp(type, "probe") == 0 ||
            strcmp(type, "device") == 0);
}

static const char* _jsonRootTypeText(const JsonDocument& doc) {
    if (doc.is<JsonObject>()) return "object";
    if (doc.is<JsonArray>()) return "array";
    return "other";
}

static void _logCaptureWriteAllowed(const char* path,
                                    const char* reason,
                                    uint32_t elapsedMs,
                                    bool ok) {
    if (RADIO_ARB.currentOwner() != RADIO_WIFI_CAPTURE) {
        return;
    }

    DLOG_INFO("STORAGE",
              "capture write allowed owner=%s path=%s reason=%s ms=%lu ok=%d",
              RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
              path ? path : "-",
              (reason && reason[0]) ? reason : "-",
              static_cast<unsigned long>(elapsedMs),
              ok ? 1 : 0);
}

class ScopedSemaphoreLock {
public:
    explicit ScopedSemaphoreLock(SemaphoreHandle_t sem) : _sem(sem) {
        if (_sem) {
            xSemaphoreTake(_sem, portMAX_DELAY);
            _locked = true;
        }
    }

    ~ScopedSemaphoreLock() {
        if (_locked) {
            xSemaphoreGive(_sem);
        }
    }

    ScopedSemaphoreLock(const ScopedSemaphoreLock&) = delete;
    ScopedSemaphoreLock& operator=(const ScopedSemaphoreLock&) = delete;

private:
    SemaphoreHandle_t _sem = nullptr;
    bool _locked = false;
};

static bool _spoolSegmentInfoEquals(const SpoolSegmentInfo& a,
                                    const SpoolSegmentInfo& b) {
    return a.segmentId == b.segmentId &&
           a.firstEventId == b.firstEventId &&
           a.lastEventId == b.lastEventId &&
           a.summaryVersion == b.summaryVersion &&
           a.summaryValid == b.summaryValid &&
           a.trustState == b.trustState &&
           a.lifecycle == b.lifecycle &&
           a.recordCount == b.recordCount &&
           a.eventCount == b.eventCount &&
           a.enrichDeltaCount == b.enrichDeltaCount &&
           a.missionCount == b.missionCount &&
           a.noiseCount == b.noiseCount &&
           a.pendingUploadMissionCount == b.pendingUploadMissionCount &&
           a.pendingUploadNoiseCount == b.pendingUploadNoiseCount &&
           a.pendingEnrichmentCount == b.pendingEnrichmentCount &&
           a.p0Count == b.p0Count &&
           a.p1Count == b.p1Count &&
           a.p2Count == b.p2Count &&
           a.p3Count == b.p3Count &&
           a.minTimestampMs == b.minTimestampMs &&
           a.maxTimestampMs == b.maxTimestampMs &&
           a.approxBytes == b.approxBytes &&
           a.format == b.format;
}

static const char* _spoolTrustText(uint8_t trustState) {
    switch (trustState) {
        case SPOOL_SEGMENT_TRUSTED:   return "trusted";
        case SPOOL_SEGMENT_UNTRUSTED: return "untrusted";
        case SPOOL_SEGMENT_INVALID:    return "invalid";
        default:                      return "unknown";
    }
}

enum BinaryEventTypeCode : uint8_t {
    BIN_EVT_CUSTOM = 0,
    BIN_EVT_PROBE = 1,
    BIN_EVT_DEVICE = 2,
    BIN_EVT_DRONE = 3,
    BIN_EVT_PMKID = 4,
    BIN_EVT_EVENT = 5,
    // "network" used to fall through to BIN_EVT_CUSTOM, which spelled the type
    // name inline AND forced the whole record into a keyed field map. Giving it
    // a code lets AP observations share the structured probe/device layout.
    BIN_EVT_NETWORK = 6
};

enum BinaryPayloadFamilyCode : uint8_t {
    BIN_PAYLOAD_JSON_FALLBACK = 0,   // read-only compatibility
    BIN_PAYLOAD_PROBE_DEVICE = 1,    // v1 — read-only, still on disk
    BIN_PAYLOAD_PMKID = 2,
    BIN_PAYLOAD_HANDSHAKE = 3,
    BIN_PAYLOAD_DRONE = 4,
    BIN_PAYLOAD_FIELD_MAP = 5,
    // v2 probe/device/network: everything the localization sidecars added is
    // positional behind flag bits instead of keyed in the extension map.
    BIN_PAYLOAD_PROBE_DEVICE_V2 = 6
};

// v2 payload flag bytes. Byte 1 mirrors the v1 flags so the two layouts stay
// readable side by side; byte 2 is the sidecar block and is only present when
// BIN_PDV2_HAS_FLAGS2 is set.
enum BinaryProbeV2Flags1 : uint8_t {
    BIN_PDV2_SSID       = 0x01,
    BIN_PDV2_RSSI       = 0x02,
    BIN_PDV2_CHANNEL    = 0x04,
    BIN_PDV2_IE_FP      = 0x08,
    BIN_PDV2_PROBE_HASH = 0x10,
    BIN_PDV2_RANDOM_MAC = 0x20,
    BIN_PDV2_BROADCAST  = 0x40,
    BIN_PDV2_HAS_FLAGS2 = 0x80
};

enum BinaryProbeV2Flags2 : uint8_t {
    BIN_PDV2_SECURITY     = 0x01,
    BIN_PDV2_HIDDEN       = 0x02,
    BIN_PDV2_WPS          = 0x04,
    BIN_PDV2_LOCALIZATION = 0x08,
    BIN_PDV2_TRACK        = 0x10,
    // Receiver RF context: noise floor and the gain of the antenna fitted.
    // Two bytes that make an RSSI interpretable rather than merely recorded.
    BIN_PDV2_RFCTX        = 0x20,
    // Advertised transmit power plus the source it came from.
    BIN_PDV2_TXPWR        = 0x40
};

// track_id is always one of three literal prefixes glued to a value the record
// already stores structurally, so store the derivation, not the string.
enum BinaryTrackMode : uint8_t {
    BIN_TRACK_NONE    = 0,
    BIN_TRACK_AP_MAC  = 1,   // "AP:"  + mac/bssid
    BIN_TRACK_MAC_MAC = 2,   // "MAC:" + mac
    BIN_TRACK_IE_FP   = 3,   // "IE:"  + ie_fingerprint
    BIN_TRACK_LITERAL = 4    // anything else — string follows
};

enum BinarySecurityCode : uint8_t {
    BIN_SEC_LITERAL = 0,
    BIN_SEC_OPEN    = 1,
    BIN_SEC_WEP     = 2,
    BIN_SEC_WPA     = 3,
    BIN_SEC_WPA2    = 4,
    BIN_SEC_WPA3    = 5,
    BIN_SEC_SAE     = 6,
    BIN_SEC_OWE     = 7
};

enum BinarySampleReasonCode : uint8_t {
    BIN_REASON_LITERAL      = 0,
    BIN_REASON_NEW          = 1,
    BIN_REASON_IDENTITY     = 2,
    BIN_REASON_INTERVAL     = 3,
    BIN_REASON_SIGNAL_DELTA = 4
};

enum BinarySessionMode : uint8_t {
    BIN_SESSION_INLINE = 0,
    BIN_SESSION_SAME_AS_PREV = 1,
    // session_tag is a per-session property that was being written on every
    // record. Carry it with the session string instead, so it costs bytes only
    // when the session (or the tag) actually changes.
    BIN_SESSION_INLINE_TAGGED = 2
};

enum BinaryMacMode : uint8_t {
    BIN_MAC_NONE = 0,
    BIN_MAC_FULL = 1,
    BIN_MAC_STRING = 2,
    BIN_MAC_OUI_SUFFIX = 3,
    BIN_MAC_PREV_OUI_SUFFIX = 4
};

struct BinaryUnsupportedAuditEntry {
    String type;
    String subtype;
    uint32_t count = 0;
};

static uint32_t g_binaryStructuredWrites = 0;
static uint32_t g_binaryUnsupportedWrites = 0;
static std::vector<BinaryUnsupportedAuditEntry> g_binaryUnsupportedAudit;

static constexpr uint32_t kUploadIndexRebuildMinBudgetMs = 15000UL;
static constexpr uint32_t kUploadIndexWindowMaxRecords = 5000UL;

static void _resetBinaryUnsupportedAudit() {
    g_binaryStructuredWrites = 0;
    g_binaryUnsupportedWrites = 0;
    g_binaryUnsupportedAudit.clear();
}

static void _recordBinaryStructuredWrite() {
    g_binaryStructuredWrites++;
}

static void _recordBinaryUnsupportedUsage(const String& type,
                                          const String& subtype) {
    g_binaryUnsupportedWrites++;

    for (auto& entry : g_binaryUnsupportedAudit) {
        if (entry.type == type && entry.subtype == subtype) {
            entry.count++;
            return;
        }
    }

    BinaryUnsupportedAuditEntry entry;
    entry.type = type;
    entry.subtype = subtype;
    entry.count = 1;
    g_binaryUnsupportedAudit.push_back(entry);
}

static void _logBinaryUnsupportedAudit(const char* reason) {
    if (g_binaryUnsupportedWrites == 0 && g_binaryUnsupportedAudit.empty()) {
        return;
    }

    const char* safeReason = (reason && reason[0]) ? reason : "?";

    DLOG_INFO("STORAGE",
              "Unsupported audit[%s] structured=%lu unsupported=%lu unique=%u",
              safeReason,
              static_cast<unsigned long>(g_binaryStructuredWrites),
              static_cast<unsigned long>(g_binaryUnsupportedWrites),
              static_cast<unsigned>(g_binaryUnsupportedAudit.size()));

    for (const auto& entry : g_binaryUnsupportedAudit) {
        const char* safeType = entry.type.length() ? entry.type.c_str() : "-";
        const char* safeSubtype = entry.subtype.length() ? entry.subtype.c_str() : "-";

        DLOG_INFO("STORAGE",
                  "Unsupported audit[%s] type=%s subtype=%s count=%lu",
                  safeReason,
                  safeType,
                  safeSubtype,
                  static_cast<unsigned long>(entry.count));
    }
}

enum BinaryEventFlags : uint8_t {
    BIN_EVENT_HAS_PRIO = 0x01,
    BIN_EVENT_HAS_LANE = 0x02,
    BIN_EVENT_HAS_PAYLOAD = 0x04,
    BIN_EVENT_HAS_FIELDS = 0x08
};

enum BinaryFieldType : uint8_t {
    BIN_FIELD_STRING = 1,
    BIN_FIELD_INT = 2,
    BIN_FIELD_UINT = 3,
    BIN_FIELD_FLOAT = 4,
    BIN_FIELD_BOOL = 5
};

static void _appendUVarintToBytes(std::vector<uint8_t>& out, uint32_t value);
static bool _readUVarintFromBytes(const uint8_t*& p, const uint8_t* end, uint32_t& out);
static void _appendZigZag32ToBytes(std::vector<uint8_t>& out, int32_t value);
static bool _readZigZag32FromBytes(const uint8_t*& p, const uint8_t* end, int32_t& out);
static void _appendStringToBytes(std::vector<uint8_t>& out, const String& s);
static bool _readStringFromBytes(const uint8_t*& p, const uint8_t* end, String& out);
static bool _readBinarySessionField(const uint8_t*& p, const uint8_t* end,
                                   uint8_t sessionMode, String& lastSession,
                                   String& lastSessionTag, String& sessionId,
                                   String& sessionTag);

// Fields every decoder reconstructs for every event record regardless of
// payload family. Storing them costs ~93 B/record and buys nothing: the read
// path overwrites prio/lane/lane_name from the flag bytes and recomputes
// session/ts_iso from the record header, so the stored copies are written,
// read back, and discarded. sensor/source are compile-time device constants
// and session_id is a third copy of the session already in the record header.
// Auto-format on mount failure is deliberately disabled: a transient mount
// error must never be allowed to wipe field data. But a genuinely blank
// partition -- fresh silicon, or a repartition after erase-flash -- has no
// filesystem to mount and no data to protect, and without this the device comes
// up with storage permanently unavailable (every capture write returns status=5)
// and no recovery path short of a firmware change.
//
// Erased NOR flash reads back as all-0xFF, so an all-0xFF prefix means "never
// written". Anything else is a real filesystem -- possibly damaged -- and is
// left alone for the operator to decide about.
static bool _storagePartitionLooksBlank() {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (!part) return false;

    static constexpr size_t kProbeBytes = 8192;
    uint8_t buf[256];
    const size_t probe = (part->size < kProbeBytes) ? part->size : kProbeBytes;
    for (size_t off = 0; off + sizeof(buf) <= probe; off += sizeof(buf)) {
        if (esp_partition_read(part, off, buf, sizeof(buf)) != ESP_OK) {
            return false;
        }
        for (size_t i = 0; i < sizeof(buf); i++) {
            if (buf[i] != 0xFFU) return false;
        }
    }
    return true;
}

// Builds a delta-coded enrichment body against `ctx` and advances it.
// Mirrors _decodeBinaryEnrichDeltaBody exactly; the two must stay in step.
//
// Typical consecutive enrichments come from one walk: record ids advance by
// one, positions by a few metres, GPS timestamps by seconds. Encoding those as
// deltas turns four multi-byte absolutes into single bytes.
static void _buildBinaryEnrichDeltaBodyV2(std::vector<uint8_t>& body,
                                          BinaryEnrichContext& ctx,
                                          uint32_t recordId,
                                          uint32_t tsDelta,
                                          bool sameSession,
                                          const String& sessionStr,
                                          const String& sessionTag,
                                          uint8_t enrichFlags,
                                          uint32_t targetEventId,
                                          int32_t latE7,
                                          int32_t lonE7,
                                          int32_t altCm,
                                          uint32_t accDm,
                                          const String& tagStr,
                                          uint32_t gpsEpochUtc) {
    // No baseline (fresh segment, or first write after a reboot mid-segment):
    // emit absolutes and say so on the wire.
    const bool absolute = !ctx.have;
    if (absolute) enrichFlags |= ENRICH_FLAG_ABSOLUTE;
    const BinaryEnrichContext base = absolute ? BinaryEnrichContext{} : ctx;

    // Record ids and timestamps are monotonic within a segment, so plain
    // unsigned deltas suffice.
    _appendUVarintToBytes(body, recordId >= base.recordId
                                    ? recordId - base.recordId : 0U);
    _appendUVarintToBytes(body, tsDelta >= base.tsDelta
                                    ? tsDelta - base.tsDelta : 0U);

    body.push_back(sameSession ? BIN_SESSION_SAME_AS_PREV
                               : (sessionTag.length() ? BIN_SESSION_INLINE_TAGGED
                                                      : BIN_SESSION_INLINE));
    if (!sameSession) {
        _appendStringToBytes(body, sessionStr);
        if (sessionTag.length()) _appendStringToBytes(body, sessionTag);
    }

    body.push_back(enrichFlags);

    _appendZigZag32ToBytes(body, static_cast<int32_t>(targetEventId) -
                                 static_cast<int32_t>(base.eventId));

    // A no-data record's coordinates are sentinel zeros: omit them entirely
    // rather than delta-code a meaningless position.
    if (!(enrichFlags & ENRICH_FLAG_NO_DATA)) {
        _appendZigZag32ToBytes(body, latE7 - base.latE7);
        _appendZigZag32ToBytes(body, lonE7 - base.lonE7);
        _appendZigZag32ToBytes(body, altCm - base.altCm);
        _appendUVarintToBytes(body, accDm);
    }

    if (enrichFlags & ENRICH_FLAG_TAG) _appendStringToBytes(body, tagStr);
    if (enrichFlags & ENRICH_FLAG_GPS_TS) {
        _appendZigZag32ToBytes(body, static_cast<int32_t>(gpsEpochUtc) -
                                     static_cast<int32_t>(base.gpsEpochUtc));
    }

    ctx.have = true;
    ctx.recordId = recordId;
    ctx.tsDelta = tsDelta;
    ctx.eventId = targetEventId;
    if (!(enrichFlags & ENRICH_FLAG_NO_DATA)) {
        ctx.latE7 = latE7;
        ctx.lonE7 = lonE7;
        ctx.altCm = altCm;
    }
    if (enrichFlags & ENRICH_FLAG_GPS_TS) ctx.gpsEpochUtc = gpsEpochUtc;
}

// One decoded enrichment record, shared by all three read paths.
struct DecodedEnrichDelta {
    uint32_t recordId      = 0;
    uint32_t tsDelta       = 0;
    String   sessionId;
    String   sessionTag;
    uint8_t  flags         = 0;
    uint32_t targetEventId = 0;
    int32_t  latE7         = 0;
    int32_t  lonE7         = 0;
    int32_t  altCm         = 0;
    uint32_t accDm         = 0;
    uint32_t gpsEpochUtc   = 0;
    String   tag;
};

// Decodes both REC_ENRICH_DELTA (v1, absolute) and REC_ENRICH_DELTA_V2
// (delta-coded) and advances `ctx`. The three call sites previously carried
// near-identical copies of this parse; unifying them is what keeps a format
// change from having to be made — and got wrong — three times.
static bool _decodeBinaryEnrichDeltaBody(const uint8_t*& p,
                                         const uint8_t* end,
                                         uint8_t recordPrefixType,
                                         BinaryEnrichContext& ctx,
                                         String& lastSession,
                                         String& lastSessionTag,
                                         DecodedEnrichDelta& out) {
    const bool v2 = (recordPrefixType == SpoolBin::REC_ENRICH_DELTA_V2);

    if (!v2) {
        // ---- v1: every field absolute ----
        if (!_readUVarintFromBytes(p, end, out.recordId) ||
            !_readUVarintFromBytes(p, end, out.tsDelta)) {
            return false;
        }
        if (p >= end) return false;
        const uint8_t sessionMode = *p++;
        if (!_readBinarySessionField(p, end, sessionMode, lastSession,
                                     lastSessionTag, out.sessionId,
                                     out.sessionTag)) {
            return false;
        }
        if (p >= end) return false;
        out.flags = *p++;
        if (!_readUVarintFromBytes(p, end, out.targetEventId) ||
            !_readZigZag32FromBytes(p, end, out.latE7) ||
            !_readZigZag32FromBytes(p, end, out.lonE7) ||
            !_readZigZag32FromBytes(p, end, out.altCm) ||
            !_readUVarintFromBytes(p, end, out.accDm)) {
            return false;
        }
        if ((out.flags & ENRICH_FLAG_TAG) &&
            !_readStringFromBytes(p, end, out.tag)) {
            return false;
        }
        if ((out.flags & ENRICH_FLAG_GPS_TS) &&
            !_readUVarintFromBytes(p, end, out.gpsEpochUtc)) {
            return false;
        }
        // v1 records still seed the context, so a v1->v2 transition inside one
        // segment (a firmware upgrade mid-segment) delta-codes correctly.
        ctx.have = true;
        ctx.recordId = out.recordId;
        ctx.tsDelta = out.tsDelta;
        ctx.eventId = out.targetEventId;
        if (!(out.flags & ENRICH_FLAG_NO_DATA)) {
            ctx.latE7 = out.latE7;
            ctx.lonE7 = out.lonE7;
            ctx.altCm = out.altCm;
        }
        if (out.flags & ENRICH_FLAG_GPS_TS) ctx.gpsEpochUtc = out.gpsEpochUtc;
        return true;
    }

    // ---- v2: deltas against the previous enrichment record ----
    uint32_t recordIdDelta = 0;
    uint32_t tsDeltaDelta = 0;
    if (!_readUVarintFromBytes(p, end, recordIdDelta) ||
        !_readUVarintFromBytes(p, end, tsDeltaDelta)) {
        return false;
    }
    if (p >= end) return false;
    const uint8_t sessionMode = *p++;
    if (!_readBinarySessionField(p, end, sessionMode, lastSession,
                                 lastSessionTag, out.sessionId,
                                 out.sessionTag)) {
        return false;
    }
    if (p >= end) return false;
    out.flags = *p++;

    const bool absolute = (out.flags & ENRICH_FLAG_ABSOLUTE) != 0;
    const BinaryEnrichContext base = absolute ? BinaryEnrichContext{} : ctx;

    out.recordId = base.recordId + recordIdDelta;
    out.tsDelta  = base.tsDelta + tsDeltaDelta;

    int32_t eventIdDelta = 0;
    if (!_readZigZag32FromBytes(p, end, eventIdDelta)) return false;
    out.targetEventId =
        static_cast<uint32_t>(static_cast<int32_t>(base.eventId) + eventIdDelta);

    // A no-data record carries sentinel coordinates, so they are omitted from
    // the wire entirely rather than delta-coded -- that keeps ~13 bytes off the
    // record AND stops a meaningless (0,0) from poisoning the running position.
    if (!(out.flags & ENRICH_FLAG_NO_DATA)) {
        int32_t dLat = 0, dLon = 0, dAlt = 0;
        if (!_readZigZag32FromBytes(p, end, dLat) ||
            !_readZigZag32FromBytes(p, end, dLon) ||
            !_readZigZag32FromBytes(p, end, dAlt) ||
            !_readUVarintFromBytes(p, end, out.accDm)) {
            return false;
        }
        out.latE7 = base.latE7 + dLat;
        out.lonE7 = base.lonE7 + dLon;
        out.altCm = base.altCm + dAlt;
    }

    if ((out.flags & ENRICH_FLAG_TAG) &&
        !_readStringFromBytes(p, end, out.tag)) {
        return false;
    }
    if (out.flags & ENRICH_FLAG_GPS_TS) {
        int32_t dGps = 0;
        if (!_readZigZag32FromBytes(p, end, dGps)) return false;
        out.gpsEpochUtc =
            static_cast<uint32_t>(static_cast<int32_t>(base.gpsEpochUtc) + dGps);
    }

    ctx.have = true;
    ctx.recordId = out.recordId;
    ctx.tsDelta = out.tsDelta;
    ctx.eventId = out.targetEventId;
    if (!(out.flags & ENRICH_FLAG_NO_DATA)) {
        ctx.latE7 = out.latE7;
        ctx.lonE7 = out.lonE7;
        ctx.altCm = out.altCm;
    }
    if (out.flags & ENRICH_FLAG_GPS_TS) ctx.gpsEpochUtc = out.gpsEpochUtc;
    return true;
}

static bool _isDecoderDerivedEventKey(const char* key) {
    return strcmp(key, "prio") == 0 ||
           strcmp(key, "lane") == 0 ||
           strcmp(key, "lane_name") == 0 ||
           strcmp(key, F_TIMESTAMP_ISO) == 0 ||
           strcmp(key, F_SESSION) == 0 ||
           strcmp(key, "session_id") == 0 ||
           strcmp(key, "session_tag") == 0 ||
           strcmp(key, "sensor") == 0;
}

// Localization sidecar fields carried positionally by the v2 probe/device/
// network payload. Keyed, these cost ~120 B/record; positional, ~7 B.
static bool _isLocalizationSidecarKey(const char* key) {
    return strcmp(key, "tx_power") == 0 ||
           strcmp(key, "tx_power_src") == 0 ||
           strcmp(key, "noise_floor") == 0 ||
           strcmp(key, "ant_gain_q2") == 0 ||
           strcmp(key, "track_id") == 0 ||
           strcmp(key, "localization_sample") == 0 ||
           strcmp(key, "sample_seq") == 0 ||
           strcmp(key, "sample_frames") == 0 ||
           strcmp(key, "rssi_min") == 0 ||
           strcmp(key, "rssi_max") == 0 ||
           strcmp(key, "sample_reason") == 0;
}

static bool _isProbeDeviceV2Type(const String& typeStr) {
    return typeStr == "probe" || typeStr == "device" || typeStr == "network";
}

static bool _isStructuredBinaryField(const String& typeStr,
                                     const String& eventSubtype,
                                     const char* key) {
    if (!key || !key[0]) return true;
    if (_isReservedEventKey(key)) return true;
    if (_isDecoderDerivedEventKey(key)) return true;

    if (strcmp(key, "event_type") == 0) {
        return (typeStr == "event" && eventSubtype == "handshake");
    }

    if (_isProbeDeviceV2Type(typeStr) && _isLocalizationSidecarKey(key)) {
        return true;
    }

    if (typeStr == "probe") {
        return strcmp(key, "mac") == 0 ||
               strcmp(key, "probed_ssid") == 0 ||
               strcmp(key, "ssid") == 0 ||
               strcmp(key, "is_broadcast") == 0 ||
               strcmp(key, "rssi") == 0 ||
               strcmp(key, "channel") == 0 ||
               strcmp(key, "ie_fingerprint") == 0;
    }

    if (typeStr == "device") {
        return strcmp(key, "mac") == 0 ||
               strcmp(key, "rssi") == 0 ||
               strcmp(key, "ie_fingerprint") == 0 ||
               strcmp(key, "probe_set_hash") == 0 ||
               strcmp(key, "is_random_mac") == 0;
    }

    if (typeStr == "network" || typeStr == "device") {
        if (strcmp(key, "source") == 0) return true;
    }

    if (typeStr == "network") {
        return strcmp(key, "bssid") == 0 ||
               strcmp(key, "mac") == 0 ||
               strcmp(key, "ssid") == 0 ||
               strcmp(key, "rssi") == 0 ||
               strcmp(key, "channel") == 0 ||
               strcmp(key, "security") == 0 ||
               strcmp(key, "is_hidden") == 0 ||
               strcmp(key, "has_wps") == 0;
    }

    if (typeStr == "pmkid") {
        return strcmp(key, "ap") == 0 ||
               strcmp(key, "sta") == 0 ||
               strcmp(key, "bssid") == 0 ||
               strcmp(key, "client") == 0 ||
               strcmp(key, "client_mac") == 0 ||
               strcmp(key, "ssid") == 0 ||
               strcmp(key, "rssi") == 0 ||
               strcmp(key, "pmkid_hex") == 0 ||
               strcmp(key, "hashcat_line") == 0;
    }

    if (typeStr == "drone") {
        return strcmp(key, "drone_id") == 0 ||
               strcmp(key, "id") == 0 ||
               strcmp(key, "mac") == 0 ||
               strcmp(key, "rssi") == 0 ||
               strcmp(key, "channel") == 0 ||
               strcmp(key, "protocol") == 0 ||
               strcmp(key, "latitude") == 0 ||
               strcmp(key, "longitude") == 0 ||
               strcmp(key, "altitude_m") == 0 ||
               strcmp(key, "speed") == 0 ||
               strcmp(key, "model") == 0;
    }

    if (typeStr == "event" && eventSubtype == "handshake") {
        return strcmp(key, "ap") == 0 ||
               strcmp(key, "sta") == 0 ||
               strcmp(key, "bssid") == 0 ||
               strcmp(key, "client") == 0 ||
               strcmp(key, "ssid") == 0 ||
               strcmp(key, "rssi") == 0 ||
               strcmp(key, "frame_mask") == 0 ||
               strcmp(key, "message") == 0 ||
               strcmp(key, "msg") == 0;
    }

    return false;
}

static bool _appendBinaryFieldMapToBytes(std::vector<uint8_t>& out,
                                         JsonObjectConst doc,
                                         const String& typeStr,
                                         const String& eventSubtype) {
    uint32_t fieldCount = 0;
    for (JsonPairConst kv : doc) {
        if (_isStructuredBinaryField(typeStr, eventSubtype, kv.key().c_str())) {
            continue;
        }
        fieldCount++;
    }

    _appendUVarintToBytes(out, fieldCount);
    if (fieldCount == 0) {
        return true;
    }

    for (JsonPairConst kv : doc) {
        const char* key = kv.key().c_str();
        if (_isStructuredBinaryField(typeStr, eventSubtype, key)) {
            continue;
        }

        _appendStringToBytes(out, String(key));
        JsonVariantConst value = kv.value();

        if (value.is<bool>()) {
            out.push_back(BIN_FIELD_BOOL);
            out.push_back(value.as<bool>() ? 1U : 0U);
            continue;
        }

        if (value.is<float>() || value.is<double>()) {
            out.push_back(BIN_FIELD_FLOAT);
            const float v = value.as<float>();
            const uint8_t* raw = reinterpret_cast<const uint8_t*>(&v);
            for (size_t i = 0; i < sizeof(v); i++) {
                out.push_back(raw[i]);
            }
            continue;
        }

        if (value.is<long long>() || value.is<long>() || value.is<int>()) {
            const long long v = value.as<long long>();
            if (v < 0) {
                out.push_back(BIN_FIELD_INT);
                _appendZigZag32ToBytes(out, static_cast<int32_t>(v));
            } else {
                out.push_back(BIN_FIELD_UINT);
                _appendUVarintToBytes(out, static_cast<uint32_t>(v));
            }
            continue;
        }

        if (value.is<unsigned long long>() || value.is<unsigned long>() || value.is<unsigned int>()) {
            out.push_back(BIN_FIELD_UINT);
            _appendUVarintToBytes(out, static_cast<uint32_t>(value.as<unsigned long long>()));
            continue;
        }

        out.push_back(BIN_FIELD_STRING);
        _appendStringToBytes(out, value.as<String>());
    }

    return true;
}

static bool _readBinaryFieldMapFromBytes(const uint8_t*& p,
                                         const uint8_t* end,
                                         JsonObject root) {
    uint32_t fieldCount = 0;
    if (!_readUVarintFromBytes(p, end, fieldCount)) {
        return false;
    }

    for (uint32_t i = 0; i < fieldCount; i++) {
        String key;
        if (!_readStringFromBytes(p, end, key) || p >= end) {
            return false;
        }

        const uint8_t fieldType = *p++;
        switch (fieldType) {
            case BIN_FIELD_STRING: {
                String value;
                if (!_readStringFromBytes(p, end, value)) return false;
                root[key] = value;
                break;
            }
            case BIN_FIELD_INT: {
                int32_t value = 0;
                if (!_readZigZag32FromBytes(p, end, value)) return false;
                root[key] = value;
                break;
            }
            case BIN_FIELD_UINT: {
                uint32_t value = 0;
                if (!_readUVarintFromBytes(p, end, value)) return false;
                root[key] = value;
                break;
            }
            case BIN_FIELD_FLOAT: {
                if (static_cast<size_t>(end - p) < sizeof(float)) return false;
                float value = 0.0f;
                memcpy(&value, p, sizeof(float));
                p += sizeof(float);
                root[key] = value;
                break;
            }
            case BIN_FIELD_BOOL: {
                if (p >= end) return false;
                root[key] = (*p++ != 0);
                break;
            }
            default:
                return false;
        }
    }

    return true;
}

static uint8_t _binaryEventTypeCodeFromString(const char* type) {
    if (!type || !type[0]) return BIN_EVT_CUSTOM;
    if (strcmp(type, "probe") == 0)  return BIN_EVT_PROBE;
    if (strcmp(type, "device") == 0) return BIN_EVT_DEVICE;
    if (strcmp(type, "drone") == 0)  return BIN_EVT_DRONE;
    if (strcmp(type, "pmkid") == 0)  return BIN_EVT_PMKID;
    if (strcmp(type, "event") == 0)  return BIN_EVT_EVENT;
    if (strcmp(type, "network") == 0) return BIN_EVT_NETWORK;
    return BIN_EVT_CUSTOM;
}

static const char* _binaryEventTypeStringFromCode(uint8_t code) {
    switch (code) {
        case BIN_EVT_PROBE:  return "probe";
        case BIN_EVT_DEVICE: return "device";
        case BIN_EVT_DRONE:  return "drone";
        case BIN_EVT_PMKID:  return "pmkid";
        case BIN_EVT_EVENT:  return "event";
        case BIN_EVT_NETWORK: return "network";
        case BIN_EVT_CUSTOM:
        default:             return "";
    }
}

static uint8_t _binarySecurityCodeFromString(const char* sec) {
    if (!sec || !sec[0]) return BIN_SEC_LITERAL;
    if (strcmp(sec, "OPEN") == 0) return BIN_SEC_OPEN;
    if (strcmp(sec, "WEP")  == 0) return BIN_SEC_WEP;
    if (strcmp(sec, "WPA")  == 0) return BIN_SEC_WPA;
    if (strcmp(sec, "WPA2") == 0) return BIN_SEC_WPA2;
    if (strcmp(sec, "WPA3") == 0) return BIN_SEC_WPA3;
    if (strcmp(sec, "SAE")  == 0) return BIN_SEC_SAE;
    if (strcmp(sec, "OWE")  == 0) return BIN_SEC_OWE;
    return BIN_SEC_LITERAL;
}

static const char* _binarySecurityStringFromCode(uint8_t code) {
    switch (code) {
        case BIN_SEC_OPEN: return "OPEN";
        case BIN_SEC_WEP:  return "WEP";
        case BIN_SEC_WPA:  return "WPA";
        case BIN_SEC_WPA2: return "WPA2";
        case BIN_SEC_WPA3: return "WPA3";
        case BIN_SEC_SAE:  return "SAE";
        case BIN_SEC_OWE:  return "OWE";
        default:           return "";
    }
}

static uint8_t _binarySampleReasonCodeFromString(const char* reason) {
    if (!reason || !reason[0]) return BIN_REASON_LITERAL;
    if (strcmp(reason, "new") == 0)          return BIN_REASON_NEW;
    if (strcmp(reason, "identity") == 0)     return BIN_REASON_IDENTITY;
    if (strcmp(reason, "interval") == 0)     return BIN_REASON_INTERVAL;
    if (strcmp(reason, "signal_delta") == 0) return BIN_REASON_SIGNAL_DELTA;
    return BIN_REASON_LITERAL;
}

static const char* _binarySampleReasonStringFromCode(uint8_t code) {
    switch (code) {
        case BIN_REASON_NEW:          return "new";
        case BIN_REASON_IDENTITY:     return "identity";
        case BIN_REASON_INTERVAL:     return "interval";
        case BIN_REASON_SIGNAL_DELTA: return "signal_delta";
        default:                      return "";
    }
}

// track_id is generated as one of three prefixes over a value already stored
// structurally in the record. Recognise the derivation so the string itself
// never reaches flash; fall back to a literal for anything unexpected.
static uint8_t _binaryTrackModeFor(const String& trackId,
                                   const String& mac,
                                   const String& ieFingerprint) {
    if (!trackId.length()) return BIN_TRACK_NONE;
    if (mac.length()) {
        if (trackId.length() == mac.length() + 3 &&
            trackId.startsWith("AP:") && trackId.endsWith(mac)) {
            return BIN_TRACK_AP_MAC;
        }
        if (trackId.length() == mac.length() + 4 &&
            trackId.startsWith("MAC:") && trackId.endsWith(mac)) {
            return BIN_TRACK_MAC_MAC;
        }
    }
    if (ieFingerprint.length() &&
        trackId.length() == ieFingerprint.length() + 3 &&
        trackId.startsWith("IE:") && trackId.endsWith(ieFingerprint)) {
        return BIN_TRACK_IE_FP;
    }
    return BIN_TRACK_LITERAL;
}

static String _binaryTrackIdFromMode(uint8_t mode,
                                     const String& mac,
                                     const String& ieFingerprint,
                                     const String& literal) {
    switch (mode) {
        case BIN_TRACK_AP_MAC:  return String("AP:") + mac;
        case BIN_TRACK_MAC_MAC: return String("MAC:") + mac;
        case BIN_TRACK_IE_FP:   return String("IE:") + ieFingerprint;
        case BIN_TRACK_LITERAL: return literal;
        default:                return String();
    }
}

static uint8_t _defaultPriorityForBinaryType(const String& typeStr,
                                             const String& eventTypeStr) {
    return static_cast<uint8_t>(
        RAMSpool::classify(typeStr.c_str(), eventTypeStr.c_str()).priority);
}

static uint8_t _defaultLaneForBinaryType(const String& typeStr,
                                         const String& eventTypeStr) {
    return static_cast<uint8_t>(
        RAMSpool::classify(typeStr.c_str(), eventTypeStr.c_str()).lane);
}

static void _appendUVarintToBytes(std::vector<uint8_t>& out, uint32_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7FU);
        value >>= 7;
        if (value) byte |= 0x80U;
        out.push_back(byte);
    } while (value);
}

static bool _readUVarintFromBytes(const uint8_t*& p, const uint8_t* end, uint32_t& out) {
    out = 0;
    uint8_t shift = 0;

    for (int i = 0; i < 5; i++) {
        if (p >= end) return false;
        const uint8_t byte = *p++;
        out |= (static_cast<uint32_t>(byte & 0x7FU) << shift);
        if ((byte & 0x80U) == 0) {
            return true;
        }
        shift += 7;
    }

    return false;
}

static void _appendZigZag32ToBytes(std::vector<uint8_t>& out, int32_t value) {
    const uint32_t zz =
        (static_cast<uint32_t>(value) << 1) ^
        static_cast<uint32_t>(value >> 31);
    _appendUVarintToBytes(out, zz);
}

static bool _readZigZag32FromBytes(const uint8_t*& p, const uint8_t* end, int32_t& out) {
    uint32_t zz = 0;
    if (!_readUVarintFromBytes(p, end, zz)) return false;
    out = static_cast<int32_t>((zz >> 1) ^ (~(zz & 1) + 1));
    return true;
}

static void _appendStringToBytes(std::vector<uint8_t>& out, const String& s) {
    _appendUVarintToBytes(out, static_cast<uint32_t>(s.length()));
    for (size_t i = 0; i < s.length(); i++) {
        out.push_back(static_cast<uint8_t>(s[i]));
    }
}

static bool _readStringFromBytes(const uint8_t*& p, const uint8_t* end, String& out) {
    uint32_t len = 0;
    if (!_readUVarintFromBytes(p, end, len)) return false;
    if (static_cast<size_t>(end - p) < len) return false;

    out = "";
    for (uint32_t i = 0; i < len; i++) {
        out += static_cast<char>(*p++);
    }
    return true;
}

// CRC32 helper moved to StorageFsUtil::crc32Bytes (same polynomial / init /
// output complement).

static uint32_t _uploadIndexRecordHash(const UploadIndexRecordV1& record) {
    return StorageFsUtil::crc32Bytes(reinterpret_cast<const uint8_t*>(&record),
                                     sizeof(record) - sizeof(record.crc));
}

static int32_t _floatToE7(float v) {
    return static_cast<int32_t>(v * 10000000.0f);
}

static float _e7ToFloat(int32_t v) {
    return static_cast<float>(v) / 10000000.0f;
}

static int32_t _floatToCm(float v) {
    return static_cast<int32_t>(v * 100.0f);
}

static float _cmToFloat(int32_t v) {
    return static_cast<float>(v) / 100.0f;
}

static uint32_t _floatToDm(float v) {
    if (v <= 0.0f) return 0;
    return static_cast<uint32_t>(v * 10.0f);
}

static float _dmToFloat(uint32_t v) {
    return static_cast<float>(v) / 10.0f;
}

static uint32_t _floatToCenti(float v) {
    if (v <= 0.0f) return 0;
    return static_cast<uint32_t>(v * 100.0f);
}

static float _centiToFloat(uint32_t v) {
    return static_cast<float>(v) / 100.0f;
}

static uint32_t _timestampDeltaFromBase(uint32_t ts, uint32_t baseTs) {
    if (ts >= baseTs) {
        return ts - baseTs;
    }
    return ts;
}

static uint32_t _timestampFromBaseDelta(uint32_t delta, uint32_t baseTs) {
    return baseTs + delta;
}

// Absolute capture UTC for a record, from the segment's epoch base plus the
// record's millis delta. baseEpoch==0 means the segment has no trusted clock
// reference, so the record is unenrichable (returns 0). Adds zero per-record
// storage — the delta is the same varint already persisted for timestampMs.
static uint32_t _epochFromBaseDelta(uint32_t deltaMs, uint32_t baseEpochUtc) {
    if (baseEpochUtc == 0) return 0U;
    return baseEpochUtc + (deltaMs / 1000U);
}

static bool _parseMacStringToBytes(const String& mac, uint8_t out[6]) {
    unsigned int b0, b1, b2, b3, b4, b5;
    if (sscanf(mac.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               &b0, &b1, &b2, &b3, &b4, &b5) == 6) {
        out[0] = static_cast<uint8_t>(b0);
        out[1] = static_cast<uint8_t>(b1);
        out[2] = static_cast<uint8_t>(b2);
        out[3] = static_cast<uint8_t>(b3);
        out[4] = static_cast<uint8_t>(b4);
        out[5] = static_cast<uint8_t>(b5);
        return true;
    }
    return false;
}

static bool _ouiEquals(const uint8_t a[3], const uint8_t b[3]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static void _copyOui(const uint8_t src[3], uint8_t dst[3]) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
}

static String _macBytesToString(const uint8_t in[6]) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             in[0], in[1], in[2], in[3], in[4], in[5]);
    return String(buf);
}

static bool _splitMacOuiSuffix(const uint8_t mac[6], uint8_t oui[3], uint8_t suffix[3]) {
    for (int i = 0; i < 3; i++) {
        oui[i] = mac[i];
        suffix[i] = mac[i + 3];
    }
    return true;
}

static void _appendMacFieldToBytes(std::vector<uint8_t>& out,
                                   const String& mac,
                                   uint8_t lastOui[3],
                                   bool& hasLastOui) {
    if (!mac.length()) {
        out.push_back(BIN_MAC_NONE);
        return;
    }

    uint8_t macBytes[6] = {0, 0, 0, 0, 0, 0};
    if (_parseMacStringToBytes(mac, macBytes)) {
        uint8_t oui[3] = {0, 0, 0};
        uint8_t suffix[3] = {0, 0, 0};
        _splitMacOuiSuffix(macBytes, oui, suffix);

        if (hasLastOui && _ouiEquals(oui, lastOui)) {
            out.push_back(BIN_MAC_PREV_OUI_SUFFIX);
            for (int i = 0; i < 3; i++) {
                out.push_back(suffix[i]);
            }
            return;
        }

        out.push_back(BIN_MAC_OUI_SUFFIX);
        for (int i = 0; i < 3; i++) {
            out.push_back(oui[i]);
        }
        for (int i = 0; i < 3; i++) {
            out.push_back(suffix[i]);
        }

        _copyOui(oui, lastOui);
        hasLastOui = true;
        return;
    }

    out.push_back(BIN_MAC_STRING);
    _appendStringToBytes(out, mac);
}

static bool _readMacFieldFromBytes(const uint8_t*& p,
                                   const uint8_t* end,
                                   String& mac,
                                   uint8_t lastOui[3],
                                   bool& hasLastOui) {
    if (p >= end) return false;

    const uint8_t mode = *p++;

    switch (mode) {
        case BIN_MAC_NONE:
            mac = "";
            return true;

        case BIN_MAC_FULL: {
            if (static_cast<size_t>(end - p) < 6) {
                return false;
            }
            uint8_t macBytes[6];
            for (int i = 0; i < 6; i++) {
                macBytes[i] = *p++;
            }
            mac = _macBytesToString(macBytes);
            return true;
        }

        case BIN_MAC_OUI_SUFFIX: {
            if (static_cast<size_t>(end - p) < 6) {
                return false;
            }
            uint8_t macBytes[6];
            uint8_t oui[3];
            for (int i = 0; i < 3; i++) {
                oui[i] = *p++;
                macBytes[i] = oui[i];
            }
            for (int i = 0; i < 3; i++) {
                macBytes[i + 3] = *p++;
            }
            _copyOui(oui, lastOui);
            hasLastOui = true;
            mac = _macBytesToString(macBytes);
            return true;
        }

        case BIN_MAC_PREV_OUI_SUFFIX: {
            if (!hasLastOui || static_cast<size_t>(end - p) < 3) {
                return false;
            }
            uint8_t macBytes[6];
            macBytes[0] = lastOui[0];
            macBytes[1] = lastOui[1];
            macBytes[2] = lastOui[2];
            for (int i = 0; i < 3; i++) {
                macBytes[i + 3] = *p++;
            }
            mac = _macBytesToString(macBytes);
            return true;
        }

        case BIN_MAC_STRING:
            return _readStringFromBytes(p, end, mac);

        default:
            return false;
    }
}

// =====================================================================
// Unified payload-family decoder.
//
// The scan decoder (_scanSegmentRecords) and the random-access decoder
// (_decodeBinarySpoolRecordBody) previously carried byte-identical copies of
// every payload family. Two copies meant every format change had to be made
// twice and every fix could be applied to only one -- which is how the
// random-access path ended up without the reboot-safe timestamp handling the
// scan path had. Both now call this.
//
// `p` is advanced past the payload. Returns false on a malformed body.
// =====================================================================
static bool _decodeBinaryPayloadBody(const uint8_t*& p,
                                     const uint8_t* end,
                                     const String& typeStr,
                                     uint8_t payloadFamily,
                                     uint8_t eventFlags,
                                     JsonObject root) {
    const bool isNetwork = (typeStr == "network");
    const char* macKey = isNetwork ? "bssid" : "mac";

    if (payloadFamily == BIN_PAYLOAD_PROBE_DEVICE ||
        payloadFamily == BIN_PAYLOAD_PROBE_DEVICE_V2) {
        const bool v2 = (payloadFamily == BIN_PAYLOAD_PROBE_DEVICE_V2);

        String mac;
        String ssid;
        String ieFingerprint;
        String probeSetHash;
        int32_t rssi = 0;
        uint32_t channel = 0;

        if (p >= end) return false;
        const uint8_t flags = *p++;

        uint8_t lastOui[3] = {0, 0, 0};
        bool hasLastOui = false;

        if (!_readMacFieldFromBytes(p, end, mac, lastOui, hasLastOui)) return false;
        if ((flags & BIN_PDV2_SSID) && !_readStringFromBytes(p, end, ssid)) return false;
        if ((flags & BIN_PDV2_RSSI) && !_readZigZag32FromBytes(p, end, rssi)) return false;

        root[macKey] = mac;
        if (flags & BIN_PDV2_SSID) {
            // probe records carry the *probed* SSID, which is a different
            // thing from the SSID a network/device record reports.
            root[(typeStr == "probe") ? "probed_ssid" : "ssid"] = ssid;
        }
        if (flags & BIN_PDV2_RSSI) root["rssi"] = rssi;
        if (flags & BIN_PDV2_CHANNEL) {
            if (!_readUVarintFromBytes(p, end, channel)) return false;
            root["channel"] = channel;
        }
        if (flags & BIN_PDV2_IE_FP) {
            if (!_readStringFromBytes(p, end, ieFingerprint)) return false;
            root["ie_fingerprint"] = ieFingerprint;
        }
        if (flags & BIN_PDV2_PROBE_HASH) {
            if (!_readStringFromBytes(p, end, probeSetHash)) return false;
            root["probe_set_hash"] = probeSetHash;
        }
        if (flags & BIN_PDV2_RANDOM_MAC) {
            if (p >= end) return false;
            root["is_random_mac"] = (*p++ != 0) ? 1 : 0;
        }
        if (flags & BIN_PDV2_BROADCAST) {
            if (p >= end) return false;
            root["is_broadcast"] = (*p++ != 0) ? 1 : 0;
        }

        // v1 stops here; 0x80 is only a flags2 marker in v2.
        if (!v2 || !(flags & BIN_PDV2_HAS_FLAGS2)) return true;

        if (p >= end) return false;
        const uint8_t flags2 = *p++;

        if (flags2 & BIN_PDV2_SECURITY) {
            if (p >= end) return false;
            const uint8_t secCode = *p++;
            if (secCode == BIN_SEC_LITERAL) {
                String security;
                if (!_readStringFromBytes(p, end, security)) return false;
                root["security"] = security;
            } else {
                root["security"] = _binarySecurityStringFromCode(secCode);
            }
        }
        if (flags2 & BIN_PDV2_HIDDEN) {
            if (p >= end) return false;
            root["is_hidden"] = (*p++ != 0) ? 1 : 0;
        }
        if (flags2 & BIN_PDV2_WPS) {
            if (p >= end) return false;
            root["has_wps"] = (*p++ != 0) ? 1 : 0;
        }
        if (flags2 & BIN_PDV2_LOCALIZATION) {
            uint32_t sampleSeq = 0;
            uint32_t sampleFrames = 0;
            int32_t rssiMinDelta = 0;
            int32_t rssiMaxDelta = 0;
            if (!_readUVarintFromBytes(p, end, sampleSeq) ||
                !_readUVarintFromBytes(p, end, sampleFrames) ||
                !_readZigZag32FromBytes(p, end, rssiMinDelta) ||
                !_readZigZag32FromBytes(p, end, rssiMaxDelta)) {
                return false;
            }
            if (p >= end) return false;
            const uint8_t reasonCode = *p++;

            root["localization_sample"] = true;
            root["sample_seq"] = sampleSeq;
            root["sample_frames"] = sampleFrames;
            // min/max are stored as deltas from rssi -- they sit within a few
            // dB of it, so the delta is almost always a single varint byte.
            root["rssi_min"] = rssi + rssiMinDelta;
            root["rssi_max"] = rssi + rssiMaxDelta;
            if (reasonCode == BIN_REASON_LITERAL) {
                String reason;
                if (!_readStringFromBytes(p, end, reason)) return false;
                root["sample_reason"] = reason;
            } else {
                root["sample_reason"] = _binarySampleReasonStringFromCode(reasonCode);
            }
        }
        if (flags2 & BIN_PDV2_TRACK) {
            if (p >= end) return false;
            const uint8_t trackMode = *p++;
            String literal;
            if (trackMode == BIN_TRACK_LITERAL &&
                !_readStringFromBytes(p, end, literal)) {
                return false;
            }
            const String trackId =
                _binaryTrackIdFromMode(trackMode, mac, ieFingerprint, literal);
            if (trackId.length()) root["track_id"] = trackId;
        }
        if (flags2 & BIN_PDV2_TXPWR) {
            if (static_cast<size_t>(end - p) < 2) return false;
            const int8_t txPower = static_cast<int8_t>(*p++);
            const uint8_t txSrc  = *p++;
            root["tx_power"] = txPower;
            root["tx_power_src"] = txSrc;
        }
        if (flags2 & BIN_PDV2_RFCTX) {
            if (static_cast<size_t>(end - p) < 2) return false;
            const int8_t noiseFloor = static_cast<int8_t>(*p++);
            const int8_t antGainQ2  = static_cast<int8_t>(*p++);
            if (noiseFloor != 0) root["noise_floor"] = noiseFloor;
            root["ant_gain_q2"] = antGainQ2;
        }
        return true;
    }

    if (payloadFamily == BIN_PAYLOAD_PMKID) {
        String ap, sta, ssid, pmkidHex, hashcatLine;
        int32_t rssi = 0;

        if (p >= end) return false;
        const uint8_t flags = *p++;

        uint8_t lastOui[3] = {0, 0, 0};
        bool hasLastOui = false;

        if (!_readMacFieldFromBytes(p, end, ap, lastOui, hasLastOui)) return false;
        if (!_readMacFieldFromBytes(p, end, sta, lastOui, hasLastOui)) return false;
        if ((flags & 0x01) && !_readStringFromBytes(p, end, ssid)) return false;
        if ((flags & 0x02) && !_readZigZag32FromBytes(p, end, rssi)) return false;
        if ((flags & 0x04) && !_readStringFromBytes(p, end, pmkidHex)) return false;
        if ((flags & 0x08) && !_readStringFromBytes(p, end, hashcatLine)) return false;

        root["ap"] = ap;
        root["sta"] = sta;
        if (flags & 0x01) root["ssid"] = ssid;
        if (flags & 0x02) root["rssi"] = rssi;
        if (flags & 0x04) root["pmkid_hex"] = pmkidHex;
        if (flags & 0x08) root["hashcat_line"] = hashcatLine;
        return true;
    }

    if (payloadFamily == BIN_PAYLOAD_HANDSHAKE) {
        String ap, sta, ssid;
        int32_t rssi = 0;
        uint32_t frameMask = 0;
        uint32_t messageNumber = 0;

        if (p >= end) return false;
        const uint8_t flags = *p++;

        uint8_t lastOui[3] = {0, 0, 0};
        bool hasLastOui = false;

        if (!_readMacFieldFromBytes(p, end, ap, lastOui, hasLastOui)) return false;
        if (!_readMacFieldFromBytes(p, end, sta, lastOui, hasLastOui)) return false;
        if ((flags & 0x01) && !_readStringFromBytes(p, end, ssid)) return false;
        if (!_readUVarintFromBytes(p, end, frameMask)) return false;
        if ((flags & 0x02) && !_readZigZag32FromBytes(p, end, rssi)) return false;
        if ((flags & 0x04) && !_readUVarintFromBytes(p, end, messageNumber)) return false;

        root["ap"] = ap;
        root["sta"] = sta;
        if (flags & 0x01) root["ssid"] = ssid;
        root["frame_mask"] = frameMask;
        if (flags & 0x02) root["rssi"] = rssi;
        if (flags & 0x04) root["message"] = messageNumber;
        root["event_type"] = "handshake";
        return true;
    }

    if (payloadFamily == BIN_PAYLOAD_DRONE) {
        String droneId, mac, protocol;
        int32_t latitudeE7 = 0, longitudeE7 = 0;
        uint32_t channel = 0;
        uint8_t lastOui[3] = {0, 0, 0};
        bool hasLastOui = false;

        if (p >= end) return false;
        const uint8_t flags = *p++;

        if ((flags & 0x01) && !_readStringFromBytes(p, end, droneId)) return false;
        if ((flags & 0x02) &&
            !_readMacFieldFromBytes(p, end, mac, lastOui, hasLastOui)) return false;
        if (flags & 0x04) {
            int32_t rssi = 0;
            if (!_readZigZag32FromBytes(p, end, rssi)) return false;
            root["rssi"] = rssi;
        }
        if (flags & 0x08) {
            if (!_readUVarintFromBytes(p, end, channel)) return false;
            root["channel"] = channel;
        }
        if ((flags & 0x10) && !_readStringFromBytes(p, end, protocol)) return false;
        if (flags & 0x10) root["protocol"] = protocol;
        if (flags & 0x20) {
            if (!_readZigZag32FromBytes(p, end, latitudeE7) ||
                !_readZigZag32FromBytes(p, end, longitudeE7)) {
                return false;
            }
            root["latitude"] = _e7ToFloat(latitudeE7);
            root["longitude"] = _e7ToFloat(longitudeE7);
        }
        if (flags & 0x40) {
            uint32_t altitudeCenti = 0;
            if (!_readUVarintFromBytes(p, end, altitudeCenti)) return false;
            root["altitude_m"] = _centiToFloat(altitudeCenti);
        }
        if (flags & 0x80) {
            uint32_t speedCenti = 0;
            if (!_readUVarintFromBytes(p, end, speedCenti)) return false;
            root["speed"] = _centiToFloat(speedCenti);
        }
        if (flags & 0x01) root["drone_id"] = droneId;
        if (flags & 0x02) root["mac"] = mac;
        return true;
    }

    if (payloadFamily == BIN_PAYLOAD_FIELD_MAP) {
        return _readBinaryFieldMapFromBytes(p, end, root);
    }

    if (payloadFamily == BIN_PAYLOAD_JSON_FALLBACK) {
        // Legacy fallback payload, readback only.
        if (!(eventFlags & BIN_EVENT_HAS_PAYLOAD)) return true;

        String payloadJson;
        if (!_readStringFromBytes(p, end, payloadJson)) return false;
        if (!payloadJson.length()) return true;

        JsonDocument payloadDoc;
        DeserializationError err = deserializeJson(payloadDoc, payloadJson);
        if (err || !payloadDoc.is<JsonObject>()) return false;
        for (JsonPairConst kv : payloadDoc.as<JsonObjectConst>()) {
            root[kv.key().c_str()].set(kv.value());
        }
        return true;
    }

    return false;
}

// Re-attach the fields the writer deliberately stopped storing. This is the
// decode edge: the values are constants or restatements of data already in the
// record header, so they cost nothing on flash and are rebuilt only here, for
// consumers (EtherGuard schema, phone offload, exports) that expect them.
static void _applyDerivedEventFields(JsonObject root,
                                     const String& typeStr,
                                     const String& sessionId,
                                     const String& sessionTag) {
    // `sensor` is stamped on every queued event by _prepareQueuedEvent.
    root["sensor"] = SPECTRE_MQTT_SENSOR_ID;
    // `source` is not: only the network and device producers emit it, so
    // restoring it everywhere would invent a field the record never had.
    if (typeStr == "network" || typeStr == "device") {
        root["source"] = SPECTRE_MQTT_SENSOR_ID;
    }
    root["session_id"] = sessionId;
    if (sessionTag.length()) root["session_tag"] = sessionTag;
}


struct BinaryMetaRecord {
    SpoolDecodedRecordType recordType = SPOOL_REC_UNKNOWN;
    uint32_t eventId = 0;
    String sessionId;
};

struct BinaryCheckpointScanResult {
    bool found = false;
    SpoolBin::SpoolSegmentCheckpointV1 checkpoint{};
    uint32_t nextOffset = 0;
};

// session_tag is a property of the session, not of each record, so it is
// written only when the session string itself is written (see
// BIN_SESSION_INLINE_TAGGED). Records that inherit their session via
// SAME_AS_PREV inherit the tag too. This side table lets the random-access
// decoder -- which starts at an arbitrary offset and has no previous record to
// inherit from -- resolve the tag for a session it has seen elsewhere.
static std::map<String, String> g_sessionTagBySession;
static constexpr size_t kSessionTagCacheMax = 32;

static void _rememberSessionTag(const String& sessionId, const String& tag) {
    if (!sessionId.length() || !tag.length()) return;
    if (g_sessionTagBySession.size() >= kSessionTagCacheMax &&
        g_sessionTagBySession.find(sessionId) == g_sessionTagBySession.end()) {
        return;
    }
    g_sessionTagBySession[sessionId] = tag;
}

static String _lookupSessionTag(const String& sessionId) {
    if (!sessionId.length()) return String();
    const auto it = g_sessionTagBySession.find(sessionId);
    return (it == g_sessionTagBySession.end()) ? String() : it->second;
}

// Reads the session field common to every record header.
//   SAME_AS_PREV  - inherit session (and tag) from the previous record
//   INLINE        - session string follows
//   INLINE_TAGGED - session string then session_tag string follow
static bool _readBinarySessionField(const uint8_t*& p,
                                    const uint8_t* end,
                                    uint8_t sessionMode,
                                    String& lastSession,
                                    String& lastSessionTag,
                                    String& sessionId,
                                    String& sessionTag) {
    if (sessionMode == BIN_SESSION_SAME_AS_PREV) {
        sessionId = lastSession;
        sessionTag = lastSessionTag;
        return true;
    }
    if (!_readStringFromBytes(p, end, sessionId)) return false;
    sessionTag = "";
    if (sessionMode == BIN_SESSION_INLINE_TAGGED &&
        !_readStringFromBytes(p, end, sessionTag)) {
        return false;
    }
    lastSession = sessionId;
    lastSessionTag = sessionTag;
    _rememberSessionTag(sessionId, sessionTag);
    return true;
}

static bool _decodeBinaryMetaRecord(const uint8_t* data,
                                    size_t len,
                                    uint8_t recordPrefixType,
                                    String& lastSession,
                                    String& lastSessionTag,
                                    BinaryEnrichContext& enrichCtx,
                                    BinaryMetaRecord& out) {
    out = {};

    const uint8_t* p = data;
    const uint8_t* end = data + len;

    if (recordPrefixType == SpoolBin::REC_ENRICH_DELTA ||
        recordPrefixType == SpoolBin::REC_ENRICH_DELTA_V2) {
        DecodedEnrichDelta d;
        if (!_decodeBinaryEnrichDeltaBody(p, end, recordPrefixType, enrichCtx,
                                          lastSession, lastSessionTag, d)) {
            return false;
        }
        out.recordType = SPOOL_REC_ENRICH_DELTA;
        out.eventId = d.recordId;
        out.sessionId = d.sessionId;
        return true;
    }

    uint32_t recordId = 0;
    uint32_t tsDelta = 0;
    uint8_t sessionMode = BIN_SESSION_INLINE;
    String sessionId;

    if (!_readUVarintFromBytes(p, end, recordId) ||
        !_readUVarintFromBytes(p, end, tsDelta)) {
        return false;
    }

    if (p >= end) return false;
    sessionMode = *p++;

    if (sessionMode == BIN_SESSION_SAME_AS_PREV) {
        sessionId = lastSession;
    } else {
        if (!_readStringFromBytes(p, end, sessionId)) {
            return false;
        }
        lastSession = sessionId;
    }

    out.recordType = SPOOL_REC_EVENT;
    out.eventId = recordId;
    out.sessionId = sessionId;
    return true;
}

static bool _scanBinarySegmentMetaRecords(const String& path,
                                          std::function<bool(const BinaryMetaRecord&)> cb) {
    if (!LittleFS.exists(path)) {
        return false;
    }

    File f = LittleFS.open(path, "r");
    if (!f) {
        return false;
    }

    SpoolBin::SegmentHeaderV2 hdr;
    if (!SpoolBin::readSegmentHeaderV2(f, hdr)) {
        DLOG_WARN("STORAGE", "Binary meta header read failed path=%s", path.c_str());
        f.close();
        return false;
    }

    if (hdr.magic != SpoolBin::SEGMENT_MAGIC || hdr.version != 2) {
        DLOG_WARN("STORAGE", "Binary meta invalid header path=%s", path.c_str());
        f.close();
        return false;
    }

    if (!f.seek(sizeof(SpoolBin::SegmentHeaderV2))) {
        f.close();
        return false;
    }

    uint32_t workCounter = 0;
    String lastSession;
    String lastSessionTag;
    // Delta-coded enrichment records resolve against the previous enrichment
    // record in this segment; the context resets with each segment scan.
    BinaryEnrichContext enrichCtx;
    bool ok = true;

    while (f.position() < f.size()) {
        const size_t remainingBeforePrefix = static_cast<size_t>(f.size() - f.position());
        if (remainingBeforePrefix < sizeof(SpoolBin::RecordPrefix)) {
            DLOG_WARN("STORAGE",
                      "Binary meta truncated tail path=%s remaining=%u",
                      path.c_str(),
                      static_cast<unsigned>(remainingBeforePrefix));
            ok = false;
            break;
        }

        SpoolBin::RecordPrefix prefix;
        if (!SpoolBin::readBytes(f, &prefix, sizeof(prefix))) {
            DLOG_WARN("STORAGE", "Binary meta prefix read failed path=%s", path.c_str());
            ok = false;
            break;
        }

        const size_t remainingAfterPrefix = static_cast<size_t>(f.size() - f.position());
        if (prefix.length > remainingAfterPrefix) {
            DLOG_WARN("STORAGE",
                      "Binary meta truncated body path=%s len=%u remaining=%u",
                      path.c_str(),
                      static_cast<unsigned>(prefix.length),
                      static_cast<unsigned>(remainingAfterPrefix));
            ok = false;
            break;
        }

        std::vector<uint8_t> body(prefix.length);
        if (prefix.length > 0) {
            if (!SpoolBin::readBytes(f, body.data(), prefix.length)) {
                DLOG_WARN("STORAGE", "Binary meta body read failed path=%s", path.c_str());
                ok = false;
                break;
            }
        }

        if (prefix.type == SpoolBin::REC_CHECKPOINT) {
            continue;
        }

        BinaryMetaRecord rec;
        if (!_decodeBinaryMetaRecord(body.data(), body.size(), prefix.type, lastSession, lastSessionTag, enrichCtx, rec)) {
            DLOG_WARN("STORAGE",
                      "Binary meta decode failed path=%s type=%u len=%u",
                      path.c_str(),
                      static_cast<unsigned>(prefix.type),
                      static_cast<unsigned>(prefix.length));
            ok = false;
            break;
        }

        if (!cb(rec)) {
            f.close();
            return true;
        }

        workCounter++;
        if ((workCounter & 0x1FU) == 0U) {
            delay(1);
        }
    }

    f.close();
    return ok;
}

static bool _findLatestBinaryCheckpoint(const String& path,
                                        BinaryCheckpointScanResult& out) {
    out = {};

    if (!LittleFS.exists(path)) {
        return false;
    }

    File f = LittleFS.open(path, "r");
    if (!f) {
        return false;
    }

    SpoolBin::SegmentHeaderV2 hdr;
    if (!SpoolBin::readSegmentHeaderV2(f, hdr)) {
        f.close();
        return false;
    }

    if (hdr.magic != SpoolBin::SEGMENT_MAGIC || hdr.version != 2) {
        f.close();
        return false;
    }

    if (!f.seek(sizeof(SpoolBin::SegmentHeaderV2))) {
        f.close();
        return false;
    }

    while (f.position() < f.size()) {
        const size_t remainingBeforePrefix =
            static_cast<size_t>(f.size() - f.position());
        if (remainingBeforePrefix < sizeof(SpoolBin::RecordPrefix)) {
            f.close();
            return false;
        }

        SpoolBin::RecordPrefix prefix;
        if (!SpoolBin::readBytes(f, &prefix, sizeof(prefix))) {
            f.close();
            return false;
        }

        const size_t remainingAfterPrefix =
            static_cast<size_t>(f.size() - f.position());
        if (prefix.length > remainingAfterPrefix) {
            f.close();
            return false;
        }

        const uint32_t bodyOffset = static_cast<uint32_t>(f.position());
        if (prefix.type != SpoolBin::REC_CHECKPOINT) {
            if (!f.seek(bodyOffset + prefix.length)) {
                f.close();
                return false;
            }
            continue;
        }

        std::vector<uint8_t> body(prefix.length);
        if (prefix.length > 0 &&
            !SpoolBin::readBytes(f, body.data(), prefix.length)) {
            f.close();
            return false;
        }

        SpoolBin::SpoolSegmentCheckpointV1 checkpoint;
        if (SpoolBin::decodeCheckpointRecordV1(body.data(),
                                               body.size(),
                                               bodyOffset,
                                               checkpoint) &&
            checkpoint.segmentId == hdr.segmentId) {
            out.found = true;
            out.checkpoint = checkpoint;
            out.nextOffset = static_cast<uint32_t>(f.position());
        }
    }

    f.close();
    return true;
}

inline void storageScanYield(uint32_t& workCounter) {
    workCounter++;
    if ((workCounter & 0x1FU) == 0U) {
        delay(1);
    }
}

// Decode failures are SKIPPED (framing intact) rather than aborting the scan.
// I/O errors and structural problems (bad header, bad prefix, body overrun) are
// FATAL and stop immediately.
// File-scope static: must access BinaryMetaRecord/_decodeBinaryMetaRecord from
// the anonymous namespace above, so it cannot be a class method.
static SpoolScanStatus _scanBinarySegmentMetaRecordsAudit(
        const String& path,
        const std::function<bool(const BinaryMetaRecord&)>& cb,
        SpoolAuditResult& audit) {

    if (!LittleFS.exists(path)) {
        return SpoolScanStatus::FATAL;
    }

    File f = LittleFS.open(path, "r");
    if (!f) {
        return SpoolScanStatus::FATAL;
    }

    SpoolBin::SegmentHeaderV2 hdr;
    if (!SpoolBin::readSegmentHeaderV2(f, hdr)) {
        DLOG_WARN("STORAGE", "Audit hdr read failed path=%s", path.c_str());
        f.close();
        return SpoolScanStatus::FATAL;
    }

    if (hdr.magic != SpoolBin::SEGMENT_MAGIC || hdr.version != 2) {
        DLOG_WARN("STORAGE", "Audit invalid hdr path=%s", path.c_str());
        f.close();
        return SpoolScanStatus::FATAL;
    }

    if (!f.seek(sizeof(SpoolBin::SegmentHeaderV2))) {
        f.close();
        return SpoolScanStatus::FATAL;
    }

    uint32_t workCounter = 0;
    uint32_t skipWarnCount = 0;
    String lastSession;
    String lastSessionTag;
    // Delta-coded enrichment records resolve against the previous enrichment
    // record in this segment; the context resets with each segment scan.
    BinaryEnrichContext enrichCtx;
    bool hadSkips = false;

    while (f.position() < f.size()) {
        const size_t remainingBeforePrefix =
            static_cast<size_t>(f.size() - f.position());
        if (remainingBeforePrefix < sizeof(SpoolBin::RecordPrefix)) {
            DLOG_WARN("STORAGE",
                      "Audit truncated tail path=%s remaining=%u",
                      path.c_str(),
                      static_cast<unsigned>(remainingBeforePrefix));
            f.close();
            return SpoolScanStatus::FATAL;
        }

        SpoolBin::RecordPrefix prefix;
        if (!SpoolBin::readBytes(f, &prefix, sizeof(prefix))) {
            DLOG_WARN("STORAGE", "Audit prefix read failed path=%s", path.c_str());
            f.close();
            return SpoolScanStatus::FATAL;
        }

        const size_t remainingAfterPrefix =
            static_cast<size_t>(f.size() - f.position());
        if (prefix.length > remainingAfterPrefix) {
            DLOG_WARN("STORAGE",
                      "Audit truncated body path=%s len=%u remaining=%u",
                      path.c_str(),
                      static_cast<unsigned>(prefix.length),
                      static_cast<unsigned>(remainingAfterPrefix));
            f.close();
            return SpoolScanStatus::FATAL;
        }

        std::vector<uint8_t> body(prefix.length);
        if (prefix.length > 0) {
            if (!SpoolBin::readBytes(f, body.data(), prefix.length)) {
                DLOG_WARN("STORAGE", "Audit body read failed path=%s", path.c_str());
                f.close();
                return SpoolScanStatus::FATAL;
            }
        }

        if (prefix.type == SpoolBin::REC_CHECKPOINT) {
            storageScanYield(workCounter);
            continue;
        }

        audit.scannedRecords++;

        BinaryMetaRecord rec;
        if (!_decodeBinaryMetaRecord(body.data(), body.size(),
                                     prefix.type, lastSession, lastSessionTag, enrichCtx, rec)) {
            audit.invalidRecords++;
            audit.skippedRecords++;
            hadSkips = true;
            if (skipWarnCount < 4) {
                DLOG_WARN("STORAGE",
                          "Audit decode skip path=%s type=%u len=%u skips=%lu",
                          path.c_str(),
                          static_cast<unsigned>(prefix.type),
                          static_cast<unsigned>(prefix.length),
                          static_cast<unsigned long>(audit.skippedRecords));
                skipWarnCount++;
            }
            storageScanYield(workCounter);
            continue;
        }

        if (rec.recordType == SPOOL_REC_EVENT) {
            audit.validEventRecords++;
            if (rec.eventId > audit.maxEventIdSeen) {
                audit.maxEventIdSeen = rec.eventId;
            }
        } else if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
            audit.validEnrichDeltas++;
            if (rec.eventId > audit.maxEventIdSeen) {
                audit.maxEventIdSeen = rec.eventId;
            }
        }

        if (!cb(rec)) {
            f.close();
            return hadSkips ? SpoolScanStatus::OK_WITH_SKIPS
                            : SpoolScanStatus::OK;
        }

        storageScanYield(workCounter);
    }

    f.close();
    return hadSkips ? SpoolScanStatus::OK_WITH_SKIPS : SpoolScanStatus::OK;
}

void _copySettingsToConfig(const RuntimeSettings& settings, DeviceConfig& config) {
    config.name = settings.deviceName;
    config.owner = settings.deviceOwner;
    config.version = settings.deviceVersion;
    config.loraFreq = static_cast<long>(settings.loraFrequency);
    config.loraNetworkId = settings.loraNetworkId;
    config.loraAddress = settings.loraAddress;
    config.loraSF = settings.loraSF;
    config.loraBW = settings.loraBW;
    config.loraCR = settings.loraCR;
    config.loraPreamble = settings.loraPreamble;
    config.mqttBroker = settings.mqttBroker;
    config.mqttPort = settings.mqttPort;
    config.mqttUser = settings.mqttUser;
    config.mqttPassword = settings.mqttPassword;
    config.mqttTopicBase = settings.mqttTopicBase;
    config.wifiNetworks.clear();
    for (uint8_t i = 0; i < settings.wifiNetworkCount; i++) {
        config.wifiNetworks.push_back({
            String(settings.wifiNetworks[i].ssid),
            String(settings.wifiNetworks[i].password)
        });
    }
}

void _copyConfigToSettings(const DeviceConfig& config, RuntimeSettings& settings) {
    strlcpy(settings.deviceName, config.name.c_str(), sizeof(settings.deviceName));
    strlcpy(settings.deviceOwner, config.owner.c_str(), sizeof(settings.deviceOwner));
    strlcpy(settings.deviceVersion, config.version.c_str(), sizeof(settings.deviceVersion));
    settings.loraFrequency = static_cast<uint32_t>(config.loraFreq);
    settings.loraNetworkId = static_cast<uint16_t>(config.loraNetworkId);
    settings.loraAddress = static_cast<uint16_t>(config.loraAddress);
    settings.loraSF = static_cast<uint8_t>(config.loraSF);
    settings.loraBW = static_cast<uint8_t>(config.loraBW);
    settings.loraCR = static_cast<uint8_t>(config.loraCR);
    settings.loraPreamble = static_cast<uint8_t>(config.loraPreamble);
    strlcpy(settings.mqttBroker, config.mqttBroker.c_str(), sizeof(settings.mqttBroker));
    settings.mqttPort = static_cast<uint16_t>(config.mqttPort);
    strlcpy(settings.mqttUser, config.mqttUser.c_str(), sizeof(settings.mqttUser));
    strlcpy(settings.mqttPassword, config.mqttPassword.c_str(), sizeof(settings.mqttPassword));
    strlcpy(settings.mqttTopicBase, config.mqttTopicBase.c_str(), sizeof(settings.mqttTopicBase));

    settings.wifiNetworkCount = 0;
    const size_t maxNetworks = std::min(
        config.wifiNetworks.size(),
        static_cast<size_t>(SETTINGS_WIFI_NETWORK_CAPACITY));
    for (size_t i = 0; i < maxNetworks; i++) {
        strlcpy(settings.wifiNetworks[i].ssid,
                config.wifiNetworks[i].first.c_str(),
                sizeof(settings.wifiNetworks[i].ssid));
        strlcpy(settings.wifiNetworks[i].password,
                config.wifiNetworks[i].second.c_str(),
                sizeof(settings.wifiNetworks[i].password));
        settings.wifiNetworkCount++;
    }
}
}  // namespace

const char* StorageManager::_segmentFormatText(uint8_t format) const {
    switch (format) {
        case SPOOL_SEGMENT_BIN_V2: return "bin_v2";
        case SPOOL_SEGMENT_JSONL:
        default:                   return "jsonl";
    }
}

// PATH_SPOOL / PATH_SPOOL_INDEX moved to SpoolPaths.h (file-scope inline
// constexpr; same names so the 13 existing call sites in this file resolve
// unchanged).
static constexpr const char* PATH_SPOOL_BAD       = "/spool_bad";
static constexpr const char* PATH_SPOOL_BAD_LOGS  = "/spool_bad/logs";
static constexpr const char* PATH_SPOOL_BAD_META  = "/spool_bad/meta";
// STORAGE_MAINT_LOG_MAGIC / STORAGE_MAINT_LOG_VERSION moved to
// MaintenanceContinuity.h (LOG_MAGIC / LOG_VERSION in the namespace).
static constexpr size_t   SPOOL_SEGMENT_TARGET_BYTES             = 48UL * 1024UL;
static constexpr size_t   SPOOL_SEGMENT_PREROTATE_BYTES          = 40UL * 1024UL;
static constexpr size_t   SPOOL_SEGMENT_CAPTURE_HARD_BYTES       = 96UL * 1024UL;
static constexpr uint32_t SPOOL_ENRICH_PREFLIGHT_ROTATE_BYTES   = 64U * 1024U;
static constexpr uint32_t SPOOL_ENRICH_PREFLIGHT_ROTATE_RECORDS = 1024U;
static constexpr uint32_t SPOOL_ENRICH_PREFLIGHT_ROTATE_DELTAS  = 1024U;
static constexpr size_t   STORAGE_METADATA_RESERVE_BYTES         = 256UL * 1024UL;
static constexpr size_t   STORAGE_HARD_FLOOR_BYTES               = 64UL * 1024UL;
static constexpr size_t   ENRICH_DELTA_BUDGET_BYTES              = 96UL;
static constexpr size_t   ENRICH_BATCH_OVERHEAD_BYTES            = 4096UL;
static constexpr uint32_t STORAGE_MAINT_UI_MIN_FREE_INTERNAL       = 48UL * 1024UL;
static constexpr uint32_t STORAGE_MAINT_UI_MIN_LARGEST_BLOCK       = 20UL * 1024UL;
// NimBLE stays initialized after phone work because full deinit is unsafe on
// this board. Bounded repair therefore must run at the observed post-BLE floor
// (~14KB free, ~7KB largest) instead of waiting for pre-BLE heap levels.
static constexpr uint32_t STORAGE_MAINT_SUMMARY_MIN_FREE_INTERNAL  = 48UL * 1024UL;
static constexpr uint32_t STORAGE_MAINT_SUMMARY_MIN_LARGEST_BLOCK  = 24UL * 1024UL;
static constexpr uint32_t STORAGE_SPOOL_ENRICH_EXACT_MIN_FREE_INTERNAL  = 48UL * 1024UL;
static constexpr uint32_t STORAGE_SPOOL_ENRICH_EXACT_MIN_LARGEST_BLOCK  = 24UL * 1024UL;
static constexpr uint32_t STORAGE_MAINT_FS_AUDIT_MIN_FREE_INTERNAL = 64UL * 1024UL;
static constexpr uint32_t STORAGE_MAINT_FS_AUDIT_MIN_LARGEST_BLOCK = 24UL * 1024UL;
static constexpr uint32_t STORAGE_MAINT_REPAIR_MIN_FREE_INTERNAL   = 8UL * 1024UL;
static constexpr uint32_t STORAGE_MAINT_REPAIR_MIN_LARGEST_BLOCK   = 4UL * 1024UL;
static constexpr uint32_t STORAGE_MAINT_RECOUNT_MIN_FREE_INTERNAL  = 12UL * 1024UL;
static constexpr uint32_t STORAGE_MAINT_RECOUNT_MIN_LARGEST_BLOCK  = 6UL * 1024UL;
static constexpr uint32_t STORAGE_UPLOAD_SMALL_WINDOW_RECORDS      = 16UL;
static constexpr uint32_t STORAGE_UPLOAD_SMALL_MIN_FREE_INTERNAL   = 16UL * 1024UL;
static constexpr uint32_t STORAGE_UPLOAD_SMALL_MIN_LARGEST_BLOCK   = 8UL * 1024UL;
static constexpr uint32_t STORAGE_BOOT_ZERO_PENDING_RETAINED_RECORDS = 512UL;
static constexpr uint32_t STORAGE_MAINT_REBUILD_MIN_FREE_INTERNAL  = 160UL * 1024UL;
static constexpr uint32_t STORAGE_MAINT_REBUILD_MIN_LARGEST_BLOCK  = 64UL * 1024UL;
static constexpr uint32_t STORAGE_MAINT_HEAP_RETRY_MS              = 30000UL;

// PATH_STORE_CONFIG_DIR, PATH_STORE_VAULT_DIR, PATH_STORE_KNOWN_LOCATIONS,
// PATH_STORE_LEGACY_KNOWN_LOCATIONS, PATH_LEGACY_BADUSB_DIR, and
// PATH_LEGACY_BADUSB_INDEX now live in data/Schema.h so the extracted
// KnownLocationsStore / BadUsbVault units can share them.
static constexpr const char* PATH_LEGACY_MQTT_QUEUE = "/mqtt_queue";
static constexpr const char* PATH_LEGACY_PMKID_DIR = "/pmkid";
static constexpr const char* PATH_VOLATILE_VAULT_DIR = "/vault";
static constexpr size_t SESSION_LOG_MAX_LINES = 128;

// _spoolCorruptionReasonText moved to SpoolRepairTypes.h as inline
// spoolCorruptionReasonText. Call sites below use the unqualified name.

static String _spoolQuarantineLogPath(uint32_t segmentId, uint8_t format) {
    char buf[80];
    const char* ext = (format == SPOOL_SEGMENT_BIN_V2) ? ".bin" : ".jsonl";
    snprintf(buf, sizeof(buf), "%s/seg_%06lu%s",
             PATH_SPOOL_BAD_LOGS,
             static_cast<unsigned long>(segmentId),
             ext);
    return String(buf);
}

static String _spoolQuarantineMetaPath(uint32_t segmentId) {
    char buf[80];
    snprintf(buf, sizeof(buf), "%s/seg_%06lu.meta.json",
             PATH_SPOOL_BAD_META,
             static_cast<unsigned long>(segmentId));
    return String(buf);
}

bool StorageManager::begin() {
    auto bootStep = [](uint32_t step, const char* label) {
#if BOOT_SEQUENCE_VERBOSE_ACTIVE
        Serial.printf("[STORAGE_BOOT] step=%lu %s heapFree=%lu largest=%lu\r\n",
                      static_cast<unsigned long>(step),
                      label ? label : "-",
                      static_cast<unsigned long>(
                          heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
                      static_cast<unsigned long>(
                          heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM)));
#endif
        crashCheckpointStep(CrashPhase::STORAGE_BOOT, 0, step, true);
    };

    bootStep(100, "begin_entry");
    if (!LittleFS.begin(false)) {
        // Only a partition that has never been written gets formatted here;
        // see _storagePartitionLooksBlank().
        if (_storagePartitionLooksBlank()) {
            DLOG_WARN("STORAGE",
                      "LittleFS partition is blank; formatting once");
#if BOOT_SEQUENCE_VERBOSE_ACTIVE
            Serial.printf("[STORAGE_BOOT] blank partition; formatting\n");
#endif
            if (!LittleFS.begin(true)) {
                DLOG_ERROR("STORAGE", "LittleFS format failed");
                return false;
            }
            DLOG_INFO("STORAGE",
                      "LittleFS formatted total=%luB",
                      static_cast<unsigned long>(LittleFS.totalBytes()));
        } else {
            DLOG_ERROR("STORAGE",
                       "LittleFS mount failed and partition is not blank; "
                       "refusing to auto-format");
#if BOOT_SEQUENCE_VERBOSE_ACTIVE
            Serial.printf("[STORAGE_BOOT] LittleFS mount failed; auto-format disabled\n");
#endif
            return false;
        }
    }
    bootStep(110, "littlefs_mounted");
    DLOG_INFO("STORAGE", "LittleFS mounted total=%luB used=%luB",
              static_cast<unsigned long>(LittleFS.totalBytes()),
              static_cast<unsigned long>(LittleFS.usedBytes()));

    _ensureDir("/config");
    _ensureDir(PATH_STORE_VAULT_DIR);
    bootStep(120, "base_dirs_ready");

    if (!_applyOneShotVaultReset()) {
        DLOG_WARN("STORAGE", "One-shot vault reset failed");
    }
    bootStep(125, "vault_reset_checked");

    if (!_applyOneShotNonVaultReset()) {
        DLOG_WARN("STORAGE", "One-shot non-vault reset failed");
    }
    bootStep(130, "one_shot_reset_checked");

    _ensureDir(PATH_LOGS);
    _ensureDir(PATH_EVENTS);
    _ensureDir(PATH_SPOOL);
    _ensureDir(PATH_EXPORTS);
    _ensureDir(PATH_PMKID_DIR);
    bootStep(140, "runtime_dirs_ready");

    // Quarantine dirs survive wipes intentionally — they hold forensic copies
    // of unrecoverable segments and are never touched by wipeNonVaultStorage().
    _ensureDir(PATH_SPOOL_BAD);
    _ensureDir(PATH_SPOOL_BAD_LOGS);
    _ensureDir(PATH_SPOOL_BAD_META);
    bootStep(150, "quarantine_dirs_ready");

    if (!_appendMutex) {
        _appendMutex = xSemaphoreCreateMutex();
        if (!_appendMutex) {
            DLOG_ERROR("STORAGE", "Storage append mutex create failed");
            return false;
        }
    }
    bootStep(160, "append_mutex_ready");

    if (!_ensureSpoolReady()) {
        DLOG_ERROR("STORAGE", "Spool init failed");
        return false;
    }
    bootStep(200, "spool_ready");

    (void)_loadMaintenanceContinuityLog();
    if (_maintenanceContinuityCurrent()) {
        _lastFsAuditCompletedMs = millis();
        DLOG_INFO("STORAGE",
                  "Boot continuity checkpoint accepted gen=%lu pending=%lu",
                  static_cast<unsigned long>(_maintenanceContinuity.storageGeneration),
                  static_cast<unsigned long>(_maintenanceContinuity.pendingTotal));
    }

#if STORAGE_FAST_BOOT_DEFER_SPOOL_REPAIR
    SpoolBootAuditResult bootAudit;
    bootStep(210, "boot_audit_start");
    if (!_auditSpoolBoot(bootAudit)) {
        _pendingEventCount = _spoolIndex.pendingTotal;
        _spoolIndex.pendingTotal = _pendingEventCount;
        _repairRequested = true;
        _spoolAuditRepairRequired = true;
        requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT, "boot_fast_audit_failed");
        requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED, "boot_fast_audit_failed");
        _setCounterTrustState(STORAGE_COUNTER_REPAIR_REQUIRED,
                              "boot_fast_audit_failed");
        DLOG_WARN("STORAGE",
                  "Fast boot: repair deferred pending=%lu active=%lu nextEventId=%lu flags=%s",
                  static_cast<unsigned long>(_pendingEventCount),
                  static_cast<unsigned long>(_spoolIndex.activeSegmentId),
                  static_cast<unsigned long>(_nextEventId),
                  maintenanceFlagsText());

        static constexpr uint32_t BOOT_SMALL_REPAIR_SEGMENT_LIMIT = 20U;
        static constexpr uint32_t BOOT_SMALL_REPAIR_RECORD_LIMIT = 2048U;
        const bool smallBootRepair =
            bootAudit.auditedSegments > 0U &&
            bootAudit.auditedSegments <= BOOT_SMALL_REPAIR_SEGMENT_LIMIT &&
            bootAudit.headerRecords > 0U &&
            bootAudit.headerRecords <= BOOT_SMALL_REPAIR_RECORD_LIMIT;
        if (smallBootRepair) {
            SpoolAuditResult smallRepair;
            DLOG_WARN("STORAGE",
                      "Fast boot: small backlog exact repair start segs=%lu records=%lu",
                      static_cast<unsigned long>(bootAudit.auditedSegments),
                      static_cast<unsigned long>(bootAudit.headerRecords));
            const bool repairOk =
                _auditAndRepairSpool("boot_small_repair", true, &smallRepair);
            if (repairOk && !smallRepair.hadFatalSegmentError) {
                _repairRequested = false;
                _spoolAuditRepairRequired = false;
                _pendingCountDirty = false;
                _spoolSummaryRebuildPending = _hasInvalidSpoolSummaries();
                _clearMaintenanceFlags(STORAGE_MAINT_SEGMENT_AUDIT |
                                       STORAGE_MAINT_COUNTER_UNTRUSTED |
                                       STORAGE_MAINT_SNAPSHOT_LAGGED);
                if (_spoolSummaryRebuildPending) {
                    requestMaintenance(STORAGE_MAINT_DIRTY_SUMMARY,
                                       "boot_small_repair");
                    _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                          "boot_small_repair_summary");
                } else {
                    _setCounterTrustState(STORAGE_COUNTER_TRUSTED,
                                          "boot_small_repair");
                }
                DLOG_WARN("STORAGE",
                          "Fast boot: small backlog exact repair complete pending=%lu nextEventId=%lu records=%lu",
                          static_cast<unsigned long>(_pendingEventCount),
                          static_cast<unsigned long>(_nextEventId),
                          static_cast<unsigned long>(smallRepair.validEventRecords));
            } else {
                DLOG_WARN("STORAGE",
                          "Fast boot: small backlog exact repair deferred ok=%d fatal=%d",
                          repairOk ? 1 : 0,
                          smallRepair.hadFatalSegmentError ? 1 : 0);
            }
        }
    } else {
        _spoolAuditRepairRequired = false;
        _pendingEventCount = _spoolIndex.pendingTotal;
        _spoolIndex.pendingTotal = _pendingEventCount;

        if (bootAudit.snapshotLagged) {
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                  "trusted_snapshot_lagged");
            requestMaintenance(STORAGE_MAINT_SNAPSHOT_LAGGED, "boot_audit");
        } else {
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED, "boot_fast_ready");
        }

        if (isPendingEventCountAuthoritative()) {
            DLOG_INFO("STORAGE",
                      "Fast boot: repair not required pending=%lu summary=%d dirty=%d",
                      static_cast<unsigned long>(_pendingEventCount),
                      _spoolSummaryRebuildPending ? 1 : 0,
                      _pendingCountDirty ? 1 : 0);
        } else {
            DLOG_INFO("STORAGE",
                      "Fast boot: repair not required pending=unknown summary=%d dirty=%d",
                      _spoolSummaryRebuildPending ? 1 : 0,
                      _pendingCountDirty ? 1 : 0);
        }
    }
    bootStep(220, "boot_audit_done");
#else
    SpoolAuditResult audit;
    if (!_auditAndRepairSpool("boot", true, &audit)) {
        DLOG_ERROR("STORAGE", "Boot spool audit failed");
        return false;
    }

    // The boot audit uses the fast binary-meta scanner which cannot populate
    // priority/lane/timestamp stats, so it leaves all BIN_V2 segments with
    // summaryValid=false.  Rebuild here while the radio arbiter hasn't started
    // yet; after hardware init, the maintenance owner is the only non-capture
    // path allowed to rebuild summaries.
    if (_hasInvalidSpoolSummaries()) {
        DLOG_INFO("STORAGE", "Post-audit summary rebuild");
        _rebuildInvalidSegmentSummaries(false);
    }

    const uint32_t bootPending = recountPendingFromSpool();
    if (bootPending != audit.rebuiltPendingTotal) {
        DLOG_WARN("STORAGE",
                  "Boot audit pending mismatch audit=%lu recount=%lu",
                  static_cast<unsigned long>(audit.rebuiltPendingTotal),
                  static_cast<unsigned long>(bootPending));
    }

    _checkSpoolInvariants("boot_post_audit", false);
    _setCounterTrustState(STORAGE_COUNTER_TRUSTED, "boot_audited");
#endif

    uint32_t segmentPendingAtBoot = 0;
    uint32_t storedEventsAtBoot = 0;
    for (const auto& seg : _spoolIndex.segments) {
        segmentPendingAtBoot +=
            seg.pendingUploadMissionCount + seg.pendingUploadNoiseCount;
        storedEventsAtBoot += seg.eventCount;
    }
    const bool uploadWatermarksPresent = !_spoolIndex.uploadedWatermarks.empty();
    if (_pendingEventCount == 0 &&
        storedEventsAtBoot >= STORAGE_BOOT_ZERO_PENDING_RETAINED_RECORDS &&
        uploadWatermarksPresent) {
        DLOG_WARN("STORAGE",
                  "Boot zero pending with retained spool; repair required stored=%lu watermarks=%u",
                  static_cast<unsigned long>(storedEventsAtBoot),
                  static_cast<unsigned>(_spoolIndex.uploadedWatermarks.size()));
        _pendingCountDirty = true;
        _spoolAuditRepairRequired = true;
        requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED,
                           "boot_zero_pending_retained_spool");
        requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                           "boot_zero_pending_retained_spool");
        _setCounterTrustState(STORAGE_COUNTER_REPAIR_REQUIRED,
                              "boot_zero_pending_retained_spool");
    }
    if (segmentPendingAtBoot > _pendingEventCount && uploadWatermarksPresent) {
        DLOG_WARN("STORAGE",
                  "Boot pending split ahead of aggregate; keeping aggregate pending=%lu split=%lu watermarks=%u",
                  static_cast<unsigned long>(_pendingEventCount),
                  static_cast<unsigned long>(segmentPendingAtBoot),
                  static_cast<unsigned>(_spoolIndex.uploadedWatermarks.size()));
    } else if (segmentPendingAtBoot > _pendingEventCount) {
        DLOG_WARN("STORAGE",
                  "Boot pending aggregate behind segment snapshot pending=%lu split=%lu",
                  static_cast<unsigned long>(_pendingEventCount),
                  static_cast<unsigned long>(segmentPendingAtBoot));
        _pendingEventCount = segmentPendingAtBoot;
        _spoolIndex.pendingTotal = segmentPendingAtBoot;
        _pendingCountDirty = true;
        requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED,
                           "boot_pending_split_reconcile");
        if (_counterTrustState == STORAGE_COUNTER_TRUSTED) {
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                  "boot_pending_split_reconcile");
        }
    }

    // The pending counter is a RAM running total whose flush to meta.json is
    // deferred to idle windows, and the per-segment split in the spool index is
    // flushed on the same deferred path. An unclean reset therefore discards
    // every delta since the last flush, and leaves BOTH snapshots stale by the
    // same amount - so the split-vs-aggregate comparison above cannot detect
    // it. That is how the 2026-08-18 panics drifted the counter (an exact audit
    // reconciled 4973 -> 5306). Force a real recount after an unclean reset
    // instead of trusting either snapshot.
    {
        const esp_reset_reason_t bootReset = esp_reset_reason();
        const bool uncleanReset = (bootReset == ESP_RST_PANIC)    ||
                                  (bootReset == ESP_RST_TASK_WDT) ||
                                  (bootReset == ESP_RST_INT_WDT)  ||
                                  (bootReset == ESP_RST_WDT)      ||
                                  (bootReset == ESP_RST_BROWNOUT);
        if (uncleanReset && storedEventsAtBoot > 0) {
            DLOG_WARN("STORAGE",
                      "Unclean reset (%d); pending counter untrusted pending=%lu stored=%lu",
                      static_cast<int>(bootReset),
                      static_cast<unsigned long>(_pendingEventCount),
                      static_cast<unsigned long>(storedEventsAtBoot));
            _pendingCountDirty = true;
            _spoolAuditRepairRequired = true;
            requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED,
                               "unclean_reset_counter_recount");
            requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                               "unclean_reset_counter_recount");
            _setCounterTrustState(STORAGE_COUNTER_DEGRADED,
                                  "unclean_reset_counter_recount");
        }
    }

    _releaseUploadIndexMemory("boot_skip_load");

    if (!loadConfig()) {
        DLOG_WARN("STORAGE", "Settings unavailable, using defaults");
    }

    _ready = true;
    _resetBinaryUnsupportedAudit();

    const uint32_t freeInternal =
        heap_caps_get_free_size(SPECTRE_CAP_DRAM);
    const uint32_t largestInternal =
        heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM);
    const uint32_t pendingAtBoot = _pendingEventCount;

    const bool bootHeapPressure =
        pendingAtBoot >= 10000UL ||
        freeInternal < (128UL * 1024UL) ||
        largestInternal < (48UL * 1024UL);

    if (bootHeapPressure) {
        DLOG_WARN("STORAGE",
                  "Boot heap pressure: deferring post-boot refresh pending=%lu freeInternal=%lu largestInternal=%lu",
                  static_cast<unsigned long>(pendingAtBoot),
                  static_cast<unsigned long>(freeInternal),
                  static_cast<unsigned long>(largestInternal));

        _storageUiRefreshPending = true;
        _storageUiRefreshDirtySinceMs = millis();
        return true;
    }

    _refreshFsStats();
    const String usedString = _cachedUsedString;
    const size_t freeBytes = _cachedFreeBytes;
    const size_t totalBytes = _cachedTotalBytes;
    const int usedPct = _cachedUsedPct;

    DLOG_INFO("STORAGE", "Ready. Used=%s Free=%uKB / %uKB",
              usedString.c_str(),
              static_cast<unsigned>(freeBytes / 1024),
              static_cast<unsigned>(totalBytes / 1024));

    if (usedPct > 80) {
        BUS.publish(EVT_STORAGE_NEARLY_FULL);
    }

    refreshStorageUiState();

#if !STORAGE_FAST_BOOT_DEFER_SPOOL_REPAIR
    _cleanupLegacyUploadSidecars();
    _cleanupLegacyEnrichSidecars();
    _cleanupLegacyRawSessionFiles();
#endif

    _logSpoolDiagnostics("ensure_ready");
    return true;
}

bool StorageManager::loadConfig() {
    if (SETTINGS.isReady()) {
        _copySettingsToConfig(SETTINGS.snapshot(), _config);
        DLOG_INFO("STORAGE", "Config synced from settings for: %s", _config.name.c_str());
        return true;
    }
    _initDefaultConfig();
    return false;
}

bool StorageManager::saveConfig() {
    if (!SETTINGS.isReady()) {
        DLOG_WARN("STORAGE", "Config save skipped: settings unavailable");
        return false;
    }

    RuntimeSettings settings = SETTINGS.snapshot();
    _copyConfigToSettings(_config, settings);
    if (!SETTINGS.apply(settings)) {
        DLOG_WARN("STORAGE", "Settings apply failed during config save");
        return false;
    }

    DLOG_INFO("STORAGE", "Config saved to settings");
    return true;
}

bool StorageManager::addWifiNetwork(const String& ssid, const String& password) {
    loadConfig();
    for (auto& net : _config.wifiNetworks) {
        if (net.first == ssid) {
            net.second = password;
            return saveConfig();
        }
    }
    _config.wifiNetworks.push_back({ssid, password});
    return saveConfig();
}

bool StorageManager::removeWifiNetwork(const String& ssid) {
    loadConfig();
    auto& nets = _config.wifiNetworks;
    nets.erase(std::remove_if(nets.begin(), nets.end(),
        [&ssid](const std::pair<String,String>& n) {
            return n.first == ssid;
        }), nets.end());
    return saveConfig();
}

std::vector<std::pair<String,String>> StorageManager::getWifiNetworks() {
    if (SETTINGS.isReady()) {
        _copySettingsToConfig(SETTINGS.snapshot(), _config);
    }
    return _config.wifiNetworks;
}

bool StorageManager::logLoraPacket(int address, const String& payload,
                                    const String& payloadHex, int rssi,
                                    int snr, long freq) {
    if (!_ready) return false;

    String path = String(PATH_LOGS) + "/lora_" + _today() + ".json";

    JsonDocument entry;
    entry[F_TIMESTAMP]   = millis();
    {
        char tsIso[24] = {};
        TIME_SVC.formatIsoForMillis(millis(), tsIso, sizeof(tsIso));
        entry[F_TIMESTAMP_ISO] = tsIso;
    }
    entry[F_SESSION]     = SESS.getId();
    entry[F_ADDRESS]     = address;
    entry[F_RSSI]        = rssi;
    entry[F_SNR]         = snr;
    entry[F_FREQUENCY]   = freq;
    entry[F_PAYLOAD]     = payload;
    entry[F_PAYLOAD_HEX] = payloadHex;

    GPSFix gps = SESS.getGPS();
    entry[F_GPS][F_LAT]      = gps.lat;
    entry[F_GPS][F_LON]      = gps.lon;
    entry[F_GPS][F_ACCURACY] = gps.accuracy;
    entry[F_GPS][F_VALID]    = gps.valid;

    if (!_appendJsonLine(path, entry)) return false;

    SESS.incrementLoraPackets();

    refreshStorageUiState();

    return true;
}

bool StorageManager::logWifiScan(const String& ssid, const String& bssid,
                                  int rssi, int channel,
                                  const String& encryption) {
    if (!_ready) return false;

    String path = String(PATH_LOGS) + "/wifi_" + _today() + ".json";

    JsonDocument entry;
    entry[F_TIMESTAMP]  = millis();
    {
        char tsIso[24] = {};
        TIME_SVC.formatIsoForMillis(millis(), tsIso, sizeof(tsIso));
        entry[F_TIMESTAMP_ISO] = tsIso;
    }
    entry[F_SESSION]    = SESS.getId();
    entry[F_SSID]       = ssid;
    entry[F_BSSID]      = bssid;
    entry[F_RSSI]       = rssi;
    entry[F_CHANNEL]    = channel;
    entry[F_ENCRYPTION] = encryption;

    GPSFix gps = SESS.getGPS();
    entry[F_GPS][F_LAT]      = gps.lat;
    entry[F_GPS][F_LON]      = gps.lon;
    entry[F_GPS][F_ACCURACY] = gps.accuracy;
    entry[F_GPS][F_VALID]    = gps.valid;

    if (!_appendJsonLine(path, entry)) return false;

    SESS.incrementWifiScans();
    refreshStorageUiState();
    return true;
}

bool StorageManager::logProbe(const String& mac, const String& ssid, int rssi) {
    if (!_ready) return false;

    String path = String(PATH_LOGS) + "/probe_" + _today() + ".json";

    JsonDocument entry;
    entry[F_TIMESTAMP] = millis();
    {
        char tsIso[24] = {};
        TIME_SVC.formatIsoForMillis(millis(), tsIso, sizeof(tsIso));
        entry[F_TIMESTAMP_ISO] = tsIso;
    }
    entry[F_SESSION]   = SESS.getId();
    entry["mac"]       = mac;
    entry[F_SSID]      = ssid;
    entry[F_RSSI]      = rssi;

    GPSFix gps = SESS.getGPS();
    entry[F_GPS][F_LAT]      = gps.lat;
    entry[F_GPS][F_LON]      = gps.lon;
    entry[F_GPS][F_ACCURACY] = gps.accuracy;
    entry[F_GPS][F_VALID]    = gps.valid;

    if (!_appendJsonLine(path, entry)) return false;

    SESS.incrementProbes();
    refreshStorageUiState();
    return true;
}

// FNV-1a helpers + _makeDedupKey + _trimDedupWindows moved to DedupFilter.cpp.

StoragePriority StorageManager::_priorityForEventType(const char* type,
                                                      JsonObjectConst payload) const {
    return static_cast<StoragePriority>(
        RAMSpool::classify(type, payload).priority);
}

StorageLane StorageManager::_laneForEventType(const char* type,
                                              JsonObjectConst payload) const {
    return RAMSpool::classify(type, payload).lane == RAMSpool::LANE_MISSION
        ? STORAGE_LANE_MISSION
        : STORAGE_LANE_NOISE;
}

StorageLane StorageManager::_eventRecordLane(JsonObjectConst doc) const {
    return RAMSpool::classify(doc["type"] | "", doc["event_type"] | "").lane ==
        RAMSpool::LANE_MISSION
            ? STORAGE_LANE_MISSION
            : STORAGE_LANE_NOISE;
}

StoragePriority StorageManager::_eventRecordPriority(JsonObjectConst doc) const {
    return static_cast<StoragePriority>(
        RAMSpool::classify(doc["type"] | "", doc["event_type"] | "").priority);
}

uint16_t StorageManager::_eventRecordValueScore(JsonObjectConst doc) const {
    return static_cast<uint16_t>(
        RAMSpool::classify(doc["type"] | "", doc).valueScore);
}

bool StorageManager::_eventRecordWantsEnrichment(JsonObjectConst doc) const {
    return RAMSpool::classify(doc["type"] | "", doc).enrichEligible;
}

bool StorageManager::_eventRecordPendingEnrichment(JsonObjectConst doc) const {
    JsonVariantConst marker = doc[F_ENRICH_STATE];
    if (!marker.isNull()) {
        return (marker | static_cast<uint8_t>(STORAGE_ENRICH_NOT_ELIGIBLE)) ==
               static_cast<uint8_t>(STORAGE_ENRICH_PENDING);
    }

    // Legacy backlog records predate the marker. Classify them with the same
    // cheap capture rules used at write time so old records stay aligned.
    return _eventRecordWantsEnrichment(doc);
}

const char* StorageManager::_laneText(StorageLane lane) const {
    switch (lane) {
        case STORAGE_LANE_MISSION: return "mission";
        case STORAGE_LANE_NOISE:
        default:                   return "noise";
    }
}

// _trimDedupWindows moved to DedupFilter::trimWindows.

// Classifier impls moved to DedupFilter.cpp. Wrappers below delegate to the
// member filter, then apply any counter delta + UI refresh side effect.

void StorageManager::_applyDedupCounterEffect(const DedupCounterEffect& effect,
                                              bool deferUiRefresh) {
    if (!effect.apply) {
        return;
    }
    _applyCounterDelta(effect.reason,
                       0,
                       0,
                       effect.droppedDelta,
                       effect.suppressedDelta,
                       effect.lane,
                       effect.priority);
    if (deferUiRefresh) {
        _queueStorageUiRefresh(true);
    }
}

bool StorageManager::_shouldSuppressDuplicate(const char* type,
                                              JsonObjectConst payload,
                                              StoragePriority priority,
                                              bool deferUiRefresh) {
    const DedupVerdict v = _dedupFilter.classifyDedupCandidate(type, payload, priority);
    _applyDedupCounterEffect(v.counter, deferUiRefresh);
    return v.suppress;
}

bool StorageManager::shouldAcceptHandshakeFrame(const char* apMac,
                                                const char* staMac,
                                                const char* ssid,
                                                uint8_t messageNumber,
                                                bool deferUiRefresh) {
    const HandshakeVerdict v =
        _dedupFilter.classifyHandshakeFrame(apMac, staMac, ssid, messageNumber);
    _applyDedupCounterEffect(v.counter, deferUiRefresh);
    return v.accept;
}

uint32_t StorageManager::appendEvent(const char* type, const char* payloadJson,
                                     const char* sessionIdOverride) {
    return appendEventDetailed(type, payloadJson, sessionIdOverride).eventId;
}

uint32_t StorageManager::appendEvent(const char* type,
                                     JsonObjectConst payload,
                                     const char* sessionIdOverride) {
    return appendEventDetailed(type, payload, sessionIdOverride).eventId;
}

AppendEventResult StorageManager::appendEventDetailed(const char* type,
                                                      const char* payloadJson,
                                                      const char* sessionIdOverride,
                                                      bool deferMetadataFlush,
                                                      bool deferUiRefresh) {
    if (!type || !type[0]) {
        return {APPEND_FAILED_INVALID, 0};
    }

    if (payloadJson && payloadJson[0]) {
        JsonDocument payloadDoc;
        DeserializationError err = deserializeJson(payloadDoc, payloadJson);
        if (err || !payloadDoc.is<JsonObject>()) {
            DLOG_WARN("STORAGE", "appendEvent payload parse error");
            return {APPEND_FAILED_PARSE, 0};
        }
        return appendEventDetailed(type,
                                   payloadDoc.as<JsonObjectConst>(),
                                   sessionIdOverride,
                                   deferMetadataFlush,
                                   deferUiRefresh);
    }

    JsonDocument emptyPayload;
    return appendEventDetailed(type,
                               emptyPayload.as<JsonObjectConst>(),
                               sessionIdOverride,
                               deferMetadataFlush,
                               deferUiRefresh);
}

AppendEventResult StorageManager::appendEventDetailed(const char* type,
                                                      JsonObjectConst payload,
                                                      const char* sessionIdOverride,
                                                      bool deferMetadataFlush,
                                                      bool deferUiRefresh) {
    if (_isSyncAppendHotPathViolationType(type)) {
        DLOG_WARN("STORAGE", "sync_append_hotpath_violation type=%s", type);
    }

    return _appendEventDetailedInternal(type,
                                        payload,
                                        sessionIdOverride,
                                        nullptr,
                                        deferMetadataFlush,
                                        deferUiRefresh,
                                        false,
                                        nullptr);
}

AppendEventResult StorageManager::_appendEventDetailedInternal(
    const char* type,
    JsonObjectConst payload,
    const char* sessionIdOverride,
    const RAMSpool::CaptureClassification* classification,
    bool deferMetadataFlush,
    bool deferUiRefresh,
    bool deferPressureRefresh,
    QueuedAppendTiming* timing) {
    const uint32_t totalStartMs = timing ? millis() : 0U;
    if (!_ready) {
        return {APPEND_FAILED_NOT_READY, 0};
    }
    if (!type || !type[0]) {
        return {APPEND_FAILED_INVALID, 0};
    }

    String sessionId =
        (sessionIdOverride && sessionIdOverride[0]) ?
            String(sessionIdOverride) : SESS.getId();
    if (!sessionId.length()) {
        DLOG_WARN("STORAGE", "appendEvent failed: no active session");
        return {APPEND_FAILED_NO_SESSION, 0};
    }

    if (!deferPressureRefresh) {
        updateStoragePressure();
    }

    const uint32_t classifyStartMs = timing ? millis() : 0U;
    const RAMSpool::CaptureClassification cls =
        classification ? *classification : RAMSpool::classify(type, payload);
    if (timing) {
        timing->classifyMs += millis() - classifyStartMs;
    }

    const uint32_t buildStartMs = timing ? millis() : 0U;
    StoragePriority priority = static_cast<StoragePriority>(cls.priority);
    const StorageLane lane =
        (cls.lane == RAMSpool::LANE_MISSION) ? STORAGE_LANE_MISSION
                                             : STORAGE_LANE_NOISE;
    auto markUiRefresh = [&]() {
        if (deferUiRefresh) {
            _queueStorageUiRefresh(true);
        } else {
            refreshStorageUiState();
        }
    };

#if DEDUP_PROFILE == DEDUP_PROFILE_STRICT
    // Long-mission profile: drop only un-enrichable records (P3 — records
    // with <2 unique fields). P2 still saved: if it can be enriched, keep it.
    if (priority > STORAGE_PRIO_P2) {
        _applyCounterDelta("strict_profile_drop",
                           0,
                           0,
                           1,
                           0,
                           lane,
                           priority);
        if (deferUiRefresh) {
            _queueStorageUiRefresh(true);
        } else {
            _queueStorageUiRefresh(false);
        }
        if (timing) {
            timing->buildDocMs = millis() - buildStartMs;
            timing->totalMs = millis() - totalStartMs;
        }
        return {APPEND_DROPPED_POLICY, 0};
    }
#endif

    if (!shouldStoreByPriority(priority)) {
        _applyCounterDelta("policy_drop",
                           0,
                           0,
                           1,
                           0,
                           lane,
                           priority);
        if (deferUiRefresh) {
            _queueStorageUiRefresh(true);
        } else {
            _queueStorageUiRefresh(false);
        }
        if (timing) {
            timing->buildDocMs = millis() - buildStartMs;
            timing->totalMs = millis() - totalStartMs;
        }
        return {APPEND_DROPPED_POLICY, 0};
    }

    uint32_t eventId = 0;
    {
        ScopedSemaphoreLock appendLock(_appendMutex);

        const char* eventType = type ? type : "event";
        const char* subType = payload["event_type"] | "";

        if (strcmp(eventType, "event") == 0 &&
            strcmp(subType, "handshake") == 0) {
            const char* apMac = payload["ap"] | payload["bssid"] | "";
            const char* staMac = payload["sta"] | payload["client"] | "";
            const char* ssid = payload["ssid"] | "";
            const uint8_t messageNumber =
                static_cast<uint8_t>(payload["msg"] | payload["message"] | 0);

            if (!shouldAcceptHandshakeFrame(apMac,
                                            staMac,
                                            ssid,
                                            messageNumber,
                                            deferUiRefresh)) {
                if (timing) {
                    timing->buildDocMs = millis() - buildStartMs;
                    timing->totalMs = millis() - totalStartMs;
                }
                return {APPEND_SUPPRESSED_DUPLICATE, 0};
            }
        } else if (_shouldSuppressDuplicate(eventType,
                                            payload,
                                            priority,
                                            deferUiRefresh)) {
            if (timing) {
                timing->buildDocMs = millis() - buildStartMs;
                timing->totalMs = millis() - totalStartMs;
            }
            return {APPEND_SUPPRESSED_DUPLICATE, 0};
        }

        const uint32_t nowMs = millis();
        eventId = _nextEventId;

        _rememberSession(sessionId);
        JsonDocument eventDoc;
        JsonObject event = eventDoc.to<JsonObject>();
        event["id"] = eventId;
        event["ts"] = nowMs;
        event["prio"] = static_cast<uint8_t>(priority);
        event["lane"] = static_cast<uint8_t>(lane);
        event["lane_name"] = _laneText(lane);
        {
            char tsIso[24] = {};
            TIME_SVC.formatIsoForMillis(nowMs, tsIso, sizeof(tsIso));
            event[F_TIMESTAMP_ISO] = tsIso;
        }
        event["type"] = type;
        event[F_SESSION] = sessionId;
        event["status"] = EVT_RAW;

        for (JsonPairConst kv : payload) {
            const char* key = kv.key().c_str();
            if (!_isReservedEventKey(key)) {
                event[key].set(kv.value());
            }
        }

        _normalizeCapturedEvent(event, &cls);
        priority = static_cast<StoragePriority>(event["prio"] | static_cast<uint8_t>(STORAGE_PRIO_P3));

        if (timing) {
            timing->buildDocMs += millis() - buildStartMs;
        }

        const bool deferIndexPersist =
            deferMetadataFlush || _workerAppendBatchActive ||
            _shouldDeferWorkerMetadataFlush();
        if (!_appendSpoolRecord(eventDoc, nullptr, deferIndexPersist, timing)) {
            if (timing) timing->totalMs = millis() - totalStartMs;
            return {APPEND_FAILED_IO, 0};
        }

        const uint32_t counterStartMs = timing ? millis() : 0U;
        _nextEventId++;
        _spoolIndex.nextEventId = _nextEventId;
        _bumpStorageMetaGeneration();
        _eventCounterDirty = true;
        _eventCounterPendingWrites++;
        _applyCounterDelta("accepted_write",
                           1,
                           0,
                           0,
                           0,
                           lane,
                           priority);
        if (deferMetadataFlush) {
            _workerMetadataDirtyPending = true;
            if (_workerMetadataPendingWrites < UINT16_MAX) {
                _workerMetadataPendingWrites++;
            }
            if (_workerMetadataPendingWrites == 1) {
                _workerMetadataDirtySinceMs = nowMs;
            }
            if (_counterTrustState == CounterTrust::Trusted) {
                _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                      "append_metadata_deferred");
            }
        } else if (_shouldDeferWorkerMetadataFlush()) {
            _workerMetadataDirtyPending = true;
            if (_workerMetadataPendingWrites < UINT16_MAX) {
                _workerMetadataPendingWrites++;
            }
            if (_workerMetadataPendingWrites == 1) {
                _workerMetadataDirtySinceMs = nowMs;
            }
            if (_counterTrustState == CounterTrust::Trusted) {
                _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                      "append_metadata_deferred");
            }
        } else {
            const uint32_t appendWriteStart = millis();
            const bool counterOk = _persistEventCounter(false, "append_event");
            _logCaptureWriteAllowed(PATH_EVENT_COUNTER,
                                    "append_event",
                                    millis() - appendWriteStart,
                                    counterOk);
            if (!counterOk) {
                if (timing) timing->counterMs += millis() - counterStartMs;
                if (timing) timing->totalMs = millis() - totalStartMs;
                return {APPEND_FAILED_IO, 0};
            }
            const uint32_t metaWriteStart = millis();
            const bool metaOk = _persistEventMeta(false, "append_event");
            _logCaptureWriteAllowed(PATH_EVENT_META,
                                    "append_event",
                                    millis() - metaWriteStart,
                                    metaOk);
            if (!metaOk) {
                if (timing) timing->counterMs += millis() - counterStartMs;
                if (timing) timing->totalMs = millis() - totalStartMs;
                return {APPEND_FAILED_IO, 0};
            }
        }
        if (timing) {
            timing->counterMs += millis() - counterStartMs;
        }

        // A record becomes an Entity observation only after the spool append
        // and its authoritative counters have succeeded.
        ENTITY_MGR.observeStoredEvent(eventId, event);
    }

    const uint32_t uiStartMs = timing ? millis() : 0U;
    markUiRefresh();

    if (timing) {
        timing->uiMs += millis() - uiStartMs;
        timing->totalMs = millis() - totalStartMs;
    }

    return {APPEND_OK, eventId};
}

AppendEventResult StorageManager::appendQueuedRecord(const RAMSpool::EventSlot& slot,
                                                     QueuedAppendTiming* timing) {
    if (!_ready) {
        return {APPEND_FAILED_NOT_READY, 0};
    }
    const uint32_t totalStartMs = timing ? millis() : 0U;
    const RAMSpool::SlotKind slotKind =
        static_cast<RAMSpool::SlotKind>(slot.slotKind);
    if (slotKind == RAMSpool::SLOT_SHADOW) {
        if (timing) timing->totalMs = millis() - totalStartMs;
        return {APPEND_FAILED_INVALID, 0};
    }
    if (!slot.type[0]) {
        if (timing) timing->totalMs = millis() - totalStartMs;
        return {APPEND_FAILED_INVALID, 0};
    }

    const size_t payloadLen = static_cast<size_t>(slot.payloadLen);
    if (payloadLen == 0) {
        if (timing) timing->totalMs = millis() - totalStartMs;
        return {APPEND_FAILED_PARSE, 0};
    }

    JsonDocument payloadDoc;
    JsonObject payloadRoot = payloadDoc.to<JsonObject>();
    const uint32_t parseStartMs = timing ? millis() : 0U;
    const bool decoded = SpoolBin::decodeFieldMapV1(slot.payload,
                                                    payloadLen,
                                                    payloadRoot);
    if (timing) {
        timing->parseMs += millis() - parseStartMs;
    }
    if (!decoded) {
        DLOG_WARN("STORAGE", "appendQueuedRecord payload decode error type=%s",
                  slot.type);
        if (timing) {
            timing->totalMs = millis() - totalStartMs;
        }
        return {APPEND_FAILED_PARSE, 0};
    }

    JsonObjectConst payload = payloadDoc.as<JsonObjectConst>();
    const char* sessionOverride = payload["session_id"] | "";
    RAMSpool::CaptureClassification cls;
    cls.priority = static_cast<RAMSpool::EventPrio>(slot.provPriority);
    cls.lane = static_cast<RAMSpool::EventLane>(slot.provLane);
    cls.enrichEligible = slot.enrichEligible != 0;
    cls.valueScore = slot.valueScore;
    AppendEventResult result = _appendEventDetailedInternal(slot.type,
                                                            payload,
                                                            (sessionOverride && sessionOverride[0]) ?
                                                                sessionOverride : nullptr,
                                                            &cls,
                                                            true,
                                                            true,
                                                            true,
                                                            timing);
    if (timing) {
        timing->totalMs = millis() - totalStartMs;
    }
    return result;
}

bool StorageManager::getEventBatch(uint32_t sinceId, int maxCount,
                                   JsonDocument& out) {
    return getEventBatchForSession(SESS.getId().c_str(), sinceId, maxCount, out);
}

bool StorageManager::getEventBatchForSession(const char* sessionIdOverride,
                                             uint32_t sinceId,
                                             int maxCount,
                                             JsonDocument& out) {
    out.clear();

    if (!_ready || maxCount <= 0) return false;

    String sessionId =
        (sessionIdOverride && sessionIdOverride[0]) ?
            String(sessionIdOverride) : SESS.getId();
    if (!sessionId.length()) return true;

    return _getEventBatchForSessionFromSpool(sessionId, sinceId, maxCount, out);
}

bool StorageManager::getNextResolvedEventForSession(const char* sessionIdOverride,
                                                    uint32_t sinceId,
                                                    JsonDocument& out) {
    out.clear();

    if (!_ready) return false;

    String sessionId =
        (sessionIdOverride && sessionIdOverride[0]) ?
            String(sessionIdOverride) : SESS.getId();
    if (!sessionId.length()) return true;

    DLOG_DEBUG("STORAGE",
              "Next resolved event begin session=%s since=%lu",
              sessionId.c_str(),
              static_cast<unsigned long>(sinceId));

    const uint32_t watermark = _uploadedWatermarkForSession(sessionId);

    for (const auto& seg : _spoolIndex.segments) {
        if (seg.lastEventId != 0 && seg.lastEventId <= sinceId) {
            continue;
        }

        bool found = false;
        DLOG_DEBUG("STORAGE",
                  "Upload scan segment=%lu format=%u session=%s",
                  static_cast<unsigned long>(seg.segmentId),
                  static_cast<unsigned>(seg.format),
                  sessionId.c_str());

        const bool ok = _scanSegmentRecords(seg.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
                    return true;
                }

                if (!rec.sessionId.length() || rec.sessionId != sessionId) {
                    return true;
                }

                if (!rec.eventId || rec.eventId <= sinceId) {
                    return true;
                }

                DLOG_DEBUG("STORAGE",
                          "Upload scan match seg=%lu event=%lu",
                          static_cast<unsigned long>(seg.segmentId),
                          static_cast<unsigned long>(rec.eventId));

                out.set(rec.doc.as<JsonVariantConst>());
                if (rec.eventId <= watermark) {
                    out["uploaded_ts"] = millis();
                    out["status"] = EVT_UPLOADED;
                } else {
                    out["status"] = EVT_RAW;
                }

                const char* type = out["type"] | "";
                DLOG_DEBUG("STORAGE",
                          "Upload scan selected event=%lu type=%s",
                          static_cast<unsigned long>(rec.eventId),
                          type);
                found = true;
                return false;
            });

        if (!ok) {
            DLOG_WARN("STORAGE",
                      "Upload scan failed session=%s seg=%lu",
                      sessionId.c_str(),
                      static_cast<unsigned long>(seg.segmentId));
            return false;
        }

        if (found) {
            return true;
        }
    }

    return true;
}

bool StorageManager::getUploadEventBatchForSession(const char* sessionIdOverride,
                                                   uint32_t sinceId,
                                                   int maxCount,
                                                   JsonDocument& out) {
    out.clear();

    if (!_ready || maxCount <= 0) return false;

    String sessionId =
        (sessionIdOverride && sessionIdOverride[0]) ?
            String(sessionIdOverride) : SESS.getId();
    if (!sessionId.length()) return true;
    if (_backlog.uploadIndexResident) {
        if (!_getUploadEventBatchForSessionFromIndex(sessionId, sinceId, maxCount, out)) {
            DLOG_WARN("STORAGE",
                      "Upload index batch failed; releasing index session=%s since=%lu",
                      sessionId.c_str(),
                      static_cast<unsigned long>(sinceId));
            _releaseUploadIndexMemory("index_batch_failed");
            return false;
        }

        // A resident upload index is the upload contract. An empty indexed
        // batch means this session/window is exhausted; falling through to
        // the spool scanner can walk past a heap-guarded window and turn a
        // bounded upload into repeated full-spool scans.
        return true;
    }

    return _getEventBatchForSessionFromSpool(sessionId, sinceId, maxCount, out);
}

// Legacy indexed single-record fetch path. The hot MQTT upload path streams
// from spool now; this remains for sidecar/index diagnostics and recovery.
bool StorageManager::getNextUploadEventForSession(const char* sessionIdOverride,
                                                  uint32_t sinceId,
                                                  JsonDocument& out,
                                                  bool& found) {
    out.clear();
    found = false;

    if (!_ready || !_backlog.uploadIndexResident) {
        return false;
    }

    String sessionId =
        (sessionIdOverride && sessionIdOverride[0]) ?
            String(sessionIdOverride) : SESS.getId();
    if (!sessionId.length()) return true;

    if (!_getNextUploadEventForSessionFromIndex(sessionId, sinceId, out, found)) {
        DLOG_WARN("STORAGE",
                  "Upload index next failed; releasing index session=%s since=%lu",
                  sessionId.c_str(),
                  static_cast<unsigned long>(sinceId));
        _releaseUploadIndexMemory("index_next_failed");
        return false;
    }
    return true;
}

bool StorageManager::prepareUploadIndexForUpload(uint32_t budgetMs) {
    if (!_ready) {
        DLOG_WARN("STORAGE",
                  "Upload stream prepare ready=0 reason=not_ready");
        return false;
    }

    if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY) {
        DLOG_INFO("STORAGE",
                  "Upload stream prepare deferred owner=%s reason=capture",
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
        return false;
    }

    if (_backlog.uploadIndexResident) {
        _releaseUploadIndexMemory("prepare_stream");
    }

    // Bound the resident PSRAM index independently of total spool depth. A
    // successful truncated window leaves the remaining records pending, so
    // continuous drain opens the next window without growing peak memory.
    static constexpr uint32_t kUploadIndexWindowMax = 8192U;
    _backlog.uploadIndexWindowLimit = kUploadIndexWindowMax;

    const uint32_t t0 = millis();
    if (!_flushWorkerAppendFile("prepare_upload_stream", true)) {
        DLOG_WARN("STORAGE",
                  "Upload stream prepare ready=0 reason=append_flush_failed");
        return false;
    }

    // Valid summaries make the upload enrich-delta skip and pending reconcile
    // work at backlog scale; the active append segment can remain invalid.
    size_t summaryRebuildGuard = _spoolIndex.segments.size() + 1U;
    while (_hasInvalidSpoolSummaries() && summaryRebuildGuard-- > 0U) {
        if (!_rebuildInvalidSegmentSummaries(false)) {
            break;
        }
    }

    // Paged PSRAM upload index avoids per-bucket spool rescans.
    if (!_backlog.uploadIndexResident) {
        const uint32_t idxT0 = millis();
        const bool idxOk = _rebuildUploadIndex();
        DLOG_INFO("STORAGE",
                  "Upload stream prepare index built ok=%d resident=%d ms=%lu",
                  idxOk ? 1 : 0,
                  _backlog.uploadIndexResident ? 1 : 0,
                  static_cast<unsigned long>(millis() - idxT0));
    }

    DLOG_INFO("STORAGE",
              "Upload stream prepare ready=1 pending=%lu budgetMs=%lu ms=%lu internalFree=%lu internalLargest=%lu psramFree=%lu",
              static_cast<unsigned long>(getPendingEventCount()),
              static_cast<unsigned long>(budgetMs),
              static_cast<unsigned long>(millis() - t0),
              static_cast<unsigned long>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
              static_cast<unsigned long>(heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM)),
              static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return true;
}

StorageLaneCounts StorageManager::getPendingUploadCounts(const char* sessionIdOverride) {
    StorageLaneCounts counts;
    if (!_ready) return counts;

    const bool filterBySession = sessionIdOverride && sessionIdOverride[0];
    if (filterBySession) {
        DLOG_WARN("STORAGE",
                  "Session pending upload lane count requested outside session summary; maintenance queued");
        requestMaintenance(STORAGE_MAINT_DIRTY_SUMMARY,
                           "pending_upload_session_count_request");
        return counts;
    }

    bool sawStaleSummary = false;
    for (const auto& seg : _spoolIndex.segments) {
        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;

        if (!summaryReady) {
            sawStaleSummary = true;
            continue;
        }

        counts.mission += seg.pendingUploadMissionCount;
        counts.noise += seg.pendingUploadNoiseCount;
    }

    if (sawStaleSummary) {
        requestMaintenance(STORAGE_MAINT_DIRTY_SUMMARY,
                           "pending_upload_summary_stale");
    }

    const uint32_t splitPending = counts.mission + counts.noise;
    if (isPendingEventCountAuthoritative() && splitPending < _pendingEventCount) {
        counts.noise += _pendingEventCount - splitPending;
    }

    return counts;
}

StorageLaneCounts StorageManager::getPendingEnrichmentCounts() {
    return _getPendingEnrichmentCounts(String(), false);
}

StorageLaneCounts StorageManager::getPendingEnrichmentCountsForSession(const char* sessionIdOverride) {
    String sessionId =
        (sessionIdOverride && sessionIdOverride[0]) ? String(sessionIdOverride) : SESS.getId();
    return _getPendingEnrichmentCounts(sessionId, true);
}

StorageLaneCounts StorageManager::_getPendingEnrichmentCounts(const String& sessionId,
                                                              bool filterBySession) {
    StorageLaneCounts counts;
    if (!_ready) return counts;

    if (filterBySession && !sessionId.length()) return counts;

    // Hot-path early-out: when every segment summary is current, the live
    // per-segment pendingEnrichmentCount tells us authoritatively whether
    // ANY pending work exists. Zero everywhere ⇒ skip the delta scan and
    // every event-record open. Segments that need scanning (summary stale
    // or non-zero pending) are visited below.
    bool allSummariesCurrent = true;
    bool anyPending = false;
    for (const auto& seg : _spoolIndex.segments) {
        // Summary-only passes do no record-level work, so they need their own
        // scheduler handoff on deep field spools.
        delay(1);
        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;
        if (!summaryReady) {
            allSummariesCurrent = false;
            break;
        }
        if (seg.pendingEnrichmentCount > 0) {
            anyPending = true;
        }
    }
    if (allSummariesCurrent && !anyPending) {
        return counts;
    }

    SpiramVector<uint32_t> enrichedIds;
    if (!_loadSpoolEnrichmentIds(sessionId, filterBySession, enrichedIds)) {
        requestMaintenance(STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY,
                           "pending_enrichment_cursor_load_failed");
        return {};
    }

    for (const auto& seg : _spoolIndex.segments) {
        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;

        if (summaryReady && seg.eventCount == 0) {
            continue;
        }

        // Drained segments contribute nothing; skip the record scan.
        // Sessions are filtered post-scan, so this skip stays correct
        // even with filterBySession (a drained segment cannot host a
        // pending event for any session).
        if (summaryReady && seg.pendingEnrichmentCount == 0) {
            continue;
        }

        const bool ok = _scanSegmentRecords(seg.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                if (rec.recordType != SPOOL_REC_EVENT ||
                    !rec.sessionId.length() ||
                    rec.eventId == 0) {
                    return true;
                }

                if (filterBySession && rec.sessionId != sessionId) {
                    return true;
                }

                if (std::binary_search(enrichedIds.begin(),
                                       enrichedIds.end(),
                                       rec.eventId)) {
                    return true;
                }

                const JsonObjectConst doc = rec.doc.as<JsonObjectConst>();
                if (!_eventRecordPendingEnrichment(doc)) {
                    return true;
                }

                if (_eventRecordLane(doc) == STORAGE_LANE_MISSION) {
                    counts.mission++;
                } else {
                    counts.noise++;
                }
                return true;
            });

        if (!ok) {
            requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                               "pending_enrichment_segment_scan_failed");
            return {};
        }
    }

    return counts;
}

uint32_t StorageManager::livePendingEnrichmentTotal() const {
    uint32_t total = 0;
    for (const auto& seg : _spoolIndex.segments) {
        total += seg.pendingEnrichmentCount;
    }
    return total;
}

uint32_t StorageManager::getPendingEnrichmentCountForSession(const char* sessionIdOverride) {
    return getPendingEnrichmentCountsForSession(sessionIdOverride).total();
}

bool StorageManager::getSessionStorageSummary(const char* sessionIdOverride,
                                              StorageSessionSummary& out) {
    out = {};

    if (!_ready) {
        return false;
    }

    const String sessionId =
        (sessionIdOverride && sessionIdOverride[0]) ? String(sessionIdOverride) : SESS.getId();
    if (!sessionId.length()) {
        return true;
    }
    const uint32_t sessionWatermark = _uploadedWatermarkForSession(sessionId);

    for (const auto& seg : _spoolIndex.segments) {
        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;

        if (summaryReady && seg.recordCount == 0) {
            continue;
        }

        const bool ok = _scanSegmentRecords(seg.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                if (!rec.sessionId.length() || rec.sessionId != sessionId) {
                    return true;
                }

                if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
                    if (rec.eventId != 0) {
                        out.enrichmentDeltas++;
                    }
                    return true;
                }

                if (rec.recordType != SPOOL_REC_EVENT || rec.eventId == 0) {
                    return true;
                }

                const JsonObjectConst doc = rec.doc.as<JsonObjectConst>();
                const StorageLane lane = _eventRecordLane(doc);
                const StoragePriority priority = _eventRecordPriority(doc);

                if (lane == STORAGE_LANE_MISSION) {
                    out.missionTotal++;
                } else {
                    out.noiseTotal++;
                }

                switch (priority) {
                    case STORAGE_PRIO_P0: out.p0Total++; break;
                    case STORAGE_PRIO_P1: out.p1Total++; break;
                    case STORAGE_PRIO_P2: out.p2Total++; break;
                    case STORAGE_PRIO_P3:
                    default:              out.p3Total++; break;
                }

                if (out.firstEventId == 0 || rec.eventId < out.firstEventId) {
                    out.firstEventId = rec.eventId;
                }
                if (rec.eventId > out.lastEventId) {
                    out.lastEventId = rec.eventId;
                }

                if (rec.eventId > sessionWatermark) {
                    if (lane == STORAGE_LANE_MISSION) {
                        out.pendingUploadMission++;
                    } else {
                        out.pendingUploadNoise++;
                    }
                }

                return true;
            });

        if (!ok) {
            requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                               "enrichment_window_segment_scan_failed");
            return false;
        }
    }

    const StorageLaneCounts pendingEnrichment =
        getPendingEnrichmentCountsForSession(sessionId.c_str());
    out.pendingEnrichmentMission = pendingEnrichment.mission;
    out.pendingEnrichmentNoise = pendingEnrichment.noise;

    return true;
}

bool StorageManager::getPendingEnrichmentBatchForSession(const char* sessionIdOverride,
                                                         PendingEventDescriptor* out,
                                                         size_t maxCount,
                                                         size_t& outCount) {
    String sessionId =
        (sessionIdOverride && sessionIdOverride[0]) ?
            String(sessionIdOverride) : SESS.getId();
    return _getPendingEnrichmentBatch(sessionId, true, out, maxCount, outCount);
}

bool StorageManager::getPendingEnrichmentBatch(PendingEventDescriptor* out,
                                               size_t maxCount,
                                               size_t& outCount) {
    return _getPendingEnrichmentBatch(String(), false, out, maxCount, outCount);
}

bool StorageManager::getPendingEnrichmentBatchExcluding(const uint32_t* excludeIds,
                                                        size_t excludeCount,
                                                        PendingEventDescriptor* out,
                                                        size_t maxCount,
                                                        size_t& outCount) {
    return _getPendingEnrichmentBatch(String(), false, out, maxCount, outCount,
                                     excludeIds, excludeCount);
}

bool StorageManager::prepareEnrichmentIndexForWindow(size_t maxRecords,
                                                     uint32_t budgetMs,
                                                     bool& ready) {
    ready = false;
    if (!_ready || maxRecords == 0) return false;
    if (_backlog.enrichmentIndexResident) {
        ready = true;
        return true;
    }

    constexpr uint32_t kPrepareMinFreeInternal = 8UL * 1024UL;
    constexpr uint32_t kPrepareMinLargestInternal = 4UL * 1024UL;
    constexpr uint32_t kPrepareMinFreePsram = 64UL * 1024UL;
    constexpr uint32_t kPrepareMinLargestPsram = 32UL * 1024UL;

    if (!_backlog.enrichmentBuildActive) {
        releaseEnrichmentIndexMemory("prepare");
        const uint32_t freeInternal =
            heap_caps_get_free_size(SPECTRE_CAP_DRAM);
        const uint32_t largestInternal =
            heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM);
        const uint32_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const uint32_t largestPsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        if (freeInternal < kPrepareMinFreeInternal ||
            largestInternal < kPrepareMinLargestInternal ||
            freePsram < kPrepareMinFreePsram ||
            largestPsram < kPrepareMinLargestPsram) {
            DLOG_WARN("STORAGE",
                      "Enrichment window prepare skipped reason=heap_guard internal=%lu/%lu psram=%lu/%lu maxRecords=%u",
                      static_cast<unsigned long>(freeInternal),
                      static_cast<unsigned long>(largestInternal),
                      static_cast<unsigned long>(freePsram),
                      static_cast<unsigned long>(largestPsram),
                      static_cast<unsigned>(maxRecords));
            requestMaintenance(STORAGE_MAINT_DIRTY_SUMMARY,
                               "enrichment_window_heap_guard");
            return false;
        }

        _backlog.enrichmentWindowLimit = maxRecords;
        _backlog.enrichmentWindow.assign(maxRecords, PendingEventDescriptor{});
        _backlog.enrichmentKnownIds.clear();
        size_t enrichedIdReserve = 0;
        for (const auto& seg : _spoolIndex.segments) {
            const bool summaryReady =
                seg.summaryValid &&
                seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;
            if (summaryReady) {
                enrichedIdReserve += seg.enrichDeltaCount;
            }
        }
        if (enrichedIdReserve > 0) {
            _backlog.enrichmentKnownIds.reserve(enrichedIdReserve);
        }
        _backlog.enrichmentBuildIdSegmentCursor = 0;
        _backlog.enrichmentBuildSegmentCursor = 0;
        _backlog.enrichmentBuildCandidateCount = 0;
        _backlog.enrichmentBuildStartedMs = millis();
        _backlog.enrichmentBuildIdsReady = false;
        _backlog.enrichmentBuildHeapReady = false;
        _backlog.enrichmentBuildSawOverflow = false;
        _backlog.enrichmentBuildActive = true;
        DLOG_INFO("STORAGE",
                  "Enrichment window build started limit=%u enrichedIdReserve=%u segments=%u freeInternal=%lu freePsram=%lu",
                  static_cast<unsigned>(maxRecords),
                  static_cast<unsigned>(enrichedIdReserve),
                  static_cast<unsigned>(_spoolIndex.segments.size()),
                  static_cast<unsigned long>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
                  static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }

    const uint32_t sliceBudgetMs =
        budgetMs == 0 ? ENRICH_SCAN_BUDGET_MS
                      : std::min<uint32_t>(budgetMs, ENRICH_SCAN_BUDGET_MS);
    const uint32_t sliceStartMs = millis();

    if (!_backlog.enrichmentBuildIdsReady) {
        size_t scannedIdsThisSlice = 0;
        size_t skippedIdsThisSlice = 0;
        while (_backlog.enrichmentBuildIdSegmentCursor <
               _spoolIndex.segments.size()) {
            if (scannedIdsThisSlice > 0 &&
                millis() - sliceStartMs >= sliceBudgetMs) {
                break;
            }

            const SpoolSegmentInfo& seg = _spoolIndex.segments[
                _backlog.enrichmentBuildIdSegmentCursor++];
            const bool summaryReady =
                seg.summaryValid &&
                seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;
            if (summaryReady && seg.enrichDeltaCount == 0) {
                skippedIdsThisSlice++;
                continue;
            }

            const bool ok = _scanSegmentRecordHeaders(seg.segmentId,
                [&](const DecodedSpoolRecordHeader& rec) -> bool {
                    if (rec.recordType == SPOOL_REC_ENRICH_DELTA &&
                        rec.targetEventId != 0) {
                        _backlog.enrichmentKnownIds.push_back(rec.targetEventId);
                    }
                    return true;
                });
            if (!ok) {
                releaseEnrichmentIndexMemory("prepare_ids_failed");
                requestMaintenance(STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY,
                                   "enrichment_window_cursor_load_failed");
                return false;
            }
            scannedIdsThisSlice++;
        }

        const bool idsScannedAll =
            _backlog.enrichmentBuildIdSegmentCursor >=
            _spoolIndex.segments.size();
        if (!idsScannedAll) {
            DLOG_INFO("STORAGE",
                      "Enrichment ID slice scanned=%u skipped=%u nextSegment=%u/%u ids=%u ms=%lu",
                      static_cast<unsigned>(scannedIdsThisSlice),
                      static_cast<unsigned>(skippedIdsThisSlice),
                      static_cast<unsigned>(_backlog.enrichmentBuildIdSegmentCursor),
                      static_cast<unsigned>(_spoolIndex.segments.size()),
                      static_cast<unsigned>(_backlog.enrichmentKnownIds.size()),
                      static_cast<unsigned long>(millis() - sliceStartMs));
            return true;
        }

        std::sort(_backlog.enrichmentKnownIds.begin(),
                  _backlog.enrichmentKnownIds.end());
        _backlog.enrichmentKnownIds.erase(
            std::unique(_backlog.enrichmentKnownIds.begin(),
                        _backlog.enrichmentKnownIds.end()),
            _backlog.enrichmentKnownIds.end());
        _backlog.enrichmentBuildIdsReady = true;
        DLOG_INFO("STORAGE",
                  "Enrichment IDs ready records=%u scanned=%u skipped=%u ms=%lu",
                  static_cast<unsigned>(_backlog.enrichmentKnownIds.size()),
                  static_cast<unsigned>(scannedIdsThisSlice),
                  static_cast<unsigned>(skippedIdsThisSlice),
                  static_cast<unsigned long>(millis() - sliceStartMs));
        if (millis() - sliceStartMs >= sliceBudgetMs) {
            return true;
        }
    }

    auto eventTypeCode = [](const char* type) -> uint8_t {
        if (!type || !type[0] || strcmp(type, "probe") == 0) return 0;
        if (strcmp(type, "device") == 0) return 1;
        if (strcmp(type, "drone") == 0) return 2;
        if (strcmp(type, "pmkid") == 0) return 3;
        return 0;
    };
    auto betterPending = [](const PendingEventDescriptor& lhs,
                            const PendingEventDescriptor& rhs) -> bool {
        const bool leftMission =
            lhs.lane == static_cast<uint8_t>(STORAGE_LANE_MISSION);
        const bool rightMission =
            rhs.lane == static_cast<uint8_t>(STORAGE_LANE_MISSION);
        if (leftMission != rightMission) return leftMission;
        if (lhs.priority != rhs.priority) return lhs.priority < rhs.priority;
        if (lhs.valueScore != rhs.valueScore) return lhs.valueScore > rhs.valueScore;
        if (lhs.timestampMs != rhs.timestampMs) return lhs.timestampMs < rhs.timestampMs;
        return lhs.eventId < rhs.eventId;
    };
    auto retainCandidate = [&](const PendingEventDescriptor& candidate) {
        size_t& count = _backlog.enrichmentBuildCandidateCount;
        PendingEventDescriptor* items = _backlog.enrichmentWindow.data();
        if (count < _backlog.enrichmentWindowLimit) {
            items[count++] = candidate;
            if (count == _backlog.enrichmentWindowLimit) {
                std::make_heap(items, items + count, betterPending);
                _backlog.enrichmentBuildHeapReady = true;
            }
            return;
        }
        _backlog.enrichmentBuildSawOverflow = true;
        if (betterPending(candidate, items[0])) {
            std::pop_heap(items, items + count, betterPending);
            items[count - 1] = candidate;
            std::push_heap(items, items + count, betterPending);
        }
    };

    size_t scannedThisSlice = 0;
    size_t skippedThisSlice = 0;
    bool reconciledThisSlice = false;

    while (_backlog.enrichmentBuildSegmentCursor < _spoolIndex.segments.size()) {
        if (scannedThisSlice > 0 && millis() - sliceStartMs >= sliceBudgetMs) {
            break;
        }

        SpoolSegmentInfo& seg =
            _spoolIndex.segments[_backlog.enrichmentBuildSegmentCursor++];
        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;
        if (summaryReady && seg.eventCount == 0) {
            skippedThisSlice++;
            continue;
        }

        uint32_t exactSegmentPending = 0;
        const bool ok = _scanSegmentRecordHeaders(seg.segmentId,
            [&](const DecodedSpoolRecordHeader& rec) -> bool {
                if (rec.recordType == SPOOL_REC_ENRICH_DELTA ||
                    !rec.sessionId.length() || rec.eventId == 0 ||
                    std::binary_search(_backlog.enrichmentKnownIds.begin(),
                                       _backlog.enrichmentKnownIds.end(),
                                       rec.eventId)) {
                    return true;
                }

                const RAMSpool::CaptureClassification cls =
                    RAMSpool::classify(rec.typeString.c_str(), "");
                if (!cls.enrichEligible) return true;

                exactSegmentPending++;
                PendingEventDescriptor candidate;
                candidate.eventId = rec.eventId;
                candidate.timestampMs = rec.timestampMs;
                candidate.epochUtc = rec.epochUtc;
                candidate.type = eventTypeCode(rec.typeString.c_str());
                candidate.status =
                    rec.eventId <= _uploadedWatermarkForSession(rec.sessionId)
                        ? EVT_UPLOADED : EVT_RAW;
                candidate.lane =
                    cls.lane == RAMSpool::LANE_MISSION
                        ? static_cast<uint8_t>(STORAGE_LANE_MISSION)
                        : static_cast<uint8_t>(STORAGE_LANE_NOISE);
                candidate.priority = static_cast<uint8_t>(cls.priority);
                retainCandidate(candidate);
                return true;
            });
        if (!ok) {
            releaseEnrichmentIndexMemory("prepare_segment_failed");
            requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                               "enrichment_window_segment_scan_failed");
            return false;
        }

        scannedThisSlice++;
        if (seg.pendingEnrichmentCount != exactSegmentPending) {
            seg.pendingEnrichmentCount = exactSegmentPending;
            reconciledThisSlice = true;
        }

        if (_backlog.enrichmentBuildCandidateCount >=
                _backlog.enrichmentWindowLimit &&
            millis() - sliceStartMs >= sliceBudgetMs) {
            _backlog.enrichmentBuildSawOverflow =
                _backlog.enrichmentBuildSawOverflow ||
                _backlog.enrichmentBuildSegmentCursor < _spoolIndex.segments.size();
            break;
        }
    }

    if (reconciledThisSlice) {
        _spoolIndexDirty = true;
        requestMaintenance(STORAGE_MAINT_DIRTY_SPOOL_INDEX,
                           "enrichment_window_reconciled");
    }

    const bool scannedAll =
        _backlog.enrichmentBuildSegmentCursor >= _spoolIndex.segments.size();
    const bool windowFull =
        _backlog.enrichmentBuildCandidateCount >= _backlog.enrichmentWindowLimit;
    if (!scannedAll && !windowFull) {
        DLOG_INFO("STORAGE",
                  "Enrichment window slice scanned=%u skipped=%u nextSegment=%u/%u candidates=%u ms=%lu",
                  static_cast<unsigned>(scannedThisSlice),
                  static_cast<unsigned>(skippedThisSlice),
                  static_cast<unsigned>(_backlog.enrichmentBuildSegmentCursor),
                  static_cast<unsigned>(_spoolIndex.segments.size()),
                  static_cast<unsigned>(_backlog.enrichmentBuildCandidateCount),
                  static_cast<unsigned long>(millis() - sliceStartMs));
        return true;
    }

    PendingEventDescriptor* items = _backlog.enrichmentWindow.data();
    const size_t count = _backlog.enrichmentBuildCandidateCount;
    if (_backlog.enrichmentBuildHeapReady) {
        std::sort_heap(items, items + count, betterPending);
    } else if (count > 1) {
        std::sort(items, items + count, betterPending);
    }
    _backlog.enrichmentWindow.resize(count);
    _backlog.enrichmentKnownIds.clear();
    _backlog.enrichmentKnownIds.shrink_to_fit();
    _backlog.enrichmentIndexResident = true;
    _backlog.enrichmentWindowCursor = 0;
    _backlog.enrichmentWindowTruncated =
        _backlog.enrichmentBuildSawOverflow || !scannedAll;
    _backlog.enrichmentBuildActive = false;
    ready = true;

    DLOG_INFO("STORAGE",
              "Enrichment window ready records=%u truncated=%u buildMs=%lu freeInternal=%lu freePsram=%lu",
              static_cast<unsigned>(count),
              _backlog.enrichmentWindowTruncated ? 1U : 0U,
              static_cast<unsigned long>(millis() - _backlog.enrichmentBuildStartedMs),
              static_cast<unsigned long>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
              static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return true;
}

bool StorageManager::getNextPendingEnrichmentRecord(PendingEventDescriptor& out,
                                                    bool& found) {
    found = false;
    out = {};
    if (!_ready || !_backlog.enrichmentIndexResident) {
        return false;
    }

    while (_backlog.enrichmentWindowCursor < _backlog.enrichmentWindow.size()) {
        out = _backlog.enrichmentWindow[_backlog.enrichmentWindowCursor++];
        if (out.eventId != 0) {
            found = true;
            return true;
        }
    }
    return true;
}

void StorageManager::releaseEnrichmentIndexMemory(const char* reason) {
    if (!_backlog.enrichmentIndexResident &&
        !_backlog.enrichmentBuildActive &&
        _backlog.enrichmentWindow.empty() &&
        _backlog.enrichmentKnownIds.empty()) {
        return;
    }

    _backlog.enrichmentWindow.clear();
    _backlog.enrichmentWindow.shrink_to_fit();
    _backlog.enrichmentKnownIds.clear();
    _backlog.enrichmentKnownIds.shrink_to_fit();
    _backlog.enrichmentWindowCursor = 0;
    _backlog.enrichmentWindowLimit = 0;
    _backlog.enrichmentWindowTruncated = false;
    _backlog.enrichmentIndexResident = false;
    _backlog.enrichmentBuildIdSegmentCursor = 0;
    _backlog.enrichmentBuildSegmentCursor = 0;
    _backlog.enrichmentBuildCandidateCount = 0;
    _backlog.enrichmentBuildStartedMs = 0;
    _backlog.enrichmentBuildActive = false;
    _backlog.enrichmentBuildIdsReady = false;
    _backlog.enrichmentBuildHeapReady = false;
    _backlog.enrichmentBuildSawOverflow = false;
    DLOG_INFO("STORAGE", "Enrichment window released reason=%s",
              (reason && reason[0]) ? reason : "-");
}

bool StorageManager::markEnriched(uint32_t eventId,
                                  float lat, float lon,
                                  float alt, float accuracy,
                                  const char* tag) {
    return enrichEvent(eventId, lat, lon, alt, accuracy, tag);
}

bool StorageManager::markEnrichmentNoData(uint32_t eventId) {
    if (!_ready || eventId == 0) return false;

    String sessionId;
    if (!_findEventSession(eventId, sessionId) || !sessionId.length()) {
        DLOG_WARN("STORAGE",
                  "markEnrichmentNoData: session not found event=%lu",
                  static_cast<unsigned long>(eventId));
        return false;
    }

    // Zero coords, empty tag, NO_DATA flag set — distinguishes from a real
    // (0,0) fix because callers that care can branch on ENRICH_FLAG_NO_DATA.
    return _appendSpoolEnrichmentDelta(sessionId,
                                       eventId,
                                       0.0f, 0.0f,
                                       0.0f, 0.0f,
                                       nullptr,
                                       /*noData=*/true);
}

bool StorageManager::_getPendingEnrichmentBatch(const String& sessionId,
                                                bool filterBySession,
                                                PendingEventDescriptor* out,
                                                size_t maxCount,
                                                size_t& outCount,
                                                const uint32_t* excludeIds,
                                                size_t excludeCount,
                                                bool authoritativeScan) {
    outCount = 0;
    if (!_ready || !out || maxCount == 0) return false;

    if (filterBySession && !sessionId.length()) return false;

    bool allSummariesCurrent = true;
    bool anyPending = false;
    for (const auto& seg : _spoolIndex.segments) {
        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;
        if (!summaryReady) {
            allSummariesCurrent = false;
            break;
        }
        if (seg.pendingEnrichmentCount > 0) {
            anyPending = true;
        }
    }
    if (allSummariesCurrent && !anyPending) {
        return true;
    }

    SpiramVector<uint32_t> enrichedIds;
    DLOG_INFO("STORAGE",
              "batch_load_ids_enter freeInternal=%lu segments=%u",
              static_cast<unsigned long>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
              static_cast<unsigned>(_spoolIndex.segments.size()));
    if (!_loadSpoolEnrichmentIds(sessionId, filterBySession, enrichedIds)) {
        requestMaintenance(STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY,
                           "enrichment_window_cursor_load_failed");
        return false;
    }
    DLOG_INFO("STORAGE",
              "batch_load_ids_done count=%u capacity=%u freeInternal=%lu",
              static_cast<unsigned>(enrichedIds.size()),
              static_cast<unsigned>(enrichedIds.capacity()),
              static_cast<unsigned long>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)));

    auto eventTypeCode = [](const char* type) -> uint8_t {
        if (!type || !type[0]) return 0;
        if (strcmp(type, "probe") == 0) return 0;
        if (strcmp(type, "device") == 0) return 1;
        if (strcmp(type, "drone") == 0) return 2;
        if (strcmp(type, "pmkid") == 0) return 3;
        return 0;
    };

    auto betterPending = [](const PendingEventDescriptor& lhs,
                            const PendingEventDescriptor& rhs) -> bool {
        const bool leftMission =
            lhs.lane == static_cast<uint8_t>(STORAGE_LANE_MISSION);
        const bool rightMission =
            rhs.lane == static_cast<uint8_t>(STORAGE_LANE_MISSION);

        if (leftMission != rightMission) {
            return leftMission;
        }

        if (lhs.priority != rhs.priority) {
            return lhs.priority < rhs.priority;
        }

        if (lhs.valueScore != rhs.valueScore) {
            return lhs.valueScore > rhs.valueScore;
        }

        if (lhs.timestampMs != rhs.timestampMs) {
            return lhs.timestampMs < rhs.timestampMs;
        }

        return lhs.eventId < rhs.eventId;
    };

    bool candidateHeapReady = false;
    auto retainCandidate = [&](const PendingEventDescriptor& candidate) {
        if (outCount < maxCount) {
            out[outCount++] = candidate;
            if (outCount == maxCount) {
                // betterPending makes the least valuable retained item the
                // heap root, so replacement remains O(log N).
                std::make_heap(out, out + outCount, betterPending);
                candidateHeapReady = true;
            }
            return;
        }

        if (betterPending(candidate, out[0])) {
            std::pop_heap(out, out + outCount, betterPending);
            out[outCount - 1] = candidate;
            std::push_heap(out, out + outCount, betterPending);
        }
    };

    const bool streamingBatch =
        !filterBySession && maxCount <= 32U;

    // Wall-time bound: at a large backlog this scan reads every pending record
    // header across all segments inside the radio-suspended exclusive window.
    // Cap it so we never hold the radio for many seconds — we keep whatever
    // candidates were collected so far and resume on the next pass. Skipped
    // segments still get scanned on a later pass (their pending count only
    // drops as records drain), so progress is preserved.
    const uint32_t scanStartMs = millis();
    bool scanTimeBudgetHit = false;

    size_t segPendingScanned = 0;
    size_t segPendingSkipped = 0;
    DLOG_INFO("STORAGE",
              "batch_pending_loop_enter segments=%u maxCount=%u streaming=%u freeInternal=%lu",
              static_cast<unsigned>(_spoolIndex.segments.size()),
              static_cast<unsigned>(maxCount),
              streamingBatch ? 1U : 0U,
              static_cast<unsigned long>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)));

    bool reconciledAnySummary = false;
    for (auto& seg : _spoolIndex.segments) {
        // Many field segments contain fewer records than the scanner's yield
        // interval. Yield between files so those short scans cannot accumulate
        // into one watchdog-length critical section.
        delay(1);
        // Stop early once the scan budget is spent, but only after we already
        // have a full batch's worth — never return empty just because the first
        // segments were slow, or a sparse backlog could stall forever.
        if (outCount >= maxCount &&
            (millis() - scanStartMs) > ENRICH_SCAN_BUDGET_MS) {
            scanTimeBudgetHit = true;
            break;
        }

        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;

        if (!authoritativeScan && summaryReady && seg.eventCount == 0) {
            segPendingSkipped++;
            continue;
        }

        // Live pending-enrichment counter: drained segments cannot host
        // any candidate, so skip the record scan entirely.
        if (!authoritativeScan && summaryReady && seg.pendingEnrichmentCount == 0) {
            segPendingSkipped++;
            continue;
        }

        uint32_t exactSegmentPending = 0;

        DLOG_DEBUG("STORAGE",
                  "batch_pending_scan seg=%u pending=%u events=%u freeInternal=%lu largest=%lu outCount=%u",
                  static_cast<unsigned>(seg.segmentId),
                  static_cast<unsigned>(seg.pendingEnrichmentCount),
                  static_cast<unsigned>(seg.eventCount),
                  static_cast<unsigned long>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
                  static_cast<unsigned long>(heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM)),
                  static_cast<unsigned>(outCount));

        // Headers-only scan: no per-record JsonDocument. We classify each
        // event from its type string alone — that's sufficient to compute
        // lane / priority / enrichEligible. We do not compute valueScore
        // here (which would require enumerating payload fields). That
        // costs us ordering precision but preserves the eligibility filter,
        // which is what actually matters for drain correctness.
        const bool ok = _scanSegmentRecordHeaders(seg.segmentId,
            [&](const DecodedSpoolRecordHeader& rec) -> bool {
                if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
                    return true;
                }

                if (!rec.sessionId.length() || rec.eventId == 0) {
                    return true;
                }

                if (filterBySession && rec.sessionId != sessionId) {
                    return true;
                }

                if (std::binary_search(enrichedIds.begin(),
                                       enrichedIds.end(),
                                       rec.eventId)) {
                    return true;
                }

                if (excludeIds && excludeCount > 0) {
                    for (size_t ej = 0; ej < excludeCount; ++ej) {
                        if (excludeIds[ej] == rec.eventId) {
                            return true;
                        }
                    }
                }

                // Classify by type string. For binary events, F_ENRICH_STATE
                // is recomputed at decode time from classify(type) — so the
                // type-only classification here matches the JSON-path
                // _eventRecordPendingEnrichment result for binary records.
                const RAMSpool::CaptureClassification cls =
                    RAMSpool::classify(rec.typeString.c_str(), "");
                if (!cls.enrichEligible) {
                    return true;
                }

                exactSegmentPending++;

                PendingEventDescriptor candidate;
                const uint32_t watermark = _uploadedWatermarkForSession(rec.sessionId);
                candidate.eventId = rec.eventId;
                candidate.timestampMs = rec.timestampMs;
                candidate.epochUtc = rec.epochUtc;
                candidate.type = eventTypeCode(rec.typeString.c_str());
                candidate.status =
                    (rec.eventId <= watermark) ? EVT_UPLOADED : EVT_RAW;
                candidate.lane = (cls.lane == RAMSpool::LANE_MISSION)
                                     ? static_cast<uint8_t>(STORAGE_LANE_MISSION)
                                     : static_cast<uint8_t>(STORAGE_LANE_NOISE);
                candidate.priority = static_cast<uint8_t>(cls.priority);
                candidate.valueScore = 0;  // not derivable from headers-only

                retainCandidate(candidate);
                if (streamingBatch && outCount >= maxCount) {
                    return false;
                }
                return true;
            });

        segPendingScanned++;
        if (!ok) {
            DLOG_WARN("STORAGE",
                      "batch_pending_scan_failed seg=%u",
                      static_cast<unsigned>(seg.segmentId));
            return false;
        }
        // A global scan that did not fill the streaming batch walked this
        // segment to EOF, so its exact count is authoritative even on the
        // normal fast path. Persist that reconciliation. Without this, an old
        // gross pending count survived after all matching deltas were written;
        // the UI mirror then resurrected a false backlog and churned WiFi/BLE
        // every minute despite repeated zero-result scans.
        const bool fullGlobalSegmentScan =
            !filterBySession &&
            (!excludeIds || excludeCount == 0) &&
            (!streamingBatch || outCount < maxCount);
        if ((authoritativeScan || fullGlobalSegmentScan) &&
            seg.pendingEnrichmentCount != exactSegmentPending) {
            seg.pendingEnrichmentCount = exactSegmentPending;
            reconciledAnySummary = true;
        }
        if (streamingBatch && outCount >= maxCount) {
            DLOG_INFO("STORAGE",
                      "batch_pending_loop_break_streaming outCount=%u maxCount=%u",
                      static_cast<unsigned>(outCount),
                      static_cast<unsigned>(maxCount));
            break;
        }
    }

    if (candidateHeapReady) {
        std::sort_heap(out, out + outCount, betterPending);
    } else if (outCount > 1) {
        std::sort(out, out + outCount, betterPending);
    }

    if (reconciledAnySummary) {
        _spoolIndexDirty = true;
        requestMaintenance(STORAGE_MAINT_DIRTY_SPOOL_INDEX,
                           "enrichment_window_reconciled");
    }

    DLOG_INFO("STORAGE",
              "batch_pending_loop_done scanned=%u skipped=%u outCount=%u timeHit=%u elapsedMs=%lu freeInternal=%lu",
              static_cast<unsigned>(segPendingScanned),
              static_cast<unsigned>(segPendingSkipped),
              static_cast<unsigned>(outCount),
              scanTimeBudgetHit ? 1U : 0U,
              static_cast<unsigned long>(millis() - scanStartMs),
              static_cast<unsigned long>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)));

    return true;
}

bool StorageManager::forEachEventForSession(const char* sessionIdOverride,
                                            const std::function<bool(JsonObjectConst)>& cb) {
    if (!_ready) return false;

    String sessionId =
        (sessionIdOverride && sessionIdOverride[0]) ?
            String(sessionIdOverride) : SESS.getId();
    if (!sessionId.length()) return true;

    return _forEachResolvedEventForSession(sessionId, 0, -1, cb);
}

bool StorageManager::forEachStoredRecord(
    const std::function<bool(const DecodedSpoolRecord&)>& cb) const {
    if (!_ready || !cb) return false;
    for (const auto& seg : _spoolIndex.segments) {
        if (!_scanSegmentRecords(seg.segmentId, cb)) return false;
        // Derived-index rebuilds can traverse thousands of decoded records
        // across many small segments. Give the idle tasks an explicit window
        // at every segment boundary as well as inside the record scanner.
        vTaskDelay(1);
    }
    return true;
}

bool StorageManager::markEventUploaded(uint32_t eventId,
                                       const char* sessionIdOverride,
                                       uint8_t laneHint) {
    if (!_ready || eventId == 0) return false;

    String sessionId =
        (sessionIdOverride && sessionIdOverride[0]) ?
            String(sessionIdOverride) : SESS.getId();

    if (!sessionId.length()) return false;

    ScopedSemaphoreLock appendLock(_appendMutex);
    {
        const uint32_t before = _uploadedWatermarkForSession(sessionId);
        if (eventId <= before) {
            return true;
        }

        // During an upload batch, keep flash quiet — radio is active. The flush
        // happens in endUploadBatch() after RADIO_ARB releases the lease, to
        // avoid the LittleFS-erase-during-active-radio brownout.
        // The batch flush is the durable boundary; outside batch mode we defer
        // the counter update until the watermark commit succeeds.
        if (_uploadBatchActive) {
            if (_backlog.uploadIndexResident) {
                UploadIndexRecordV1 selected;
                bool selectedValid = false;
                auto it = _backlog.uploadIndexBySession.find(sessionId);
                if (it != _backlog.uploadIndexBySession.end()) {
                    const UploadIndexPagedSession& ptrs = it->second;
                    for (const auto& pagePtr : ptrs.pages) {
                        if (!pagePtr) continue;
                        const UploadIndexPage& page = *pagePtr;
                        for (uint8_t i = 0; i < page.count; ++i) {
                            const UploadIndexRecordV1& rec = page.records[i];
                            if (rec.eventId == eventId) {
                                selected = rec;
                                selectedValid = true;
                                break;
                            }
                        }
                        if (selectedValid) {
                            break;
                        }
                    }
                }

                if (selectedValid &&
                    selected.eventId > before &&
                    selected.eventId <= eventId &&
                    selected.sessionId[0] &&
                    sessionId == selected.sessionId) {
                    SpoolSegmentInfo* seg = _findSegmentInfo(selected.segmentId);
                    uint32_t* laneCounter = nullptr;
                    if (seg &&
                        selected.lane == static_cast<uint8_t>(STORAGE_LANE_MISSION)) {
                        laneCounter = &seg->pendingUploadMissionCount;
                    } else if (seg &&
                               selected.lane == static_cast<uint8_t>(STORAGE_LANE_NOISE)) {
                        laneCounter = &seg->pendingUploadNoiseCount;
                    }

                    if (laneCounter && *laneCounter > 0 && _pendingEventCount > 0) {
                        (*laneCounter)--;
                        _refreshSegmentLifecycle(*seg);
                        _applyCounterDelta(
                            "upload_batch_exact_single", -1, 0, 0, 0,
                            static_cast<StorageLane>(selected.lane),
                            static_cast<StoragePriority>(selected.priority));
                        _pendingCountDirty = false;
                        _spoolAuditRepairRequired = false;
                        _setUploadedWatermarkForSession(sessionId, eventId);
                        DLOG_UPLOAD_TRACE("STORAGE",
                                   "Counter trust=trusted reason=upload_batch_exact_single gen=%lu pending=%lu nextEventId=%lu",
                                   static_cast<unsigned long>(_storageMetaGeneration),
                                   static_cast<unsigned long>(_pendingEventCount),
                                   static_cast<unsigned long>(_nextEventId));
                        _uploadBatchDirty = true;
                        return true;
                    }

                    // The event has already been published successfully. If a
                    // stale sidecar or repaired segment summary says the
                    // segment has no lane counter left, still debit the global
                    // pending count for this indexed event. Leaving the global
                    // count untouched makes upload keep chasing already-drained
                    // records until it fails with a maintenance-only remainder.
                    _setUploadedWatermarkForSession(sessionId, eventId);
                    if (_pendingEventCount > 0) {
                        _applyCounterDelta(
                            "upload_mark_counter_underflow", -1, 0, 0, 0,
                            static_cast<StorageLane>(selected.lane),
                            static_cast<StoragePriority>(selected.priority));
                        _spoolSummaryRebuildPending = true;
                        requestMaintenance(STORAGE_MAINT_DIRTY_SUMMARY,
                                           "upload_mark_counter_underflow");
                        requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                                           "upload_mark_counter_underflow");
                        _setCounterTrustState(
                            STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                            "upload_mark_counter_underflow");
                    } else {
                        _pendingCountDirty = true;
                        _spoolAuditRepairRequired = true;
                        requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED,
                                           "upload_mark_global_underflow");
                    }
                    _uploadBatchDirty = true;
                    DLOG_WARN("STORAGE",
                              "markEventUploaded tolerated lane underflow session=%s event=%lu seg=%lu lane=%u pending=%lu",
                              sessionId.c_str(),
                              static_cast<unsigned long>(eventId),
                              static_cast<unsigned long>(selected.segmentId),
                              static_cast<unsigned>(selected.lane),
                              static_cast<unsigned long>(_pendingEventCount));
                    return true;
                }

                if (selectedValid) {
                    DLOG_WARN("STORAGE",
                              "markEventUploaded exact single failed session=%s event=%lu seg=%lu lane=%u pending=%lu",
                              sessionId.c_str(),
                              static_cast<unsigned long>(eventId),
                              static_cast<unsigned long>(selected.segmentId),
                              static_cast<unsigned>(selected.lane),
                              static_cast<unsigned long>(_pendingEventCount));
                } else {
                    DLOG_WARN("STORAGE",
                              "markEventUploaded exact path unavailable session=%s event=%lu",
                              sessionId.c_str(),
                              static_cast<unsigned long>(eventId));
                }
                requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED,
                                   "single_mark_upload_index_absent");
            }

            if (!_decrementPendingUploadForEvent(eventId,
                                                 laneHint,
                                                 "mark_uploaded_batch")) {
                _setUploadedWatermarkForSession(sessionId, eventId);
                if (_pendingEventCount > 0) {
                    _applyCounterDelta(
                        "upload_stream_mark_summary_lag", -1, 0, 0, 0,
                        static_cast<StorageLane>(
                            laneHint == static_cast<uint8_t>(STORAGE_LANE_MISSION)
                                ? STORAGE_LANE_MISSION
                                : STORAGE_LANE_NOISE),
                        STORAGE_PRIO_P3);
                    _spoolSummaryRebuildPending = true;
                    requestMaintenance(STORAGE_MAINT_DIRTY_SUMMARY,
                                       "upload_stream_mark_summary_lag");
                    requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                                       "upload_stream_mark_summary_lag");
                    _setCounterTrustState(
                        STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                        "upload_stream_mark_summary_lag");
                    DLOG_WARN("STORAGE",
                              "markEventUploaded tolerated stream summary lag session=%s event=%lu lane=%u pending=%lu",
                              sessionId.c_str(),
                              static_cast<unsigned long>(eventId),
                              static_cast<unsigned>(laneHint),
                              static_cast<unsigned long>(_pendingEventCount));
                } else {
                    _spoolIndex.pendingTotal = 0;
                    _spoolSummaryRebuildPending = true;
                    requestMaintenance(STORAGE_MAINT_DIRTY_SUMMARY,
                                       "upload_stream_mark_zero_pending");
                    requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                                       "upload_stream_mark_zero_pending");
                    _setCounterTrustState(
                        STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                        "upload_stream_mark_zero_pending");
                    DLOG_WARN("STORAGE",
                              "markEventUploaded tolerated stream zero-pending session=%s event=%lu lane=%u",
                              sessionId.c_str(),
                              static_cast<unsigned long>(eventId),
                              static_cast<unsigned>(laneHint));
                }
            } else {
                _setUploadedWatermarkForSession(sessionId, eventId);
                _applyCounterDelta("mark_uploaded_batch",
                                   -1,
                                   0,
                                   0,
                                   0,
                                   static_cast<StorageLane>(laneHint == static_cast<uint8_t>(STORAGE_LANE_MISSION)
                                                                 ? STORAGE_LANE_MISSION
                                                                 : STORAGE_LANE_NOISE),
                                   STORAGE_PRIO_P3);
            }
            _uploadBatchDirty = true;
            return true;
        }

        if (!_decrementPendingUploadForEvent(eventId,
                                             laneHint,
                                             "mark_uploaded_single")) {
            requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED,
                               "mark_uploaded_single_decrement_failed");
            return false;
        }

        _setUploadedWatermarkForSession(sessionId, eventId);
        _applyCounterDelta("mark_uploaded_single",
                           -1,
                           0,
                           0,
                           0,
                           static_cast<StorageLane>(laneHint == static_cast<uint8_t>(STORAGE_LANE_MISSION)
                                                         ? STORAGE_LANE_MISSION
                                                         : STORAGE_LANE_NOISE),
                           STORAGE_PRIO_P3);

        CONTRACT_WARN_ONCE(CONTRACT_STORAGE_MARK_OUTSIDE_BATCH,
                           "STORAGE",
                           !RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD),
                           "event=%lu marked while upload owner active without batch",
                           static_cast<unsigned long>(eventId));

        const uint32_t singleIndexWriteStart = millis();
        if (!_persistSpoolIndex(false, "upload_mark_single")) {
            _restoreUploadedWatermarkForSession(sessionId, before);
            _adjustPendingUploadForEvent(eventId,
                                         laneHint,
                                         1,
                                         "mark_uploaded_single_rollback");
            _applyCounterDelta("mark_uploaded_single_rollback",
                               1,
                               0,
                               0,
                               0,
                               static_cast<StorageLane>(laneHint == static_cast<uint8_t>(STORAGE_LANE_MISSION)
                                                             ? STORAGE_LANE_MISSION
                                                             : STORAGE_LANE_NOISE),
                               STORAGE_PRIO_P3);
            DLOG_WARN("STORAGE",
                      "Spool mark persist failed session=%s event=%lu",
                      sessionId.c_str(),
                      static_cast<unsigned long>(eventId));
            return false;
        }
        _logCaptureWriteAllowed(_spoolIndexPath().c_str(),
                                "upload_mark_single",
                                millis() - singleIndexWriteStart,
                                true);
        if (!_spoolAuditRepairRequired && !_repairRequested) {
            _pendingCountDirty = false;
        }
    }
    {
        const uint32_t metaWriteStart = millis();
        _persistEventMeta(true, "upload_mark_single");
        _logCaptureWriteAllowed(PATH_EVENT_META,
                                "upload_mark_single",
                                millis() - metaWriteStart,
                                true);
    }
    refreshStorageUiState();
    _logSpoolDiagnostics("mark_uploaded_single", sessionId);
    return true;
}


bool StorageManager::markEventsUploaded(uint32_t upToId) {
    if (!_ready || upToId == 0) return false;

    String sessionId = SESS.getId();
    if (!sessionId.length()) return false;

    ScopedSemaphoreLock appendLock(_appendMutex);
    {
        const uint32_t before = _uploadedWatermarkForSession(sessionId);
        if (upToId <= before) {
            return true;
        }

        // Compute the live decrement BEFORE advancing the watermark so we
        // never need to rescan the whole spool on the hot path.  When the
        // upload index is resident we can count exactly; otherwise we mark
        // the counter as needing an explicit recount (which the runtime
        // will never run on its own).
        bool decrementExact = false;
        std::vector<UploadIndexRecordV1> selected;
        const uint32_t pendingBeforeExact = _pendingEventCount;
        const uint32_t generationBeforeExact = _storageMetaGeneration;
        const bool pendingDirtyBeforeExact = _pendingCountDirty;
        const bool auditRepairBeforeExact = _spoolAuditRepairRequired;
        if (_backlog.uploadIndexResident) {
            auto it = _backlog.uploadIndexBySession.find(sessionId);
            if (it == _backlog.uploadIndexBySession.end()) {
                DLOG_WARN("STORAGE",
                          "markEventsUploaded exact path missing upload index session=%s upTo=%lu",
                          sessionId.c_str(),
                          static_cast<unsigned long>(upToId));
                requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED,
                                   "bulk_mark_upload_index_absent");
                return false;
            }

            const UploadIndexPagedSession& ptrs = it->second;
            for (const auto& pagePtr : ptrs.pages) {
                if (!pagePtr) continue;
                const UploadIndexPage& page = *pagePtr;
                for (uint8_t i = 0; i < page.count; ++i) {
                    const UploadIndexRecordV1& rec = page.records[i];
                    if (rec.eventId > before && rec.eventId <= upToId) {
                        selected.push_back(rec);
                    }
                }
            }

            if (_applyExactUploadedMarks(sessionId, selected, before, upToId)) {
                decrementExact = true;
            } else {
                return false;
            }
        } else {
            DLOG_WARN("STORAGE",
                      "markEventsUploaded refused without resident upload index; "
                      "counter-accurate bulk mark unavailable session=%s upTo=%lu",
                      sessionId.c_str(),
                      static_cast<unsigned long>(upToId));
            requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED,
                               "bulk_mark_index_absent");
            return false;
        }

        _setUploadedWatermarkForSession(sessionId, upToId);

        if (decrementExact) {
            const uint32_t decrement = static_cast<uint32_t>(selected.size());
            if (_uploadBatchActive) {
                DLOG_INFO("STORAGE",
                          "Counter trust=trusted reason=upload_batch_exact gen=%lu pending=%lu nextEventId=%lu",
                          static_cast<unsigned long>(_storageMetaGeneration),
                          static_cast<unsigned long>(_pendingEventCount),
                          static_cast<unsigned long>(_nextEventId));
                _uploadBatchDirty = true;
                _logSpoolDiagnostics("mark_uploaded_bulk_exact", sessionId);
                return true;
            }

            CONTRACT_WARN_ONCE(CONTRACT_STORAGE_MARK_OUTSIDE_BATCH,
                               "STORAGE",
                               !RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD),
                               "bulk mark=%lu while upload owner active without batch",
                               static_cast<unsigned long>(upToId));

            const uint32_t bulkIndexWriteStart = millis();
            if (!_persistSpoolIndex(false, "upload_mark_bulk_exact")) {
                _restoreUploadedWatermarkForSession(sessionId, before);
                for (const auto& rec : selected) {
                    if (!rec.sessionId[0] || sessionId != rec.sessionId) {
                        continue;
                    }

                    SpoolSegmentInfo* seg = _findSegmentInfo(rec.segmentId);
                    if (!seg) {
                        continue;
                    }

                    if (rec.lane == static_cast<uint8_t>(STORAGE_LANE_MISSION)) {
                        seg->pendingUploadMissionCount++;
                    } else if (rec.lane == static_cast<uint8_t>(STORAGE_LANE_NOISE)) {
                        seg->pendingUploadNoiseCount++;
                    }
                }
                _pendingEventCount = pendingBeforeExact;
                _spoolIndex.pendingTotal = _pendingEventCount;
                _storageMetaGeneration = generationBeforeExact;
                _pendingCountDirty = pendingDirtyBeforeExact;
                _spoolAuditRepairRequired = auditRepairBeforeExact;
                DLOG_WARN("STORAGE", "Spool bulk mark persist failed session=%s upTo=%lu",
                          sessionId.c_str(),
                          static_cast<unsigned long>(upToId));
                return false;
            }
            _logCaptureWriteAllowed(_spoolIndexPath().c_str(),
                                    "upload_mark_bulk_exact",
                                    millis() - bulkIndexWriteStart,
                                    true);

            {
                const uint32_t bulkMetaWriteStart = millis();
                _persistEventMeta(true, "upload_mark_bulk_exact");
                _logCaptureWriteAllowed(PATH_EVENT_META,
                                        "upload_mark_bulk_exact",
                                        millis() - bulkMetaWriteStart,
                                        true);
            }
            refreshStorageUiState();
            DLOG_INFO("STORAGE",
                      "Counter trust=trusted reason=upload_batch_exact gen=%lu pending=%lu nextEventId=%lu",
                      static_cast<unsigned long>(_storageMetaGeneration),
                      static_cast<unsigned long>(_pendingEventCount),
                      static_cast<unsigned long>(_nextEventId));
            _logSpoolDiagnostics("mark_uploaded_bulk_exact", sessionId);
            return true;
        }

        // Defer flash writes while an upload batch is active so brownout-prone
        // "w" opens stay out of the active-radio window. No rescan needed:
        // the live counter already reflects the exact bulk decrement.
        if (_uploadBatchActive) {
            _uploadBatchDirty = true;
            return true;
        }
    }

    {
        const uint32_t bulkMetaWriteStart = millis();
        _persistEventMeta(true, "upload_mark_bulk");
        _logCaptureWriteAllowed(PATH_EVENT_META,
                                "upload_mark_bulk",
                                millis() - bulkMetaWriteStart,
                                true);
    }
    refreshStorageUiState();
    _logSpoolDiagnostics("mark_uploaded_bulk", sessionId);
    return true;
}

void StorageManager::beginUploadBatch() {
    if (!_ready) return;
    (void)_flushWorkerAppendFile("upload_start", true);
    CONTRACT_WARN_ONCE(CONTRACT_STORAGE_BATCH_NESTING,
                       "STORAGE",
                       !_uploadBatchActive,
                       "beginUploadBatch nested while active=%d dirty=%d",
                       _uploadBatchActive ? 1 : 0,
                       _uploadBatchDirty ? 1 : 0);
    if (_workerMetadataDirtyPending || _spoolIndexDirty || _pendingCountDirty) {
        requestMaintenance(STORAGE_MAINT_SNAPSHOT_LAGGED, "upload_start");
    }
    _uploadBatchActive = true;
    _uploadBatchDirty  = false;
    DLOG_INFO("STORAGE", "Upload batch open; deferring watermark flush");
}

bool StorageManager::endUploadBatch() {
    if (!_ready) {
        _uploadBatchActive = false;
        _uploadBatchDirty  = false;
        _releaseUploadIndexMemory("upload_batch_end_not_ready");
        return false;
    }

    // MQTT intentionally releases WIFI_UPLOAD without resuming fallback capture
    // before this call, leaving RADIO_NONE as the flash-safe cleanup window.
    CONTRACT_WARN_ONCE(CONTRACT_UPLOAD_BATCH_OWNER_SYNC,
                       "STORAGE",
                       !_uploadBatchActive ||
                           RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD) ||
                           RADIO_ARB.isOwner(RADIO_BLE_GPS) ||
                           RADIO_ARB.isOwner(RADIO_NONE),
                       "upload batch closing without upload owner; owner=%s",
                       RadioArbiter::ownerName(RADIO_ARB.currentOwner()));

    const bool wasActive = _uploadBatchActive;
    const bool wasDirty  = _uploadBatchDirty;
    _uploadBatchActive = false;

    const bool hasSidecarWork =
        _workerMetadataDirtyPending ||
        _spoolIndexDirty ||
        _pendingCountDirty ||
        _storageUiRefreshPending;
    if (!wasActive || (!wasDirty && !hasSidecarWork)) {
        _uploadBatchDirty = false;
        _releaseUploadIndexMemory("upload_batch_end_clean");
        return true;
    }

    requestMaintenance(STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY, "upload_end");
    requestMaintenance(STORAGE_MAINT_DIRTY_SPOOL_INDEX, "upload_end");
    requestMaintenance(STORAGE_MAINT_DELETE_DRAINED, "upload_end");
    if (_pendingCountDirty || !isPendingEventCountAuthoritative()) {
        requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED, "upload_end");
    } else {
        requestMaintenance(STORAGE_MAINT_SNAPSHOT_LAGGED, "upload_end");
    }
    _queueStorageUiRefresh(true);
    DLOG_INFO("STORAGE",
              "Upload batch closed; maintenance queued dirty=%d sidecar=%d flags=%s",
              wasDirty ? 1 : 0,
              hasSidecarWork ? 1 : 0,
              maintenanceFlagsText());
    _releaseUploadIndexMemory("upload_batch_end");
    return true;
}

bool StorageManager::flushUploadCheckpoint() {
    if (!_ready)               return false;
    if (!_uploadBatchActive)   return true;
    if (!_uploadBatchDirty &&
        !_workerMetadataDirtyPending &&
        !_storageUiRefreshPending) {
        return true;
    }

    // Intentionally mid-radio: caller accepts the brownout trade-off in
    // exchange for durable mid-upload progress.  Batch stays open.
    _uploadBatchDirty = false;

    flushWorkerMetadataBatch("upload_checkpoint", true);
    if (SpoolSegmentInfo* active = _findSegmentInfo(_spoolIndex.activeSegmentId)) {
        bool wroteCheckpoint = false;
        (void)_maybeWriteBinarySegmentCheckpoint(*active,
                                                 "upload_checkpoint",
                                                 false,
                                                 &wroteCheckpoint);
        if (wroteCheckpoint) {
            _spoolIndexDirty = true;
        }
    }
    const uint32_t checkpointIndexWriteStart = millis();
    bool ok = _persistSpoolIndex(true, "upload_checkpoint");
    if (!ok) {
        DLOG_WARN("STORAGE", "Upload checkpoint flush: spool index persist failed");
    }
    _logCaptureWriteAllowed(_spoolIndexPath().c_str(),
                            "upload_checkpoint",
                            millis() - checkpointIndexWriteStart,
                            ok);

    // No rescan: every markEventUploaded already adjusted the live counter.
    {
        const uint32_t checkpointMetaWriteStart = millis();
        _persistEventMeta(true, "upload_checkpoint");
        _logCaptureWriteAllowed(PATH_EVENT_META,
                                "upload_checkpoint",
                                millis() - checkpointMetaWriteStart,
                                true);
    }
    return ok;
}

bool StorageManager::compactUploadedEventFiles(const char* sessionIdOverride) {
    if (!_ready) return false;

    if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY) {
        DLOG_INFO("STORAGE",
                  "Uploaded event file compaction deferred owner=%s reason=capture",
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
        return true;
    }

    String sessionId =
        (sessionIdOverride && sessionIdOverride[0]) ?
            String(sessionIdOverride) : SESS.getId();

    const bool ok = compactSpool();
    if (ok && !_spoolAuditRepairRequired &&
        _counterTrustState != CounterTrust::Degraded &&
        _counterTrustState != CounterTrust::RepairRequired &&
        _counterTrustState != CounterTrust::EmergencyOnly) {
        _pendingCountDirty = false;
    }
    _persistEventMeta(true, "compact_uploaded");
    refreshStorageUiState();

    if (sessionId.length()) {
        DLOG_INFO("STORAGE",
                  "Spool compact request session=%s result=%d pendingUpload=%lu",
                  sessionId.c_str(),
                  ok ? 1 : 0,
                  static_cast<unsigned long>(_pendingEventCount));
    } else {
        DLOG_INFO("STORAGE",
                  "Spool compact request result=%d pendingUpload=%lu",
                  ok ? 1 : 0,
                  static_cast<unsigned long>(_pendingEventCount));
    }

    return ok;
}

int StorageManager::compactAllUploadedEventFiles() {
    if (!_ready) return 0;
    std::vector<String> sessionIds;
    listEventSessions(sessionIds);
    return compactUploadedEventFiles(nullptr) ? static_cast<int>(sessionIds.size()) : 0;
}

uint32_t StorageManager::getPendingEventCount(bool forceRescan) {
    if (!_ready) return 0;

    // Hot path: never scans.  The live counter is updated by every
    // appendEvent / markEventUploaded.  Callers that genuinely need to
    // resolve drift must pass forceRescan=true (or call
    // recountPendingFromSpool() directly), which is reserved for boot,
    // explicit repair, and post-quarantine recovery.
    if (forceRescan) {
        return recountPendingFromSpool();
    }
    return _pendingEventCount;
}

uint32_t StorageManager::recountPendingFromSpool() {
    const RadioOwner owner = RADIO_ARB.currentOwner();
    CONTRACT_WARN_ONCE(CONTRACT_NO_DIRECT_RECOUNT_OUTSIDE_MAINT,
                       "STORAGE",
                       !RADIO_ARB.isReady() ||
                           owner == RADIO_STORAGE_MAINTENANCE,
                       "direct recount owner=%s",
                       RadioArbiter::ownerName(owner));
    _servicePendingSpoolSummaryRebuild();
    _pendingEventCount = _rescanPendingEventCountFromSpool();
    _spoolIndex.pendingTotal = _pendingEventCount;
    _pendingCountDirty = false;
    _spoolAuditRepairRequired = false;
    _spoolSummaryRebuildPending = false;
    _setCounterTrustState(STORAGE_COUNTER_TRUSTED, "recount_pending");
    if (owner == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY) {
        _workerMetadataDirtyPending = true;
        _pendingCountDirty = true;
        DLOG_INFO("STORAGE",
                  "Pending recount meta persist deferred owner=%s reason=capture",
                  RadioArbiter::ownerName(owner));
        return _pendingEventCount;
    }

    _persistEventMeta(true, "recount_pending");
    return _pendingEventCount;
}

uint32_t StorageManager::getPendingEventCountForSession(const char* sessionIdOverride) {
    if (!_ready) return 0;

    String sessionId =
        (sessionIdOverride && sessionIdOverride[0]) ?
            String(sessionIdOverride) : SESS.getId();
    if (!sessionId.length()) return 0;

    return _pendingEventCountForSessionFromSpool(sessionId);
}

uint32_t StorageManager::getSessionPendingEventCount() {
    const String sessionId = SESS.getId();
    if (!sessionId.length()) return 0;

    return _pendingEventCountForSessionFromSpool(sessionId);
}

uint32_t StorageManager::getLastUploadedEventId(const char* sessionIdOverride) {
    if (!_ready) return 0;

    String sessionId =
        (sessionIdOverride && sessionIdOverride[0]) ?
            String(sessionIdOverride) : SESS.getId();
    if (!sessionId.length()) return 0;

    return _uploadedWatermarkForSession(sessionId);
}

void StorageManager::listEventSessions(std::vector<String>& sessionIds) {
    if (_backlog.uploadIndexResident && !_backlog.uploadIndexSessions.empty()) {
        sessionIds = _backlog.uploadIndexSessions;
        return;
    }
    sessionIds = _spoolIndex.sessions;
}

uint32_t StorageManager::_lastUploadedEventIdForSession(const String& sessionId) {
    if (!sessionId.length()) return 0;
    return _uploadedWatermarkForSession(sessionId);
}

uint32_t StorageManager::_pendingEventCountForSession(const String& sessionId) {
    if (!sessionId.length()) return 0;
    return _pendingEventCountForSessionFromSpool(sessionId);
}

uint32_t StorageManager::getNextEventId() {
    return _nextEventId;
}

uint32_t StorageManager::getStoredRecordCount() const {
    uint32_t total = 0;
    for (const auto& seg : _spoolIndex.segments) {
        if (seg.recordCount > 0U) {
            total += seg.recordCount;
        } else if (seg.eventCount > 0U || seg.enrichDeltaCount > 0U) {
            total += seg.eventCount + seg.enrichDeltaCount;
        } else if (seg.firstEventId != 0U &&
                   seg.lastEventId >= seg.firstEventId) {
            total += seg.lastEventId - seg.firstEventId + 1U;
        }
    }

    return total;
}

uint32_t StorageManager::getStoredEventCount() const {
    uint32_t total = 0;
    for (const auto& seg : _spoolIndex.segments) {
        if (seg.eventCount > 0U) {
            total += seg.eventCount;
        } else if (seg.recordCount > seg.enrichDeltaCount) {
            total += seg.recordCount - seg.enrichDeltaCount;
        } else if (seg.firstEventId != 0U &&
                   seg.lastEventId >= seg.firstEventId) {
            total += seg.lastEventId - seg.firstEventId + 1U;
        }
    }

    return total;
}

uint32_t StorageManager::getDisplayRecordCount() const {
    const uint32_t indexed = getStoredRecordCount();
    if (!_storedRecordCountCacheValid) {
        return indexed;
    }

    if (indexed >= _storedRecordCountIndexBase) {
        return _storedRecordCountExactBase +
               (indexed - _storedRecordCountIndexBase);
    }

    const uint32_t removed = _storedRecordCountIndexBase - indexed;
    return (_storedRecordCountExactBase > removed)
               ? (_storedRecordCountExactBase - removed)
               : indexed;
}

uint32_t StorageManager::getDisplayEventCount() const {
    return getStoredEventCount();
}

void StorageManager::_setStoredRecordCountCache(uint32_t exactTotal,
                                                uint32_t nowMs) {
    _storedRecordCountExactBase = exactTotal;
    _storedRecordCountIndexBase = getStoredRecordCount();
    _storedRecordCountUpdatedMs = nowMs;
    _storedRecordCountCacheValid = true;
}

bool StorageManager::refreshStoredRecordCountCache(uint32_t nowMs, bool force) {
    static constexpr uint32_t REFRESH_INTERVAL_MS = 30000UL;
    if (!_ready) {
        return false;
    }
    if (!force &&
        _storedRecordCountCacheValid &&
        (nowMs - _storedRecordCountUpdatedMs) < REFRESH_INTERVAL_MS) {
        return false;
    }
    if (!force && RADIO_ARB.currentOwner() != RADIO_NONE) {
        return false;
    }

    SpoolAuditResult audit;
    const bool ok = _auditAndRepairSpool(
        force ? "forced_count_refresh" : "ui_count_refresh",
        false,
        &audit);
    if (!ok || audit.hadFatalSegmentError) {
        return false;
    }

    _setStoredRecordCountCache(audit.totalValidRecords(), nowMs);
    (void)_persistEventMeta(true, "ui_count_refresh_total");
    return true;
}

// =====================================================================
// Headers-only scan: decode the minimum metadata per record (id, ts,
// session, type, flags) and skip the payload-family-specific body decode.
// No JsonDocument per record — drops the heap pressure that caused the
// manual-enrich crash at 22k+ backlog.
// =====================================================================

bool StorageManager::_scanSegmentRecordHeaders(
    uint32_t segmentId,
    std::function<bool(const DecodedSpoolRecordHeader&)> cb) const {

    CONTRACT_WARN_ONCE(CONTRACT_NO_FS_SCAN_DURING_CAPTURE,
                       "STORAGE",
                       RADIO_ARB.currentOwner() != RADIO_WIFI_CAPTURE,
                       "segment=%lu owner=%s",
                       static_cast<unsigned long>(segmentId),
                       RadioArbiter::ownerName(RADIO_ARB.currentOwner()));

    const SpoolSegmentInfo* seg = _findSegmentInfo(segmentId);
    if (!seg) return false;

    switch (seg->format) {
        case SPOOL_SEGMENT_BIN_V2:
            return _scanBinarySegmentRecordHeaders(segmentId, cb);
        case SPOOL_SEGMENT_JSONL:
        default:
            return _scanJsonlSegmentRecordHeaders(segmentId, cb);
    }
}

bool StorageManager::_scanJsonlSegmentRecordHeaders(
    uint32_t segmentId,
    std::function<bool(const DecodedSpoolRecordHeader&)> cb) const {
    // JSONL segments still need full-line JSON parse to find type/session/id.
    // This fallback wraps the existing full scanner and projects the fields
    // we expose in the header view. JSONL segments are legacy / minority;
    // the hot path is binary.
    return _scanJsonlSegmentRecords(segmentId,
        [&](const DecodedSpoolRecord& rec) -> bool {
            DecodedSpoolRecordHeader hdr;
            hdr.recordType = rec.recordType;
            hdr.eventId = rec.eventId;
            hdr.sessionId = rec.sessionId;
            JsonObjectConst doc = rec.doc.as<JsonObjectConst>();
            hdr.timestampMs = doc[F_TIMESTAMP] | 0U;
            hdr.typeString = String((const char*)(doc["type"] | ""));
            // JSONL records don't carry the binary eventFlags/payloadFamily
            // bytes — leave them 0. Callers that depend on those should use
            // the binary scanner. enrich_state lives in the JSON body.
            if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
                hdr.targetEventId = doc["event_id"] | 0U;
            }
            return cb(hdr);
        });
}

bool StorageManager::_scanBinarySegmentRecordHeaders(
    uint32_t segmentId,
    std::function<bool(const DecodedSpoolRecordHeader&)> cb) const {

    const String path = _spoolBinarySegmentPath(segmentId);
    if (!LittleFS.exists(path)) return false;

    File f = LittleFS.open(path, "r");
    if (!f) return false;

    SpoolBin::SegmentHeaderV2 hdr;
    if (!SpoolBin::readSegmentHeaderV2(f, hdr)) {
        f.close();
        return false;
    }
    if (hdr.magic != SpoolBin::SEGMENT_MAGIC || hdr.version != 2) {
        f.close();
        return false;
    }
    if (!f.seek(sizeof(SpoolBin::SegmentHeaderV2))) {
        f.close();
        return false;
    }

    const uint32_t tsBase = hdr.createdMs;
    const uint32_t epochBase = hdr.createdEpochUtc;
    String lastSession;
    String lastSessionTag;
    // Delta-coded enrichment records resolve against the previous enrichment
    // record in this segment; the context resets with each segment scan.
    BinaryEnrichContext enrichCtx;

    // Per-record body buffer reused across iterations. Capacity grows to
    // accommodate the largest body seen; never re-allocates after that.
    // This eliminates per-record malloc/free in the loop and is the key
    // heap-pressure reduction vs the JsonDocument-per-record approach.
    std::vector<uint8_t> body;

    // Yield to the scheduler periodically. The exclusive maintenance window
    // pauses other storage work, but the task watchdog still expects
    // TaskHardware to feed it. A large segment (500+ records) takes ~100ms
    // and several back-to-back segments can blow past TWDT's ~5s default,
    // causing a silent reset. yieldEveryNRecords keeps each yield bounded.
    constexpr uint32_t kYieldEveryNRecords = 32U;
    uint32_t recordsSinceYield = 0;

    while (f.position() < f.size()) {
        if (++recordsSinceYield >= kYieldEveryNRecords) {
            recordsSinceYield = 0;
            vTaskDelay(1);
        }

        SpoolBin::RecordPrefix prefix;
        if (!SpoolBin::readBytes(f, &prefix, sizeof(prefix))) {
            f.close();
            return false;
        }

        const size_t remainingAfterPrefix =
            static_cast<size_t>(f.size() - f.position());
        if (prefix.length > remainingAfterPrefix) {
            DLOG_WARN("STORAGE",
                      "Binary spool header scan truncated body seg=%lu len=%u remaining=%u",
                      static_cast<unsigned long>(segmentId),
                      static_cast<unsigned>(prefix.length),
                      static_cast<unsigned>(remainingAfterPrefix));
            f.close();
            return false;
        }

        if (body.size() < prefix.length) {
            body.resize(prefix.length);
        }
        if (prefix.length > 0) {
            if (!SpoolBin::readBytes(f, body.data(), prefix.length)) {
                f.close();
                return false;
            }
        }

        if (prefix.type == SpoolBin::REC_CHECKPOINT) {
            continue;
        }

        const uint8_t* p = body.data();
        const uint8_t* end = body.data() + prefix.length;

        DecodedSpoolRecordHeader rec;

        if (prefix.type == SpoolBin::REC_ENRICH_DELTA ||
            prefix.type == SpoolBin::REC_ENRICH_DELTA_V2) {
            DecodedEnrichDelta d;
            if (!_decodeBinaryEnrichDeltaBody(p, end, prefix.type, enrichCtx,
                                              lastSession, lastSessionTag, d)) {
                f.close();
                return false;
            }
            rec.recordType = SPOOL_REC_ENRICH_DELTA;
            rec.eventId = d.recordId;
            rec.sessionId = d.sessionId;
            rec.timestampMs = _timestampFromBaseDelta(d.tsDelta, tsBase);
            rec.epochUtc = _epochFromBaseDelta(d.tsDelta, epochBase);
            rec.typeString = String("enrich_delta");
            rec.eventFlags = d.flags;
            rec.targetEventId = d.targetEventId;
        } else {
            uint32_t recordId = 0;
            uint32_t tsDelta = 0;
            if (!_readUVarintFromBytes(p, end, recordId) ||
                !_readUVarintFromBytes(p, end, tsDelta)) {
                f.close();
                return false;
            }
            if (p >= end) { f.close(); return false; }
            const uint8_t sessionMode = *p++;
            String sessionId;
            String sessionTag;
            if (!_readBinarySessionField(p, end, sessionMode, lastSession,
                                         lastSessionTag, sessionId, sessionTag)) {
                f.close();
                return false;
            }
            if (p >= end) { f.close(); return false; }
            const uint8_t typeCode = *p++;
            String typeStr;
            if (typeCode == BIN_EVT_CUSTOM) {
                if (!_readStringFromBytes(p, end, typeStr)) { f.close(); return false; }
            } else {
                typeStr = _binaryEventTypeStringFromCode(typeCode);
            }
            if (p >= end) { f.close(); return false; }
            const uint8_t eventFlags = *p++;
            if (p >= end) { f.close(); return false; }
            const uint8_t payloadFamily = *p++;

            rec.recordType = SPOOL_REC_EVENT;
            rec.eventId = recordId;
            rec.sessionId = sessionId;
            rec.timestampMs = _timestampFromBaseDelta(tsDelta, tsBase);
            rec.epochUtc = _epochFromBaseDelta(tsDelta, epochBase);
            rec.typeString = typeStr;
            rec.eventFlags = eventFlags;
            rec.payloadFamily = payloadFamily;
        }

        if (!cb(rec)) {
            f.close();
            return true;
        }
        // Remainder of body (payload-family-specific bytes) is not parsed —
        // simply discarded by virtue of the next iteration's seek being
        // relative to the file position, not to where parsing stopped.
        // f.position() is already past the body bytes because we read them
        // into `body` above.
    }

    f.close();
    return true;
}

bool StorageManager::_scanSegmentRecords(
    uint32_t segmentId,
    std::function<bool(const DecodedSpoolRecord&)> cb) const {

    CONTRACT_WARN_ONCE(CONTRACT_NO_FS_SCAN_DURING_CAPTURE,
                       "STORAGE",
                       RADIO_ARB.currentOwner() != RADIO_WIFI_CAPTURE,
                       "segment=%lu owner=%s",
                       static_cast<unsigned long>(segmentId),
                       RadioArbiter::ownerName(RADIO_ARB.currentOwner()));

    const SpoolSegmentInfo* seg = _findSegmentInfo(segmentId);
    if (!seg) {
        return false;
    }

    switch (seg->format) {
        case SPOOL_SEGMENT_BIN_V2:
            return _scanBinarySegmentRecords(segmentId, cb);
        case SPOOL_SEGMENT_JSONL:
        default:
            return _scanJsonlSegmentRecords(segmentId, cb);
    }
}

bool StorageManager::_scanJsonlSegmentRecords(
    uint32_t segmentId,
    std::function<bool(const DecodedSpoolRecord&)> cb) const {

    const String path = _spoolSegmentPathForFormat(segmentId, SPOOL_SEGMENT_JSONL);
    if (!LittleFS.exists(path)) return false;

    File f = LittleFS.open(path, "r");
    if (!f) return false;

    // Yield to the scheduler periodically so a large legacy JSONL segment can't
    // starve the task watchdog (~5s TWDT) and silently reset the device — the
    // binary scanner does the same. This path is hit by the up-front summary
    // rebuild before an upload, which can scan many segments back-to-back.
    constexpr uint32_t kYieldEveryNRecords = 32U;
    uint32_t recordsSinceYield = 0;

    while (f.available()) {
        if (++recordsSinceYield >= kYieldEveryNRecords) {
            recordsSinceYield = 0;
            vTaskDelay(1);
        }

        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length()) continue;

        JsonDocument doc;
        if (deserializeJson(doc, line)) {
            continue;
        }

        DecodedSpoolRecord rec;
        rec.eventId = doc["id"] | 0U;
        rec.sessionId = String(doc[F_SESSION] | doc["session_id"] | "");

        const char* recType = doc["type"] | "";
        if (strcmp(recType, "enrich_delta") == 0) {
            rec.recordType = SPOOL_REC_ENRICH_DELTA;
        } else {
            rec.recordType = SPOOL_REC_EVENT;
        }

        if (rec.recordType == SPOOL_REC_EVENT) {
            _normalizeCapturedEvent(doc.as<JsonObject>());
        }

        rec.doc = doc;

        if (!cb(rec)) {
            f.close();
            return true;
        }
    }

    f.close();
    return true;
}

bool StorageManager::_scanBinarySegmentRecords(
    uint32_t segmentId,
    std::function<bool(const DecodedSpoolRecord&)> cb) const {

    const String path = _spoolBinarySegmentPath(segmentId);
    if (!LittleFS.exists(path)) {
        return false;
    }

    File f = LittleFS.open(path, "r");
    if (!f) {
        return false;
    }

    SpoolBin::SegmentHeaderV2 hdr;
    if (!SpoolBin::readSegmentHeaderV2(f, hdr)) {
        DLOG_WARN("STORAGE", "Binary spool header read failed seg=%lu path=%s",
                  static_cast<unsigned long>(segmentId),
                  path.c_str());
        f.close();
        return false;
    }

    if (hdr.magic != SpoolBin::SEGMENT_MAGIC || hdr.version != 2) {
        DLOG_WARN("STORAGE", "Binary spool invalid header seg=%lu path=%s",
                  static_cast<unsigned long>(segmentId),
                  path.c_str());
        f.close();
        return false;
    }

    if (!f.seek(sizeof(SpoolBin::SegmentHeaderV2))) {
        f.close();
        return false;
    }

    const uint32_t tsBase = hdr.createdMs;
    // Reboot-safe absolute UTC base for this segment (0 => no trusted clock at
    // creation/backfill, record is unenrichable and has no recoverable UTC).
    const uint32_t epochBase = hdr.createdEpochUtc;
    String lastSession;
    String lastSessionTag;
    // Delta-coded enrichment records resolve against the previous enrichment
    // record in this segment; the context resets with each segment scan.
    BinaryEnrichContext enrichCtx;

    // Yield to the scheduler periodically. Callers run this under an exclusive
    // maintenance/upload window, but the task watchdog still expects
    // TaskHardware to be fed. A full record decode of a 500+ record segment —
    // and especially several back-to-back segment scans (e.g. the upload
    // enrich-delta back-scan) — easily exceeds the ~5s TWDT and silently resets
    // the device. This mirrors the yield cadence in _scanSegmentRecordHeaders.
    constexpr uint32_t kYieldEveryNRecords = 128U;
    uint32_t recordsSinceYield = 0;

    while (f.position() < f.size()) {
        if (++recordsSinceYield >= kYieldEveryNRecords) {
            recordsSinceYield = 0;
            vTaskDelay(1);
        }

        SpoolBin::RecordPrefix prefix;
        if (!SpoolBin::readBytes(f, &prefix, sizeof(prefix))) {
            DLOG_WARN("STORAGE", "Binary spool prefix read failed seg=%lu",
                      static_cast<unsigned long>(segmentId));
            f.close();
            return false;
        }

        // Validate length against remaining file bytes BEFORE allocating —
        // mirrors the guards at _loadBinaryMetaRecords (~line 940) and the
        // audit scanner (~line 1141). Without this, a corrupt prefix.length
        // (up to 64KB) would always trigger a 64KB heap allocation that
        // fails the readBytes check anyway, but the alloc itself can OOM
        // a memory-pressured device during the corruption-recovery path.
        const size_t remainingAfterPrefix =
            static_cast<size_t>(f.size() - f.position());
        if (prefix.length > remainingAfterPrefix) {
            DLOG_WARN("STORAGE",
                      "Binary spool truncated body seg=%lu len=%u remaining=%u",
                      static_cast<unsigned long>(segmentId),
                      static_cast<unsigned>(prefix.length),
                      static_cast<unsigned>(remainingAfterPrefix));
            f.close();
            return false;
        }

        std::vector<uint8_t> body(prefix.length);
        if (prefix.length > 0) {
            if (!SpoolBin::readBytes(f, body.data(), prefix.length)) {
                DLOG_WARN("STORAGE", "Binary spool body read failed seg=%lu len=%u",
                          static_cast<unsigned long>(segmentId),
                          static_cast<unsigned>(prefix.length));
                f.close();
                return false;
            }
        }

        if (prefix.type == SpoolBin::REC_CHECKPOINT) {
            continue;
        }

        const uint8_t* p = body.data();
        const uint8_t* end = body.data() + body.size();

        JsonDocument doc;
        JsonObject root = doc.to<JsonObject>();
        DecodedSpoolRecord rec;

        if (prefix.type == SpoolBin::REC_ENRICH_DELTA ||
            prefix.type == SpoolBin::REC_ENRICH_DELTA_V2) {
            DecodedEnrichDelta d;
            if (!_decodeBinaryEnrichDeltaBody(p, end, prefix.type, enrichCtx,
                                              lastSession, lastSessionTag, d)) {
                DLOG_WARN("STORAGE", "Binary enrich decode failed seg=%lu",
                          static_cast<unsigned long>(segmentId));
                f.close();
                return false;
            }

            root["id"] = d.recordId;
            const uint32_t ts = _timestampFromBaseDelta(d.tsDelta, tsBase);
            root["ts"] = ts;
            root["type"] = "enrich_delta";
            root[F_SESSION] = d.sessionId;
            root["event_id"] = d.targetEventId;
            root["lat"] = _e7ToFloat(d.latE7);
            root["lon"] = _e7ToFloat(d.lonE7);
            root["alt"] = _cmToFloat(d.altCm);
            root["acc"] = _dmToFloat(d.accDm);
            if (d.flags & ENRICH_FLAG_TAG) root["tag"] = d.tag;
            if (d.gpsEpochUtc >= MIN_ENRICH_GPS_EPOCH) {
                root[F_GPS_TS] = d.gpsEpochUtc;
            }
            // Surface the NO_DATA sentinel so downstream readers can branch
            // on it (e.g., emit STORAGE_ENRICH_NO_DATA instead of writing
            // the zero coords as a fix).
            if (d.flags & ENRICH_FLAG_NO_DATA) {
                root["enrich_no_data"] = true;
            }
            {
                // Prefer the reboot-safe persisted epoch (createdEpochUtc +
                // delta/1000); the live-clock projection from a boot-relative
                // millis is wrong for any record carried across a reboot. Fall
                // back to the projection only when no UTC base was ever stamped.
                char tsIso[24] = {};
                const uint32_t epochUtc = _epochFromBaseDelta(d.tsDelta, epochBase);
                if (epochUtc != 0) {
                    TIME_SVC.formatIsoForEpoch(epochUtc, tsIso, sizeof(tsIso));
                } else {
                    TIME_SVC.formatIsoForMillis(ts, tsIso, sizeof(tsIso));
                }
                root[F_TIMESTAMP_ISO] = tsIso;
            }

            rec.recordType = SPOOL_REC_ENRICH_DELTA;
            rec.eventId = d.recordId;
            rec.sessionId = d.sessionId;
            rec.doc.set(doc.as<JsonVariantConst>());

        } else {
            uint32_t recordId = 0;
            uint32_t tsDelta = 0;
            uint8_t sessionMode = BIN_SESSION_INLINE;
            String sessionId;
            String sessionTag;
            String tsIsoStr;
            uint8_t typeCode = BIN_EVT_CUSTOM;
            String typeStr;
            uint8_t eventFlags = 0;
            uint8_t prio = 0;
            uint8_t lane = 0;
            uint8_t payloadFamily = BIN_PAYLOAD_JSON_FALLBACK;

            if (!_readUVarintFromBytes(p, end, recordId) ||
                !_readUVarintFromBytes(p, end, tsDelta)) {
                DLOG_WARN("STORAGE", "Binary event decode failed seg=%lu",
                          static_cast<unsigned long>(segmentId));
                f.close();
                return false;
            }

            if (p >= end) {
                f.close();
                return false;
            }
            sessionMode = *p++;

            if (!_readBinarySessionField(p, end, sessionMode, lastSession,
                                         lastSessionTag, sessionId, sessionTag)) {
                f.close();
                return false;
            }

            if (p >= end) {
                f.close();
                return false;
            }
            typeCode = *p++;

            if (typeCode == BIN_EVT_CUSTOM) {
                if (!_readStringFromBytes(p, end, typeStr)) {
                    f.close();
                    return false;
                }
            } else {
                typeStr = _binaryEventTypeStringFromCode(typeCode);
            }

            if (p >= end) {
                f.close();
                return false;
            }
            eventFlags = *p++;

            if (p >= end) {
                f.close();
                return false;
            }
            payloadFamily = *p++;

            root["id"] = recordId;
            const uint32_t ts = _timestampFromBaseDelta(tsDelta, tsBase);
            root["ts"] = ts;
            root["type"] = typeStr;
            root[F_SESSION] = sessionId;
            {
                // Prefer the reboot-safe persisted epoch (createdEpochUtc +
                // delta/1000); the live-clock projection from a boot-relative
                // millis is wrong for any record carried across a reboot. Fall
                // back to the projection only when no UTC base was ever stamped.
                char tsIso[24] = {};
                const uint32_t epochUtc = _epochFromBaseDelta(tsDelta, epochBase);
                rec.epochUtc = epochUtc;
                if (epochUtc != 0) {
                    TIME_SVC.formatIsoForEpoch(epochUtc, tsIso, sizeof(tsIso));
                } else {
                    TIME_SVC.formatIsoForMillis(ts, tsIso, sizeof(tsIso));
                }
                tsIsoStr = tsIso;
                root[F_TIMESTAMP_ISO] = tsIsoStr;
            }

            if (!_decodeBinaryPayloadBody(p, end, typeStr, payloadFamily,
                                          eventFlags, root)) {
                DLOG_WARN("STORAGE",
                          "Binary payload decode failed seg=%lu family=%u",
                          static_cast<unsigned long>(segmentId),
                          static_cast<unsigned>(payloadFamily));
                f.close();
                return false;
            }

            if (eventFlags & BIN_EVENT_HAS_FIELDS) {
                if (!_readBinaryFieldMapFromBytes(p, end, root)) {
                    DLOG_WARN("STORAGE", "Binary event extension decode failed seg=%lu",
                              static_cast<unsigned long>(segmentId));
                    f.close();
                    return false;
                }
            }

            String eventTypeStr = String((const char*)(root["event_type"] | ""));
            prio = _defaultPriorityForBinaryType(typeStr, eventTypeStr);
            lane = _defaultLaneForBinaryType(typeStr, eventTypeStr);

            if (eventFlags & BIN_EVENT_HAS_PRIO) {
                if (p >= end) {
                    f.close();
                    return false;
                }
                prio = *p++;
            }
            if (eventFlags & BIN_EVENT_HAS_LANE) {
                if (p >= end) {
                    f.close();
                    return false;
                }
                lane = *p++;
            }

            root["prio"] = prio;
            root["lane"] = lane;
            root["lane_name"] = _laneText(static_cast<StorageLane>(lane));

            // Re-assert after the extension map, but only when the segment had
            // a UTC base to project from. _readBinaryFieldMapFromBytes assigns
            // into root, so without this the stale stored copy clobbers the
            // reboot-safe value. With epochBase == 0 the computed value is a
            // projection from a PREVIOUS boot's millis and is meaningless --
            // there the stored capture-time string, written during the boot
            // that captured the record, is the better of the two.
            if (rec.epochUtc != 0 || !root[F_TIMESTAMP_ISO].is<const char*>() ||
                !(root[F_TIMESTAMP_ISO].as<const char*>()[0])) {
                root[F_TIMESTAMP_ISO] = tsIsoStr;
            }
            _applyDerivedEventFields(root, typeStr, sessionId, sessionTag);

            _normalizeCapturedEvent(root);

            rec.recordType = SPOOL_REC_EVENT;
            rec.eventId = recordId;
            rec.sessionId = sessionId;
            rec.doc.set(doc.as<JsonVariantConst>());
        }

        if (!cb(rec)) {
            f.close();
            return true;
        }
    }

    f.close();
    return true;
}

bool StorageManager::_ensureWorkerAppendFileOpen(SpoolSegmentInfo& seg) {
    if (seg.format != SPOOL_SEGMENT_BIN_V2 || seg.segmentId == 0) {
        return false;
    }

    if (_workerAppendFile && _workerAppendSegmentId == seg.segmentId &&
        _workerAppendHeaderOk) {
        if (!_workerAppendFile.seek(_workerAppendWriteOffset)) {
            _closeWorkerAppendFile("append_seek_failed");
            return false;
        }
        return true;
    }

    if (_workerAppendFile) {
        if (!_flushWorkerAppendFile("append_segment_switch", true)) {
            _closeWorkerAppendFile("append_segment_switch_failed");
            return false;
        }
    }

    const String segPath = _spoolBinarySegmentPath(seg.segmentId);
    _workerAppendFile = LittleFS.open(segPath, "r+");
    if (!_workerAppendFile) {
        _workerAppendSegmentId = 0;
        _workerAppendHeaderOk = false;
        return false;
    }

    if (!SpoolBin::readSegmentHeaderV2(_workerAppendFile, _workerAppendHeader) ||
        _workerAppendHeader.magic != SpoolBin::SEGMENT_MAGIC ||
        _workerAppendHeader.version != 2) {
        _closeWorkerAppendFile("append_header_invalid");
        return false;
    }

    _workerAppendWriteOffset = static_cast<uint32_t>(_workerAppendFile.size());
    if (!_workerAppendFile.seek(_workerAppendWriteOffset)) {
        _closeWorkerAppendFile("append_seek_eof_failed");
        return false;
    }

    _workerAppendSegmentId = seg.segmentId;
    _workerAppendHeaderOk = true;
    _workerAppendHeaderDirty = false;
    _workerAppendRecordsSinceFlush = 0;
    return true;
}

bool StorageManager::_flushWorkerAppendFile(const char* reason, bool closeFile) {
    if (!_workerAppendFile) {
        _workerAppendSegmentId = 0;
        _workerAppendWriteOffset = 0;
        _workerAppendHeaderOk = false;
        _workerAppendHeaderDirty = false;
        _workerAppendRecordsSinceFlush = 0;
        return true;
    }

    const uint32_t startMs = millis();
    const uint32_t recordsSinceFlush = _workerAppendRecordsSinceFlush;
    const uint32_t segmentId = _workerAppendSegmentId;
    bool ok = true;

    if (_workerAppendHeaderDirty) {
        ok = SpoolBin::writeSegmentHeaderV2(_workerAppendFile, _workerAppendHeader);
        if (ok) {
            _workerAppendFile.flush();
            _workerAppendHeaderDirty = false;
            _workerAppendRecordsSinceFlush = 0;
        }
    } else if (closeFile) {
        _workerAppendFile.flush();
    }

    if (ok && !closeFile) {
        ok = _workerAppendFile.seek(_workerAppendWriteOffset);
    }

    const uint32_t elapsed = millis() - startMs;
    if (elapsed >= 250UL || !ok) {
        DLOG_WARN("STORAGE",
                  "worker append file flush reason=%s seg=%lu records=%lu ms=%lu close=%d ok=%d",
                  (reason && reason[0]) ? reason : "-",
                  static_cast<unsigned long>(segmentId),
                  static_cast<unsigned long>(recordsSinceFlush),
                  static_cast<unsigned long>(elapsed),
                  closeFile ? 1 : 0,
                  ok ? 1 : 0);
    }

    if (closeFile) {
        _workerAppendFile.close();
        _workerAppendSegmentId = 0;
        _workerAppendWriteOffset = 0;
        _workerAppendHeaderOk = false;
        _workerAppendHeaderDirty = false;
        _workerAppendRecordsSinceFlush = 0;
    }

    return ok;
}

void StorageManager::_closeWorkerAppendFile(const char* reason) {
    (void)_flushWorkerAppendFile(reason, true);
}

bool StorageManager::_appendSegmentRecord(SpoolSegmentInfo& seg,
                                          JsonDocument& doc,
                                          uint32_t* outEventId,
                                          SpoolBin::AppendRecordLocation* outLoc,
                                          QueuedAppendTiming* timing) {
    (void)timing;
    switch (seg.format) {
        case SPOOL_SEGMENT_BIN_V2: {
            const uint32_t recordId = doc["id"] | 0U;
            const uint32_t ts = doc["ts"] | 0U;

            const bool useWorkerAppendFile = _workerAppendBatchActive;
            File localSegFile;
            File* segFile = nullptr;
            SpoolBin::SegmentHeaderV2 segHdr{};
            uint32_t writeOffset = 0;

            if (useWorkerAppendFile) {
                if (!_ensureWorkerAppendFileOpen(seg)) {
                    return false;
                }
                segFile = &_workerAppendFile;
                segHdr = _workerAppendHeader;
                writeOffset = _workerAppendWriteOffset;
            } else {
                if (_workerAppendFile && _workerAppendSegmentId == seg.segmentId) {
                    if (!_flushWorkerAppendFile("sync_append", true)) {
                        return false;
                    }
                }
                const String segPath = _spoolBinarySegmentPath(seg.segmentId);
                localSegFile = LittleFS.open(segPath, "r+");
                if (!localSegFile) return false;

                if (!SpoolBin::readSegmentHeaderV2(localSegFile, segHdr)) {
                    localSegFile.close();
                    return false;
                }

                if (segHdr.magic != SpoolBin::SEGMENT_MAGIC || segHdr.version != 2) {
                    localSegFile.close();
                    return false;
                }

                writeOffset = static_cast<uint32_t>(localSegFile.size());
                if (!localSegFile.seek(writeOffset)) {
                    localSegFile.close();
                    return false;
                }
                segFile = &localSegFile;
            }

            auto failAppend = [&]() -> bool {
                if (useWorkerAppendFile) {
                    _closeWorkerAppendFile("append_build_failed");
                } else if (localSegFile) {
                    localSegFile.close();
                }
                return false;
            };

            const uint32_t tsDelta = _timestampDeltaFromBase(ts, segHdr.createdMs);
            const String sessionId = String((const char*)(doc[F_SESSION] | doc["session_id"] | ""));
            const String typeStr = String((const char*)(doc["type"] | ""));
            const String eventSubtype = String((const char*)(doc["event_type"] | ""));

            std::vector<uint8_t> body;
            body.reserve(96);

            _appendUVarintToBytes(body, recordId);
            _appendUVarintToBytes(body, tsDelta);

            // session_tag rides along with the session string instead of being
            // repeated on every record: it only costs bytes when the session
            // (or the tag) actually changes.
            const String sessionTag = String((const char*)(doc["session_tag"] | ""));
            const String lastSession = _binaryLastSessionBySegment[seg.segmentId];
            const String lastSessionTag = _binaryLastSessionTagBySegment[seg.segmentId];
            const bool sameSession = (sessionId.length() &&
                                      sessionId == lastSession &&
                                      sessionTag == lastSessionTag);
            if (sameSession) {
                body.push_back(BIN_SESSION_SAME_AS_PREV);
            } else {
                body.push_back(sessionTag.length() ? BIN_SESSION_INLINE_TAGGED
                                                   : BIN_SESSION_INLINE);
                _appendStringToBytes(body, sessionId);
                if (sessionTag.length()) _appendStringToBytes(body, sessionTag);
            }

            const uint8_t typeCode = _binaryEventTypeCodeFromString(typeStr.c_str());
            body.push_back(typeCode);
            if (typeCode == BIN_EVT_CUSTOM) {
                _appendStringToBytes(body, typeStr);
            }

            const uint8_t prio = static_cast<uint8_t>(doc["prio"] | 0U);
            const uint8_t lane = static_cast<uint8_t>(doc["lane"] | 0U);

            const uint8_t defaultPrio = _defaultPriorityForBinaryType(typeStr, eventSubtype);
            const uint8_t defaultLane = _defaultLaneForBinaryType(typeStr, eventSubtype);

            uint8_t eventFlags = 0;
            if (prio != defaultPrio) eventFlags |= BIN_EVENT_HAS_PRIO;
            if (lane != defaultLane) eventFlags |= BIN_EVENT_HAS_LANE;

            const size_t eventFlagsOffset = body.size();
            body.push_back(eventFlags);

            const bool isProbeLike =
                (typeCode == BIN_EVT_PROBE || typeCode == BIN_EVT_DEVICE ||
                 typeCode == BIN_EVT_NETWORK);
            const bool isPmkid = (typeCode == BIN_EVT_PMKID);
            const bool isDrone = (typeCode == BIN_EVT_DRONE);
            const bool isHandshake =
                (typeCode == BIN_EVT_EVENT && eventSubtype == "handshake");
            bool hasExtensionFields = false;
            for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
                if (!_isStructuredBinaryField(typeStr, eventSubtype, kv.key().c_str())) {
                    hasExtensionFields = true;
                    break;
                }
            }

            if (isProbeLike) {
                body.push_back(BIN_PAYLOAD_PROBE_DEVICE_V2);
                _recordBinaryStructuredWrite();

                // network records key the radio address as "bssid"; probe and
                // device records call the same thing "mac".
                const String mac = (typeCode == BIN_EVT_NETWORK)
                    ? String((const char*)(doc["bssid"] | doc["mac"] | ""))
                    : String((const char*)(doc["mac"] | ""));
                const String ssid = (typeStr == "probe")
                    ? String((const char*)(doc["probed_ssid"] | doc["ssid"] | ""))
                    : String((const char*)(doc["ssid"] | ""));
                const String ieFingerprint = String((const char*)(doc["ie_fingerprint"] | ""));
                const String probeSetHash = String((const char*)(doc["probe_set_hash"] | ""));
                // as<T>() converts across numeric variant types; `| 0` returns
                // the default for anything is<int>() rejects (e.g. a legacy
                // float-typed record), silently zeroing the field.
                const int32_t rssi = doc["rssi"].as<int32_t>();
                const uint32_t channel = doc["channel"].as<uint32_t>();
                const bool isRandomMac = (doc["is_random_mac"] | 0) != 0;
                const bool isBroadcast = (doc["is_broadcast"] | 0) != 0;

                // --- sidecar block (flags2) ---
                const String security = String((const char*)(doc["security"] | ""));
                const String trackId = String((const char*)(doc["track_id"] | ""));
                const bool hasLocalization =
                    !doc["sample_seq"].isNull() || !doc["sample_frames"].isNull() ||
                    !doc["rssi_min"].isNull()   || !doc["rssi_max"].isNull();

                const bool hasRfCtx =
                    !doc["noise_floor"].isNull() || !doc["ant_gain_q2"].isNull();

                const bool hasTxPwr = !doc["tx_power_src"].isNull();

                uint8_t flags2 = 0;
                if (hasRfCtx) flags2 |= BIN_PDV2_RFCTX;
                if (hasTxPwr) flags2 |= BIN_PDV2_TXPWR;
                if (security.length()) flags2 |= BIN_PDV2_SECURITY;
                if (!doc["is_hidden"].isNull()) flags2 |= BIN_PDV2_HIDDEN;
                if (!doc["has_wps"].isNull()) flags2 |= BIN_PDV2_WPS;
                if (hasLocalization) flags2 |= BIN_PDV2_LOCALIZATION;
                if (trackId.length()) flags2 |= BIN_PDV2_TRACK;

                uint8_t flags = 0;
                if (ssid.length()) flags |= BIN_PDV2_SSID;
                if (doc["rssi"].is<int>() || doc["rssi"].is<long>() || doc["rssi"].is<float>())
                    flags |= BIN_PDV2_RSSI;
                if (!doc["channel"].isNull()) flags |= BIN_PDV2_CHANNEL;
                if (ieFingerprint.length()) flags |= BIN_PDV2_IE_FP;
                if (probeSetHash.length()) flags |= BIN_PDV2_PROBE_HASH;
                if (!doc["is_random_mac"].isNull()) flags |= BIN_PDV2_RANDOM_MAC;
                if (!doc["is_broadcast"].isNull()) flags |= BIN_PDV2_BROADCAST;
                if (flags2) flags |= BIN_PDV2_HAS_FLAGS2;
                body.push_back(flags);

                uint8_t lastOui[3] = {0};
                bool hasLastOui = false;

                _appendMacFieldToBytes(body, mac, lastOui, hasLastOui);
                if (flags & BIN_PDV2_SSID) _appendStringToBytes(body, ssid);
                if (flags & BIN_PDV2_RSSI) _appendZigZag32ToBytes(body, rssi);
                if (flags & BIN_PDV2_CHANNEL) _appendUVarintToBytes(body, channel);
                if (flags & BIN_PDV2_IE_FP) _appendStringToBytes(body, ieFingerprint);
                if (flags & BIN_PDV2_PROBE_HASH) _appendStringToBytes(body, probeSetHash);
                if (flags & BIN_PDV2_RANDOM_MAC) body.push_back(isRandomMac ? 1U : 0U);
                if (flags & BIN_PDV2_BROADCAST) body.push_back(isBroadcast ? 1U : 0U);

                if (flags2) {
                    body.push_back(flags2);

                    if (flags2 & BIN_PDV2_SECURITY) {
                        const uint8_t secCode =
                            _binarySecurityCodeFromString(security.c_str());
                        body.push_back(secCode);
                        if (secCode == BIN_SEC_LITERAL) {
                            _appendStringToBytes(body, security);
                        }
                    }
                    if (flags2 & BIN_PDV2_HIDDEN) {
                        body.push_back((doc["is_hidden"] | 0) != 0 ? 1U : 0U);
                    }
                    if (flags2 & BIN_PDV2_WPS) {
                        body.push_back((doc["has_wps"] | 0) != 0 ? 1U : 0U);
                    }
                    if (flags2 & BIN_PDV2_LOCALIZATION) {
                        _appendUVarintToBytes(body, doc["sample_seq"].as<uint32_t>());
                        _appendUVarintToBytes(body, doc["sample_frames"].as<uint32_t>());
                        // rssi_min/rssi_max sit within a few dB of rssi, so
                        // store the delta rather than the absolute value.
                        _appendZigZag32ToBytes(body,
                            doc["rssi_min"].as<int32_t>() - rssi);
                        _appendZigZag32ToBytes(body,
                            doc["rssi_max"].as<int32_t>() - rssi);
                        const String reason =
                            String((const char*)(doc["sample_reason"] | ""));
                        const uint8_t reasonCode =
                            _binarySampleReasonCodeFromString(reason.c_str());
                        body.push_back(reasonCode);
                        if (reasonCode == BIN_REASON_LITERAL) {
                            _appendStringToBytes(body, reason);
                        }
                    }
                    if (flags2 & BIN_PDV2_TRACK) {
                        const uint8_t trackMode =
                            _binaryTrackModeFor(trackId, mac, ieFingerprint);
                        body.push_back(trackMode);
                        if (trackMode == BIN_TRACK_LITERAL) {
                            _appendStringToBytes(body, trackId);
                        }
                    }
                    if (flags2 & BIN_PDV2_TXPWR) {
                        body.push_back(static_cast<uint8_t>(
                            static_cast<int8_t>(doc["tx_power"].as<int32_t>())));
                        body.push_back(static_cast<uint8_t>(
                            doc["tx_power_src"].as<uint32_t>() & 0xFFU));
                    }
                    if (flags2 & BIN_PDV2_RFCTX) {
                        // Both are plain signed bytes: noise floor in dBm,
                        // antenna gain in quarter-dBi. 0 noise floor means the
                        // driver reported none.
                        body.push_back(static_cast<uint8_t>(
                            static_cast<int8_t>(doc["noise_floor"].as<int32_t>())));
                        body.push_back(static_cast<uint8_t>(
                            static_cast<int8_t>(doc["ant_gain_q2"].as<int32_t>())));
                    }
                }

            } else if (isPmkid) {
                body.push_back(BIN_PAYLOAD_PMKID);
                _recordBinaryStructuredWrite();

                const String ap = String((const char*)(doc["ap"] | doc["bssid"] | ""));
                const String sta = String((const char*)(doc["sta"] | doc["client"] | doc["client_mac"] | ""));
                const String ssid = String((const char*)(doc["ssid"] | ""));
                // as<T>() converts across numeric variant types; `| 0` returns
                // the default for anything is<int>() rejects (e.g. a legacy
                // float-typed record), silently zeroing the field.
                const int32_t rssi = doc["rssi"].as<int32_t>();
                const String pmkidHex = String((const char*)(doc["pmkid_hex"] | ""));
                const String hashcatLine = String((const char*)(doc["hashcat_line"] | ""));

                uint8_t flags = 0;
                if (ssid.length()) flags |= 0x01;
                if (!doc["rssi"].isNull()) flags |= 0x02;
                if (pmkidHex.length()) flags |= 0x04;
                if (hashcatLine.length()) flags |= 0x08;
                body.push_back(flags);

                uint8_t lastOui[3] = {0};
                bool hasLastOui = false;

                _appendMacFieldToBytes(body, ap, lastOui, hasLastOui);
                _appendMacFieldToBytes(body, sta, lastOui, hasLastOui);
                if (flags & 0x01) _appendStringToBytes(body, ssid);
                if (flags & 0x02) _appendZigZag32ToBytes(body, rssi);
                if (flags & 0x04) _appendStringToBytes(body, pmkidHex);
                if (flags & 0x08) _appendStringToBytes(body, hashcatLine);

            } else if (isHandshake) {
                body.push_back(BIN_PAYLOAD_HANDSHAKE);
                _recordBinaryStructuredWrite();

                const String ap = String((const char*)(doc["ap"] | doc["bssid"] | ""));
                const String sta = String((const char*)(doc["sta"] | doc["client"] | ""));
                const String ssid = String((const char*)(doc["ssid"] | ""));
                // as<T>() converts across numeric variant types; `| 0` returns
                // the default for anything is<int>() rejects (e.g. a legacy
                // float-typed record), silently zeroing the field.
                const int32_t rssi = doc["rssi"].as<int32_t>();
                const uint32_t frameMask = doc["frame_mask"] | 0U;
                const uint32_t messageNumber = doc["message"] | doc["msg"] | 0U;

                uint8_t flags = 0;
                if (ssid.length()) flags |= 0x01;
                if (!doc["rssi"].isNull()) flags |= 0x02;
                if (!doc["message"].isNull() || !doc["msg"].isNull()) flags |= 0x04;
                body.push_back(flags);

                uint8_t lastOui[3] = {0};
                bool hasLastOui = false;

                _appendMacFieldToBytes(body, ap, lastOui, hasLastOui);
                _appendMacFieldToBytes(body, sta, lastOui, hasLastOui);
                if (flags & 0x01) _appendStringToBytes(body, ssid);
                _appendUVarintToBytes(body, frameMask);
                if (flags & 0x02) _appendZigZag32ToBytes(body, rssi);
                if (flags & 0x04) _appendUVarintToBytes(body, messageNumber);

            } else if (isDrone) {
                body.push_back(BIN_PAYLOAD_DRONE);
                _recordBinaryStructuredWrite();

                const String droneId = String((const char*)(doc["drone_id"] | doc["id"] | ""));
                const String mac = String((const char*)(doc["mac"] | ""));
                // as<T>() converts across numeric variant types; `| 0` returns
                // the default for anything is<int>() rejects (e.g. a legacy
                // float-typed record), silently zeroing the field.
                const int32_t rssi = doc["rssi"].as<int32_t>();
                const uint32_t channel = doc["channel"].as<uint32_t>();
                const String protocol = String((const char*)(doc["protocol"] | ""));
                const bool hasLatLon = !doc["latitude"].isNull() && !doc["longitude"].isNull();
                const uint32_t altitudeCenti = _floatToCenti(doc["altitude_m"] | 0.0f);
                const uint32_t speedCenti = _floatToCenti(doc["speed"] | 0.0f);

                uint8_t flags = 0;
                if (droneId.length()) flags |= 0x01;
                if (mac.length()) flags |= 0x02;
                if (!doc["rssi"].isNull()) flags |= 0x04;
                if (!doc["channel"].isNull()) flags |= 0x08;
                if (protocol.length()) flags |= 0x10;
                if (hasLatLon) flags |= 0x20;
                if (!doc["altitude_m"].isNull()) flags |= 0x40;
                if (!doc["speed"].isNull()) flags |= 0x80;
                body.push_back(flags);

                uint8_t lastOui[3] = {0};
                bool hasLastOui = false;

                if (flags & 0x01) _appendStringToBytes(body, droneId);
                if (flags & 0x02) _appendMacFieldToBytes(body, mac, lastOui, hasLastOui);
                if (flags & 0x04) _appendZigZag32ToBytes(body, rssi);
                if (flags & 0x08) _appendUVarintToBytes(body, channel);
                if (flags & 0x10) _appendStringToBytes(body, protocol);
                if (flags & 0x20) {
                    _appendZigZag32ToBytes(body, _floatToE7(doc["latitude"] | 0.0f));
                    _appendZigZag32ToBytes(body, _floatToE7(doc["longitude"] | 0.0f));
                }
                if (flags & 0x40) _appendUVarintToBytes(body, altitudeCenti);
                if (flags & 0x80) _appendUVarintToBytes(body, speedCenti);

            } else {
                body.push_back(BIN_PAYLOAD_FIELD_MAP);
                _recordBinaryStructuredWrite();
                if (!_appendBinaryFieldMapToBytes(body,
                                                  doc.as<JsonObjectConst>(),
                                                  typeStr,
                                                  eventSubtype)) {
                    return failAppend();
                }
                hasExtensionFields = false;
            }

            if (hasExtensionFields) {
                eventFlags |= BIN_EVENT_HAS_FIELDS;
                if (!_appendBinaryFieldMapToBytes(body,
                                                  doc.as<JsonObjectConst>(),
                                                  typeStr,
                                                  eventSubtype)) {
                    return failAppend();
                }
            }

            body[eventFlagsOffset] = eventFlags;

            if (eventFlags & BIN_EVENT_HAS_PRIO) body.push_back(prio);
            if (eventFlags & BIN_EVENT_HAS_LANE) body.push_back(lane);

            if (body.size() <= eventFlagsOffset + 1) return failAppend();

            SpoolBin::SegmentHeaderV2 hdr;
            SpoolBin::AppendRecordLocation loc{};
            SpoolBin::AppendRecordLocation* locOut = outLoc ? &loc : nullptr;
            const uint32_t appendStartMs = millis();
            const bool appendOk = SpoolBin::appendRecordToOpen(
                    *segFile,
                    static_cast<uint8_t>(SpoolBin::REC_EVENT),
                    body.data(),
                    static_cast<uint16_t>(body.size()),
                    recordId,
                    segHdr);
            if (appendOk) {
                hdr = segHdr;
                if (useWorkerAppendFile) {
                    _workerAppendHeader = hdr;
                    _workerAppendHeaderDirty = true;
                    _workerAppendRecordsSinceFlush++;
                    _workerAppendWriteOffset =
                        writeOffset + static_cast<uint32_t>(sizeof(SpoolBin::RecordPrefix)) +
                        static_cast<uint32_t>(body.size());
                } else if (!SpoolBin::writeSegmentHeaderV2(*segFile, hdr)) {
                    localSegFile.close();
                    return false;
                }
            }
            const uint32_t appendMs = millis() - appendStartMs;
            if (appendMs >= 250UL) {
                DLOG_WARN("STORAGE",
                          "Spool binary append slow ms=%lu seg=%lu event=%lu type=%s bytes=%u",
                          static_cast<unsigned long>(appendMs),
                          static_cast<unsigned long>(seg.segmentId),
                          static_cast<unsigned long>(recordId),
                          typeStr.c_str(),
                          static_cast<unsigned>(body.size()));
            }
            if (!appendOk) {
                if (useWorkerAppendFile) {
                    _closeWorkerAppendFile("append_failed");
                } else {
                    localSegFile.close();
                }
                return false;
            }

            if (locOut) {
                locOut->offset = writeOffset;
                locOut->len = static_cast<uint32_t>(sizeof(SpoolBin::RecordPrefix)) +
                              static_cast<uint32_t>(body.size());
            }
            if (!useWorkerAppendFile) {
                localSegFile.close();
            }

            if (outLoc) {
                *outLoc = loc;
                DLOG_DEBUG("STORAGE",
                    "append loc event=%lu seg=%lu off=%lu len=%lu session=%s lane=%u prio=%u",
                    static_cast<unsigned long>(recordId),
                    static_cast<unsigned long>(seg.segmentId),
                    static_cast<unsigned long>(loc.offset),
                    static_cast<unsigned long>(loc.len),
                    sessionId.c_str(),
                    static_cast<unsigned>(lane),
                    static_cast<unsigned>(prio));
            } else {
                DLOG_DEBUG("STORAGE",
                    "append event=%lu seg=%lu session=%s lane=%u prio=%u",
                    static_cast<unsigned long>(recordId),
                    static_cast<unsigned long>(seg.segmentId),
                    sessionId.c_str(),
                    static_cast<unsigned>(lane),
                    static_cast<unsigned>(prio));
            }

            seg.firstEventId = hdr.firstEventId;
            seg.lastEventId = hdr.lastEventId;
            seg.recordCount = hdr.recordCount;
            seg.approxBytes = hdr.bodyBytes + sizeof(SpoolBin::SegmentHeaderV2);

            _binaryLastSessionBySegment[seg.segmentId] = sessionId;
            _binaryLastSessionTagBySegment[seg.segmentId] = sessionTag;
            _rememberSessionTag(sessionId, sessionTag);

            if (outEventId) *outEventId = recordId;
            return true;
        }

        case SPOOL_SEGMENT_JSONL:
        default: {
            const uint32_t eventId = doc["id"] | 0U;
            const String path = _spoolSegmentPath(seg.segmentId);

            File f = LittleFS.open(path, "a");
            if (!f) return false;
            const uint32_t writeOffset =
                outLoc ? static_cast<uint32_t>(f.size()) : 0U;
            const size_t written = serializeJson(doc, f);
            if (f.print('\n') != 1) {
                f.close();
                return false;
            }
            f.close();

            if (outLoc) {
                outLoc->offset = writeOffset;
                outLoc->len = static_cast<uint32_t>(written + 1U);
            }

            if (seg.recordCount == 0) seg.firstEventId = eventId;
            seg.lastEventId = eventId;
            seg.recordCount++;

            if (outEventId) *outEventId = eventId;
            return true;
        }
    }
}

bool StorageManager::enrichEvent(uint32_t eventId,
                                 float lat, float lon,
                                 float alt, float accuracy,
                                 const char* tag) {
    if (!_ready || eventId == 0) return false;

    String sessionId;
    if (!_findEventSession(eventId, sessionId) || !sessionId.length()) {
        DLOG_WARN("STORAGE",
                  "Enrich failed: event session not found event=%lu",
                  static_cast<unsigned long>(eventId));
        return false;
    }

    return _appendSpoolEnrichmentDelta(sessionId,
                                       eventId,
                                       lat, lon,
                                       alt, accuracy,
                                       tag);
}

bool StorageManager::enrichEventForSession(uint32_t eventId,
                                           const char* sessionIdOverride,
                                           float lat, float lon,
                                           float alt, float accuracy,
                                           const char* tag) {
    if (!_ready || eventId == 0) return false;

    const String sessionId =
        (sessionIdOverride && sessionIdOverride[0]) ?
            String(sessionIdOverride) : String();
    if (!sessionId.length()) {
        DLOG_WARN("STORAGE",
                  "Enrich failed: missing known session event=%lu",
                  static_cast<unsigned long>(eventId));
        return false;
    }

    return _appendSpoolEnrichmentDelta(sessionId,
                                       eventId,
                                       lat, lon,
                                       alt, accuracy,
                                       tag);
}

bool StorageManager::findEventSessions(const uint32_t* eventIds,
                                       size_t count,
                                       String* outSessionIds) const {
    if (!_ready || !eventIds || !outSessionIds || count == 0) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        outSessionIds[i] = "";
    }

    size_t remaining = 0;
    for (size_t i = 0; i < count; ++i) {
        if (eventIds[i] != 0) {
            remaining++;
        }
    }
    if (remaining == 0) {
        return true;
    }

    auto wantedInSegment = [&](const SpoolSegmentInfo& seg) -> bool {
        for (size_t i = 0; i < count; ++i) {
            const uint32_t id = eventIds[i];
            if (id == 0 || outSessionIds[i].length()) {
                continue;
            }
            if (seg.firstEventId != 0 && id < seg.firstEventId) {
                continue;
            }
            if (seg.lastEventId != 0 && id > seg.lastEventId) {
                continue;
            }
            return true;
        }
        return false;
    };

    auto remember = [&](uint32_t id, const String& sessionId) {
        if (id == 0 || !sessionId.length()) {
            return;
        }
        for (size_t i = 0; i < count; ++i) {
            if (eventIds[i] == id && !outSessionIds[i].length()) {
                outSessionIds[i] = sessionId;
                if (remaining > 0) {
                    remaining--;
                }
            }
        }
    };

    for (const auto& seg : _spoolIndex.segments) {
        if (!wantedInSegment(seg)) {
            continue;
        }

        if (seg.format == SPOOL_SEGMENT_BIN_V2) {
            const bool ok = _scanBinarySegmentMetaRecords(
                _spoolBinarySegmentPath(seg.segmentId),
                [&](const BinaryMetaRecord& rec) -> bool {
                    if (rec.recordType == SPOOL_REC_EVENT &&
                        rec.eventId != 0 &&
                        rec.sessionId.length()) {
                        remember(rec.eventId, rec.sessionId);
                    }
                    return remaining > 0;
                });
            if (!ok) {
                return false;
            }
        } else {
            const bool ok = _scanSegmentRecords(seg.segmentId,
                [&](const DecodedSpoolRecord& rec) -> bool {
                    if (rec.recordType == SPOOL_REC_EVENT &&
                        rec.eventId != 0 &&
                        rec.sessionId.length()) {
                        remember(rec.eventId, rec.sessionId);
                    }
                    return remaining > 0;
                });
            if (!ok) {
                return false;
            }
        }

        if (remaining == 0) {
            return true;
        }
    }

    return true;
}

bool StorageManager::beginSession() {
    SESS.newSession();
    ENTITY_MGR.startSession(SESS.getId().c_str());
    DLOG_INFO("STORAGE", "Session started: %s", SESS.getId().c_str());
    return true;
}

bool StorageManager::endSession() {
    const bool captureAllowed = RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE;
    const uint32_t sessionWriteStart = captureAllowed ? millis() : 0U;
    JsonDocument entry;
    entry["id"]       = SESS.getId();
    entry[F_START]    = SESS.getStartTime();
    entry[F_END]      = millis();
    entry[F_MODE]     = currentSessionContextLabel();
    entry["lora"]     = SESS.getLoraPackets();
    entry["wifi"]     = SESS.getWifiScans();
    entry["probes"]   = SESS.getProbes();
    {
        char startIso[24] = {};
        char endIso[24] = {};
        TIME_SVC.formatIsoForMillis(SESS.getStartTime(), startIso, sizeof(startIso));
        TIME_SVC.formatIsoForMillis(millis(), endIso, sizeof(endIso));
        entry[F_START_ISO] = startIso;
        entry[F_END_ISO] = endIso;
        entry[F_TIME_SOURCE] = TIME_SVC.sourceName();
    }

    if (!_appendJsonLine(PATH_SESSIONS, entry)) return false;
    _logCaptureWriteAllowed(PATH_SESSIONS,
                            "end_session",
                            captureAllowed ? (millis() - sessionWriteStart) : 0U,
                            true);
    _trimJsonLinesFile(PATH_SESSIONS, SESSION_LOG_MAX_LINES);

    flushWorkerMetadataBatch("end_session", true);
    SESS.endSession();
    DLOG_INFO("STORAGE", "Session ended");
    return true;
}

bool StorageManager::checkpointSessionState() {
    if (!_ready) return false;

    if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY) {
        DLOG_INFO("STORAGE",
                  "Session checkpoint deferred owner=%s reason=capture",
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
        return true;
    }

    bool ok = true;
    if (hasWorkerMetadataFlushWork()) {
        ok &= flushWorkerMetadataBatch("session_checkpoint", true);
    }
    if (SpoolSegmentInfo* active = _findSegmentInfo(_spoolIndex.activeSegmentId)) {
        bool wroteCheckpoint = false;
        ok &= _maybeWriteBinarySegmentCheckpoint(*active,
                                                 "session_checkpoint",
                                                 true,
                                                 &wroteCheckpoint);
        if (wroteCheckpoint) {
            _spoolIndexDirty = true;
            ok &= _persistSpoolIndex(true, "session_checkpoint");
        }
    }
    refreshStorageUiState();

    if (ok) {
        DLOG_INFO("STORAGE", "Session checkpoint saved");
    } else {
        DLOG_WARN("STORAGE", "Session checkpoint save failed");
    }
    return ok;
}

size_t StorageManager::getTotalBytes() {
    _refreshFsStats();
    return _cachedTotalBytes;
}

size_t StorageManager::getUsedBytes() {
    _refreshFsStats();
    return _cachedUsedBytes;
}

size_t StorageManager::getFreeBytes() {
    _refreshFsStats();
    return _cachedFreeBytes;
}

int StorageManager::getUsedPercent() {
    _refreshFsStats();
    return _cachedUsedPct;
}

String StorageManager::getUsedString() {
    _refreshFsStats();
    return _cachedUsedString;
}

const char* StorageManager::_policyText() const {
    switch (_retentionPolicy) {
        case STORAGE_POLICY_REDUCED:       return "REDUCED";
        case STORAGE_POLICY_CRITICAL_ONLY: return "CRITICAL";
        case STORAGE_POLICY_NORMAL:
        default:                           return "NORMAL";
    }
}

const char* StorageManager::_counterTrustText() const {
    return counterTrustText(_counterTrustState);
}

void StorageManager::_setCounterTrustState(StorageCounterTrustState state,
                                           const char* reason) {
    if (_counterTrustState == state) {
        return;
    }

    const StorageCounterTrustState prevState = _counterTrustState;

    _counterTrustState = state;
    _counterTrustSinceMs = millis();
    if (reason && reason[0]) {
        _counterTrustReason = reason;
    } else {
        _counterTrustReason = "";
    }

    // Trusted <-> TrustedSnapshotLagged is the ordinary lifecycle: the metadata
    // write gets deferred, then the next flush catches it up.  Logging that at
    // WARN produced ~29 warnings per 3.5 h of idle capture, which buries the
    // transitions that actually matter.  Warn only when a genuinely degraded
    // state is entered or recovered from; keep the routine cycle at debug.
    auto isConcerning = [](StorageCounterTrustState s) {
        switch (s) {
            case CounterTrust::Degraded:
            case CounterTrust::RepairRequired:
            case CounterTrust::EmergencyOnly:
                return true;
            default:
                return false;
        }
    };

    if (isConcerning(state) || isConcerning(prevState)) {
        DLOG_WARN("STORAGE",
                  "Counter trust=%s reason=%s since=%lu",
                  _counterTrustText(),
                  _counterTrustReason.length() ? _counterTrustReason.c_str() : "-",
                  static_cast<unsigned long>(_counterTrustSinceMs));
    } else {
        DLOG_DEBUG("STORAGE",
                   "Counter trust=%s reason=%s since=%lu",
                   _counterTrustText(),
                   _counterTrustReason.length() ? _counterTrustReason.c_str() : "-",
                   static_cast<unsigned long>(_counterTrustSinceMs));
    }

    switch (state) {
        case CounterTrust::Trusted:
            _clearMaintenanceFlags(STORAGE_MAINT_COUNTER_UNTRUSTED);
            break;
        case CounterTrust::TrustedSnapshotLagged:
            _clearMaintenanceFlags(STORAGE_MAINT_COUNTER_UNTRUSTED);
            requestMaintenance(STORAGE_MAINT_SNAPSHOT_LAGGED, reason);
            break;
        case CounterTrust::Degraded:
        case CounterTrust::RepairRequired:
            requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED, reason);
            break;
        case CounterTrust::EmergencyOnly:
            requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED, reason);
            requestMaintenance(STORAGE_MAINT_EMERGENCY_REPAIR, reason);
            break;
    }
}

void StorageManager::_applyCounterDelta(const char* reason,
                                        int32_t pendingUploadDelta,
                                        int32_t pendingEnrichDelta,
                                        int32_t droppedDelta,
                                        int32_t suppressedDelta,
                                        StorageLane lane,
                                        StoragePriority priority) {
    const char* safeReason = (reason && reason[0]) ? reason : "-";
    const uint32_t beforePending = _pendingEventCount;

    if (pendingUploadDelta != 0) {
        int64_t nextPending =
            static_cast<int64_t>(beforePending) +
            static_cast<int64_t>(pendingUploadDelta);
        if (nextPending < 0) {
            DLOG_WARN("STORAGE",
                      "Counter underflow reason=%s pendingBefore=%lu pendingDelta=%ld lane=%u prio=%u",
                      safeReason,
                      static_cast<unsigned long>(beforePending),
                      static_cast<long>(pendingUploadDelta),
                      static_cast<unsigned>(lane),
                      static_cast<unsigned>(priority));
            nextPending = 0;
            _pendingCountDirty = true;
            _setCounterTrustState(STORAGE_COUNTER_DEGRADED, safeReason);
            _spoolAuditRepairRequired = true;
        }

        _pendingEventCount = static_cast<uint32_t>(nextPending);
        _spoolIndex.pendingTotal = _pendingEventCount;
        _bumpStorageMetaGeneration();
    } else {
        _spoolIndex.pendingTotal = _pendingEventCount;
    }

    if (pendingEnrichDelta != 0) {
        _spoolSummaryRebuildPending = true;
    }

    if (droppedDelta != 0) {
        if (droppedDelta < 0) {
            _dedupStats.dropped = 0;
            _setCounterTrustState(STORAGE_COUNTER_DEGRADED, safeReason);
            _spoolAuditRepairRequired = true;
        } else {
            const uint32_t before = _dedupStats.dropped;
            const uint64_t next = static_cast<uint64_t>(before) +
                                  static_cast<uint64_t>(droppedDelta);
            _dedupStats.dropped = static_cast<uint32_t>(
                next > UINT32_MAX ? UINT32_MAX : next);
        }
    }

    if (suppressedDelta != 0) {
        if (suppressedDelta < 0) {
            _dedupStats.suppressed = 0;
            _setCounterTrustState(STORAGE_COUNTER_DEGRADED, safeReason);
            _spoolAuditRepairRequired = true;
        } else {
            const uint32_t before = _dedupStats.suppressed;
            const uint64_t next = static_cast<uint64_t>(before) +
                                  static_cast<uint64_t>(suppressedDelta);
            _dedupStats.suppressed = static_cast<uint32_t>(
                next > UINT32_MAX ? UINT32_MAX : next);
        }
    }

    if (pendingUploadDelta != 0 ||
        pendingEnrichDelta != 0 ||
        droppedDelta != 0 ||
        suppressedDelta != 0) {
        _queueStorageUiRefresh(false);
    }

    DLOG_DEBUG("STORAGE",
               "Counter delta reason=%s pending=%ld enrich=%ld dropped=%ld suppressed=%ld lane=%u prio=%u trust=%s",
               safeReason,
               static_cast<long>(pendingUploadDelta),
               static_cast<long>(pendingEnrichDelta),
               static_cast<long>(droppedDelta),
               static_cast<long>(suppressedDelta),
               static_cast<unsigned>(lane),
               static_cast<unsigned>(priority),
               _counterTrustText());
}

void StorageManager::_applyPendingEventCountDelta(int32_t delta,
                                                  const char* reason,
                                                  bool markMetadataDirty,
                                                  bool markUiRefresh) {
    const StorageLane lane = STORAGE_LANE_NOISE;
    const StoragePriority priority = STORAGE_PRIO_P3;
    _applyCounterDelta(reason,
                       delta,
                       0,
                       0,
                       0,
                       lane,
                       priority);

    if (markMetadataDirty) {
        _workerMetadataDirtyPending = true;
        if (_workerMetadataPendingWrites < UINT16_MAX) {
            _workerMetadataPendingWrites++;
        }
        if (_workerMetadataDirtySinceMs == 0) {
            _workerMetadataDirtySinceMs = millis();
        }
    }

    if (markUiRefresh) {
        _queueStorageUiRefresh(false);
    }
}

bool StorageManager::shouldStoreByPriority(StoragePriority priority) const {
    if (_cachedTotalBytes != 0 && _cachedFreeBytes <= STORAGE_HARD_FLOOR_BYTES) {
        return false;
    }
    if (_cachedTotalBytes != 0 &&
        _cachedFreeBytes <= STORAGE_METADATA_RESERVE_BYTES &&
        priority > STORAGE_PRIO_P1) {
        return false;
    }
    switch (_retentionPolicy) {
        case STORAGE_POLICY_NORMAL:
            return true;
        case STORAGE_POLICY_REDUCED:
            return priority <= STORAGE_PRIO_P2;
        case STORAGE_POLICY_CRITICAL_ONLY:
            return priority <= STORAGE_PRIO_P1;
        default:
            return true;
    }
}

bool StorageManager::_eventRecordIsMission(JsonObjectConst doc) const {
    return _eventRecordLane(doc) == STORAGE_LANE_MISSION;
}

void StorageManager::_clearSegmentSummary(SpoolSegmentInfo& seg) {
    seg.summaryVersion = SPOOL_SEGMENT_SUMMARY_VERSION;
    seg.summaryValid = true;

    seg.eventCount = 0;
    seg.enrichDeltaCount = 0;
    seg.missionCount = 0;
    seg.noiseCount = 0;
    seg.pendingUploadMissionCount = 0;
    seg.pendingUploadNoiseCount = 0;
    seg.p0Count = 0;
    seg.p1Count = 0;
    seg.p2Count = 0;
    seg.p3Count = 0;
    seg.pendingEnrichmentCount = 0;
    seg.minTimestampMs = 0;
    seg.maxTimestampMs = 0;
}

void StorageManager::_updateSegmentSummaryFromEventDoc(SpoolSegmentInfo& seg,
                                                       JsonObjectConst doc,
                                                       uint32_t eventId,
                                                       uint32_t timestampMs,
                                                       bool pendingUpload) {
    const RAMSpool::CaptureClassification cls = _captureClassification(doc);
    const StorageLane lane =
        (cls.lane == RAMSpool::LANE_MISSION) ? STORAGE_LANE_MISSION
                                              : STORAGE_LANE_NOISE;
    const StoragePriority prio = static_cast<StoragePriority>(cls.priority);

    seg.eventCount++;

    if (lane == STORAGE_LANE_MISSION) {
        seg.missionCount++;
        if (pendingUpload) {
            seg.pendingUploadMissionCount++;
        }
    } else {
        seg.noiseCount++;
        if (pendingUpload) {
            seg.pendingUploadNoiseCount++;
        }
    }

    switch (prio) {
        case STORAGE_PRIO_P0: seg.p0Count++; break;
        case STORAGE_PRIO_P1: seg.p1Count++; break;
        case STORAGE_PRIO_P2: seg.p2Count++; break;
        case STORAGE_PRIO_P3:
        default:              seg.p3Count++; break;
    }

    if (cls.enrichEligible && _eventRecordPendingEnrichment(doc)) {
        seg.pendingEnrichmentCount++;
    }

    if (eventId != 0) {
        if (seg.firstEventId == 0 || eventId < seg.firstEventId) {
            seg.firstEventId = eventId;
        }
        if (eventId > seg.lastEventId) {
            seg.lastEventId = eventId;
        }
    }

    if (timestampMs != 0) {
        if (seg.minTimestampMs == 0 || timestampMs < seg.minTimestampMs) {
            seg.minTimestampMs = timestampMs;
        }
        if (timestampMs > seg.maxTimestampMs) {
            seg.maxTimestampMs = timestampMs;
        }
    }

    seg.summaryVersion = SPOOL_SEGMENT_SUMMARY_VERSION;
    seg.summaryValid = true;
}

void StorageManager::_decrementPendingEnrichmentForEvent(uint32_t eventId) {
    if (eventId == 0) return;

    // Clamp duplicate/stale deltas; recount paths dedupe before calling here.
    for (auto& seg : _spoolIndex.segments) {
        if (seg.firstEventId == 0 || seg.lastEventId == 0) continue;
        if (eventId < seg.firstEventId || eventId > seg.lastEventId) continue;

        if (!seg.summaryValid ||
            seg.summaryVersion != SPOOL_SEGMENT_SUMMARY_VERSION) {
            _pendingCountDirty = true;
            _spoolSummaryRebuildPending = true;
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                  "pending_enrich_summary_stale");
            return;
        }

        if (seg.pendingEnrichmentCount == 0) {
            // Do not underflow on duplicates or stale summary drift.
            _pendingCountDirty = true;
            _spoolSummaryRebuildPending = true;
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                  "pending_enrich_counter_clamp");
            return;
        }

        seg.pendingEnrichmentCount--;
        return;
    }
    // Origin segment not found (e.g., compacted away).  Treat as benign:
    // the events that segment held have already left the spool, so any
    // count it contributed is gone with it.
}

bool StorageManager::_adjustPendingUploadForEvent(uint32_t eventId,
                                                  uint8_t laneHint,
                                                  int32_t delta,
                                                  const char* reason) {
    if (eventId == 0 || delta == 0) return false;

    for (auto& seg : _spoolIndex.segments) {
        if (seg.firstEventId == 0 || seg.lastEventId == 0) continue;
        if (eventId < seg.firstEventId || eventId > seg.lastEventId) continue;

        if (!seg.summaryValid ||
            seg.summaryVersion != SPOOL_SEGMENT_SUMMARY_VERSION) {
            _pendingCountDirty = true;
            _spoolSummaryRebuildPending = true;
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                  reason ? reason : "pending_upload_summary_stale");
            return false;
        }

        uint32_t* counter = nullptr;
        if (laneHint == static_cast<uint8_t>(STORAGE_LANE_MISSION)) {
            counter = &seg.pendingUploadMissionCount;
        } else if (laneHint == static_cast<uint8_t>(STORAGE_LANE_NOISE)) {
            counter = &seg.pendingUploadNoiseCount;
        } else if (seg.pendingUploadMissionCount > 0 &&
                   seg.pendingUploadNoiseCount == 0) {
            counter = &seg.pendingUploadMissionCount;
        } else if (seg.pendingUploadNoiseCount > 0 &&
                   seg.pendingUploadMissionCount == 0) {
            counter = &seg.pendingUploadNoiseCount;
        }

        if (delta < 0) {
            const uint32_t amount = static_cast<uint32_t>(-delta);
            if (counter && *counter >= amount) {
                *counter -= amount;
                _refreshSegmentLifecycle(seg);
                return true;
            }

            _pendingCountDirty = true;
            _setCounterTrustState(STORAGE_COUNTER_DEGRADED,
                                  reason ? reason : "pending_upload_lane_underflow");
            return false;
        }

        if (counter) {
            const uint32_t amount = static_cast<uint32_t>(delta);
            if (UINT32_MAX - *counter < amount) {
                _pendingCountDirty = true;
                _setCounterTrustState(STORAGE_COUNTER_DEGRADED,
                                      reason ? reason : "pending_upload_lane_overflow");
                return false;
            }
            *counter += amount;
            _refreshSegmentLifecycle(seg);
            return true;
        }

        _pendingCountDirty = true;
        _setCounterTrustState(STORAGE_COUNTER_DEGRADED,
                              reason ? reason : "pending_upload_lane_unknown");
        return false;
    }

    _pendingCountDirty = true;
    _setCounterTrustState(STORAGE_COUNTER_DEGRADED,
                          reason ? reason : "pending_upload_segment_missing");
    return false;
}

bool StorageManager::_decrementPendingUploadForEvent(uint32_t eventId,
                                                     uint8_t laneHint,
                                                     const char* reason) {
    return _adjustPendingUploadForEvent(eventId, laneHint, -1, reason);
}

bool StorageManager::_applyExactUploadedMarks(
    const String& sessionId,
    const std::vector<UploadIndexRecordV1>& selected,
    uint32_t before,
    uint32_t upToId) {

    struct SegmentDelta {
        uint32_t mission = 0;
        uint32_t noise = 0;
    };

    std::map<uint32_t, SegmentDelta> deltas;
    uint32_t decrement = 0;

    for (const auto& rec : selected) {
        if (rec.eventId <= before || rec.eventId > upToId) {
            continue;
        }

        if (!rec.sessionId[0] || sessionId != rec.sessionId) {
            requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                               "bulk_mark_session_mismatch");
            return false;
        }

        const SpoolSegmentInfo* seg = _findSegmentInfo(rec.segmentId);
        if (!seg) {
            requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                               "bulk_mark_missing_segment");
            return false;
        }

        SegmentDelta& delta = deltas[rec.segmentId];
        if (rec.lane == static_cast<uint8_t>(STORAGE_LANE_MISSION)) {
            delta.mission++;
        } else if (rec.lane == static_cast<uint8_t>(STORAGE_LANE_NOISE)) {
            delta.noise++;
        } else {
            requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED,
                               "bulk_mark_lane_invalid");
            return false;
        }

        decrement++;
    }

    if (decrement == 0) {
        return true;
    }

    if (_pendingEventCount < decrement) {
        requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED,
                           "bulk_mark_counter_underflow");
        return false;
    }

    for (const auto& entry : deltas) {
        const SpoolSegmentInfo* seg = _findSegmentInfo(entry.first);
        if (!seg) {
            requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                               "bulk_mark_missing_segment");
            return false;
        }

        if (seg->pendingUploadMissionCount < entry.second.mission ||
            seg->pendingUploadNoiseCount < entry.second.noise) {
            requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED,
                               "bulk_mark_lane_underflow");
            return false;
        }
    }

    _applyCounterDelta("bulk_mark_uploaded",
                       -static_cast<int32_t>(decrement),
                       0, 0, 0,
                       STORAGE_LANE_NOISE,
                       STORAGE_PRIO_P3);

    for (const auto& entry : deltas) {
        SpoolSegmentInfo* seg = _findSegmentInfo(entry.first);
        if (!seg) {
            requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                               "bulk_mark_missing_segment");
            return false;
        }

        seg->pendingUploadMissionCount -= entry.second.mission;
        seg->pendingUploadNoiseCount -= entry.second.noise;
        _refreshSegmentLifecycle(*seg);
    }

    _pendingCountDirty = false;
    _spoolAuditRepairRequired = false;
    return true;
}

void StorageManager::_markSegmentEnrichmentDelta(SpoolSegmentInfo& seg,
                                                 uint32_t recordId,
                                                 uint32_t timestampMs) {
    (void)recordId;

    seg.enrichDeltaCount++;

    if (timestampMs != 0) {
        if (seg.minTimestampMs == 0 || timestampMs < seg.minTimestampMs) {
            seg.minTimestampMs = timestampMs;
        }
        if (timestampMs > seg.maxTimestampMs) {
            seg.maxTimestampMs = timestampMs;
        }
    }

    seg.summaryVersion = SPOOL_SEGMENT_SUMMARY_VERSION;
    seg.summaryValid = true;
}

void StorageManager::_updateSegmentSummaryFromDecodedRecord(SpoolSegmentInfo& seg,
                                                            const DecodedSpoolRecord& rec) {
    if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
        const JsonObjectConst deltaDoc = rec.doc.as<JsonObjectConst>();
        _markSegmentEnrichmentDelta(seg, rec.eventId, deltaDoc["ts"] | 0U);
        return;
    }

    if (rec.recordType != SPOOL_REC_EVENT) {
        return;
    }

    const JsonObjectConst doc = rec.doc.as<JsonObjectConst>();
    const bool pendingUpload =
        rec.sessionId.length() &&
        rec.eventId > _uploadedWatermarkForSession(rec.sessionId);
    _updateSegmentSummaryFromEventDoc(seg, doc, rec.eventId, doc["ts"] | 0U, pendingUpload);
}

bool StorageManager::_rebuildSegmentSummary(SpoolSegmentInfo& seg) {
    if (seg.segmentId == 0) {
        return false;
    }

    SpoolSegmentInfo rebuilt = seg;
    _clearSegmentSummary(rebuilt);
    rebuilt.segmentId = seg.segmentId;
    rebuilt.format = seg.format;
    rebuilt.approxBytes = seg.approxBytes;
    rebuilt.firstEventId = 0;
    rebuilt.lastEventId = 0;
    rebuilt.recordCount = 0;

    const bool ok = _scanSegmentRecords(seg.segmentId,
        [&](const DecodedSpoolRecord& rec) -> bool {
            rebuilt.recordCount++;

            _updateSegmentSummaryFromDecodedRecord(rebuilt, rec);
            return true;
        });

    if (!ok) {
        seg.summaryValid = false;
        seg.trustState = SPOOL_SEGMENT_INVALID;
        return false;
    }

    rebuilt.summaryVersion = SPOOL_SEGMENT_SUMMARY_VERSION;
    rebuilt.summaryValid = true;
    rebuilt.trustState = SPOOL_SEGMENT_TRUSTED;
    seg = rebuilt;
    _refreshSegmentLifecycle(seg);
    return true;
}

bool StorageManager::_rebuildInvalidSegmentSummaries(bool force) {
    uint32_t rebuiltCount = 0;
    bool changed = false;
    bool anyRebuilt = false;
    const bool bounded = !force;

    for (auto& seg : _spoolIndex.segments) {
        const SpoolSegmentInfo before = seg;
        if (!force &&
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION) {
            continue;
        }

        if (_rebuildSegmentSummary(seg)) {
            rebuiltCount++;
            anyRebuilt = true;
        }

        if (!_spoolSegmentInfoEquals(before, seg)) {
            changed = true;
        }

        if (bounded && rebuiltCount > 0U) {
            break;
        }
    }

    // After all summaries are valid, reconcile unique enrich deltas by eventId.
    bool pendingEnrichReconcileOk = true;
    if (anyRebuilt && !_hasInvalidSpoolSummaries()) {
        // PSRAM-backed because mass NO_DATA retirement can add thousands.
        std::map<uint32_t, bool, std::less<uint32_t>,
                 SpiramStlAllocator<std::pair<const uint32_t, bool>>> seenEnrichedIds;
        for (const auto& seg : _spoolIndex.segments) {
            const bool summaryReady =
                seg.summaryValid &&
                seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;
            if (summaryReady && seg.enrichDeltaCount == 0) {
                continue;
            }

            const bool ok = _scanSegmentRecords(seg.segmentId,
                [&](const DecodedSpoolRecord& rec) -> bool {
                    if (rec.recordType != SPOOL_REC_ENRICH_DELTA) return true;
                    const uint32_t targetId = rec.doc["event_id"] | 0U;
                    if (targetId == 0) return true;
                    // First-application proof: only the first delta per
                    // eventId actually decrements the gross count.
                    if (seenEnrichedIds.find(targetId) != seenEnrichedIds.end()) {
                        return true;
                    }
                    seenEnrichedIds[targetId] = true;
                    _decrementPendingEnrichmentForEvent(targetId);
                    return true;
                });
            if (!ok) {
                pendingEnrichReconcileOk = false;
                break;
            }
        }
        if (!pendingEnrichReconcileOk) {
            requestMaintenance(STORAGE_MAINT_DIRTY_SUMMARY,
                               "pending_enrich_delta_reconcile_failed");
        }
    }

    _spoolSummaryRebuildPending =
        _hasInvalidSpoolSummaries() || !pendingEnrichReconcileOk;

    if (changed) {
        if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
            _selectRepairMode() != REPAIR_EMERGENCY) {
            _spoolSummaryRebuildPending = true;
            _spoolIndexDirty = true;
            DLOG_INFO("STORAGE",
                      "Spool summary rebuild deferred owner=%s reason=capture",
                      RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
            return true;
        }

        _persistSpoolIndex(true, "summary_rebuild");
    }

    return changed || rebuiltCount > 0;
}

bool StorageManager::_hasInvalidSpoolSummaries() const {
    for (const auto& seg : _spoolIndex.segments) {
        if (!seg.summaryValid ||
            seg.summaryVersion != SPOOL_SEGMENT_SUMMARY_VERSION) {
            return true;
        }
    }
    return false;
}

bool StorageManager::_scanSegmentForAudit(SpoolSegmentInfo& rebuilt,
                                          SpoolAuditResult& audit,
                                          std::vector<String>& rebuiltSessions,
                                          bool repair) {
    (void)repair;

    SpoolSegmentInfo scanned = rebuilt;
    _clearSegmentSummary(scanned);
    scanned.segmentId = rebuilt.segmentId;
    scanned.format = rebuilt.format;
    scanned.approxBytes = rebuilt.approxBytes;
    scanned.firstEventId = 0;
    scanned.lastEventId = 0;
    scanned.recordCount = 0;

    SpoolAuditResult local;
    std::vector<String> segmentSessions;
    auto rememberSession = [&](const String& sessionId) {
        if (!sessionId.length()) {
            return;
        }
        for (const auto& existing : segmentSessions) {
            if (existing == sessionId) {
                return;
            }
        }
        segmentSessions.push_back(sessionId);
    };

    bool ok;

    if (scanned.format == SPOOL_SEGMENT_BIN_V2) {
        // Use the skip-capable audit scanner so a single undecodable record
        // doesn't condemn the whole segment.  BinaryMetaRecord only carries
        // recordType/eventId/sessionId — no payload — so priority, lane, and
        // timestamp summary stats can't be rebuilt here.  Mark the summary
        // invalid so _rebuildInvalidSegmentSummaries picks it up later.
        // The scanner itself updates local.scannedRecords, .validEventRecords,
        // .validEnrichDeltas, .invalidRecords, .skippedRecords, .maxEventIdSeen.
        const SpoolScanStatus status = _scanBinarySegmentMetaRecordsAudit(
            _spoolBinarySegmentPath(scanned.segmentId),
            [&](const BinaryMetaRecord& rec) -> bool {
                scanned.recordCount++;
                rememberSession(rec.sessionId);

                if (rec.recordType == SPOOL_REC_EVENT) {
                    const uint32_t watermark = _uploadedWatermarkForSession(rec.sessionId);
                    if (rec.eventId > watermark) {
                        local.rebuiltPendingTotal++;
                    }
                    if (rec.eventId != 0) {
                        if (scanned.firstEventId == 0 || rec.eventId < scanned.firstEventId) {
                            scanned.firstEventId = rec.eventId;
                        }
                        if (rec.eventId > scanned.lastEventId) {
                            scanned.lastEventId = rec.eventId;
                        }
                    }
                }
                return true;
            },
            local);

        scanned.eventCount = local.validEventRecords;
        scanned.enrichDeltaCount = local.validEnrichDeltas;

        const bool existingSummaryStillMatches =
            rebuilt.summaryValid &&
            rebuilt.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION &&
            rebuilt.recordCount == scanned.recordCount &&
            rebuilt.eventCount == scanned.eventCount &&
            rebuilt.enrichDeltaCount == scanned.enrichDeltaCount &&
            rebuilt.firstEventId == scanned.firstEventId &&
            rebuilt.lastEventId == scanned.lastEventId;

        if (existingSummaryStillMatches) {
            scanned.summaryVersion = rebuilt.summaryVersion;
            scanned.summaryValid = rebuilt.summaryValid;
            scanned.missionCount = rebuilt.missionCount;
            scanned.noiseCount = rebuilt.noiseCount;
            scanned.pendingUploadMissionCount = rebuilt.pendingUploadMissionCount;
            scanned.pendingUploadNoiseCount = rebuilt.pendingUploadNoiseCount;
            scanned.p0Count = rebuilt.p0Count;
            scanned.p1Count = rebuilt.p1Count;
            scanned.p2Count = rebuilt.p2Count;
            scanned.p3Count = rebuilt.p3Count;
            scanned.pendingEnrichmentCount = rebuilt.pendingEnrichmentCount;
            scanned.minTimestampMs = rebuilt.minTimestampMs;
            scanned.maxTimestampMs = rebuilt.maxTimestampMs;
        } else {
            scanned.summaryValid = false;
        }
        ok = (status != SpoolScanStatus::FATAL);
    } else {
        ok = _scanSegmentRecords(scanned.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                local.scannedRecords++;

                const bool isEvent = rec.recordType == SPOOL_REC_EVENT;
                const bool isEnrich = rec.recordType == SPOOL_REC_ENRICH_DELTA;
                if (!isEvent && !isEnrich) {
                    local.invalidRecords++;
                    local.skippedRecords++;
                    return true;
                }

                if (!rec.eventId || !rec.sessionId.length()) {
                    local.invalidRecords++;
                    local.skippedRecords++;
                    return true;
                }

                scanned.recordCount++;
                local.maxEventIdSeen = std::max(local.maxEventIdSeen, rec.eventId);
                rememberSession(rec.sessionId);

                if (isEvent) {
                    local.validEventRecords++;
                    const uint32_t watermark = _uploadedWatermarkForSession(rec.sessionId);
                    if (rec.eventId > watermark) {
                        local.rebuiltPendingTotal++;
                    }
                } else {
                    local.validEnrichDeltas++;
                }

                _updateSegmentSummaryFromDecodedRecord(scanned, rec);
                return true;
            });
    }

    if (!ok) {
        rebuilt.trustState = SPOOL_SEGMENT_INVALID;
        return false;
    }

    for (const auto& sessionId : segmentSessions) {
        bool seen = false;
        for (const auto& existing : rebuiltSessions) {
            if (existing == sessionId) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            rebuiltSessions.push_back(sessionId);
        }
    }

    scanned.trustState = SPOOL_SEGMENT_TRUSTED;
    rebuilt = scanned;
    audit.scannedRecords += local.scannedRecords;
    audit.validEventRecords += local.validEventRecords;
    audit.validEnrichDeltas += local.validEnrichDeltas;
    audit.invalidRecords += local.invalidRecords;
    audit.skippedRecords += local.skippedRecords;
    audit.rebuiltPendingTotal += local.rebuiltPendingTotal;
    audit.maxEventIdSeen = std::max(audit.maxEventIdSeen, local.maxEventIdSeen);
    return true;
}

bool StorageManager::_auditAndRepairSpool(const char* reason,
                                         bool repair,
                                         SpoolAuditResult* out) {
    SpoolAuditResult audit;
    audit.oldPendingTotal = _spoolIndex.pendingTotal;
    audit.oldNextEventId = _nextEventId;
    const uint32_t _auditStartMs = millis();

    // The audit scans segment files straight off LittleFS, and a segment's
    // record count lives in its header — which _flushWorkerAppendFile() only
    // rewrites when the header is dirty. Records already appended to the open
    // worker file are therefore invisible to this scan, and the callers that
    // reconcile counters from the result will "correct" pendingUpload down to
    // the visible count. Running `spool count` during active capture was enough
    // to zero a real backlog and make `upload now` report no pending records.
    // Flush first so the scan sees everything that has actually been written.
    if (_workerAppendFile) {
        if (RAMSpool::isWorkerPaused()) {
            if (!_flushWorkerAppendFile(reason ? reason : "audit", true)) {
                audit.scanIncomplete = true;
                DLOG_WARN("STORAGE",
                          "Spool audit[%s] worker append flush failed; counts are a floor",
                          reason ? reason : "-");
            }
        } else {
            // Cannot touch the file under a live writer. Scan anyway for
            // diagnostics, but mark the result so nothing reconciles down.
            audit.scanIncomplete = true;
            DLOG_WARN("STORAGE",
                      "Spool audit[%s] worker active with open append file;"
                      " counts are a floor, reconcile suppressed",
                      reason ? reason : "-");
        }
    }

    std::vector<SpoolSegmentInfo> repairedSegments;
    repairedSegments.reserve(_spoolIndex.segments.size());
    std::vector<String> rebuiltSessions;
    rebuiltSessions.reserve(_spoolIndex.sessions.size());

    bool segmentChanged = false;
    bool sessionsChanged = false;

    for (const auto& seg : _spoolIndex.segments) {
        if (_isSegmentQuarantined(seg.segmentId)) {
            audit.quarantinedSegments++;
            segmentChanged = true;
            continue;
        }

        const String segmentPath =
            _spoolSegmentPathForFormat(seg.segmentId, seg.format);
        if (!LittleFS.exists(segmentPath)) {
            audit.unreadableSegments++;
            audit.hadFatalSegmentError = true;

            if (repair) {
                DLOG_WARN("STORAGE",
                          "Spool audit dropping missing segment seg=%lu path=%s",
                          static_cast<unsigned long>(seg.segmentId),
                          segmentPath.c_str());
                const String indexPath = _uploadIndexPath(seg.segmentId);
                if (LittleFS.exists(indexPath) && !LittleFS.remove(indexPath)) {
                    DLOG_WARN("STORAGE",
                              "Upload index sidecar remove failed missing seg=%lu path=%s",
                              static_cast<unsigned long>(seg.segmentId),
                              indexPath.c_str());
                }
                segmentChanged = true;
                audit.repaired = true;
                continue;
            }

            SpoolSegmentInfo kept = seg;
            kept.trustState = SPOOL_SEGMENT_INVALID;
            repairedSegments.push_back(kept);
            continue;
        }

        audit.scannedSegments++;

        SpoolSegmentInfo rebuilt = seg;
        const bool ok = _scanSegmentForAudit(rebuilt, audit, rebuiltSessions, repair);
        if (!ok) {
            audit.unreadableSegments++;
            audit.hadFatalSegmentError = true;

            if (repair) {
                if (!_quarantineSpoolSegment(seg.segmentId,
                                             SpoolCorruptionReason::SCAN_FAILED,
                                             "audit scan failed")) {
                    if (out) {
                        *out = audit;
                    }
                    return false;
                }

                audit.quarantinedSegments++;
                segmentChanged = true;
                continue;
            }

            SpoolSegmentInfo kept = seg;
            kept.trustState = SPOOL_SEGMENT_INVALID;
            repairedSegments.push_back(kept);
            continue;
        }

        if (!_spoolSegmentInfoEquals(seg, rebuilt)) {
            segmentChanged = true;
        }
        repairedSegments.push_back(rebuilt);
    }

    sessionsChanged = (rebuiltSessions != _spoolIndex.sessions);

    const uint32_t safeNextFloor =
        std::max<uint32_t>(audit.maxEventIdSeen, audit.validEventRecords);

    audit.hadMismatch =
        audit.oldPendingTotal != audit.rebuiltPendingTotal ||
        audit.oldNextEventId <= safeNextFloor ||
        audit.quarantinedSegments > 0 ||
        audit.invalidRecords > 0 ||
        audit.hadFatalSegmentError ||
        segmentChanged ||
        sessionsChanged;

    const bool metadataOnlyReconcile =
        reason && strcmp(reason, "manual_count") == 0;
    // An incomplete scan yields a floor, not a total — never rebuild counters
    // from it (see the flush note at the top of this function).
    if ((repair || metadataOnlyReconcile) && audit.hadMismatch &&
        !audit.scanIncomplete) {
        if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
            _selectRepairMode() != REPAIR_EMERGENCY) {
            _spoolAuditRepairRequired = true;
            _pendingCountDirty = true;
            _spoolIndexDirty = true;
            _eventCounterDirty = true;
            DLOG_INFO("STORAGE",
                      "Spool audit repair deferred owner=%s reason=%s",
                      RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                      (reason && reason[0]) ? reason : "-");
            if (out) {
                *out = audit;
            }
            return false;
        }

        _spoolIndex.pendingTotal = audit.rebuiltPendingTotal;
        _pendingEventCount = audit.rebuiltPendingTotal;
        _bumpStorageMetaGeneration();

        const uint32_t rebuiltNext =
            (audit.maxEventIdSeen == UINT32_MAX) ?
                UINT32_MAX :
                std::max<uint32_t>(audit.maxEventIdSeen + 1U,
                                   audit.validEventRecords + 1U);
        if (_nextEventId < rebuiltNext) {
            _nextEventId = rebuiltNext;
            _spoolIndex.nextEventId = _nextEventId;
            _bumpStorageMetaGeneration();
            _eventCounterDirty = true;
            _eventCounterPendingWrites = 1;
        }

        _spoolIndex.segments.assign(repairedSegments.begin(),
                                    repairedSegments.end());
        _spoolIndex.segments.erase(
            std::remove_if(_spoolIndex.segments.begin(),
                           _spoolIndex.segments.end(),
                           [&](const SpoolSegmentInfo& seg) {
                               return seg.segmentId == 0;
                           }),
            _spoolIndex.segments.end());

        if (!_spoolIndex.segments.empty()) {
            _spoolIndex.oldestSegmentId = _spoolIndex.segments.front().segmentId;
        } else {
            _spoolIndex.oldestSegmentId = 0;
        }

        _spoolIndex.sessions = rebuiltSessions;
        _pendingCountDirty = false;
        _spoolIndexDirty = true;

        // The audit scanner rebuilds each segment independently. That yields
        // the gross enrich-eligible event count, but most field enrichment
        // deltas live in later append segments. Join the complete delta-ID set
        // back onto every event segment before persisting, otherwise a harmless
        // `spool count` resurrects thousands of already-resolved phone jobs.
        if (!_reconcilePendingEnrichmentSummaries(
                repair ? "spool_audit_repair" : "manual_count_reconcile")) {
            requestMaintenance(STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY,
                               "audit_pending_enrich_reconcile_failed");
            if (out) {
                *out = audit;
            }
            return false;
        }

        for (auto it = _binaryLastSessionBySegment.begin();
             it != _binaryLastSessionBySegment.end();) {
            const bool keep =
                std::any_of(_spoolIndex.segments.begin(),
                            _spoolIndex.segments.end(),
                            [&](const SpoolSegmentInfo& seg) {
                                return seg.segmentId == it->first;
                            });
            if (!keep) {
                _binaryLastSessionTagBySegment.erase(it->first);
                _binaryEnrichCtxBySegment.erase(it->first);
                it = _binaryLastSessionBySegment.erase(it);
            } else {
                ++it;
            }
        }

        for (auto it = _binaryCheckpointBySegment.begin();
             it != _binaryCheckpointBySegment.end();) {
            const bool keep =
                std::any_of(_spoolIndex.segments.begin(),
                            _spoolIndex.segments.end(),
                            [&](const SpoolSegmentInfo& seg) {
                                return seg.segmentId == it->first;
                            });
            if (!keep) {
                it = _binaryCheckpointBySegment.erase(it);
            } else {
                ++it;
            }
        }

        const bool activeStillExists =
            _spoolIndex.activeSegmentId != 0 &&
            _findSegmentInfo(_spoolIndex.activeSegmentId) != nullptr;
        if (!activeStillExists) {
            _spoolIndex.activeSegmentId = 0;
            if (!_openNewSpoolSegment()) {
                audit.hadFatalSegmentError = true;
                if (out) {
                    *out = audit;
                }
                return false;
            }
        }

        const char* persistReason =
            repair ? "spool_audit_repair" : "manual_count_reconcile";
        if (!_persistEventCounter(true, persistReason) ||
            !_persistSpoolIndex(true, persistReason) ||
            !_persistEventMeta(true, persistReason)) {
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                  "spool_audit_persist_failed");
            if (out) {
                *out = audit;
            }
            return false;
        }

        audit.repaired = repair;
    }

    const uint32_t _auditElapsedMs = millis() - _auditStartMs;
    if (_auditElapsedMs > 200U || audit.scannedRecords > 512U) {
        DLOG_WARN("STORAGE",
                  "Spool audit slow reason=%s segs=%lu recs=%lu ms=%lu",
                  (reason && reason[0]) ? reason : "-",
                  static_cast<unsigned long>(audit.scannedSegments),
                  static_cast<unsigned long>(audit.scannedRecords),
                  static_cast<unsigned long>(_auditElapsedMs));
    }
    _logSpoolAuditResult(reason, audit);

    if (!audit.hadFatalSegmentError &&
        audit.validEventRecords > audit.rebuiltPendingTotal) {
        requestMaintenance(STORAGE_MAINT_DELETE_DRAINED,
                           (reason && reason[0]) ? reason : "spool_audit");
    }

    if (out) {
        *out = audit;
    }
    return !audit.hadFatalSegmentError || repair;
}

bool StorageManager::requestSpoolRepair(const char* reason) {
    if (!_ready) {
        return false;
    }

    const char* safeReason = (reason && reason[0]) ? reason : "maintenance";
    const bool wasRequested = _repairRequested || _spoolAuditRepairRequired;
    _repairRequested = true;
    _spoolAuditRepairRequired = true;
    requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT, safeReason);
    requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED, safeReason);
    if (_counterTrustState != CounterTrust::EmergencyOnly) {
        _setCounterTrustState(STORAGE_COUNTER_REPAIR_REQUIRED, safeReason);
    }

    if (!_repairJob.active) {
        _repairJob.reason = safeReason;
    }

    if (!wasRequested) {
        DLOG_WARN("STORAGE", "Spool repair queued reason=%s", safeReason);
    }
    return true;
}

void StorageManager::_resetRepairJob() {
    _repairJob = SpoolRepairJob();
}

void StorageManager::_startRepairJob(const char* reason) {
    const String jobReason = (reason && reason[0]) ? String(reason)
                                                   : _repairJob.reason;
    _resetRepairJob();
    _repairJob.active = true;
    _repairJob.reason = jobReason.length() ? jobReason : String("maintenance");
    _repairJob.startMs = millis();
    _repairJob.audit.oldPendingTotal = _spoolIndex.pendingTotal;
    _repairJob.audit.oldNextEventId = _nextEventId;
    _repairJob.repairedSegments.reserve(_spoolIndex.segments.size());
    _repairJob.rebuiltSessions.reserve(_spoolIndex.sessions.size());
    DLOG_INFO("STORAGE", "Spool repair start reason=%s segs=%u",
              _repairJob.reason.c_str(),
              static_cast<unsigned>(_spoolIndex.segments.size()));
}

void StorageManager::_rememberRepairSession(const String& sessionId) {
    if (!sessionId.length()) {
        return;
    }
    for (const auto& existing : _repairJob.segmentSessions) {
        if (existing == sessionId) {
            return;
        }
    }
    _repairJob.segmentSessions.push_back(sessionId);
}

bool StorageManager::_beginRepairSegment() {
    while (_repairJob.segmentIndex < _spoolIndex.segments.size()) {
        const SpoolSegmentInfo& seg = _spoolIndex.segments[_repairJob.segmentIndex];

        // Skip the active write-head segment — it is a moving target.
        // Its live counters are trusted and re-injected in _finalizeRepairJob.
        if (seg.segmentId == _spoolIndex.activeSegmentId) {
            _repairJob.segmentIndex++;
            continue;
        }

        if (_isSegmentQuarantined(seg.segmentId)) {
            _repairJob.audit.quarantinedSegments++;
            _repairJob.segmentChanged = true;
            _repairJob.segmentIndex++;
            continue;
        }

        const String segmentPath =
            _spoolSegmentPathForFormat(seg.segmentId, seg.format);
        if (!LittleFS.exists(segmentPath)) {
            _repairJob.audit.unreadableSegments++;
            _repairJob.audit.hadFatalSegmentError = true;
            _repairJob.segmentChanged = true;
            _repairJob.audit.repaired = true;

            DLOG_WARN("STORAGE",
                      "Spool repair dropping missing segment seg=%lu path=%s",
                      static_cast<unsigned long>(seg.segmentId),
                      segmentPath.c_str());
            const String indexPath = _uploadIndexPath(seg.segmentId);
            if (LittleFS.exists(indexPath) && !LittleFS.remove(indexPath)) {
                DLOG_WARN("STORAGE",
                          "Upload index sidecar remove failed missing seg=%lu path=%s",
                          static_cast<unsigned long>(seg.segmentId),
                          indexPath.c_str());
            }
            _repairJob.segmentIndex++;
            continue;
        }

        _repairJob.originalSegment = seg;
        _repairJob.segmentSessions.clear();
        _repairJob.lastSession = "";

        _repairJob.rebuiltSegment = seg;
        _clearSegmentSummary(_repairJob.rebuiltSegment);
        _repairJob.rebuiltSegment.segmentId = seg.segmentId;
        _repairJob.rebuiltSegment.format = seg.format;
        _repairJob.rebuiltSegment.approxBytes = seg.approxBytes;
        _repairJob.rebuiltSegment.firstEventId = seg.firstEventId;
        _repairJob.rebuiltSegment.lastEventId = 0;
        _repairJob.rebuiltSegment.recordCount = 0;
        _repairJob.fileOffset = 0;
        _repairJob.segmentValidEventRecords = 0;
        _repairJob.segmentValidEnrichDeltas = 0;

        if (seg.format == SPOOL_SEGMENT_BIN_V2) {
            BinaryCheckpointScanResult checkpointScan;
            const bool probeOk = _findLatestBinaryCheckpoint(segmentPath, checkpointScan);
            if (probeOk && checkpointScan.found) {
                const auto& cp = checkpointScan.checkpoint;
                _repairJob.rebuiltSegment.recordCount = cp.recordCount;
                _repairJob.rebuiltSegment.eventCount = cp.eventCount;
                _repairJob.rebuiltSegment.enrichDeltaCount = cp.enrichDeltaCount;
                _repairJob.rebuiltSegment.missionCount = cp.missionCount;
                _repairJob.rebuiltSegment.noiseCount = cp.noiseCount;
                _repairJob.rebuiltSegment.pendingUploadMissionCount =
                    cp.pendingUploadMissionCount;
                _repairJob.rebuiltSegment.pendingUploadNoiseCount =
                    cp.pendingUploadNoiseCount;
                _repairJob.rebuiltSegment.p0Count = cp.p0Count;
                _repairJob.rebuiltSegment.p1Count = cp.p1Count;
                _repairJob.rebuiltSegment.p2Count = cp.p2Count;
                _repairJob.rebuiltSegment.p3Count = cp.p3Count;
                _repairJob.rebuiltSegment.pendingEnrichmentCount =
                    cp.pendingEnrichmentCount;
                _repairJob.rebuiltSegment.minTimestampMs = cp.minTimestampMs;
                _repairJob.rebuiltSegment.maxTimestampMs = cp.maxTimestampMs;
                _repairJob.rebuiltSegment.lastEventId = cp.lastEventId;
                _repairJob.rebuiltSegment.summaryVersion =
                    SPOOL_SEGMENT_SUMMARY_VERSION;
                _repairJob.rebuiltSegment.summaryValid = true;
                _repairJob.rebuiltSegment.trustState = SPOOL_SEGMENT_TRUSTED;
                _repairJob.fileOffset = checkpointScan.nextOffset;
                _repairJob.segmentValidEventRecords = cp.eventCount;
                _repairJob.segmentValidEnrichDeltas = cp.enrichDeltaCount;
                _repairJob.audit.rebuiltPendingTotal +=
                    cp.pendingUploadMissionCount + cp.pendingUploadNoiseCount;
                _repairJob.audit.maxEventIdSeen =
                    std::max(_repairJob.audit.maxEventIdSeen, cp.lastEventId);
                DLOG_INFO("STORAGE",
                          "Spool repair checkpoint seg=%lu off=%lu records=%lu events=%lu enrich=%lu",
                          static_cast<unsigned long>(seg.segmentId),
                          static_cast<unsigned long>(_repairJob.fileOffset),
                          static_cast<unsigned long>(cp.recordCount),
                          static_cast<unsigned long>(cp.eventCount),
                          static_cast<unsigned long>(cp.enrichDeltaCount));
            } else if (!probeOk) {
                DLOG_WARN("STORAGE",
                          "Spool repair checkpoint probe failed seg=%lu path=%s",
                          static_cast<unsigned long>(seg.segmentId),
                          segmentPath.c_str());
            }
        }

        _repairJob.scanningSegment = true;
        _repairJob.audit.scannedSegments++;
        return true;
    }

    return false;
}

bool StorageManager::_repairJsonlSlice(uint32_t startMs,
                                       uint32_t budgetMs,
                                       uint32_t maxRecords,
                                       uint32_t& recordsScanned) {
    const String path =
        _spoolSegmentPathForFormat(_repairJob.originalSegment.segmentId,
                                   SPOOL_SEGMENT_JSONL);
    File f = LittleFS.open(path, "r");
    if (!f) {
        return false;
    }
    if (_repairJob.fileOffset > 0 && !f.seek(_repairJob.fileOffset)) {
        f.close();
        return false;
    }

    while (f.available() && recordsScanned < maxRecords) {
        // Check budget AFTER the first record so file-open overhead can't
        // prevent any progress on a 2ms budget.
        if (recordsScanned > 0 && (millis() - startMs) >= budgetMs) {
            break;
        }
        String line = f.readStringUntil('\n');
        _repairJob.fileOffset = f.position();
        line.trim();
        if (!line.length()) {
            continue;
        }

        recordsScanned++;

        JsonDocument doc;
        if (deserializeJson(doc, line)) {
            continue;
        }

        DecodedSpoolRecord rec;
        rec.eventId = doc["id"] | 0U;
        rec.sessionId = String(doc[F_SESSION] | doc["session_id"] | "");
        rec.doc = doc;

        const char* recType = doc["type"] | "";
        rec.recordType = (strcmp(recType, "enrich_delta") == 0)
            ? SPOOL_REC_ENRICH_DELTA
            : SPOOL_REC_EVENT;

        _repairJob.audit.scannedRecords++;

        const bool isEvent = rec.recordType == SPOOL_REC_EVENT;
        const bool isEnrich = rec.recordType == SPOOL_REC_ENRICH_DELTA;
        if (!isEvent && !isEnrich) {
            _repairJob.audit.invalidRecords++;
            _repairJob.audit.skippedRecords++;
            continue;
        }

        if (!rec.eventId || !rec.sessionId.length()) {
            _repairJob.audit.invalidRecords++;
            _repairJob.audit.skippedRecords++;
            continue;
        }

        _repairJob.rebuiltSegment.recordCount++;
        _repairJob.audit.maxEventIdSeen =
            std::max(_repairJob.audit.maxEventIdSeen, rec.eventId);
        _rememberRepairSession(rec.sessionId);

        if (isEvent) {
            _repairJob.audit.validEventRecords++;
            const uint32_t watermark = _uploadedWatermarkForSession(rec.sessionId);
            if (rec.eventId > watermark) {
                _repairJob.audit.rebuiltPendingTotal++;
            }
            _normalizeCapturedEvent(rec.doc.as<JsonObject>());
        } else {
            _repairJob.audit.validEnrichDeltas++;
        }

        _updateSegmentSummaryFromDecodedRecord(_repairJob.rebuiltSegment, rec);
    }

    const bool done = !f.available();
    f.close();
    if (done) {
        _repairJob.scanningSegment = false;
    }
    return true;
}

bool StorageManager::_repairBinaryMetaSlice(uint32_t startMs,
                                            uint32_t budgetMs,
                                            uint32_t maxRecords,
                                            uint32_t& recordsScanned) {
    const uint32_t segmentId = _repairJob.originalSegment.segmentId;
    const String path = _spoolBinarySegmentPath(segmentId);
    File f = LittleFS.open(path, "r");
    if (!f) {
        return false;
    }

    SpoolBin::SegmentHeaderV2 hdr;
    if (!SpoolBin::readSegmentHeaderV2(f, hdr)) {
        DLOG_WARN("STORAGE", "Repair hdr read failed path=%s", path.c_str());
        f.close();
        return false;
    }

    if (hdr.magic != SpoolBin::SEGMENT_MAGIC || hdr.version != 2) {
        DLOG_WARN("STORAGE", "Repair invalid hdr path=%s", path.c_str());
        f.close();
        return false;
    }

    if (_repairJob.fileOffset == 0) {
        _repairJob.fileOffset = sizeof(SpoolBin::SegmentHeaderV2);
    }

    if (!f.seek(_repairJob.fileOffset)) {
        f.close();
        return false;
    }

    uint32_t skipWarnCount = 0;
    while (f.position() < f.size() && recordsScanned < maxRecords) {
        // Check budget AFTER the first record so file-open + seek overhead
        // can't exhaust a 2ms budget before a single record is processed.
        if (recordsScanned > 0 && (millis() - startMs) >= budgetMs) {
            break;
        }
        const size_t remainingBeforePrefix =
            static_cast<size_t>(f.size() - f.position());
        if (remainingBeforePrefix < sizeof(SpoolBin::RecordPrefix)) {
            DLOG_WARN("STORAGE",
                      "Repair truncated tail path=%s remaining=%u",
                      path.c_str(),
                      static_cast<unsigned>(remainingBeforePrefix));
            f.close();
            return false;
        }

        SpoolBin::RecordPrefix prefix;
        if (!SpoolBin::readBytes(f, &prefix, sizeof(prefix))) {
            DLOG_WARN("STORAGE", "Repair prefix read failed path=%s", path.c_str());
            f.close();
            return false;
        }

        const size_t remainingAfterPrefix =
            static_cast<size_t>(f.size() - f.position());
        if (prefix.length > remainingAfterPrefix) {
            DLOG_WARN("STORAGE",
                      "Repair truncated body path=%s len=%u remaining=%u",
                      path.c_str(),
                      static_cast<unsigned>(prefix.length),
                      static_cast<unsigned>(remainingAfterPrefix));
            f.close();
            return false;
        }

        std::vector<uint8_t> body(prefix.length);
        if (prefix.length > 0 &&
            !SpoolBin::readBytes(f, body.data(), prefix.length)) {
            DLOG_WARN("STORAGE", "Repair body read failed path=%s", path.c_str());
            f.close();
            return false;
        }

        if (prefix.type == SpoolBin::REC_CHECKPOINT) {
            _repairJob.fileOffset = f.position();
            recordsScanned++;
            continue;
        }

        _repairJob.fileOffset = f.position();
        recordsScanned++;
        _repairJob.audit.scannedRecords++;

        const String sessionSeed = _repairJob.lastSession;
        BinaryMetaRecord rec;
        if (!_decodeBinaryMetaRecord(body.data(),
                                     body.size(),
                                     prefix.type,
                                     _repairJob.lastSession,
                                     _repairJob.lastSessionTag,
                                     _repairJob.enrichCtx,
                                     rec)) {
            _repairJob.audit.invalidRecords++;
            _repairJob.audit.skippedRecords++;
            if (skipWarnCount < 4) {
                DLOG_WARN("STORAGE",
                          "Repair decode skip path=%s type=%u len=%u skips=%lu",
                          path.c_str(),
                          static_cast<unsigned>(prefix.type),
                          static_cast<unsigned>(prefix.length),
                          static_cast<unsigned long>(_repairJob.audit.skippedRecords));
                skipWarnCount++;
            }
            continue;
        }

        _repairJob.rebuiltSegment.recordCount++;
        _rememberRepairSession(rec.sessionId);

        if (rec.recordType == SPOOL_REC_EVENT) {
            _repairJob.audit.validEventRecords++;
            _repairJob.segmentValidEventRecords++;
            const uint32_t watermark = _uploadedWatermarkForSession(rec.sessionId);
            if (rec.eventId > watermark) {
                _repairJob.audit.rebuiltPendingTotal++;
            }
            DecodedSpoolRecord decoded;
            if (_decodeBinarySpoolRecordBody(segmentId,
                                             prefix.type,
                                             body.data(),
                                             body.size(),
                                             hdr.createdMs,
                                             hdr.createdEpochUtc,
                                             sessionSeed,
                                             decoded)) {
                _normalizeCapturedEvent(decoded.doc.as<JsonObject>());
                _updateSegmentSummaryFromDecodedRecord(_repairJob.rebuiltSegment,
                                                       decoded);
            } else {
                _repairJob.rebuiltSegment.summaryValid = false;
            }
            if (rec.eventId != 0) {
                if (_repairJob.rebuiltSegment.firstEventId == 0 ||
                    rec.eventId < _repairJob.rebuiltSegment.firstEventId) {
                    _repairJob.rebuiltSegment.firstEventId = rec.eventId;
                }
                if (rec.eventId > _repairJob.rebuiltSegment.lastEventId) {
                    _repairJob.rebuiltSegment.lastEventId = rec.eventId;
                }
            }
        } else if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
            _repairJob.audit.validEnrichDeltas++;
            _repairJob.segmentValidEnrichDeltas++;
            _repairJob.rebuiltSegment.enrichDeltaCount++;
        }

        if (rec.eventId > _repairJob.audit.maxEventIdSeen) {
            _repairJob.audit.maxEventIdSeen = rec.eventId;
        }
    }

    const bool done = f.position() >= f.size();
    f.close();
    if (done) {
        _repairJob.scanningSegment = false;
    }
    return true;
}

void StorageManager::_finishRepairSegment() {
    if (_repairJob.originalSegment.format == SPOOL_SEGMENT_BIN_V2) {
        _repairJob.rebuiltSegment.eventCount =
            _repairJob.segmentValidEventRecords;
        _repairJob.rebuiltSegment.enrichDeltaCount =
            _repairJob.segmentValidEnrichDeltas;

        const SpoolSegmentInfo& before = _repairJob.originalSegment;
        const bool existingSummaryStillMatches =
            before.summaryValid &&
            before.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION &&
            before.recordCount == _repairJob.rebuiltSegment.recordCount &&
            before.eventCount == _repairJob.rebuiltSegment.eventCount &&
            before.enrichDeltaCount == _repairJob.rebuiltSegment.enrichDeltaCount &&
            before.firstEventId == _repairJob.rebuiltSegment.firstEventId &&
            before.lastEventId == _repairJob.rebuiltSegment.lastEventId;

        const bool rebuiltSummaryReady =
            _repairJob.rebuiltSegment.summaryValid &&
            _repairJob.rebuiltSegment.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;

        if (rebuiltSummaryReady) {
            // Full binary decode during maintenance rebuilt the lane/priority
            // counters in slices, so keep the freshly computed summary.
        } else if (existingSummaryStillMatches) {
            _repairJob.rebuiltSegment.summaryVersion = before.summaryVersion;
            _repairJob.rebuiltSegment.summaryValid = before.summaryValid;
            _repairJob.rebuiltSegment.missionCount = before.missionCount;
            _repairJob.rebuiltSegment.noiseCount = before.noiseCount;
            _repairJob.rebuiltSegment.pendingUploadMissionCount =
                before.pendingUploadMissionCount;
            _repairJob.rebuiltSegment.pendingUploadNoiseCount =
                before.pendingUploadNoiseCount;
            _repairJob.rebuiltSegment.p0Count = before.p0Count;
            _repairJob.rebuiltSegment.p1Count = before.p1Count;
            _repairJob.rebuiltSegment.p2Count = before.p2Count;
            _repairJob.rebuiltSegment.p3Count = before.p3Count;
            _repairJob.rebuiltSegment.pendingEnrichmentCount =
                before.pendingEnrichmentCount;
            _repairJob.rebuiltSegment.minTimestampMs = before.minTimestampMs;
            _repairJob.rebuiltSegment.maxTimestampMs = before.maxTimestampMs;
        } else {
            _repairJob.rebuiltSegment.summaryValid = false;
        }
    }

    _repairJob.rebuiltSegment.trustState = SPOOL_SEGMENT_TRUSTED;

    for (const auto& sessionId : _repairJob.segmentSessions) {
        bool seen = false;
        for (const auto& existing : _repairJob.rebuiltSessions) {
            if (existing == sessionId) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            _repairJob.rebuiltSessions.push_back(sessionId);
        }
    }

    if (!_spoolSegmentInfoEquals(_repairJob.originalSegment,
                                 _repairJob.rebuiltSegment)) {
        _repairJob.segmentChanged = true;
    }
    _repairJob.repairedSegments.push_back(_repairJob.rebuiltSegment);
    _repairJob.segmentIndex++;
    _repairJob.scanningSegment = false;
    _repairJob.segmentSessions.clear();
    _repairJob.lastSession = "";
}

bool StorageManager::_finalizeRepairJob() {
    SpoolRepairJob job = _repairJob;

    // Log if _nextEventId advanced during scan — informational only.
    // We skip the active write-head segment, so counter advances are expected
    // and do not invalidate closed-segment repair results.
    if (_nextEventId != job.audit.oldNextEventId) {
        DLOG_INFO("STORAGE",
                  "Spool repair: counter advanced during scan old=%lu now=%lu "
                  "(active segment skipped, OK)",
                  static_cast<unsigned long>(job.audit.oldNextEventId),
                  static_cast<unsigned long>(_nextEventId));
    }

    // Capture the live active-segment state before we may overwrite
    // _spoolIndex.segments below.  We skipped this segment during scanning
    // because it is a live write target; fold its current pending counts into
    // the rebuilt total and re-inject the segment entry after the merge.
    const uint32_t activeId = _spoolIndex.activeSegmentId;
    SpoolSegmentInfo activeLiveSnap;
    bool haveActiveLiveSnap = false;
    if (activeId != 0) {
        const SpoolSegmentInfo* ap = _findSegmentInfo(activeId);
        if (ap) {
            activeLiveSnap     = *ap;
            activeLiveSnap.trustState = SPOOL_SEGMENT_TRUSTED;
            haveActiveLiveSnap = true;
            job.audit.rebuiltPendingTotal +=
                activeLiveSnap.pendingUploadMissionCount +
                activeLiveSnap.pendingUploadNoiseCount;
        }
    }

    const bool sessionsChanged =
        job.rebuiltSessions.size() != _spoolIndex.sessions.size() ||
        !std::equal(job.rebuiltSessions.begin(),
                    job.rebuiltSessions.end(),
                    _spoolIndex.sessions.begin());
    const uint32_t safeNextFloor =
        std::max<uint32_t>(job.audit.maxEventIdSeen,
                           job.audit.validEventRecords);

    job.audit.hadMismatch =
        job.audit.oldPendingTotal != job.audit.rebuiltPendingTotal ||
        job.audit.oldNextEventId <= safeNextFloor ||
        job.audit.quarantinedSegments > 0 ||
        job.audit.invalidRecords > 0 ||
        job.audit.hadFatalSegmentError ||
        job.segmentChanged ||
        sessionsChanged;

    bool persistOk = true;
    if (job.audit.hadMismatch && !job.audit.scanIncomplete) {
        if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
            _selectRepairMode() != REPAIR_EMERGENCY) {
            _spoolAuditRepairRequired = true;
            _repairRequested = true;
            DLOG_INFO("STORAGE",
                      "Spool repair deferred owner=%s reason=%s",
                      RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                      job.reason.c_str());
            _resetRepairJob();
            refreshStorageUiState();
            return false;
        }

        _spoolIndex.pendingTotal = job.audit.rebuiltPendingTotal;
        _pendingEventCount = job.audit.rebuiltPendingTotal;
        _bumpStorageMetaGeneration();

        const uint32_t rebuiltNext =
            (job.audit.maxEventIdSeen == UINT32_MAX)
                ? UINT32_MAX
                : std::max<uint32_t>(job.audit.maxEventIdSeen + 1U,
                                     job.audit.validEventRecords + 1U);
        if (_nextEventId < rebuiltNext) {
            _nextEventId = rebuiltNext;
            _spoolIndex.nextEventId = _nextEventId;
            _bumpStorageMetaGeneration();
            _eventCounterDirty = true;
            _eventCounterPendingWrites = 1;
        }

        _spoolIndex.segments.assign(job.repairedSegments.begin(),
                                    job.repairedSegments.end());
        _spoolIndex.segments.erase(
            std::remove_if(_spoolIndex.segments.begin(),
                           _spoolIndex.segments.end(),
                           [&](const SpoolSegmentInfo& seg) {
                               return seg.segmentId == 0;
                           }),
            _spoolIndex.segments.end());

        // Re-inject the live active segment (it was skipped during scan).
        // Use the snapshot captured before we overwrote _spoolIndex.segments.
        if (haveActiveLiveSnap) {
            const bool present = std::any_of(
                _spoolIndex.segments.begin(),
                _spoolIndex.segments.end(),
                [&](const SpoolSegmentInfo& s) {
                    return s.segmentId == activeId;
                });
            if (!present) {
                _spoolIndex.segments.push_back(activeLiveSnap);
                std::sort(_spoolIndex.segments.begin(),
                          _spoolIndex.segments.end(),
                          [](const SpoolSegmentInfo& a,
                             const SpoolSegmentInfo& b) {
                              return a.segmentId < b.segmentId;
                          });
            }
        }

        if (!_spoolIndex.segments.empty()) {
            _spoolIndex.oldestSegmentId = _spoolIndex.segments.front().segmentId;
        } else {
            _spoolIndex.oldestSegmentId = 0;
        }

        _spoolIndex.sessions.assign(job.rebuiltSessions.begin(),
                                    job.rebuiltSessions.end());
        _pendingCountDirty = false;
        _spoolIndexDirty = true;

        for (auto it = _binaryLastSessionBySegment.begin();
             it != _binaryLastSessionBySegment.end();) {
            const bool keep =
                std::any_of(_spoolIndex.segments.begin(),
                            _spoolIndex.segments.end(),
                            [&](const SpoolSegmentInfo& seg) {
                                return seg.segmentId == it->first;
                            });
            if (!keep) {
                _binaryLastSessionTagBySegment.erase(it->first);
                _binaryEnrichCtxBySegment.erase(it->first);
                it = _binaryLastSessionBySegment.erase(it);
            } else {
                ++it;
            }
        }

        for (auto it = _binaryCheckpointBySegment.begin();
             it != _binaryCheckpointBySegment.end();) {
            const bool keep =
                std::any_of(_spoolIndex.segments.begin(),
                            _spoolIndex.segments.end(),
                            [&](const SpoolSegmentInfo& seg) {
                                return seg.segmentId == it->first;
                            });
            if (!keep) {
                it = _binaryCheckpointBySegment.erase(it);
            } else {
                ++it;
            }
        }

        const bool activeStillExists =
            _spoolIndex.activeSegmentId != 0 &&
            _findSegmentInfo(_spoolIndex.activeSegmentId) != nullptr;
        if (!activeStillExists) {
            _spoolIndex.activeSegmentId = 0;
            if (!_openNewSpoolSegment()) {
                job.audit.hadFatalSegmentError = true;
                persistOk = false;
            }
        }

        if (persistOk &&
            (!_persistEventCounter(true, "repair_finalize") ||
             !_persistSpoolIndex(true, "repair_finalize") ||
             !_persistEventMeta(true, "repair_finalize"))) {
            persistOk = false;
        }

        job.audit.repaired = persistOk;
    } else {
        _pendingCountDirty = false;
    }

    _spoolSummaryRebuildPending = _hasInvalidSpoolSummaries();

    if (!persistOk) {
        if (job.audit.hadFatalSegmentError) {
            _setCounterTrustState(CounterTrust::EmergencyOnly,
                                  "repair_fatal_segment");
        } else {
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                  "repair_persist_failed");
        }
    } else if (job.audit.hadFatalSegmentError) {
        _setCounterTrustState(CounterTrust::EmergencyOnly,
                              "repair_quarantine");
    } else if (_spoolSummaryRebuildPending ||
               _pendingCountDirty ||
               _workerMetadataDirtyPending) {
        _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                              "repair_snapshot_lagged");
    } else {
        _setCounterTrustState(STORAGE_COUNTER_TRUSTED, "repair_complete");
    }

    const uint32_t elapsedMs = millis() - job.startMs;
    if (elapsedMs > 200U || job.audit.scannedRecords > 512U) {
        DLOG_WARN("STORAGE",
                  "Spool repair slow reason=%s segs=%lu recs=%lu ms=%lu",
                  job.reason.c_str(),
                  static_cast<unsigned long>(job.audit.scannedSegments),
                  static_cast<unsigned long>(job.audit.scannedRecords),
                  static_cast<unsigned long>(elapsedMs));
    }
    _logSpoolAuditResult(job.reason.c_str(), job.audit);

    _resetRepairJob();

    if (!persistOk) {
        _repairRequested = true;
        _spoolAuditRepairRequired = true;
        refreshStorageUiState();
        return false;
    }

    _repairRequested = false;
    _spoolAuditRepairRequired = false;
    refreshStorageUiState();
    return true;
}

SpoolRepairMode StorageManager::_selectRepairMode() const {
    if (!_ready || !hasSpoolRepairWork()) {
        return REPAIR_FAST_IDLE;
    }

    if (_counterTrustState == CounterTrust::EmergencyOnly) {
        return REPAIR_EMERGENCY;
    }

    // Emergency is reserved for boots that never managed to load any metadata.
    if (!_eventMetaLoaded && !_eventCounterLoaded) {
        return REPAIR_EMERGENCY;
    }

    const RadioOwner owner = RADIO_ARB.currentOwner();
    if (owner == RADIO_WIFI_CAPTURE) {
        return REPAIR_BACKGROUND;
    }
    const bool maintenanceOwner =
        owner == RADIO_NONE || owner == RADIO_STORAGE_MAINTENANCE;

    RAMSpool::Stats spool = {};
    if (RAMSpool::isReady()) {
        spool = RAMSpool::snapshot();
    }

    const bool writerPressure =
        _workerAppendBatchActive ||
        _uploadBatchActive ||
        _uploadBatchDirty ||
        (!maintenanceOwner &&
         (_workerMetadataDirtyPending ||
          _pendingCountDirty ||
          _spoolIndexDirty ||
          _spoolSummaryRebuildPending));
    const bool spoolPressure =
        spool.inflight >= 384U ||
        spool.pressureState >= 2U;
    const uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const uint32_t largestHeap =
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const bool heapTight =
        freeHeap < (96UL * 1024UL) ||
        largestHeap < (32UL * 1024UL);

    if (writerPressure || spoolPressure || heapTight) {
        return REPAIR_BACKGROUND;
    }

    return maintenanceOwner ? REPAIR_FAST_IDLE : REPAIR_BACKGROUND;
}

// _repairBudgetsForMode moved to SpoolRepairTypes.h as inline
// repairBudgetsForMode free function.

bool StorageManager::_shouldDeferWorkerMetadataFlush() const {
    const RadioOwner owner = RADIO_ARB.currentOwner();
    if (owner == RADIO_WIFI_CAPTURE) {
        return _selectRepairMode() != REPAIR_EMERGENCY;
    }

    return hasSpoolRepairWork() &&
           _selectRepairMode() == REPAIR_BACKGROUND;
}

const char* StorageManager::_maintenanceReasonText(StorageMaintenanceReason reason) const {
    switch (reason) {
        case STORAGE_MAINT_DIRTY_SPOOL_INDEX:
            return "dirty_spool_index";
        case STORAGE_MAINT_DIRTY_SUMMARY:
            return "dirty_summary";
        case STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST:
            return "boot_safe_deferred_persist";
        case STORAGE_MAINT_SNAPSHOT_LAGGED:
            return "snapshot_lagged";
        case STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY:
            return "upload_enrich_cursor_dirty";
        case STORAGE_MAINT_ACTIVE_SEGMENT_INVALID:
            return "active_segment_invalid";
        case STORAGE_MAINT_COUNTER_UNTRUSTED:
            return "counter_untrusted";
        case STORAGE_MAINT_EMERGENCY_REPAIR:
            return "emergency_repair";
        case STORAGE_MAINT_ACTIVE_SEGMENT_NEAR_FULL:
            return "active_segment_near_full";
        case STORAGE_MAINT_SEGMENT_AUDIT:
            return "segment_audit";
        case STORAGE_MAINT_DELETE_DRAINED:
            return "delete_drained";
        case STORAGE_MAINT_CAPTURE_INDEX_DIRTY:
            return "capture_index_dirty";
        case STORAGE_MAINT_FS_AUDIT:
            return "fs_audit";
        case STORAGE_MAINT_BINARY_CHECKPOINT:
            return "binary_checkpoint";
        case STORAGE_MAINT_CONTINUITY_PASS:
            return "continuity_pass";
        case STORAGE_MAINT_NONE:
        default:
            return "none";
    }
}

const char* StorageManager::_maintenanceFlagsText(uint32_t flags) const {
    if (flags == STORAGE_MAINT_NONE) {
        strlcpy(_maintenanceFlagsTextBuf, "none", sizeof(_maintenanceFlagsTextBuf));
        return _maintenanceFlagsTextBuf;
    }

    _maintenanceFlagsTextBuf[0] = '\0';
    auto appendFlag = [&](StorageMaintenanceReason reason) {
        if ((flags & static_cast<uint32_t>(reason)) == 0U) {
            return;
        }
        if (_maintenanceFlagsTextBuf[0] != '\0') {
            strlcat(_maintenanceFlagsTextBuf, "|", sizeof(_maintenanceFlagsTextBuf));
        }
        strlcat(_maintenanceFlagsTextBuf,
                _maintenanceReasonText(reason),
                sizeof(_maintenanceFlagsTextBuf));
    };

    appendFlag(STORAGE_MAINT_DIRTY_SPOOL_INDEX);
    appendFlag(STORAGE_MAINT_DIRTY_SUMMARY);
    appendFlag(STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST);
    appendFlag(STORAGE_MAINT_SNAPSHOT_LAGGED);
    appendFlag(STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY);
    appendFlag(STORAGE_MAINT_ACTIVE_SEGMENT_INVALID);
    appendFlag(STORAGE_MAINT_COUNTER_UNTRUSTED);
    appendFlag(STORAGE_MAINT_EMERGENCY_REPAIR);
    appendFlag(STORAGE_MAINT_ACTIVE_SEGMENT_NEAR_FULL);
    appendFlag(STORAGE_MAINT_SEGMENT_AUDIT);
    appendFlag(STORAGE_MAINT_DELETE_DRAINED);
    appendFlag(STORAGE_MAINT_CAPTURE_INDEX_DIRTY);
    appendFlag(STORAGE_MAINT_FS_AUDIT);
    appendFlag(STORAGE_MAINT_BINARY_CHECKPOINT);
    appendFlag(STORAGE_MAINT_CONTINUITY_PASS);
    return _maintenanceFlagsTextBuf;
}

uint32_t StorageManager::_derivedMaintenanceFlags() const {
    uint32_t flags = STORAGE_MAINT_NONE;

    if (_spoolIndexDirty) {
        flags |= STORAGE_MAINT_DIRTY_SPOOL_INDEX;
    }
    if (_spoolSummaryRebuildPending || _hasInvalidSpoolSummaries() ||
        (_storageUiRefreshPending && _storageUiRefreshDue())) {
        flags |= STORAGE_MAINT_DIRTY_SUMMARY;
    }
    if (_workerMetadataDirtyPending || _eventCounterDirty || _pendingCountDirty) {
        flags |= STORAGE_MAINT_SNAPSHOT_LAGGED;
    }
    if (_uploadBatchDirty) {
        flags |= STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY;
    }
    if (_spoolIndex.activeSegmentId == 0 ||
        (_spoolIndex.activeSegmentId != 0 &&
         _findSegmentInfo(_spoolIndex.activeSegmentId) == nullptr)) {
        flags |= STORAGE_MAINT_ACTIVE_SEGMENT_INVALID;
    }
    if (_counterTrustState == CounterTrust::Degraded ||
        _counterTrustState == CounterTrust::RepairRequired ||
        _counterTrustState == CounterTrust::EmergencyOnly) {
        flags |= STORAGE_MAINT_COUNTER_UNTRUSTED;
    }
    if (_repairRequested || _spoolAuditRepairRequired || _repairJob.active) {
        flags |= STORAGE_MAINT_SEGMENT_AUDIT;
    }
    if (_activeSegmentNearRotateThreshold()) {
        flags |= STORAGE_MAINT_ACTIVE_SEGMENT_NEAR_FULL;
    }
    if (_binaryCheckpointDeferred) {
        flags |= STORAGE_MAINT_BINARY_CHECKPOINT;
    }

    return flags;
}

uint32_t StorageManager::maintenanceFlags() const {
    return _maintenanceRequestedFlags | _derivedMaintenanceFlags();
}

const char* StorageManager::maintenanceFlagsText() const {
    return _maintenanceFlagsText(maintenanceFlags());
}

void StorageManager::requestMaintenance(StorageMaintenanceReason reason,
                                        const char* source) {
    if (reason == STORAGE_MAINT_NONE) {
        return;
    }

    if (reason == STORAGE_MAINT_FS_AUDIT &&
        _maintenanceContinuityCurrent()) {
        _lastFsAuditCompletedMs = millis();
        DLOG_INFO("STORAGE",
                  "Maintenance request satisfied by continuity log source=%s gen=%lu",
                  (source && source[0]) ? source : "-",
                  static_cast<unsigned long>(_maintenanceContinuity.storageGeneration));
        return;
    }

    const bool routineBookkeeping =
        reason == STORAGE_MAINT_SNAPSHOT_LAGGED ||
        reason == STORAGE_MAINT_DIRTY_SUMMARY;
    const uint32_t bit = static_cast<uint32_t>(reason);
    const bool alreadySet = (_maintenanceRequestedFlags & bit) != 0U;
    _maintenanceRequestedFlags |= bit;
    if (!routineBookkeeping) {
        _maintenanceCaptureGate = true;
    }

    if (!alreadySet) {
        _maintenanceHeapRetryAfterMs = 0;
        if (reason == STORAGE_MAINT_EMERGENCY_REPAIR) {
            _maintenanceFullRebuildAttempted = false;
        }
        if (routineBookkeeping) {
            _routineMaintenanceRequestCount++;
            DLOG_DEBUG("STORAGE",
                       "Maintenance requested reason=%s source=%s flags=%s",
                       _maintenanceReasonText(reason),
                       (source && source[0]) ? source : "-",
                       _maintenanceFlagsText(maintenanceFlags()));
            const uint32_t now = millis();
            if (_routineMaintenanceLogWindowMs == 0 ||
                static_cast<int32_t>(now - _routineMaintenanceLogWindowMs) >=
                    60000) {
                DLOG_INFO("STORAGE",
                          "Routine maintenance requests coalesced count=%lu latest=%s source=%s flags=%s",
                          static_cast<unsigned long>(_routineMaintenanceRequestCount),
                          _maintenanceReasonText(reason),
                          (source && source[0]) ? source : "-",
                          _maintenanceFlagsText(maintenanceFlags()));
                _routineMaintenanceRequestCount = 0;
                _routineMaintenanceLogWindowMs = now;
            }
        } else {
            DLOG_INFO("STORAGE",
                      "Maintenance requested reason=%s source=%s flags=%s",
                      _maintenanceReasonText(reason),
                      (source && source[0]) ? source : "-",
                      _maintenanceFlagsText(maintenanceFlags()));
        }
    }
}

void StorageManager::completeExternalMaintenance(StorageMaintenanceReason reason,
                                                 const char* source) {
    if (reason == STORAGE_MAINT_NONE) {
        return;
    }
    _clearMaintenanceFlags(static_cast<uint32_t>(reason));
    DLOG_INFO("STORAGE",
              "External maintenance complete reason=%s source=%s flags=%s",
              _maintenanceReasonText(reason),
              (source && source[0]) ? source : "-",
              _maintenanceFlagsText(maintenanceFlags()));
}

void StorageManager::_clearMaintenanceFlags(uint32_t flags) {
    _maintenanceRequestedFlags &= ~flags;
}

bool StorageManager::_loadMaintenanceContinuityLog() {
    _maintenanceContinuity = {};
    _maintenanceContinuityValid = false;

    MaintenanceContinuityRecord rec{};
    if (!MaintenanceContinuity::load(rec)) {
        return false;
    }

    _maintenanceContinuity = rec;
    _maintenanceContinuityValid = true;
    DLOG_INFO("STORAGE",
              "Maintenance continuity log loaded clean=%u remaining=%s gen=%lu pending=%lu",
              rec.clean ? 1U : 0U,
              _maintenanceFlagsText(rec.remainingFlags),
              static_cast<unsigned long>(rec.storageGeneration),
              static_cast<unsigned long>(rec.pendingTotal));
    return true;
}

bool StorageManager::_writeMaintenanceContinuityLog(uint32_t completedFlags,
                                                   uint32_t remainingFlags,
                                                   bool clean,
                                                   const char* reason) {
    MaintenanceContinuityRecord rec{};
    rec.completedFlags = completedFlags;
    rec.remainingFlags = remainingFlags;
    rec.storageGeneration = _storageMetaGeneration;
    rec.spoolGeneration = _spoolIndex.generation;
    rec.eventMetaGeneration = _eventMetaGeneration;
    rec.eventCounterGeneration = _eventCounterGeneration;
    rec.activeSegmentId = _spoolIndex.activeSegmentId;
    rec.nextEventId = _nextEventId;
    rec.pendingTotal = _pendingEventCount;
    rec.segmentCount = static_cast<uint32_t>(_spoolIndex.segments.size());
    rec.fsUsedBytes = static_cast<uint32_t>(LittleFS.usedBytes());
    rec.fsTotalBytes = static_cast<uint32_t>(LittleFS.totalBytes());
    rec.lastFsAuditCompletedMs = _lastFsAuditCompletedMs;
    rec.writtenMs = millis();
    rec.counterTrust = static_cast<uint8_t>(_counterTrustState);
    rec.clean = clean ? 1U : 0U;

    if (!MaintenanceContinuity::save(rec)) {
        return false;
    }

    const uint32_t settledUsedBytes =
        static_cast<uint32_t>(LittleFS.usedBytes());
    if (rec.fsUsedBytes != settledUsedBytes) {
        rec.fsUsedBytes = settledUsedBytes;
        if (!MaintenanceContinuity::save(rec)) {
            return false;
        }
    }

    _maintenanceContinuity = rec;
    _maintenanceContinuityValid = true;
    char completedText[192] = {};
    char remainingText[192] = {};
    strlcpy(completedText, _maintenanceFlagsText(completedFlags), sizeof(completedText));
    strlcpy(remainingText, _maintenanceFlagsText(remainingFlags), sizeof(remainingText));
    DLOG_INFO("STORAGE",
              "Maintenance continuity log wrote clean=%u reason=%s completed=%s remaining=%s gen=%lu",
              clean ? 1U : 0U,
              (reason && reason[0]) ? reason : "-",
              completedText,
              remainingText,
              static_cast<unsigned long>(rec.storageGeneration));
    return true;
}

bool StorageManager::_maintenanceContinuityCurrent() const {
    if (!_maintenanceContinuityValid ||
        !_maintenanceContinuity.clean ||
        _maintenanceContinuity.remainingFlags != STORAGE_MAINT_NONE) {
        return false;
    }

    if (maintenanceFlags() != STORAGE_MAINT_NONE ||
        _workerAppendBatchActive ||
        _uploadBatchActive ||
        _uploadBatchDirty ||
        _workerMetadataDirtyPending ||
        _spoolIndexDirty ||
        _pendingCountDirty ||
        _eventCounterDirty) {
        return false;
    }

    return _maintenanceContinuity.storageGeneration == _storageMetaGeneration &&
           _maintenanceContinuity.spoolGeneration == _spoolIndex.generation &&
           _maintenanceContinuity.eventMetaGeneration == _eventMetaGeneration &&
           _maintenanceContinuity.eventCounterGeneration == _eventCounterGeneration &&
           _maintenanceContinuity.activeSegmentId == _spoolIndex.activeSegmentId &&
           _maintenanceContinuity.nextEventId == _nextEventId &&
           _maintenanceContinuity.pendingTotal == _pendingEventCount &&
           _maintenanceContinuity.segmentCount ==
               static_cast<uint32_t>(_spoolIndex.segments.size()) &&
           _maintenanceContinuity.fsUsedBytes ==
               static_cast<uint32_t>(LittleFS.usedBytes()) &&
           _maintenanceContinuity.fsTotalBytes ==
               static_cast<uint32_t>(LittleFS.totalBytes()) &&
           _counterTrustState == CounterTrust::Trusted;
}

bool StorageManager::_maintenanceOwnsFilesystem(const char* reason) const {
    const bool bootQuiet =
        reason && strcmp(reason, "pre_capture_boot") == 0 &&
        RADIO_ARB.currentOwner() == RADIO_NONE;
    const bool owns = RADIO_ARB.isOwner(RADIO_STORAGE_MAINTENANCE) || bootQuiet;
    CONTRACT_WARN_ONCE(CONTRACT_MAINTENANCE_OWNER_FOR_REPAIR,
                       "STORAGE",
                       owns,
                       "maintenance window without owner reason=%s owner=%s",
                       (reason && reason[0]) ? reason : "-",
                       RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
    return owns;
}

bool StorageManager::hasMaintenanceWork() const {
    return _ready &&
           (maintenanceFlags() != STORAGE_MAINT_NONE ||
            _workerAppendBatchActive);
}

bool StorageManager::isCaptureSafeToResume() const {
    if (!_ready) {
        return true;
    }

    const uint32_t unsafe =
        STORAGE_MAINT_ACTIVE_SEGMENT_INVALID |
        STORAGE_MAINT_COUNTER_UNTRUSTED |
        STORAGE_MAINT_EMERGENCY_REPAIR;
    return (maintenanceFlags() & unsafe) == 0U &&
           _counterTrustState != CounterTrust::RepairRequired &&
           _counterTrustState != CounterTrust::EmergencyOnly;
}

bool StorageManager::needsMaintenanceBeforeCapture() const {
    if (!hasMaintenanceWork()) {
        return false;
    }

    if (_maintenanceHeapRetryAfterMs != 0U &&
        static_cast<int32_t>(millis() - _maintenanceHeapRetryAfterMs) < 0) {
        // Heap-pressure deferrals suppress soft repair gates until retry time.
        // Active-segment damage and emergency repair still preempt the delay.
        const uint32_t forceEvenDuringBackoff =
            STORAGE_MAINT_ACTIVE_SEGMENT_INVALID |
            STORAGE_MAINT_EMERGENCY_REPAIR;
        if ((maintenanceFlags() & forceEvenDuringBackoff) == 0U) {
            return false;
        }
    }

    return hasMaintenanceWork() &&
           (_maintenanceCaptureGate ||
            _activeSegmentNearRotateThreshold() ||
            !isCaptureSafeToResume());
}

bool StorageManager::shouldRunMaintenanceNow() const {
    if (!hasMaintenanceWork()) {
        return false;
    }
    if (_maintenanceHeapRetryAfterMs != 0U &&
        static_cast<int32_t>(millis() - _maintenanceHeapRetryAfterMs) < 0) {
        return false;
    }
    if (needsMaintenanceBeforeCapture()) {
        return true;
    }
    if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        isCaptureSafeToResume()) {
        const uint32_t flags = maintenanceFlags();
        const uint32_t captureSliceMask =
            STORAGE_MAINT_DIRTY_SPOOL_INDEX |
            STORAGE_MAINT_DIRTY_SUMMARY |
            STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST |
            STORAGE_MAINT_SNAPSHOT_LAGGED |
            STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY |
            STORAGE_MAINT_BINARY_CHECKPOINT;
        const bool hasCaptureSliceWork =
            (flags & captureSliceMask) != 0U ||
            _workerMetadataDirtyPending ||
            _eventCounterDirty ||
            _pendingCountDirty ||
            _spoolIndexDirty ||
            _binaryCheckpointDeferred;
        if (!hasCaptureSliceWork) {
            return false;
        }

        const uint32_t now = millis();
        const bool metadataDueByCount =
            _workerMetadataPendingWrites >= STORAGE_EVENT_COUNTER_SAVE_EVERY_N;
        const bool metadataDueByTime =
            _workerMetadataDirtySinceMs != 0 &&
            (now - _workerMetadataDirtySinceMs) >=
                STORAGE_HOT_META_SAVE_INTERVAL_MS;
        const bool uiDue =
            _storageUiRefreshPending && _storageUiRefreshDue();
        const bool checkpointDue =
            _binaryCheckpointDeferred ||
            ((_maintenanceRequestedFlags & STORAGE_MAINT_BINARY_CHECKPOINT) != 0U);

        return metadataDueByCount ||
               metadataDueByTime ||
               uiDue ||
               checkpointDue;
    }
    return true;
}

bool StorageManager::runCaptureMaintenanceSlice(uint32_t budgetMs,
                                                const char* reason) {
    if (!_ready || RADIO_ARB.currentOwner() != RADIO_WIFI_CAPTURE) {
        return false;
    }

    if (!isCaptureSafeToResume() ||
        _workerAppendBatchActive ||
        _uploadBatchActive ||
        _uploadBatchDirty) {
        return false;
    }

    const uint32_t microMask =
        STORAGE_MAINT_DIRTY_SPOOL_INDEX |
        STORAGE_MAINT_DIRTY_SUMMARY |
        STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST |
        STORAGE_MAINT_SNAPSHOT_LAGGED |
        STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY |
        STORAGE_MAINT_BINARY_CHECKPOINT;
    const uint32_t startFlags = maintenanceFlags();
    const bool uiRefreshDue =
        _storageUiRefreshPending && _storageUiRefreshDue();
    const uint32_t now = millis();
    const bool metadataDueByCount =
        _workerMetadataPendingWrites >= STORAGE_EVENT_COUNTER_SAVE_EVERY_N;
    const bool metadataDueByTime =
        _workerMetadataDirtySinceMs != 0 &&
        (now - _workerMetadataDirtySinceMs) >=
            STORAGE_HOT_META_SAVE_INTERVAL_MS;
    const bool metadataFlushDue =
        metadataDueByCount ||
        metadataDueByTime ||
        _maintenanceCaptureGate ||
        ((_maintenanceRequestedFlags &
          (STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST |
           STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY)) != 0U);
    const bool hasMicroWork =
        (startFlags & microMask) != 0U ||
        _workerMetadataDirtyPending ||
        _eventCounterDirty ||
        _pendingCountDirty ||
        _spoolIndexDirty ||
        _binaryCheckpointDeferred ||
        uiRefreshDue;
    if (!hasMicroWork) {
        return false;
    }

    if (RAMSpool::isReady()) {
        const RAMSpool::Stats stats = RAMSpool::snapshot();
        if (stats.inflight != 0 || stats.pressureState != 0) {
            return false;
        }
        if (!RAMSpool::tryPauseWorkerIfIdle(0)) {
            return false;
        }
    }

    const uint32_t startMs = millis();
    if (budgetMs == 0U) {
        budgetMs = 1U;
    }

    const char* sliceReason =
        (reason && reason[0]) ? reason : "capture_micro_maintenance";
    uint32_t actions = STORAGE_MAINT_NONE;
    uint32_t skipped = STORAGE_MAINT_NONE;

    auto finish = [&](bool progressed) -> bool {
        if (RAMSpool::isReady() && RAMSpool::isWorkerPaused()) {
            RAMSpool::resumeWorker(false);
        }

        if (progressed) {
            _maintenanceHeapRetryAfterMs = 0;
        }

        char actionsText[192] = {};
        char skippedText[192] = {};
        char remainingText[192] = {};
        strlcpy(actionsText, _maintenanceFlagsText(actions), sizeof(actionsText));
        strlcpy(skippedText, _maintenanceFlagsText(skipped), sizeof(skippedText));
        strlcpy(remainingText,
                _maintenanceFlagsText(maintenanceFlags()),
                sizeof(remainingText));
        DLOG_DEBUG("STORAGE",
                   "Capture maintenance slice reason=%s actions=%s skipped=%s remaining=%s elapsedMs=%lu",
                   sliceReason,
                   actionsText,
                   skippedText,
                   remainingText,
                   static_cast<unsigned long>(millis() - startMs));
        return progressed;
    };

    auto budgetLeft = [&]() -> bool {
        return (millis() - startMs) < budgetMs;
    };

    if (_workerAppendBatchActive) {
        skipped |= microMask & maintenanceFlags();
        return finish(false);
    }

    if (budgetLeft() && metadataFlushDue &&
        (_workerMetadataDirtyPending ||
         _eventCounterDirty ||
         _pendingCountDirty ||
         _spoolIndexDirty ||
         (_maintenanceRequestedFlags & (STORAGE_MAINT_DIRTY_SPOOL_INDEX |
                                        STORAGE_MAINT_SNAPSHOT_LAGGED |
                                        STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST |
                                        STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY)))) {
        // flushWorkerMetadataBatch() does two independently expensive things
        // back to back: it flushes the append file, then persists the counter
        // and metadata.  Each is a LittleFS write costing 300-900 ms, and
        // neither is budget-checked internally, so together they overran this
        // slice's budget by ~11x (measured 1391 ms worst case) and stalled
        // TaskHardware for over a second.
        //
        // Run the append flush as its own budgeted step and re-check before
        // committing to the metadata write.  The append flush inside the
        // batch call then costs only a seek (header already clean), so a slice
        // now pays at most one of the two latencies instead of both.  A
        // deferred metadata write simply lands on the next slice, which is how
        // this work is already scheduled — no durability change.
        _flushWorkerAppendFile(sliceReason, false);

        if (!budgetLeft()) {
            skipped |= STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST |
                       STORAGE_MAINT_SNAPSHOT_LAGGED |
                       STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY |
                       STORAGE_MAINT_DIRTY_SPOOL_INDEX;
            return finish(actions != STORAGE_MAINT_NONE ||
                          maintenanceFlags() != startFlags);
        }

        const bool ok = flushWorkerMetadataBatch(sliceReason, true, true);
        if (ok || !_workerMetadataDirtyPending) {
            if (!_spoolIndexDirty && !_pendingCountDirty && !_eventCounterDirty) {
                _uploadBatchDirty = false;
            }
            actions |= STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST |
                       STORAGE_MAINT_SNAPSHOT_LAGGED |
                       STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY;
            if (!_spoolIndexDirty) {
                actions |= STORAGE_MAINT_DIRTY_SPOOL_INDEX;
            }
            _clearMaintenanceFlags(STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST |
                                   STORAGE_MAINT_SNAPSHOT_LAGGED |
                                   STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY |
                                   STORAGE_MAINT_DIRTY_SPOOL_INDEX);
        } else {
            skipped |= STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST |
                       STORAGE_MAINT_SNAPSHOT_LAGGED |
                       STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY |
                       STORAGE_MAINT_DIRTY_SPOOL_INDEX;
        }
    }

    if (budgetLeft() &&
        (_binaryCheckpointDeferred ||
         (_maintenanceRequestedFlags & STORAGE_MAINT_BINARY_CHECKPOINT))) {
        if (_serviceDeferredBinaryCheckpoint(sliceReason, true)) {
            actions |= STORAGE_MAINT_BINARY_CHECKPOINT;
            _clearMaintenanceFlags(STORAGE_MAINT_BINARY_CHECKPOINT);
        } else {
            skipped |= STORAGE_MAINT_BINARY_CHECKPOINT;
        }
    }

    if (budgetLeft() && uiRefreshDue) {
        refreshStorageUiState(false, false);
        actions |= STORAGE_MAINT_DIRTY_SUMMARY;
    }

    if (budgetLeft() &&
        (_maintenanceRequestedFlags & STORAGE_MAINT_DIRTY_SUMMARY)) {
        if (!_spoolSummaryRebuildPending && !_hasInvalidSpoolSummaries()) {
            actions |= STORAGE_MAINT_DIRTY_SUMMARY;
            _clearMaintenanceFlags(STORAGE_MAINT_DIRTY_SUMMARY);
        } else {
            skipped |= STORAGE_MAINT_DIRTY_SUMMARY;
        }
    }

    const bool progressed =
        actions != STORAGE_MAINT_NONE || maintenanceFlags() != startFlags;
    return finish(progressed);
}

bool StorageManager::runMaintenanceWindow(uint32_t budgetMs,
                                          const char* reason) {
    if (!_ready) {
        return false;
    }

    if (budgetMs == 0U) {
        budgetMs = 1U;
    }

    const uint32_t startMs = millis();
    const char* windowReason = (reason && reason[0]) ? reason : "maintenance";
    if (!_maintenanceOwnsFilesystem(windowReason)) {
        return false;
    }

    _releaseUploadIndexMemory("maintenance_begin");
    releaseEnrichmentIndexMemory("maintenance_begin");

    const bool captureSafeAtStart = isCaptureSafeToResume();
    const bool preCaptureMetadataOnly =
        strcmp(windowReason, "pre_capture_boot") == 0 && captureSafeAtStart;
    uint32_t startFlags = maintenanceFlags();
    uint32_t actions = STORAGE_MAINT_NONE;
    uint32_t skipped = STORAGE_MAINT_NONE;
    bool heapGuardBlocked = false;
    char startFlagsText[192] = {};
    strlcpy(startFlagsText, _maintenanceFlagsText(startFlags), sizeof(startFlagsText));

    DLOG_INFO("STORAGE",
              "Maintenance start reason=%s budgetMs=%lu flags=%s captureSafe=%d",
              windowReason,
              static_cast<unsigned long>(budgetMs),
              startFlagsText,
              captureSafeAtStart ? 1 : 0);

    if (!captureSafeAtStart &&
        !preCaptureMetadataOnly &&
        (startFlags & STORAGE_MAINT_DELETE_DRAINED) != 0U) {
        const bool ok = compactSpool();
        if (ok) {
            actions |= STORAGE_MAINT_DELETE_DRAINED;
            _clearMaintenanceFlags(STORAGE_MAINT_DELETE_DRAINED);
            startFlags = maintenanceFlags();
            strlcpy(startFlagsText,
                    _maintenanceFlagsText(startFlags),
                    sizeof(startFlagsText));
            DLOG_INFO("STORAGE",
                      "Maintenance pre-repair delete_drained complete remaining=%s",
                      startFlagsText);
        } else {
            skipped |= STORAGE_MAINT_DELETE_DRAINED;
            DLOG_WARN("STORAGE",
                      "Maintenance pre-repair delete_drained failed flags=%s",
                      _maintenanceFlagsText(maintenanceFlags()));
        }
    }

    if (startFlags == STORAGE_MAINT_NONE && actions != STORAGE_MAINT_NONE) {
        _maintenanceHeapRetryAfterMs = 0;
        char actionsText[192] = {};
        char skippedText[192] = {};
        strlcpy(actionsText, _maintenanceFlagsText(actions), sizeof(actionsText));
        strlcpy(skippedText, _maintenanceFlagsText(skipped), sizeof(skippedText));
        DLOG_INFO("STORAGE",
                  "Maintenance done reason=%s actions=%s skipped=%s remaining=none elapsedMs=%lu captureSafe=%d",
                  windowReason,
                  actionsText,
                  skippedText,
                  static_cast<unsigned long>(millis() - startMs),
                  captureSafeAtStart ? 1 : 0);
        return true;
    }

    auto budgetLeft = [&]() -> bool {
        return (millis() - startMs) < budgetMs;
    };
    auto heapAllows = [&](uint32_t minFreeInternal,
                          uint32_t minLargestInternal,
                          const char* action) -> bool {
        const uint32_t freeInternal =
            heap_caps_get_free_size(SPECTRE_CAP_DRAM);
        const uint32_t largestInternal =
            heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM);
        if (freeInternal >= minFreeInternal &&
            largestInternal >= minLargestInternal) {
            return true;
        }

        heapGuardBlocked = true;
        DLOG_WARN("STORAGE",
                  "Maintenance skip action=%s reason=heap_guard freeInternal=%lu largestInternal=%lu minFree=%lu minLargest=%lu flags=%s",
                  action ? action : "?",
                  static_cast<unsigned long>(freeInternal),
                  static_cast<unsigned long>(largestInternal),
                  static_cast<unsigned long>(minFreeInternal),
                  static_cast<unsigned long>(minLargestInternal),
                  _maintenanceFlagsText(maintenanceFlags()));
        return false;
    };

    if (!budgetLeft()) {
        skipped |= startFlags;
    }

    if (budgetLeft() &&
        (_workerMetadataDirtyPending || _eventCounterDirty ||
         _pendingCountDirty || _spoolIndexDirty ||
         (_maintenanceRequestedFlags & (STORAGE_MAINT_DIRTY_SPOOL_INDEX |
                                        STORAGE_MAINT_SNAPSHOT_LAGGED |
                                        STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST |
                                        STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY)))) {
        const bool ok = flushWorkerMetadataBatch(windowReason, true);
        if (ok || !_workerMetadataDirtyPending) {
            if (!_spoolIndexDirty && !_pendingCountDirty && !_eventCounterDirty) {
                _uploadBatchDirty = false;
            }
            actions |= STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST |
                       STORAGE_MAINT_SNAPSHOT_LAGGED |
                       STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY;
            if (!_spoolIndexDirty) {
                actions |= STORAGE_MAINT_DIRTY_SPOOL_INDEX;
            }
            _clearMaintenanceFlags(STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST |
                                   STORAGE_MAINT_SNAPSHOT_LAGGED |
                                   STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY |
                                   STORAGE_MAINT_DIRTY_SPOOL_INDEX);
        } else {
            skipped |= STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST |
                       STORAGE_MAINT_SNAPSHOT_LAGGED |
                       STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY |
                       STORAGE_MAINT_DIRTY_SPOOL_INDEX;
        }
    }

    if (budgetLeft() &&
        (_binaryCheckpointDeferred ||
         (_maintenanceRequestedFlags & STORAGE_MAINT_BINARY_CHECKPOINT))) {
        if (_serviceDeferredBinaryCheckpoint(windowReason)) {
            actions |= STORAGE_MAINT_BINARY_CHECKPOINT;
            _clearMaintenanceFlags(STORAGE_MAINT_BINARY_CHECKPOINT);
        } else {
            skipped |= STORAGE_MAINT_BINARY_CHECKPOINT;
        }
    }

    if (budgetLeft() && _activeSegmentNearRotateThreshold()) {
        if (_preRotateActiveSegmentIfNearFull(windowReason)) {
            actions |= STORAGE_MAINT_ACTIVE_SEGMENT_NEAR_FULL;
            _clearMaintenanceFlags(STORAGE_MAINT_ACTIVE_SEGMENT_NEAR_FULL);
        } else {
            skipped |= STORAGE_MAINT_ACTIVE_SEGMENT_NEAR_FULL;
        }
    }

    if (budgetLeft() &&
        (_storageUiRefreshPending ||
         _spoolSummaryRebuildPending ||
         _hasInvalidSpoolSummaries() ||
         (_maintenanceRequestedFlags & STORAGE_MAINT_DIRTY_SUMMARY))) {
        bool ok = true;
        if (preCaptureMetadataOnly) {
            skipped |= STORAGE_MAINT_DIRTY_SUMMARY;
            DLOG_INFO("STORAGE",
                      "Maintenance skip action=summary_rebuild reason=pre_capture_metadata_only flags=%s",
                      _maintenanceFlagsText(maintenanceFlags()));
        } else {
            if (_storageUiRefreshPending) {
                if (heapAllows(STORAGE_MAINT_UI_MIN_FREE_INTERNAL,
                               STORAGE_MAINT_UI_MIN_LARGEST_BLOCK,
                               "summary_ui_refresh")) {
                    refreshStorageUiState(false, true);
                } else {
                    ok = false;
                }
            }
            if (ok && (_spoolSummaryRebuildPending || _hasInvalidSpoolSummaries())) {
                if (!heapAllows(STORAGE_MAINT_SUMMARY_MIN_FREE_INTERNAL,
                                STORAGE_MAINT_SUMMARY_MIN_LARGEST_BLOCK,
                                "summary_rebuild")) {
                    skipped |= STORAGE_MAINT_DIRTY_SUMMARY;
                } else {
                    ok = _servicePendingSpoolSummaryRebuild();
                }
            }
            if (ok && !_spoolSummaryRebuildPending && !_hasInvalidSpoolSummaries()) {
                actions |= STORAGE_MAINT_DIRTY_SUMMARY;
                _clearMaintenanceFlags(STORAGE_MAINT_DIRTY_SUMMARY);
            } else {
                skipped |= STORAGE_MAINT_DIRTY_SUMMARY;
            }
        }
    }

    // Quiet-period UTC backfill: if the clock became trusted after this boot's
    // segments were created (e.g. phone connected while idle), stamp their
    // epoch bases now so their records become enrichable. Header-only.
    if (budgetLeft()) {
        _backfillThisBootSegmentEpochs();
    }

    if (budgetLeft() &&
        (_maintenanceRequestedFlags & STORAGE_MAINT_ACTIVE_SEGMENT_INVALID)) {
        const bool resyncChanged = _resyncSpoolIndexFromFilesystem();
        if (_spoolIndex.activeSegmentId == 0 ||
            !_findSegmentInfo(_spoolIndex.activeSegmentId)) {
            if (!_openNewSpoolSegment()) {
                skipped |= STORAGE_MAINT_ACTIVE_SEGMENT_INVALID;
            }
        }
        if (resyncChanged || _spoolIndexDirty) {
            (void)_persistSpoolIndex(true, "maintenance_reconcile");
        }
        if (_spoolIndex.activeSegmentId != 0 &&
            _findSegmentInfo(_spoolIndex.activeSegmentId) != nullptr) {
            actions |= STORAGE_MAINT_ACTIVE_SEGMENT_INVALID;
            _clearMaintenanceFlags(STORAGE_MAINT_ACTIVE_SEGMENT_INVALID);
        }
    }

    if (budgetLeft() &&
        (hasSpoolRepairWork() ||
         (_maintenanceRequestedFlags & STORAGE_MAINT_SEGMENT_AUDIT))) {
        if (preCaptureMetadataOnly) {
            skipped |= STORAGE_MAINT_SEGMENT_AUDIT;
            DLOG_INFO("STORAGE",
                      "Maintenance skip action=bounded_repair reason=pre_capture_metadata_only flags=%s",
                      _maintenanceFlagsText(maintenanceFlags()));
        } else {
            if (!hasSpoolRepairWork()) {
                requestSpoolRepair("maintenance_segment_audit");
            }
            const SpoolRepairMode mode = _selectRepairMode();
            uint32_t repairBudgetMs = budgetMs - (millis() - startMs);
            uint32_t repairMaxRecords = UINT32_MAX;
            if (mode == REPAIR_BACKGROUND || mode == REPAIR_FAST_IDLE) {
                uint32_t modeBudgetMs = repairBudgetMs;
                uint32_t modeMaxRecords = repairMaxRecords;
                repairBudgetsForMode(mode, modeBudgetMs, modeMaxRecords);
                repairBudgetMs = std::min<uint32_t>(repairBudgetMs, modeBudgetMs);
                repairMaxRecords = modeMaxRecords;
            }
            repairBudgetMs = std::min<uint32_t>(
                repairBudgetMs,
                budgetMs - (millis() - startMs));
            if (!heapAllows(STORAGE_MAINT_REPAIR_MIN_FREE_INTERNAL,
                            STORAGE_MAINT_REPAIR_MIN_LARGEST_BLOCK,
                            "bounded_repair")) {
                skipped |= STORAGE_MAINT_SEGMENT_AUDIT;
            } else {
                const bool done = repairStep(mode, repairBudgetMs, repairMaxRecords);
                actions |= STORAGE_MAINT_SEGMENT_AUDIT;
                if (done && !hasSpoolRepairWork()) {
                    _clearMaintenanceFlags(STORAGE_MAINT_SEGMENT_AUDIT |
                                           STORAGE_MAINT_COUNTER_UNTRUSTED);
                } else {
                    skipped |= STORAGE_MAINT_SEGMENT_AUDIT;
                }
            }
        }
    }

    if (budgetLeft() &&
        (_counterTrustState == CounterTrust::Degraded ||
         (_maintenanceRequestedFlags & STORAGE_MAINT_COUNTER_UNTRUSTED))) {
        if (preCaptureMetadataOnly) {
            skipped |= STORAGE_MAINT_COUNTER_UNTRUSTED;
            DLOG_INFO("STORAGE",
                      "Maintenance skip action=counter_recount reason=pre_capture_metadata_only flags=%s",
                      _maintenanceFlagsText(maintenanceFlags()));
        } else if (hasSpoolRepairWork()) {
            skipped |= STORAGE_MAINT_COUNTER_UNTRUSTED;
            DLOG_INFO("STORAGE",
                      "Maintenance skip action=counter_recount reason=repair_in_progress flags=%s",
                      _maintenanceFlagsText(maintenanceFlags()));
        } else {
            if (!heapAllows(STORAGE_MAINT_RECOUNT_MIN_FREE_INTERNAL,
                            STORAGE_MAINT_RECOUNT_MIN_LARGEST_BLOCK,
                            "counter_recount")) {
                skipped |= STORAGE_MAINT_COUNTER_UNTRUSTED;
            } else {
                const uint32_t pending = recountPendingFromSpool();
                (void)pending;
                actions |= STORAGE_MAINT_COUNTER_UNTRUSTED;
                if (isPendingEventCountAuthoritative()) {
                    _clearMaintenanceFlags(STORAGE_MAINT_COUNTER_UNTRUSTED);
                } else {
                    skipped |= STORAGE_MAINT_COUNTER_UNTRUSTED;
                }
            }
        }
    }

    const bool fullRebuildAllowed =
        !_maintenanceFullRebuildAttempted &&
        _counterTrustState == CounterTrust::EmergencyOnly &&
        !isCaptureSafeToResume();
    if (budgetLeft() && fullRebuildAllowed) {
        if (preCaptureMetadataOnly) {
            skipped |= STORAGE_MAINT_EMERGENCY_REPAIR;
            DLOG_INFO("STORAGE",
                      "Maintenance skip action=full_rebuild reason=pre_capture_metadata_only flags=%s",
                      _maintenanceFlagsText(maintenanceFlags()));
        } else {
            if (!heapAllows(STORAGE_MAINT_REBUILD_MIN_FREE_INTERNAL,
                            STORAGE_MAINT_REBUILD_MIN_LARGEST_BLOCK,
                            "full_rebuild")) {
                skipped |= STORAGE_MAINT_EMERGENCY_REPAIR;
            } else {
                _maintenanceFullRebuildAttempted = true;
                SpoolAuditResult audit;
                const bool ok = _auditAndRepairSpool("maintenance_full_rebuild",
                                                     true,
                                                     &audit);
                actions |= STORAGE_MAINT_EMERGENCY_REPAIR;
                if (ok) {
                    _setCounterTrustState(STORAGE_COUNTER_TRUSTED,
                                          "maintenance_full_rebuild");
                    _clearMaintenanceFlags(STORAGE_MAINT_EMERGENCY_REPAIR |
                                           STORAGE_MAINT_COUNTER_UNTRUSTED |
                                           STORAGE_MAINT_ACTIVE_SEGMENT_INVALID);
                } else {
                    skipped |= STORAGE_MAINT_EMERGENCY_REPAIR;
                }
            }
        }
    }

    if (budgetLeft() &&
        (_maintenanceRequestedFlags & STORAGE_MAINT_DELETE_DRAINED)) {
        if (preCaptureMetadataOnly) {
            skipped |= STORAGE_MAINT_DELETE_DRAINED;
            DLOG_INFO("STORAGE",
                      "Maintenance skip action=delete_drained reason=pre_capture_metadata_only flags=%s",
                      _maintenanceFlagsText(maintenanceFlags()));
        } else {
            const bool ok = compactSpool();
            if (ok) {
                actions |= STORAGE_MAINT_DELETE_DRAINED;
                _clearMaintenanceFlags(STORAGE_MAINT_DELETE_DRAINED);
            } else {
                skipped |= STORAGE_MAINT_DELETE_DRAINED;
            }
        }
    }

    if (budgetLeft() && (_spoolIndexDirty || _workerMetadataDirtyPending)) {
        if (flushWorkerMetadataBatch(windowReason, true)) {
            actions |= STORAGE_MAINT_DIRTY_SPOOL_INDEX;
            _clearMaintenanceFlags(STORAGE_MAINT_DIRTY_SPOOL_INDEX);
        } else {
            skipped |= STORAGE_MAINT_DIRTY_SPOOL_INDEX;
        }
    }

    // FS-wide audit. Runs last so spool/summary repair work always wins the
    // budget when both are pending. Never run during pre_capture_metadata_only
    // — sweeping the FS is exactly the kind of work that would surprise the
    // radio path with disk reads. Phase 2: actually deletes tmp orphans and
    // quarantines header-invalid files (bounded), and requests a spool index
    // rebuild when /spool/index.json is broken.
    if (budgetLeft() &&
        (_maintenanceRequestedFlags & STORAGE_MAINT_FS_AUDIT)) {
        if (preCaptureMetadataOnly) {
            skipped |= STORAGE_MAINT_FS_AUDIT;
            DLOG_INFO("STORAGE",
                      "Maintenance skip action=fs_audit reason=pre_capture_metadata_only flags=%s",
                      _maintenanceFlagsText(maintenanceFlags()));
        } else if (!heapAllows(STORAGE_MAINT_FS_AUDIT_MIN_FREE_INTERNAL,
                               STORAGE_MAINT_FS_AUDIT_MIN_LARGEST_BLOCK,
                               "fs_audit")) {
            skipped |= STORAGE_MAINT_FS_AUDIT;
        } else {
            // Pick the FsAudit ladder rung from the same global pressure
            // signal that drives spool repair. Background → minimal slice
            // when capture/upload/heap is under load; emergency → full sweep
            // when the system is in recovery.
            const SpoolRepairMode rung = _selectRepairMode();
            FsAudit::FsAuditMode auditMode = FsAudit::FS_AUDIT_NORMAL;
            if (rung == REPAIR_BACKGROUND) {
                auditMode = FsAudit::FS_AUDIT_BACKGROUND;
            } else if (rung == REPAIR_EMERGENCY) {
                auditMode = FsAudit::FS_AUDIT_EMERGENCY;
            }
            FsAudit::FsAuditLimits limits = FsAudit::limitsForMode(auditMode);
            // Never let the audit consume more than the maintenance window
            // has left; the per-mode budget is an upper bound, not a floor.
            const uint32_t elapsed = millis() - startMs;
            const uint32_t remaining =
                (elapsed >= budgetMs) ? 0U : (budgetMs - elapsed);
            limits.budgetMs = std::min<uint32_t>(limits.budgetMs, remaining);

            // Floor: if we have less than 50ms of window left, skip — opening
            // the FS root and tearing down for a few entries is wasted work.
            // The flag stays set so the next maintenance window picks it up.
            constexpr uint32_t kMinAuditBudgetMs = 50UL;
            if (limits.budgetMs < kMinAuditBudgetMs) {
                skipped |= STORAGE_MAINT_FS_AUDIT;
                DLOG_INFO("STORAGE",
                          "fs_audit defer reason=window_too_small remainingMs=%lu",
                          static_cast<unsigned long>(remaining));
            } else {
                FsAudit::FsAuditReport report{};
                const bool complete = FsAudit::runWindow(limits, report);
                actions |= STORAGE_MAINT_FS_AUDIT;

                // Cross-reference: walker reports the spool segment files it
                // saw on disk; we compare against the in-memory index. Drift
                // (file count or active id) → request a rebuild so the
                // existing reconcile path runs in the next window.
                bool spoolDrift = false;
                if (complete) {
                    const uint16_t indexedSegs =
                        static_cast<uint16_t>(_spoolIndex.segments.size());
                    if (report.spoolSegmentFilesSeen != indexedSegs) {
                        spoolDrift = true;
                    }
                    if (_spoolIndex.activeSegmentId != 0 &&
                        report.spoolMaxSegmentIdSeen <
                            _spoolIndex.activeSegmentId) {
                        spoolDrift = true;
                    }
                    if (spoolDrift) {
                        DLOG_WARN("STORAGE",
                                  "fs_audit spool drift seenFiles=%u indexedSegs=%u "
                                  "seenMaxId=%lu activeId=%lu",
                                  report.spoolSegmentFilesSeen,
                                  static_cast<unsigned>(indexedSegs),
                                  static_cast<unsigned long>(
                                      report.spoolMaxSegmentIdSeen),
                                  static_cast<unsigned long>(
                                      _spoolIndex.activeSegmentId));
                        requestMaintenance(STORAGE_MAINT_DIRTY_SPOOL_INDEX,
                                           "fs_audit_spool_drift");
                    }
                }

                DLOG_INFO("STORAGE",
                          "fs_audit done mode=%u complete=%d files=%u invalid=%u tmp=%u "
                          "missing=%u del=%u quar=%u rebuild=%u salv=%u evict=%u "
                          "skip=%u fail=%u drift=%d ms=%lu",
                          static_cast<unsigned>(auditMode),
                          complete ? 1 : 0,
                          report.totalFiles, report.knownInvalid, report.tmpOrphan,
                          report.missingEssential,
                          report.deletedTmp, report.quarantined, report.rebuildRequests,
                          report.salvaged, report.evicted,
                          report.actionsSkipped, report.actionsFailed,
                          spoolDrift ? 1 : 0,
                          static_cast<unsigned long>(report.durationMs));
                if (complete) {
                    _clearMaintenanceFlags(STORAGE_MAINT_FS_AUDIT);
                    _lastFsAuditCompletedMs = millis();
                } else {
                    skipped |= STORAGE_MAINT_FS_AUDIT;
                }
            }
        }
    }

    bool continuityDirsOk = true;
    if (budgetLeft()) {
        continuityDirsOk &= _ensureDir(PATH_STORE_CONFIG_DIR);
        continuityDirsOk &= _ensureDir(PATH_STORE_VAULT_DIR);
        continuityDirsOk &= _ensureDir(PATH_FIELDVAULT_DIR);
        continuityDirsOk &= _ensureDir(PATH_LOGS);
        continuityDirsOk &= _ensureDir(PATH_EVENTS);
        continuityDirsOk &= _ensureDir(PATH_SPOOL);
        continuityDirsOk &= _ensureDir(PATH_EXPORTS);
        continuityDirsOk &= _ensureDir(PATH_PMKID_DIR);
        continuityDirsOk &= _ensureDir(PATH_SPOOL_BAD);
        continuityDirsOk &= _ensureDir(PATH_SPOOL_BAD_LOGS);
        continuityDirsOk &= _ensureDir(PATH_SPOOL_BAD_META);
        if (!continuityDirsOk) {
            requestMaintenance(STORAGE_MAINT_FS_AUDIT,
                               "continuity_dirs_failed");
        }
    }

    const uint32_t preContinuityFlags = maintenanceFlags();
    const uint32_t nonContinuityFlags =
        preContinuityFlags & ~static_cast<uint32_t>(STORAGE_MAINT_CONTINUITY_PASS);
    const bool continuityClean =
        continuityDirsOk &&
        nonContinuityFlags == STORAGE_MAINT_NONE &&
        !_workerAppendBatchActive &&
        !_uploadBatchActive &&
        !_uploadBatchDirty &&
        !_workerMetadataDirtyPending &&
        !_spoolIndexDirty &&
        !_pendingCountDirty &&
        !_eventCounterDirty &&
        isCaptureSafeToResume() &&
        _counterTrustState == CounterTrust::Trusted;
    if (budgetLeft() && continuityClean && !_maintenanceContinuityCurrent()) {
        if (_writeMaintenanceContinuityLog(actions,
                                           STORAGE_MAINT_NONE,
                                           true,
                                           windowReason)) {
            actions |= STORAGE_MAINT_CONTINUITY_PASS;
            _clearMaintenanceFlags(STORAGE_MAINT_CONTINUITY_PASS);
        } else {
            skipped |= STORAGE_MAINT_CONTINUITY_PASS;
            _maintenanceContinuityValid = false;
            requestMaintenance(STORAGE_MAINT_CONTINUITY_PASS,
                               "continuity_log_failed");
        }
    } else if ((_maintenanceRequestedFlags & STORAGE_MAINT_CONTINUITY_PASS) != 0U) {
        skipped |= STORAGE_MAINT_CONTINUITY_PASS;
    }

    const uint32_t remainingFlags = maintenanceFlags();
    const bool captureSafe = isCaptureSafeToResume();
    // Keep capture blocked while storage is unsafe, but do not spin forever on
    // capture-safe maintenance that could not make progress in this window.
    // Examples such as dirty_summary can remain queued until a quieter pass
    // without preventing WIFI_CAPTURE from starting.
    const bool maintenanceDrained = !hasMaintenanceWork();
    const bool madeProgress =
        actions != STORAGE_MAINT_NONE || remainingFlags != startFlags;
    const uint32_t heapGuardBlockedFlags =
        skipped & (STORAGE_MAINT_DIRTY_SUMMARY |
                   STORAGE_MAINT_SEGMENT_AUDIT |
                   STORAGE_MAINT_COUNTER_UNTRUSTED |
                   STORAGE_MAINT_EMERGENCY_REPAIR);
    const bool heapGuardDeferredWork =
        heapGuardBlocked &&
        heapGuardBlockedFlags != 0U &&
        (remainingFlags & heapGuardBlockedFlags) != 0U;
    if (captureSafe && (maintenanceDrained || !madeProgress)) {
        _maintenanceCaptureGate = false;
    }
    if (captureSafe && heapGuardDeferredWork) {
        _maintenanceCaptureGate = false;
    }
    if (heapGuardDeferredWork && remainingFlags != STORAGE_MAINT_NONE) {
        _maintenanceHeapRetryAfterMs = millis() + STORAGE_MAINT_HEAP_RETRY_MS;
        DLOG_WARN("STORAGE",
                  "maintenance deferred reason=heap_guard backoffMs=%lu remaining=%s",
                  static_cast<unsigned long>(STORAGE_MAINT_HEAP_RETRY_MS),
                  _maintenanceFlagsText(remainingFlags));
    } else if (!captureSafe || maintenanceDrained || madeProgress) {
        _maintenanceHeapRetryAfterMs = 0;
    }
    char actionsText[192] = {};
    char skippedText[192] = {};
    char remainingText[192] = {};
    strlcpy(actionsText, _maintenanceFlagsText(actions), sizeof(actionsText));
    strlcpy(skippedText, _maintenanceFlagsText(skipped), sizeof(skippedText));
    strlcpy(remainingText, _maintenanceFlagsText(remainingFlags), sizeof(remainingText));

    DLOG_INFO("STORAGE",
              "Maintenance done reason=%s actions=%s skipped=%s remaining=%s elapsedMs=%lu captureSafe=%d",
              windowReason,
              actionsText,
              skippedText,
              remainingText,
              static_cast<unsigned long>(millis() - startMs),
              captureSafe ? 1 : 0);

    return captureSafe;
}

bool StorageManager::serviceStorageMaintenanceStep(uint32_t budgetMs,
                                                   uint32_t maxRecords) {
    if (!_ready) {
        return false;
    }

    if (_storageUiRefreshPending) {
        refreshStorageUiState(false, false);
        return true;
    }

    if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY) {
        return true;
    }

    if (hasSpoolRepairWork()) {
        const SpoolRepairMode mode = _selectRepairMode();
        repairBudgetsForMode(mode, budgetMs, maxRecords);
        return repairStep(mode, budgetMs, maxRecords);
    }

    if (_spoolSummaryRebuildPending || _hasInvalidSpoolSummaries()) {
        _spoolSummaryRebuildPending = true;
        _servicePendingSpoolSummaryRebuild();
        return true;
    }

    if (_workerMetadataDirtyPending) {
        flushWorkerMetadataBatch("maintenance", false);
        return true;
    }

    if (_pendingCountDirty) {
        flushWorkerMetadataBatch("maintenance_counter", false);
        return true;
    }

    (void)budgetMs;
    (void)maxRecords;
    return true;
}

bool StorageManager::repairStep(SpoolRepairMode mode,
                                uint32_t budgetMs,
                                uint32_t maxRecords) {
    if (!_ready) {
        return false;
    }

    if (!_repairRequested && !_spoolAuditRepairRequired && !_repairJob.active) {
        return true;
    }

    if (budgetMs == 0) {
        budgetMs = 1;
    }
    if (maxRecords == 0) {
        maxRecords = 1;
    }

    if (!_repairJob.active) {
        _startRepairJob(_repairJob.reason.length()
                            ? _repairJob.reason.c_str()
                            : "maintenance_repair");
    }

    const uint32_t startMs = millis();
    uint32_t recordsScanned = 0;

    while (recordsScanned < maxRecords &&
           (millis() - startMs) < budgetMs) {
        const size_t segmentIndexBefore = _repairJob.segmentIndex;
        if (!_repairJob.scanningSegment && !_beginRepairSegment()) {
            return _finalizeRepairJob();
        }

        const bool ok =
            (_repairJob.originalSegment.format == SPOOL_SEGMENT_BIN_V2)
                ? _repairBinaryMetaSlice(startMs, budgetMs, maxRecords, recordsScanned)
                : _repairJsonlSlice(startMs, budgetMs, maxRecords, recordsScanned);

        if (!ok) {
            _repairJob.audit.unreadableSegments++;
            _repairJob.audit.hadFatalSegmentError = true;

            if (!_quarantineSpoolSegment(_repairJob.originalSegment.segmentId,
                                         SpoolCorruptionReason::SCAN_FAILED,
                                         "maintenance repair scan failed")) {
                _repairRequested = true;
                _spoolAuditRepairRequired = true;
                return false;
            }

            _repairJob.audit.quarantinedSegments++;
            _repairJob.segmentChanged = true;
            _repairJob.scanningSegment = false;
            _repairJob.segmentIndex++;
            continue;
        }

        if (!_repairJob.scanningSegment) {
            _finishRepairSegment();
            delay(1);
        }

        if (recordsScanned == 0 &&
            _repairJob.segmentIndex == segmentIndexBefore &&
            _repairJob.scanningSegment) {
            break;
        }
    }

    (void)mode;
    return !_repairJob.active;
}

bool StorageManager::_canRebuildSpoolSummariesNow() const {
    if (!_ready || _uploadBatchActive) {
        return false;
    }

    const RadioOwner owner = RADIO_ARB.currentOwner();
    return owner == RADIO_NONE || owner == RADIO_STORAGE_MAINTENANCE;
}

bool StorageManager::_servicePendingSpoolSummaryRebuild() {
    if (!_ready) {
        return false;
    }

    if (!_spoolSummaryRebuildPending) {
        _spoolSummaryRebuildPending = _hasInvalidSpoolSummaries();
    }

    if (!_spoolSummaryRebuildPending) {
        return false;
    }

    if (!_canRebuildSpoolSummariesNow()) {
        _spoolSummaryRebuildPending = true;
        return false;
    }

    return _rebuildInvalidSegmentSummaries(false);
}

void StorageManager::_publishStorageEventIfNeeded(StoragePressureMode oldMode,
                                                  StoragePressureMode newMode) {
    if (oldMode == newMode) return;

    if (newMode >= STORAGE_MODE_WATCH) {
        BUS.publish(EVT_STORAGE_NEARLY_FULL);
    }
    if (newMode >= STORAGE_MODE_FULL) {
        BUS.publish(EVT_STORAGE_FULL);
    }
}

void StorageManager::_maybeCompactForPressure(StoragePressureMode oldMode,
                                              StoragePressureMode newMode) {
    if (!_ready) return;

    if (oldMode < STORAGE_MODE_WATCH && newMode >= STORAGE_MODE_WATCH) {
        requestMaintenance(STORAGE_MAINT_DELETE_DRAINED,
                           "storage_watch_reclaim");
    }

    // Only react when pressure rises into FULL/OVERRUN or worsens.
    const bool enteredFull =
        (oldMode < STORAGE_MODE_FULL && newMode >= STORAGE_MODE_FULL);
    const bool worsened =
        (newMode > oldMode && newMode >= STORAGE_MODE_FULL);

    if (!(enteredFull || worsened)) {
        return;
    }

    if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY) {
        return;
    }

    DLOG_WARN("STORAGE",
              "Pressure compaction trigger mode=%u used=%d%% pendingUpload=%lu",
              static_cast<unsigned>(newMode),
              getUsedPercent(),
              static_cast<unsigned long>(_pendingEventCount));

    const bool ok = compactSpool();
    if (!ok) {
        DLOG_WARN("STORAGE", "Pressure compaction failed");
        return;
    }

    // compactSpool only removes fully-uploaded segments, so the live
    // counter is already correct — no rescan needed.
    _persistEventMeta(true, "pressure_compact");
    refreshStorageUiState();

    DLOG_INFO("STORAGE",
              "Pressure compaction complete used=%d%% pendingUpload=%lu",
              getUsedPercent(),
              static_cast<unsigned long>(_pendingEventCount));
}

void StorageManager::updateStoragePressure(bool allowSideEffects) {
    const StoragePressureMode oldMode = _pressureMode;
    const int usedPct = getUsedPercent();

    const StoragePressure::Classification cls = StoragePressure::classify(usedPct);
    _pressureMode = cls.mode;
    _retentionPolicy = cls.policy;

    if (allowSideEffects) {
        _publishStorageEventIfNeeded(oldMode, _pressureMode);
        _maybeCompactForPressure(oldMode, _pressureMode);
    }
}

void StorageManager::_refreshFsStats(bool force) {
    const uint32_t now = millis();
    if (!force &&
        _cachedTotalBytes != 0 &&
        (now - _lastFsStatsRefreshMs) < STORAGE_FS_STATS_CACHE_MS) {
        return;
    }

    _cachedTotalBytes = LittleFS.totalBytes();
    _cachedUsedBytes = LittleFS.usedBytes();
    _cachedFreeBytes = (_cachedTotalBytes > _cachedUsedBytes) ?
        (_cachedTotalBytes - _cachedUsedBytes) : 0;
    _cachedUsedPct = (_cachedTotalBytes == 0) ? 0 :
        static_cast<int>((_cachedUsedBytes * 100) / _cachedTotalBytes);
    char usedBuf[32];
    snprintf(usedBuf,
             sizeof(usedBuf),
             "%luKB / %luKB",
             static_cast<unsigned long>(_cachedUsedBytes / 1024UL),
             static_cast<unsigned long>(_cachedTotalBytes / 1024UL));
    _cachedUsedString = usedBuf;
    _lastFsStatsRefreshMs = now;
}

void StorageManager::_queueStorageUiRefresh(bool defer) {
    const uint32_t now = millis();
    if (!_storageUiRefreshPending) {
        _storageUiRefreshDirtySinceMs = now;
    } else {
        // Keep pushing the service window forward while a burst is active.
        _storageUiRefreshDirtySinceMs = now;
    }
    _storageUiRefreshPending = true;
    if (defer) {
        _storageUiRefreshDeferred++;
    }
}

bool StorageManager::_storageUiRefreshDue() const {
    if (!_storageUiRefreshPending) {
        return false;
    }
    if (_storageUiRefreshDirtySinceMs == 0) {
        return true;
    }
    return (millis() - _storageUiRefreshDirtySinceMs) >=
           STORAGE_UI_REFRESH_COALESCE_MS;
}

StorageUiSnapshot StorageManager::_buildStorageUiSnapshot(
    size_t freeBytes,
    int usedPct) const {
    StorageUiSnapshot snap;
    snap.nearlyFull = (_pressureMode >= STORAGE_MODE_WATCH);
    snap.full = (_pressureMode >= STORAGE_MODE_FULL);
    snap.overrun = (_pressureMode >= STORAGE_MODE_OVERRUN);
    snap.dumpAdvised = (_pressureMode >= STORAGE_MODE_FULL) || _pendingEventCount >= 64;
    snap.mode = static_cast<uint8_t>(_pressureMode);
    snap.policy = static_cast<uint8_t>(_retentionPolicy);
    snap.usedPct = static_cast<uint16_t>(usedPct);
    snap.freeBytes = static_cast<uint32_t>(freeBytes);
    snap.pending = _pendingEventCount;
    snap.eventTotal = getDisplayEventCount();
    snap.recordTotal = getDisplayRecordCount();
    snap.deduped = _dedupStats.suppressed;
    snap.dropped = _dedupStats.dropped;

    snap.summaryValid = true;
    for (const auto& seg : _spoolIndex.segments) {
        // Header scans reset their work counter per file; without a boundary
        // yield, many sub-threshold segments plus the final sort can still
        // exceed the task watchdog as one continuous run.
        delay(1);
        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;
        if (!summaryReady) {
            const uint32_t eventTotal =
                (seg.eventCount > 0) ? seg.eventCount :
                (seg.recordCount > seg.enrichDeltaCount ?
                    (seg.recordCount - seg.enrichDeltaCount) : 0U);
            snap.noiseTotal += eventTotal;
            snap.pendingUploadNoise +=
                seg.pendingUploadMissionCount + seg.pendingUploadNoiseCount;
            snap.pendingEnrichNoise += seg.pendingEnrichmentCount;

            if (seg.firstEventId != 0 &&
                (snap.firstEventId == 0 || seg.firstEventId < snap.firstEventId)) {
                snap.firstEventId = seg.firstEventId;
            }
            if (seg.lastEventId > snap.lastEventId) {
                snap.lastEventId = seg.lastEventId;
            }

            snap.summaryValid = false;
            continue;
        }

        snap.missionTotal += seg.missionCount;
        snap.noiseTotal += seg.noiseCount;
        const uint32_t classifiedEvents = seg.missionCount + seg.noiseCount;
        const uint32_t eventTotal =
            (seg.eventCount > 0) ? seg.eventCount :
            (seg.recordCount > seg.enrichDeltaCount ?
                (seg.recordCount - seg.enrichDeltaCount) : 0U);
        if (eventTotal > classifiedEvents) {
            snap.noiseTotal += eventTotal - classifiedEvents;
        }
        snap.p0Total += seg.p0Count;
        snap.p1Total += seg.p1Count;
        snap.p2Total += seg.p2Count;
        snap.p3Total += seg.p3Count;
        snap.pendingUploadMission += seg.pendingUploadMissionCount;
        snap.pendingUploadNoise += seg.pendingUploadNoiseCount;
        if (seg.pendingEnrichmentCount > 0U) {
            if (seg.missionCount > 0U && seg.noiseCount == 0U) {
                snap.pendingEnrichMission += seg.pendingEnrichmentCount;
            } else {
                snap.pendingEnrichNoise += seg.pendingEnrichmentCount;
            }
        }
        snap.enrichmentDeltas += seg.enrichDeltaCount;

        if (seg.firstEventId != 0 &&
            (snap.firstEventId == 0 || seg.firstEventId < snap.firstEventId)) {
            snap.firstEventId = seg.firstEventId;
        }
        if (seg.lastEventId > snap.lastEventId) {
            snap.lastEventId = seg.lastEventId;
        }
    }

    const uint32_t splitPending =
        snap.pendingUploadMission + snap.pendingUploadNoise;
    if (splitPending < snap.pending) {
        snap.pendingUploadNoise += snap.pending - splitPending;
    } else if (splitPending > snap.pending) {
        uint32_t excess = splitPending - snap.pending;
        const uint32_t noiseTrim =
            (snap.pendingUploadNoise >= excess) ? excess : snap.pendingUploadNoise;
        snap.pendingUploadNoise -= noiseTrim;
        excess -= noiseTrim;
        if (excess > 0U) {
            snap.pendingUploadMission =
                (snap.pendingUploadMission >= excess)
                    ? (snap.pendingUploadMission - excess)
                    : 0U;
        }
    }

    snap.repairRequired =
        _counterTrustState == CounterTrust::RepairRequired ||
        _counterTrustState == CounterTrust::EmergencyOnly;
    snap.counterTrust = static_cast<uint8_t>(_counterTrustState);
    strlcpy(snap.policyText, _policyText(), sizeof(snap.policyText));
    return snap;
}

bool StorageManager::_storageUiSnapshotChanged(const StorageUiSnapshot& next) const {
    if (!_storageUiSnapshotValid) {
        return true;
    }
    return _storageUiSnapshot.nearlyFull != next.nearlyFull ||
           _storageUiSnapshot.full != next.full ||
           _storageUiSnapshot.overrun != next.overrun ||
           _storageUiSnapshot.dumpAdvised != next.dumpAdvised ||
           _storageUiSnapshot.mode != next.mode ||
           _storageUiSnapshot.policy != next.policy ||
           _storageUiSnapshot.usedPct != next.usedPct ||
           _storageUiSnapshot.freeBytes != next.freeBytes ||
           _storageUiSnapshot.pending != next.pending ||
           _storageUiSnapshot.summaryValid != next.summaryValid ||
           _storageUiSnapshot.missionTotal != next.missionTotal ||
           _storageUiSnapshot.noiseTotal != next.noiseTotal ||
           _storageUiSnapshot.eventTotal != next.eventTotal ||
           _storageUiSnapshot.recordTotal != next.recordTotal ||
           _storageUiSnapshot.p0Total != next.p0Total ||
           _storageUiSnapshot.p1Total != next.p1Total ||
           _storageUiSnapshot.p2Total != next.p2Total ||
           _storageUiSnapshot.p3Total != next.p3Total ||
           _storageUiSnapshot.pendingUploadMission != next.pendingUploadMission ||
           _storageUiSnapshot.pendingUploadNoise != next.pendingUploadNoise ||
           _storageUiSnapshot.pendingEnrichMission != next.pendingEnrichMission ||
           _storageUiSnapshot.pendingEnrichNoise != next.pendingEnrichNoise ||
           _storageUiSnapshot.enrichmentDeltas != next.enrichmentDeltas ||
           _storageUiSnapshot.deduped != next.deduped ||
           _storageUiSnapshot.dropped != next.dropped ||
           _storageUiSnapshot.firstEventId != next.firstEventId ||
           _storageUiSnapshot.lastEventId != next.lastEventId ||
           _storageUiSnapshot.repairRequired != next.repairRequired ||
           _storageUiSnapshot.counterTrust != next.counterTrust ||
           strcmp(_storageUiSnapshot.policyText, next.policyText) != 0;
}

void StorageManager::refreshStorageUiState(bool defer, bool recalcPressure) {
    if (defer) {
        _queueStorageUiRefresh(true);
        return;
    }

    if (_storageUiRefreshPending) {
        _storageUiRefreshFlush++;
    }
    _storageUiRefreshPending = false;
    _storageUiRefreshDirtySinceMs = 0;

    if (recalcPressure) {
        updateStoragePressure();
    }

    if (!recalcPressure && _cachedTotalBytes == 0) {
        _refreshFsStats(true);
    }

    const size_t freeBytes = recalcPressure ? getFreeBytes() : _cachedFreeBytes;
    const int usedPct = recalcPressure ? getUsedPercent() : _cachedUsedPct;
    const StorageUiSnapshot nextSnapshot = _buildStorageUiSnapshot(freeBytes, usedPct);

    if (!_storageUiSnapshotChanged(nextSnapshot)) {
        _storageUiRefreshPending = false;
        _storageUiRefreshDirtySinceMs = 0;
        return;
    }

    _storageUiSnapshot = nextSnapshot;
    _storageUiSnapshotValid = true;

    if (!recalcPressure) {
        const uint32_t serviceCount = ++_storageUiRefreshServiceCount;
        DLOG_INFO("STORAGE",
                  "worker ui refresh serviced uiRefreshServiceCount=%lu uiDeferred=%lu uiFlush=%lu pressure=%u usedPct=%d pending=%lu",
                  static_cast<unsigned long>(serviceCount),
                  static_cast<unsigned long>(_storageUiRefreshDeferred),
                  static_cast<unsigned long>(_storageUiRefreshFlush),
                  static_cast<unsigned>(_pressureMode),
                  static_cast<int>(nextSnapshot.usedPct),
                  static_cast<unsigned long>(nextSnapshot.pending));
    }

    StorageUiMirror::applyFull(nextSnapshot,
                               millis(),
                               _dedupStats.suppressed,
                               _dedupStats.dropped);
}

void StorageManager::beginWorkerAppendBatch(const char* reason) {
    if (!_ready) return;
    if (reason && reason[0]) {
        strlcpy(_workerBatchFlushReason, reason, sizeof(_workerBatchFlushReason));
    }
    _workerAppendBatchActive = true;
}

bool StorageManager::hasOpenWorkerAppendBatch() const {
    return _workerAppendBatchActive;
}

bool StorageManager::endWorkerAppendBatch(const char* reason, bool forceFlush) {
    if (!_ready) {
        _workerAppendBatchActive = false;
        return false;
    }

    const bool wasActive = _workerAppendBatchActive;
    _workerAppendBatchActive = false;

    if (!wasActive && !forceFlush) {
        return true;
    }

    if (!forceFlush) {
        return true;
    }

    const uint32_t startMs = millis();
    bool ok = true;
    ok = _flushWorkerAppendFile(reason ? reason : "worker_batch_end", true);
    if (!ok) {
        return false;
    }
    ok = flushWorkerMetadataBatch(reason ? reason : "worker_batch_end",
                                  true);
    refreshStorageUiState(false, true);

    if (reason && reason[0]) {
        strlcpy(_workerBatchFlushReason, reason, sizeof(_workerBatchFlushReason));
    } else {
        strlcpy(_workerBatchFlushReason, "worker_batch_end", sizeof(_workerBatchFlushReason));
    }

    const uint32_t elapsed = millis() - startMs;
    if (forceFlush || elapsed >= 50U) {
        DLOG_WARN("STORAGE",
                  "worker append batch flush reason=%s ms=%lu ok=%d",
                  _workerBatchFlushReason,
                  static_cast<unsigned long>(elapsed),
                  ok ? 1 : 0);
    } else {
        DLOG_INFO("STORAGE",
                  "worker append batch flush reason=%s ms=%lu ok=%d",
                  _workerBatchFlushReason,
                  static_cast<unsigned long>(elapsed),
                  ok ? 1 : 0);
    }

    return ok;
}

bool StorageManager::flushWorkerMetadataBatch(const char* reason,
                                              bool force,
                                              bool allowDuringCapture) {
    if (!_ready) {
        return false;
    }

    if (!_flushWorkerAppendFile(reason ? reason : "worker_metadata", false)) {
        return false;
    }

    const bool entryMetadataDirtyPending =
        _workerMetadataDirtyPending || _pendingCountDirty || _spoolIndexDirty;
    const bool entryUiRefreshDirty = _storageUiRefreshPending;
    if (!entryMetadataDirtyPending && !entryUiRefreshDirty) {
        return false;
    }

    if (!entryMetadataDirtyPending) {
        refreshStorageUiState(false, false);
        return true;
    }

    if (!allowDuringCapture &&
        RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY) {
        if (entryUiRefreshDirty) {
            refreshStorageUiState(false, false);
        }
        return true;
    }

    if (!force && _shouldDeferWorkerMetadataFlush()) {
        if (entryUiRefreshDirty) {
            refreshStorageUiState(false, false);
        }
        return true;
    }

    const uint32_t now = millis();
    const bool dueByCount =
        _workerMetadataPendingWrites >= STORAGE_EVENT_COUNTER_SAVE_EVERY_N;
    const bool dueByTime =
        _workerMetadataDirtySinceMs != 0 &&
        (now - _workerMetadataDirtySinceMs) >= STORAGE_HOT_META_SAVE_INTERVAL_MS;
    const bool hardFlush = force || dueByCount || dueByTime ||
                           _pendingCountDirty || _spoolIndexDirty;

    if (!hardFlush && !force) {
        if (entryUiRefreshDirty) {
            refreshStorageUiState(false, false);
        }
        return true;
    }

    const uint32_t startMs = millis();
    const bool hadPendingCountDirty = _pendingCountDirty;
    bool ok = true;
    ok &= _persistEventCounter(hardFlush, reason ? reason : "worker_batch");
    ok &= _persistEventMeta(hardFlush, reason ? reason : "worker_batch");
    ok &= _persistSpoolIndex(hardFlush, reason ? reason : "worker_batch");

    if (ok && hadPendingCountDirty &&
        !_repairRequested && !_spoolAuditRepairRequired) {
        _pendingCountDirty = false;
    }

    if (entryUiRefreshDirty || force) {
        refreshStorageUiState(false, false);
    }

    _workerMetadataDirtyPending =
        _eventCounterDirty || _pendingCountDirty || _spoolIndexDirty;
    if (ok && !_workerMetadataDirtyPending) {
        _workerMetadataPendingWrites = 0;
        _workerMetadataDirtySinceMs = 0;
        if (!_repairRequested && !_spoolAuditRepairRequired) {
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED, "worker_metadata_flush");
        }
    }

    if (reason && reason[0]) {
        strlcpy(_workerBatchFlushReason, reason, sizeof(_workerBatchFlushReason));
    } else {
        strlcpy(_workerBatchFlushReason, "worker_batch", sizeof(_workerBatchFlushReason));
    }

    const uint32_t elapsed = millis() - startMs;
    _metadataFlushCount++;
    if (elapsed > _metadataFlushSlowMs) {
        _metadataFlushSlowMs = elapsed;
    }
    static bool s_lastFlushLogValid = false;
    static bool s_lastEntryMetadataDirtyPending = false;
    static bool s_lastEntryUiRefreshDirty = false;
    static bool s_lastMetadataDirtyPending = false;
    static bool s_lastOk = true;
    static char s_lastFlushReason[32] = {};
    const bool flushStateChanged =
        !s_lastFlushLogValid ||
        s_lastEntryMetadataDirtyPending != entryMetadataDirtyPending ||
        s_lastEntryUiRefreshDirty != entryUiRefreshDirty ||
        s_lastMetadataDirtyPending != _workerMetadataDirtyPending ||
        s_lastOk != ok ||
        strcmp(s_lastFlushReason, _workerBatchFlushReason) != 0;

    if (elapsed >= 50U && flushStateChanged) {
        DLOG_WARN("STORAGE",
                  "worker metadata flush reason=%s ms=%lu ok=%d dirty=%d",
                  _workerBatchFlushReason,
                  static_cast<unsigned long>(elapsed),
                  ok ? 1 : 0,
                  _workerMetadataDirtyPending ? 1 : 0);
    }

    if (!ok) {
        _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                              "worker_metadata_flush_failed");
    }

    if (flushStateChanged || !ok) {
        DLOG_INFO("STORAGE",
                  "worker metadata flush metadataFlushCount=%lu reason=%s entryMetadataDirtyPending=%d entryUiRefreshPending=%d metadataDirtyPending=%d uiDeferred=%lu uiFlush=%lu uiRefreshServiceCount=%lu metadataFlushSlowMs=%lu",
                  static_cast<unsigned long>(_metadataFlushCount),
                  _workerBatchFlushReason,
                  entryMetadataDirtyPending ? 1 : 0,
                  entryUiRefreshDirty ? 1 : 0,
                  _workerMetadataDirtyPending ? 1 : 0,
                  static_cast<unsigned long>(_storageUiRefreshDeferred),
                  static_cast<unsigned long>(_storageUiRefreshFlush),
                  static_cast<unsigned long>(_storageUiRefreshServiceCount),
                  static_cast<unsigned long>(_metadataFlushSlowMs));

        s_lastFlushLogValid = true;
        s_lastEntryMetadataDirtyPending = entryMetadataDirtyPending;
        s_lastEntryUiRefreshDirty = entryUiRefreshDirty;
        s_lastMetadataDirtyPending = _workerMetadataDirtyPending;
        s_lastOk = ok;
        strlcpy(s_lastFlushReason, _workerBatchFlushReason, sizeof(s_lastFlushReason));
    }

    return ok;
}

void StorageManager::checkHealth() {
    DLOG_INFO("STORAGE", "Health check");
    DLOG_INFO("STORAGE", "Usage=%s", getUsedString().c_str());

    _ensureDir(PATH_LOGS);
    if (!LittleFS.exists(PATH_SESSIONS)) {
        File f = LittleFS.open(PATH_SESSIONS, "w");
        if (f) f.close();
    }

    updateStoragePressure();

    const bool hadTruthDebt =
        _spoolAuditRepairRequired ||
        _spoolSummaryRebuildPending ||
        _pendingCountDirty;

    if (hadTruthDebt) {
        requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT, "health_truth_debt");
        requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED, "health_truth_debt");
    }
    flushWorkerMetadataBatch("health", true);
    refreshStorageUiState();
    _logSpoolDiagnostics("health");
}

bool StorageManager::listLogFiles(std::vector<String>& files) {
    File dir = LittleFS.open(PATH_LOGS);
    if (!dir || !dir.isDirectory()) return false;
    File f = dir.openNextFile();
    while (f) {
        files.push_back(String(f.name()));
        f = dir.openNextFile();
    }
    return true;
}

bool StorageManager::readFile(const String& path, String& contents) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    contents = f.readString();
    f.close();
    return true;
}

bool StorageManager::deleteFile(const String& path) {
    return LittleFS.remove(path);
}

bool StorageManager::deleteOldestLog() {
    std::vector<String> files;
    listLogFiles(files);
    if (files.empty()) return false;
    std::sort(files.begin(), files.end());
    return deleteFile(String(PATH_LOGS) + "/" + files[0]);
}

void StorageManager::saveKnownLocations(
    SpectreState::KnownLocation* locs, int count) {
    KnownLocationsStore::save(locs, count);
}

int StorageManager::loadKnownLocations(
    SpectreState::KnownLocation* locs, int maxCount) {
    return KnownLocationsStore::load(locs, maxCount);
}

bool StorageManager::ensureBadUsbVault() {
    return BadUsbVault::ensureVault();
}

int StorageManager::loadBadUsbScriptIndex(BadUsbScriptInfo* outScripts, int maxCount) {
    return BadUsbVault::loadScriptIndex(outScripts, maxCount);
}

bool StorageManager::readBadUsbScript(const char* fileName, String& outScript) {
    return BadUsbVault::readScript(fileName, outScript);
}

bool StorageManager::writeBadUsbScript(const char* fileName, const char* scriptBody,
                                       const char* displayName, const char* desc) {
    return BadUsbVault::writeScript(fileName, scriptBody, displayName, desc);
}

bool StorageManager::_ensureDir(String path) {
    return StorageFsUtil::ensureDir(path);
}

bool StorageManager::_removePathWithRetry(const String& path) {
    return StorageFsUtil::removePathWithRetry(path);
}

bool StorageManager::_rmdirWithRetry(const String& path) {
    return StorageFsUtil::rmdirWithRetry(path);
}

bool StorageManager::_removeTree(const String& path) {
    return StorageFsUtil::removeTree(path);
}

bool StorageManager::wipeNonVaultStorage() {
    const size_t usedBefore = LittleFS.usedBytes();

    _uploadBatchActive = false;
    _uploadBatchDirty = false;
    _currentLoraLog = "";
    _currentWifiLog = "";
    _currentProbeLog = "";
    _nextEventId = 1;
    _eventCounterDirty = false;
    _eventCounterPendingWrites = 0;
    _pendingEventCount = 0;
    _pendingCountDirty = false;
    _spoolIndexDirty = false;
    _spoolIndexPendingWrites = 0;
    _spoolIndex = {};
    _binaryLastSessionBySegment.clear();
    _binaryLastSessionTagBySegment.clear();
    _binaryEnrichCtxBySegment.clear();
    _binaryCheckpointBySegment.clear();
    _backlog.uploadIndexBySession.clear();
    _backlog.uploadEnrichBySession.clear();
    _backlog.uploadIndexSessions.clear();
    _backlog.uploadIndexStats = {};

    bool ok = true;
    ok &= _removeTree(PATH_LOGS);
    ok &= _removeTree(PATH_EVENTS);
    ok &= _removeTree(PATH_SPOOL);
    ok &= _removeTree(PATH_EXPORTS);
    ok &= _removeTree(PATH_PMKID_DIR);
    ok &= _removeTree(PATH_LEGACY_PMKID_DIR);
    ok &= _removeTree(PATH_VOLATILE_VAULT_DIR);
    ok &= _removeTree(PATH_LEGACY_MQTT_QUEUE);

    if (LittleFS.exists(PATH_STORE_LEGACY_KNOWN_LOCATIONS) &&
        !_removePathWithRetry(PATH_STORE_LEGACY_KNOWN_LOCATIONS)) {
        ok = false;
    }

    _ensureDir(PATH_STORE_CONFIG_DIR);
    _ensureDir(PATH_STORE_VAULT_DIR);
    _ensureDir(PATH_LOGS);
    _ensureDir(PATH_EVENTS);
    _ensureDir(PATH_SPOOL);
    _ensureDir(PATH_EXPORTS);
    _ensureDir(PATH_PMKID_DIR);

    const size_t usedAfter = LittleFS.usedBytes();
    DLOG_INFO("STORAGE",
              "Non-vault storage wiped used=%uKB->%uKB",
              static_cast<unsigned>(usedBefore / 1024),
              static_cast<unsigned>(usedAfter / 1024));
    return ok;
}

bool StorageManager::wipeVaultStorage() {
    const size_t usedBefore = LittleFS.usedBytes();

    bool ok = true;
    ok &= _removeTree(PATH_STORE_VAULT_DIR);
    ok &= _removeTree(PATH_VOLATILE_VAULT_DIR);

    _ensureDir(PATH_STORE_CONFIG_DIR);
    _ensureDir(PATH_STORE_VAULT_DIR);
    _ensureDir(PATH_BADUSB_DIR);
    _ensureDir(PATH_FIELDVAULT_DIR);

    const size_t usedAfter = LittleFS.usedBytes();
    DLOG_WARN("STORAGE",
              "Vault storage wiped used=%uKB->%uKB",
              static_cast<unsigned>(usedBefore / 1024),
              static_cast<unsigned>(usedAfter / 1024));
    return ok;
}

bool StorageManager::_resetTagMatches(const char* path, const char* expectedTag) {
    if (!path || !path[0] || !expectedTag || !expectedTag[0] ||
        !LittleFS.exists(path)) {
        return false;
    }

    File f = LittleFS.open(path, "r");
    if (!f) {
        return false;
    }

    String tag = f.readStringUntil('\n');
    f.close();
    tag.trim();
    return tag == expectedTag;
}

bool StorageManager::_writeResetTag(const char* path, const char* tag) {
    if (!path || !path[0] || !tag || !tag[0]) {
        return false;
    }

    File f = LittleFS.open(path, "w");
    if (!f) {
        DLOG_WARN("STORAGE", "Reset tag open failed path=%s", path);
        return false;
    }

    const size_t written = f.print(tag);
    f.print('\n');
    f.close();

    if (written != strlen(tag)) {
        DLOG_WARN("STORAGE", "Reset tag write failed path=%s", path);
        return false;
    }
    return true;
}

bool StorageManager::_applyOneShotVaultReset() {
    if (!STORAGE_ONE_SHOT_VAULT_RESET_ENABLED) {
        return true;
    }

    if (_resetTagMatches(PATH_STORE_VAULT_RESET_TAG,
                         STORAGE_ONE_SHOT_VAULT_RESET_TAG)) {
        return true;
    }

    DLOG_WARN("STORAGE",
              "Applying vault reset via config.h toggle tag=%s",
              STORAGE_ONE_SHOT_VAULT_RESET_TAG);

    if (!wipeVaultStorage()) {
        return false;
    }

    _ensureDir(PATH_STORE_CONFIG_DIR);
    return _writeResetTag(PATH_STORE_VAULT_RESET_TAG,
                          STORAGE_ONE_SHOT_VAULT_RESET_TAG);
}

bool StorageManager::_applyOneShotNonVaultReset() {
    if (!STORAGE_ONE_SHOT_NON_VAULT_RESET_ENABLED) {
        return true;
    }

    if (_resetTagMatches(PATH_STORE_NON_VAULT_RESET_TAG,
                         STORAGE_ONE_SHOT_NON_VAULT_RESET_TAG)) {
        return true;
    }

    DLOG_WARN("STORAGE",
              "Applying non-vault reset via config.h toggle tag=%s",
              STORAGE_ONE_SHOT_NON_VAULT_RESET_TAG);

    if (!wipeNonVaultStorage()) {
        return false;
    }

    _ensureDir(PATH_STORE_CONFIG_DIR);
    _ensureDir(PATH_STORE_VAULT_DIR);
    return _writeResetTag(PATH_STORE_NON_VAULT_RESET_TAG,
                          STORAGE_ONE_SHOT_NON_VAULT_RESET_TAG);
}

bool StorageManager::_trimJsonLinesFile(const char* path, size_t keepLastLines) {
    if (!path || !path[0] || keepLastLines == 0 || !LittleFS.exists(path)) {
        return true;
    }

    File f = LittleFS.open(path, "r");
    if (!f) {
        return false;
    }

    std::vector<String> lines;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length()) {
            lines.push_back(line);
        }
    }
    f.close();

    if (lines.size() <= keepLastLines) {
        return true;
    }

    File out = LittleFS.open(path, "w");
    if (!out) {
        return false;
    }

    const size_t start = lines.size() - keepLastLines;
    for (size_t i = start; i < lines.size(); ++i) {
        out.print(lines[i]);
        out.print('\n');
    }
    out.close();
    return true;
}

bool StorageManager::_appendJsonLine(const String& path, JsonDocument& doc) {
    File f = LittleFS.open(path, "a");
    if (!f) return false;
    serializeJson(doc, f);
    f.print('\n');
    f.close();
    return true;
}

bool StorageManager::_atomicWriteFile(const String& path,
                                      std::function<bool(fs::File&)> writer,
                                      bool keepBackup) {
    const String tmpPath = path + ".tmp";
    const String bakPath = path + ".bak";

    if (LittleFS.exists(tmpPath) && !LittleFS.remove(tmpPath)) {
        return false;
    }

    File tmp = LittleFS.open(tmpPath, "w");
    if (!tmp) {
        return false;
    }

    const bool wrote = writer(tmp);
    tmp.flush();
    tmp.close();
    if (!wrote) {
        LittleFS.remove(tmpPath);
        return false;
    }

    bool movedToBak = false;
    if (keepBackup && LittleFS.exists(path)) {
        if (LittleFS.exists(bakPath) && !LittleFS.remove(bakPath)) {
            LittleFS.remove(tmpPath);
            return false;
        }
        if (!LittleFS.rename(path, bakPath)) {
            LittleFS.remove(tmpPath);
            return false;
        }
        movedToBak = true;
    } else if (LittleFS.exists(path) && !LittleFS.remove(path)) {
        LittleFS.remove(tmpPath);
        return false;
    }

    if (!LittleFS.rename(tmpPath, path)) {
        LittleFS.remove(tmpPath);
        if (movedToBak) {
            if (LittleFS.exists(path)) {
                LittleFS.remove(path);
            }
            (void)LittleFS.rename(bakPath, path);
        }
        return false;
    }

    bool verified = false;
    bool verifyOpened = false;
    size_t verifySize = 0;
    char parseText[32] = "not_checked";
    const char* rootType = "not_checked";
    bool requiredFieldsPresent = false;
    const char* restoreResult = "not_needed";
    File verify = LittleFS.open(path, "r");
    if (verify) {
        verifyOpened = true;
        verifySize = verify.size();
        JsonDocument verifyDoc;
        DeserializationError err = deserializeJson(verifyDoc, verify);
        strlcpy(parseText, err ? err.c_str() : "ok", sizeof(parseText));
        rootType = _jsonRootTypeText(verifyDoc);
        if (!err && verifyDoc.is<JsonObject>()) {
            JsonObject obj = verifyDoc.as<JsonObject>();
            if (path == PATH_EVENT_COUNTER) {
                verified = obj["generation"].is<uint32_t>() &&
                           obj["next_event_id"].is<uint32_t>();
                requiredFieldsPresent = verified;
            } else if (path == PATH_EVENT_META) {
                verified = obj["generation"].is<uint32_t>() &&
                           obj["pending_total"].is<uint32_t>();
                requiredFieldsPresent = verified;
            } else if (path == _spoolIndexPath()) {
                verified = obj["generation"].is<uint32_t>() &&
                           obj["pending_total"].is<uint32_t>() &&
                           obj["next_event_id"].is<uint32_t>();
                requiredFieldsPresent = verified;
            } else {
                verified = true;
            }
        }
        verify.close();
    }

    if (verified) {
        return true;
    }

    if (movedToBak) {
        if (LittleFS.exists(bakPath) && LittleFS.rename(bakPath, path)) {
            restoreResult = "restored";
        } else {
            restoreResult = "restore_failed";
        }
    } else {
        restoreResult = "no_backup";
    }

    if (path == PATH_EVENT_META) {
        DLOG_WARN("STORAGE",
                  "Atomic verify meta failed reopen=%d size=%lu parse_error=%s root=%s required_fields=%d restore=%s path=%s",
                  verifyOpened ? 1 : 0,
                  static_cast<unsigned long>(verifySize),
                  parseText,
                  rootType,
                  requiredFieldsPresent ? 1 : 0,
                  restoreResult,
                  path.c_str());
    } else {
        DLOG_WARN("STORAGE",
                  "Atomic write verify failed path=%s restore=%s",
                  path.c_str(),
                  restoreResult);
    }
    return false;
}

void StorageManager::_bumpStorageMetaGeneration() {
    if (_storageMetaGeneration == 0) {
        _storageMetaGeneration = 1;
    } else if (_storageMetaGeneration < UINT32_MAX) {
        _storageMetaGeneration++;
    }
}

bool StorageManager::_loadEventCounter() {
    _eventCounterLoaded = false;
    _eventCounterGeneration = 0;

    uint32_t nextId = 0;
    uint32_t liveNextId = 0;
    uint32_t backupNextId = 0;
    bool loaded = false;

    auto loadCounterFile = [](const String& path,
                              uint32_t& outNextId,
                              uint32_t& outGeneration) -> bool {
        if (!LittleFS.exists(path)) {
            return false;
        }

        File f = LittleFS.open(path, "r");
        if (f) {
            String stored = f.readString();
            stored.trim();
            if (stored.length()) {
                JsonDocument counterDoc;
                DeserializationError err = deserializeJson(counterDoc, stored);
                if (!err && counterDoc.is<JsonObject>()) {
                    outNextId = counterDoc["next_event_id"] | 0U;
                    outGeneration = counterDoc["generation"] | 0U;
                    f.close();
                    return outNextId > 0U;
                } else {
                    uint32_t storedNext = strtoul(stored.c_str(), nullptr, 10);
                    if (storedNext > 0U) {
                        outNextId = storedNext;
                        outGeneration = 0;
                        f.close();
                        return true;
                    }
                }
            }
            f.close();
        }
        return false;
    };

    uint32_t liveGeneration = 0;
    uint32_t backupGeneration = 0;
    const bool liveLoaded =
        loadCounterFile(PATH_EVENT_COUNTER, liveNextId, liveGeneration);
    const String backupPath = String(PATH_EVENT_COUNTER) + ".bak";
    const bool backupLoaded =
        loadCounterFile(backupPath, backupNextId, backupGeneration);

    if (liveLoaded || backupLoaded) {
        loaded = true;
        if (backupNextId > liveNextId) {
            nextId = backupNextId;
            _eventCounterGeneration = backupGeneration;
            DLOG_WARN("STORAGE",
                      "Event counter recovered from backup live=%lu backup=%lu",
                      static_cast<unsigned long>(liveNextId),
                      static_cast<unsigned long>(backupNextId));
        } else {
            nextId = liveNextId;
            _eventCounterGeneration = liveGeneration;
        }
    }

    if (!loaded) {
        nextId = _spoolIndex.nextEventId;
        if (nextId == 0U) {
            nextId = _scanNextEventIdFromSpool();
        }
    }

    if (nextId == 0U) {
        nextId = 1U;
    }

    _nextEventId = nextId;
    _eventCounterLoaded = loaded;
    _lastCounterSaveMs = millis();
    return true;
}

bool StorageManager::_loadEventMeta() {
    _eventMetaLoaded = false;
    _eventMetaGeneration = 0;

    if (!LittleFS.exists(PATH_EVENT_META)) {
        _pendingEventCount = _spoolIndex.pendingTotal;
        _pendingCountDirty = true;
        return true;
    }

    File f = LittleFS.open(PATH_EVENT_META, "r");
    if (!f) {
        _pendingEventCount = _spoolIndex.pendingTotal;
        _pendingCountDirty = true;
        return true;
    }

    JsonDocument metaDoc;
    DeserializationError err = deserializeJson(metaDoc, f);
    f.close();
    if (err || !metaDoc.is<JsonObject>()) {
        _pendingEventCount = _spoolIndex.pendingTotal;
        _pendingCountDirty = true;
        return true;
    }

    _pendingEventCount = metaDoc["pending_total"] | 0U;
    _eventMetaGeneration = metaDoc["generation"] | 0U;
    JsonVariant storedRecordTotal = metaDoc["record_total"];
    if (!storedRecordTotal.isNull()) {
        _storedRecordCountExactBase = storedRecordTotal | 0U;
        _storedRecordCountIndexBase =
            metaDoc["record_total_index"] | getStoredRecordCount();
        _storedRecordCountUpdatedMs = millis();
        _storedRecordCountCacheValid = true;
    }
    _eventMetaLoaded = true;
    _pendingCountDirty = false;
    if (_eventMetaGeneration == 0 && _spoolIndex.generation != 0) {
        _eventMetaGeneration = _spoolIndex.generation;
    }
    _lastMetaSaveMs = millis();
    return true;
}

bool StorageManager::_persistEventMeta(bool force, const char* reason) {
    CONTRACT_WARN_ONCE(CONTRACT_NO_FS_WRITE_DURING_UPLOAD_EXCEPT_CHECKPOINT,
                       "STORAGE",
                       !RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD) ||
                           (reason && strcmp(reason, "upload_checkpoint") == 0),
                       "meta reason=%s",
                       (reason && reason[0]) ? reason : "-");
    const bool dueByTime =
        millis() - _lastMetaSaveMs >= STORAGE_HOT_META_SAVE_INTERVAL_MS;
    if (!force && !dueByTime) {
        return true;
    }

    JsonDocument metaDoc;
    const uint32_t generation = _storageMetaGeneration == 0 ? 1U : _storageMetaGeneration;
    _storageMetaGeneration = generation;
    metaDoc["generation"] = generation;
    metaDoc["pending_total"] = _pendingEventCount;
    if (_storedRecordCountCacheValid) {
        metaDoc["record_total"] = getDisplayRecordCount();
        metaDoc["record_total_index"] = getStoredRecordCount();
    }

    const uint32_t writeStartMs = millis();
    const bool ok = _atomicWriteFile(
        PATH_EVENT_META,
        [&](fs::File& f) -> bool {
            return serializeJson(metaDoc, f) > 0U;
        },
        true);
    if (!ok) {
        return false;
    }
    _lastMetaSaveMs = millis();
    _logCaptureWriteAllowed(PATH_EVENT_META,
                            reason,
                            millis() - writeStartMs,
                            true);
    return true;
}

uint32_t StorageManager::_scanNextEventIdFromSpool() const {
    uint32_t maxEventId = 0;
    for (const auto& seg : _spoolIndex.segments) {
        const bool ok = _scanSegmentRecords(
            seg.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                if (rec.eventId > maxEventId) {
                    maxEventId = rec.eventId;
                }
                return true;
            });
        if (!ok) {
            DLOG_WARN("STORAGE",
                      "Boot nextEventId scan failed seg=%lu trust=%s",
                      static_cast<unsigned long>(seg.segmentId),
                      _spoolTrustText(seg.trustState));
        }
    }

    return maxEventId > 0 ? (maxEventId + 1U) : 1U;
}

bool StorageManager::_persistEventCounter(bool force, const char* reason) {
    CONTRACT_WARN_ONCE(CONTRACT_NO_FS_WRITE_DURING_UPLOAD_EXCEPT_CHECKPOINT,
                       "STORAGE",
                       !RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD) ||
                           (reason && strcmp(reason, "upload_checkpoint") == 0),
                       "counter reason=%s",
                       (reason && reason[0]) ? reason : "-");
    if (!_eventCounterDirty) return true;

    const bool dueByCount =
        _eventCounterPendingWrites >= STORAGE_EVENT_COUNTER_SAVE_EVERY_N;
    const bool dueByTime =
        millis() - _lastCounterSaveMs >= STORAGE_HOT_META_SAVE_INTERVAL_MS;
    if (!force && !dueByCount && !dueByTime) {
        return true;
    }

    JsonDocument counterDoc;
    const uint32_t generation = _storageMetaGeneration == 0 ? 1U : _storageMetaGeneration;
    _storageMetaGeneration = generation;
    counterDoc["generation"] = generation;
    counterDoc["next_event_id"] = _nextEventId;

    const uint32_t writeStartMs = millis();
    const bool ok = _atomicWriteFile(
        PATH_EVENT_COUNTER,
        [&](fs::File& f) -> bool {
            return serializeJson(counterDoc, f) > 0U;
        },
        true);
    if (!ok) return false;

    _lastCounterSaveMs = millis();
    _eventCounterPendingWrites = 0;
    _eventCounterDirty = false;
    _logCaptureWriteAllowed(PATH_EVENT_COUNTER,
                            reason,
                            millis() - writeStartMs,
                            true);
    return true;
}

bool StorageManager::_activeSegmentConfirmsNextEventId(uint32_t nextEventId) const {
    if (nextEventId == 0) {
        return false;
    }

    if (_spoolIndex.activeSegmentId == 0) {
        return nextEventId <= 1U;
    }

    const SpoolSegmentInfo* active = _findSegmentInfo(_spoolIndex.activeSegmentId);
    if (!active || active->format != SPOOL_SEGMENT_BIN_V2) {
        return false;
    }

    const String path = _spoolBinarySegmentPath(active->segmentId);
    File f = LittleFS.open(path, "r");
    if (!f) {
        return false;
    }

    SpoolBin::SegmentHeaderV2 hdr;
    const bool ok = SpoolBin::readSegmentHeaderV2(f, hdr);
    f.close();
    if (!ok || hdr.magic != SpoolBin::SEGMENT_MAGIC || hdr.version != 2) {
        return false;
    }

    if (hdr.lastEventId == 0U) {
        return nextEventId == 1U;
    }

    return (hdr.lastEventId + 1U) == nextEventId;
}

bool StorageManager::_activeSegmentNearRotateThreshold() const {
    if (_spoolIndex.activeSegmentId == 0) {
        return false;
    }

    const SpoolSegmentInfo* active = _findSegmentInfo(_spoolIndex.activeSegmentId);
    if (!active || active->recordCount == 0U) {
        return false;
    }

    return active->approxBytes >= SPOOL_SEGMENT_PREROTATE_BYTES;
}

bool StorageManager::_shouldRotateSegmentAfterAppend(const SpoolSegmentInfo& seg) const {
    size_t rotateBytes = SPOOL_SEGMENT_TARGET_BYTES;
    if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE) {
        rotateBytes = SPOOL_SEGMENT_CAPTURE_HARD_BYTES;
    }
    return seg.approxBytes >= rotateBytes;
}

bool StorageManager::_preRotateActiveSegmentIfNearFull(const char* reason) {
    if (!_activeSegmentNearRotateThreshold()) {
        return true;
    }

    const SpoolSegmentInfo* active = _findSegmentInfo(_spoolIndex.activeSegmentId);
    if (!active) {
        return false;
    }

    const uint32_t oldSegmentId = active->segmentId;
    const uint32_t oldBytes = static_cast<uint32_t>(active->approxBytes);
    const uint32_t oldRecords = active->recordCount;

    DLOG_INFO("STORAGE",
              "Spool maintenance pre-rotate seg=%lu bytes=%lu records=%lu reason=%s",
              static_cast<unsigned long>(oldSegmentId),
              static_cast<unsigned long>(oldBytes),
              static_cast<unsigned long>(oldRecords),
              (reason && reason[0]) ? reason : "maintenance");

    return _openNewSpoolSegment();
}

bool StorageManager::_reconcileStorageMetadata() {
    const bool captureDefer =
        RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY;
    const uint32_t indexGen = _spoolIndex.generation;
    const uint32_t metaGen = _eventMetaLoaded ? _eventMetaGeneration : 0U;
    const uint32_t counterGen = _eventCounterLoaded ? _eventCounterGeneration : 0U;
    const uint32_t chosenGen = std::max(
        std::max(indexGen, metaGen),
        std::max(counterGen, static_cast<uint32_t>(1U)));

    const bool allHaveGenerations =
        indexGen != 0U && _eventMetaLoaded && _eventCounterLoaded &&
        metaGen != 0U && counterGen != 0U;
    if (allHaveGenerations) {
        const uint32_t lowestGen = std::min(
            std::min(indexGen, metaGen),
            counterGen);
        if (chosenGen - lowestGen > 1U) {
            DLOG_WARN("STORAGE",
                      "Metadata generations diverged index=%lu meta=%lu counter=%lu; reconciling",
                      static_cast<unsigned long>(indexGen),
                      static_cast<unsigned long>(metaGen),
                      static_cast<unsigned long>(counterGen));
        }
    }

    const bool indexNewest = indexGen >= metaGen && indexGen >= counterGen;
    if (indexNewest) {
        const uint32_t sourceGen = chosenGen;
        const uint32_t indexPendingBefore = _spoolIndex.pendingTotal;
        const uint32_t indexGenBefore = indexGen;
        const uint32_t indexNextBefore = _spoolIndex.nextEventId;
        uint32_t sourceNextEventId = indexNextBefore;
        if (sourceNextEventId == 0U) {
            sourceNextEventId = _nextEventId > 0U ? _nextEventId : 1U;
        }
        const bool indexPendingMismatch = _pendingEventCount != indexPendingBefore;

        const bool metaNeedsRewrite =
            !_eventMetaLoaded ||
            _eventMetaGeneration != sourceGen ||
            _pendingEventCount != indexPendingBefore;
        const bool counterNeedsRewrite =
            !_eventCounterLoaded ||
            _eventCounterGeneration != sourceGen ||
            _nextEventId != sourceNextEventId;

        _storageMetaGeneration = sourceGen;
        _spoolIndex.generation = sourceGen;
        _spoolIndex.pendingTotal = indexPendingBefore;
        _spoolIndex.nextEventId = sourceNextEventId;
        _pendingEventCount = indexPendingBefore;
        _nextEventId = sourceNextEventId;

        bool ok = true;
        if (metaNeedsRewrite) {
            _pendingCountDirty = true;
            const bool metaOk = _persistEventMeta(true, "metadata_generation_match");
            ok &= metaOk;
            if (metaOk) {
                _eventMetaGeneration = sourceGen;
                _eventMetaLoaded = true;
            }
        } else {
            _eventMetaGeneration = sourceGen;
        }

        if (counterNeedsRewrite) {
            _eventCounterDirty = true;
            _eventCounterPendingWrites = 1;
            const bool counterOk = _persistEventCounter(true, "metadata_generation_match");
            ok &= counterOk;
            if (counterOk) {
                _eventCounterGeneration = sourceGen;
                _eventCounterLoaded = true;
            }
        } else {
            _eventCounterGeneration = sourceGen;
        }

        const bool indexNeedsRewrite =
            indexGenBefore != sourceGen ||
            indexPendingMismatch ||
            indexNextBefore == 0U ||
            indexNextBefore != sourceNextEventId;
        if (captureDefer &&
            (metaNeedsRewrite || counterNeedsRewrite || indexNeedsRewrite)) {
            _spoolAuditRepairRequired = false;
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                  "metadata_generation_capture_deferred");
            DLOG_INFO("STORAGE",
                      "Metadata reconcile deferred owner=%s reason=capture gen=%lu pending=%lu nextEventId=%lu",
                      RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                      static_cast<unsigned long>(_storageMetaGeneration),
                      static_cast<unsigned long>(_pendingEventCount),
                      static_cast<unsigned long>(_nextEventId));
            return true;
        }
        if (indexNeedsRewrite) {
            _spoolIndexDirty = true;
            ok &= _persistSpoolIndex(true, "metadata_generation_match");
        }

        if (ok) {
            _pendingCountDirty = false;
            _spoolAuditRepairRequired = false;
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED,
                                  "metadata_generation_match");
            DLOG_INFO("STORAGE",
                      "Counter trust=trusted reason=metadata_generation_match gen=%lu pending=%lu nextEventId=%lu",
                      static_cast<unsigned long>(_storageMetaGeneration),
                      static_cast<unsigned long>(_pendingEventCount),
                      static_cast<unsigned long>(_nextEventId));
        } else {
            _spoolAuditRepairRequired = false;
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                  "metadata_generation_rewrite_failed");
        }
        return true;
    }

    const uint32_t sourceGen = chosenGen;
    const uint32_t sourcePending =
        (_eventMetaGeneration >= indexGen) ? _pendingEventCount : _spoolIndex.pendingTotal;
    const uint32_t sourceNextEventId =
        _nextEventId > 0U ? _nextEventId :
        (_spoolIndex.nextEventId > 0U ? _spoolIndex.nextEventId : 1U);

    if (!_activeSegmentConfirmsNextEventId(sourceNextEventId)) {
        _spoolAuditRepairRequired = true;
        _setCounterTrustState(STORAGE_COUNTER_REPAIR_REQUIRED,
                              "metadata_generation_header_mismatch");
        return true;
    }

    _storageMetaGeneration = sourceGen;
    _spoolIndex.generation = sourceGen;
    _spoolIndex.pendingTotal = sourcePending;
    _spoolIndex.nextEventId = sourceNextEventId;
    _pendingEventCount = sourcePending;
    _nextEventId = sourceNextEventId;

    _pendingCountDirty = true;
    _eventCounterDirty = true;
    _eventCounterPendingWrites = 1;
    _spoolIndexDirty = true;

    if (captureDefer) {
        _spoolAuditRepairRequired = false;
        _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                              "metadata_generation_capture_deferred");
        DLOG_INFO("STORAGE",
                  "Metadata reconcile deferred owner=%s reason=capture gen=%lu pending=%lu nextEventId=%lu",
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                  static_cast<unsigned long>(_storageMetaGeneration),
                  static_cast<unsigned long>(_pendingEventCount),
                  static_cast<unsigned long>(_nextEventId));
        return true;
    }

    bool ok = true;
    ok &= _persistEventMeta(true, "metadata_generation_match");
    ok &= _persistEventCounter(true, "metadata_generation_match");
    ok &= _persistSpoolIndex(true, "metadata_generation_match");
    if (ok) {
        _eventMetaGeneration = sourceGen;
        _eventMetaLoaded = true;
        _eventCounterGeneration = sourceGen;
        _eventCounterLoaded = true;
        _pendingCountDirty = false;
        _spoolAuditRepairRequired = false;
        _setCounterTrustState(STORAGE_COUNTER_TRUSTED,
                              "metadata_generation_match");
        DLOG_INFO("STORAGE",
                  "Counter trust=trusted reason=metadata_generation_match gen=%lu pending=%lu nextEventId=%lu",
                  static_cast<unsigned long>(_storageMetaGeneration),
                  static_cast<unsigned long>(_pendingEventCount),
                  static_cast<unsigned long>(_nextEventId));
    } else {
        _spoolAuditRepairRequired = false;
        _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                              "metadata_generation_rewrite_failed");
    }
    return true;
}

bool StorageManager::_auditSpoolBinaryHeader(const SpoolSegmentInfo& seg,
                                             SpoolBin::SegmentHeaderV2& hdr) const {
    if (seg.format != SPOOL_SEGMENT_BIN_V2) {
        DLOG_WARN("STORAGE",
                  "Boot audit legacy segment format seg=%lu format=%s",
                  static_cast<unsigned long>(seg.segmentId),
                  _segmentFormatText(seg.format));
        return false;
    }

    const String path = _spoolBinarySegmentPath(seg.segmentId);
    File f = LittleFS.open(path, "r");
    if (!f) {
        DLOG_WARN("STORAGE",
                  "Boot audit header open failed seg=%lu path=%s",
                  static_cast<unsigned long>(seg.segmentId),
                  path.c_str());
        return false;
    }

    const uint32_t fileSize = static_cast<uint32_t>(f.size());
    if (!SpoolBin::readSegmentHeaderV2(f, hdr)) {
        DLOG_WARN("STORAGE",
                  "Boot audit header read failed seg=%lu path=%s",
                  static_cast<unsigned long>(seg.segmentId),
                  path.c_str());
        f.close();
        return false;
    }
    f.close();

    if (hdr.magic != SpoolBin::SEGMENT_MAGIC ||
        hdr.version != 2 ||
        hdr.headerSize != sizeof(SpoolBin::SegmentHeaderV2) ||
        hdr.segmentId != seg.segmentId) {
        DLOG_WARN("STORAGE",
                  "Boot audit header invalid seg=%lu path=%s",
                  static_cast<unsigned long>(seg.segmentId),
                  path.c_str());
        return false;
    }

    if (hdr.recordCount == 0U) {
        if (hdr.firstEventId != 0U || hdr.lastEventId != 0U || hdr.bodyBytes != 0U) {
            DLOG_WARN("STORAGE",
                      "Boot audit empty header mismatch seg=%lu first=%lu last=%lu body=%lu",
                      static_cast<unsigned long>(seg.segmentId),
                      static_cast<unsigned long>(hdr.firstEventId),
                      static_cast<unsigned long>(hdr.lastEventId),
                      static_cast<unsigned long>(hdr.bodyBytes));
            return false;
        }
    } else {
        const uint32_t minRecordBytes =
            static_cast<uint32_t>(hdr.recordCount) *
            static_cast<uint32_t>(sizeof(SpoolBin::RecordPrefix));
        if (hdr.firstEventId == 0U ||
            hdr.lastEventId == 0U ||
            hdr.firstEventId > hdr.lastEventId ||
            hdr.bodyBytes < minRecordBytes) {
            DLOG_WARN("STORAGE",
                      "Boot audit header impossible seg=%lu first=%lu last=%lu records=%lu body=%lu",
                      static_cast<unsigned long>(seg.segmentId),
                      static_cast<unsigned long>(hdr.firstEventId),
                      static_cast<unsigned long>(hdr.lastEventId),
                      static_cast<unsigned long>(hdr.recordCount),
                      static_cast<unsigned long>(hdr.bodyBytes));
            return false;
        }
    }

    const uint32_t minApproxBytes =
        static_cast<uint32_t>(sizeof(SpoolBin::SegmentHeaderV2)) + hdr.bodyBytes;
    if (fileSize < minApproxBytes) {
        DLOG_WARN("STORAGE",
                  "Boot audit truncated segment seg=%lu size=%lu min=%lu",
                  static_cast<unsigned long>(seg.segmentId),
                  static_cast<unsigned long>(fileSize),
                  static_cast<unsigned long>(minApproxBytes));
        return false;
    }

    return true;
}

bool StorageManager::_auditSpoolBinaryCheckpointTail(
    const SpoolSegmentInfo& seg,
    const SpoolBin::SegmentHeaderV2& hdr) const {
    if (seg.format != SPOOL_SEGMENT_BIN_V2) {
        return false;
    }

    if (hdr.recordCount == 0U && hdr.bodyBytes == 0U) {
        return true;
    }

    const String path = _spoolBinarySegmentPath(seg.segmentId);
    File f = LittleFS.open(path, "r");
    if (!f) {
        DLOG_WARN("STORAGE",
                  "Boot audit checkpoint open failed seg=%lu path=%s",
                  static_cast<unsigned long>(seg.segmentId),
                  path.c_str());
        return false;
    }

    if (!f.seek(sizeof(SpoolBin::SegmentHeaderV2))) {
        DLOG_WARN("STORAGE",
                  "Boot audit checkpoint seek failed seg=%lu path=%s",
                  static_cast<unsigned long>(seg.segmentId),
                  path.c_str());
        f.close();
        return false;
    }

    bool foundCheckpoint = false;
    SpoolBin::SpoolSegmentCheckpointV1 latestCheckpoint;
    while (f.position() < f.size()) {
        const uint32_t prefixOffset = static_cast<uint32_t>(f.position());
        const size_t remainingBeforePrefix =
            static_cast<size_t>(f.size() - f.position());
        if (remainingBeforePrefix < sizeof(SpoolBin::RecordPrefix)) {
            DLOG_WARN("STORAGE",
                      "Boot audit checkpoint truncated tail seg=%lu remaining=%u",
                      static_cast<unsigned long>(seg.segmentId),
                      static_cast<unsigned>(remainingBeforePrefix));
            f.close();
            return false;
        }

        SpoolBin::RecordPrefix prefix;
        if (!SpoolBin::readBytes(f, &prefix, sizeof(prefix))) {
            DLOG_WARN("STORAGE",
                      "Boot audit checkpoint prefix read failed seg=%lu path=%s",
                      static_cast<unsigned long>(seg.segmentId),
                      path.c_str());
            f.close();
            return false;
        }

        const size_t remainingAfterPrefix =
            static_cast<size_t>(f.size() - f.position());
        if (prefix.length > remainingAfterPrefix) {
            DLOG_WARN("STORAGE",
                      "Boot audit checkpoint body overrun seg=%lu off=%lu type=%u len=%u remaining=%u",
                      static_cast<unsigned long>(seg.segmentId),
                      static_cast<unsigned long>(prefixOffset),
                      static_cast<unsigned>(prefix.type),
                      static_cast<unsigned>(prefix.length),
                      static_cast<unsigned>(remainingAfterPrefix));
            f.close();
            return false;
        }

        const uint32_t bodyOffset = static_cast<uint32_t>(f.position());
        if (prefix.type != SpoolBin::REC_CHECKPOINT) {
            if (!f.seek(bodyOffset + prefix.length)) {
                DLOG_WARN("STORAGE",
                          "Boot audit checkpoint skip failed seg=%lu off=%lu",
                          static_cast<unsigned long>(seg.segmentId),
                          static_cast<unsigned long>(prefixOffset));
                f.close();
                return false;
            }
            continue;
        }

        uint8_t body[sizeof(SpoolBin::SpoolSegmentCheckpointV1)] = {};
        if (prefix.length != sizeof(body) ||
            !SpoolBin::readBytes(f, body, sizeof(body))) {
            DLOG_WARN("STORAGE",
                      "Boot audit checkpoint body invalid seg=%lu len=%u",
                      static_cast<unsigned long>(seg.segmentId),
                      static_cast<unsigned>(prefix.length));
            f.close();
            return false;
        }

        SpoolBin::SpoolSegmentCheckpointV1 checkpoint;
        if (!SpoolBin::decodeCheckpointRecordV1(body,
                                                sizeof(body),
                                                bodyOffset,
                                                checkpoint)) {
            DLOG_WARN("STORAGE",
                      "Boot audit checkpoint decode failed seg=%lu path=%s",
                      static_cast<unsigned long>(seg.segmentId),
                      path.c_str());
            f.close();
            return false;
        }

        foundCheckpoint = true;
        latestCheckpoint = checkpoint;
    }

    f.close();

    if (!foundCheckpoint) {
        return true;
    }

    if (latestCheckpoint.segmentId != hdr.segmentId ||
        latestCheckpoint.recordCount > hdr.recordCount ||
        latestCheckpoint.lastEventId > hdr.lastEventId) {
        DLOG_WARN("STORAGE",
                  "Boot audit checkpoint mismatch seg=%lu hdrRecords=%lu cpRecords=%lu hdrLast=%lu cpLast=%lu",
                  static_cast<unsigned long>(seg.segmentId),
                  static_cast<unsigned long>(hdr.recordCount),
                  static_cast<unsigned long>(latestCheckpoint.recordCount),
                  static_cast<unsigned long>(hdr.lastEventId),
                  static_cast<unsigned long>(latestCheckpoint.lastEventId));
        return false;
    }

    return true;
}

bool StorageManager::_auditSpoolBoot(SpoolBootAuditResult& audit) const {
    audit = {};

    if (_spoolIndexLoadFailed) {
        DLOG_WARN("STORAGE", "Boot audit failed: spool index unavailable");
        audit.repairRequired = true;
        return false;
    }

    const bool generationMatch =
        _storageMetaGeneration != 0U &&
        _eventMetaLoaded &&
        _eventCounterLoaded &&
        _spoolIndex.generation != 0U &&
        _storageMetaGeneration == _spoolIndex.generation &&
        _spoolIndex.generation == _eventMetaGeneration &&
        _spoolIndex.generation == _eventCounterGeneration;
    audit.generationMatch = generationMatch;

    const bool counterImpossible = (_nextEventId == 0U);
    if (counterImpossible) {
        DLOG_WARN("STORAGE", "Boot audit failed: counter impossible nextEventId=0");
        audit.repairRequired = true;
        return false;
    }

    const bool dirtySnapshot =
        _pendingCountDirty ||
        _spoolSummaryRebuildPending ||
        _workerMetadataDirtyPending ||
        _eventCounterDirty ||
        _spoolIndexDirty;

    audit.counterOk = true;
    if (!generationMatch || dirtySnapshot) {
        audit.snapshotLagged = true;
    }

    // Bypass per-segment file I/O at boot when the backlog is large. Every
    // LittleFS.open() inside this loop is a sequential I/O, and on this
    // device the filesystem wedges (silent crash, observed at ~iteration 32-40)
    // when we open dozens of segment files back-to-back during early boot.
    // Defer the actual segment audit to maintenance, which runs in a paced
    // task with vTaskDelay between files. Boot proceeds in repair_required
    // mode either way, so this only changes WHEN the work happens.
    constexpr size_t BOOT_AUDIT_SEGMENT_LIMIT = 20U;
    if (_spoolIndex.segments.size() > BOOT_AUDIT_SEGMENT_LIMIT) {
        if (generationMatch && !dirtySnapshot) {
            const SpoolSegmentInfo* active =
                _findSegmentInfo(_spoolIndex.activeSegmentId);
            if (active && !_isSegmentQuarantined(active->segmentId)) {
                SpoolBin::SegmentHeaderV2 hdr;
                if (!_auditSpoolBinaryHeader(*active, hdr)) {
                audit.repairRequired = true;
                return false;
            }

            audit.auditedSegments = 1;
            audit.indexedRecords = active->recordCount;
            audit.headerRecords = hdr.recordCount;
            audit.headerAuditOk = true;

                if (active->recordCount != hdr.recordCount ||
                    active->firstEventId != hdr.firstEventId ||
                    active->lastEventId != hdr.lastEventId) {
                    audit.snapshotLagged = true;
                    audit.indexBehind = true;
                    audit.repairRequired = true;
                    DLOG_WARN("STORAGE",
                              "Boot audit: active segment header ahead of clean index seg=%lu idxRecords=%lu hdrRecords=%lu idxLast=%lu hdrLast=%lu",
                              static_cast<unsigned long>(active->segmentId),
                              static_cast<unsigned long>(active->recordCount),
                              static_cast<unsigned long>(hdr.recordCount),
                              static_cast<unsigned long>(active->lastEventId),
                              static_cast<unsigned long>(hdr.lastEventId));
                    return false;
                }

                if (!_auditSpoolBinaryCheckpointTail(*active, hdr)) {
                    audit.snapshotLagged = true;
                    audit.repairRequired = true;
                    DLOG_WARN("STORAGE",
                              "Boot audit checkpoint required active seg=%lu",
                              static_cast<unsigned long>(active->segmentId));
                    return false;
                }

                audit.checkpointAuditOk = true;
                return true;
            }
        }
#if BOOT_SEQUENCE_VERBOSE_ACTIVE
        Serial.printf("[STORAGE_BOOT] boot_audit_skip segments=%u limit=%u deferred_to_maintenance\r\n",
                      static_cast<unsigned>(_spoolIndex.segments.size()),
                      static_cast<unsigned>(BOOT_AUDIT_SEGMENT_LIMIT));
        Serial.flush();
#endif
        audit.repairRequired = true;
        return false;
    }

    bool indexBehind = false;
    uint32_t failedSegments = 0;
    for (const auto& seg : _spoolIndex.segments) {
        if (seg.segmentId == 0U) {
            continue;
        }

        // Per-segment progress breadcrumb. Encoded as 21000 + (segId & 0xFFFF) in
        // the STORAGE_BOOT phase 'pending' slot so a panic/WDT mid-loop pinpoints
        // the offending segment on next boot. Volatile (RTC only) — avoids 1 NVS
        // write per segment for a recovery path that already crashes on power loss.
        crashCheckpointStep(CrashPhase::STORAGE_BOOT, 0,
                            21000U + (seg.segmentId & 0xFFFFU), false);
#if BOOT_SEQUENCE_VERBOSE_ACTIVE
        Serial.printf("[STORAGE_BOOT] step=%lu audit_seg seg=%lu records=%lu\r\n",
                      static_cast<unsigned long>(21000U + (seg.segmentId & 0xFFFFU)),
                      static_cast<unsigned long>(seg.segmentId),
                      static_cast<unsigned long>(seg.recordCount));
#endif

        if (_isSegmentQuarantined(seg.segmentId)) {
            audit.snapshotLagged = true;
            yield();
            continue;
        }

        SpoolBin::SegmentHeaderV2 hdr;
        if (!_auditSpoolBinaryHeader(seg, hdr)) {
            audit.repairRequired = true;
            failedSegments++;
            yield();
            continue;
        }

        audit.auditedSegments++;
        audit.indexedRecords += seg.recordCount;
        audit.headerRecords += hdr.recordCount;

        if (seg.recordCount > hdr.recordCount ||
            (hdr.firstEventId != 0U && seg.firstEventId > hdr.firstEventId) ||
            (hdr.lastEventId != 0U && seg.lastEventId > hdr.lastEventId)) {
            DLOG_WARN("STORAGE",
                      "Boot audit index ahead seg=%lu idxRecords=%lu hdrRecords=%lu idxFirst=%lu hdrFirst=%lu idxLast=%lu hdrLast=%lu",
                      static_cast<unsigned long>(seg.segmentId),
                      static_cast<unsigned long>(seg.recordCount),
                      static_cast<unsigned long>(hdr.recordCount),
                      static_cast<unsigned long>(seg.firstEventId),
                      static_cast<unsigned long>(hdr.firstEventId),
                      static_cast<unsigned long>(seg.lastEventId),
                      static_cast<unsigned long>(hdr.lastEventId));
            audit.repairRequired = true;
            failedSegments++;
            yield();
            continue;
        }

        if (seg.recordCount != hdr.recordCount ||
            seg.firstEventId != hdr.firstEventId ||
            seg.lastEventId != hdr.lastEventId) {
            indexBehind = true;
            audit.indexBehind = true;
        }

        if (!_auditSpoolBinaryCheckpointTail(seg, hdr)) {
            DLOG_WARN("STORAGE",
                      "Boot audit checkpoint required seg=%lu",
                      static_cast<unsigned long>(seg.segmentId));
            audit.repairRequired = true;
            failedSegments++;
            yield();
            continue;
        }

        audit.checkpointAuditOk = true;
        yield();
    }

    audit.headerAuditOk = true;
    audit.checkpointAuditOk = true;
    if (indexBehind || !generationMatch || dirtySnapshot || _hasInvalidSpoolSummaries()) {
        audit.snapshotLagged = true;
    }

    if (indexBehind) {
        DLOG_WARN("STORAGE",
                  "Boot audit: segment headers ahead of index; repair required audited=%lu",
                  static_cast<unsigned long>(audit.auditedSegments));
        audit.repairRequired = true;
        return false;
    }

    if (failedSegments > 0U) {
        DLOG_WARN("STORAGE",
                  "Boot audit: %lu segment(s) need repair (continuing into repair_required mode)",
                  static_cast<unsigned long>(failedSegments));
        return false;
    }

    return true;
}

uint32_t StorageManager::_activeSessionWatermark(const String& sessionId) const {
    if (!sessionId.length()) return 0;
    return _uploadedWatermarkForSession(sessionId);
}

void StorageManager::beginHotPathDiagnosticsSuppressed() {
    _suppressHotPathDiagnostics = true;
}

void StorageManager::endHotPathDiagnosticsSuppressed() {
    _suppressHotPathDiagnostics = false;
}

void StorageManager::_logSpoolDiagnostics(const char* reason,
                                          const String& sessionId,
                                          bool includeRuntimeCounters) const {
    if (_suppressHotPathDiagnostics &&
        reason &&
        (strcmp(reason, "append_enrich_batch") == 0 ||
         strcmp(reason, "append_enrich_rotate") == 0)) {
        if (isPendingEventCountAuthoritative()) {
            DLOG_INFO("STORAGE",
                      "Spool diag[%s] suppressed hot_path activeSegment=%lu pendingUpload=%lu",
                      reason,
                      static_cast<unsigned long>(_spoolIndex.activeSegmentId),
                      static_cast<unsigned long>(_spoolIndex.pendingTotal));
        } else {
            DLOG_INFO("STORAGE",
                      "Spool diag[%s] suppressed hot_path activeSegment=%lu pendingUpload=unknown",
                      reason,
                      static_cast<unsigned long>(_spoolIndex.activeSegmentId));
        }
        return;
    }

    (void)sessionId;

    uint32_t totalRecords = getStoredRecordCount();
    uint32_t totalEvents = 0;
    uint32_t validSegments = 0;
    uint32_t untrustedSegments = 0;
    uint32_t invalidSegments = 0;

    for (const auto& seg : _spoolIndex.segments) {
        switch (seg.trustState) {
            case SPOOL_SEGMENT_TRUSTED:
                validSegments++;
                totalEvents += seg.eventCount;
                break;
            case SPOOL_SEGMENT_UNTRUSTED:
                untrustedSegments++;
                break;
            case SPOOL_SEGMENT_INVALID:
            default:
                invalidSegments++;
                break;
        }
    }

    const bool backlogTrusted = isPendingEventCountAuthoritative();
    if (includeRuntimeCounters) {
        if (backlogTrusted) {
            DLOG_INFO("STORAGE",
                      "Spool diag[%s] validSegments=%u untrustedSegments=%u invalidSegments=%u activeSegment=%lu totalRecords=%lu storedEvents=%lu pendingUpload=%lu nextEventId=%lu",
                      reason ? reason : "?",
                      static_cast<unsigned>(validSegments),
                      static_cast<unsigned>(untrustedSegments),
                      static_cast<unsigned>(invalidSegments),
                      static_cast<unsigned long>(_spoolIndex.activeSegmentId),
                      static_cast<unsigned long>(totalRecords),
                      static_cast<unsigned long>(totalEvents),
                      static_cast<unsigned long>(_spoolIndex.pendingTotal),
                      static_cast<unsigned long>(_nextEventId));
        } else {
            const char* backlogStateText =
                (getBacklogTrustState() == BACKLOG_DEGRADED) ? "degraded" : "unknown";
            DLOG_INFO("STORAGE",
                      "Spool diag[%s] validSegments=%u untrustedSegments=%u invalidSegments=%u activeSegment=%lu totalRecords=%lu storedEvents=%lu pendingUpload=unknown nextEventId=%lu backlog=%s",
                      reason ? reason : "?",
                      static_cast<unsigned>(validSegments),
                      static_cast<unsigned>(untrustedSegments),
                      static_cast<unsigned>(invalidSegments),
                      static_cast<unsigned long>(_spoolIndex.activeSegmentId),
                      static_cast<unsigned long>(totalRecords),
                      static_cast<unsigned long>(totalEvents),
                      static_cast<unsigned long>(_nextEventId),
                      backlogStateText);
        }
    } else {
        DLOG_INFO("STORAGE",
                  "Spool diag[%s] validSegments=%u untrustedSegments=%u invalidSegments=%u activeSegment=%lu totalRecords=%lu storedEvents=%lu",
                  reason ? reason : "?",
                  static_cast<unsigned>(validSegments),
                  static_cast<unsigned>(untrustedSegments),
                  static_cast<unsigned>(invalidSegments),
                  static_cast<unsigned long>(_spoolIndex.activeSegmentId),
                  static_cast<unsigned long>(totalRecords),
                  static_cast<unsigned long>(totalEvents));
    }

    _logBinaryUnsupportedAudit(reason);
}

String StorageManager::_today() {
    return TIME_SVC.dayStampForMillis(millis());
}

String StorageManager::_spoolDir() const {
    return SpoolPaths::dir();
}

String StorageManager::_spoolIndexPath() const {
    return SpoolPaths::indexPath();
}

String StorageManager::_spoolSegmentPath(uint32_t segmentId) const {
    return SpoolPaths::segmentPath(segmentId);
}

String StorageManager::_spoolBinarySegmentPath(uint32_t segmentId) const {
    return SpoolPaths::binarySegmentPath(segmentId);
}

String StorageManager::_uploadIndexPath(uint32_t segmentId) const {
    return SpoolPaths::uploadIndexPath(segmentId);
}

String StorageManager::_spoolSegmentPathForFormat(uint32_t segmentId, uint8_t format) const {
    return SpoolPaths::segmentPathForFormat(segmentId, format);
}

SpoolSegmentInfo* StorageManager::_findSegmentInfo(uint32_t segmentId) {
    for (auto& seg : _spoolIndex.segments) {
        if (seg.segmentId == segmentId) return &seg;
    }
    return nullptr;
}

const SpoolSegmentInfo* StorageManager::_findSegmentInfo(uint32_t segmentId) const {
    for (const auto& seg : _spoolIndex.segments) {
        if (seg.segmentId == segmentId) return &seg;
    }
    return nullptr;
}

void StorageManager::_rememberSession(const String& sessionId) {
    if (!sessionId.length()) return;
    for (const auto& s : _spoolIndex.sessions) {
        if (s == sessionId) return;
    }
    _spoolIndex.sessions.push_back(sessionId);
}

uint32_t StorageManager::_uploadedWatermarkForSession(const String& sessionId) const {
    if (!sessionId.length()) return 0;
    for (const auto& entry : _spoolIndex.uploadedWatermarks) {
        if (entry.first == sessionId) return entry.second;
    }
    return 0;
}

void StorageManager::_setUploadedWatermarkForSession(const String& sessionId, uint32_t eventId) {
    if (!sessionId.length()) return;

    for (auto& entry : _spoolIndex.uploadedWatermarks) {
        if (entry.first == sessionId) {
            if (eventId > entry.second) {
                entry.second = eventId;
            }
            return;
        }
    }

    _spoolIndex.uploadedWatermarks.push_back({sessionId, eventId});
}

uint32_t StorageManager::rewindUploadWatermarks(const char* sessionIdOverride,
                                                uint32_t toEventId) {
    if (!_ready) return 0;

    if (_uploadBatchActive) {
        DLOG_WARN("STORAGE",
                  "Upload watermark rewind refused; upload batch active");
        return 0;
    }

    const String target =
        (sessionIdOverride && sessionIdOverride[0]) ? String(sessionIdOverride)
                                                    : String();

    // Collect first: _restoreUploadedWatermarkForSession() erases the entry
    // when toEventId is 0, which would invalidate an in-flight iterator.
    std::vector<String> rewindSessions;
    for (const auto& entry : _spoolIndex.uploadedWatermarks) {
        if (target.length() && entry.first != target) continue;
        if (entry.second <= toEventId) continue;
        rewindSessions.push_back(entry.first);
    }

    for (const String& sessionId : rewindSessions) {
        const uint32_t before = _uploadedWatermarkForSession(sessionId);
        _restoreUploadedWatermarkForSession(sessionId, toEventId);
        DLOG_INFO("STORAGE",
                  "Upload watermark rewound session=%s %lu -> %lu",
                  sessionId.c_str(),
                  static_cast<unsigned long>(before),
                  static_cast<unsigned long>(toEventId));
    }

    if (rewindSessions.empty()) {
        return 0;
    }

    _persistSpoolIndex(true, "upload_watermark_rewind");

    // The resident index caches nothing derived from the watermark, but the
    // dump reads its per-session start cursor from getLastUploadedEventId()
    // once per session. Drop the index so the next upload rebuilds cleanly
    // against the rewound watermarks.
    _releaseUploadIndexMemory("upload_watermark_rewind");

    // _pendingEventCount is a maintained counter that was decremented as these
    // records uploaded; the rewind does not put them back. Recount so the
    // pending total matches the watermarks again — otherwise the upload
    // trigger sees no work and refuses to re-send anything.
    const uint32_t pendingAfter = recountPendingFromSpool();

    DLOG_INFO("STORAGE",
              "Upload watermark rewind complete sessions=%u to=%lu pending=%lu",
              static_cast<unsigned>(rewindSessions.size()),
              static_cast<unsigned long>(toEventId),
              static_cast<unsigned long>(pendingAfter));

    return static_cast<uint32_t>(rewindSessions.size());
}

void StorageManager::_restoreUploadedWatermarkForSession(const String& sessionId, uint32_t eventId) {
    if (!sessionId.length()) return;

    for (auto it = _spoolIndex.uploadedWatermarks.begin();
         it != _spoolIndex.uploadedWatermarks.end();
         ++it) {
        if (it->first == sessionId) {
            if (eventId == 0U) {
                _spoolIndex.uploadedWatermarks.erase(it);
            } else {
                it->second = eventId;
            }
            return;
        }
    }

    if (eventId != 0U) {
        _spoolIndex.uploadedWatermarks.push_back({sessionId, eventId});
    }
}

bool StorageManager::_persistSpoolIndex(bool force, const char* reason) {
    // The no-write rule applies only while the dump is actively publishing
    // (_uploadBatchActive). Post-dump cleanup (endUploadBatch -> compactSpool ->
    // rotate) deliberately runs while the upload lease is still held so capture
    // doesn't resume before deferred watermarks flush (see MQTTManager MQTT_DONE),
    // so those persists are legitimate even though owner==WIFI_UPLOAD.
    CONTRACT_WARN_ONCE(CONTRACT_NO_FS_WRITE_DURING_UPLOAD_EXCEPT_CHECKPOINT,
                       "STORAGE",
                       !RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD) ||
                           !_uploadBatchActive ||
                           (reason && strcmp(reason, "upload_checkpoint") == 0),
                       "spool_index reason=%s",
                       (reason && reason[0]) ? reason : "-");
    _spoolIndexDirty = true;
    if (_spoolIndexPendingWrites < 0xFFFFu) {
        _spoolIndexPendingWrites++;
    }

    if (_uploadBatchActive && !force) {
        _uploadBatchDirty = true;
        return true;
    }

    const bool dueByCount =
        _spoolIndexPendingWrites >= STORAGE_SPOOL_INDEX_SAVE_EVERY_N;
    const bool dueByTime =
        millis() - _lastSpoolIndexSaveMs >= STORAGE_HOT_META_SAVE_INTERVAL_MS;
    if (!force && !dueByCount && !dueByTime) {
        return true;
    }

    JsonDocument doc;
    doc["version"] = _spoolIndex.version;
    const uint32_t generation = _storageMetaGeneration == 0 ? 1U : _storageMetaGeneration;
    _storageMetaGeneration = generation;
    doc["generation"] = generation;
    doc["spool_format"] = _spoolIndex.format;
    doc["next_segment_id"] = _spoolIndex.nextSegmentId;
    doc["active_segment_id"] = _spoolIndex.activeSegmentId;
    doc["oldest_segment_id"] = _spoolIndex.oldestSegmentId;
    doc["pending_total"] = _spoolIndex.pendingTotal;
    const uint32_t nextEventId =
        _spoolIndex.nextEventId != 0U ? _spoolIndex.nextEventId :
        (_nextEventId != 0U ? _nextEventId : 1U);
    _spoolIndex.nextEventId = nextEventId;
    doc["next_event_id"] = nextEventId;

    JsonArray sessions = doc["sessions"].to<JsonArray>();
    for (const auto& sessionId : _spoolIndex.sessions) {
        sessions.add(sessionId);
    }

    JsonArray watermarks = doc["uploaded_watermarks"].to<JsonArray>();
    for (const auto& entry : _spoolIndex.uploadedWatermarks) {
        JsonObject o = watermarks.add<JsonObject>();
        o["session_id"] = entry.first;
        o["event_id"] = entry.second;
    }

    JsonArray segs = doc["segments"].to<JsonArray>();
    for (const auto& seg : _spoolIndex.segments) {
        JsonObject o = segs.add<JsonObject>();
        o["segment_id"] = seg.segmentId;
        o["first_event_id"] = seg.firstEventId;
        o["last_event_id"] = seg.lastEventId;
        o["sv"] = seg.summaryVersion;
        o["sok"] = seg.summaryValid ? 1 : 0;
        o["record_count"] = seg.recordCount;
        o["ev"] = seg.eventCount;
        o["en"] = seg.enrichDeltaCount;
        o["mi"] = seg.missionCount;
        o["no"] = seg.noiseCount;
        o["pum"] = seg.pendingUploadMissionCount;
        o["pun"] = seg.pendingUploadNoiseCount;
        o["pe"] = seg.pendingEnrichmentCount;
        o["p0"] = seg.p0Count;
        o["p1"] = seg.p1Count;
        o["p2"] = seg.p2Count;
        o["p3"] = seg.p3Count;
        o["t0"] = seg.minTimestampMs;
        o["t1"] = seg.maxTimestampMs;
        o["approx_bytes"] = seg.approxBytes;
        o["format"] = seg.format;
        o["trust"] = seg.trustState;
        o["life"] = seg.lifecycle;
    }

    const String indexPath = _spoolIndexPath();
    const uint32_t writeStartMs = millis();
    const bool ok = _atomicWriteFile(
        indexPath,
        [&](fs::File& f) -> bool {
            return serializeJson(doc, f) > 0U;
        },
        true);
    if (!ok) return false;

    _lastSpoolIndexSaveMs = millis();
    _spoolIndexPendingWrites = 0;
    _spoolIndexDirty = false;
    _logCaptureWriteAllowed(indexPath.c_str(),
                            reason,
                            millis() - writeStartMs,
                            true);
    return true;
}

bool StorageManager::_loadSpoolIndex() {
    _spoolIndex = {};
    _spoolIndexLoadFailed = false;
    auto loadIndex = [&](const String& indexPath) -> bool {
        File f = LittleFS.open(indexPath, "r");
        if (!f) {
            return false;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err || !doc.is<JsonObject>()) {
            return false;
        }

        _spoolIndex.version = doc["version"] | 1U;
        _spoolIndex.generation = doc["generation"] | 0U;
        _spoolIndex.format = static_cast<uint8_t>(doc["spool_format"] | SPOOL_SEGMENT_JSONL);
        _spoolIndex.nextSegmentId = doc["next_segment_id"] | 1U;
        _spoolIndex.activeSegmentId = doc["active_segment_id"] | 0U;
        _spoolIndex.oldestSegmentId = doc["oldest_segment_id"] | 0U;
        _spoolIndex.pendingTotal = doc["pending_total"] | 0U;
        _spoolIndex.nextEventId = doc["next_event_id"] | 0U;

        JsonArray sessions = doc["sessions"].as<JsonArray>();
        for (JsonVariant v : sessions) {
            const char* sid = v | "";
            if (sid && sid[0]) {
                _spoolIndex.sessions.push_back(String(sid));
            }
        }

        JsonArray watermarks = doc["uploaded_watermarks"].as<JsonArray>();
        for (JsonObject o : watermarks) {
            const char* sid = o["session_id"] | "";
            const uint32_t eid = o["event_id"] | 0U;
            if (sid && sid[0]) {
                _spoolIndex.uploadedWatermarks.push_back({String(sid), eid});
            }
        }

        JsonArray segs = doc["segments"].as<JsonArray>();
        for (JsonObject segObj : segs) {
            SpoolSegmentInfo seg;
            seg.segmentId = segObj["segment_id"] | 0U;
            seg.firstEventId = segObj["first_event_id"] | 0U;
            seg.lastEventId = segObj["last_event_id"] | 0U;
            seg.summaryVersion = static_cast<uint16_t>(segObj["sv"] | 0U);
            const bool hasPendingEnrichField = !segObj["pe"].isNull();
            seg.summaryValid = ((segObj["sok"] | 0U) != 0U) &&
                               (seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION) &&
                               hasPendingEnrichField;
            seg.recordCount = segObj["record_count"] | 0U;
            seg.eventCount = segObj["ev"] | 0U;
            seg.enrichDeltaCount = segObj["en"] | 0U;
            seg.missionCount = segObj["mi"] | 0U;
            seg.noiseCount = segObj["no"] | 0U;
            seg.pendingUploadMissionCount = segObj["pum"] | 0U;
            seg.pendingUploadNoiseCount = segObj["pun"] | 0U;
            seg.pendingEnrichmentCount = segObj["pe"] | 0U;
            seg.p0Count = segObj["p0"] | 0U;
            seg.p1Count = segObj["p1"] | 0U;
            seg.p2Count = segObj["p2"] | 0U;
            seg.p3Count = segObj["p3"] | 0U;
            seg.minTimestampMs = segObj["t0"] | 0U;
            seg.maxTimestampMs = segObj["t1"] | 0U;
            seg.approxBytes = segObj["approx_bytes"] | 0U;
            seg.format = static_cast<uint8_t>(segObj["format"] | _spoolIndex.format);
            seg.trustState = static_cast<uint8_t>(
                segObj["trust"] | (seg.summaryValid ? SPOOL_SEGMENT_TRUSTED
                                                    : SPOOL_SEGMENT_UNTRUSTED));
            seg.lifecycle = static_cast<uint8_t>(
                segObj["life"] | SPOOL_SEGMENT_SEALED);
            _refreshSegmentLifecycle(seg);
            if (seg.segmentId != 0) {
                _spoolIndex.segments.push_back(seg);
            }
        }

        _lastSpoolIndexSaveMs = millis();
        _spoolIndexPendingWrites = 0;
        _spoolIndexDirty = false;

        return true;
    };

    const String indexPath = _spoolIndexPath();
    const String backupPath = indexPath + ".bak";
    const bool primaryExists = LittleFS.exists(indexPath);

    if (loadIndex(indexPath)) {
        return true;
    }

    if (primaryExists) {
        DLOG_WARN("STORAGE", "Spool index primary parse failed; trying backup");
    } else {
        DLOG_WARN("STORAGE", "Spool index primary missing; trying backup");
    }

    if (loadIndex(backupPath)) {
        DLOG_INFO("STORAGE", "Spool index backup accepted");
        DLOG_INFO("STORAGE", "Counter trust=trusted reason=spool_index_backup");
        return true;
    }

    _spoolIndexLoadFailed = true;
    _setCounterTrustState(STORAGE_COUNTER_REPAIR_REQUIRED, "spool_index_repair_required");
    return false;
}

bool StorageManager::_validateUploadIndexRecord(const UploadIndexRecordV1& rec,
                                                uint32_t expectedSegmentId) const {
    if (rec.magic != UIX_MAGIC) return false;
    if (rec.version != 1) return false;
    if (rec.segmentId != expectedSegmentId) return false;
    if (rec.recordLen == 0 || rec.recordLen != UIX_RECORD_LEN_V1) return false;
    if (rec.len == 0) return false;
    if (rec.lane > static_cast<uint8_t>(STORAGE_LANE_NOISE)) return false;
    if (rec.priority > static_cast<uint8_t>(STORAGE_PRIO_P3)) return false;
    return rec.crc == _uploadIndexRecordHash(rec);
}

bool StorageManager::_addUploadPtrToMemory(const UploadIndexRecordV1& rec) {
    if (!rec.sessionId[0]) {
        return true;
    }

    const String sessionId(rec.sessionId);
    const bool newSession = _backlog.uploadIndexBySession.find(sessionId) == _backlog.uploadIndexBySession.end();
    UploadIndexPagedSession& session = _backlog.uploadIndexBySession[sessionId];

    if (session.pages.empty() ||
        session.pages.back()->count >= UPLOAD_INDEX_PAGE_CAPACITY) {
        std::unique_ptr<UploadIndexPage> page(new (std::nothrow) UploadIndexPage());
        if (!page) {
            DLOG_WARN("STORAGE",
                      "Upload index page alloc failed session=%s count=%lu",
                      sessionId.c_str(),
                      static_cast<unsigned long>(session.count));
            if (newSession && session.count == 0 && session.pages.empty()) {
                _backlog.uploadIndexBySession.erase(sessionId);
            }
            return false;
        }
        session.pages.push_back(std::move(page));
    }

    UploadIndexPage* page = session.pages.back().get();
    page->records[page->count++] = rec;
    session.count++;

    for (const auto& existing : _backlog.uploadIndexSessions) {
        if (existing == sessionId) {
            return true;
        }
    }
    _backlog.uploadIndexSessions.push_back(sessionId);
    return true;
}

void StorageManager::_releaseUploadIndexMemory(const char* reason) {
    const uint32_t indexed = _backlog.uploadIndexStats.indexedEvents;
    const uint32_t sessions = static_cast<uint32_t>(_backlog.uploadIndexSessions.size());
    const bool hadResidentState =
        _backlog.uploadIndexResident ||
        indexed != 0 ||
        sessions != 0 ||
        !_backlog.uploadIndexBySession.empty();

    _closeUploadSegmentFile(reason);

    std::map<String, UploadIndexPagedSession>().swap(_backlog.uploadIndexBySession);
    UploadEnrichBySessionMap().swap(_backlog.uploadEnrichBySession);
    std::vector<String>().swap(_backlog.uploadIndexSessions);
    _backlog.uploadIndexStats = {};
    _backlog.uploadIndexResident = false;
    _backlog.uploadIndexWindowLimit = 0;
    _backlog.uploadIndexWindowTruncated = false;

    if (hadResidentState) {
        DLOG_INFO("STORAGE",
                  "Upload index released reason=%s indexed=%lu sessions=%lu",
                  (reason && reason[0]) ? reason : "-",
                  static_cast<unsigned long>(indexed),
                  static_cast<unsigned long>(sessions));
    }
}

void StorageManager::closeUploadReadFile() {
    _closeUploadSegmentFile("public_close");
}

void StorageManager::releaseUploadIndexMemory(const char* reason) {
    _releaseUploadIndexMemory(reason);
}

void StorageManager::_closeUploadSegmentFile(const char* /*reason*/) {
    if (_backlog.uploadReadFile) {
        _backlog.uploadReadFile.close();
    }
    _backlog.uploadReadSegmentId = 0;
    _backlog.uploadReadFileSize = 0;
    _backlog.uploadReadFormat = 0;
    _backlog.uploadReadHeader = SpoolBin::SegmentHeaderV2{};
    _backlog.uploadReadHeaderOk = false;
}

bool StorageManager::_ensureUploadSegmentFileOpen(uint32_t segmentId,
                                                  uint8_t format,
                                                  const String& path) {
    if (_backlog.uploadReadFile && _backlog.uploadReadSegmentId == segmentId) {
        return true;
    }

    if (_backlog.uploadReadFile) {
        _backlog.uploadReadFile.close();
        _backlog.uploadReadHeaderOk = false;
    }

    _backlog.uploadReadFile = LittleFS.open(path, "r");
    if (!_backlog.uploadReadFile) {
        _backlog.uploadReadSegmentId = 0;
        _backlog.uploadReadFileSize = 0;
        _backlog.uploadReadFormat = 0;
        _backlog.uploadReadHeader = SpoolBin::SegmentHeaderV2{};
        _backlog.uploadReadHeaderOk = false;
        return false;
    }

    _backlog.uploadReadSegmentId = segmentId;
    _backlog.uploadReadFileSize = static_cast<uint32_t>(_backlog.uploadReadFile.size());
    _backlog.uploadReadFormat = format;
    _backlog.uploadReadHeader = SpoolBin::SegmentHeaderV2{};

    if (format == SPOOL_SEGMENT_BIN_V2) {
        _backlog.uploadReadHeaderOk =
            SpoolBin::readSegmentHeaderV2(_backlog.uploadReadFile, _backlog.uploadReadHeader) &&
            _backlog.uploadReadHeader.magic == SpoolBin::SEGMENT_MAGIC &&
            _backlog.uploadReadHeader.version == 2;
    } else {
        // Legacy JSON segments don't have a binary header; consider OK.
        _backlog.uploadReadHeaderOk = true;
    }

    return true;
}

bool StorageManager::_loadUploadIndexSidecarSegment(const SpoolSegmentInfo& seg) {
    if (seg.segmentId == 0) {
        return true;
    }

    const uint32_t pendingUpload =
        seg.pendingUploadMissionCount + seg.pendingUploadNoiseCount;
    if (pendingUpload == 0) {
        return true;
    }

    const String indexPath = _uploadIndexPath(seg.segmentId);
    if (!LittleFS.exists(indexPath)) {
        return false;
    }

    File indexFile = LittleFS.open(indexPath, "r");
    if (!indexFile) {
        return false;
    }

    const size_t indexSize = indexFile.size();
    if (indexSize == 0 ||
        (indexSize % sizeof(UploadIndexRecordV1)) != 0) {
        indexFile.close();
        DLOG_WARN("STORAGE",
                  "Upload index sidecar stale seg=%lu reason=size path=%s",
                  static_cast<unsigned long>(seg.segmentId),
                  indexPath.c_str());
        requestMaintenance(STORAGE_MAINT_DIRTY_SPOOL_INDEX,
                           "upload_index_bad_sidecar_size");
        return false;
    }

    uint32_t maxIndexedEventId = 0;
    uint32_t recordsRead = 0;
    UploadIndexRecordV1 rec;
    while (indexFile.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) ==
           sizeof(rec)) {
        if ((recordsRead & 0x3FU) == 0x3FU) {
            vTaskDelay(1);
        }
        if (!_validateUploadIndexRecord(rec, seg.segmentId) ||
            rec.sessionId[0] == '\0') {
            indexFile.close();
            DLOG_WARN("STORAGE",
                      "Upload index sidecar stale seg=%lu reason=record path=%s",
                      static_cast<unsigned long>(seg.segmentId),
                      indexPath.c_str());
            requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                               "upload_index_bad_sidecar_record");
            return false;
        }

        if (rec.eventId > maxIndexedEventId) {
            maxIndexedEventId = rec.eventId;
        }
        recordsRead++;
    }

    if (recordsRead == 0) {
        indexFile.close();
        return false;
    }

    if (pendingUpload > 0 &&
        seg.lastEventId != 0 &&
        maxIndexedEventId < seg.lastEventId) {
        indexFile.close();
        DLOG_WARN("STORAGE",
                  "Upload index sidecar stale seg=%lu reason=coverage max=%lu last=%lu pending=%lu",
                  static_cast<unsigned long>(seg.segmentId),
                  static_cast<unsigned long>(maxIndexedEventId),
                  static_cast<unsigned long>(seg.lastEventId),
                  static_cast<unsigned long>(pendingUpload));
        requestMaintenance(STORAGE_MAINT_DIRTY_SPOOL_INDEX,
                           "upload_index_sidecar_coverage");
        return false;
    }

    if (!indexFile.seek(0)) {
        indexFile.close();
        return false;
    }

    const uint32_t windowLimit = _backlog.uploadIndexWindowLimit;
    uint32_t loaded = 0;
    uint32_t recordsVisited = 0;
    while (indexFile.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) ==
           sizeof(rec)) {
        if ((recordsVisited++ & 0x3FU) == 0x3FU) {
            vTaskDelay(1);
        }
        if (windowLimit > 0 &&
            _backlog.uploadIndexStats.indexedEvents >= windowLimit) {
            _backlog.uploadIndexWindowTruncated = true;
            break;
        }

        const String sessionId(rec.sessionId);
        if (rec.eventId <= _uploadedWatermarkForSession(sessionId)) {
            continue;
        }

        if (!_addUploadPtrToMemory(rec)) {
            indexFile.close();
            return false;
        }
        _backlog.uploadIndexStats.indexedEvents++;
        loaded++;
    }

    indexFile.close();

    DLOG_DEBUG("STORAGE",
               "Upload index sidecar loaded seg=%lu records=%lu loaded=%lu indexed=%lu",
               static_cast<unsigned long>(seg.segmentId),
               static_cast<unsigned long>(recordsRead),
               static_cast<unsigned long>(loaded),
               static_cast<unsigned long>(_backlog.uploadIndexStats.indexedEvents));
    return true;
}

bool StorageManager::_rebuildUploadIndexSegment(const SpoolSegmentInfo& seg) {
    if (seg.segmentId == 0) {
        return true;
    }

    const uint32_t windowLimit = _backlog.uploadIndexWindowLimit;

    auto uploadWindowFull = [&]() -> bool {
        return windowLimit > 0 &&
               _backlog.uploadIndexStats.indexedEvents >= windowLimit;
    };

    if (uploadWindowFull()) {
        _backlog.uploadIndexWindowTruncated = true;
        return true;
    }

    SpoolSegmentInfo* mutableSeg = _findSegmentInfo(seg.segmentId);
    const bool isActiveSegment = seg.segmentId == _spoolIndex.activeSegmentId;
    const uint32_t oldPendingMission =
        mutableSeg ? mutableSeg->pendingUploadMissionCount : 0U;
    const uint32_t oldPendingNoise =
        mutableSeg ? mutableSeg->pendingUploadNoiseCount : 0U;
    uint32_t indexedPendingMission = 0;
    uint32_t indexedPendingNoise = 0;
    bool segmentHadIssue = false;
    bool segmentStructuralStop = false;
    bool segmentWindowTruncated = false;

    auto flagSegmentIssue = [&](const char* detail, bool structural) {
        segmentHadIssue = true;
        segmentStructuralStop = segmentStructuralStop || structural;
        requestMaintenance(structural ? STORAGE_MAINT_SEGMENT_AUDIT
                                      : STORAGE_MAINT_DIRTY_SUMMARY,
                           detail);
    };

    auto applySalvageAccounting = [&](const char* detail) {
        DLOG_WARN("STORAGE",
                  "Upload index saw segment issue seg=%lu structural=%u reason=%s; queued maintenance",
                  static_cast<unsigned long>(seg.segmentId),
                  segmentStructuralStop ? 1U : 0U,
                  (detail && detail[0]) ? detail : "-");
    };

    auto reconcileIndexedCounters = [&]() {
        if (!mutableSeg || isActiveSegment || segmentWindowTruncated) {
            return;
        }

        if (mutableSeg->pendingUploadMissionCount == indexedPendingMission &&
            mutableSeg->pendingUploadNoiseCount == indexedPendingNoise) {
            return;
        }

        requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                           "upload_index_counter_drift");

        DLOG_WARN("STORAGE",
                  "Upload index observed segment counter drift seg=%lu old=%lu/%lu scan=%lu/%lu; queued maintenance",
                  static_cast<unsigned long>(seg.segmentId),
                  static_cast<unsigned long>(oldPendingMission),
                  static_cast<unsigned long>(oldPendingNoise),
                  static_cast<unsigned long>(indexedPendingMission),
                  static_cast<unsigned long>(indexedPendingNoise));
    };

    const String indexPath = _uploadIndexPath(seg.segmentId);
    const String tmpIndexPath = indexPath + ".tmp";
    const bool canRefreshSidecar = !isActiveSegment;
    File sidecarTmp;
    bool sidecarTmpOpen = false;
    bool sidecarTmpHasRecords = false;
    bool sidecarTmpOk = false;

    auto abandonSidecarRefresh = [&]() {
        if (sidecarTmpOpen) {
            sidecarTmp.close();
            sidecarTmpOpen = false;
        }
        if (tmpIndexPath.length() && LittleFS.exists(tmpIndexPath)) {
            (void)LittleFS.remove(tmpIndexPath);
        }
    };

    auto commitSidecarRefresh = [&]() {
        if (!canRefreshSidecar || !sidecarTmpOpen) {
            return;
        }
        sidecarTmp.flush();
        sidecarTmp.close();
        sidecarTmpOpen = false;
        if (!sidecarTmpOk || !sidecarTmpHasRecords ||
            segmentHadIssue || segmentWindowTruncated) {
            abandonSidecarRefresh();
            return;
        }
        if (LittleFS.exists(indexPath) && !LittleFS.remove(indexPath)) {
            DLOG_WARN("STORAGE",
                      "Upload index sidecar replace remove failed seg=%lu path=%s",
                      static_cast<unsigned long>(seg.segmentId),
                      indexPath.c_str());
            abandonSidecarRefresh();
            return;
        }
        if (!LittleFS.rename(tmpIndexPath, indexPath)) {
            DLOG_WARN("STORAGE",
                      "Upload index sidecar replace rename failed seg=%lu path=%s",
                      static_cast<unsigned long>(seg.segmentId),
                      indexPath.c_str());
            abandonSidecarRefresh();
        }
    };

    if (canRefreshSidecar) {
        if (LittleFS.exists(tmpIndexPath)) {
            (void)LittleFS.remove(tmpIndexPath);
        }
        sidecarTmp = LittleFS.open(tmpIndexPath, "w");
        sidecarTmpOpen = static_cast<bool>(sidecarTmp);
        sidecarTmpOk = sidecarTmpOpen;
    }

    auto writePtr = [&](uint32_t offset,
                        uint32_t len,
                        uint32_t eventId,
                        const String& sessionId,
                        uint8_t lane,
                        uint8_t priority) -> bool {
        if (eventId == 0 || !sessionId.length()) {
            return true;
        }

        if (uploadWindowFull()) {
            _backlog.uploadIndexWindowTruncated = true;
            segmentWindowTruncated = true;
            return true;
        }

        UploadIndexRecordV1 record;
        memset(&record, 0, sizeof(record));
        record.magic = UIX_MAGIC;
        record.version = 1;
        record.recordLen = UIX_RECORD_LEN_V1;
        record.segmentId = seg.segmentId;
        record.offset = offset;
        record.len = len;
        record.eventId = eventId;
        record.lane = lane;
        record.priority = priority;
        record.reserved1 = 0;
        snprintf(record.sessionId, sizeof(record.sessionId), "%s", sessionId.c_str());
        record.crc = _uploadIndexRecordHash(record);

        if (sidecarTmpOpen && sidecarTmpOk) {
            const size_t written =
                sidecarTmp.write(reinterpret_cast<const uint8_t*>(&record),
                                 sizeof(record));
            if (written == sizeof(record)) {
                sidecarTmpHasRecords = true;
            } else {
                sidecarTmpOk = false;
            }
        }

        if (eventId <= _uploadedWatermarkForSession(sessionId)) {
            return true;
        }

        if (!_addUploadPtrToMemory(record)) {
            return false;
        }
        _backlog.uploadIndexStats.indexedEvents++;
        if (lane == static_cast<uint8_t>(STORAGE_LANE_MISSION)) {
            indexedPendingMission++;
        } else {
            indexedPendingNoise++;
        }
        return true;
    };

    bool ok = true;
    uint32_t scanned = 0;
    uint32_t skipped = 0;
    auto noteSkippedRecord = [&](const char* why,
                                 uint8_t type,
                                 uint32_t len) {
        flagSegmentIssue(why, false);
        skipped++;
        _backlog.uploadIndexStats.skippedRecords++;
        if (skipped <= 4U) {
            DLOG_WARN("STORAGE",
                      "Upload index salvage skipped seg=%lu reason=%s type=%u len=%u skips=%lu",
                      static_cast<unsigned long>(seg.segmentId),
                      (why && why[0]) ? why : "-",
                      static_cast<unsigned>(type),
                      static_cast<unsigned>(len),
                      static_cast<unsigned long>(skipped));
        }
    };

    if (seg.format == SPOOL_SEGMENT_BIN_V2) {
        const String spoolPath = _spoolBinarySegmentPath(seg.segmentId);
        File spool = LittleFS.open(spoolPath, "r");
        if (!spool) {
            flagSegmentIssue("upload_index_segment_open_failed", true);
            applySalvageAccounting("segment_open_failed");
            abandonSidecarRefresh();
            return true;
        }

        SpoolBin::SegmentHeaderV2 hdr;
        if (!SpoolBin::readSegmentHeaderV2(spool, hdr) ||
            hdr.magic != SpoolBin::SEGMENT_MAGIC ||
            hdr.version != 2 ||
            !spool.seek(sizeof(SpoolBin::SegmentHeaderV2))) {
            spool.close();
            flagSegmentIssue("upload_index_header_invalid", true);
            applySalvageAccounting("header_invalid");
            abandonSidecarRefresh();
            return true;
        }

        String lastSession;
        String lastSessionTag;
        BinaryEnrichContext enrichCtx;
        while (spool.position() < spool.size()) {
            if (uploadWindowFull()) {
                _backlog.uploadIndexWindowTruncated = true;
                segmentWindowTruncated = true;
                break;
            }

            const uint32_t offset = static_cast<uint32_t>(spool.position());
            SpoolBin::RecordPrefix prefix;
            if (!SpoolBin::readBytes(spool, &prefix, sizeof(prefix))) {
                flagSegmentIssue("upload_index_prefix_read_failed", true);
                break;
            }

            const size_t remaining = static_cast<size_t>(spool.size() - spool.position());
            if (prefix.length > remaining) {
                flagSegmentIssue("upload_index_truncated_body", true);
                break;
            }

            std::vector<uint8_t> body(prefix.length);
            if (prefix.length > 0 &&
                !SpoolBin::readBytes(spool, body.data(), prefix.length)) {
                flagSegmentIssue("upload_index_body_read_failed", true);
                break;
            }

            if (prefix.type == SpoolBin::REC_CHECKPOINT) {
                scanned++;
                if ((scanned & 0x1FU) == 0U) {
                    delay(1);
                }
                continue;
            }

            if (prefix.type == SpoolBin::REC_ENRICH_DELTA) {
                String sessionCursor = lastSession;
                String sessionTagCursor = lastSessionTag;
                BinaryMetaRecord meta;
                if (_decodeBinaryMetaRecord(body.data(),
                                            body.size(),
                                            prefix.type,
                                            sessionCursor,
                                            sessionTagCursor,
                                            enrichCtx,
                                            meta)) {
                    lastSession = sessionCursor;
                    lastSessionTag = sessionTagCursor;
                } else {
                    noteSkippedRecord("enrich_decode_failed",
                                      prefix.type,
                                      prefix.length);
                    requestMaintenance(STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY,
                                       "upload_index_enrich_decode_skip");
                }
                scanned++;
                if ((scanned & 0x1FU) == 0U) {
                    delay(1);
                }
                continue;
            }

            if (prefix.type != SpoolBin::REC_EVENT) {
                noteSkippedRecord("unknown_record_type",
                                  prefix.type,
                                  prefix.length);
                requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                                   "upload_index_unknown_record_skip");
                scanned++;
                if ((scanned & 0x1FU) == 0U) {
                    delay(1);
                }
                continue;
            }

            const String sessionSeed = lastSession;
            DecodedSpoolRecord rec;
            if (!_decodeBinarySpoolRecordBody(seg.segmentId,
                                              prefix.type,
                                              body.data(),
                                              body.size(),
                                              hdr.createdMs,
                                              hdr.createdEpochUtc,
                                              sessionSeed,
                                              rec)) {
                String sessionCursor = lastSession;
                String sessionTagCursor = lastSessionTag;
                BinaryMetaRecord meta;
                if (_decodeBinaryMetaRecord(body.data(),
                                            body.size(),
                                            prefix.type,
                                            sessionCursor,
                                            sessionTagCursor,
                                            enrichCtx,
                                            meta)) {
                    lastSession = sessionCursor;
                    lastSessionTag = sessionTagCursor;
                }
                noteSkippedRecord("event_decode_failed",
                                  prefix.type,
                                  prefix.length);
                requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                                   "upload_index_event_decode_skip");
                scanned++;
                if ((scanned & 0x1FU) == 0U) {
                    delay(1);
                }
                continue;
            }
            if (rec.sessionId.length()) {
                lastSession = rec.sessionId;
            }

            if (rec.recordType == SPOOL_REC_EVENT) {
                const uint32_t len =
                    static_cast<uint32_t>(sizeof(prefix)) +
                    static_cast<uint32_t>(prefix.length);
                const JsonObjectConst metaDoc = rec.doc.as<JsonObjectConst>();
                const uint8_t lane =
                    static_cast<uint8_t>(_eventRecordLane(metaDoc));
                const uint8_t priority =
                    static_cast<uint8_t>(_eventRecordPriority(metaDoc));
                if (!writePtr(offset,
                              len,
                              rec.eventId,
                              rec.sessionId,
                              lane,
                              priority)) {
                    ok = false;
                    break;
                }
            }

            scanned++;
            if ((scanned & 0x1FU) == 0U) {
                delay(1);
            }
        }

        spool.close();
    } else {
        const String spoolPath = _spoolSegmentPath(seg.segmentId);
        File spool = LittleFS.open(spoolPath, "r");
        if (!spool) {
            flagSegmentIssue("upload_index_jsonl_open_failed", true);
            applySalvageAccounting("jsonl_open_failed");
            abandonSidecarRefresh();
            return true;
        }

        while (spool.available()) {
            if (uploadWindowFull()) {
                _backlog.uploadIndexWindowTruncated = true;
                segmentWindowTruncated = true;
                break;
            }

            const uint32_t offset = static_cast<uint32_t>(spool.position());
            String line = spool.readStringUntil('\n');
            const uint32_t len =
                static_cast<uint32_t>(spool.position()) - offset;
            line.trim();
            if (!line.length()) {
                continue;
            }

            JsonDocument doc;
            if (deserializeJson(doc, line)) {
                noteSkippedRecord("json_decode_failed", 0, len);
                requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                                   "upload_index_json_decode_skip");
                scanned++;
                if ((scanned & 0x1FU) == 0U) {
                    delay(1);
                }
                continue;
            }

            const char* type = doc["type"] | "";
            if (strcmp(type, "enrich_delta") != 0) {
                const uint32_t eventId = doc["id"] | 0U;
                const String sessionId =
                    String((const char*)(doc[F_SESSION] | doc["session_id"] | ""));
                const JsonObjectConst root = doc.as<JsonObjectConst>();
                const uint8_t lane =
                    static_cast<uint8_t>(_eventRecordLane(root));
                const uint8_t priority =
                    static_cast<uint8_t>(_eventRecordPriority(root));
                if (!writePtr(offset,
                              len,
                              eventId,
                              sessionId,
                              lane,
                              priority)) {
                    ok = false;
                    break;
                }
            }

            scanned++;
            if ((scanned & 0x1FU) == 0U) {
                delay(1);
            }
        }

        spool.close();
    }

    if (ok && !segmentHadIssue && !segmentWindowTruncated) {
        commitSidecarRefresh();
    } else {
        abandonSidecarRefresh();
    }

    if (ok && segmentHadIssue) {
        applySalvageAccounting("index_salvage");
    } else if (ok) {
        reconcileIndexedCounters();
    }

    if (!ok) {
        _backlog.uploadIndexStats.failedSegments++;
        DLOG_WARN("STORAGE", "Upload index rebuild failed seg=%lu",
                  static_cast<unsigned long>(seg.segmentId));
    } else if (segmentHadIssue) {
        if (segmentStructuralStop) {
            _backlog.uploadIndexStats.failedSegments++;
        }
        DLOG_WARN("STORAGE",
                  "Upload index salvage accepted seg=%lu skipped=%lu indexed=%lu structural=%u",
                  static_cast<unsigned long>(seg.segmentId),
                  static_cast<unsigned long>(skipped),
                  static_cast<unsigned long>(_backlog.uploadIndexStats.indexedEvents),
                  segmentStructuralStop ? 1U : 0U);
    }

    const bool summaryReady =
        seg.summaryValid &&
        seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;
    if (ok && summaryReady && seg.enrichDeltaCount > 0) {
        const bool enrichOk = _scanSegmentRecords(seg.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                if (rec.recordType != SPOOL_REC_ENRICH_DELTA ||
                    !rec.sessionId.length()) {
                    return true;
                }

                SpoolEnrichmentDelta enrichment;
                enrichment.id = rec.doc["event_id"] | 0U;
                if (enrichment.id == 0) {
                    return true;
                }

                enrichment.lat = rec.doc["lat"] | 0.0f;
                enrichment.lon = rec.doc["lon"] | 0.0f;
                enrichment.alt = rec.doc["alt"] | 0.0f;
                enrichment.acc = rec.doc["acc"] | 0.0f;
                strlcpy(enrichment.tag,
                        (const char*)(rec.doc["tag"] | ""),
                        sizeof(enrichment.tag));
                enrichment.ts = rec.doc["ts"] | 0U;
                enrichment.gpsTs = rec.doc[F_GPS_TS] | 0U;
                enrichment.noData = rec.doc["enrich_no_data"] | false;
                _backlog.uploadEnrichBySession[rec.sessionId][enrichment.id] = enrichment;
                return true;
            });

        if (!enrichOk) {
            DLOG_WARN("STORAGE", "Upload enrich index skipped corrupt seg=%lu",
                      static_cast<unsigned long>(seg.segmentId));
            requestMaintenance(STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY,
                               "upload_enrich_index_scan_failed");
        }
    }
    return ok;
}

bool StorageManager::_rebuildUploadIndex() {
    _backlog.uploadIndexBySession.clear();
    _backlog.uploadEnrichBySession.clear();
    _backlog.uploadIndexSessions.clear();
    _backlog.uploadIndexStats = {};
    _backlog.uploadIndexResident = true;
    _backlog.uploadIndexWindowTruncated = false;

    const uint32_t pending = getPendingEventCount();
    const uint32_t windowLimit = _backlog.uploadIndexWindowLimit;

    DLOG_INFO("STORAGE",
              "Upload index rebuild begin pending=%lu window=%lu heapFree=%lu largest=%lu",
              static_cast<unsigned long>(pending),
              static_cast<unsigned long>(windowLimit),
              static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
              static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

    bool ok = true;
    uint32_t segsScanned = 0;
    uint32_t sidecarsLoaded = 0;
    uint32_t sidecarsStale = 0;
    for (const auto& seg : _spoolIndex.segments) {
        if (windowLimit > 0 &&
            _backlog.uploadIndexStats.indexedEvents >= windowLimit) {
            _backlog.uploadIndexWindowTruncated = true;
            DLOG_INFO("STORAGE",
                      "Upload index window full indexed=%lu window=%lu stopSeg=%lu",
                      static_cast<unsigned long>(_backlog.uploadIndexStats.indexedEvents),
                      static_cast<unsigned long>(windowLimit),
                      static_cast<unsigned long>(seg.segmentId));
            break;
        }

        const bool sealedSummaryReady =
            seg.segmentId != _spoolIndex.activeSegmentId &&
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;
        const bool loadedSidecar =
            sealedSummaryReady && _loadUploadIndexSidecarSegment(seg);
        if (loadedSidecar) {
            sidecarsLoaded++;
        } else {
            if (sealedSummaryReady) {
                sidecarsStale++;
            }
            if (!_rebuildUploadIndexSegment(seg)) {
                ok = false;
            }
        }

        if (loadedSidecar && seg.enrichDeltaCount > 0) {
            const bool enrichOk = _scanSegmentRecords(seg.segmentId,
                [&](const DecodedSpoolRecord& rec) -> bool {
                    if (rec.recordType != SPOOL_REC_ENRICH_DELTA ||
                        !rec.sessionId.length()) {
                        return true;
                    }

                    SpoolEnrichmentDelta enrichment;
                    enrichment.id = rec.doc["event_id"] | 0U;
                    if (enrichment.id == 0) {
                        return true;
                    }

                    enrichment.lat = rec.doc["lat"] | 0.0f;
                    enrichment.lon = rec.doc["lon"] | 0.0f;
                    enrichment.alt = rec.doc["alt"] | 0.0f;
                    enrichment.acc = rec.doc["acc"] | 0.0f;
                    strlcpy(enrichment.tag,
                            (const char*)(rec.doc["tag"] | ""),
                            sizeof(enrichment.tag));
                    enrichment.ts = rec.doc["ts"] | 0U;
                    enrichment.gpsTs = rec.doc[F_GPS_TS] | 0U;
                    enrichment.noData = rec.doc["enrich_no_data"] | false;
                    _backlog.uploadEnrichBySession[rec.sessionId][enrichment.id] = enrichment;
                    return true;
                });

            if (!enrichOk) {
                DLOG_WARN("STORAGE", "Upload enrich index skipped corrupt seg=%lu",
                          static_cast<unsigned long>(seg.segmentId));
                requestMaintenance(STORAGE_MAINT_UPLOAD_ENRICH_CURSOR_DIRTY,
                                   "upload_enrich_index_scan_failed");
            }
        }

        segsScanned++;
        // Sidecar-only segments do not enter the record scanners, so without
        // an explicit yield a deep field backlog can starve Core 1's idle task
        // for the full index build and trip the ~5 second task watchdog.
        vTaskDelay(1);
        if ((segsScanned % 25U) == 0U) {
            DLOG_INFO("STORAGE",
                      "Upload index rebuild progress segs=%lu indexed=%lu window=%lu heapFree=%lu largest=%lu",
                      static_cast<unsigned long>(segsScanned),
                      static_cast<unsigned long>(_backlog.uploadIndexStats.indexedEvents),
                      static_cast<unsigned long>(windowLimit),
                      static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                      static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        }
    }

    _backlog.uploadIndexStats.sessions = static_cast<uint32_t>(_backlog.uploadIndexSessions.size());

    DLOG_INFO("STORAGE",
              "Upload window rebuild accepted indexed=%lu window=%lu pending=%lu sessions=%lu skipped=%lu failedSegs=%lu sidecarLoaded=%lu sidecarStale=%lu truncated=%u heapFree=%lu largest=%lu",
              static_cast<unsigned long>(_backlog.uploadIndexStats.indexedEvents),
              static_cast<unsigned long>(windowLimit),
              static_cast<unsigned long>(pending),
              static_cast<unsigned long>(_backlog.uploadIndexStats.sessions),
              static_cast<unsigned long>(_backlog.uploadIndexStats.skippedRecords),
              static_cast<unsigned long>(_backlog.uploadIndexStats.failedSegments),
              static_cast<unsigned long>(sidecarsLoaded),
              static_cast<unsigned long>(sidecarsStale),
              _backlog.uploadIndexWindowTruncated ? 1U : 0U,
              static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
              static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

    return ok;
}

bool StorageManager::_openNewSpoolSegment() {
    if (_workerAppendFile) {
        (void)_flushWorkerAppendFile("open_new_segment", true);
    }

    _ensureBootSegmentBaseline();

    const uint32_t segmentId = _spoolIndex.nextSegmentId++;
    const uint8_t format = _spoolIndex.format;

    String path;
    bool created = false;

    // Stamp the absolute UTC base now if we already have a trusted clock; the
    // per-record epoch is then derived for free from the existing millis delta.
    const uint32_t createdMs = millis();
    uint32_t createdEpochUtc = 0;
    if (TIME_SVC.isTimeValid()) {
        (void)TIME_SVC.epochForMillis(createdMs, createdEpochUtc);
    }

    if (format == SPOOL_SEGMENT_BIN_V2) {
        path = _spoolBinarySegmentPath(segmentId);
        created = SpoolBin::createEmptySegmentV2(path, segmentId, createdMs,
                                                createdEpochUtc);
    } else {
        path = _spoolSegmentPath(segmentId);
        File f = LittleFS.open(path, "w");
        if (f) {
            f.close();
            created = true;
        }
    }

    if (!created) {
        DLOG_WARN("STORAGE", "Spool segment create failed seg=%lu format=%s path=%s",
                  static_cast<unsigned long>(segmentId),
                  _segmentFormatText(format),
                  path.c_str());
        return false;
    }

    SpoolSegmentInfo seg;
    seg.segmentId = segmentId;
    seg.format = format;
    seg.trustState = SPOOL_SEGMENT_TRUSTED;
    _clearSegmentSummary(seg);
    _spoolIndex.segments.push_back(seg);
    // Remember the segment we're rotating off so compactSpool() graces it for
    // one cycle before unlink (see _lastRotatedOffSegmentId). Covers the
    // upload-completion rotate path, not just compact-initiated rotates.
    const uint32_t rotatedOff = _spoolIndex.activeSegmentId;
    if (rotatedOff != 0 && rotatedOff != segmentId) {
        _lastRotatedOffSegmentId = rotatedOff;
    }
    _spoolIndex.activeSegmentId = segmentId;
    // If we couldn't stamp a UTC base at creation (clock not yet trusted),
    // remember this this-boot segment for backfill once the clock arrives.
    if (createdEpochUtc == 0) {
        _unstampedThisBootSegments.insert(segmentId);
    }
    _binaryLastSessionBySegment.erase(segmentId);
    _binaryLastSessionTagBySegment.erase(segmentId);
    _binaryEnrichCtxBySegment.erase(segmentId);
    _binaryCheckpointBySegment.erase(segmentId);
    if (_spoolIndex.oldestSegmentId == 0) {
        _spoolIndex.oldestSegmentId = segmentId;
    }

    if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY) {
        _spoolIndexDirty = true;
        if (_spoolIndexPendingWrites < 0xFFFFu) {
            _spoolIndexPendingWrites++;
        }
        _workerMetadataDirtyPending = true;
        DLOG_INFO("STORAGE",
                  "Spool rotate index deferred owner=%s reason=capture seg=%lu format=%s",
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()),
                  static_cast<unsigned long>(segmentId),
                  _segmentFormatText(format));
        return true;
    }

    const uint32_t rotatePersistStart = millis();
    const bool ok = _persistSpoolIndex(true, "rotate_new_segment");
    if (ok) {
        DLOG_INFO("STORAGE", "Spool rotate newSeg=%lu format=%s",
                  static_cast<unsigned long>(segmentId),
                  _segmentFormatText(format));
        _logCaptureWriteAllowed(_spoolIndexPath().c_str(),
                                "rotate_new_segment",
                                millis() - rotatePersistStart,
                                true);
        _logSpoolDiagnostics("rotate");
    }
    return ok;
}

void StorageManager::_ensureBootSegmentBaseline() {
    if (_bootSegmentBaselineSet) return;
    // Every segment id assignable from now on belongs to this boot. Anything
    // already on disk (id < baseline) carries a prior boot's millis base.
    _thisBootSegmentBaseId = _spoolIndex.nextSegmentId;
    _bootSegmentBaselineSet = true;
    _pendingBootEventRotate = true;
}

bool StorageManager::_segmentCreatedThisBoot(uint32_t segmentId) const {
    return _bootSegmentBaselineSet && segmentId != 0 &&
           segmentId >= _thisBootSegmentBaseId;
}

bool StorageManager::_stampSegmentEpoch(uint32_t segmentId) {
    if (segmentId == 0) return false;
    const SpoolSegmentInfo* seg = _findSegmentInfo(segmentId);
    if (!seg || seg->format != SPOOL_SEGMENT_BIN_V2) return false;

    const String path = _spoolBinarySegmentPath(segmentId);

    // Flush any open worker append handle so the on-disk header is current
    // before we rewrite it underneath.
    if (_workerAppendFile && _workerAppendSegmentId == segmentId) {
        (void)_flushWorkerAppendFile("stamp_segment_epoch", true);
    }

    File f = LittleFS.open(path, "r+");
    if (!f) return false;
    SpoolBin::SegmentHeaderV2 hdr;
    if (!SpoolBin::readSegmentHeaderV2(f, hdr) ||
        hdr.magic != SpoolBin::SEGMENT_MAGIC || hdr.version != 2) {
        f.close();
        return false;
    }
    if (hdr.createdEpochUtc != 0) {
        // Already dated on disk (e.g. stamped at creation).
        f.close();
        return true;
    }

    uint32_t epoch = 0;
    if (!TIME_SVC.epochForMillis(hdr.createdMs, epoch) || epoch == 0) {
        f.close();
        return false;
    }
    hdr.createdEpochUtc = epoch;
    const bool ok = SpoolBin::writeSegmentHeaderV2(f, hdr);
    f.close();
    if (!ok) return false;

    // Keep the cached worker header in sync if it tracks this segment.
    if (_workerAppendHeaderOk && _workerAppendSegmentId == segmentId) {
        _workerAppendHeader.createdEpochUtc = epoch;
    }
    char iso[24] = {};
    TIME_SVC.formatIsoForMillis(hdr.createdMs, iso, sizeof(iso));
    DLOG_INFO("STORAGE",
              "Segment UTC base stamped seg=%lu epoch=%lu utc=%s",
              static_cast<unsigned long>(segmentId),
              static_cast<unsigned long>(epoch),
              iso);
    return true;
}

void StorageManager::_backfillThisBootSegmentEpochs() {
    if (_unstampedThisBootSegments.empty()) return;
    if (!TIME_SVC.isTimeValid()) return;
    for (auto it = _unstampedThisBootSegments.begin();
         it != _unstampedThisBootSegments.end();) {
        if (_stampSegmentEpoch(*it)) {
            it = _unstampedThisBootSegments.erase(it);
        } else {
            ++it;
        }
    }
}

bool StorageManager::_ensureSpoolReady() {
    auto bootStep = [](uint32_t step, const char* label) {
#if BOOT_SEQUENCE_VERBOSE_ACTIVE
        Serial.printf("[STORAGE_BOOT] step=%lu %s heapFree=%lu largest=%lu\r\n",
                      static_cast<unsigned long>(step),
                      label ? label : "-",
                      static_cast<unsigned long>(
                          heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
                      static_cast<unsigned long>(
                          heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM)));
#endif
        crashCheckpointStep(CrashPhase::STORAGE_BOOT, 0, step, true);
    };

    bootStep(300, "ensure_spool_entry");
    _ensureDir(_spoolDir());
    bootStep(310, "spool_dir_ready");

    if (!_loadSpoolIndex()) {
        bootStep(320, "spool_index_defaulted");
        _spoolIndex.version = 2;
        _spoolIndex.format = SPOOL_SEGMENT_BIN_V2;
        _spoolIndex.nextSegmentId = 1;
        _spoolIndex.activeSegmentId = 0;
        _spoolIndex.oldestSegmentId = 0;
        _spoolIndex.pendingTotal = 0;
        _spoolIndex.sessions.clear();
        _spoolIndex.uploadedWatermarks.clear();
        _spoolIndex.segments.clear();
    } else {
        bootStep(321, "spool_index_loaded");
    }

    _spoolIndex.format = SPOOL_SEGMENT_BIN_V2;

    bootStep(330, "spool_resync_start");
    const bool resyncChanged = _resyncSpoolIndexFromFilesystem();
    bootStep(331, "spool_resync_done");
#if STORAGE_FAST_BOOT_DEFER_SPOOL_REPAIR
    const bool summaryChanged = false;
    if (_hasInvalidSpoolSummaries()) {
        _spoolSummaryRebuildPending = true;
        requestMaintenance(STORAGE_MAINT_DIRTY_SUMMARY, "boot_invalid_summary");
    }
    if (resyncChanged) {
        _spoolIndexDirty = true;
        requestMaintenance(STORAGE_MAINT_DIRTY_SPOOL_INDEX, "boot_resync_changed");
    }
#else
    const bool summaryChanged = _rebuildInvalidSegmentSummaries(false);
#endif

    bootStep(340, "event_counter_load_start");
    if (!_loadEventCounter()) {
        DLOG_WARN("STORAGE", "Event counter recovery failed");
    }
    bootStep(341, "event_counter_load_done");

    bootStep(350, "event_meta_load_start");
    if (!_loadEventMeta()) {
        DLOG_WARN("STORAGE", "Event meta recovery required");
    }
    bootStep(351, "event_meta_load_done");

    bootStep(360, "metadata_reconcile_start");
    if (!_reconcileStorageMetadata()) {
        DLOG_WARN("STORAGE", "Storage metadata reconcile requested audit");
    }
    bootStep(361, "metadata_reconcile_done");

    if (_spoolIndex.activeSegmentId == 0 || !_findSegmentInfo(_spoolIndex.activeSegmentId)) {
        requestMaintenance(STORAGE_MAINT_ACTIVE_SEGMENT_INVALID,
                           "ensure_ready_active_missing");
        bootStep(370, "active_segment_open_start");
        if (!_openNewSpoolSegment()) {
            return false;
        }
        bootStep(371, "active_segment_open_done");
    }

    const bool needPersist = resyncChanged || summaryChanged;
    const bool activeSegmentValid =
        _spoolIndex.activeSegmentId != 0 &&
        _findSegmentInfo(_spoolIndex.activeSegmentId);
    const bool countersTrusted =
        _counterTrustState == STORAGE_COUNTER_TRUSTED;

    if (!countersTrusted || hasSpoolRepairWork()) {
        if (needPersist) {
            _spoolIndexDirty = true;
            _workerMetadataDirtyPending = true;
            requestMaintenance(STORAGE_MAINT_DIRTY_SPOOL_INDEX,
                               "ensure_ready_repair_deferred");
        }
        requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                           "ensure_ready_repair_required");
        requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED,
                           "ensure_ready_repair_required");
        DLOG_INFO("STORAGE",
                  "Ensure-ready deferred reason=repair_required active=%lu pending=%lu resyncChanged=%u summaryChanged=%u",
                  static_cast<unsigned long>(_spoolIndex.activeSegmentId),
                  static_cast<unsigned long>(_pendingEventCount),
                  resyncChanged ? 1U : 0U,
                  summaryChanged ? 1U : 0U);
        return activeSegmentValid;
    }

    if (needPersist && activeSegmentValid && countersTrusted) {
        _spoolIndexDirty = true;
        _workerMetadataDirtyPending = true;
        requestMaintenance(STORAGE_MAINT_BOOT_SAFE_DEFERRED_PERSIST,
                           "ensure_ready");
        if (resyncChanged) {
            requestMaintenance(STORAGE_MAINT_DIRTY_SPOOL_INDEX,
                               "ensure_ready_resync");
        }
        if (summaryChanged) {
            requestMaintenance(STORAGE_MAINT_DIRTY_SUMMARY,
                               "ensure_ready_summary");
        }
        DLOG_INFO("STORAGE",
                  "Ensure-ready index persist deferred reason=boot_safe_defer active=%lu pending=%lu resyncChanged=%u summaryChanged=%u",
                  static_cast<unsigned long>(_spoolIndex.activeSegmentId),
                  static_cast<unsigned long>(_pendingEventCount),
                  resyncChanged ? 1U : 0U,
                  summaryChanged ? 1U : 0U);
        return true;
    }

    if (needPersist &&
        RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY) {
        _spoolIndexDirty = true;
        _workerMetadataDirtyPending = true;
        requestMaintenance(STORAGE_MAINT_DIRTY_SPOOL_INDEX,
                           "ensure_ready_capture");
        DLOG_INFO("STORAGE",
                  "Ensure-ready index persist deferred owner=%s reason=capture",
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
        return true;
    }

    const bool ok = needPersist
                        ? _persistSpoolIndex(true, "ensure_ready_rescan")
                        : true;
    if (ok) {
        _logSpoolDiagnostics("ensure_ready_pre_rescan", String(), false);
    }
    bootStep(390, "ensure_spool_done");
    return ok;
}

static bool _parseSpoolSegmentFilename(const char* name,
                                      uint32_t& outSegmentId,
                                      uint8_t& outFormat) {
    if (!name) return false;

    const char* base = strrchr(name, '/');
    base = base ? (base + 1) : name;

    if (strncmp(base, "seg_", 4) != 0) return false;

    const char* p = base + 4;
    char* end = nullptr;
    unsigned long id = strtoul(p, &end, 10);
    if (end == p || id == 0UL || id > 0xFFFFFFFFUL) return false;

    if (strcmp(end, ".jsonl") == 0) {
        outFormat = SPOOL_SEGMENT_JSONL;
    } else if (strcmp(end, ".bin") == 0 || strcmp(end, ".sp2") == 0) {
        outFormat = SPOOL_SEGMENT_BIN_V2;
    } else {
        return false;
    }

    outSegmentId = static_cast<uint32_t>(id);
    return true;
}

bool StorageManager::_resyncSpoolIndexFromFilesystem() {
    File dir = LittleFS.open(PATH_SPOOL);
    if (!dir || !dir.isDirectory()) {
        return false;
    }

    std::map<uint32_t, uint8_t> onDiskFormat;
    std::vector<uint32_t> duplicateJsonToRemove;

    File f = dir.openNextFile();
    while (f) {
        uint32_t id = 0;
        uint8_t fmt = 0;
        if (_parseSpoolSegmentFilename(f.name(), id, fmt)) {
            auto it = onDiskFormat.find(id);
            if (it == onDiskFormat.end()) {
                onDiskFormat[id] = fmt;
        } else if (it->second != SPOOL_SEGMENT_BIN_V2 && fmt == SPOOL_SEGMENT_BIN_V2) {
            it->second = fmt;
            duplicateJsonToRemove.push_back(id);
            } else if (it->second == SPOOL_SEGMENT_BIN_V2 && fmt == SPOOL_SEGMENT_JSONL) {
                duplicateJsonToRemove.push_back(id);
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();

    for (uint32_t segmentId : duplicateJsonToRemove) {
        const String legacyJsonPath = _spoolSegmentPath(segmentId);
        const String binaryPath = _spoolBinarySegmentPath(segmentId);

        if (LittleFS.exists(binaryPath) && LittleFS.exists(legacyJsonPath)) {
            if (LittleFS.remove(legacyJsonPath)) {
                DLOG_INFO("STORAGE",
                          "Retired duplicate legacy json spool seg=%lu",
                          static_cast<unsigned long>(segmentId));
            } else {
                DLOG_WARN("STORAGE",
                          "Failed to retire duplicate legacy json spool seg=%lu path=%s",
                          static_cast<unsigned long>(segmentId),
                          legacyJsonPath.c_str());
            }
        }
    }

    std::map<uint32_t, SpoolSegmentInfo> prior;
    for (const auto& seg : _spoolIndex.segments) {
        prior[seg.segmentId] = seg;
    }

    std::vector<SpoolSegmentInfo> rebuilt;
    rebuilt.reserve(onDiskFormat.size());

    for (const auto& kv : onDiskFormat) {
        SpoolSegmentInfo seg;
        auto it = prior.find(kv.first);
        if (it != prior.end()) {
            seg = it->second;
        } else {
            seg.trustState = SPOOL_SEGMENT_UNTRUSTED;
            seg.summaryValid = false;
        }
        seg.segmentId = kv.first;
        seg.format = kv.second;
        rebuilt.push_back(seg);
    }

    for (const auto& kv : prior) {
        if (onDiskFormat.find(kv.first) == onDiskFormat.end()) {
            DLOG_WARN("STORAGE",
                      "Spool index segment missing on disk; dropping seg=%lu format=%s",
                      static_cast<unsigned long>(kv.first),
                      _segmentFormatText(kv.second.format));
        }
    }

    bool changed = false;
    if (rebuilt.size() != _spoolIndex.segments.size()) {
        changed = true;
    } else {
        for (size_t i = 0; i < rebuilt.size(); i++) {
            const auto& a = rebuilt[i];
            const auto& b = _spoolIndex.segments[i];
            if (a.segmentId != b.segmentId || a.format != b.format) {
                changed = true;
                break;
            }
        }
    }

    _spoolIndex.segments.assign(rebuilt.begin(), rebuilt.end());

    if (!_spoolIndex.segments.empty()) {
        const uint32_t oldest = _spoolIndex.segments.front().segmentId;
        if (_spoolIndex.oldestSegmentId != oldest) {
            _spoolIndex.oldestSegmentId = oldest;
            changed = true;
        }

        uint32_t maxId = _spoolIndex.segments.back().segmentId;
        if (_spoolIndex.nextSegmentId <= maxId) {
            _spoolIndex.nextSegmentId = maxId + 1U;
            changed = true;
        }
    } else {
        if (_spoolIndex.nextSegmentId == 0) {
            _spoolIndex.nextSegmentId = 1;
            changed = true;
        }
        if (_spoolIndex.oldestSegmentId != 0) {
            _spoolIndex.oldestSegmentId = 0;
            changed = true;
        }
    }

    if (_spoolIndex.activeSegmentId != 0 &&
        onDiskFormat.find(_spoolIndex.activeSegmentId) == onDiskFormat.end()) {
        DLOG_WARN("STORAGE", "Active spool segment missing on disk; clearing active=%lu",
                  static_cast<unsigned long>(_spoolIndex.activeSegmentId));
        _spoolIndex.activeSegmentId = 0;
        changed = true;
    }

    if (changed) {
        DLOG_INFO("STORAGE", "Spool index resynced segments=%u",
                  static_cast<unsigned>(_spoolIndex.segments.size()));
    }

    return changed;
}

bool StorageManager::prepareForEnrichmentAppend(size_t expectedDeltaCount) {
    if (!_ready) return false;
    _refreshFsStats(true);
    const size_t deltaBudget =
        expectedDeltaCount > (SIZE_MAX - ENRICH_BATCH_OVERHEAD_BYTES) /
                                 ENRICH_DELTA_BUDGET_BYTES
            ? SIZE_MAX
            : expectedDeltaCount * ENRICH_DELTA_BUDGET_BYTES +
                  ENRICH_BATCH_OVERHEAD_BYTES;
    const size_t requiredFree =
        deltaBudget > SIZE_MAX - STORAGE_METADATA_RESERVE_BYTES
            ? SIZE_MAX
            : deltaBudget + STORAGE_METADATA_RESERVE_BYTES;
    if (_cachedFreeBytes < requiredFree) {
        requestMaintenance(STORAGE_MAINT_DELETE_DRAINED,
                           "enrich_storage_reserve");
        DLOG_WARN("STORAGE",
                  "Enrich preflight blocked free=%lu required=%lu count=%u reserve=%lu",
                  static_cast<unsigned long>(_cachedFreeBytes),
                  static_cast<unsigned long>(requiredFree),
                  static_cast<unsigned>(expectedDeltaCount),
                  static_cast<unsigned long>(STORAGE_METADATA_RESERVE_BYTES));
        return false;
    }
    if (_spoolIndex.activeSegmentId == 0) return _openNewSpoolSegment();

    SpoolSegmentInfo* active = _findSegmentInfo(_spoolIndex.activeSegmentId);
    if (!active) return _openNewSpoolSegment();

    const bool tooManyBytes =
        active->approxBytes >= SPOOL_ENRICH_PREFLIGHT_ROTATE_BYTES;

    const bool tooManyRecords =
        active->recordCount + expectedDeltaCount >= SPOOL_ENRICH_PREFLIGHT_ROTATE_RECORDS;

    const bool tooManyDeltas =
        active->enrichDeltaCount + expectedDeltaCount >= SPOOL_ENRICH_PREFLIGHT_ROTATE_DELTAS;

    if (!(tooManyBytes || tooManyRecords || tooManyDeltas)) {
        return true;
    }

    DLOG_INFO("STORAGE",
              "Spool enrich preflight rotate seg=%lu bytes=%lu records=%lu enrich=%lu expected=%u",
              static_cast<unsigned long>(active->segmentId),
              static_cast<unsigned long>(active->approxBytes),
              static_cast<unsigned long>(active->recordCount),
              static_cast<unsigned long>(active->enrichDeltaCount),
              static_cast<unsigned>(expectedDeltaCount));

    return _openNewSpoolSegment();
}

bool StorageManager::appendEnrichDeltasBatch(const SpoolEnrichBatchEntry* entries,
                                             size_t count,
                                             uint32_t* appliedOut,
                                             uint32_t* failedOut) {
    if (!entries || count == 0) return false;
    if (!_ready) return false;

    if (!prepareForEnrichmentAppend(count)) return false;

    SpoolSegmentInfo* seg = _findSegmentInfo(_spoolIndex.activeSegmentId);
    if (!seg) return false;

    if (seg->format != SPOOL_SEGMENT_BIN_V2) {
        DLOG_WARN("STORAGE", "Enrich batch requires binary segment seg=%lu",
                  static_cast<unsigned long>(seg->segmentId));
        return false;
    }

    uint32_t applied        = 0;
    uint32_t failed         = 0;
    uint32_t firstRecord    = 0;
    uint32_t lastRecord     = 0;
    uint32_t firstEvent     = 0;
    uint32_t lastEvent      = 0;
    uint32_t tStart         = 0;
    bool ok                 = false;
    String lastSession;
    std::vector<size_t> appliedEntryIndexes;
    appliedEntryIndexes.reserve(count);

    ScopedSemaphoreLock appendLock(_appendMutex);
    {
    const String segPath = _spoolBinarySegmentPath(seg->segmentId);

    if (_workerAppendFile && _workerAppendSegmentId == seg->segmentId) {
        if (!_flushWorkerAppendFile("append_enrich_batch_sync", true)) {
            return false;
        }
    }

    File f = LittleFS.open(segPath, "r+");
    if (!f) return false;

    SpoolBin::SegmentHeaderV2 hdr;
    if (!SpoolBin::readSegmentHeaderV2(f, hdr) ||
        hdr.magic != SpoolBin::SEGMENT_MAGIC || hdr.version != 2) {
        f.close();
        return false;
    }

    if (!f.seek(f.size())) {
        f.close();
        return false;
    }

    const uint32_t tsBase   = hdr.createdMs;
    tStart = millis();
    lastSession = _binaryLastSessionBySegment[seg->segmentId];
    BinaryEnrichContext& enrichCtx = _binaryEnrichCtxBySegment[seg->segmentId];

    for (size_t i = 0; i < count; ++i) {
        const SpoolEnrichBatchEntry& e = entries[i];

        if (e.eventId == 0 || !e.sessionId || !e.sessionId[0]) {
            failed++;
            continue;
        }

        const uint32_t recordId = _nextEventId++;
        _eventCounterDirty = true;
        _eventCounterPendingWrites++;

        const uint32_t ts      = millis();
        const uint32_t tsDelta = _timestampDeltaFromBase(ts, tsBase);

        std::vector<uint8_t> body;
        body.reserve(64);

        const String sessionStr(e.sessionId);
        const bool sameSession = (sessionStr == lastSession);

        uint8_t enrichFlags = 0;
        const String tagStr(e.tag ? e.tag : "");
        if (tagStr.length()) enrichFlags |= ENRICH_FLAG_TAG;
        if (e.gpsEpochUtc >= MIN_ENRICH_GPS_EPOCH) {
            enrichFlags |= ENRICH_FLAG_GPS_TS;
        }
        if (e.noData) {
            enrichFlags |= ENRICH_FLAG_NO_DATA;
        }

        _buildBinaryEnrichDeltaBodyV2(body, enrichCtx, recordId, tsDelta,
                                      sameSession, sessionStr, String(),
                                      enrichFlags, e.eventId,
                                      _floatToE7(e.lat), _floatToE7(e.lon),
                                      _floatToCm(e.alt), _floatToDm(e.acc),
                                      tagStr, e.gpsEpochUtc);

        if (!SpoolBin::appendRecordToOpen(f,
                                          static_cast<uint8_t>(SpoolBin::REC_ENRICH_DELTA_V2),
                                          body.data(),
                                          static_cast<uint16_t>(body.size()),
                                          recordId,
                                          hdr)) {
            failed += (count - i);
            DLOG_WARN("STORAGE", "Enrich batch write failed at delta %u; aborting",
                      static_cast<unsigned>(i));
            break;
        }

        lastSession = sessionStr;
        if (firstRecord == 0) firstRecord = recordId;
        lastRecord = recordId;
        if (firstEvent == 0) firstEvent = e.eventId;
        lastEvent = e.eventId;
        applied++;
        appliedEntryIndexes.push_back(i);
        _decrementPendingEnrichmentForEvent(e.eventId);
    }

    ok = SpoolBin::writeSegmentHeaderV2(f, hdr);
    if (ok) f.flush();
    f.close();

    seg->firstEventId = hdr.firstEventId;
    seg->lastEventId  = hdr.lastEventId;
    seg->recordCount  = hdr.recordCount;
    seg->approxBytes  = hdr.bodyBytes + sizeof(SpoolBin::SegmentHeaderV2);

    if (applied > 0) {
        _bumpStorageMetaGeneration();
        const uint32_t batchTs = millis();
        seg->enrichDeltaCount += applied;
        if (seg->minTimestampMs == 0 || batchTs < seg->minTimestampMs) {
            seg->minTimestampMs = batchTs;
        }
        if (batchTs > seg->maxTimestampMs) {
            seg->maxTimestampMs = batchTs;
        }
        seg->summaryVersion = SPOOL_SEGMENT_SUMMARY_VERSION;
        seg->summaryValid   = true;
        _binaryLastSessionBySegment[seg->segmentId] = lastSession;
        // Batched enrich deltas carry no session_tag either.
        _binaryLastSessionTagBySegment[seg->segmentId] = "";
        _spoolIndex.nextEventId = _nextEventId;
    }

    if (applied > 0) {
        _workerMetadataDirtyPending = true;
        if (_workerMetadataPendingWrites < UINT16_MAX) {
            _workerMetadataPendingWrites +=
                static_cast<uint16_t>(std::min<uint32_t>(
                    applied,
                    static_cast<uint32_t>(UINT16_MAX - _workerMetadataPendingWrites)));
        }
        if (_workerMetadataDirtySinceMs == 0) {
            _workerMetadataDirtySinceMs = millis();
        }
        if (_counterTrustState == CounterTrust::Trusted) {
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                  "append_enrich_batch_deferred");
        }
    }

    (void)_maybeWriteBinarySegmentCheckpoint(*seg,
                                             "append_enrich_batch",
                                             false,
                                             nullptr);

    if (ok && seg->approxBytes >= SPOOL_SEGMENT_TARGET_BYTES) {
        if (!_openNewSpoolSegment()) ok = false;
    } else if (ok) {
        _spoolIndexDirty = true;
        if (_spoolIndexPendingWrites < 0xFFFFu) {
            _spoolIndexPendingWrites++;
        }
        requestMaintenance(STORAGE_MAINT_SNAPSHOT_LAGGED,
                           "append_enrich_batch_deferred");
    }

    if (appliedOut) *appliedOut = applied;
    if (failedOut)  *failedOut  = failed;
    }

    if (ok) {
        for (size_t index : appliedEntryIndexes) {
            const SpoolEnrichBatchEntry& e = entries[index];
            ENTITY_MGR.applyEnrichment(e.eventId, e.lat, e.lon, e.alt, e.acc,
                                       e.noData);
        }
        _refreshFsStats(true);
        updateStoragePressure(false);
        if (_pressureMode >= STORAGE_MODE_WATCH) {
            requestMaintenance(STORAGE_MAINT_DELETE_DRAINED,
                               "enrich_pressure_reclaim");
        }
        _queueStorageUiRefresh(true);
    }

    DLOG_INFO("STORAGE",
              "Spool enrich_batch applied=%lu failed=%lu"
              " batchFirstEventId=%lu batchLastEventId=%lu firstRecord=%lu lastRecord=%lu ms=%lu",
              static_cast<unsigned long>(applied),
              static_cast<unsigned long>(failed),
              static_cast<unsigned long>(firstEvent),
              static_cast<unsigned long>(lastEvent),
              static_cast<unsigned long>(firstRecord),
              static_cast<unsigned long>(lastRecord),
              static_cast<unsigned long>(millis() - tStart));

    return ok && failed == 0;
}

bool StorageManager::_appendSpoolRecord(JsonDocument& doc,
                                        uint32_t* outEventId,
                                        bool deferIndexPersist,
                                        QueuedAppendTiming* timing) {
    _ensureBootSegmentBaseline();

    // First event append after boot: never extend a segment created in a prior
    // boot (its createdMs belongs to a millis() epoch that reset). Rotate so the
    // record lands in a fresh, this-boot segment with a coherent time base.
    if (_pendingBootEventRotate) {
        _pendingBootEventRotate = false;
        if (_spoolIndex.activeSegmentId != 0 &&
            !_segmentCreatedThisBoot(_spoolIndex.activeSegmentId)) {
            if (!_openNewSpoolSegment()) {
                return false;
            }
        }
    }

    if (_spoolIndex.activeSegmentId == 0) {
        if (!_openNewSpoolSegment()) {
            return false;
        }
    }

    SpoolSegmentInfo* seg = _findSegmentInfo(_spoolIndex.activeSegmentId);
    if (!seg) {
        if (!_openNewSpoolSegment()) {
            return false;
        }
        seg = _findSegmentInfo(_spoolIndex.activeSegmentId);
        if (!seg) return false;
    }

    // Backfill UTC bases the first time we append with a trusted clock.
    // Stamping a base retroactively dates every record already in that segment
    // (each record's epoch = base + its millis delta). Header-only, bounded by
    // the small set of unstamped this-boot segments.
    _backfillThisBootSegmentEpochs();

    const uint32_t appendStartMs = timing ? millis() : 0U;
    if (!_appendSegmentRecord(*seg, doc, outEventId, nullptr, timing)) {
        return false;
    }
    if (timing) {
        timing->appendSegmentMs += millis() - appendStartMs;
    }

    const uint32_t eventId =
        outEventId && *outEventId ? *outEventId : (doc["id"] | 0U);
    const uint32_t ts = doc["ts"] | 0U;
    const JsonObjectConst eventObj = doc.as<JsonObjectConst>();
    _updateSegmentSummaryFromEventDoc(*seg, eventObj, eventId, ts);

    const bool rotateAfterAppend = _shouldRotateSegmentAfterAppend(*seg);
    const bool checkpointForce = rotateAfterAppend;
    const uint32_t checkpointStartMs = timing ? millis() : 0U;
    (void)_maybeWriteBinarySegmentCheckpoint(
        *seg,
        checkpointForce ? "append_rotate" : "append_periodic",
        checkpointForce,
        nullptr);
    if (timing) {
        timing->checkpointMs += millis() - checkpointStartMs;
    }
    const bool logAppendBatchDiag = (seg->recordCount % 64U) == 0U;

    const bool shouldDeferIndex =
        deferIndexPersist || _workerAppendBatchActive ||
        _shouldDeferWorkerMetadataFlush();
    const uint32_t indexStartMs = timing ? millis() : 0U;
    if (rotateAfterAppend) {
        if (!_openNewSpoolSegment()) {
            _setCounterTrustState(STORAGE_COUNTER_REPAIR_REQUIRED,
                                  "append_rotate_open_failed");
            requestMaintenance(STORAGE_MAINT_ACTIVE_SEGMENT_INVALID,
                               "append_rotate_open_failed");
            return false;
        }
        if (timing) {
            timing->indexPersistMs += millis() - indexStartMs;
        }
        _logSpoolDiagnostics("append_rotate");
        _checkSpoolInvariants("rotation", false);
    } else {
        if (shouldDeferIndex) {
            _spoolIndexDirty = true;
            if (_spoolIndexPendingWrites < 0xFFFFu) {
                _spoolIndexPendingWrites++;
            }
        } else if (!_persistSpoolIndex(false, "append_index")) {
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                  "append_index_persist_failed");
            return false;
        }
        if (timing) {
            timing->indexPersistMs += millis() - indexStartMs;
        }
    }

    if (shouldDeferIndex) {
        _workerMetadataDirtyPending = true;
    }

    if (logAppendBatchDiag) {
        _logSpoolDiagnostics("append_batch");
    }

    return true;
}

bool StorageManager::_maybeWriteBinarySegmentCheckpoint(SpoolSegmentInfo& seg,
                                                        const char* reason,
                                                        bool force,
                                                        bool* outWrote) {
    if (outWrote) {
        *outWrote = false;
    }

    if (seg.format != SPOOL_SEGMENT_BIN_V2) {
        return true;
    }

    if (seg.recordCount == 0 && seg.eventCount == 0 && seg.enrichDeltaCount == 0) {
        return true;
    }

    if (!seg.summaryValid || seg.summaryVersion != SPOOL_SEGMENT_SUMMARY_VERSION) {
        return true;
    }

    // Checkpoint records are sidecars: they do not advance seg.recordCount.
    static constexpr uint32_t CHECKPOINT_RECORD_INTERVAL = 128U;
    static constexpr uint32_t CHECKPOINT_PENDING_DELTA = 4U;
    static constexpr uint32_t CHECKPOINT_CAPTURE_PENDING_DELTA = 64U;
    // An enrichment drain moves pendingEnrichmentCount by exactly 1 per delta,
    // so the pending-delta rule degenerates into "checkpoint every 4 records"
    // and appends an ~84 B sidecar against a ~30 B delta. Monotonic enrichment
    // progress is fully recoverable by replaying the deltas that follow the
    // last checkpoint, so it gets its own much coarser threshold.
    static constexpr uint32_t CHECKPOINT_ENRICH_PENDING_DELTA = 256U;

    const auto it = _binaryCheckpointBySegment.find(seg.segmentId);
    const bool haveCheckpoint = (it != _binaryCheckpointBySegment.end());
    const SpoolBin::SpoolSegmentCheckpointV1* prev =
        haveCheckpoint ? &it->second : nullptr;

    const uint32_t currentPendingUpload =
        seg.pendingUploadMissionCount + seg.pendingUploadNoiseCount;
    const uint32_t currentPendingEnrichment = seg.pendingEnrichmentCount;

    uint32_t recordDelta = seg.recordCount;
    // Upload and enrichment pending counts are tracked separately: they move
    // for different reasons and at wildly different rates, so folding them into
    // one delta let the fast one dominate the threshold.
    uint32_t uploadPendingDelta = currentPendingUpload;
    uint32_t enrichPendingDelta = currentPendingEnrichment;

    if (prev) {
        if (seg.recordCount >= prev->recordCount) {
            recordDelta = seg.recordCount - prev->recordCount;
        }
        const uint32_t prevPendingUpload =
            prev->pendingUploadMissionCount + prev->pendingUploadNoiseCount;
        const uint32_t prevPendingEnrichment = prev->pendingEnrichmentCount;
        uploadPendingDelta =
            (currentPendingUpload > prevPendingUpload)
                ? (currentPendingUpload - prevPendingUpload)
                : (prevPendingUpload - currentPendingUpload);
        enrichPendingDelta =
            (currentPendingEnrichment > prevPendingEnrichment)
                ? (currentPendingEnrichment - prevPendingEnrichment)
                : (prevPendingEnrichment - currentPendingEnrichment);
    }

    const uint32_t uploadThreshold =
        (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE)
            ? CHECKPOINT_CAPTURE_PENDING_DELTA
            : CHECKPOINT_PENDING_DELTA;

    const bool due =
        force ||
        (recordDelta >= CHECKPOINT_RECORD_INTERVAL) ||
        (uploadPendingDelta >= uploadThreshold) ||
        (enrichPendingDelta >= CHECKPOINT_ENRICH_PENDING_DELTA);

    if (!due) {
        return true;
    }

    if (!force && _workerAppendBatchActive) {
        if (!_binaryCheckpointDeferred) {
            _binaryCheckpointDeferred = true;
            requestMaintenance(STORAGE_MAINT_BINARY_CHECKPOINT,
                               "checkpoint_deferred_worker_batch");
        }
        return true;
    }

    if (!force && RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE) {
        if (!_binaryCheckpointDeferred) {
            _binaryCheckpointDeferred = true;
            requestMaintenance(STORAGE_MAINT_BINARY_CHECKPOINT,
                               "checkpoint_deferred_capture");
        }
        return true;
    }

    SpoolBin::SpoolSegmentCheckpointV1 checkpoint;
    checkpoint.segmentId = seg.segmentId;
    checkpoint.lastEventId = seg.lastEventId;
    checkpoint.recordCount = seg.recordCount;
    checkpoint.eventCount = seg.eventCount;
    checkpoint.enrichDeltaCount = seg.enrichDeltaCount;
    checkpoint.missionCount = seg.missionCount;
    checkpoint.noiseCount = seg.noiseCount;
    checkpoint.pendingUploadMissionCount = seg.pendingUploadMissionCount;
    checkpoint.pendingUploadNoiseCount = seg.pendingUploadNoiseCount;
    checkpoint.pendingEnrichmentCount = seg.pendingEnrichmentCount;
    checkpoint.p0Count = seg.p0Count;
    checkpoint.p1Count = seg.p1Count;
    checkpoint.p2Count = seg.p2Count;
    checkpoint.p3Count = seg.p3Count;
    checkpoint.minTimestampMs = seg.minTimestampMs;
    checkpoint.maxTimestampMs = seg.maxTimestampMs;

    const String path = _spoolBinarySegmentPath(seg.segmentId);
    SpoolBin::AppendRecordLocation loc;
    const uint32_t writeStartMs = millis();

    if (_workerAppendFile && _workerAppendSegmentId == seg.segmentId) {
        bool ok = true;
        if (_workerAppendHeaderDirty) {
            ok = SpoolBin::writeSegmentHeaderV2(_workerAppendFile,
                                                _workerAppendHeader);
        }
        if (ok) {
            ok = _workerAppendFile.seek(_workerAppendWriteOffset);
        }
        if (ok) {
            ok = SpoolBin::appendCheckpointRecordToOpen(_workerAppendFile,
                                                        checkpoint,
                                                        &loc);
        }
        if (ok) {
            _workerAppendWriteOffset = loc.offset + loc.len;
            _workerAppendHeaderDirty = false;
            _workerAppendRecordsSinceFlush = 0;
            _workerAppendFile.flush();
        }
        if (!ok) {
            _closeWorkerAppendFile("checkpoint_failed");
            DLOG_WARN("STORAGE",
                      "Binary checkpoint write failed seg=%lu reason=%s",
                      static_cast<unsigned long>(seg.segmentId),
                      (reason && reason[0]) ? reason : "-");
            return false;
        }
    } else if (!SpoolBin::appendCheckpointRecordV1(path, checkpoint, &loc)) {
        DLOG_WARN("STORAGE",
                  "Binary checkpoint write failed seg=%lu reason=%s",
                  static_cast<unsigned long>(seg.segmentId),
                  (reason && reason[0]) ? reason : "-");
        return false;
    }

    seg.approxBytes = loc.offset + loc.len;
    _binaryCheckpointBySegment[seg.segmentId] = checkpoint;
    _binaryLastSessionBySegment[seg.segmentId] = "";
    _binaryLastSessionTagBySegment[seg.segmentId] = "";
    _binaryEnrichCtxBySegment[seg.segmentId] = BinaryEnrichContext{};
    if (seg.segmentId == _spoolIndex.activeSegmentId) {
        _binaryCheckpointDeferred = false;
        _clearMaintenanceFlags(STORAGE_MAINT_BINARY_CHECKPOINT);
    }

    if (outWrote) {
        *outWrote = true;
    }

    _logCaptureWriteAllowed(path.c_str(),
                            reason,
                            millis() - writeStartMs,
                            true);
    DLOG_DEBUG("STORAGE",
               "Binary checkpoint written seg=%lu reason=%s recordCount=%lu eventCount=%lu enrichDeltaCount=%lu",
               static_cast<unsigned long>(seg.segmentId),
               (reason && reason[0]) ? reason : "-",
               static_cast<unsigned long>(checkpoint.recordCount),
               static_cast<unsigned long>(checkpoint.eventCount),
               static_cast<unsigned long>(checkpoint.enrichDeltaCount));
    return true;
}

bool StorageManager::_serviceDeferredBinaryCheckpoint(const char* reason,
                                                      bool allowDuringCapture) {
    if (!_binaryCheckpointDeferred) {
        return true;
    }

    if (!allowDuringCapture &&
        RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE) {
        return false;
    }

    if (_spoolIndex.activeSegmentId == 0) {
        _binaryCheckpointDeferred = false;
        return true;
    }

    SpoolSegmentInfo* seg = _findSegmentInfo(_spoolIndex.activeSegmentId);
    if (!seg) {
        _binaryCheckpointDeferred = false;
        return true;
    }

    const char* checkpointReason =
        (reason && reason[0]) ? reason : "deferred_checkpoint";
    bool wrote = false;
    if (!_maybeWriteBinarySegmentCheckpoint(*seg,
                                            checkpointReason,
                                            allowDuringCapture,
                                            &wrote)) {
        return false;
    }

    _binaryCheckpointDeferred = false;
    return true;
}

bool StorageManager::_appendSpoolEnrichmentDelta(const String& sessionId,
                                                 uint32_t eventId,
                                                 float lat, float lon,
                                                 float alt, float accuracy,
                                                 const char* tag,
                                                 bool noData) {
    if (!sessionId.length() || eventId == 0) return false;

    if (_spoolIndex.activeSegmentId == 0) {
        if (!_openNewSpoolSegment()) {
            return false;
        }
    }

    SpoolSegmentInfo* seg = _findSegmentInfo(_spoolIndex.activeSegmentId);
    if (!seg) {
        if (!_openNewSpoolSegment()) {
            return false;
        }
        seg = _findSegmentInfo(_spoolIndex.activeSegmentId);
        if (!seg) return false;
    }

    if (seg->format != SPOOL_SEGMENT_BIN_V2) {
        DLOG_WARN("STORAGE",
                  "Enrich delta append requires binary active segment seg=%lu format=%s",
                  static_cast<unsigned long>(seg->segmentId),
                  _segmentFormatText(seg->format));
        return false;
    }

    ScopedSemaphoreLock appendLock(_appendMutex);
    {
    const uint32_t recordId = _nextEventId++;
    _spoolIndex.nextEventId = _nextEventId;
    const uint32_t ts = millis();

    _eventCounterDirty = true;
    _eventCounterPendingWrites++;
    const bool captureDeferCounter =
        RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY;
    if (captureDeferCounter) {
        _workerMetadataDirtyPending = true;
    } else {
        const uint32_t counterWriteStart = millis();
        const bool counterOk = _persistEventCounter(false, "append_enrich_counter");
        _logCaptureWriteAllowed(PATH_EVENT_COUNTER,
                                "append_enrich_counter",
                                millis() - counterWriteStart,
                                counterOk);
        if (!counterOk) {
            return false;
        }
    }

    uint32_t tsBase = 0U;
    {
        if (_workerAppendFile && _workerAppendSegmentId == seg->segmentId) {
            if (!_flushWorkerAppendFile("append_enrich_sync", true)) {
                return false;
            }
        }

        File hdrFile = LittleFS.open(_spoolBinarySegmentPath(seg->segmentId), "r");
        if (!hdrFile) return false;

        SpoolBin::SegmentHeaderV2 segHdr;
        if (!SpoolBin::readSegmentHeaderV2(hdrFile, segHdr)) {
            hdrFile.close();
            return false;
        }
        hdrFile.close();

        tsBase = segHdr.createdMs;
    }

    const uint32_t tsDelta = _timestampDeltaFromBase(ts, tsBase);

    std::vector<uint8_t> body;
    body.reserve(64);

    const String lastSession = _binaryLastSessionBySegment[seg->segmentId];
    const bool sameSession = (sessionId == lastSession);

    uint8_t enrichFlags = 0;
    const String tagStr = String(tag ? tag : "");
    if (tagStr.length()) {
        enrichFlags |= ENRICH_FLAG_TAG;
    }
    if (noData) {
        enrichFlags |= ENRICH_FLAG_NO_DATA;
    }

    BinaryEnrichContext& enrichCtx = _binaryEnrichCtxBySegment[seg->segmentId];
    _buildBinaryEnrichDeltaBodyV2(body, enrichCtx, recordId, tsDelta,
                                  sameSession, sessionId, String(),
                                  enrichFlags, eventId,
                                  _floatToE7(lat), _floatToE7(lon),
                                  _floatToCm(alt), _floatToDm(accuracy),
                                  tagStr, 0U);

    SpoolBin::SegmentHeaderV2 hdr;
    const uint32_t appendStartMs = millis();
    const bool appendOk = SpoolBin::appendRecordV2(
            _spoolBinarySegmentPath(seg->segmentId),
            static_cast<uint8_t>(SpoolBin::REC_ENRICH_DELTA_V2),
            body.data(),
            static_cast<uint16_t>(body.size()),
            recordId,
            &hdr);
    const uint32_t appendMs = millis() - appendStartMs;
    if (appendMs >= 250UL) {
        DLOG_WARN("STORAGE",
                  "Spool enrich append slow ms=%lu seg=%lu record=%lu event=%lu bytes=%u",
                  static_cast<unsigned long>(appendMs),
                  static_cast<unsigned long>(seg->segmentId),
                  static_cast<unsigned long>(recordId),
                  static_cast<unsigned long>(eventId),
                  static_cast<unsigned>(body.size()));
    }
    if (!appendOk) {
        return false;
    }

    _bumpStorageMetaGeneration();

    seg->firstEventId = hdr.firstEventId;
    seg->lastEventId = hdr.lastEventId;
    seg->recordCount = hdr.recordCount;
    seg->approxBytes = hdr.bodyBytes + sizeof(SpoolBin::SegmentHeaderV2);

    _markSegmentEnrichmentDelta(*seg, recordId, ts);
    _decrementPendingEnrichmentForEvent(eventId);

    // Enrich deltas never carry a session_tag, so writing an inline session
    // here clears the sticky tag; otherwise the next event record could emit
    // SAME_AS_PREV against a tag the reader has already dropped.
    _binaryLastSessionBySegment[seg->segmentId] = sessionId;
    _binaryLastSessionTagBySegment[seg->segmentId] = "";

    const bool rotateAfterAppend = _shouldRotateSegmentAfterAppend(*seg);
    const bool checkpointForce = rotateAfterAppend;
    (void)_maybeWriteBinarySegmentCheckpoint(
        *seg,
        checkpointForce ? "append_rotate" : "append_periodic",
        checkpointForce,
        nullptr);
    const bool logEnrichBatchDiag = (seg->recordCount % 64U) == 0U;

    if (rotateAfterAppend) {
        if (!_openNewSpoolSegment()) {
            return false;
        }
        _logSpoolDiagnostics("append_enrich_rotate");
    } else {
        if (captureDeferCounter) {
            _spoolIndexDirty = true;
            if (_spoolIndexPendingWrites < 0xFFFFu) {
                _spoolIndexPendingWrites++;
            }
            _workerMetadataDirtyPending = true;
        } else {
            const uint32_t indexWriteStart = millis();
            const bool indexOk = _persistSpoolIndex(false, "append_enrich_index");
            _logCaptureWriteAllowed(_spoolIndexPath().c_str(),
                                    "append_enrich_index",
                                    millis() - indexWriteStart,
                                    indexOk);
            if (!indexOk) {
                return false;
            }
        }
    }

    if (logEnrichBatchDiag) {
        _logSpoolDiagnostics("append_enrich_batch");
    }
    }

#if SPECTRE_TRACE_ENRICH_DELTA
    DLOG_INFO("STORAGE", "Spool enrich_delta session=%s event=%lu record=%lu",
              sessionId.c_str(),
              static_cast<unsigned long>(eventId),
              static_cast<unsigned long>(recordId));
#endif
    ENTITY_MGR.applyEnrichment(eventId, lat, lon, alt, accuracy, noData);
    _refreshFsStats(true);
    updateStoragePressure(false);
    if (_pressureMode >= STORAGE_MODE_WATCH) {
        requestMaintenance(STORAGE_MAINT_DELETE_DRAINED,
                           "enrich_pressure_reclaim");
    }
    _queueStorageUiRefresh(true);
    return true;
}

bool StorageManager::_forEachResolvedEventForSession(
    const String& sessionId,
    uint32_t sinceId,
    int maxCount,
    const std::function<bool(JsonObjectConst)>& cb) const {

    if (!sessionId.length()) {
        return true;
    }

    DLOG_DEBUG("STORAGE",
              "Resolved scan begin session=%s since=%lu max=%d",
              sessionId.c_str(),
              static_cast<unsigned long>(sinceId),
              maxCount);

    std::vector<SpoolEnrichmentDelta> spoolEnrichments;
    DLOG_DEBUG("STORAGE", "Resolved scan loading enrichments");
    if (!_loadSpoolEnrichmentsForSession(sessionId, spoolEnrichments)) {
        DLOG_WARN("STORAGE", "Failed to load spool enrichments for session=%s",
                  sessionId.c_str());
    }
    DLOG_DEBUG("STORAGE",
              "Resolved scan enrichments loaded count=%u",
              static_cast<unsigned>(spoolEnrichments.size()));

    std::map<uint32_t, SpoolEnrichmentDelta> enrichById;
    for (const auto& e : spoolEnrichments) {
        if (e.id != 0) {
            enrichById[e.id] = e;
        }
    }

    const uint32_t watermark = _uploadedWatermarkForSession(sessionId);
    int emitted = 0;

    for (const auto& seg : _spoolIndex.segments) {
        DLOG_DEBUG("STORAGE",
                  "Resolved scan segment=%lu format=%u emitted=%d",
                  static_cast<unsigned long>(seg.segmentId),
                  static_cast<unsigned>(seg.format),
                  emitted);
        bool callbackFailed = false;
        const bool ok = _scanSegmentRecords(seg.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                if (maxCount > 0 && emitted >= maxCount) {
                    return false;
                }

                if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
                    return true;
                }

                if (!rec.sessionId.length() || rec.sessionId != sessionId) {
                    return true;
                }

                if (!rec.eventId || rec.eventId <= sinceId) {
                    return true;
                }

                if (emitted == 0) {
                    DLOG_DEBUG("STORAGE",
                              "Resolved scan first event candidate seg=%lu event=%lu",
                              static_cast<unsigned long>(seg.segmentId),
                              static_cast<unsigned long>(rec.eventId));
                }

                JsonDocument materialized;
                JsonObject dst = materialized.to<JsonObject>();
                for (JsonPairConst kv : rec.doc.as<JsonObjectConst>()) {
                    dst[kv.key().c_str()].set(kv.value());
                }

                if (emitted == 0) {
                    const char* type = dst["type"] | "";
                    DLOG_DEBUG("STORAGE",
                              "Resolved scan first event materialized event=%lu type=%s",
                              static_cast<unsigned long>(rec.eventId),
                              type);
                }

                EventStatus status = EVT_RAW;

                auto it = enrichById.find(rec.eventId);
                if (it != enrichById.end()) {
                    const SpoolEnrichmentDelta& enrichment = it->second;
                    if (enrichment.noData) {
                        // NO_DATA is terminal, but never a zero-coordinate fix.
                        dst[F_ENRICH_STATE] =
                            static_cast<uint8_t>(STORAGE_ENRICH_NO_DATA);
                        dst["enriched_ts"] = enrichment.ts;
                    } else {
                        dst["lat"] = enrichment.lat;
                        dst["lon"] = enrichment.lon;
                        dst["alt"] = enrichment.alt;
                        dst["acc"] = enrichment.acc;
                        if (enrichment.tag[0]) {
                            dst["tag"] = enrichment.tag;
                        }
                        dst["enriched_ts"] = enrichment.ts;
                        if (enrichment.gpsTs >= MIN_ENRICH_GPS_EPOCH) {
                            dst[F_GPS_TS] = enrichment.gpsTs;
                        }
                        dst[F_ENRICH_STATE] =
                            static_cast<uint8_t>(STORAGE_ENRICH_DONE);
                        status = EVT_ENRICHED;
                    }
                } else if (_eventRecordPendingEnrichment(
                               rec.doc.as<JsonObjectConst>())) {
                    dst[F_ENRICH_STATE] =
                        static_cast<uint8_t>(STORAGE_ENRICH_PENDING);
                }

                if (rec.eventId <= watermark) {
                    dst["uploaded_ts"] = millis();
                    status = EVT_UPLOADED;
                }

                dst["status"] = status;
                emitted++;
                JsonObjectConst dstConst(dst);
                if (emitted == 1) {
                    DLOG_DEBUG("STORAGE", "Resolved scan invoking callback");
                }
                if (!cb(dstConst)) {
                    callbackFailed = true;
                    return false;
                }
                if (emitted == 1) {
                    DLOG_DEBUG("STORAGE", "Resolved scan callback returned");
                }
                return true;
            });

        if (!ok || callbackFailed) {
            return false;
        }

        if (maxCount > 0 && emitted >= maxCount) {
            break;
        }
    }

    return true;
}

bool StorageManager::_getNextUploadEventForSessionFromIndex(const String& sessionId,
                                                            uint32_t sinceId,
                                                            JsonDocument& out,
                                                            bool& found) {
    out.clear();
    found = false;

    if (!sessionId.length()) {
        return true;
    }

    auto sessionIt = _backlog.uploadIndexBySession.find(sessionId);
    if (sessionIt == _backlog.uploadIndexBySession.end()) {
        return true;
    }

    UploadIndexRecordV1 ptr{};
    bool havePtr = false;
    const UploadIndexPagedSession& ptrs = sessionIt->second;
    for (const auto& pagePtr : ptrs.pages) {
        if (!pagePtr) {
            continue;
        }

        const UploadIndexPage& page = *pagePtr;
        for (uint8_t i = 0; i < page.count; ++i) {
            const UploadIndexRecordV1& rec = page.records[i];
            if (rec.eventId > sinceId) {
                ptr = rec;
                havePtr = true;
                break;
            }
        }

        if (havePtr) {
            break;
        }
    }

    if (!havePtr) {
        return true;
    }

    const SpoolSegmentInfo* seg = _findSegmentInfo(ptr.segmentId);
    if (!seg) {
        _releaseUploadIndexMemory("index_next_missing_segment");
        return false;
    }

    const String path = _spoolSegmentPathForFormat(ptr.segmentId, seg->format);

    if (!_ensureUploadSegmentFileOpen(ptr.segmentId, seg->format, path)) {
        DLOG_WARN("STORAGE",
                  "Upload index next open failed path=%s",
                  path.c_str());
        return false;
    }

    File& f = _backlog.uploadReadFile;
    const uint32_t fileSize = _backlog.uploadReadFileSize;
    const SpoolBin::SegmentHeaderV2& hdr = _backlog.uploadReadHeader;
    const bool headerOk = _backlog.uploadReadHeaderOk;

    if (!headerOk) {
        _closeUploadSegmentFile("header_invalid");
        return false;
    }

    DecodedSpoolRecord rec;
    rec.recordType = SPOOL_REC_UNKNOWN;
    rec.eventId = 0;
    rec.sessionId = "";
    rec.doc.clear();

    const bool boundsOk =
        ptr.len != 0 &&
        ptr.offset < fileSize &&
        ptr.len <= (fileSize - ptr.offset);
    bool ok = _validateUploadIndexRecord(ptr, ptr.segmentId) && boundsOk;

    if (ok && seg->format == SPOOL_SEGMENT_BIN_V2) {
        ok = f.seek(ptr.offset);

        SpoolBin::RecordPrefix prefix{};
        if (ok) {
            ok = SpoolBin::readBytes(f, &prefix, sizeof(prefix));
        }

        std::vector<uint8_t> body;
        if (ok) {
            const uint32_t encodedLen =
                static_cast<uint32_t>(sizeof(prefix)) +
                static_cast<uint32_t>(prefix.length);
            ok = encodedLen == ptr.len;
        }

        if (ok) {
            body.resize(prefix.length);
            if (prefix.length > 0) {
                ok = SpoolBin::readBytes(f, body.data(), prefix.length);
            }
        }

        if (ok) {
            ok = _decodeBinarySpoolRecordBody(ptr.segmentId,
                                              prefix.type,
                                              body.data(),
                                              body.size(),
                                              hdr.createdMs,
                                              hdr.createdEpochUtc,
                                              String(ptr.sessionId),
                                              rec);
        }
    } else if (ok) {
        ok = f.seek(ptr.offset);

        std::vector<char> buf;
        if (ok) {
            buf.assign(ptr.len + 1U, '\0');
            ok = f.readBytes(buf.data(), ptr.len) == ptr.len;
        }

        if (ok) {
            JsonDocument doc;
            ok = !deserializeJson(doc, buf.data());
            if (ok) {
                rec.recordType = SPOOL_REC_EVENT;
                rec.eventId = doc["id"] | 0U;
                rec.sessionId =
                    String((const char*)(doc[F_SESSION] | doc["session_id"] | ""));
                rec.doc.set(doc.as<JsonVariantConst>());
            }
        }
    }

    // File stays open across consecutive same-segment reads; closed by
    // _closeUploadSegmentFile on segment change or upload index teardown.

    ok = ok &&
         rec.recordType == SPOOL_REC_EVENT &&
         rec.eventId == ptr.eventId &&
         rec.sessionId == sessionId;

    if (!ok) {
        // Close on read failure so the next attempt re-opens cleanly.
        _closeUploadSegmentFile("read_failed");
        DLOG_WARN("STORAGE",
                  "Upload index next read failed seg=%lu event=%lu off=%lu len=%lu",
                  static_cast<unsigned long>(ptr.segmentId),
                  static_cast<unsigned long>(ptr.eventId),
                  static_cast<unsigned long>(ptr.offset),
                  static_cast<unsigned long>(ptr.len));
        return false;
    }

    JsonObject dst = out.to<JsonObject>();
    if (dst.isNull()) {
        DLOG_WARN("STORAGE",
                  "Upload index next alloc failed session=%s event=%lu",
                  sessionId.c_str(),
                  static_cast<unsigned long>(rec.eventId));
        return false;
    }
    for (JsonPairConst kv : rec.doc.as<JsonObjectConst>()) {
        dst[kv.key().c_str()].set(kv.value());
    }

    EventStatus status = EVT_RAW;

    // Enrichment is persisted as a separate delta record. Phone offload uses
    // this single-record reader, so it must resolve the delta just like the
    // indexed batch/MQTT reader below. Otherwise the phone receives the
    // original observation body with no coordinates even after a successful
    // GPS match.
    const SpoolEnrichmentDelta* enrichment = nullptr;
    auto enrichSessionIt = _backlog.uploadEnrichBySession.find(sessionId);
    if (enrichSessionIt != _backlog.uploadEnrichBySession.end()) {
        auto deltaIt = enrichSessionIt->second.find(rec.eventId);
        if (deltaIt != enrichSessionIt->second.end()) {
            enrichment = &deltaIt->second;
        }
    }

    if (enrichment) {
        if (enrichment->noData) {
            dst[F_ENRICH_STATE] =
                static_cast<uint8_t>(STORAGE_ENRICH_NO_DATA);
            dst["enriched_ts"] = enrichment->ts;
        } else {
            dst["lat"] = enrichment->lat;
            dst["lon"] = enrichment->lon;
            dst["alt"] = enrichment->alt;
            dst["acc"] = enrichment->acc;
            if (enrichment->tag[0]) {
                dst["tag"] = enrichment->tag;
            }
            dst["enriched_ts"] = enrichment->ts;
            if (enrichment->gpsTs >= MIN_ENRICH_GPS_EPOCH) {
                dst[F_GPS_TS] = enrichment->gpsTs;
            }
            dst[F_ENRICH_STATE] =
                static_cast<uint8_t>(STORAGE_ENRICH_DONE);
            status = EVT_ENRICHED;
        }
    } else if (_eventRecordPendingEnrichment(rec.doc.as<JsonObjectConst>())) {
        dst[F_ENRICH_STATE] =
            static_cast<uint8_t>(STORAGE_ENRICH_PENDING);
    }

    const uint32_t watermark = _uploadedWatermarkForSession(sessionId);
    if (rec.eventId <= watermark) {
        dst["uploaded_ts"] = millis();
        status = EVT_UPLOADED;
    }

    dst["status"] = status;
    found = true;
    return true;
}

bool StorageManager::_getUploadEventBatchForSessionFromIndex(const String& sessionId,
                                                             uint32_t sinceId,
                                                             int maxCount,
                                                             JsonDocument& out) {
    out.clear();
    JsonArray batch = out.to<JsonArray>();
    const bool probeFetch =
        sinceId >= 128U && sinceId <= 160U &&
        sessionId == "33e864-3099-39da1a93";
    if (batch.isNull()) {
        DLOG_WARN("STORAGE",
                  "Upload index batch alloc failed session=%s since=%lu max=%d",
                  sessionId.c_str(),
                  static_cast<unsigned long>(sinceId),
                  maxCount);
        return false;
    }

    if (!sessionId.length() || maxCount <= 0) {
        return true;
    }

    if (probeFetch) {
        DLOG_INFO("STORAGE",
                  "Upload index fetch probe stage=enter session=%s since=%lu max=%d heapFree=%lu psramFree=%lu",
                  sessionId.c_str(),
                  static_cast<unsigned long>(sinceId),
                  maxCount,
                  static_cast<unsigned long>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
                  static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }

    DLOG_UPLOAD_TRACE("STORAGE",
               "Upload index batch enter session=%s since=%lu max=%d",
               sessionId.c_str(),
               static_cast<unsigned long>(sinceId),
               maxCount);

    auto resetOutRecord = [](DecodedSpoolRecord& rec) {
        rec.recordType = SPOOL_REC_UNKNOWN;
        rec.eventId = 0;
        rec.sessionId = "";
        rec.doc.clear();
    };

    static constexpr uint8_t UPLOAD_FETCH_STACK_CAPACITY = 32;
    UploadIndexRecordV1 selected[UPLOAD_FETCH_STACK_CAPACITY];
    uint8_t selectedCount = 0;
    const uint8_t selectLimit =
        static_cast<uint8_t>(
            std::min<int>(maxCount, UPLOAD_FETCH_STACK_CAPACITY));
    auto sessionIt = _backlog.uploadIndexBySession.find(sessionId);
    if (sessionIt == _backlog.uploadIndexBySession.end()) {
        return true;
    }

    const UploadIndexPagedSession& ptrs = sessionIt->second;
    for (const auto& pagePtr : ptrs.pages) {
        if (!pagePtr) {
            continue;
        }

        const UploadIndexPage& page = *pagePtr;
        for (uint8_t i = 0; i < page.count; ++i) {
            const UploadIndexRecordV1& rec = page.records[i];
            if (rec.eventId <= sinceId) {
                continue;
            }
            selected[selectedCount++] = rec;
            if (selectedCount >= selectLimit) {
                break;
            }
        }

        if (selectedCount >= selectLimit) {
            break;
        }
    }

    DLOG_UPLOAD_TRACE("STORAGE",
               "Upload index batch selected count=%u first=%lu last=%lu",
               static_cast<unsigned>(selectedCount),
               selectedCount ? static_cast<unsigned long>(selected[0].eventId) : 0UL,
               selectedCount ? static_cast<unsigned long>(selected[selectedCount - 1].eventId) : 0UL);
    if (probeFetch) {
        DLOG_INFO("STORAGE",
                  "Upload index fetch probe stage=selected count=%u first=%lu last=%lu",
                  static_cast<unsigned>(selectedCount),
                  selectedCount ? static_cast<unsigned long>(selected[0].eventId) : 0UL,
                  selectedCount ? static_cast<unsigned long>(selected[selectedCount - 1].eventId) : 0UL);
    }

    if (selectedCount == 0) {
        if (probeFetch) {
            DLOG_INFO("STORAGE",
                      "Upload index fetch probe stage=empty session=%s since=%lu",
                      sessionId.c_str(),
                      static_cast<unsigned long>(sinceId));
        }
        return true;
    }

    // Enrichment deltas are stored as separate records keyed by event_id, so
    // every reader that emits events for output has to join them back on. The
    // resident map is built alongside the upload index in _rebuildUploadIndex();
    // without this lookup the upload path ships each event with its
    // pre-enrichment body and a PENDING marker, silently discarding every fix
    // the phone applied. Mirrors _forEachResolvedEventForSession().
    const UploadEnrichDeltaMap* sessionEnrich = nullptr;
    {
        auto enrichIt = _backlog.uploadEnrichBySession.find(sessionId);
        if (enrichIt != _backlog.uploadEnrichBySession.end()) {
            sessionEnrich = &enrichIt->second;
        }
    }

    auto appendDecodedEvent = [&](const DecodedSpoolRecord& rec) -> bool {
        JsonObject dst = batch.add<JsonObject>();
        if (dst.isNull()) {
            DLOG_WARN("STORAGE",
                      "Upload index batch alloc failed session=%s event=%lu",
                      sessionId.c_str(),
                      static_cast<unsigned long>(rec.eventId));
            return false;
        }

        for (JsonPairConst kv : rec.doc.as<JsonObjectConst>()) {
            dst[kv.key().c_str()].set(kv.value());
        }

        EventStatus status = EVT_RAW;

        const SpoolEnrichmentDelta* enrichment = nullptr;
        if (sessionEnrich) {
            auto deltaIt = sessionEnrich->find(rec.eventId);
            if (deltaIt != sessionEnrich->end()) {
                enrichment = &deltaIt->second;
            }
        }

        if (enrichment) {
            if (enrichment->noData) {
                // NO_DATA is terminal, but never a zero-coordinate fix.
                dst[F_ENRICH_STATE] =
                    static_cast<uint8_t>(STORAGE_ENRICH_NO_DATA);
                dst["enriched_ts"] = enrichment->ts;
            } else {
                dst["lat"] = enrichment->lat;
                dst["lon"] = enrichment->lon;
                dst["alt"] = enrichment->alt;
                dst["acc"] = enrichment->acc;
                if (enrichment->tag[0]) {
                    dst["tag"] = enrichment->tag;
                }
                dst["enriched_ts"] = enrichment->ts;
                if (enrichment->gpsTs >= MIN_ENRICH_GPS_EPOCH) {
                    dst[F_GPS_TS] = enrichment->gpsTs;
                }
                dst[F_ENRICH_STATE] =
                    static_cast<uint8_t>(STORAGE_ENRICH_DONE);
                status = EVT_ENRICHED;
            }
        } else if (_eventRecordPendingEnrichment(rec.doc.as<JsonObjectConst>())) {
            dst[F_ENRICH_STATE] =
                static_cast<uint8_t>(STORAGE_ENRICH_PENDING);
        }

        const uint32_t watermark = _uploadedWatermarkForSession(sessionId);
        if (rec.eventId <= watermark) {
            dst["uploaded_ts"] = millis();
            status = EVT_UPLOADED;
        }

        dst["status"] = status;
        return true;
    };

    uint8_t selectedIndex = 0;
    while (selectedIndex < selectedCount) {
        const uint32_t segmentId = selected[selectedIndex].segmentId;
        const SpoolSegmentInfo* seg = _findSegmentInfo(segmentId);
        if (!seg) {
            _releaseUploadIndexMemory("index_missing_segment");
            return false;
        }

        const String path = _spoolSegmentPathForFormat(segmentId, seg->format);
        if (probeFetch) {
            DLOG_INFO("STORAGE",
                      "Upload index fetch probe stage=open seg=%lu fmt=%u path=%s selectedIndex=%u",
                      static_cast<unsigned long>(segmentId),
                      static_cast<unsigned>(seg->format),
                      path.c_str(),
                      static_cast<unsigned>(selectedIndex));
        }
        DLOG_UPLOAD_TRACE("STORAGE",
               "Upload index batch opening seg=%lu selectedIndex=%u",
                   static_cast<unsigned long>(segmentId),
                   static_cast<unsigned>(selectedIndex));
        if (!_ensureUploadSegmentFileOpen(segmentId, seg->format, path)) {
            if (probeFetch) {
                DLOG_WARN("STORAGE",
                          "Upload index fetch probe stage=open_failed seg=%lu",
                          static_cast<unsigned long>(segmentId));
            }
            return false;
        }
        File& f = _backlog.uploadReadFile;
        const uint32_t fileSize = _backlog.uploadReadFileSize;
        if (probeFetch) {
            DLOG_INFO("STORAGE",
                      "Upload index fetch probe stage=opened seg=%lu size=%lu",
                      static_cast<unsigned long>(segmentId),
                      static_cast<unsigned long>(fileSize));
        }
        DLOG_UPLOAD_TRACE("STORAGE",
               "Upload index batch opened seg=%lu size=%lu",
                   static_cast<unsigned long>(segmentId),
                   static_cast<unsigned long>(fileSize));

        const SpoolBin::SegmentHeaderV2& hdr = _backlog.uploadReadHeader;
        const bool headerOk =
            seg->format != SPOOL_SEGMENT_BIN_V2 || _backlog.uploadReadHeaderOk;

        if (!headerOk) {
            if (probeFetch) {
                DLOG_WARN("STORAGE",
                          "Upload index fetch probe stage=header_failed seg=%lu",
                          static_cast<unsigned long>(segmentId));
            }
            _closeUploadSegmentFile("upload_batch_header_failed");
            return false;
        }
        if (probeFetch) {
            DLOG_INFO("STORAGE",
                      "Upload index fetch probe stage=header_ok seg=%lu records=%lu",
                      static_cast<unsigned long>(segmentId),
                      static_cast<unsigned long>(hdr.recordCount));
        }
        DLOG_UPLOAD_TRACE("STORAGE",
               "Upload index batch header ok seg=%lu records=%lu",
                   static_cast<unsigned long>(segmentId),
                   static_cast<unsigned long>(hdr.recordCount));

        while (selectedIndex < selectedCount &&
               selected[selectedIndex].segmentId == segmentId) {
            const UploadIndexRecordV1& ptr = selected[selectedIndex];
            DecodedSpoolRecord rec;
            resetOutRecord(rec);
            if (probeFetch) {
                DLOG_INFO("STORAGE",
                          "Upload index fetch probe stage=ptr idx=%u seg=%lu event=%lu off=%lu len=%lu lane=%u",
                          static_cast<unsigned>(selectedIndex),
                          static_cast<unsigned long>(ptr.segmentId),
                          static_cast<unsigned long>(ptr.eventId),
                          static_cast<unsigned long>(ptr.offset),
                          static_cast<unsigned long>(ptr.len),
                          static_cast<unsigned>(ptr.lane));
            }

            DLOG_UPLOAD_TRACE("STORAGE",
               "Upload index batch ptr seg=%lu event=%lu",
                       static_cast<unsigned long>(ptr.segmentId),
                       static_cast<unsigned long>(ptr.eventId));

            const bool boundsOk =
                ptr.len != 0 &&
                ptr.offset < fileSize &&
                ptr.len <= (fileSize - ptr.offset);
            bool ok = _validateUploadIndexRecord(ptr, ptr.segmentId) &&
                      boundsOk;

            if (ok && seg->format == SPOOL_SEGMENT_BIN_V2) {
                ok = f.seek(ptr.offset);

                SpoolBin::RecordPrefix prefix{};
                if (ok) {
                    ok = SpoolBin::readBytes(f, &prefix, sizeof(prefix));
                }

                std::vector<uint8_t> body;
                if (ok) {
                    const uint32_t encodedLen =
                        static_cast<uint32_t>(sizeof(prefix)) +
                        static_cast<uint32_t>(prefix.length);
                    ok = encodedLen == ptr.len;
                    if (probeFetch) {
                        DLOG_INFO("STORAGE",
                                  "Upload index fetch probe stage=prefix event=%lu type=%u prefixLen=%u encoded=%lu ok=%u",
                                  static_cast<unsigned long>(ptr.eventId),
                                  static_cast<unsigned>(prefix.type),
                                  static_cast<unsigned>(prefix.length),
                                  static_cast<unsigned long>(encodedLen),
                                  ok ? 1U : 0U);
                    }
                }

                if (ok) {
                    body.resize(prefix.length);
                    if (prefix.length > 0) {
                        ok = SpoolBin::readBytes(f, body.data(), prefix.length);
                    }
                    if (probeFetch) {
                        DLOG_INFO("STORAGE",
                                  "Upload index fetch probe stage=body event=%lu bytes=%u ok=%u",
                                  static_cast<unsigned long>(ptr.eventId),
                                  static_cast<unsigned>(body.size()),
                                  ok ? 1U : 0U);
                    }
                }

                if (ok) {
                    ok = _decodeBinarySpoolRecordBody(ptr.segmentId,
                                                      prefix.type,
                                                      body.data(),
                                                      body.size(),
                                                      hdr.createdMs,
                                                      hdr.createdEpochUtc,
                                                      String(ptr.sessionId),
                                                      rec);
                    if (probeFetch) {
                        DLOG_INFO("STORAGE",
                                  "Upload index fetch probe stage=decoded event=%lu ok=%u recType=%u recEvent=%lu recSession=%s",
                                  static_cast<unsigned long>(ptr.eventId),
                                  ok ? 1U : 0U,
                                  static_cast<unsigned>(rec.recordType),
                                  static_cast<unsigned long>(rec.eventId),
                                  rec.sessionId.c_str());
                    }
                    DLOG_UPLOAD_TRACE("STORAGE",
               "Upload index batch decoded ok=%d event=%lu",
                               ok ? 1 : 0,
                               static_cast<unsigned long>(ptr.eventId));
                }
            } else if (ok) {
                ok = f.seek(ptr.offset);

                std::vector<char> buf;
                if (ok) {
                    buf.assign(ptr.len + 1U, '\0');
                    ok = f.readBytes(buf.data(), ptr.len) == ptr.len;
                }

                if (ok) {
                    JsonDocument doc;
                    ok = !deserializeJson(doc, buf.data());
                    if (ok) {
                        rec.recordType = SPOOL_REC_EVENT;
                        rec.eventId = doc["id"] | 0U;
                        rec.sessionId =
                            String((const char*)(doc[F_SESSION] | doc["session_id"] | ""));
                        rec.doc.set(doc.as<JsonVariantConst>());
                    }
                }
            }

            ok = ok &&
                 rec.recordType == SPOOL_REC_EVENT &&
                 rec.eventId == ptr.eventId &&
                 rec.sessionId == sessionId;

            if (!ok) {
                DLOG_WARN("STORAGE",
                          "Upload index batch read failed seg=%lu event=%lu off=%lu len=%lu",
                          static_cast<unsigned long>(ptr.segmentId),
                          static_cast<unsigned long>(ptr.eventId),
                          static_cast<unsigned long>(ptr.offset),
                          static_cast<unsigned long>(ptr.len));
                _closeUploadSegmentFile("upload_batch_read_failed");
                return false;
            }

            if (!appendDecodedEvent(rec)) {
                return false;
            }
            if (probeFetch) {
                DLOG_INFO("STORAGE",
                          "Upload index fetch probe stage=appended event=%lu batch=%u heapFree=%lu",
                          static_cast<unsigned long>(rec.eventId),
                          static_cast<unsigned>(batch.size()),
                          static_cast<unsigned long>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)));
            }
            DLOG_UPLOAD_TRACE("STORAGE",
               "Upload index batch appended event=%lu size=%u",
                       static_cast<unsigned long>(rec.eventId),
                       static_cast<unsigned>(batch.size()));
            selectedIndex++;
        }

    }

    DLOG_UPLOAD_TRACE("STORAGE",
               "Upload index batch done count=%u",
               static_cast<unsigned>(batch.size()));

    return true;
}

bool StorageManager::_getEventBatchForSessionFromSpool(const String& sessionId,
                                                       uint32_t sinceId,
                                                       int maxCount,
                                                       JsonDocument& out) {
    out.clear();
    JsonArray batch = out.to<JsonArray>();

    if (!sessionId.length() || maxCount <= 0) {
        return true;
    }

    struct BatchedEvent {
        uint32_t eventId = 0;
        uint32_t epochUtc = 0;
        JsonDocument doc;
    };

    std::vector<BatchedEvent> events;
    events.reserve(static_cast<size_t>(maxCount));

    std::map<uint32_t, SpoolEnrichmentDelta> enrichById;

    for (const auto& seg : _spoolIndex.segments) {
        if (seg.lastEventId != 0 && seg.lastEventId <= sinceId) {
            continue;
        }

        const bool ok = _scanSegmentRecords(seg.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                if (rec.recordType != SPOOL_REC_EVENT ||
                    !rec.sessionId.length() ||
                    rec.sessionId != sessionId ||
                    !rec.eventId ||
                    rec.eventId <= sinceId) {
                    return true;
                }

                if (events.size() >= static_cast<size_t>(maxCount)) {
                    return false;
                }

                BatchedEvent event;
                event.eventId = rec.eventId;
                event.epochUtc = rec.epochUtc;
                event.doc.set(rec.doc.as<JsonVariantConst>());
                events.push_back(std::move(event));
                return true;
            });

        if (!ok) {
            return false;
        }

        if (events.size() >= static_cast<size_t>(maxCount)) {
            break;
        }
    }

    if (!events.empty()) {
        auto selectedIndex = [&](uint32_t eventId) -> int {
            for (size_t i = 0; i < events.size(); ++i) {
                if (events[i].eventId == eventId) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };

        for (const auto& seg : _spoolIndex.segments) {
            const bool summaryReady =
                seg.summaryValid &&
                seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;

            if (summaryReady && seg.enrichDeltaCount == 0) {
                continue;
            }

            const bool ok = _scanSegmentRecords(seg.segmentId,
                [&](const DecodedSpoolRecord& rec) -> bool {
                    if (rec.recordType != SPOOL_REC_ENRICH_DELTA ||
                        !rec.sessionId.length() ||
                        rec.sessionId != sessionId) {
                        return true;
                    }

                    SpoolEnrichmentDelta enrichment;
                    enrichment.id = rec.doc["event_id"] | 0U;
                    if (enrichment.id == 0 || selectedIndex(enrichment.id) < 0) {
                        return true;
                    }

                    enrichment.lat = rec.doc["lat"] | 0.0f;
                    enrichment.lon = rec.doc["lon"] | 0.0f;
                    enrichment.alt = rec.doc["alt"] | 0.0f;
                    enrichment.acc = rec.doc["acc"] | 0.0f;
                    strlcpy(enrichment.tag,
                            (const char*)(rec.doc["tag"] | ""),
                            sizeof(enrichment.tag));
                    enrichment.ts = rec.doc["ts"] | 0U;
                    enrichment.gpsTs = rec.doc[F_GPS_TS] | 0U;
                    enrichment.noData = rec.doc["enrich_no_data"] | false;
                    enrichById[enrichment.id] = enrichment;
                    return enrichById.size() < events.size();
                });

            if (!ok) {
                return false;
            }

            if (enrichById.size() >= events.size()) {
                break;
            }
        }
    }

    const uint32_t watermark = _uploadedWatermarkForSession(sessionId);

    uint32_t nowEpoch = 0;
    const bool haveNowEpoch = TIME_SVC.epochForMillis(millis(), nowEpoch);

    for (const auto& event : events) {
        const bool alreadyUploaded = (event.eventId <= watermark);
        auto it = enrichById.find(event.eventId);
        const bool hasDelta = (it != enrichById.end());

        // Hold recent enrichable records behind the upload watermark briefly.
        if (!alreadyUploaded && !hasDelta && event.epochUtc != 0 &&
            haveNowEpoch && nowEpoch >= event.epochUtc &&
            (nowEpoch - event.epochUtc) < UPLOAD_ENRICH_GRACE_SEC &&
            _eventRecordPendingEnrichment(event.doc.as<JsonObjectConst>())) {
            break;
        }

        JsonObject dst = batch.add<JsonObject>();
        for (JsonPairConst kv : event.doc.as<JsonObjectConst>()) {
            dst[kv.key().c_str()].set(kv.value());
        }

        EventStatus status = EVT_RAW;
        if (it != enrichById.end()) {
            const SpoolEnrichmentDelta& enrichment = it->second;
            if (enrichment.noData) {
                // NO_DATA is terminal, but never a zero-coordinate fix.
                dst[F_ENRICH_STATE] =
                    static_cast<uint8_t>(STORAGE_ENRICH_NO_DATA);
                dst["enriched_ts"] = enrichment.ts;
            } else {
                dst["lat"] = enrichment.lat;
                dst["lon"] = enrichment.lon;
                dst["alt"] = enrichment.alt;
                dst["acc"] = enrichment.acc;
                if (enrichment.tag[0]) {
                    dst["tag"] = enrichment.tag;
                }
                dst["enriched_ts"] = enrichment.ts;
                if (enrichment.gpsTs >= MIN_ENRICH_GPS_EPOCH) {
                    dst[F_GPS_TS] = enrichment.gpsTs;
                }
                dst[F_ENRICH_STATE] =
                    static_cast<uint8_t>(STORAGE_ENRICH_DONE);
                status = EVT_ENRICHED;
            }
        } else if (_eventRecordPendingEnrichment(event.doc.as<JsonObjectConst>())) {
            dst[F_ENRICH_STATE] =
                static_cast<uint8_t>(STORAGE_ENRICH_PENDING);
        }

        if (event.eventId <= watermark) {
            dst["uploaded_ts"] = millis();
            status = EVT_UPLOADED;
        }

        dst["status"] = status;
    }

    return true;
}

bool StorageManager::_decodeBinarySpoolRecordBody(uint32_t segmentId,
                                                  uint8_t recordType,
                                                  const uint8_t* data,
                                                  size_t len,
                                                  uint32_t tsBase,
                                                  uint32_t epochBase,
                                                  const String& sessionSeed,
                                                  DecodedSpoolRecord& out) const {
    out.recordType = SPOOL_REC_UNKNOWN;
    out.eventId = 0;
    out.sessionId = "";
    out.doc.clear();
    if (!data && len > 0) {
        return false;
    }
    if (recordType != SpoolBin::REC_EVENT) {
        return false;
    }

    const uint8_t* p = data;
    const uint8_t* end = data + len;

    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();

    uint32_t recordId = 0;
    uint32_t tsDelta = 0;
    uint8_t sessionMode = BIN_SESSION_INLINE;
    String sessionId;
    String sessionTag;
    uint8_t typeCode = BIN_EVT_CUSTOM;
    String typeStr;
    uint8_t eventFlags = 0;
    uint8_t prio = 0;
    uint8_t lane = 0;
    uint8_t payloadFamily = BIN_PAYLOAD_JSON_FALLBACK;

    if (!_readUVarintFromBytes(p, end, recordId) ||
        !_readUVarintFromBytes(p, end, tsDelta)) {
        return false;
    }

    if (p >= end) return false;
    sessionMode = *p++;

    {
        String seed = sessionSeed;
        String seedTag = _lookupSessionTag(sessionSeed);
        if (!_readBinarySessionField(p, end, sessionMode, seed, seedTag,
                                     sessionId, sessionTag)) {
            return false;
        }
        if (!sessionTag.length()) sessionTag = _lookupSessionTag(sessionId);
    }

    if (p >= end) return false;
    typeCode = *p++;

    if (typeCode == BIN_EVT_CUSTOM) {
        if (!_readStringFromBytes(p, end, typeStr)) {
            return false;
        }
    } else {
        typeStr = _binaryEventTypeStringFromCode(typeCode);
    }

    if (p >= end) return false;
    eventFlags = *p++;

    if (p >= end) return false;
    payloadFamily = *p++;

    root["id"] = recordId;
    const uint32_t ts = _timestampFromBaseDelta(tsDelta, tsBase);
    root["ts"] = ts;
    root["type"] = typeStr;
    root[F_SESSION] = sessionId;
    // Prefer the reboot-safe persisted epoch (createdEpochUtc + delta/1000);
    // projecting from a boot-relative millis is wrong for any record carried
    // across a reboot. This path used to have no epoch base at all and always
    // took the wrong branch.
    String tsIsoStr;
    {
        char tsIso[24] = {};
        const uint32_t epochUtc = _epochFromBaseDelta(tsDelta, epochBase);
        out.epochUtc = epochUtc;
        if (epochUtc != 0) {
            TIME_SVC.formatIsoForEpoch(epochUtc, tsIso, sizeof(tsIso));
        } else {
            TIME_SVC.formatIsoForMillis(ts, tsIso, sizeof(tsIso));
        }
        tsIsoStr = tsIso;
        root[F_TIMESTAMP_ISO] = tsIsoStr;
    }

    if (!_decodeBinaryPayloadBody(p, end, typeStr, payloadFamily,
                                  eventFlags, root)) {
        DLOG_WARN("STORAGE",
                  "Indexed binary payload decode failed seg=%lu family=%u",
                  static_cast<unsigned long>(segmentId),
                  static_cast<unsigned>(payloadFamily));
        return false;
    }

    if (eventFlags & BIN_EVENT_HAS_FIELDS) {
        if (!_readBinaryFieldMapFromBytes(p, end, root)) {
            DLOG_WARN("STORAGE", "Indexed binary extension decode failed seg=%lu",
                      static_cast<unsigned long>(segmentId));
            return false;
        }
    }

    const String eventTypeStr = String((const char*)(root["event_type"] | ""));
    prio = _defaultPriorityForBinaryType(typeStr, eventTypeStr);
    lane = _defaultLaneForBinaryType(typeStr, eventTypeStr);

    if (eventFlags & BIN_EVENT_HAS_PRIO) {
        if (p >= end) return false;
        prio = *p++;
    }
    if (eventFlags & BIN_EVENT_HAS_LANE) {
        if (p >= end) return false;
        lane = *p++;
    }

    root["prio"] = prio;
    root["lane"] = lane;
    root["lane_name"] = _laneText(static_cast<StorageLane>(lane));

    // See the matching note in _scanSegmentRecords: the extension map of an
    // older record can carry a stale ts_iso that must not win -- unless this
    // segment has no UTC base, in which case the stored value is the better one.
    if (out.epochUtc != 0 || !root[F_TIMESTAMP_ISO].is<const char*>() ||
        !(root[F_TIMESTAMP_ISO].as<const char*>()[0])) {
        root[F_TIMESTAMP_ISO] = tsIsoStr;
    }
    _applyDerivedEventFields(root, typeStr, sessionId, sessionTag);

    out.recordType = SPOOL_REC_EVENT;
    out.eventId = recordId;
    out.sessionId = sessionId;
    out.doc.set(doc.as<JsonVariantConst>());
    return true;
}

bool StorageManager::_loadSpoolEnrichmentsForSession(
    const String& sessionId,
    std::vector<SpoolEnrichmentDelta>& enrichments) const {

    enrichments.clear();
    if (!sessionId.length()) return true;

    for (const auto& seg : _spoolIndex.segments) {
        DLOG_DEBUG("STORAGE",
                   "Enrich load segment=%lu format=%u session=%s",
                   static_cast<unsigned long>(seg.segmentId),
                   static_cast<unsigned>(seg.format),
                   sessionId.c_str());
        const bool ok = _scanSegmentRecords(seg.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                if (rec.recordType != SPOOL_REC_ENRICH_DELTA) {
                    return true;
                }

                if (!rec.sessionId.length() || rec.sessionId != sessionId) {
                    return true;
                }

                SpoolEnrichmentDelta out;
                out.id = rec.doc["event_id"] | 0U;
                if (!out.id) {
                    return true;
                }

                DLOG_DEBUG("STORAGE",
                           "Enrich match session=%s seg=%lu event=%lu",
                           sessionId.c_str(),
                           static_cast<unsigned long>(seg.segmentId),
                           static_cast<unsigned long>(out.id));

                out.lat = rec.doc["lat"] | 0.0f;
                out.lon = rec.doc["lon"] | 0.0f;
                out.alt = rec.doc["alt"] | 0.0f;
                out.acc = rec.doc["acc"] | 0.0f;
                out.tag[0] = '\0';
                JsonVariantConst tagField = rec.doc["tag"];
                if (!tagField.isNull()) {
                    if (tagField.is<const char*>()) {
                        const char* tagText = tagField.as<const char*>();
                        if (tagText) {
                            strlcpy(out.tag, tagText, sizeof(out.tag));
                        }
                    } else {
                        DLOG_WARN("STORAGE",
                                  "Enrich tag non-string session=%s event=%lu",
                                  sessionId.c_str(),
                                  static_cast<unsigned long>(out.id));
                    }
                }
                out.ts = rec.doc["ts"] | 0U;
                out.gpsTs = rec.doc[F_GPS_TS] | 0U;
                out.noData = rec.doc["enrich_no_data"] | false;
                enrichments.push_back(out);
                return true;
            });

        if (!ok) {
            return false;
        }
    }

    return true;
}

bool StorageManager::_loadSpoolEnrichmentIds(
    const String& sessionId,
    bool filterBySession,
    SpiramVector<uint32_t>& enrichedIds) const {

    enrichedIds.clear();
    if (filterBySession && !sessionId.length()) return true;

    // Reserve up front based on segment-summary enrich-delta totals. One
    // contiguous allocation beats thousands of std::map node mallocs and
    // keeps the worst-case footprint to ~4 bytes per enriched event.
    size_t reserveCount = 0;
    for (const auto& seg : _spoolIndex.segments) {
        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;
        if (summaryReady) {
            reserveCount += seg.enrichDeltaCount;
        }
    }
    DLOG_INFO("STORAGE",
              "load_ids_reserve count=%u freeInternal=%lu largest=%lu",
              static_cast<unsigned>(reserveCount),
              static_cast<unsigned long>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
              static_cast<unsigned long>(heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM)));
    if (reserveCount > 0) {
        enrichedIds.reserve(reserveCount);
    }
    DLOG_INFO("STORAGE",
              "load_ids_reserved freeInternal=%lu",
              static_cast<unsigned long>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)));

    size_t segsScanned = 0;
    size_t segsSkipped = 0;
    for (const auto& seg : _spoolIndex.segments) {
        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;

        if (summaryReady && seg.enrichDeltaCount == 0) {
            segsSkipped++;
            continue;
        }

        DLOG_DEBUG("STORAGE",
                  "load_ids_scan seg=%u enrichDeltas=%u freeInternal=%lu largest=%lu",
                  static_cast<unsigned>(seg.segmentId),
                  static_cast<unsigned>(seg.enrichDeltaCount),
                  static_cast<unsigned long>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)),
                  static_cast<unsigned long>(heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM)));

        // Use headers-only scan: each ENRICH_DELTA record exposes its
        // targetEventId directly without a JsonDocument materialization.
        const bool ok = _scanSegmentRecordHeaders(seg.segmentId,
            [&](const DecodedSpoolRecordHeader& rec) -> bool {
                if (rec.recordType != SPOOL_REC_ENRICH_DELTA) {
                    return true;
                }

                if (filterBySession &&
                    (!rec.sessionId.length() || rec.sessionId != sessionId)) {
                    return true;
                }

                if (rec.targetEventId != 0) {
                    enrichedIds.push_back(rec.targetEventId);
                }
                return true;
            });

        segsScanned++;
        if (!ok) {
            DLOG_WARN("STORAGE",
                      "load_ids_scan_failed seg=%u",
                      static_cast<unsigned>(seg.segmentId));
            return false;
        }
    }
    DLOG_INFO("STORAGE",
              "load_ids_loop_done scanned=%u skipped=%u idsCollected=%u freeInternal=%lu",
              static_cast<unsigned>(segsScanned),
              static_cast<unsigned>(segsSkipped),
              static_cast<unsigned>(enrichedIds.size()),
              static_cast<unsigned long>(heap_caps_get_free_size(SPECTRE_CAP_DRAM)));

    // Sort + uniq so callers can std::binary_search. Duplicates from re-runs
    // of the same enrichment delta are collapsed here, matching the prior
    // std::map's set semantics.
    delay(1);
    std::sort(enrichedIds.begin(), enrichedIds.end());
    delay(1);
    enrichedIds.erase(std::unique(enrichedIds.begin(), enrichedIds.end()),
                      enrichedIds.end());
    delay(1);

    return true;
}

bool StorageManager::_reconcilePendingEnrichmentSummaries(const char* reason) {
    SpiramVector<uint32_t> enrichedIds;
    if (!_loadSpoolEnrichmentIds(String(), false, enrichedIds)) {
        DLOG_WARN("STORAGE",
                  "Pending enrich reconcile failed reason=%s stage=load_ids",
                  (reason && reason[0]) ? reason : "-");
        return false;
    }

    uint32_t pendingTotal = 0;
    uint32_t changedSegments = 0;
    for (auto& seg : _spoolIndex.segments) {
        uint32_t exactPending = 0;
        if (seg.eventCount > 0) {
            const bool ok = _scanSegmentRecordHeaders(
                seg.segmentId,
                [&](const DecodedSpoolRecordHeader& rec) -> bool {
                    if (rec.recordType != SPOOL_REC_EVENT ||
                        rec.eventId == 0 ||
                        std::binary_search(enrichedIds.begin(),
                                           enrichedIds.end(),
                                           rec.eventId)) {
                        return true;
                    }
                    const RAMSpool::CaptureClassification cls =
                        RAMSpool::classify(rec.typeString.c_str(), "");
                    if (cls.enrichEligible) {
                        exactPending++;
                    }
                    return true;
                });
            if (!ok) {
                DLOG_WARN("STORAGE",
                          "Pending enrich reconcile failed reason=%s seg=%lu stage=scan",
                          (reason && reason[0]) ? reason : "-",
                          static_cast<unsigned long>(seg.segmentId));
                return false;
            }
        }

        pendingTotal += exactPending;
        if (seg.pendingEnrichmentCount != exactPending) {
            seg.pendingEnrichmentCount = exactPending;
            changedSegments++;
        }
        delay(1);
    }

    if (changedSegments > 0) {
        _spoolIndexDirty = true;
    }
    DLOG_INFO("STORAGE",
              "Pending enrich reconcile reason=%s ids=%u pending=%lu changedSegs=%lu",
              (reason && reason[0]) ? reason : "-",
              static_cast<unsigned>(enrichedIds.size()),
              static_cast<unsigned long>(pendingTotal),
              static_cast<unsigned long>(changedSegments));
    return true;
}

bool StorageManager::_findEventSession(uint32_t eventId, String& outSessionId) const {
    outSessionId = "";
    if (eventId == 0) return false;

    for (const auto& seg : _spoolIndex.segments) {
        if (seg.firstEventId != 0 && eventId < seg.firstEventId) {
            continue;
        }
        if (seg.lastEventId != 0 && eventId > seg.lastEventId) {
            continue;
        }

        bool found = false;
        const bool ok = _scanSegmentRecords(seg.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                if (rec.recordType != SPOOL_REC_EVENT ||
                    rec.eventId != eventId ||
                    !rec.sessionId.length()) {
                    return true;
                }

                outSessionId = rec.sessionId;
                found = true;
                return false;
            });

        if (!ok) {
            return false;
        }
        if (found) {
            return true;
        }
    }

    return false;
}

uint32_t StorageManager::_pendingEventCountForSessionFromSpool(const String& sessionId) const {
    if (!sessionId.length()) return 0;

    uint32_t pending = 0;
    const uint32_t watermark = _uploadedWatermarkForSession(sessionId);

    for (const auto& seg : _spoolIndex.segments) {
        // A legacy-session cleanup can call this across the entire spool.  Give
        // the scheduler a chance between LittleFS files even when summaries
        // let the individual record scans return quickly.
        delay(1);
        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;
        if (summaryReady && seg.eventCount == 0) {
            continue;
        }

        if (seg.format == SPOOL_SEGMENT_BIN_V2) {
            const bool ok = _scanBinarySegmentMetaRecords(
                _spoolBinarySegmentPath(seg.segmentId),
                [&](const BinaryMetaRecord& rec) -> bool {
                    if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
                        return true;
                    }

                    if (!rec.eventId || !rec.sessionId.length()) {
                        return true;
                    }

                    if (rec.sessionId != sessionId) {
                        return true;
                    }

                    if (rec.eventId > watermark) {
                        pending++;
                    }
                    return true;
                });

            if (!ok) {
                DLOG_WARN("STORAGE", "Pending count binary scan failed session=%s seg=%lu",
                          sessionId.c_str(),
                          static_cast<unsigned long>(seg.segmentId));
            }
            continue;
        }

        const bool ok = _scanSegmentRecords(seg.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
                    return true;
                }

                if (!rec.eventId || !rec.sessionId.length()) {
                    return true;
                }

                if (rec.sessionId != sessionId) {
                    return true;
                }

                if (rec.eventId > watermark) {
                    pending++;
                }
                return true;
            });

        if (!ok) {
            DLOG_WARN("STORAGE", "Pending count scan failed session=%s seg=%lu",
                      sessionId.c_str(),
                      static_cast<unsigned long>(seg.segmentId));
        }
    }

    return pending;
}

uint32_t StorageManager::_rescanPendingEventCountFromSpool() {
    const uint32_t _rescanStartMs = millis();
    _rebuildSessionListFromSpool();

    uint32_t pending = 0;
    uint32_t scannedRecords = 0;

    for (const auto& seg : _spoolIndex.segments) {
        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;
        if (summaryReady && seg.eventCount == 0) {
            continue;
        }

        if (seg.format == SPOOL_SEGMENT_BIN_V2) {
            const bool ok = _scanBinarySegmentMetaRecords(
                _spoolBinarySegmentPath(seg.segmentId),
                [&](const BinaryMetaRecord& rec) -> bool {
                    scannedRecords++;
                    if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
                        return true;
                    }

                    if (!rec.eventId || !rec.sessionId.length()) {
                        return true;
                    }

                    const uint32_t watermark = _uploadedWatermarkForSession(rec.sessionId);
                    if (rec.eventId > watermark) {
                        pending++;
                    }

                    return true;
                });

            if (!ok) {
                DLOG_WARN("STORAGE", "Pending rescan binary failed seg=%lu",
                          static_cast<unsigned long>(seg.segmentId));
            }
            continue;
        }

        const bool ok = _scanSegmentRecords(seg.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                scannedRecords++;
                if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
                    return true;
                }

                if (!rec.eventId || !rec.sessionId.length()) {
                    return true;
                }

                const uint32_t watermark = _uploadedWatermarkForSession(rec.sessionId);
                if (rec.eventId > watermark) {
                    pending++;
                }

                return true;
            });

        if (!ok) {
            DLOG_WARN("STORAGE", "Pending rescan failed seg=%lu",
                      static_cast<unsigned long>(seg.segmentId));
        }
    }

    _spoolIndex.pendingTotal = pending;
    _bumpStorageMetaGeneration();
    if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY) {
        _spoolIndexDirty = true;
        if (_spoolIndexPendingWrites < 0xFFFFu) {
            _spoolIndexPendingWrites++;
        }
        _workerMetadataDirtyPending = true;
        DLOG_INFO("STORAGE",
                  "Pending rescan persist deferred owner=%s reason=capture",
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
        return pending;
    }

    _persistSpoolIndex(false, "pending_rescan");

    const uint32_t _rescanElapsedMs = millis() - _rescanStartMs;
    if (_rescanElapsedMs > 200U || scannedRecords > 512U) {
        DLOG_WARN("STORAGE",
                  "Spool rescan slow segs=%u recs=%lu ms=%lu",
                  static_cast<unsigned>(_spoolIndex.segments.size()),
                  static_cast<unsigned long>(scannedRecords),
                  static_cast<unsigned long>(_rescanElapsedMs));
    }
    return pending;
}

void StorageManager::_rebuildSessionListFromSpool() {
    std::vector<String> rebuilt;

    auto remember = [&](const String& sessionId) {
        if (!sessionId.length()) {
            return;
        }
        for (const auto& existing : rebuilt) {
            if (existing == sessionId) {
                return;
            }
        }
        rebuilt.push_back(sessionId);
    };

    for (const auto& seg : _spoolIndex.segments) {
        const bool ok = _scanSegmentRecords(seg.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                remember(rec.sessionId);
                return true;
            });

        if (!ok) {
            DLOG_WARN("STORAGE", "Session rebuild scan failed seg=%lu",
                      static_cast<unsigned long>(seg.segmentId));
        }
    }

    _spoolIndex.sessions = std::move(rebuilt);
}

bool StorageManager::_segmentContainsRecords(uint32_t segmentId) const {
    const SpoolSegmentInfo* seg = _findSegmentInfo(segmentId);
    if (!seg) return false;

    if (seg->summaryValid &&
        seg->summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION) {
        return (seg->eventCount + seg->enrichDeltaCount) > 0;
    }

    if (seg->format == SPOOL_SEGMENT_BIN_V2) {
        const String path = _spoolSegmentPathForFormat(segmentId, seg->format);
        if (!LittleFS.exists(path)) return false;

        File f = LittleFS.open(path, "r");
        if (!f) return false;

        SpoolBin::SegmentHeaderV2 hdr;
        const bool ok = SpoolBin::readBytes(f, &hdr, sizeof(hdr));
        f.close();

        if (!ok) return false;
        if (hdr.magic != SpoolBin::SEGMENT_MAGIC || hdr.version != 2) return false;
        return hdr.recordCount > 0 || hdr.bodyBytes > 0;
    }

    const String path = _spoolSegmentPathForFormat(segmentId, seg->format);
    if (!LittleFS.exists(path)) return false;

    File f = LittleFS.open(path, "r");
    if (!f) return false;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length()) continue;
        f.close();
        return true;
    }

    f.close();
    return false;
}

bool StorageManager::_segmentFullyUploaded(uint32_t segmentId) const {
    const SpoolSegmentInfo* seg = _findSegmentInfo(segmentId);
    if (!seg) return false;

    if (seg->trustState != SPOOL_SEGMENT_TRUSTED &&
        (seg->pendingUploadMissionCount + seg->pendingUploadNoiseCount) == 0) {
        return true;
    }

    if (seg->summaryValid &&
        seg->summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION &&
        seg->eventCount == 0) {
        return true;
    }

    bool foundPending = false;

    if (seg->format == SPOOL_SEGMENT_BIN_V2) {
        SpoolAuditResult audit;
        const SpoolScanStatus status = _scanBinarySegmentMetaRecordsAudit(
            _spoolBinarySegmentPath(segmentId),
            [&](const BinaryMetaRecord& rec) -> bool {
                if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
                    return true;
                }

                if (!rec.eventId || !rec.sessionId.length()) {
                    return true;
                }

                const uint32_t watermark = _uploadedWatermarkForSession(rec.sessionId);
                if (rec.eventId > watermark) {
                    foundPending = true;
                    return false;
                }

                return true;
            },
            audit);

        return status != SpoolScanStatus::FATAL && !foundPending;
    }

    const bool ok = _scanSegmentRecords(segmentId,
        [&](const DecodedSpoolRecord& rec) -> bool {
            if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
                return true;
            }

            if (!rec.eventId || !rec.sessionId.length()) {
                return true;
            }

            const uint32_t watermark = _uploadedWatermarkForSession(rec.sessionId);
            if (rec.eventId > watermark) {
                foundPending = true;
                return false;
            }

            return true;
        });

    return ok && !foundPending;
}

int StorageManager::_cleanupLegacyUploadSidecars() {
    if (!LittleFS.exists(PATH_EVENTS)) return 0;

    File dir = LittleFS.open(PATH_EVENTS);
    if (!dir || !dir.isDirectory()) return 0;

    int removed = 0;
    File f = dir.openNextFile();
    while (f) {
        delay(1);
        if (!f.isDirectory()) {
            String name = String(f.name());
            if (name.startsWith(String(PATH_EVENTS) + "/")) {
                name.remove(0, String(PATH_EVENTS).length() + 1);
            }

            if (name.endsWith(".upload.jsonl")) {
                String fullPath = String(PATH_EVENTS) + "/" + name;
                f.close();
                if (LittleFS.remove(fullPath)) {
                    removed++;
                }
                f = dir.openNextFile();
                continue;
            }
        }
        f = dir.openNextFile();
    }

    if (removed > 0) {
        DLOG_INFO("STORAGE", "Removed %d legacy upload sidecar(s)", removed);
    }

    return removed;
}

int StorageManager::_cleanupLegacyEnrichSidecars() {
    if (!LittleFS.exists(PATH_EVENTS)) return 0;

    File dir = LittleFS.open(PATH_EVENTS);
    if (!dir || !dir.isDirectory()) return 0;

    int removed = 0;
    File f = dir.openNextFile();
    while (f) {
        delay(1);
        if (!f.isDirectory()) {
            String name = String(f.name());
            if (name.startsWith(String(PATH_EVENTS) + "/")) {
                name.remove(0, String(PATH_EVENTS).length() + 1);
            }

            if (name.endsWith(".enrich.jsonl")) {
                String fullPath = String(PATH_EVENTS) + "/" + name;
                f.close();
                if (LittleFS.remove(fullPath)) {
                    removed++;
                }
                f = dir.openNextFile();
                continue;
            }
        }
        f = dir.openNextFile();
    }

    if (removed > 0) {
        DLOG_INFO("STORAGE", "Removed %d legacy enrich sidecar(s)", removed);
    }

    return removed;
}

int StorageManager::_cleanupLegacyRawSessionFiles() {
    if (!LittleFS.exists(PATH_EVENTS)) return 0;

    File dir = LittleFS.open(PATH_EVENTS);
    if (!dir || !dir.isDirectory()) return 0;

    int removed = 0;
    File f = dir.openNextFile();
    while (f) {
        // Directory iteration plus a per-session spool recount can otherwise
        // monopolize the storage task long enough to trip the task watchdog.
        delay(1);
        if (!f.isDirectory()) {
            String name = String(f.name());
            if (name.startsWith(String(PATH_EVENTS) + "/")) {
                name.remove(0, String(PATH_EVENTS).length() + 1);
            }

            // Only target legacy raw session files:
            //   <session>.jsonl
            // and skip sidecars:
            //   <session>.upload.jsonl
            //   <session>.enrich.jsonl
            if (name.endsWith(".jsonl") &&
                !name.endsWith(".upload.jsonl") &&
                !name.endsWith(".enrich.jsonl")) {

                String sessionId = name;
                sessionId.remove(sessionId.length() - 6);  // strip ".jsonl"

                if (sessionId.length()) {
                    const uint32_t pending =
                        _pendingEventCountForSessionFromSpool(sessionId);
                    if (pending == 0) {
                        String fullPath = String(PATH_EVENTS) + "/" + name;
                        f.close();
                        if (LittleFS.remove(fullPath)) {
                            removed++;
                        }
                        f = dir.openNextFile();
                        continue;
                    }
                }
            }
        }
        f = dir.openNextFile();
    }

    if (removed > 0) {
        DLOG_INFO("STORAGE", "Removed %d legacy raw session file(s)", removed);
    }

    return removed;
}

bool StorageManager::_removeSpoolSegmentFile(uint32_t segmentId) {
    if (_workerAppendFile && _workerAppendSegmentId == segmentId) {
        _closeWorkerAppendFile("remove_segment");
    }

    const SpoolSegmentInfo* seg = _findSegmentInfo(segmentId);
    const String path = _spoolSegmentPathForFormat(
    segmentId,
    seg ? seg->format : static_cast<uint8_t>(SPOOL_SEGMENT_JSONL));
    const String indexPath = _uploadIndexPath(segmentId);

    auto removeIndex = [&]() {
        if (LittleFS.exists(indexPath) && !LittleFS.remove(indexPath)) {
            DLOG_WARN("STORAGE",
                      "Upload index sidecar remove failed seg=%lu path=%s",
                      static_cast<unsigned long>(segmentId),
                      indexPath.c_str());
        }
    };

    if (!LittleFS.exists(path)) {
        removeIndex();
        return true;
    }

    // LittleFS can briefly report "Has open FD" right after recent scans/reads.
    // Retry a few times with a short yield before giving up.
    for (int attempt = 1; attempt <= 4; ++attempt) {
        if (LittleFS.remove(path)) {
            removeIndex();
            if (attempt > 1) {
                DLOG_INFO("STORAGE",
                          "Spool segment remove recovered seg=%lu attempt=%d",
                          static_cast<unsigned long>(segmentId),
                          attempt);
            }
            return true;
        }

        DLOG_WARN("STORAGE",
                  "Spool segment remove retry seg=%lu attempt=%d path=%s",
                  static_cast<unsigned long>(segmentId),
                  attempt,
                  path.c_str());

        delay(2);
    }

    DLOG_WARN("STORAGE", "Spool compact remove_failed seg=%lu path=%s",
              static_cast<unsigned long>(segmentId),
              path.c_str());
    return false;
}

SegmentPendingScanResult
StorageManager::_segmentPendingScanResult(uint32_t segmentId) const {
    const SpoolSegmentInfo* seg = _findSegmentInfo(segmentId);
    if (!seg) return SegmentPendingScanResult::SCAN_FAILED;

    if (seg->lifecycle == SPOOL_SEGMENT_DELETE_PENDING ||
        seg->lifecycle == SPOOL_SEGMENT_DRAINED ||
        seg->lifecycle == SPOOL_SEGMENT_FULLY_UPLOADED) {
        return SegmentPendingScanResult::NO_PENDING;
    }

    if (seg->trustState != SPOOL_SEGMENT_TRUSTED &&
        (seg->pendingUploadMissionCount + seg->pendingUploadNoiseCount) == 0) {
        DLOG_WARN("STORAGE",
                  "Spool compact retiring salvaged segment seg=%lu trust=%s",
                  static_cast<unsigned long>(segmentId),
                  _spoolTrustText(seg->trustState));
        return SegmentPendingScanResult::NO_PENDING;
    }

    if (seg->summaryValid &&
        seg->summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION &&
        seg->eventCount == 0) {
        return SegmentPendingScanResult::NO_PENDING;
    }

    bool foundPending = false;

    if (seg->format == SPOOL_SEGMENT_BIN_V2) {
        SpoolAuditResult audit;
        const SpoolScanStatus status = _scanBinarySegmentMetaRecordsAudit(
            _spoolBinarySegmentPath(segmentId),
            [&](const BinaryMetaRecord& rec) -> bool {
                if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
                    return true;
                }

                if (!rec.eventId || !rec.sessionId.length()) {
                    return true;
                }

                const uint32_t watermark = _uploadedWatermarkForSession(rec.sessionId);
                if (rec.eventId > watermark) {
                    foundPending = true;
                    return false;
                }

                return true;
            },
            audit);

        if (status == SpoolScanStatus::FATAL) {
            DLOG_WARN("STORAGE", "Spool compact scan_failed seg=%lu format=bin",
                      static_cast<unsigned long>(segmentId));
            return SegmentPendingScanResult::SCAN_FAILED;
        }
        if (audit.skippedRecords > 0) {
            DLOG_WARN("STORAGE",
                      "Spool compact discard-mark seg=%lu skipped=%lu status=%u",
                      static_cast<unsigned long>(segmentId),
                      static_cast<unsigned long>(audit.skippedRecords),
                      static_cast<unsigned>(status));
        }
        return foundPending ? SegmentPendingScanResult::HAS_PENDING
                            : SegmentPendingScanResult::NO_PENDING;
    }

    const bool ok = _scanSegmentRecords(segmentId,
        [&](const DecodedSpoolRecord& rec) -> bool {
            if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
                return true;
            }

            if (!rec.eventId || !rec.sessionId.length()) {
                return true;
            }

            const uint32_t watermark = _uploadedWatermarkForSession(rec.sessionId);
            if (rec.eventId > watermark) {
                foundPending = true;
                return false;
            }

            return true;
        });

    if (!ok) {
        DLOG_WARN("STORAGE", "Spool compact scan_failed seg=%lu format=jsonl",
                  static_cast<unsigned long>(segmentId));
        return SegmentPendingScanResult::SCAN_FAILED;
    }
    return foundPending ? SegmentPendingScanResult::HAS_PENDING
                        : SegmentPendingScanResult::NO_PENDING;
}

bool StorageManager::_pruneUploadedSessionState() {
    const uint32_t startMs = millis();
    const String activeSession = SESS.getId();
    uint16_t scannedSegments = 0;

    std::vector<String> sessionsWithRetainedRecords;
    auto rememberRetainedSession = [&](const String& sid) {
        if (!sid.length()) {
            return;
        }
        for (const auto& existing : sessionsWithRetainedRecords) {
            if (existing == sid) {
                return;
            }
        }
        sessionsWithRetainedRecords.push_back(sid);
    };

    for (const auto& seg : _spoolIndex.segments) {
        // _scanSegmentRecords() yields while decoding records; this boundary
        // yield also covers file open/close and summary-only segments.
        delay(1);
        scannedSegments++;
        const bool ok = _scanSegmentRecords(seg.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                if (rec.recordType == SPOOL_REC_EVENT) {
                    rememberRetainedSession(rec.sessionId);
                }
                return true;
            });
        if (!ok) {
            DLOG_WARN("STORAGE", "Spool compact prune skipped session scan_failed seg=%lu",
                      static_cast<unsigned long>(seg.segmentId));
            return false;
        }
    }

    auto sessionHasRetainedRecords = [&](const String& sid) -> bool {
        for (const auto& retained : sessionsWithRetainedRecords) {
            if (retained == sid) {
                return true;
            }
        }
        return false;
    };

    auto sessionShouldKeep = [&](const String& sid) -> bool {
        if (sid == activeSession) return true;
        // The retained-record scan above already visits every segment that
        // compact kept. A pending event necessarily appears in that set, so
        // avoid per-session spool recounts here; those repeated full scans
        // turned compact metadata pruning into multi-second maintenance stalls.
        return sessionHasRetainedRecords(sid);
    };

    const size_t sessionsBefore = _spoolIndex.sessions.size();
    _spoolIndex.sessions.erase(
        std::remove_if(_spoolIndex.sessions.begin(),
                       _spoolIndex.sessions.end(),
                       [&](const String& sid) {
                           return !sessionShouldKeep(sid);
                       }),
        _spoolIndex.sessions.end());

    const size_t watermarksBefore = _spoolIndex.uploadedWatermarks.size();
    _spoolIndex.uploadedWatermarks.erase(
        std::remove_if(_spoolIndex.uploadedWatermarks.begin(),
                       _spoolIndex.uploadedWatermarks.end(),
                       [&](const std::pair<String, uint32_t>& entry) {
                           return !sessionShouldKeep(entry.first);
                       }),
        _spoolIndex.uploadedWatermarks.end());

    const size_t sessionsRemoved =
        sessionsBefore - _spoolIndex.sessions.size();
    const size_t watermarksRemoved =
        watermarksBefore - _spoolIndex.uploadedWatermarks.size();

    if (sessionsRemoved || watermarksRemoved) {
        DLOG_INFO("STORAGE",
                  "Spool compact pruned sessions=%u watermarks=%u scannedSegs=%u retainedSessions=%u ms=%lu",
                  static_cast<unsigned>(sessionsRemoved),
                  static_cast<unsigned>(watermarksRemoved),
                  static_cast<unsigned>(scannedSegments),
                  static_cast<unsigned>(sessionsWithRetainedRecords.size()),
                  static_cast<unsigned long>(millis() - startMs));
        return true;
    }

    const uint32_t elapsedMs = millis() - startMs;
    if (elapsedMs > 1000UL) {
        DLOG_WARN("STORAGE",
                  "Spool compact prune slow no_change scannedSegs=%u retainedSessions=%u ms=%lu",
                  static_cast<unsigned>(scannedSegments),
                  static_cast<unsigned>(sessionsWithRetainedRecords.size()),
                  static_cast<unsigned long>(elapsedMs));
    }

    return false;
}

bool StorageManager::_segmentScanReadable(uint32_t segmentId) const {
    const SpoolSegmentInfo* seg = _findSegmentInfo(segmentId);
    if (!seg) return false;

    if (seg->summaryValid &&
        seg->summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION &&
        seg->eventCount == 0) {
        return true;
    }

    if (seg->format == SPOOL_SEGMENT_BIN_V2) {
        SpoolAuditResult audit;
        const SpoolScanStatus status = _scanBinarySegmentMetaRecordsAudit(
            _spoolBinarySegmentPath(segmentId),
            [](const BinaryMetaRecord&) -> bool {
                return true;
            },
            audit);
        return status != SpoolScanStatus::FATAL;
    }

    return _scanSegmentRecords(segmentId,
        [](const DecodedSpoolRecord&) -> bool {
            return true;
        });
}

bool StorageManager::_isSegmentQuarantined(uint32_t segmentId) const {
    return LittleFS.exists(_spoolQuarantineMetaPath(segmentId));
}

bool StorageManager::_writeQuarantineMeta(uint32_t segmentId,
                                         SpoolCorruptionReason reason,
                                         const char* detail,
                                         const String& originalPath,
                                         const String& quarantinePath,
                                         const String& metaPath) {
    if (!LittleFS.exists(PATH_SPOOL_BAD_META)) {
        _ensureDir(PATH_SPOOL_BAD_META);
    }

    File f = LittleFS.open(metaPath, "w");
    if (!f) {
        return false;
    }

    JsonDocument doc;
    doc["segment_id"] = segmentId;
    doc["reason"] = spoolCorruptionReasonText(reason);
    doc["detail"] = (detail && detail[0]) ? detail : "";
    doc["original_path"] = originalPath;
    doc["quarantine_path"] = quarantinePath;
    doc["timestamp_ms"] = millis();
    doc["active_segment"] = (segmentId == _spoolIndex.activeSegmentId);
    doc["build"] = _config.version;
    doc["pending_before"] = _spoolIndex.pendingTotal;
    doc["next_event_before"] = _nextEventId;

    serializeJson(doc, f);
    f.close();
    return true;
}

bool StorageManager::_quarantineSpoolSegment(uint32_t segmentId,
                                             SpoolCorruptionReason reason,
                                             const char* detail) {
    const SpoolSegmentInfo* seg = _findSegmentInfo(segmentId);
    if (!seg) {
        return false;
    }

    const String originalPath = _spoolSegmentPathForFormat(segmentId, seg->format);
    const String quarantinePath = _spoolQuarantineLogPath(segmentId, seg->format);
    const bool originalExists = LittleFS.exists(originalPath);
    const bool quarantineExists = LittleFS.exists(quarantinePath);

    if (originalExists) {
        if (quarantineExists) {
            if (!LittleFS.remove(originalPath)) {
                DLOG_WARN("STORAGE",
                          "Failed to drop duplicate quarantined segment seg=%lu path=%s",
                          static_cast<unsigned long>(segmentId),
                          originalPath.c_str());
                return false;
            }
        } else if (!LittleFS.rename(originalPath, quarantinePath)) {
            DLOG_WARN("STORAGE",
                      "Quarantine rename failed seg=%lu from=%s to=%s",
                      static_cast<unsigned long>(segmentId),
                      originalPath.c_str(),
                      quarantinePath.c_str());
            return false;
        }
    } else if (!quarantineExists) {
        DLOG_WARN("STORAGE", "Quarantine skipped missing segment seg=%lu path=%s",
                  static_cast<unsigned long>(segmentId),
                  originalPath.c_str());
        return false;
    }

    if (!_writeQuarantineMeta(segmentId,
                             reason,
                             detail,
                             originalPath,
                             quarantinePath,
                             _spoolQuarantineMetaPath(segmentId))) {
        DLOG_WARN("STORAGE",
                  "Quarantine meta write failed seg=%lu meta=%s",
                  static_cast<unsigned long>(segmentId),
                  _spoolQuarantineMetaPath(segmentId).c_str());
        return false;
    }

    DLOG_WARN("STORAGE",
              "Quarantined spool segment seg=%lu reason=%s detail=%s path=%s",
              static_cast<unsigned long>(segmentId),
              spoolCorruptionReasonText(reason),
              (detail && detail[0]) ? detail : "-",
              quarantinePath.c_str());
    if (SpoolSegmentInfo* live = _findSegmentInfo(segmentId)) {
        live->lifecycle = SPOOL_SEGMENT_QUARANTINED;
        live->trustState = SPOOL_SEGMENT_INVALID;
        _spoolIndexDirty = true;
    }
    _setCounterTrustState(CounterTrust::EmergencyOnly, "segment_quarantined");
    return true;
}

void StorageManager::_logSpoolAuditResult(const char* reason,
                                          const SpoolAuditResult& audit) {
    const char* safeReason = (reason && reason[0] != '\0') ? reason : "-";
    const uint32_t uploadedRetained =
        (audit.validEventRecords >= audit.rebuiltPendingTotal)
            ? (audit.validEventRecords - audit.rebuiltPendingTotal)
            : 0U;
    if (audit.hadFatalSegmentError) {
        DLOG_WARN("STORAGE",
                  "Spool audit[%s] scannedSegs=%lu scannedRecords=%lu validEvents=%lu uploadedRetained=%lu validEnrichDeltas=%lu invalidRecords=%lu skippedRecords=%lu quarantined=%lu unreadable=%lu pendingUpload=%lu->%lu nextEventId=%lu maxEventIdSeen=%lu repaired=%d fatal=%d",
                  safeReason,
                  static_cast<unsigned long>(audit.scannedSegments),
                  static_cast<unsigned long>(audit.scannedRecords),
                  static_cast<unsigned long>(audit.validEventRecords),
                  static_cast<unsigned long>(uploadedRetained),
                  static_cast<unsigned long>(audit.validEnrichDeltas),
                  static_cast<unsigned long>(audit.invalidRecords),
                  static_cast<unsigned long>(audit.skippedRecords),
                  static_cast<unsigned long>(audit.quarantinedSegments),
                  static_cast<unsigned long>(audit.unreadableSegments),
                  static_cast<unsigned long>(audit.oldPendingTotal),
                  static_cast<unsigned long>(audit.rebuiltPendingTotal),
                  static_cast<unsigned long>(audit.oldNextEventId),
                  static_cast<unsigned long>(audit.maxEventIdSeen),
                  audit.repaired ? 1 : 0,
                  audit.hadFatalSegmentError ? 1 : 0);
    } else {
        DLOG_INFO("STORAGE",
                  "Spool audit[%s] scannedSegs=%lu scannedRecords=%lu validEvents=%lu uploadedRetained=%lu validEnrichDeltas=%lu invalidRecords=%lu skippedRecords=%lu quarantined=%lu unreadable=%lu pendingUpload=%lu->%lu nextEventId=%lu maxEventIdSeen=%lu repaired=%d fatal=%d",
                  safeReason,
                  static_cast<unsigned long>(audit.scannedSegments),
                  static_cast<unsigned long>(audit.scannedRecords),
                  static_cast<unsigned long>(audit.validEventRecords),
                  static_cast<unsigned long>(uploadedRetained),
                  static_cast<unsigned long>(audit.validEnrichDeltas),
                  static_cast<unsigned long>(audit.invalidRecords),
                  static_cast<unsigned long>(audit.skippedRecords),
                  static_cast<unsigned long>(audit.quarantinedSegments),
                  static_cast<unsigned long>(audit.unreadableSegments),
                  static_cast<unsigned long>(audit.oldPendingTotal),
                  static_cast<unsigned long>(audit.rebuiltPendingTotal),
                  static_cast<unsigned long>(audit.oldNextEventId),
                  static_cast<unsigned long>(audit.maxEventIdSeen),
                  audit.repaired ? 1 : 0,
                  audit.hadFatalSegmentError ? 1 : 0);
    }
}

const char* StorageManager::_segmentLifecycleText(uint8_t lifecycle) const {
    switch (static_cast<SpoolSegmentLifecycle>(lifecycle)) {
        case SPOOL_SEGMENT_ACTIVE:             return "active";
        case SPOOL_SEGMENT_SEALED:             return "sealed";
        case SPOOL_SEGMENT_INDEXED:            return "indexed";
        case SPOOL_SEGMENT_PARTIALLY_UPLOADED: return "partially_uploaded";
        case SPOOL_SEGMENT_FULLY_UPLOADED:     return "fully_uploaded";
        case SPOOL_SEGMENT_SALVAGED:           return "salvaged";
        case SPOOL_SEGMENT_DRAINED:            return "drained";
        case SPOOL_SEGMENT_DELETE_PENDING:     return "delete_pending";
        case SPOOL_SEGMENT_QUARANTINED:        return "quarantined";
        default:                               return "unknown";
    }
}

SpoolSegmentLifecycle StorageManager::_deriveSegmentLifecycle(
    const SpoolSegmentInfo& seg) const {
    if (_isSegmentQuarantined(seg.segmentId)) {
        return SPOOL_SEGMENT_QUARANTINED;
    }

    const bool isActive = seg.segmentId != 0 &&
                          seg.segmentId == _spoolIndex.activeSegmentId;
    if (isActive) {
        return SPOOL_SEGMENT_ACTIVE;
    }

    const uint32_t pendingUpload =
        seg.pendingUploadMissionCount + seg.pendingUploadNoiseCount;
    const bool summaryReady =
        seg.summaryValid &&
        seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;

    if (seg.lifecycle == SPOOL_SEGMENT_DELETE_PENDING) {
        return SPOOL_SEGMENT_DELETE_PENDING;
    }

    if (seg.trustState != SPOOL_SEGMENT_TRUSTED) {
        return pendingUpload == 0 ? SPOOL_SEGMENT_DRAINED
                                  : SPOOL_SEGMENT_SALVAGED;
    }

    if (summaryReady && seg.eventCount == 0 && seg.enrichDeltaCount == 0) {
        return SPOOL_SEGMENT_DRAINED;
    }

    if (pendingUpload == 0 && summaryReady) {
        return SPOOL_SEGMENT_FULLY_UPLOADED;
    }

    if (summaryReady &&
        pendingUpload < (seg.missionCount + seg.noiseCount)) {
        return SPOOL_SEGMENT_PARTIALLY_UPLOADED;
    }

    if (summaryReady) {
        return SPOOL_SEGMENT_INDEXED;
    }

    return SPOOL_SEGMENT_SEALED;
}

void StorageManager::_refreshSegmentLifecycle(SpoolSegmentInfo& seg) {
    const SpoolSegmentLifecycle next = _deriveSegmentLifecycle(seg);
    if (seg.lifecycle != static_cast<uint8_t>(next)) {
        DLOG_INFO("STORAGE",
                  "Segment lifecycle seg=%lu %s->%s trust=%s pending=%lu",
                  static_cast<unsigned long>(seg.segmentId),
                  _segmentLifecycleText(seg.lifecycle),
                  _segmentLifecycleText(static_cast<uint8_t>(next)),
                  _spoolTrustText(seg.trustState),
                  static_cast<unsigned long>(seg.pendingUploadMissionCount +
                                             seg.pendingUploadNoiseCount));
        seg.lifecycle = static_cast<uint8_t>(next);
        _spoolIndexDirty = true;
    }
}

bool StorageManager::compactSpool() {
    if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY) {
        DLOG_INFO("STORAGE",
                  "Spool compact deferred owner=%s reason=capture",
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
        return true;
    }

    // If the active segment has records and all of them are uploaded,
    // rotate to a fresh writable segment before reclaiming old segments.
    const uint32_t previouslyActiveId = _spoolIndex.activeSegmentId;
    uint32_t graceSegmentId = 0;

    if (previouslyActiveId != 0 &&
        _segmentContainsRecords(previouslyActiveId) &&
        _segmentFullyUploaded(previouslyActiveId)) {
        if (!_openNewSpoolSegment()) {
            DLOG_WARN("STORAGE", "Spool compact rotate_failed active=%lu",
                      static_cast<unsigned long>(previouslyActiveId));
            return false;
        }

        // The segment we just rotated off is the one most likely to still have
        // an open FD somewhere in LittleFS. Keep it for one more compact pass.
        graceSegmentId = previouslyActiveId;

        DLOG_INFO("STORAGE", "Spool compact rotated off fully-uploaded active=%lu",
                  static_cast<unsigned long>(previouslyActiveId));
    }

    // A segment rotated off the active slot by the upload-completion path (or
    // a prior compact) may still have a lingering reader FD. Grace it for one
    // compact cycle before unlink, then consume the marker so the next pass
    // reclaims it. graceSegmentId (compact's own rotate above) is folded in
    // below so both sources get identical treatment.
    const uint32_t externallyRotatedSegmentId = _lastRotatedOffSegmentId;
    _lastRotatedOffSegmentId = 0;

    std::vector<SpoolSegmentInfo> kept;
    bool changed = false;
    bool scanFailed = false;
    bool removeFailed = false;
    unsigned removedCount = 0;

    for (auto& seg : _spoolIndex.segments) {
        _refreshSegmentLifecycle(seg);
        // Lifecycle refresh performs LittleFS existence checks.  On a large
        // spool, the file-level work alone can exceed the task watchdog even
        // though no single operation is slow.
        delay(1);
    }

    for (const auto& seg : _spoolIndex.segments) {
        // Keep compaction cooperative even for active, grace, and summary-only
        // segments that bypass the record scanner's internal yield cadence.
        delay(1);
        const bool isActive = (seg.segmentId == _spoolIndex.activeSegmentId);

        if (isActive) {
            kept.push_back(seg);
            continue;
        }

        // Give the just-rotated segment one full grace cycle before trying
        // to unlink it. This avoids LittleFS "Has open FD" churn. Covers both
        // compact's own rotate (graceSegmentId) and an upload-path rotate
        // (externallyRotatedSegmentId).
        if ((graceSegmentId != 0 && seg.segmentId == graceSegmentId) ||
            (externallyRotatedSegmentId != 0 &&
             seg.segmentId == externallyRotatedSegmentId)) {
            kept.push_back(seg);
            continue;
        }

        const SegmentPendingScanResult pendingScan =
            _segmentPendingScanResult(seg.segmentId);
        switch (pendingScan) {
        case SegmentPendingScanResult::HAS_PENDING:
            kept.push_back(seg);
            break;

        case SegmentPendingScanResult::NO_PENDING: {
            if (SpoolSegmentInfo* live = _findSegmentInfo(seg.segmentId)) {
                live->lifecycle = SPOOL_SEGMENT_DELETE_PENDING;
                _spoolIndexDirty = true;
                DLOG_INFO("STORAGE",
                          "Segment lifecycle seg=%lu %s->delete_pending",
                          static_cast<unsigned long>(seg.segmentId),
                          _segmentLifecycleText(seg.lifecycle));
            }
            const bool removed = _removeSpoolSegmentFile(seg.segmentId);
            if (removed) {
                changed = true;
                removedCount++;
            } else {
                removeFailed = true;
                _spoolAuditRepairRequired = true;
                _setCounterTrustState(STORAGE_COUNTER_REPAIR_REQUIRED,
                                      "compact_remove_failed");
                requestMaintenance(static_cast<StorageMaintenanceReason>(
                                       STORAGE_MAINT_COUNTER_UNTRUSTED |
                                       STORAGE_MAINT_SEGMENT_AUDIT),
                                   "compact_remove_failed");
                SpoolSegmentInfo keepSeg = seg;
                keepSeg.lifecycle = SPOOL_SEGMENT_DELETE_PENDING;
                kept.push_back(keepSeg);
            }
            break;
        }

        case SegmentPendingScanResult::SCAN_FAILED:
            scanFailed = true;
            _spoolAuditRepairRequired = true;
            kept.push_back(seg);
            break;
        }
    }

    if (changed) {
        _spoolIndex.segments.assign(kept.begin(), kept.end());
        if (!_spoolIndex.segments.empty()) {
            _spoolIndex.oldestSegmentId = _spoolIndex.segments.front().segmentId;
        } else {
            _spoolIndex.oldestSegmentId = _spoolIndex.activeSegmentId;
        }

        if (!scanFailed && !removeFailed) {
            changed |= _pruneUploadedSessionState();
            _spoolAuditRepairRequired = false;
        } else {
            DLOG_WARN("STORAGE",
                      "Spool compact skipped prune scanFailed=%d removeFailed=%d",
                      scanFailed ? 1 : 0,
                      removeFailed ? 1 : 0);
        }

        const bool ok = _persistSpoolIndex(true, "compact");
        if (ok) {
            DLOG_INFO("STORAGE", "Spool compact removed=%u remaining=%u",
                      removedCount,
                      static_cast<unsigned>(_spoolIndex.segments.size()));
            _cleanupLegacyUploadSidecars();
            _cleanupLegacyEnrichSidecars();
            _cleanupLegacyRawSessionFiles();
            _logSpoolDiagnostics("compact");
            _checkSpoolInvariants("compact", false);
        }
        return ok;
    }

    if (scanFailed || removeFailed) {
        DLOG_WARN("STORAGE",
                  "Spool compact no_change prune_skipped scanFailed=%d removeFailed=%d",
                  scanFailed ? 1 : 0,
                  removeFailed ? 1 : 0);
        _spoolAuditRepairRequired = true;
        if (removeFailed) {
            _setCounterTrustState(STORAGE_COUNTER_REPAIR_REQUIRED,
                                  "compact_remove_failed");
            requestMaintenance(static_cast<StorageMaintenanceReason>(
                                   STORAGE_MAINT_COUNTER_UNTRUSTED |
                                   STORAGE_MAINT_SEGMENT_AUDIT),
                               "compact_remove_failed");
        }
    } else {
        // Session/watermark metadata is prunable only after retained segment
        // files have actually been removed.  With an unchanged segment set,
        // _pruneUploadedSessionState() must retain the same sessions and its
        // full decoded-record scan is guaranteed to be a no-op.  Skipping it
        // avoids a multi-second display/capture stall on deep field spools.
        _spoolAuditRepairRequired = false;
    }

    DLOG_INFO("STORAGE", "Spool compact no_change");
    _logSpoolDiagnostics("compact_noop");
    return true;
}

// ── Phase 9: lightweight post-operation invariant check ──────────────────────
//
// Runs fast (no I/O, no segment scan). Catches counter splits, impossible
// segment ranges, and dangling active/oldest pointers that can arise from
// interrupted writes or bugs in the deferred-persist path.
//
// When an invariant fails, the function marks counter trust degraded and queues
// budgeted maintenance. It never walks the spool inline.
// ─────────────────────────────────────────────────────────────────────────────
bool StorageManager::_checkSpoolInvariants(const char* reason, bool repairIfBad) {
    (void)repairIfBad;
    if (!_ready) {
        return false;
    }

    bool ok = true;
    const char* r = (reason && reason[0]) ? reason : "-";

    // active segment must appear in the index (or activeSegmentId be zero)
    if (_spoolIndex.activeSegmentId != 0 &&
        _findSegmentInfo(_spoolIndex.activeSegmentId) == nullptr) {
        DLOG_WARN("STORAGE", "Inv[%s] active seg=%lu not in index",
                  r, static_cast<unsigned long>(_spoolIndex.activeSegmentId));
        ok = false;
    }

    // oldest segment must appear in the index (or oldestSegmentId be zero)
    if (_spoolIndex.oldestSegmentId != 0 &&
        _findSegmentInfo(_spoolIndex.oldestSegmentId) == nullptr) {
        DLOG_WARN("STORAGE", "Inv[%s] oldest seg=%lu not in index",
                  r, static_cast<unsigned long>(_spoolIndex.oldestSegmentId));
        ok = false;
    }

    // in-memory pending counter must agree with the spool index
    if (_spoolIndex.pendingTotal != _pendingEventCount) {
        DLOG_WARN("STORAGE", "Inv[%s] pendingUpload split idx=%lu counter=%lu",
                  r,
                  static_cast<unsigned long>(_spoolIndex.pendingTotal),
                  static_cast<unsigned long>(_pendingEventCount));
        ok = false;
    }

    // next event ID must never be zero
    if (_nextEventId == 0) {
        DLOG_WARN("STORAGE", "Inv[%s] nextEventId=0", r);
        ok = false;
    }

    // no segment may have an impossible [first, last] event ID range
    bool allSummariesCurrent = true;
    uint32_t segmentPendingUpload = 0;
    for (const auto& seg : _spoolIndex.segments) {
        if (seg.firstEventId != 0 && seg.lastEventId != 0 &&
            seg.firstEventId > seg.lastEventId) {
            DLOG_WARN("STORAGE",
                      "Inv[%s] seg=%lu impossible range first=%lu last=%lu",
                      r,
                      static_cast<unsigned long>(seg.segmentId),
                      static_cast<unsigned long>(seg.firstEventId),
                      static_cast<unsigned long>(seg.lastEventId));
            ok = false;
        }

        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;
        if (!summaryReady) {
            allSummariesCurrent = false;
            continue;
        }

        const uint32_t pendingMission = seg.pendingUploadMissionCount;
        const uint32_t pendingNoise = seg.pendingUploadNoiseCount;
        segmentPendingUpload += pendingMission + pendingNoise;

        if (seg.eventCount + seg.enrichDeltaCount > seg.recordCount) {
            DLOG_WARN("STORAGE",
                      "Inv[%s] seg=%lu record split impossible records=%lu events=%lu enrich=%lu",
                      r,
                      static_cast<unsigned long>(seg.segmentId),
                      static_cast<unsigned long>(seg.recordCount),
                      static_cast<unsigned long>(seg.eventCount),
                      static_cast<unsigned long>(seg.enrichDeltaCount));
            if (seg.format == SPOOL_SEGMENT_BIN_V2) {
                _spoolSummaryRebuildPending = true;
                _pendingCountDirty = true;
                requestMaintenance(STORAGE_MAINT_DIRTY_SUMMARY,
                                   "invariant_binary_split_stale");
                requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                                   "invariant_binary_split_stale");
            } else {
                ok = false;
            }
        }

        if (seg.missionCount + seg.noiseCount > seg.eventCount) {
            DLOG_WARN("STORAGE",
                      "Inv[%s] seg=%lu lane split impossible events=%lu mission=%lu noise=%lu",
                      r,
                      static_cast<unsigned long>(seg.segmentId),
                      static_cast<unsigned long>(seg.eventCount),
                      static_cast<unsigned long>(seg.missionCount),
                      static_cast<unsigned long>(seg.noiseCount));
            ok = false;
        }

        if (pendingMission > seg.missionCount || pendingNoise > seg.noiseCount) {
            DLOG_WARN("STORAGE",
                      "Inv[%s] seg=%lu pending split impossible mission=%lu/%lu noise=%lu/%lu",
                      r,
                      static_cast<unsigned long>(seg.segmentId),
                      static_cast<unsigned long>(pendingMission),
                      static_cast<unsigned long>(seg.missionCount),
                      static_cast<unsigned long>(pendingNoise),
                      static_cast<unsigned long>(seg.noiseCount));
            ok = false;
        }

        if (seg.pendingEnrichmentCount > seg.eventCount) {
            DLOG_WARN("STORAGE",
                      "Inv[%s] seg=%lu enrich pending impossible pending=%lu events=%lu",
                      r,
                      static_cast<unsigned long>(seg.segmentId),
                      static_cast<unsigned long>(seg.pendingEnrichmentCount),
                      static_cast<unsigned long>(seg.eventCount));
            ok = false;
        }
    }

    if (allSummariesCurrent && segmentPendingUpload != _pendingEventCount) {
        const bool rotationCounterInFlight =
            strcmp(r, "rotation") == 0 &&
            segmentPendingUpload == (_pendingEventCount + 1U);
        if (rotationCounterInFlight) {
            DLOG_DEBUG("STORAGE",
                       "Inv[%s] pendingUpload aggregate waiting for accepted counter segments=%lu counter=%lu",
                       r,
                       static_cast<unsigned long>(segmentPendingUpload),
                       static_cast<unsigned long>(_pendingEventCount));
        } else {
            DLOG_WARN("STORAGE",
                      "Inv[%s] pendingUpload aggregate mismatch segments=%lu counter=%lu",
                      r,
                      static_cast<unsigned long>(segmentPendingUpload),
                      static_cast<unsigned long>(_pendingEventCount));
            const bool watermarkLag =
                !_spoolIndex.uploadedWatermarks.empty() &&
                segmentPendingUpload > _pendingEventCount;
            if (watermarkLag) {
                _spoolSummaryRebuildPending = true;
                _pendingCountDirty = true;
                _spoolAuditRepairRequired = true;
                requestMaintenance(STORAGE_MAINT_DIRTY_SUMMARY,
                                   "invariant_watermark_lag");
                requestMaintenance(STORAGE_MAINT_SNAPSHOT_LAGGED,
                                   "invariant_watermark_lag");
                requestMaintenance(STORAGE_MAINT_SEGMENT_AUDIT,
                                   "invariant_watermark_lag");
                requestMaintenance(STORAGE_MAINT_COUNTER_UNTRUSTED,
                                   "invariant_watermark_lag");
            } else {
                ok = false;
            }
        }
    }

    if (ok) {
        if (_spoolAuditRepairRequired) {
            // Leave repair states untouched until the explicit repair path
            // finishes; a quick invariant pass cannot make them safe again.
        } else if (_spoolSummaryRebuildPending ||
                   _workerMetadataDirtyPending ||
                   _eventCounterDirty ||
                   _spoolIndexDirty) {
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED, r);
        } else if (_pendingCountDirty) {
            _setCounterTrustState(STORAGE_COUNTER_DEGRADED, r);
        } else if (_counterTrustState != STORAGE_COUNTER_TRUSTED) {
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED, r);
        }
        DLOG_DEBUG("STORAGE", "Inv[%s] ok segs=%u",
                   r, static_cast<unsigned>(_spoolIndex.segments.size()));
    } else {
        _pendingCountDirty = true;
        _setCounterTrustState(STORAGE_COUNTER_DEGRADED, r);
        _spoolAuditRepairRequired = true;
        DLOG_WARN("STORAGE",
                  "Inv[%s] marked repair-required; maintenance only",
                  r);
    }

    return ok;
}

// ── Phase 7: serial diagnostic command implementations ───────────────────────

void StorageManager::spoolAuditToSerial(bool repair) {
    if (!_ready) {
        Serial.println("[SPOOL] not ready");
        return;
    }
    Serial.printf("[SPOOL] %s starting...\r\n", repair ? "repair" : "audit");
    SpoolAuditResult audit;
    const bool ok = _auditAndRepairSpool(
        repair ? "manual_repair" : "manual_audit", repair, &audit);

    const uint32_t totalRecords = audit.totalValidRecords();
    const uint32_t uploadedRetained =
        (audit.validEventRecords >= audit.rebuiltPendingTotal)
            ? (audit.validEventRecords - audit.rebuiltPendingTotal)
            : 0U;
    Serial.printf("[SPOOL] ok=%d scannedSegs=%lu scannedRecords=%lu totalRecords=%lu\r\n",
                  ok ? 1 : 0,
                  static_cast<unsigned long>(audit.scannedSegments),
                  static_cast<unsigned long>(audit.scannedRecords),
                  static_cast<unsigned long>(totalRecords));
    Serial.printf("[SPOOL] validEvents=%lu uploadedRetained=%lu validEnrichDeltas=%lu invalidRecords=%lu skippedRecords=%lu\r\n",
                  static_cast<unsigned long>(audit.validEventRecords),
                  static_cast<unsigned long>(uploadedRetained),
                  static_cast<unsigned long>(audit.validEnrichDeltas),
                  static_cast<unsigned long>(audit.invalidRecords),
                  static_cast<unsigned long>(audit.skippedRecords));
    Serial.printf("[SPOOL] quarantined=%lu unreadable=%lu\r\n",
                  static_cast<unsigned long>(audit.quarantinedSegments),
                  static_cast<unsigned long>(audit.unreadableSegments));
    Serial.printf("[SPOOL] pendingUpload %lu->%lu  nextEventId %lu->%lu\r\n",
                  static_cast<unsigned long>(audit.oldPendingTotal),
                  static_cast<unsigned long>(audit.rebuiltPendingTotal),
                  static_cast<unsigned long>(audit.oldNextEventId),
                  static_cast<unsigned long>(_nextEventId));
    Serial.printf("[SPOOL] mismatch=%d repaired=%d fatal=%d\r\n",
                  audit.hadMismatch ? 1 : 0,
                  audit.repaired    ? 1 : 0,
                  audit.hadFatalSegmentError ? 1 : 0);

    if (repair) {
        _checkSpoolInvariants("post_repair", false);
    }

    if (ok && !audit.hadFatalSegmentError) {
        _setStoredRecordCountCache(totalRecords, millis());
        (void)_persistEventMeta(true, repair ? "spool_repair_count_total"
                                             : "spool_audit_count_total");
        StorageUiMirror::publishPostAuditRecordTotal(getDisplayRecordCount(),
                                                     getDisplayEventCount(),
                                                     millis());
    }
}

void StorageManager::spoolCountToSerial() {
    if (!_ready) {
        Serial.println("[SPOOL] not ready");
        return;
    }

    Serial.println("[SPOOL] count starting...");
    SpoolAuditResult audit;
    const bool ok = _auditAndRepairSpool("manual_count", false, &audit);
    const uint32_t totalRecords = audit.totalValidRecords();
    const uint32_t uploadedRetained =
        (audit.validEventRecords >= audit.rebuiltPendingTotal)
            ? (audit.validEventRecords - audit.rebuiltPendingTotal)
            : 0U;

    Serial.printf("[SPOOL] totalRecords=%lu validEvents=%lu uploadedRetained=%lu validEnrichDeltas=%lu scannedRecords=%lu ok=%d fatal=%d\r\n",
                  static_cast<unsigned long>(totalRecords),
                  static_cast<unsigned long>(audit.validEventRecords),
                  static_cast<unsigned long>(uploadedRetained),
                  static_cast<unsigned long>(audit.validEnrichDeltas),
                  static_cast<unsigned long>(audit.scannedRecords),
                  ok ? 1 : 0,
                  audit.hadFatalSegmentError ? 1 : 0);

    uint32_t validSegments = 0;
    uint32_t untrustedSegments = 0;
    uint32_t invalidSegments = 0;
    uint32_t storedEvents = 0;
    uint32_t pendingUpload = 0;
    for (const auto& seg : _spoolIndex.segments) {
        switch (seg.trustState) {
            case SPOOL_SEGMENT_TRUSTED:
                validSegments++;
                storedEvents += seg.eventCount;
                pendingUpload +=
                    seg.pendingUploadMissionCount + seg.pendingUploadNoiseCount;
                break;
            case SPOOL_SEGMENT_UNTRUSTED:
                untrustedSegments++;
                break;
            case SPOOL_SEGMENT_INVALID:
            default:
                invalidSegments++;
                break;
        }
    }

    const uint32_t uploadedRetainedEstimate =
        (storedEvents >= pendingUpload) ? (storedEvents - pendingUpload) : 0U;

    Serial.printf("[SPOOL] validSegments=%u untrustedSegments=%u invalidSegments=%u active=%lu oldest=%lu\r\n",
                  static_cast<unsigned>(validSegments),
                  static_cast<unsigned>(untrustedSegments),
                  static_cast<unsigned>(invalidSegments),
                  static_cast<unsigned long>(_spoolIndex.activeSegmentId),
                  static_cast<unsigned long>(_spoolIndex.oldestSegmentId));
    Serial.printf("[SPOOL] indexedTotalRecords=%lu storedEvents=%lu uploadedRetainedEstimate=%lu\r\n",
                  static_cast<unsigned long>(totalRecords),
                  static_cast<unsigned long>(storedEvents),
                  static_cast<unsigned long>(uploadedRetainedEstimate));

    if (audit.scanIncomplete) {
        // A diagnostic command must never be able to strand a backlog. The
        // scan saw only part of what is on disk, so its total is a floor —
        // adopting it would drop pendingUpload below the real figure and make
        // the upload trigger refuse to run.
        Serial.printf("[SPOOL] scan incomplete (worker append unflushed);"
                      " counts shown are a floor, pendingUpload left at %lu\r\n",
                      static_cast<unsigned long>(_pendingEventCount));
        DLOG_WARN("STORAGE",
                  "Manual count reconcile skipped: scan incomplete pending=%lu scanned=%lu",
                  static_cast<unsigned long>(_pendingEventCount),
                  static_cast<unsigned long>(audit.rebuiltPendingTotal));
    } else if (ok && !audit.hadFatalSegmentError) {
        const bool summaryPending =
            _spoolSummaryRebuildPending || _hasInvalidSpoolSummaries();
        _spoolIndex.pendingTotal = audit.rebuiltPendingTotal;
        _pendingEventCount = audit.rebuiltPendingTotal;

        const uint32_t rebuiltNextFloor =
            (audit.maxEventIdSeen == UINT32_MAX)
                ? UINT32_MAX
                : std::max<uint32_t>(audit.maxEventIdSeen + 1U,
                                     audit.validEventRecords + 1U);
        const uint32_t reconciledNext =
            std::max<uint32_t>(_nextEventId == 0U ? 1U : _nextEventId,
                               rebuiltNextFloor);
        if (_nextEventId != reconciledNext ||
            _spoolIndex.nextEventId != reconciledNext) {
            _nextEventId = reconciledNext;
            _spoolIndex.nextEventId = reconciledNext;
            _eventCounterDirty = true;
            _eventCounterPendingWrites = 1;
        }

        _bumpStorageMetaGeneration();
        _pendingCountDirty = false;
        _spoolIndexDirty = true;
        _setStoredRecordCountCache(totalRecords, millis());
        const bool persisted =
            _persistEventCounter(true, "manual_count_reconcile") &&
            _persistSpoolIndex(true, "manual_count_reconcile") &&
            _persistEventMeta(true, "manual_count_reconcile");
        _repairRequested = false;
        _spoolAuditRepairRequired = false;
        _maintenanceCaptureGate = false;
        _clearMaintenanceFlags(STORAGE_MAINT_SEGMENT_AUDIT |
                               STORAGE_MAINT_COUNTER_UNTRUSTED |
                               STORAGE_MAINT_SNAPSHOT_LAGGED);
        if (summaryPending) {
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                  "manual_count_reconcile");
            requestMaintenance(STORAGE_MAINT_DIRTY_SUMMARY,
                               "manual_count_reconcile");
        } else {
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED,
                                  "manual_count_reconcile");
        }
        DLOG_INFO("STORAGE",
                  "Manual count reconcile clean summaryPending=%d flags=%s",
                  summaryPending ? 1 : 0,
                  maintenanceFlagsText());

        STATE_WRITE_BEGIN();
        StorageUiMirror::writePostReconcileCounters_locked(
            _pendingEventCount,
            getDisplayEventCount(),
            getDisplayRecordCount(),
            millis(),
            _spoolSummaryRebuildPending || _hasInvalidSpoolSummaries());
        g_state.sessionFilesPending = static_cast<int>(_pendingEventCount);
        g_state.kaliSyncAvailable = (_pendingEventCount > 0);
        g_state.kaliSyncPending = (_pendingEventCount > 0);
        g_state.dataRefresh = true;
        STATE_WRITE_END();

        Serial.printf("[SPOOL] reconciled pendingUpload %lu->%lu nextEventId %lu->%lu persisted=%d\r\n",
                      static_cast<unsigned long>(audit.oldPendingTotal),
                      static_cast<unsigned long>(_pendingEventCount),
                      static_cast<unsigned long>(audit.oldNextEventId),
                      static_cast<unsigned long>(_nextEventId),
                      persisted ? 1 : 0);
    }

    if (isPendingEventCountAuthoritative()) {
        Serial.printf("[SPOOL] pendingUpload=%lu nextEventId=%lu sessions=%u\r\n",
                      static_cast<unsigned long>(_pendingEventCount),
                      static_cast<unsigned long>(_nextEventId),
                      static_cast<unsigned>(_spoolIndex.sessions.size()));
    } else {
        Serial.printf("[SPOOL] pendingUpload=unknown nextEventId=%lu sessions=%u\r\n",
                      static_cast<unsigned long>(_nextEventId),
                      static_cast<unsigned>(_spoolIndex.sessions.size()));
    }

    Serial.printf("[SPOOL] used=%s auditRepairReq=%d summaryRebuildPending=%d\r\n",
                  getUsedString().c_str(),
                  _spoolAuditRepairRequired ? 1 : 0,
                  _spoolSummaryRebuildPending ? 1 : 0);
}

void StorageManager::spoolEnrichToSerial() {
    if (!_ready) {
        Serial.println("[SPOOL] not ready");
        return;
    }

    Serial.println("[SPOOL] enrich starting...");

    StorageUiSnapshot snap =
        _buildStorageUiSnapshot(getFreeBytes(), getUsedPercent());
    const uint32_t freeInternal =
        heap_caps_get_free_size(SPECTRE_CAP_DRAM);
    const uint32_t largestInternal =
        heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM);

    const bool exactPendingCountsAllowed =
        freeInternal >= STORAGE_SPOOL_ENRICH_EXACT_MIN_FREE_INTERNAL &&
        largestInternal >= STORAGE_SPOOL_ENRICH_EXACT_MIN_LARGEST_BLOCK;

    StorageLaneCounts pending{};
    bool usedExactPendingCounts = false;
    if (exactPendingCountsAllowed) {
        pending = getPendingEnrichmentCounts();
        usedExactPendingCounts = true;
    } else {
        pending.mission = snap.pendingEnrichMission;
        pending.noise = snap.pendingEnrichNoise;
    }

    const uint32_t pendingTotal = pending.total();
    const uint32_t alreadyEnriched = snap.enrichmentDeltas;
    const uint32_t trackedTotal = pendingTotal + alreadyEnriched;

    Serial.printf("[SPOOL] pendingEnrich mission=%lu noise=%lu total=%lu\r\n",
                  static_cast<unsigned long>(pending.mission),
                  static_cast<unsigned long>(pending.noise),
                  static_cast<unsigned long>(pendingTotal));
    Serial.printf("[SPOOL] enriched=%lu trackedTotal=%lu summaryValid=%d\r\n",
                  static_cast<unsigned long>(alreadyEnriched),
                  static_cast<unsigned long>(trackedTotal),
                  snap.summaryValid ? 1 : 0);
    if (!usedExactPendingCounts) {
        Serial.printf("[SPOOL] exact enrichment scan deferred reason=heap_guard"
                      " freeInternal=%lu largestInternal=%lu\r\n",
                      static_cast<unsigned long>(freeInternal),
                      static_cast<unsigned long>(largestInternal));
    }
    Serial.printf("[SPOOL] used=%s auditRepairReq=%d summaryRebuildPending=%d\r\n",
                  getUsedString().c_str(),
                  _spoolAuditRepairRequired ? 1 : 0,
                  _spoolSummaryRebuildPending ? 1 : 0);
}

void StorageManager::spoolDiagToSerial() {
    if (!_ready) {
        Serial.println("[SPOOL] not ready");
        return;
    }

    uint32_t validSegments = 0;
    uint32_t untrustedSegments = 0;
    uint32_t invalidSegments = 0;
    uint32_t totalRecords = getStoredRecordCount();
    uint32_t storedEvents = 0;
    uint32_t pendingUpload = 0;
    for (const auto& seg : _spoolIndex.segments) {
        switch (seg.trustState) {
            case SPOOL_SEGMENT_TRUSTED:
                validSegments++;
                storedEvents += seg.eventCount;
                pendingUpload +=
                    seg.pendingUploadMissionCount + seg.pendingUploadNoiseCount;
                break;
            case SPOOL_SEGMENT_UNTRUSTED: untrustedSegments++; break;
            case SPOOL_SEGMENT_INVALID:
            default:                      invalidSegments++; break;
        }
    }
    const uint32_t uploadedRetainedEstimate =
        (storedEvents >= pendingUpload) ? (storedEvents - pendingUpload) : 0U;

    Serial.printf("[SPOOL] validSegments=%u untrustedSegments=%u invalidSegments=%u active=%lu oldest=%lu\r\n",
                  static_cast<unsigned>(validSegments),
                  static_cast<unsigned>(untrustedSegments),
                  static_cast<unsigned>(invalidSegments),
                  static_cast<unsigned long>(_spoolIndex.activeSegmentId),
                  static_cast<unsigned long>(_spoolIndex.oldestSegmentId));
    Serial.printf("[SPOOL] indexedTotalRecords=%lu storedEvents=%lu uploadedRetainedEstimate=%lu\r\n",
                  static_cast<unsigned long>(totalRecords),
                  static_cast<unsigned long>(storedEvents),
                  static_cast<unsigned long>(uploadedRetainedEstimate));
    if (isPendingEventCountAuthoritative()) {
        Serial.printf("[SPOOL] pendingUpload=%lu nextEventId=%lu sessions=%u\r\n",
                      static_cast<unsigned long>(_pendingEventCount),
                      static_cast<unsigned long>(_nextEventId),
                      static_cast<unsigned>(_spoolIndex.sessions.size()));
    } else {
        Serial.printf("[SPOOL] pendingUpload=unknown nextEventId=%lu sessions=%u\r\n",
                      static_cast<unsigned long>(_nextEventId),
                      static_cast<unsigned>(_spoolIndex.sessions.size()));
    }
    StorageUiSnapshot snap =
        _buildStorageUiSnapshot(getFreeBytes(), getUsedPercent());
    const uint32_t freeInternal =
        heap_caps_get_free_size(SPECTRE_CAP_DRAM);
    const uint32_t largestInternal =
        heap_caps_get_largest_free_block(SPECTRE_CAP_DRAM);

    const bool exactPendingCountsAllowed =
        freeInternal >= STORAGE_SPOOL_ENRICH_EXACT_MIN_FREE_INTERNAL &&
        largestInternal >= STORAGE_SPOOL_ENRICH_EXACT_MIN_LARGEST_BLOCK;

    StorageLaneCounts pendingEnrich{};
    if (exactPendingCountsAllowed) {
        pendingEnrich = getPendingEnrichmentCounts();
    } else {
        pendingEnrich.mission = snap.pendingEnrichMission;
        pendingEnrich.noise = snap.pendingEnrichNoise;
    }
    Serial.printf("[SPOOL] pendingEnrich mission=%lu noise=%lu total=%lu\r\n",
                  static_cast<unsigned long>(pendingEnrich.mission),
                  static_cast<unsigned long>(pendingEnrich.noise),
                  static_cast<unsigned long>(pendingEnrich.total()));
    if (!exactPendingCountsAllowed) {
        Serial.printf("[SPOOL] exact enrichment scan deferred reason=heap_guard"
                      " freeInternal=%lu largestInternal=%lu\r\n",
                      static_cast<unsigned long>(freeInternal),
                      static_cast<unsigned long>(largestInternal));
    }
    Serial.printf("[SPOOL] used=%s auditRepairReq=%d summaryRebuildPending=%d\r\n",
                  getUsedString().c_str(),
                  _spoolAuditRepairRequired ? 1 : 0,
                  _spoolSummaryRebuildPending ? 1 : 0);

    for (const auto& seg : _spoolIndex.segments) {
        Serial.printf("[SPOOL]   seg=%lu fmt=%s trust=%s records=%lu storedEvents=%lu"
                      " storedLaneEvents=%lu pendingUpload=%lu pendingMission=%lu pendingNoise=%lu pendingEnrich=%lu summaryOk=%d active=%d\r\n",
                      static_cast<unsigned long>(seg.segmentId),
                      _segmentFormatText(seg.format),
                      _spoolTrustText(seg.trustState),
                      static_cast<unsigned long>(seg.recordCount),
                      static_cast<unsigned long>(seg.eventCount),
                      static_cast<unsigned long>(seg.missionCount + seg.noiseCount),
                      static_cast<unsigned long>(seg.pendingUploadMissionCount +
                                                 seg.pendingUploadNoiseCount),
                      static_cast<unsigned long>(seg.pendingUploadMissionCount),
                      static_cast<unsigned long>(seg.pendingUploadNoiseCount),
                      static_cast<unsigned long>(seg.pendingEnrichmentCount),
                      seg.summaryValid ? 1 : 0,
                      (seg.segmentId == _spoolIndex.activeSegmentId) ? 1 : 0);
    }

    _logSpoolDiagnostics("manual_diag");
}

void StorageManager::spoolQuarantineListToSerial() {
    const auto listDir = [](const char* dirPath) {
        Serial.printf("[SPOOL] -- %s --\r\n", dirPath);
        File dir = LittleFS.open(dirPath);
        if (!dir || !dir.isDirectory()) {
            Serial.println("[SPOOL]   (empty)");
            return;
        }
        File f = dir.openNextFile();
        bool any = false;
        while (f) {
            Serial.printf("[SPOOL]   %-44s  %6lu B\r\n",
                          f.name(),
                          static_cast<unsigned long>(f.size()));
            f = dir.openNextFile();
            any = true;
        }
        if (!any) {
            Serial.println("[SPOOL]   (empty)");
        }
    };
    listDir(PATH_SPOOL_BAD_LOGS);
    listDir(PATH_SPOOL_BAD_META);
}

void StorageManager::spoolQuarantineMetaToSerial() {
    File dir = LittleFS.open(PATH_SPOOL_BAD_META);
    if (!dir || !dir.isDirectory()) {
        Serial.println("[SPOOL] quarantine meta: empty");
        return;
    }
    File f = dir.openNextFile();
    bool any = false;
    while (f) {
        Serial.printf("[SPOOL] === %s ===\r\n", f.name());
        while (f.available()) {
            Serial.write(f.read());
        }
        Serial.println();
        f = dir.openNextFile();
        any = true;
    }
    if (!any) {
        Serial.println("[SPOOL] quarantine meta: empty");
    }
}

bool StorageManager::spoolQuarantineClear() {
    // Collect before deleting — iterating a LittleFS dir while removing
    // entries from it is not safe.
    std::vector<String> toRemove;
    const auto collect = [&](const char* dirPath) {
        File dir = LittleFS.open(dirPath);
        if (!dir || !dir.isDirectory()) return;
        File f = dir.openNextFile();
        while (f) {
            toRemove.push_back(String(dirPath) + "/" + f.name());
            f = dir.openNextFile();
        }
    };
    collect(PATH_SPOOL_BAD_LOGS);
    collect(PATH_SPOOL_BAD_META);

    bool ok = true;
    for (const auto& path : toRemove) {
        if (!LittleFS.remove(path)) {
            DLOG_WARN("STORAGE", "Quarantine clear remove failed path=%s",
                      path.c_str());
            ok = false;
        }
    }

    Serial.printf("[SPOOL] quarantine clear %s removed=%u\r\n",
                  ok ? "ok" : "partial",
                  static_cast<unsigned>(toRemove.size()));
    DLOG_INFO("STORAGE", "Quarantine cleared removed=%u ok=%d",
              static_cast<unsigned>(toRemove.size()), ok ? 1 : 0);
    return ok;
}

void StorageManager::_initDefaultConfig() {
    if (SETTINGS.isReady()) {
        _copySettingsToConfig(SETTINGS.snapshot(), _config);
        return;
    }

    _config.name         = SPECTRE_DEVICE_NAME;
    _config.owner        = SPECTRE_DEVICE_OWNER;
    _config.version      = SPECTRE_DEVICE_VERSION;
    _config.loraFreq     = SPECTRE_LORA_FREQUENCY;
    _config.loraNetworkId = 6;
    _config.loraAddress  = 1;
    _config.loraSF       = 9;
    _config.loraBW       = 7;
    _config.loraCR       = 1;
    _config.loraPreamble = 12;
    _config.mqttBroker   = "";
    _config.mqttPort     = 1883;
    _config.mqttUser     = "";
    _config.mqttPassword = "";
    _config.mqttTopicBase = SPECTRE_MQTT_TOPIC_BASE;
}

// =====================================================================
// Codec round-trip self-test.
//
// The v2 payload moved a dozen fields from keyed pairs into positional slots.
// A layout mistake there is silent: it does not fail to write, it writes bytes
// that read back as something else. This drives the real writer and the real
// reader and diffs every field, so a wire-format regression fails loudly.
// =====================================================================
namespace {

struct CodecSelfTestCase {
    const char* type;
    const char* subtype;
};

bool _codecFieldsMatch(JsonObjectConst want, JsonObjectConst got, String& firstBad) {
    bool ok = true;
    for (JsonPairConst kv : want) {
        const char* key = kv.key().c_str();
        // Fields the storage layer legitimately owns or rewrites.
        if (strcmp(key, "id") == 0 || strcmp(key, "ts") == 0 ||
            strcmp(key, "status") == 0 || strcmp(key, F_ENRICH_STATE) == 0 ||
            strcmp(key, F_TIMESTAMP_ISO) == 0 || strcmp(key, F_SESSION) == 0) {
            continue;
        }
        JsonVariantConst a = kv.value();
        JsonVariantConst b = got[key];
        if (b.isNull()) {
            if (ok) firstBad = String("missing:") + key;
            ok = false;
            continue;
        }
        // Compare as text: the codec is allowed to change numeric width, but
        // not value.
        String as, bs;
        serializeJson(a, as);
        serializeJson(b, bs);
        if (as != bs) {
            // ints written as 1/0 may read back as true/false and vice versa.
            const bool boolish =
                (as == "true" && bs == "1") || (as == "1" && bs == "true") ||
                (as == "false" && bs == "0") || (as == "0" && bs == "false");
            if (!boolish) {
                if (ok) firstBad = String(key) + " want=" + as + " got=" + bs;
                ok = false;
            }
        }
    }
    return ok;
}

}  // namespace

bool StorageManager::spoolCodecSelfTestToSerial() {
    if (!_ready) {
        DLOG_WARN("STORAGE", "codec selftest: storage not ready");
        return false;
    }

    const CodecSelfTestCase cases[] = {
        {"probe", ""}, {"device", ""}, {"network", ""},
        {"pmkid", ""}, {"drone", ""}, {"event", "handshake"},
    };

    // Sum of live segment bodies -- the delta across one append is that
    // record's true on-disk cost, prefix included.
    auto totalSpoolBytes = [this]() -> uint32_t {
        uint32_t total = 0;
        for (const auto& seg : _spoolIndex.segments) total += seg.approxBytes;
        return total;
    };

    // Vary the radio addresses per run: identical synthetic records would be
    // dropped by the duplicate suppressor on a second invocation and the test
    // would report a spurious append failure.
    const uint32_t nonce = millis();
    char macA[18], macB[18], macC[18];
    snprintf(macA, sizeof(macA), "AA:BB:CC:%02X:%02X:%02X",
             static_cast<unsigned>((nonce >> 16) & 0xFF),
             static_cast<unsigned>((nonce >> 8) & 0xFF),
             static_cast<unsigned>(nonce & 0xFF));
    snprintf(macB, sizeof(macB), "AA:BB:DD:%02X:%02X:%02X",
             static_cast<unsigned>((nonce >> 16) & 0xFF),
             static_cast<unsigned>((nonce >> 8) & 0xFF),
             static_cast<unsigned>(nonce & 0xFF));
    snprintf(macC, sizeof(macC), "AA:BB:EE:%02X:%02X:%02X",
             static_cast<unsigned>((nonce >> 16) & 0xFF),
             static_cast<unsigned>((nonce >> 8) & 0xFF),
             static_cast<unsigned>(nonce & 0xFF));

    bool allOk = true;
    std::vector<uint32_t> targetIds;
    DLOG_INFO("STORAGE",
              "codec selftest begin -- writes 6 synthetic records to the spool");

    for (const auto& tc : cases) {
        JsonDocument want;
        JsonObject w = want.to<JsonObject>();
        const String type(tc.type);

        // Envelope fields the capture path always supplies.
        w["sensor"] = SPECTRE_MQTT_SENSOR_ID;
        w["session_id"] = SESS.getId();

        if (type == "probe" || type == "device" || type == "network") {
            if (type == "network") {
                w["bssid"] = macA;
                w["ssid"] = "SelfTestAP";
                w["security"] = "WPA2";
                w["is_hidden"] = 0;
                w["has_wps"] = 1;
                w["track_id"] = String("AP:") + macA;
                // Only APs advertise transmit power, so cover it on the
                // network case where it actually occurs.
                w["tx_power"] = 20;
                w["tx_power_src"] = 1;   // TPC report
            } else {
                w["mac"] = macB;
                w["ie_fingerprint"] = "8f2a91c4";
                if (type == "probe") {
                    w["probed_ssid"] = "SelfTestProbe";
                    w["is_broadcast"] = 0;
                    w["track_id"] = "IE:8f2a91c4";
                } else {
                    w["probe_set_hash"] = "deadbeef";
                    w["is_random_mac"] = 1;
                    w["track_id"] = String("MAC:") + macB;
                }
            }
            w["rssi"] = -67;
            w["channel"] = 6;
            // RF context travels with every real capture; cover it here so a
            // layout slip in the RFCTX block fails loudly.
            w["noise_floor"] = -96;
            w["ant_gain_q2"] = 36;   // 9 dBi
            w["localization_sample"] = true;
            w["sample_seq"] = 12;
            w["sample_frames"] = 34;
            w["rssi_min"] = -72;
            w["rssi_max"] = -61;
            w["sample_reason"] = "signal_delta";
        } else if (type == "pmkid") {
            w["ap"] = macA;
            w["sta"] = macB;
            w["ssid"] = "SelfTestAP";
            w["rssi"] = -55;
            w["pmkid_hex"] = "00112233445566778899aabbccddeeff";
        } else if (type == "drone") {
            w["drone_id"] = "SELFTEST-DRONE";
            w["mac"] = macC;
            w["rssi"] = -70;
            w["channel"] = 11;
            w["protocol"] = "wifi";
            w["latitude"] = 47.6205;
            w["longitude"] = -122.3493;
        } else {  // event/handshake
            w["event_type"] = "handshake";
            w["ap"] = macA;
            w["sta"] = macB;
            w["ssid"] = "SelfTestAP";
            w["rssi"] = -60;
            w["frame_mask"] = 0x0FU;
            w["message"] = 2;
        }

        const uint32_t bytesBefore = totalSpoolBytes();
        const AppendEventResult res =
            _appendEventDetailedInternal(tc.type, want.as<JsonObjectConst>(),
                                         nullptr, nullptr, false, true, true,
                                         nullptr);
        if (!res.ok()) {
            DLOG_WARN("STORAGE", "codec selftest type=%s append failed status=%u",
                      tc.type, static_cast<unsigned>(res.status));
            allOk = false;
            continue;
        }
        // A rotate or compaction between the two samples can shrink the total,
        // so the delta is signed; a negative one means the measurement was
        // spoiled by segment churn rather than that the record was free.
        targetIds.push_back(res.eventId);
        const int64_t recordBytesSigned =
            static_cast<int64_t>(totalSpoolBytes()) - static_cast<int64_t>(bytesBefore);

        // Read it back through the scan decoder. Segments carry their event-id
        // range, so scan only the one that can hold this record -- walking all
        // of them costs a full-spool decode per record, which on a real backlog
        // is minutes, not milliseconds.
        JsonDocument got;
        bool found = false;
        for (const auto& seg : _spoolIndex.segments) {
            if (found) break;
            if (seg.lastEventId && seg.lastEventId < res.eventId) continue;
            if (seg.firstEventId && seg.firstEventId > res.eventId) continue;
            _scanSegmentRecords(seg.segmentId,
                [&](const DecodedSpoolRecord& rec) -> bool {
                    if (rec.recordType != SPOOL_REC_EVENT) return true;
                    if (rec.eventId != res.eventId) return true;
                    got.set(rec.doc.as<JsonVariantConst>());
                    found = true;
                    return false;
                });
        }

        if (!found) {
            DLOG_WARN("STORAGE", "codec selftest type=%s record %lu not found",
                      tc.type, static_cast<unsigned long>(res.eventId));
            allOk = false;
            continue;
        }

        String firstBad;
        const bool match = _codecFieldsMatch(want.as<JsonObjectConst>(),
                                             got.as<JsonObjectConst>(), firstBad);
        if (!match) allOk = false;

        char bytesText[16];
        if (recordBytesSigned >= 0) {
            snprintf(bytesText, sizeof(bytesText), "%ld",
                     static_cast<long>(recordBytesSigned));
        } else {
            // Segment churn ate the delta; the byte figure is not meaningful.
            snprintf(bytesText, sizeof(bytesText), "n/a");
        }
        DLOG_INFO("STORAGE",
                  "codec selftest type=%-8s bytes=%-4s %s%s",
                  tc.type,
                  bytesText,
                  match ? "OK" : "MISMATCH ",
                  match ? "" : firstBad.c_str());
    }

    // ---------------------------------------------------------------
    // Enrichment round-trip. REC_ENRICH_DELTA_V2 delta-codes every numeric
    // field against the previous enrichment in the segment, so a writer/reader
    // drift does not fail loudly -- it silently returns a position a few metres
    // wrong, or an event id off by one. Walk a synthetic track and check every
    // field of every record, including the no-data case whose coordinates are
    // omitted from the wire entirely.
    // ---------------------------------------------------------------
    if (!targetIds.empty()) {
        struct EnrichCase {
            double lat, lon, alt, acc;
            uint32_t gpsEpoch;
            bool noData;
        };
        // Consecutive fixes a few metres apart -- the case the delta coding is
        // built for -- plus a no-data record in the middle to prove it neither
        // consumes coordinate bytes nor poisons the running position.
        const EnrichCase cases[] = {
            {47.6205000, -122.3493000, 56.25, 4.0, 1787000000U, false},
            {47.6205400, -122.3492600, 56.75, 3.5, 1787000006U, false},
            {0.0,          0.0,         0.0,  0.0,          0U, true },
            {47.6205900, -122.3492100, 57.00, 3.0, 1787000019U, false},
            {47.6206500, -122.3491500, 57.50, 5.0, 1787000027U, false},
        };
        const size_t n = std::min(targetIds.size(),
                                  sizeof(cases) / sizeof(cases[0]));

        const String sessionId = SESS.getId();
        std::vector<SpoolEnrichBatchEntry> batch;
        batch.reserve(n);
        for (size_t i = 0; i < n; i++) {
            SpoolEnrichBatchEntry e;
            e.eventId = targetIds[i];
            e.sessionId = sessionId.c_str();
            e.lat = static_cast<float>(cases[i].lat);
            e.lon = static_cast<float>(cases[i].lon);
            e.alt = static_cast<float>(cases[i].alt);
            e.acc = static_cast<float>(cases[i].acc);
            e.tag = nullptr;
            e.gpsEpochUtc = cases[i].gpsEpoch;
            e.noData = cases[i].noData;
            batch.push_back(e);
        }

        const uint32_t beforeBytes = totalSpoolBytes();
        uint32_t applied = 0, failedCount = 0;
        const bool batchOk =
            appendEnrichDeltasBatch(batch.data(), batch.size(),
                                    &applied, &failedCount);
        const int64_t enrichBytes =
            static_cast<int64_t>(totalSpoolBytes()) -
            static_cast<int64_t>(beforeBytes);

        if (!batchOk || applied != n) {
            DLOG_WARN("STORAGE",
                      "codec selftest enrich batch applied=%lu of %u ok=%d",
                      static_cast<unsigned long>(applied),
                      static_cast<unsigned>(n), batchOk ? 1 : 0);
            allOk = false;
        } else {
            // Read every delta back and match it to its case by target event id.
            size_t checked = 0;
            String firstBad;
            for (const auto& seg : _spoolIndex.segments) {
                _scanSegmentRecords(seg.segmentId,
                    [&](const DecodedSpoolRecord& rec) -> bool {
                        if (rec.recordType != SPOOL_REC_ENRICH_DELTA) return true;
                        JsonObjectConst o = rec.doc.as<JsonObjectConst>();
                        const uint32_t target = o["event_id"] | 0U;
                        for (size_t i = 0; i < n; i++) {
                            if (targetIds[i] != target) continue;
                            const EnrichCase& c = cases[i];
                            const bool noData = (o["enrich_no_data"] | false);
                            if (noData != c.noData) {
                                if (!firstBad.length())
                                    firstBad = String("no_data flag event=") + target;
                                allOk = false;
                            } else if (!c.noData) {
                                // Compare in E7 integer space against what the
                                // writer would have stored from the same float.
                                // SpoolEnrichBatchEntry holds lat/lon as float,
                                // which alone costs ~1e-6 deg -- comparing
                                // against the original double would be testing
                                // float32, not the codec. Here the only
                                // permitted difference is zero.
                                const int32_t wantLatE7 =
                                    _floatToE7(static_cast<float>(c.lat));
                                const int32_t wantLonE7 =
                                    _floatToE7(static_cast<float>(c.lon));
                                const int32_t gotLatE7 =
                                    _floatToE7(o["lat"] | 0.0f);
                                const int32_t gotLonE7 =
                                    _floatToE7(o["lon"] | 0.0f);
                                const double dAlt = fabs((o["alt"] | 0.0) - c.alt);
                                const uint32_t gps = o[F_GPS_TS] | 0U;
                                if (gotLatE7 != wantLatE7 || gotLonE7 != wantLonE7) {
                                    if (!firstBad.length())
                                        firstBad = String("coords event=") + target +
                                                   " dLatE7=" + String(gotLatE7 - wantLatE7) +
                                                   " dLonE7=" + String(gotLonE7 - wantLonE7);
                                    allOk = false;
                                } else if (dAlt > 0.02) {
                                    if (!firstBad.length())
                                        firstBad = String("alt event=") + target;
                                    allOk = false;
                                } else if (gps != c.gpsEpoch) {
                                    if (!firstBad.length())
                                        firstBad = String("gps_ts event=") + target +
                                                   " got=" + gps;
                                    allOk = false;
                                }
                            }
                            checked++;
                            break;
                        }
                        return true;
                    });
            }
            if (checked != n) {
                DLOG_WARN("STORAGE",
                          "codec selftest enrich readback found %u of %u",
                          static_cast<unsigned>(checked),
                          static_cast<unsigned>(n));
                allOk = false;
            }
            DLOG_INFO("STORAGE",
                      "codec selftest enrich n=%u bytes=%ld (%ld B/delta) %s%s",
                      static_cast<unsigned>(n),
                      static_cast<long>(enrichBytes),
                      static_cast<long>(enrichBytes / (n ? n : 1)),
                      (allOk && checked == n) ? "OK" : "MISMATCH ",
                      firstBad.c_str());
        }
    }

    DLOG_INFO("STORAGE", "codec selftest %s", allOk ? "PASSED" : "FAILED");
    return allOk;
}
