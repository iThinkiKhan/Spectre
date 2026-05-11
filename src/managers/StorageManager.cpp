

#include "StorageManager.h"
#include "../config.h"
#include "../core/DebugLog.h"
#include "../core/MissionRuntime.h"
#include "../core/RuntimeContracts.h"
#include "RadioArbiter.h"
#include "SettingsManager.h"
#include "RAMSpool.h"
#include "SpoolBinaryCodec.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <functional>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <new>

namespace {

bool _isReservedEventKey(const char* key) {
    return strcmp(key, "id") == 0 ||
           strcmp(key, "ts") == 0 ||
           strcmp(key, "type") == 0 ||
           strcmp(key, F_ENRICH_STATE) == 0 ||
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
           a.recordCount == b.recordCount &&
           a.eventCount == b.eventCount &&
           a.enrichDeltaCount == b.enrichDeltaCount &&
           a.missionCount == b.missionCount &&
           a.noiseCount == b.noiseCount &&
           a.pendingUploadMissionCount == b.pendingUploadMissionCount &&
           a.pendingUploadNoiseCount == b.pendingUploadNoiseCount &&
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
    BIN_EVT_EVENT = 5
};

enum BinaryPayloadFamilyCode : uint8_t {
    BIN_PAYLOAD_JSON_FALLBACK = 0,   // read-only compatibility
    BIN_PAYLOAD_PROBE_DEVICE = 1,
    BIN_PAYLOAD_PMKID = 2,
    BIN_PAYLOAD_HANDSHAKE = 3,
    BIN_PAYLOAD_DRONE = 4,
    BIN_PAYLOAD_FIELD_MAP = 5
};

enum BinarySessionMode : uint8_t {
    BIN_SESSION_INLINE = 0,
    BIN_SESSION_SAME_AS_PREV = 1
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

static bool _isStructuredBinaryField(const String& typeStr,
                                     const String& eventSubtype,
                                     const char* key) {
    if (!key || !key[0]) return true;
    if (_isReservedEventKey(key)) return true;

    if (strcmp(key, "event_type") == 0) {
        return (typeStr == "event" && eventSubtype == "handshake");
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
    return BIN_EVT_CUSTOM;
}

static const char* _binaryEventTypeStringFromCode(uint8_t code) {
    switch (code) {
        case BIN_EVT_PROBE:  return "probe";
        case BIN_EVT_DEVICE: return "device";
        case BIN_EVT_DRONE:  return "drone";
        case BIN_EVT_PMKID:  return "pmkid";
        case BIN_EVT_EVENT:  return "event";
        case BIN_EVT_CUSTOM:
        default:             return "";
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

static uint32_t _crc32Bytes(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }
    return ~crc;
}

static uint32_t _uploadIndexRecordHash(const UploadIndexRecordV1& record) {
    return _crc32Bytes(reinterpret_cast<const uint8_t*>(&record),
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

static bool _decodeBinaryMetaRecord(const uint8_t* data,
                                    size_t len,
                                    uint8_t recordPrefixType,
                                    String& lastSession,
                                    BinaryMetaRecord& out) {
    out = {};

    const uint8_t* p = data;
    const uint8_t* end = data + len;

    if (recordPrefixType == SpoolBin::REC_ENRICH_DELTA) {
            uint32_t recordId = 0;
            uint32_t tsDelta = 0;
            uint8_t sessionMode = BIN_SESSION_INLINE;
            uint8_t enrichFlags = 0;
            String sessionId;
            uint32_t targetEventId = 0;
            int32_t latE7 = 0;
            int32_t lonE7 = 0;
            int32_t altCm = 0;
            uint32_t accDm = 0;
            String tag;

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

        if (p >= end) {
                return false;
            }
            enrichFlags = *p++;

            if (!_readUVarintFromBytes(p, end, targetEventId) ||
                !_readZigZag32FromBytes(p, end, latE7) ||
                !_readZigZag32FromBytes(p, end, lonE7) ||
                !_readZigZag32FromBytes(p, end, altCm) ||
                !_readUVarintFromBytes(p, end, accDm)) {
            return false;
        }

        if (enrichFlags & 0x01) {
            if (!_readStringFromBytes(p, end, tag)) {
                return false;
            }
        }

        out.recordType = SPOOL_REC_ENRICH_DELTA;
        out.eventId = recordId;
        out.sessionId = sessionId;
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
        if (!_decodeBinaryMetaRecord(body.data(), body.size(), prefix.type, lastSession, rec)) {
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
        StorageManager::SpoolAuditResult& audit) {

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
                                     prefix.type, lastSession, rec)) {
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

static constexpr const char* PATH_SPOOL          = "/spool";
static constexpr const char* PATH_SPOOL_INDEX     = "/spool/index.json";
static constexpr const char* PATH_SPOOL_BAD       = "/spool_bad";
static constexpr const char* PATH_SPOOL_BAD_LOGS  = "/spool_bad/logs";
static constexpr const char* PATH_SPOOL_BAD_META  = "/spool_bad/meta";
static constexpr size_t   SPOOL_SEGMENT_TARGET_BYTES             = 48UL * 1024UL;
static constexpr uint32_t SPOOL_ENRICH_PREFLIGHT_ROTATE_BYTES   = 64U * 1024U;
static constexpr uint32_t SPOOL_ENRICH_PREFLIGHT_ROTATE_RECORDS = 128U;
static constexpr uint32_t SPOOL_ENRICH_PREFLIGHT_ROTATE_DELTAS  = 96U;

static constexpr const char* PATH_STORE_CONFIG_DIR = "/config";
static constexpr const char* PATH_STORE_VAULT_DIR = "/config/vault";
static constexpr const char* PATH_STORE_KNOWN_LOCATIONS = "/config/vault/known_locations.json";
static constexpr const char* PATH_STORE_LEGACY_KNOWN_LOCATIONS = "/config/locations.json";
static constexpr const char* PATH_LEGACY_MQTT_QUEUE = "/mqtt_queue";
static constexpr const char* PATH_LEGACY_PMKID_DIR = "/pmkid";
static constexpr const char* PATH_VOLATILE_VAULT_DIR = "/vault";
static constexpr const char* PATH_LEGACY_BADUSB_DIR = "/vault/badusb";
static constexpr const char* PATH_LEGACY_BADUSB_INDEX = "/vault/badusb/index.json";
static constexpr size_t SESSION_LOG_MAX_LINES = 128;

static const char* _spoolCorruptionReasonText(StorageManager::SpoolCorruptionReason reason) {
    switch (reason) {
        case StorageManager::SpoolCorruptionReason::SEGMENT_OPEN_FAILED:
            return "segment_open_failed";
        case StorageManager::SpoolCorruptionReason::SEGMENT_HEADER_INVALID:
            return "segment_header_invalid";
        case StorageManager::SpoolCorruptionReason::RECORD_LENGTH_INVALID:
            return "record_length_invalid";
        case StorageManager::SpoolCorruptionReason::RECORD_CRC_FAILED:
            return "record_crc_failed";
        case StorageManager::SpoolCorruptionReason::RECORD_DECODE_FAILED:
            return "record_decode_failed";
        case StorageManager::SpoolCorruptionReason::RECORD_SEMANTIC_INVALID:
            return "record_semantic_invalid";
        case StorageManager::SpoolCorruptionReason::SCAN_FAILED:
            return "scan_failed";
        case StorageManager::SpoolCorruptionReason::SUMMARY_MISMATCH:
            return "summary_mismatch";
        case StorageManager::SpoolCorruptionReason::UNKNOWN:
        case StorageManager::SpoolCorruptionReason::NONE:
        default:
            return "unknown";
    }
}

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
    if (!LittleFS.begin(true)) {
        DLOG_ERROR("STORAGE", "LittleFS mount failed");
        return false;
    }

    _ensureDir("/config");
    _ensureDir(PATH_STORE_VAULT_DIR);

    if (!_applyOneShotNonVaultReset()) {
        DLOG_WARN("STORAGE", "One-shot non-vault reset failed");
    }

    _ensureDir(PATH_LOGS);
    _ensureDir(PATH_EVENTS);
    _ensureDir(PATH_SPOOL);
    _ensureDir(PATH_EXPORTS);
    _ensureDir(PATH_PMKID_DIR);

    // Quarantine dirs survive wipes intentionally — they hold forensic copies
    // of unrecoverable segments and are never touched by wipeNonVaultStorage().
    _ensureDir(PATH_SPOOL_BAD);
    _ensureDir(PATH_SPOOL_BAD_LOGS);
    _ensureDir(PATH_SPOOL_BAD_META);

    if (!_appendMutex) {
        _appendMutex = xSemaphoreCreateMutex();
        if (!_appendMutex) {
            DLOG_ERROR("STORAGE", "Storage append mutex create failed");
            return false;
        }
    }

    if (!_ensureSpoolReady()) {
        DLOG_ERROR("STORAGE", "Spool init failed");
        return false;
    }

#if STORAGE_FAST_BOOT_DEFER_SPOOL_REPAIR
    SpoolBootAuditResult bootAudit;
    if (!_auditSpoolBoot(bootAudit)) {
        DLOG_WARN("STORAGE", "Fast boot: repair required");

        SpoolAuditResult audit;
        if (!_auditAndRepairSpool("boot", true, &audit)) {
            DLOG_ERROR("STORAGE", "Boot spool audit failed");
            return false;
        }

        // The boot audit uses the fast binary-meta scanner which cannot populate
        // priority/lane/timestamp stats, so it leaves all BIN_V2 segments with
        // summaryValid=false.  Rebuild here while the radio arbiter hasn't started
        // yet; _servicePendingSpoolSummaryRebuild() requires radio idle (NONE) and
        // would be blocked indefinitely once WiFi capture starts at hardware init.
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
    } else {
        _spoolAuditRepairRequired = false;
        _pendingEventCount = _spoolIndex.pendingTotal;
        _spoolIndex.pendingTotal = _pendingEventCount;

        if (bootAudit.snapshotLagged) {
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                  "trusted_snapshot_lagged");
        } else {
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED, "boot_fast_ready");
        }

        if (isPendingEventCountAuthoritative()) {
            DLOG_WARN("STORAGE",
                      "Fast boot: repair not required pending=%lu summary=%d dirty=%d",
                      static_cast<unsigned long>(_pendingEventCount),
                      _spoolSummaryRebuildPending ? 1 : 0,
                      _pendingCountDirty ? 1 : 0);
        } else {
            DLOG_WARN("STORAGE",
                      "Fast boot: repair not required pending=unknown summary=%d dirty=%d",
                      _spoolSummaryRebuildPending ? 1 : 0,
                      _pendingCountDirty ? 1 : 0);
        }
    }
#else
    SpoolAuditResult audit;
    if (!_auditAndRepairSpool("boot", true, &audit)) {
        DLOG_ERROR("STORAGE", "Boot spool audit failed");
        return false;
    }

    // The boot audit uses the fast binary-meta scanner which cannot populate
    // priority/lane/timestamp stats, so it leaves all BIN_V2 segments with
    // summaryValid=false.  Rebuild here while the radio arbiter hasn't started
    // yet; _servicePendingSpoolSummaryRebuild() requires radio idle (NONE) and
    // would be blocked indefinitely once WiFi capture starts at hardware init.
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

    _releaseUploadIndexMemory("boot_skip_load");

    if (!loadConfig()) {
        DLOG_WARN("STORAGE", "Settings unavailable, using defaults");
    }

    _ready = true;
    _resetBinaryUnsupportedAudit();
    _cachedUsedString = getUsedString();
    DLOG_INFO("STORAGE", "Ready. Used=%s Free=%uKB / %uKB",
              _cachedUsedString.c_str(),
              static_cast<unsigned>(getFreeBytes() / 1024),
              static_cast<unsigned>(getTotalBytes() / 1024));

    if (getUsedPercent() > 80) {
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

String StorageManager::_makeDedupKey(const char* type, JsonObjectConst payload) const {
    String key = String(type ? type : "event");
    key += "|";

    if (strcmp(type ? type : "", "probe") == 0) {
        key += String(payload["mac"] | "");
        key += "|";
        key += String(payload["probed_ssid"] | payload["ssid"] | "");
        key += "|";
        key += String(payload["channel"] | "");
        return key;
    }

    if (strcmp(type ? type : "", "device") == 0) {
        key += String(payload["mac"] | "");
        key += "|";
        key += String(payload["probe_set_hash"] | "");
        key += "|";
        key += String(payload["ie_fingerprint"] | "");
        return key;
    }

    if (strcmp(type ? type : "", "drone") == 0) {
        key += String(payload["id"] | payload["drone_id"] | "");
        return key;
    }

    if (strcmp(type ? type : "", "pmkid") == 0) {
        key += String(payload["ap"] | payload["bssid"] | "");
        key += "|";
        key += String(payload["sta"] | payload["client"] | payload["client_mac"] | "");
        key += "|";
        key += String(payload["pmkid_hex"] | "");
        return key;
    }

    if (strcmp(type ? type : "", "subghz") == 0) {
        key += String(payload["source_addr"] | "");
        key += "|";
        key += String(payload["frequency_hz"] | "");
        key += "|";
        key += String(payload["payload_hex"] | payload["payload"] | "");
        return key;
    }

    key += String(payload["mac"] | "");
    key += "|";
    key += String(payload["ssid"] | "");
    key += "|";
    key += String(payload["detail"] | "");
    return key;
}

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

void StorageManager::_trimDedupWindows() {
    const uint32_t now = millis();

    _dedupWindow.erase(
        std::remove_if(_dedupWindow.begin(), _dedupWindow.end(),
            [now](const DedupWindowEntry& e) {
                return (now - e.lastSeenMs) > DEDUP_WINDOW_MS;
            }),
        _dedupWindow.end());

    _handshakeWindow.erase(
        std::remove_if(_handshakeWindow.begin(), _handshakeWindow.end(),
            [now](const HandshakeProgress& e) {
                return (now - e.lastUpdateMs) > HANDSHAKE_WINDOW_MS;
            }),
        _handshakeWindow.end());

    if (_dedupWindow.size() > DEDUP_WINDOW_MAX) {
        _dedupWindow.erase(_dedupWindow.begin(),
                           _dedupWindow.begin() + (_dedupWindow.size() - DEDUP_WINDOW_MAX));
    }
}

bool StorageManager::_shouldSuppressDuplicate(const char* type,
                                              JsonObjectConst payload,
                                              StoragePriority priority,
                                              bool deferUiRefresh) {
    if (strcmp(type ? type : "", "subghz") == 0) {
        return false;
    }

    if (priority <= STORAGE_PRIO_P1) {
        return false;
    }

    _trimDedupWindows();

    const String key = _makeDedupKey(type, payload);
    const uint32_t now = millis();

    for (auto& entry : _dedupWindow) {
        if (entry.key == key) {
            if ((now - entry.lastSeenMs) <= DEDUP_WINDOW_MS) {
                entry.lastSeenMs = now;
                entry.count++;
                _applyCounterDelta("duplicate_suppressed",
                                   0,
                                   0,
                                   0,
                                   1,
                                   (priority <= STORAGE_PRIO_P1)
                                       ? STORAGE_LANE_MISSION
                                       : STORAGE_LANE_NOISE,
                                   priority);
                if (deferUiRefresh) {
                    _queueStorageUiRefresh(true);
                }
                return true;
            }

            entry.firstSeenMs = now;
            entry.lastSeenMs = now;
            entry.count = 1;
            return false;
        }
    }

    DedupWindowEntry e;
    e.key = key;
    e.firstSeenMs = now;
    e.lastSeenMs = now;
    e.count = 1;
    _dedupWindow.push_back(e);
    return false;
}

bool StorageManager::shouldAcceptHandshakeFrame(const char* apMac,
                                                const char* staMac,
                                                const char* ssid,
                                                uint8_t messageNumber,
                                                bool deferUiRefresh) {
    if (messageNumber < 1 || messageNumber > 4) {
        return false;
    }

    _trimDedupWindows();

    String key = String(apMac ? apMac : "");
    key += "|";
    key += String(staMac ? staMac : "");
    key += "|";
    key += String(ssid ? ssid : "");

    const uint8_t bit = static_cast<uint8_t>(1u << (messageNumber - 1));
    const uint32_t now = millis();

    for (auto& hs : _handshakeWindow) {
        String existingKey = hs.apMac + "|" + hs.staMac + "|" + hs.ssid;
        if (existingKey == key) {
            hs.lastUpdateMs = now;

            if (hs.complete && (hs.frameMask & bit)) {
                _applyCounterDelta("handshake_duplicate_drop",
                                   0,
                                   0,
                                   1,
                                   0,
                                   STORAGE_LANE_MISSION,
                                   STORAGE_PRIO_P1);
                if (deferUiRefresh) {
                    _queueStorageUiRefresh(true);
                }
                return false;
            }

            if (hs.frameMask & bit) {
                _applyCounterDelta("handshake_suppressed",
                                   0,
                                   0,
                                   0,
                                   1,
                                   STORAGE_LANE_MISSION,
                                   STORAGE_PRIO_P1);
                if (deferUiRefresh) {
                    _queueStorageUiRefresh(true);
                }
                return false;
            }

            hs.frameMask |= bit;
            hs.complete = ((hs.frameMask & 0x0F) == 0x0F);
            return true;
        }
    }

    HandshakeProgress hs;
    hs.apMac = apMac ? apMac : "";
    hs.staMac = staMac ? staMac : "";
    hs.ssid = ssid ? ssid : "";
    hs.frameMask = bit;
    hs.lastUpdateMs = now;
    hs.complete = (bit == 0x0F);
    _handshakeWindow.push_back(hs);
    return true;
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

    JsonDocument payloadDoc;
    const size_t payloadLen = slot.payloadLen
        ? static_cast<size_t>(slot.payloadLen)
        : strnlen(slot.payload, RAMSpool::MAX_PAYLOAD);
    if (payloadLen == 0) {
        if (timing) timing->totalMs = millis() - totalStartMs;
        return {APPEND_FAILED_PARSE, 0};
    }

    const uint32_t parseStartMs = timing ? millis() : 0U;
    DeserializationError err = deserializeJson(payloadDoc, slot.payload, payloadLen);
    if (timing) {
        timing->parseMs += millis() - parseStartMs;
    }
    if (err || !payloadDoc.is<JsonObject>()) {
        DLOG_WARN("STORAGE", "appendQueuedRecord payload parse error type=%s",
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
    if (_uploadIndexResident &&
        _getUploadEventBatchForSessionFromIndex(sessionId, sinceId, maxCount, out)) {
        return true;
    }

    if (_uploadBatchActive) {
        DLOG_WARN("STORAGE",
                  "Upload index unavailable during active upload; spool scan fallback disabled session=%s since=%lu",
                  sessionId.c_str(),
                  static_cast<unsigned long>(sinceId));
        return false;
    }

    DLOG_WARN("STORAGE",
              "Upload index unavailable; falling back to spool scan session=%s since=%lu",
              sessionId.c_str(),
              static_cast<unsigned long>(sinceId));
    return _getEventBatchForSessionFromSpool(sessionId, sinceId, maxCount, out);
}

bool StorageManager::prepareUploadIndexForUpload(uint32_t budgetMs) {
    if (!_ready) {
        DLOG_WARN("STORAGE",
                  "Upload index prepare rebuilt=0 reason=not_ready fallback=1");
        return false;
    }

    if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY) {
        DLOG_INFO("STORAGE",
                  "Upload index prepare deferred owner=%s reason=capture",
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
        return false;
    }

    if (budgetMs < kUploadIndexRebuildMinBudgetMs) {
        DLOG_INFO("STORAGE",
                  "Upload index prepare rebuilt=0 reason=budget_too_small fallback=1");
        _releaseUploadIndexMemory("prepare_budget_too_small");
        return false;
    }

    const uint32_t pending = getPendingEventCount();
    const uint32_t estimatedIndexBytes =
        pending * static_cast<uint32_t>(sizeof(UploadIndexRecordV1));
    const uint32_t largestAny =
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const uint32_t freeAny =
        heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const uint32_t largestInternal =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t maxIndexAllocBytes =
        static_cast<uint32_t>(sizeof(UploadIndexPage)) + 4096UL;
    const uint32_t guardBytes = estimatedIndexBytes + 32768UL;
    if (pending > 0 &&
        (largestAny < maxIndexAllocBytes || freeAny < (guardBytes * 2U))) {
        DLOG_WARN("STORAGE",
                  "Upload index prepare rebuilt=0 reason=heap_guard pending=%lu estimate=%lu maxAlloc=%lu largest=%lu free=%lu internalLargest=%lu fallback=1",
                  static_cast<unsigned long>(pending),
                  static_cast<unsigned long>(estimatedIndexBytes),
                  static_cast<unsigned long>(maxIndexAllocBytes),
                  static_cast<unsigned long>(largestAny),
                  static_cast<unsigned long>(freeAny),
                  static_cast<unsigned long>(largestInternal));
        _releaseUploadIndexMemory("prepare_heap_guard");
        return false;
    }

    const uint32_t t0 = millis();
    const bool ok = _rebuildUploadIndex();
    const uint32_t dt = millis() - t0;
    if (ok) {
        DLOG_INFO("STORAGE",
                  "Upload index prepare rebuilt=1 indexed=%lu sessions=%lu ms=%lu fallback=0",
                  static_cast<unsigned long>(_uploadIndexStats.indexedEvents),
                  static_cast<unsigned long>(_uploadIndexStats.sessions),
                  static_cast<unsigned long>(dt));
        return true;
    }

    DLOG_WARN("STORAGE",
              "Upload index prepare rebuilt=0 reason=rebuild_failed fallback=1");
    _releaseUploadIndexMemory("prepare_rebuild_failed");
    return false;
}

StorageLaneCounts StorageManager::getPendingUploadCounts(const char* sessionIdOverride) {
    StorageLaneCounts counts;
    if (!_ready) return counts;

    const bool filterBySession = sessionIdOverride && sessionIdOverride[0];
    if (filterBySession) {
        DLOG_WARN("STORAGE",
                  "Session pending upload lane count requested outside session summary; maintenance queued");
        requestSpoolRepair("pending_upload_session_count_request");
        return counts;
    }

    for (const auto& seg : _spoolIndex.segments) {
        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;

        if (!summaryReady) {
            requestSpoolRepair("pending_upload_summary_stale");
            continue;
        }

        counts.mission += seg.pendingUploadMissionCount;
        counts.noise += seg.pendingUploadNoiseCount;
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

    std::map<uint32_t, bool> enrichedById;
    if (!_loadSpoolEnrichmentIds(sessionId, filterBySession, enrichedById)) {
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

                if (enrichedById.find(rec.eventId) != enrichedById.end()) {
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
            return {};
        }
    }

    return counts;
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

bool StorageManager::_getPendingEnrichmentBatch(const String& sessionId,
                                                bool filterBySession,
                                                PendingEventDescriptor* out,
                                                size_t maxCount,
                                                size_t& outCount,
                                                const uint32_t* excludeIds,
                                                size_t excludeCount) {
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

    std::map<uint32_t, bool> enrichedById;
    if (!_loadSpoolEnrichmentIds(sessionId, filterBySession, enrichedById)) {
        return false;
    }

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

    auto insertCandidate = [&](const PendingEventDescriptor& candidate) {
        size_t insertAt = outCount;
        while (insertAt > 0 && betterPending(candidate, out[insertAt - 1])) {
            --insertAt;
        }

        const size_t limit = std::min(maxCount, outCount + 1);
        for (size_t i = limit - 1; i > insertAt; --i) {
            out[i] = out[i - 1];
        }
        out[insertAt] = candidate;
        if (outCount < maxCount) {
            outCount++;
        }
    };

    for (const auto& seg : _spoolIndex.segments) {
        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;

        if (summaryReady && seg.eventCount == 0) {
            continue;
        }

        // Live pending-enrichment counter: drained segments cannot host
        // any candidate, so skip the record scan entirely.
        if (summaryReady && seg.pendingEnrichmentCount == 0) {
            continue;
        }

        const bool ok = _scanSegmentRecords(seg.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
                    return true;
                }

                if (!rec.sessionId.length() || rec.eventId == 0) {
                    return true;
                }

                if (filterBySession && rec.sessionId != sessionId) {
                    return true;
                }

                if (enrichedById.find(rec.eventId) != enrichedById.end()) {
                    return true;
                }

                if (excludeIds && excludeCount > 0) {
                    for (size_t ej = 0; ej < excludeCount; ++ej) {
                        if (excludeIds[ej] == rec.eventId) {
                            return true;
                        }
                    }
                }

                PendingEventDescriptor candidate;
                const JsonObjectConst doc = rec.doc.as<JsonObjectConst>();
                if (!_eventRecordPendingEnrichment(doc)) {
                    return true;
                }

                const uint32_t watermark = _uploadedWatermarkForSession(rec.sessionId);
                candidate.eventId = rec.eventId;
                candidate.timestampMs = doc["ts"] | 0U;
                candidate.type = eventTypeCode(doc["type"] | "");
                candidate.status =
                    (rec.eventId <= watermark) ? EVT_UPLOADED : EVT_RAW;
                candidate.lane = static_cast<uint8_t>(_eventRecordLane(doc));
                candidate.priority = static_cast<uint8_t>(_eventRecordPriority(doc));
                candidate.valueScore = _eventRecordValueScore(doc);

                if (outCount < maxCount || betterPending(candidate, out[outCount - 1])) {
                    insertCandidate(candidate);
                }
                return true;
            });

        if (!ok) {
            return false;
        }
    }

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

        _setUploadedWatermarkForSession(sessionId, eventId);

        // During an upload batch, keep flash quiet — radio is active. The flush
        // happens in endUploadBatch() after RADIO_ARB releases the lease, to
        // avoid the LittleFS-erase-during-active-radio brownout.
        // The batch flush is the durable boundary; outside batch mode we defer
        // the counter update until the watermark commit succeeds.
        if (_uploadBatchActive) {
            _applyCounterDelta("mark_uploaded_batch",
                               -1,
                               0,
                               0,
                               0,
                               static_cast<StorageLane>(laneHint == static_cast<uint8_t>(STORAGE_LANE_MISSION)
                                                             ? STORAGE_LANE_MISSION
                                                             : STORAGE_LANE_NOISE),
                               STORAGE_PRIO_P3);
            _decrementPendingUploadForEvent(eventId, laneHint, "mark_uploaded_batch");
            _uploadBatchDirty = true;
            return true;
        }

        CONTRACT_WARN_ONCE(CONTRACT_STORAGE_MARK_OUTSIDE_BATCH,
                           "STORAGE",
                           !RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD),
                           "event=%lu marked while upload owner active without batch",
                           static_cast<unsigned long>(eventId));

        const uint32_t singleIndexWriteStart = millis();
        if (!_persistSpoolIndex(false, "upload_mark_single")) {
            _setUploadedWatermarkForSession(sessionId, before);
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

        _applyCounterDelta("mark_uploaded_single",
                           -1,
                           0,
                           0,
                           0,
                           static_cast<StorageLane>(laneHint == static_cast<uint8_t>(STORAGE_LANE_MISSION)
                                                         ? STORAGE_LANE_MISSION
                                                         : STORAGE_LANE_NOISE),
                           STORAGE_PRIO_P3);
        _decrementPendingUploadForEvent(eventId, laneHint, "mark_uploaded_single");
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
        if (_uploadIndexResident) {
            auto it = _uploadIndexBySession.find(sessionId);
            if (it == _uploadIndexBySession.end()) {
                DLOG_WARN("STORAGE",
                          "markEventsUploaded exact path missing upload index session=%s upTo=%lu",
                          sessionId.c_str(),
                          static_cast<unsigned long>(upToId));
                requestSpoolRepair("bulk_mark_upload_index_absent");
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
                _setUploadedWatermarkForSession(sessionId, before);
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

        if (!_uploadIndexResident) {
            DLOG_WARN("STORAGE",
                      "markEventsUploaded bulk without resident upload index; "
                      "live counter may drift; explicit recount required "
                      "session=%s upTo=%lu",
                      sessionId.c_str(),
                      static_cast<unsigned long>(upToId));
            _pendingCountDirty = true;
            requestSpoolRepair("bulk_mark_inexact");
        }

        // Defer flash writes while an upload batch is active so brownout-prone
        // "w" opens stay out of the active-radio window.  No rescan needed —
        // the live counter already reflects the bulk decrement (or has been
        // explicitly flagged for repair when the index was not resident).
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
    CONTRACT_WARN_ONCE(CONTRACT_STORAGE_BATCH_NESTING,
                       "STORAGE",
                       !_uploadBatchActive,
                       "beginUploadBatch nested while active=%d dirty=%d",
                       _uploadBatchActive ? 1 : 0,
                       _uploadBatchDirty ? 1 : 0);
    flushWorkerMetadataBatch("upload_start", true);
    _uploadBatchActive = true;
    _uploadBatchDirty  = false;
    DLOG_INFO("STORAGE", "Upload batch open — deferring watermark flush");
}

bool StorageManager::endUploadBatch() {
    if (!_ready) {
        _uploadBatchActive = false;
        _uploadBatchDirty  = false;
        _releaseUploadIndexMemory("upload_batch_end_not_ready");
        return false;
    }

    CONTRACT_WARN_ONCE(CONTRACT_UPLOAD_BATCH_OWNER_SYNC,
                       "STORAGE",
                       !_uploadBatchActive || !RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD),
                       "upload batch closing while radio owner still=%s",
                       RadioArbiter::ownerName(RADIO_ARB.currentOwner()));

    const bool wasActive = _uploadBatchActive;
    const bool wasDirty  = _uploadBatchDirty;
    _uploadBatchActive = false;
    _uploadBatchDirty  = false;

    const bool hasSidecarWork = _workerMetadataDirtyPending || _storageUiRefreshPending;
    if (!wasActive || (!wasDirty && !hasSidecarWork)) {
        _releaseUploadIndexMemory("upload_batch_end_clean");
        return true;
    }

    flushWorkerMetadataBatch("upload_end", true);
    const uint32_t endIndexWriteStart = millis();
    bool ok = _persistSpoolIndex(true, "upload_end");
    if (!ok) {
        DLOG_WARN("STORAGE", "Upload batch flush: spool index persist failed");
    }
    _logCaptureWriteAllowed(_spoolIndexPath().c_str(),
                            "upload_end",
                            millis() - endIndexWriteStart,
                            ok);

    // Live counter is authoritative — every markEventUploaded already
    // decremented it.  No rescan on the hot path; if the caller used the
    // bulk path without a resident upload index it set _pendingCountDirty
    // and must invoke recountPendingFromSpool() explicitly.
    {
        const uint32_t endMetaWriteStart = millis();
        _persistEventMeta(true, "upload_end");
        _logCaptureWriteAllowed(PATH_EVENT_META,
                                "upload_end",
                                millis() - endMetaWriteStart,
                                true);
    }
    refreshStorageUiState();
    _logSpoolDiagnostics("upload_batch_flush");
    _checkSpoolInvariants("upload_batch_end", false);
    _releaseUploadIndexMemory("upload_batch_end");
    return ok;
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
    _pendingCountDirty = false;
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
    _servicePendingSpoolSummaryRebuild();
    _pendingEventCount = _rescanPendingEventCountFromSpool();
    _spoolIndex.pendingTotal = _pendingEventCount;
    _pendingCountDirty = false;
    _spoolAuditRepairRequired = false;
    _spoolSummaryRebuildPending = false;
    _setCounterTrustState(STORAGE_COUNTER_TRUSTED, "recount_pending");
    if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY) {
        _workerMetadataDirtyPending = true;
        _pendingCountDirty = true;
        DLOG_INFO("STORAGE",
                  "Pending recount meta persist deferred owner=%s reason=capture",
                  RadioArbiter::ownerName(RADIO_ARB.currentOwner()));
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

bool StorageManager::_scanSegmentRecords(
    uint32_t segmentId,
    std::function<bool(const DecodedSpoolRecord&)> cb) const {

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

    while (f.available()) {
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
    String lastSession;

    while (f.position() < f.size()) {
        SpoolBin::RecordPrefix prefix;
        if (!SpoolBin::readBytes(f, &prefix, sizeof(prefix))) {
            DLOG_WARN("STORAGE", "Binary spool prefix read failed seg=%lu",
                      static_cast<unsigned long>(segmentId));
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

        if (prefix.type == SpoolBin::REC_ENRICH_DELTA) {
            uint32_t recordId = 0;
            uint32_t tsDelta = 0;
            uint8_t sessionMode = BIN_SESSION_INLINE;
            uint8_t enrichFlags = 0;
            String sessionId;
            uint32_t targetEventId = 0;
            int32_t latE7 = 0;
            int32_t lonE7 = 0;
            int32_t altCm = 0;
            uint32_t accDm = 0;
            String tag;

            if (!_readUVarintFromBytes(p, end, recordId) ||
                !_readUVarintFromBytes(p, end, tsDelta)) {
                DLOG_WARN("STORAGE", "Binary enrich decode failed seg=%lu",
                          static_cast<unsigned long>(segmentId));
                f.close();
                return false;
            }

            if (p >= end) {
                f.close();
                return false;
            }
            sessionMode = *p++;

            if (sessionMode == BIN_SESSION_SAME_AS_PREV) {
                sessionId = lastSession;
            } else {
                if (!_readStringFromBytes(p, end, sessionId)) {
                    f.close();
                    return false;
                }
                lastSession = sessionId;
            }

            if (p >= end) {
                f.close();
                return false;
            }
            enrichFlags = *p++;

            if (!_readUVarintFromBytes(p, end, targetEventId) ||
                !_readZigZag32FromBytes(p, end, latE7) ||
                !_readZigZag32FromBytes(p, end, lonE7) ||
                !_readZigZag32FromBytes(p, end, altCm) ||
                !_readUVarintFromBytes(p, end, accDm)) {
                DLOG_WARN("STORAGE", "Binary enrich decode failed seg=%lu",
                          static_cast<unsigned long>(segmentId));
                f.close();
                return false;
            }

            if (enrichFlags & 0x01) {
                if (!_readStringFromBytes(p, end, tag)) {
                    f.close();
                    return false;
                }
            }

            root["id"] = recordId;
            const uint32_t ts = _timestampFromBaseDelta(tsDelta, tsBase);
            root["ts"] = ts;
            root["type"] = "enrich_delta";
            root[F_SESSION] = sessionId;
            root["event_id"] = targetEventId;
            root["lat"] = _e7ToFloat(latE7);
            root["lon"] = _e7ToFloat(lonE7);
            root["alt"] = _cmToFloat(altCm);
            root["acc"] = _dmToFloat(accDm);
            if (enrichFlags & 0x01) root["tag"] = tag;
            {
                char tsIso[24] = {};
                TIME_SVC.formatIsoForMillis(ts, tsIso, sizeof(tsIso));
                root[F_TIMESTAMP_ISO] = tsIso;
            }

            rec.recordType = SPOOL_REC_ENRICH_DELTA;
            rec.eventId = recordId;
            rec.sessionId = sessionId;
            rec.doc.set(doc.as<JsonVariantConst>());

        } else {
            uint32_t recordId = 0;
            uint32_t tsDelta = 0;
            uint8_t sessionMode = BIN_SESSION_INLINE;
            String sessionId;
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

            if (sessionMode == BIN_SESSION_SAME_AS_PREV) {
                sessionId = lastSession;
            } else {
                if (!_readStringFromBytes(p, end, sessionId)) {
                    f.close();
                    return false;
                }
                lastSession = sessionId;
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
                char tsIso[24] = {};
                TIME_SVC.formatIsoForMillis(ts, tsIso, sizeof(tsIso));
                root[F_TIMESTAMP_ISO] = tsIso;
            }

            if (payloadFamily == BIN_PAYLOAD_PROBE_DEVICE) {
                String mac;
                String ssid;
                String ieFingerprint;
                String probeSetHash;
                int32_t rssi = 0;
                uint32_t channel = 0;
                bool isBroadcast = false;
                bool isRandomMac = false;

                if (p >= end) {
                    f.close();
                    return false;
                }
                const uint8_t flags = *p++;

                uint8_t lastOui[3] = {0, 0, 0};
                bool hasLastOui = false;

                if (!_readMacFieldFromBytes(p, end, mac, lastOui, hasLastOui)) {
                    DLOG_WARN("STORAGE", "Binary probe/device MAC decode failed seg=%lu",
                              static_cast<unsigned long>(segmentId));
                    f.close();
                    return false;
                }

                if (flags & 0x01) {
                    if (!_readStringFromBytes(p, end, ssid)) {
                        f.close();
                        return false;
                    }
                }

                if (flags & 0x02) {
                    if (!_readZigZag32FromBytes(p, end, rssi)) {
                        f.close();
                        return false;
                    }
                }

                root["mac"] = mac;
                if (flags & 0x01) {
                    if (typeStr == "probe") root["probed_ssid"] = ssid;
                    else root["ssid"] = ssid;
                }
                if (flags & 0x02) root["rssi"] = rssi;
                if (flags & 0x04) {
                    if (!_readUVarintFromBytes(p, end, channel)) {
                        f.close();
                        return false;
                    }
                    root["channel"] = channel;
                }
                if (flags & 0x08) {
                    if (!_readStringFromBytes(p, end, ieFingerprint)) {
                        f.close();
                        return false;
                    }
                    root["ie_fingerprint"] = ieFingerprint;
                }
                if (flags & 0x10) {
                    if (!_readStringFromBytes(p, end, probeSetHash)) {
                        f.close();
                        return false;
                    }
                    root["probe_set_hash"] = probeSetHash;
                }
                if (flags & 0x20) {
                    if (p >= end) {
                        f.close();
                        return false;
                    }
                    isRandomMac = (*p++ != 0);
                    root["is_random_mac"] = isRandomMac ? 1 : 0;
                }
                if (flags & 0x40) {
                    if (p >= end) {
                        f.close();
                        return false;
                    }
                    isBroadcast = (*p++ != 0);
                    root["is_broadcast"] = isBroadcast ? 1 : 0;
                }

            } else if (payloadFamily == BIN_PAYLOAD_PMKID) {
                String ap;
                String sta;
                String ssid;
                String pmkidHex;
                String hashcatLine;
                int32_t rssi = 0;

                if (p >= end) {
                    f.close();
                    return false;
                }
                const uint8_t flags = *p++;

                uint8_t lastOui[3] = {0, 0, 0};
                bool hasLastOui = false;

                if (!_readMacFieldFromBytes(p, end, ap, lastOui, hasLastOui) ||
                    !_readMacFieldFromBytes(p, end, sta, lastOui, hasLastOui)) {
                    DLOG_WARN("STORAGE", "Binary pmkid MAC decode failed seg=%lu",
                              static_cast<unsigned long>(segmentId));
                    f.close();
                    return false;
                }

                if (flags & 0x01) {
                    if (!_readStringFromBytes(p, end, ssid)) {
                        f.close();
                        return false;
                    }
                }

                root["ap"] = ap;
                root["sta"] = sta;
                root["bssid"] = ap;
                root["client_mac"] = sta;
                if (flags & 0x01) root["ssid"] = ssid;

                if (flags & 0x02) {
                    if (!_readZigZag32FromBytes(p, end, rssi)) {
                        f.close();
                        return false;
                    }
                    root["rssi"] = rssi;
                }
                if (flags & 0x04) {
                    if (!_readStringFromBytes(p, end, pmkidHex)) {
                        f.close();
                        return false;
                    }
                    root["pmkid_hex"] = pmkidHex;
                }
                if (flags & 0x08) {
                    if (!_readStringFromBytes(p, end, hashcatLine)) {
                        f.close();
                        return false;
                    }
                    root["hashcat_line"] = hashcatLine;
                }

            } else if (payloadFamily == BIN_PAYLOAD_HANDSHAKE) {
                String ap;
                String sta;
                String ssid;
                uint32_t frameMask = 0;
                uint32_t messageNumber = 0;

                if (p >= end) {
                    f.close();
                    return false;
                }
                const uint8_t flags = *p++;

                uint8_t lastOui[3] = {0, 0, 0};
                bool hasLastOui = false;

                if (!_readMacFieldFromBytes(p, end, ap, lastOui, hasLastOui) ||
                    !_readMacFieldFromBytes(p, end, sta, lastOui, hasLastOui)) {
                    DLOG_WARN("STORAGE", "Binary handshake MAC decode failed seg=%lu",
                              static_cast<unsigned long>(segmentId));
                    f.close();
                    return false;
                }

                if (flags & 0x01) {
                    if (!_readStringFromBytes(p, end, ssid)) {
                        f.close();
                        return false;
                    }
                }

                if (!_readUVarintFromBytes(p, end, frameMask)) {
                    f.close();
                    return false;
                }

                root["ap"] = ap;
                root["sta"] = sta;
                root["bssid"] = ap;
                root["client"] = sta;
                if (flags & 0x01) root["ssid"] = ssid;
                root["frame_mask"] = frameMask;
                root["event_type"] = "handshake";

                if (flags & 0x02) {
                    int32_t rssi = 0;
                    if (!_readZigZag32FromBytes(p, end, rssi)) {
                        f.close();
                        return false;
                    }
                    root["rssi"] = rssi;
                }
                if (flags & 0x04) {
                    if (!_readUVarintFromBytes(p, end, messageNumber)) {
                        f.close();
                        return false;
                    }
                    root["message"] = messageNumber;
                    root["msg"] = messageNumber;
                }

            } else if (payloadFamily == BIN_PAYLOAD_DRONE) {
                String droneId;
                String mac;
                String protocol;
                int32_t latitudeE7 = 0;
                int32_t longitudeE7 = 0;
                uint32_t channel = 0;
                uint8_t lastOui[3] = {0, 0, 0};
                bool hasLastOui = false;

                if (p >= end) {
                    f.close();
                    return false;
                }
                const uint8_t flags = *p++;

                if (flags & 0x01) {
                    if (!_readStringFromBytes(p, end, droneId)) {
                        f.close();
                        return false;
                    }
                }
                if (flags & 0x02) {
                    if (!_readMacFieldFromBytes(p, end, mac, lastOui, hasLastOui)) {
                        f.close();
                        return false;
                    }
                }
                if (flags & 0x04) {
                    int32_t rssi = 0;
                    if (!_readZigZag32FromBytes(p, end, rssi)) {
                        f.close();
                        return false;
                    }
                    root["rssi"] = rssi;
                }
                if (flags & 0x08) {
                    if (!_readUVarintFromBytes(p, end, channel)) {
                        f.close();
                        return false;
                    }
                    root["channel"] = channel;
                }
                if (flags & 0x10) {
                    if (!_readStringFromBytes(p, end, protocol)) {
                        f.close();
                        return false;
                    }
                    root["protocol"] = protocol;
                }
                if (flags & 0x20) {
                    if (!_readZigZag32FromBytes(p, end, latitudeE7) ||
                        !_readZigZag32FromBytes(p, end, longitudeE7)) {
                        f.close();
                        return false;
                    }
                    root["latitude"] = _e7ToFloat(latitudeE7);
                    root["longitude"] = _e7ToFloat(longitudeE7);
                }
                if (flags & 0x40) {
                    uint32_t altitudeCenti = 0;
                    if (!_readUVarintFromBytes(p, end, altitudeCenti)) {
                        f.close();
                        return false;
                    }
                    root["altitude_m"] = _centiToFloat(altitudeCenti);
                }
                if (flags & 0x80) {
                    uint32_t speedCenti = 0;
                    if (!_readUVarintFromBytes(p, end, speedCenti)) {
                        f.close();
                        return false;
                    }
                    root["speed"] = _centiToFloat(speedCenti);
                }

                if (flags & 0x01) root["drone_id"] = droneId;
                if (flags & 0x02) root["mac"] = mac;

            } else if (payloadFamily == BIN_PAYLOAD_FIELD_MAP) {
                if (!_readBinaryFieldMapFromBytes(p, end, root)) {
                    DLOG_WARN("STORAGE", "Binary event field map decode failed seg=%lu",
                              static_cast<unsigned long>(segmentId));
                    f.close();
                    return false;
                }

            } else if (payloadFamily == BIN_PAYLOAD_JSON_FALLBACK) {
                // Legacy fallback payload still supported for readback only.
                if (eventFlags & BIN_EVENT_HAS_PAYLOAD) {
                    String payloadJson;
                    if (!_readStringFromBytes(p, end, payloadJson)) {
                        DLOG_WARN("STORAGE", "Binary event payload decode failed seg=%lu",
                                  static_cast<unsigned long>(segmentId));
                        f.close();
                        return false;
                    }

                    if (payloadJson.length()) {
                        JsonDocument payloadDoc;
                        DeserializationError err = deserializeJson(payloadDoc, payloadJson);
                        if (err || !payloadDoc.is<JsonObject>()) {
                            DLOG_WARN("STORAGE", "Binary event payload JSON failed seg=%lu",
                                      static_cast<unsigned long>(segmentId));
                            f.close();
                            return false;
                        }

                        for (JsonPairConst kv : payloadDoc.as<JsonObjectConst>()) {
                            root[kv.key().c_str()].set(kv.value());
                        }
                    }
                }

            } else {
                DLOG_WARN("STORAGE", "Unknown payload family seg=%lu family=%u",
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

            const String segPath = _spoolBinarySegmentPath(seg.segmentId);
            File segFile = LittleFS.open(segPath, "r+");
            if (!segFile) return false;

            SpoolBin::SegmentHeaderV2 segHdr;
            if (!SpoolBin::readSegmentHeaderV2(segFile, segHdr)) {
                segFile.close();
                return false;
            }

            if (segHdr.magic != SpoolBin::SEGMENT_MAGIC || segHdr.version != 2) {
                segFile.close();
                return false;
            }

            const uint32_t tsDelta = _timestampDeltaFromBase(ts, segHdr.createdMs);
            const String sessionId = String((const char*)(doc[F_SESSION] | doc["session_id"] | ""));
            const String typeStr = String((const char*)(doc["type"] | ""));
            const String eventSubtype = String((const char*)(doc["event_type"] | ""));

            std::vector<uint8_t> body;
            body.reserve(96);

            _appendUVarintToBytes(body, recordId);
            _appendUVarintToBytes(body, tsDelta);

            const String lastSession = _binaryLastSessionBySegment[seg.segmentId];
            const bool sameSession = (sessionId.length() && sessionId == lastSession);
            body.push_back(sameSession ? BIN_SESSION_SAME_AS_PREV : BIN_SESSION_INLINE);
            if (!sameSession) _appendStringToBytes(body, sessionId);

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
                (typeCode == BIN_EVT_PROBE || typeCode == BIN_EVT_DEVICE);
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
                body.push_back(BIN_PAYLOAD_PROBE_DEVICE);
                _recordBinaryStructuredWrite();

                const String mac = String((const char*)(doc["mac"] | ""));
                const String ssid = (typeStr == "probe")
                    ? String((const char*)(doc["probed_ssid"] | doc["ssid"] | ""))
                    : String((const char*)(doc["ssid"] | ""));
                const String ieFingerprint = String((const char*)(doc["ie_fingerprint"] | ""));
                const String probeSetHash = String((const char*)(doc["probe_set_hash"] | ""));
                const int32_t rssi = static_cast<int32_t>(doc["rssi"] | 0);
                const uint32_t channel = doc["channel"] | 0U;
                const bool isRandomMac = (doc["is_random_mac"] | 0) != 0;
                const bool isBroadcast = (doc["is_broadcast"] | 0) != 0;

                uint8_t flags = 0;
                if (ssid.length()) flags |= 0x01;
                if (doc["rssi"].is<int>() || doc["rssi"].is<long>() || doc["rssi"].is<float>())
                    flags |= 0x02;
                if (!doc["channel"].isNull()) flags |= 0x04;
                if (ieFingerprint.length()) flags |= 0x08;
                if (probeSetHash.length()) flags |= 0x10;
                if (!doc["is_random_mac"].isNull()) flags |= 0x20;
                if (!doc["is_broadcast"].isNull()) flags |= 0x40;
                body.push_back(flags);

                uint8_t lastOui[3] = {0};
                bool hasLastOui = false;

                _appendMacFieldToBytes(body, mac, lastOui, hasLastOui);
                if (flags & 0x01) _appendStringToBytes(body, ssid);
                if (flags & 0x02) _appendZigZag32ToBytes(body, rssi);
                if (flags & 0x04) _appendUVarintToBytes(body, channel);
                if (flags & 0x08) _appendStringToBytes(body, ieFingerprint);
                if (flags & 0x10) _appendStringToBytes(body, probeSetHash);
                if (flags & 0x20) body.push_back(isRandomMac ? 1U : 0U);
                if (flags & 0x40) body.push_back(isBroadcast ? 1U : 0U);

            } else if (isPmkid) {
                body.push_back(BIN_PAYLOAD_PMKID);
                _recordBinaryStructuredWrite();

                const String ap = String((const char*)(doc["ap"] | doc["bssid"] | ""));
                const String sta = String((const char*)(doc["sta"] | doc["client"] | doc["client_mac"] | ""));
                const String ssid = String((const char*)(doc["ssid"] | ""));
                const int32_t rssi = static_cast<int32_t>(doc["rssi"] | 0);
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
                const int32_t rssi = static_cast<int32_t>(doc["rssi"] | 0);
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
                const int32_t rssi = static_cast<int32_t>(doc["rssi"] | 0);
                const uint32_t channel = doc["channel"] | 0U;
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
                    return false;
                }
                hasExtensionFields = false;
            }

            if (hasExtensionFields) {
                eventFlags |= BIN_EVENT_HAS_FIELDS;
                if (!_appendBinaryFieldMapToBytes(body,
                                                  doc.as<JsonObjectConst>(),
                                                  typeStr,
                                                  eventSubtype)) {
                    return false;
                }
            }

            body[eventFlagsOffset] = eventFlags;

            if (eventFlags & BIN_EVENT_HAS_PRIO) body.push_back(prio);
            if (eventFlags & BIN_EVENT_HAS_LANE) body.push_back(lane);

            if (body.size() <= eventFlagsOffset + 1) return false;

            SpoolBin::SegmentHeaderV2 hdr;
            SpoolBin::AppendRecordLocation loc{};
            SpoolBin::AppendRecordLocation* locOut = outLoc ? &loc : nullptr;
            const uint32_t writeOffset = static_cast<uint32_t>(segFile.size());
            if (!segFile.seek(writeOffset)) {
                segFile.close();
                return false;
            }
            const uint32_t appendStartMs = millis();
            const bool appendOk = SpoolBin::appendRecordToOpen(
                    segFile,
                    static_cast<uint8_t>(SpoolBin::REC_EVENT),
                    body.data(),
                    static_cast<uint16_t>(body.size()),
                    recordId,
                    segHdr);
            if (appendOk) {
                hdr = segHdr;
                if (!SpoolBin::writeSegmentHeaderV2(segFile, hdr)) {
                    segFile.close();
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
                segFile.close();
                return false;
            }

            if (locOut) {
                locOut->offset = writeOffset;
                locOut->len = static_cast<uint32_t>(sizeof(SpoolBin::RecordPrefix)) +
                              static_cast<uint32_t>(body.size());
            }
            segFile.close();

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
    switch (_counterTrustState) {
        case CounterTrust::TrustedSnapshotLagged: return "trusted_snapshot_lagged";
        case CounterTrust::Degraded:              return "degraded";
        case CounterTrust::RepairRequired:        return "repair_required";
        case CounterTrust::EmergencyOnly:         return "emergency_only";
        case CounterTrust::Trusted:
        default:                                  return "trusted";
    }
}

void StorageManager::_setCounterTrustState(StorageCounterTrustState state,
                                           const char* reason) {
    if (_counterTrustState == state) {
        return;
    }

    _counterTrustState = state;
    _counterTrustSinceMs = millis();
    if (reason && reason[0]) {
        _counterTrustReason = reason;
    } else {
        _counterTrustReason = "";
    }
    DLOG_WARN("STORAGE",
              "Counter trust=%s reason=%s since=%lu",
              _counterTrustText(),
              _counterTrustReason.length() ? _counterTrustReason.c_str() : "-",
              static_cast<unsigned long>(_counterTrustSinceMs));
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

    // Idempotency contract: this helper must NOT double-decrement when a
    // duplicate delta lands for an already-enriched event.  The runtime
    // does not maintain an in-memory enrichedSet (memory cost rejected at
    // 2k+ events scale), so we cannot cheaply prove first-application.
    // Per the design, we therefore CLAMP and mark recount-required:
    //   * summary stale  → can't reason about the counter; flag recount.
    //   * counter == 0   → either a duplicate delta or pre-existing drift;
    //                      do NOT underflow, flag recount.
    //   * counter  > 0   → decrement once and trust the live count until
    //                      the next explicit recountPendingFromSpool().
    // Cross-segment audit/rebuild paths dedupe per eventId in their own
    // local set, so they never reach this helper for a duplicate.
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
            // Either a duplicate delta against a fully-drained segment
            // or pre-existing drift.  Either way, do not underflow; let
            // the next explicit recount reconcile.
            _pendingCountDirty = true;
            _setCounterTrustState(STORAGE_COUNTER_DEGRADED,
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

void StorageManager::_decrementPendingUploadForEvent(uint32_t eventId,
                                                     uint8_t laneHint,
                                                     const char* reason) {
    if (eventId == 0) return;

    for (auto& seg : _spoolIndex.segments) {
        if (seg.firstEventId == 0 || seg.lastEventId == 0) continue;
        if (eventId < seg.firstEventId || eventId > seg.lastEventId) continue;

        if (!seg.summaryValid ||
            seg.summaryVersion != SPOOL_SEGMENT_SUMMARY_VERSION) {
            _pendingCountDirty = true;
            _spoolSummaryRebuildPending = true;
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                  reason ? reason : "pending_upload_summary_stale");
            return;
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

        if (counter && *counter > 0) {
            (*counter)--;
            return;
        }

        _pendingCountDirty = true;
        _setCounterTrustState(STORAGE_COUNTER_DEGRADED,
                              reason ? reason : "pending_upload_lane_unknown");
        return;
    }
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
            requestSpoolRepair("bulk_mark_session_mismatch");
            return false;
        }

        const SpoolSegmentInfo* seg = _findSegmentInfo(rec.segmentId);
        if (!seg) {
            requestSpoolRepair("bulk_mark_missing_segment");
            return false;
        }

        SegmentDelta& delta = deltas[rec.segmentId];
        if (rec.lane == static_cast<uint8_t>(STORAGE_LANE_MISSION)) {
            delta.mission++;
        } else if (rec.lane == static_cast<uint8_t>(STORAGE_LANE_NOISE)) {
            delta.noise++;
        } else {
            requestSpoolRepair("bulk_mark_lane_invalid");
            return false;
        }

        decrement++;
    }

    if (decrement == 0) {
        return true;
    }

    if (_pendingEventCount < decrement) {
        requestSpoolRepair("bulk_mark_counter_underflow");
        return false;
    }

    for (const auto& entry : deltas) {
        const SpoolSegmentInfo* seg = _findSegmentInfo(entry.first);
        if (!seg) {
            requestSpoolRepair("bulk_mark_missing_segment");
            return false;
        }

        if (seg->pendingUploadMissionCount < entry.second.mission ||
            seg->pendingUploadNoiseCount < entry.second.noise) {
            requestSpoolRepair("bulk_mark_lane_underflow");
            return false;
        }
    }

    _pendingEventCount -= decrement;
    _spoolIndex.pendingTotal = _pendingEventCount;

    for (const auto& entry : deltas) {
        SpoolSegmentInfo* seg = _findSegmentInfo(entry.first);
        if (!seg) {
            requestSpoolRepair("bulk_mark_missing_segment");
            return false;
        }

        seg->pendingUploadMissionCount -= entry.second.mission;
        seg->pendingUploadNoiseCount -= entry.second.noise;
    }

    _pendingCountDirty = false;
    _spoolAuditRepairRequired = false;
    _bumpStorageMetaGeneration();
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
    return true;
}

bool StorageManager::_rebuildInvalidSegmentSummaries(bool force) {
    uint32_t rebuiltCount = 0;
    bool changed = false;
    bool anyRebuilt = false;

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
    }

    // Per-segment rebuild produces gross pending-enrichment counts (every
    // event-that-wants-enrichment is counted).  Now apply cross-segment
    // delta decrements: each UNIQUE enriched eventId found anywhere
    // subtracts 1 from its originating segment's pendingEnrichmentCount.
    // The local set guarantees idempotency — duplicate deltas (e.g. from
    // a phone retry) decrement exactly once, matching the semantics of
    // _eventRecordPendingEnrichment.  This is the ONLY place we walk the
    // whole spool — explicit, costly, never on the hot path.
    if (anyRebuilt && !_hasInvalidSpoolSummaries()) {
        std::map<uint32_t, bool> seenEnrichedIds;
        for (const auto& seg : _spoolIndex.segments) {
            _scanSegmentRecords(seg.segmentId,
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
        }
    }

    _spoolSummaryRebuildPending = _hasInvalidSpoolSummaries();

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

    rebuilt = scanned;
    audit.scannedRecords += local.scannedRecords;
    audit.validEventRecords += local.validEventRecords;
    audit.validEnrichDeltas += local.validEnrichDeltas;
    audit.invalidRecords += local.invalidRecords;
    audit.skippedRecords += local.skippedRecords;
    audit.rebuiltPendingTotal += local.rebuiltPendingTotal;
    audit.maxEventIdSeen = std::max(audit.maxEventIdSeen, local.maxEventIdSeen);
    scanned.trustState = SPOOL_SEGMENT_TRUSTED;
    return true;
}

bool StorageManager::_auditAndRepairSpool(const char* reason,
                                         bool repair,
                                         SpoolAuditResult* out) {
    SpoolAuditResult audit;
    audit.oldPendingTotal = _spoolIndex.pendingTotal;
    audit.oldNextEventId = _nextEventId;
    const uint32_t _auditStartMs = millis();

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

    audit.hadMismatch =
        audit.oldPendingTotal != audit.rebuiltPendingTotal ||
        audit.oldNextEventId <= audit.maxEventIdSeen ||
        audit.quarantinedSegments > 0 ||
        audit.invalidRecords > 0 ||
        audit.hadFatalSegmentError ||
        segmentChanged ||
        sessionsChanged;

    if (repair && audit.hadMismatch) {
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
                std::max<uint32_t>(audit.maxEventIdSeen + 1U, 1U);
        if (_nextEventId <= audit.maxEventIdSeen) {
            _nextEventId = rebuiltNext;
            _spoolIndex.nextEventId = _nextEventId;
            _bumpStorageMetaGeneration();
            _eventCounterDirty = true;
            _eventCounterPendingWrites = 1;
        }

        _spoolIndex.segments = repairedSegments;
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

        for (auto it = _binaryLastSessionBySegment.begin();
             it != _binaryLastSessionBySegment.end();) {
            const bool keep =
                std::any_of(_spoolIndex.segments.begin(),
                            _spoolIndex.segments.end(),
                            [&](const SpoolSegmentInfo& seg) {
                                return seg.segmentId == it->first;
                            });
            if (!keep) {
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

        if (!_persistEventCounter(true, "spool_audit_repair") ||
            !_persistSpoolIndex(true, "spool_audit_repair") ||
            !_persistEventMeta(true, "spool_audit_repair")) {
            _setCounterTrustState(STORAGE_COUNTER_TRUSTED_SNAPSHOT_LAGGED,
                                  "spool_audit_persist_failed");
            if (out) {
                *out = audit;
            }
            return false;
        }

        audit.repaired = true;
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
                _repairJob.audit.rebuiltPendingTotal =
                    cp.pendingUploadMissionCount + cp.pendingUploadNoiseCount;
                _repairJob.audit.maxEventIdSeen = cp.lastEventId;
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
                                       uint16_t maxRecords,
                                       uint16_t& recordsScanned) {
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
                                            uint16_t maxRecords,
                                            uint16_t& recordsScanned) {
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

    const bool sessionsChanged = (job.rebuiltSessions != _spoolIndex.sessions);
    job.audit.hadMismatch =
        job.audit.oldPendingTotal != job.audit.rebuiltPendingTotal ||
        job.audit.oldNextEventId <= job.audit.maxEventIdSeen ||
        job.audit.quarantinedSegments > 0 ||
        job.audit.invalidRecords > 0 ||
        job.audit.hadFatalSegmentError ||
        job.segmentChanged ||
        sessionsChanged;

    bool persistOk = true;
    if (job.audit.hadMismatch) {
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
                : std::max<uint32_t>(job.audit.maxEventIdSeen + 1U, 1U);
        if (_nextEventId <= job.audit.maxEventIdSeen) {
            _nextEventId = rebuiltNext;
            _spoolIndex.nextEventId = _nextEventId;
            _bumpStorageMetaGeneration();
            _eventCounterDirty = true;
            _eventCounterPendingWrites = 1;
        }

        _spoolIndex.segments = job.repairedSegments;
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

        _spoolIndex.sessions = job.rebuiltSessions;
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

    RAMSpool::Stats spool = {};
    if (RAMSpool::isReady()) {
        spool = RAMSpool::snapshot();
    }

    const bool writerPressure =
        _workerAppendBatchActive ||
        _uploadBatchActive ||
        _uploadBatchDirty ||
        _workerMetadataDirtyPending ||
        _pendingCountDirty ||
        _spoolIndexDirty ||
        _spoolSummaryRebuildPending ||
        _repairRequested;
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

    return (owner == RADIO_NONE) ? REPAIR_FAST_IDLE : REPAIR_BACKGROUND;
}

void StorageManager::_repairBudgetsForMode(SpoolRepairMode mode,
                                           uint32_t& budgetMs,
                                           uint16_t& maxRecords) const {
    switch (mode) {
        case REPAIR_FAST_IDLE:
            budgetMs = 16UL;
            maxRecords = 32U;
            break;
        case REPAIR_EMERGENCY:
            budgetMs = 50UL;
            maxRecords = 128U;
            break;
        case REPAIR_BACKGROUND:
        default:
            budgetMs = 1UL;
            maxRecords = 1U;
            break;
    }

    if (budgetMs == 0) {
        budgetMs = 1UL;
    }
    if (maxRecords == 0) {
        maxRecords = 1U;
    }
}

bool StorageManager::_shouldDeferWorkerMetadataFlush() const {
    const RadioOwner owner = RADIO_ARB.currentOwner();
    if (owner == RADIO_WIFI_CAPTURE) {
        return _selectRepairMode() != REPAIR_EMERGENCY;
    }

    return hasSpoolRepairWork() &&
           _selectRepairMode() == REPAIR_BACKGROUND;
}

bool StorageManager::serviceStorageMaintenanceStep(uint32_t budgetMs,
                                                   uint16_t maxRecords) {
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
        _repairBudgetsForMode(mode, budgetMs, maxRecords);
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
                                uint16_t maxRecords) {
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
    uint16_t recordsScanned = 0;

    while (recordsScanned < maxRecords &&
           (millis() - startMs) < budgetMs) {
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
        }

        if (recordsScanned == 0) {
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

    return RADIO_ARB.currentOwner() == RADIO_NONE;
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

    if (usedPct >= STORAGE_OVERRUN_PCT) {
        _pressureMode = STORAGE_MODE_OVERRUN;
        _retentionPolicy = STORAGE_POLICY_CRITICAL_ONLY;
    } else if (usedPct >= STORAGE_FULL_PCT) {
        _pressureMode = STORAGE_MODE_FULL;
        _retentionPolicy = STORAGE_POLICY_REDUCED;
    } else if (usedPct >= STORAGE_WATCH_PCT) {
        _pressureMode = STORAGE_MODE_WATCH;
        _retentionPolicy = STORAGE_POLICY_NORMAL;
    } else {
        _pressureMode = STORAGE_MODE_NORMAL;
        _retentionPolicy = STORAGE_POLICY_NORMAL;
    }

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
    _cachedUsedString = String(_cachedUsedBytes / 1024) + "KB / " +
                        String(_cachedTotalBytes / 1024) + "KB";
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

StorageManager::StorageUiSnapshot StorageManager::_buildStorageUiSnapshot(
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

    STATE_WRITE_BEGIN();
    g_state.storageNearlyFull = nextSnapshot.nearlyFull;
    g_state.storageFull = nextSnapshot.full;
    g_state.storageOverrun = nextSnapshot.overrun;
    g_state.storageMode = nextSnapshot.mode;
    g_state.storagePolicy = nextSnapshot.policy;
    g_state.storageUsedPct = nextSnapshot.usedPct;
    g_state.storageFreeBytes = nextSnapshot.freeBytes;
    g_state.storagePending = nextSnapshot.pending;
    g_state.storageDeduped = _dedupStats.suppressed;
    g_state.storageDropped = _dedupStats.dropped;
    g_state.storageDumpAdvised = nextSnapshot.dumpAdvised;
    g_state.storageRepairRequired = nextSnapshot.repairRequired;
    g_state.storageCounterTrust = nextSnapshot.counterTrust;
    strlcpy(g_state.storagePolicyText, nextSnapshot.policyText, sizeof(g_state.storagePolicyText));
    g_state.dataRefresh = true;
    STATE_WRITE_END();
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

bool StorageManager::flushWorkerMetadataBatch(const char* reason, bool force) {
    if (!_ready) {
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

    if (RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
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
        requestSpoolRepair("health_truth_debt");
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
    _ensureDir(PATH_STORE_CONFIG_DIR);
    _ensureDir(PATH_STORE_VAULT_DIR);
    if (!locs || count <= 0) {
        LittleFS.remove(PATH_STORE_KNOWN_LOCATIONS);
        LittleFS.remove(PATH_STORE_LEGACY_KNOWN_LOCATIONS);
        DLOG_INFO("STORAGE", "Vault known locations cleared");
        return;
    }

    JsonDocument doc;
    doc["vault_schema"] = 1;
    doc["kind"] = "known_locations";
    doc["count"] = count;
    doc["updated_ms"] = millis();

    JsonArray arr = doc["locations"].to<JsonArray>();
    for (int i = 0; i < count; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["tag"] = locs[i].tag;
        o["lat"] = locs[i].lat;
        o["lon"] = locs[i].lon;
        o["r"]   = locs[i].radiusM;
    }

    File f = LittleFS.open(PATH_STORE_KNOWN_LOCATIONS, FILE_WRITE);
    if (!f) {
        DLOG_WARN("STORAGE", "Vault known locations open failed path=%s",
                  PATH_STORE_KNOWN_LOCATIONS);
        return;
    }

    serializeJson(doc, f);
    f.close();

    if (LittleFS.exists(PATH_STORE_LEGACY_KNOWN_LOCATIONS)) {
        LittleFS.remove(PATH_STORE_LEGACY_KNOWN_LOCATIONS);
    }

    DLOG_INFO("STORAGE", "Vault known locations saved count=%d", count);
}

int StorageManager::loadKnownLocations(
    SpectreState::KnownLocation* locs, int maxCount) {
    if (!locs || maxCount <= 0) return 0;

    auto loadFromPath = [&](const char* path, bool isVaultFormat) -> int {
        File f = LittleFS.open(path, "r");
        if (!f) return 0;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) {
            DLOG_WARN("STORAGE", "Known locations decode failed path=%s", path);
            return 0;
        }

        JsonArray arr = doc["locations"].as<JsonArray>();
        int count = 0;
        for (JsonObject o : arr) {
            if (count >= maxCount) break;
            strlcpy(locs[count].tag, o["tag"] | "", sizeof(locs[count].tag));
            locs[count].lat     = o["lat"] | 0.0f;
            locs[count].lon     = o["lon"] | 0.0f;
            locs[count].radiusM = o["r"]   | 50.0f;
            count++;
        }

        if (count > 0) {
            DLOG_INFO("STORAGE",
                      "Known locations loaded count=%d path=%s format=%s",
                      count,
                      path,
                      isVaultFormat ? "vault" : "legacy");
        }

        return count;
    };

    int count = loadFromPath(PATH_STORE_KNOWN_LOCATIONS, true);
    if (count > 0) {
        return count;
    }

    count = loadFromPath(PATH_STORE_LEGACY_KNOWN_LOCATIONS, false);
    if (count > 0) {
        saveKnownLocations(locs, count);
        DLOG_INFO("STORAGE", "Known locations migrated legacy->vault count=%d", count);
        return count;
    }

    return 0;
}

bool StorageManager::ensureBadUsbVault() {
    _ensureDir(PATH_STORE_CONFIG_DIR);
    _ensureDir(PATH_STORE_VAULT_DIR);
    _ensureDir(PATH_BADUSB_DIR);

    if (!LittleFS.exists(PATH_BADUSB_INDEX) && LittleFS.exists(PATH_LEGACY_BADUSB_DIR)) {
        _ensureDir(PATH_BADUSB_DIR);

        File legacyDir = LittleFS.open(PATH_LEGACY_BADUSB_DIR);
        if (legacyDir && legacyDir.isDirectory()) {
            File entry = legacyDir.openNextFile();
            while (entry) {
                String oldPath = String(entry.name());
                String fileName = oldPath;
                const int slash = fileName.lastIndexOf('/');
                if (slash >= 0) {
                    fileName = fileName.substring(slash + 1);
                }
                entry.close();

                const String newPath = String(PATH_BADUSB_DIR) + "/" + fileName;
                if (!LittleFS.exists(newPath)) {
                    LittleFS.rename(oldPath, newPath);
                }
                entry = legacyDir.openNextFile();
            }
            legacyDir.close();
        }

        if (LittleFS.exists(PATH_LEGACY_BADUSB_INDEX) && !LittleFS.exists(PATH_BADUSB_INDEX)) {
            LittleFS.rename(PATH_LEGACY_BADUSB_INDEX, PATH_BADUSB_INDEX);
        }

        LittleFS.rmdir(PATH_LEGACY_BADUSB_DIR);
    }

    if (LittleFS.exists(PATH_BADUSB_INDEX)) {
        return true;
    }

    JsonDocument doc;
    doc["vault_schema"] = 1;
    doc["kind"] = "badusb_scripts";
    doc["count"] = 0;
    doc["scripts"].to<JsonArray>();

    File f = LittleFS.open(PATH_BADUSB_INDEX, FILE_WRITE);
    if (!f) {
        DLOG_WARN("STORAGE", "BadUSB index create failed path=%s",
                  PATH_BADUSB_INDEX);
        return false;
    }

    serializeJson(doc, f);
    f.close();
    return true;
}

int StorageManager::loadBadUsbScriptIndex(BadUsbScriptInfo* outScripts, int maxCount) {
    if (!outScripts || maxCount <= 0) {
        return 0;
    }

    for (int i = 0; i < maxCount; ++i) {
        outScripts[i] = {};
    }

    ensureBadUsbVault();

    auto countScriptLines = [](const String& body) -> uint16_t {
        if (body.isEmpty()) {
            return 0;
        }

        uint16_t count = 0;
        int start = 0;
        while (start < body.length()) {
            int end = body.indexOf('\n', start);
            if (end < 0) {
                end = body.length();
            }

            String line = body.substring(start, end);
            line.trim();
            if (!line.isEmpty()) {
                ++count;
            }

            start = end + 1;
        }

        return count;
    };

    auto makeDisplayName = [](const String& fileName) -> String {
        String base = fileName;
        int slash = base.lastIndexOf('/');
        if (slash >= 0) {
            base = base.substring(slash + 1);
        }
        int dot = base.lastIndexOf('.');
        if (dot > 0) {
            base = base.substring(0, dot);
        }
        base.replace("_", " ");
        base.replace("-", " ");
        base.trim();
        return base.isEmpty() ? String("Script") : base;
    };

    int count = 0;
    File indexFile = LittleFS.open(PATH_BADUSB_INDEX, "r");
    if (indexFile) {
        JsonDocument doc;
        const DeserializationError err = deserializeJson(doc, indexFile);
        indexFile.close();

        if (!err) {
            JsonArray scripts = doc["scripts"].as<JsonArray>();
            for (JsonObject script : scripts) {
                if (count >= maxCount) {
                    break;
                }

                const char* fileName = script["file"] | "";
                if (!fileName[0]) {
                    continue;
                }

                strlcpy(outScripts[count].name,
                        script["name"] | makeDisplayName(fileName).c_str(),
                        sizeof(outScripts[count].name));
                strlcpy(outScripts[count].file,
                        fileName,
                        sizeof(outScripts[count].file));
                strlcpy(outScripts[count].desc,
                        script["desc"] | "",
                        sizeof(outScripts[count].desc));
                outScripts[count].lineCount =
                    static_cast<uint16_t>(script["lines"] | 0);
                if (outScripts[count].lineCount == 0) {
                    String body;
                    if (readBadUsbScript(fileName, body)) {
                        outScripts[count].lineCount = countScriptLines(body);
                    }
                }
                outScripts[count].valid = script["valid"] | true;
                ++count;
            }
        } else {
            DLOG_WARN("STORAGE", "BadUSB index parse failed path=%s",
                      PATH_BADUSB_INDEX);
        }
    }

    if (count > 0) {
        return count;
    }

    File dir = LittleFS.open(PATH_BADUSB_DIR);
    if (!dir || !dir.isDirectory()) {
        return 0;
    }

    File entry = dir.openNextFile();
    while (entry && count < maxCount) {
        if (!entry.isDirectory()) {
            String fullPath = entry.name();
            String fileName = fullPath;
            int slash = fileName.lastIndexOf('/');
            if (slash >= 0) {
                fileName = fileName.substring(slash + 1);
            }

            if (!fileName.equalsIgnoreCase("index.json")) {
                String body = entry.readString();
                strlcpy(outScripts[count].name,
                        makeDisplayName(fileName).c_str(),
                        sizeof(outScripts[count].name));
                strlcpy(outScripts[count].file,
                        fileName.c_str(),
                        sizeof(outScripts[count].file));
                strlcpy(outScripts[count].desc,
                        "Imported script",
                        sizeof(outScripts[count].desc));
                outScripts[count].lineCount = countScriptLines(body);
                outScripts[count].valid = true;
                ++count;
            }
        }

        entry = dir.openNextFile();
    }

    return count;
}

bool StorageManager::readBadUsbScript(const char* fileName, String& outScript) {
    outScript = "";
    if (!fileName || !fileName[0]) {
        return false;
    }

    String file = fileName;
    if (file.indexOf("..") >= 0) {
        DLOG_WARN("STORAGE", "BadUSB script path rejected: %s", fileName);
        return false;
    }
    if (!file.startsWith("/")) {
        file = String(PATH_BADUSB_DIR) + "/" + file;
    }

    File f = LittleFS.open(file, "r");
    if (!f) {
        DLOG_WARN("STORAGE", "BadUSB script open failed path=%s", file.c_str());
        return false;
    }

    outScript = f.readString();
    f.close();
    return true;
}

bool StorageManager::writeBadUsbScript(const char* fileName, const char* scriptBody,
                                       const char* displayName, const char* desc) {
    if (!fileName || !fileName[0] || !scriptBody) {
        return false;
    }

    ensureBadUsbVault();

    String normalized = fileName;
    if (normalized.indexOf("..") >= 0) {
        DLOG_WARN("STORAGE", "BadUSB script path rejected: %s", fileName);
        return false;
    }
    if (!normalized.endsWith(".txt")) {
        normalized += ".txt";
    }

    const String fullPath = String(PATH_BADUSB_DIR) + "/" + normalized;
    File script = LittleFS.open(fullPath, FILE_WRITE);
    if (!script) {
        DLOG_WARN("STORAGE", "BadUSB script write failed path=%s", fullPath.c_str());
        return false;
    }

    script.print(scriptBody);
    script.close();

    BadUsbScriptInfo scripts[16] = {};
    int count = loadBadUsbScriptIndex(scripts, 16);
    int slot = -1;
    for (int i = 0; i < count; ++i) {
        if (strcmp(scripts[i].file, normalized.c_str()) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0 && count < 16) {
        slot = count++;
    }

    if (slot >= 0) {
        const String scriptText = String(scriptBody);
        strlcpy(scripts[slot].name,
                (displayName && displayName[0]) ? displayName : normalized.c_str(),
                sizeof(scripts[slot].name));
        strlcpy(scripts[slot].file,
                normalized.c_str(),
                sizeof(scripts[slot].file));
        strlcpy(scripts[slot].desc,
                desc ? desc : "",
                sizeof(scripts[slot].desc));
        scripts[slot].lineCount = 0;
        if (!scriptText.isEmpty()) {
            uint16_t lineCount = 0;
            int start = 0;
            while (start < scriptText.length()) {
                int end = scriptText.indexOf('\n', start);
                if (end < 0) {
                    end = scriptText.length();
                }

                String line = scriptText.substring(start, end);
                line.trim();
                if (!line.isEmpty()) {
                    ++lineCount;
                }

                start = end + 1;
            }
            scripts[slot].lineCount = lineCount;
        }
        scripts[slot].valid = true;

        JsonDocument doc;
        doc["vault_schema"] = 1;
        doc["kind"] = "badusb_scripts";
        doc["count"] = count;
        JsonArray arr = doc["scripts"].to<JsonArray>();
        for (int i = 0; i < count; ++i) {
            JsonObject item = arr.add<JsonObject>();
            item["name"] = scripts[i].name;
            item["file"] = scripts[i].file;
            item["desc"] = scripts[i].desc;
            item["lines"] = scripts[i].lineCount;
            item["valid"] = scripts[i].valid;
        }

        File index = LittleFS.open(PATH_BADUSB_INDEX, FILE_WRITE);
        if (!index) {
            DLOG_WARN("STORAGE", "BadUSB index write failed path=%s",
                      PATH_BADUSB_INDEX);
            return false;
        }
        serializeJson(doc, index);
        index.close();
    }

    return true;
}

bool StorageManager::_ensureDir(String path) {
    if (!LittleFS.exists(path)) {
        return LittleFS.mkdir(path);
    }
    return true;
}

bool StorageManager::_removePathWithRetry(const String& path) {
    for (int attempt = 1; attempt <= 4; ++attempt) {
        if (LittleFS.remove(path)) {
            if (attempt > 1) {
                DLOG_INFO("STORAGE",
                          "Remove recovered path=%s attempt=%d",
                          path.c_str(),
                          attempt);
            }
            return true;
        }
        delay(2);
    }

    DLOG_WARN("STORAGE", "Remove failed path=%s", path.c_str());
    return false;
}

bool StorageManager::_rmdirWithRetry(const String& path) {
    for (int attempt = 1; attempt <= 4; ++attempt) {
        if (LittleFS.rmdir(path)) {
            if (attempt > 1) {
                DLOG_INFO("STORAGE",
                          "Rmdir recovered path=%s attempt=%d",
                          path.c_str(),
                          attempt);
            }
            return true;
        }
        delay(2);
    }

    DLOG_WARN("STORAGE", "Rmdir failed path=%s", path.c_str());
    return false;
}

bool StorageManager::_removeTree(const String& path) {
    if (!path.length() || path == "/") {
        return false;
    }

    if (!LittleFS.exists(path)) {
        return true;
    }

    File node = LittleFS.open(path);
    if (!node) {
        return _removePathWithRetry(path);
    }

    if (!node.isDirectory()) {
        node.close();
        return _removePathWithRetry(path);
    }

    std::vector<String> childPaths;
    File child = node.openNextFile();
    while (child) {
        String childPath = String(child.name());
        if (!childPath.startsWith("/")) {
            childPath = path;
            if (!childPath.endsWith("/")) {
                childPath += "/";
            }
            childPath += String(child.name());
        }
        childPaths.push_back(childPath);
        child.close();
        child = node.openNextFile();
    }
    node.close();

    for (const String& childPath : childPaths) {
        if (!_removeTree(childPath)) {
            DLOG_WARN("STORAGE",
                      "Remove tree child failed parent=%s child=%s",
                      path.c_str(),
                      childPath.c_str());
            return false;
        }
    }

    return _rmdirWithRetry(path);
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
    _binaryCheckpointBySegment.clear();
    _uploadIndexBySession.clear();
    _uploadEnrichBySession.clear();
    _uploadIndexSessions.clear();
    _uploadIndexStats = {};

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

bool StorageManager::_applyOneShotNonVaultReset() {
    if (!STORAGE_ONE_SHOT_NON_VAULT_RESET_ENABLED) {
        return true;
    }

    DLOG_WARN("STORAGE",
              "Applying non-vault reset via config.h toggle");
    return wipeNonVaultStorage();
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
    bool loaded = false;

    if (LittleFS.exists(PATH_EVENT_COUNTER)) {
        File f = LittleFS.open(PATH_EVENT_COUNTER, "r");
        if (f) {
            String stored = f.readString();
            stored.trim();
            if (stored.length()) {
                JsonDocument counterDoc;
                DeserializationError err = deserializeJson(counterDoc, stored);
                if (!err && counterDoc.is<JsonObject>()) {
                    nextId = counterDoc["next_event_id"] | 0U;
                    _eventCounterGeneration = counterDoc["generation"] | 0U;
                    loaded = nextId > 0U;
                } else {
                    uint32_t storedNext = strtoul(stored.c_str(), nullptr, 10);
                    if (storedNext > 0U) {
                        nextId = storedNext;
                        _eventCounterGeneration = 0;
                        loaded = true;
                    }
                }
            }
            f.close();
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
    _eventMetaLoaded = true;
    _pendingCountDirty = false;
    if (_eventMetaGeneration == 0 && _spoolIndex.generation != 0) {
        _eventMetaGeneration = _spoolIndex.generation;
    }
    _lastMetaSaveMs = millis();
    return true;
}

bool StorageManager::_persistEventMeta(bool force, const char* reason) {
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
            _spoolAuditRepairRequired = true;
            _setCounterTrustState(STORAGE_COUNTER_REPAIR_REQUIRED,
                                  "metadata_generation_diverged");
            return true;
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

    const uint32_t fileSize = static_cast<uint32_t>(f.size());
    const uint32_t tailSize =
        static_cast<uint32_t>(sizeof(SpoolBin::RecordPrefix)) +
        static_cast<uint32_t>(sizeof(SpoolBin::SpoolSegmentCheckpointV1));
    if (fileSize < sizeof(SpoolBin::SegmentHeaderV2) + tailSize) {
        DLOG_WARN("STORAGE",
                  "Boot audit checkpoint missing seg=%lu size=%lu tail=%lu",
                  static_cast<unsigned long>(seg.segmentId),
                  static_cast<unsigned long>(fileSize),
                  static_cast<unsigned long>(tailSize));
        f.close();
        return false;
    }

    const uint32_t tailOffset = fileSize - tailSize;
    if (!f.seek(tailOffset)) {
        DLOG_WARN("STORAGE",
                  "Boot audit checkpoint seek failed seg=%lu path=%s",
                  static_cast<unsigned long>(seg.segmentId),
                  path.c_str());
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

    if (prefix.type != SpoolBin::REC_CHECKPOINT ||
        prefix.length != sizeof(SpoolBin::SpoolSegmentCheckpointV1)) {
        DLOG_WARN("STORAGE",
                  "Boot audit checkpoint tail invalid seg=%lu type=%u len=%u",
                  static_cast<unsigned long>(seg.segmentId),
                  static_cast<unsigned>(prefix.type),
                  static_cast<unsigned>(prefix.length));
        f.close();
        return false;
    }

    std::vector<uint8_t> body(prefix.length);
    if (!SpoolBin::readBytes(f, body.data(), prefix.length)) {
        DLOG_WARN("STORAGE",
                  "Boot audit checkpoint body read failed seg=%lu path=%s",
                  static_cast<unsigned long>(seg.segmentId),
                  path.c_str());
        f.close();
        return false;
    }

    const uint32_t bodyOffset = tailOffset + sizeof(SpoolBin::RecordPrefix);
    SpoolBin::SpoolSegmentCheckpointV1 checkpoint;
    if (!SpoolBin::decodeCheckpointRecordV1(body.data(),
                                            body.size(),
                                            bodyOffset,
                                            checkpoint)) {
        DLOG_WARN("STORAGE",
                  "Boot audit checkpoint decode failed seg=%lu path=%s",
                  static_cast<unsigned long>(seg.segmentId),
                  path.c_str());
        f.close();
        return false;
    }

    f.close();

    if (checkpoint.segmentId != hdr.segmentId ||
        checkpoint.recordCount != hdr.recordCount ||
        checkpoint.lastEventId != hdr.lastEventId) {
        DLOG_WARN("STORAGE",
                  "Boot audit checkpoint mismatch seg=%lu hdrRecords=%lu cpRecords=%lu hdrLast=%lu cpLast=%lu",
                  static_cast<unsigned long>(seg.segmentId),
                  static_cast<unsigned long>(hdr.recordCount),
                  static_cast<unsigned long>(checkpoint.recordCount),
                  static_cast<unsigned long>(hdr.lastEventId),
                  static_cast<unsigned long>(checkpoint.lastEventId));
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

    if (generationMatch && !dirtySnapshot) {
        audit.counterOk = true;
        audit.headerAuditOk = true;
        audit.checkpointAuditOk = true;
        return true;
    }

    if (generationMatch) {
        audit.snapshotLagged = true;
        audit.counterOk = true;
        audit.headerAuditOk = true;
        audit.checkpointAuditOk = true;
        return true;
    }

    audit.snapshotLagged = true;
    audit.counterOk = true;

    bool indexBehind = false;
    for (const auto& seg : _spoolIndex.segments) {
        if (seg.segmentId == 0U) {
            continue;
        }

        if (_isSegmentQuarantined(seg.segmentId)) {
            audit.snapshotLagged = true;
            continue;
        }

        SpoolBin::SegmentHeaderV2 hdr;
        if (!_auditSpoolBinaryHeader(seg, hdr)) {
            audit.repairRequired = true;
            return false;
        }

        audit.auditedSegments++;

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
            return false;
        }

        if (seg.recordCount != hdr.recordCount ||
            seg.firstEventId != hdr.firstEventId ||
            seg.lastEventId != hdr.lastEventId) {
            indexBehind = true;
        }

        if (!_auditSpoolBinaryCheckpointTail(seg, hdr)) {
            DLOG_WARN("STORAGE",
                      "Boot audit checkpoint required seg=%lu",
                      static_cast<unsigned long>(seg.segmentId));
            audit.repairRequired = true;
            return false;
        }

        audit.checkpointAuditOk = true;
    }

    audit.headerAuditOk = true;
    audit.checkpointAuditOk = true;
    if (indexBehind || !generationMatch || dirtySnapshot || _hasInvalidSpoolSummaries()) {
        audit.snapshotLagged = true;
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
                      "Spool diag[%s] validSegments=%u untrustedSegments=%u invalidSegments=%u activeSegment=%lu storedEvents=%lu pendingUpload=%lu nextEventId=%lu",
                      reason ? reason : "?",
                      static_cast<unsigned>(validSegments),
                      static_cast<unsigned>(untrustedSegments),
                      static_cast<unsigned>(invalidSegments),
                      static_cast<unsigned long>(_spoolIndex.activeSegmentId),
                      static_cast<unsigned long>(totalEvents),
                      static_cast<unsigned long>(_spoolIndex.pendingTotal),
                      static_cast<unsigned long>(_nextEventId));
        } else {
            const char* backlogStateText =
                (getBacklogTrustState() == BACKLOG_DEGRADED) ? "degraded" : "unknown";
            DLOG_INFO("STORAGE",
                      "Spool diag[%s] validSegments=%u untrustedSegments=%u invalidSegments=%u activeSegment=%lu storedEvents=%lu pendingUpload=unknown nextEventId=%lu backlog=%s",
                      reason ? reason : "?",
                      static_cast<unsigned>(validSegments),
                      static_cast<unsigned>(untrustedSegments),
                      static_cast<unsigned>(invalidSegments),
                      static_cast<unsigned long>(_spoolIndex.activeSegmentId),
                      static_cast<unsigned long>(totalEvents),
                      static_cast<unsigned long>(_nextEventId),
                      backlogStateText);
        }
    } else {
        DLOG_INFO("STORAGE",
                  "Spool diag[%s] validSegments=%u untrustedSegments=%u invalidSegments=%u activeSegment=%lu storedEvents=%lu",
                  reason ? reason : "?",
                  static_cast<unsigned>(validSegments),
                  static_cast<unsigned>(untrustedSegments),
                  static_cast<unsigned>(invalidSegments),
                  static_cast<unsigned long>(_spoolIndex.activeSegmentId),
                  static_cast<unsigned long>(totalEvents));
    }

    _logBinaryUnsupportedAudit(reason);
}

String StorageManager::_today() {
    return TIME_SVC.dayStampForMillis(millis());
}

String StorageManager::_spoolDir() const {
    return String(PATH_SPOOL);
}

String StorageManager::_spoolIndexPath() const {
    return String(PATH_SPOOL_INDEX);
}

String StorageManager::_spoolSegmentPath(uint32_t segmentId) const {
    char buf[48];
    snprintf(buf, sizeof(buf), "%s/seg_%06lu.jsonl",
             PATH_SPOOL,
             static_cast<unsigned long>(segmentId));
    return String(buf);
}

String StorageManager::_spoolBinarySegmentPath(uint32_t segmentId) const {
    char binPath[48];
    snprintf(binPath, sizeof(binPath), "%s/seg_%06lu.bin",
             PATH_SPOOL,
             static_cast<unsigned long>(segmentId));

    if (LittleFS.exists(binPath)) {
        return String(binPath);
    }

    char legacyPath[48];
    snprintf(legacyPath, sizeof(legacyPath), "%s/seg_%06lu.sp2",
             PATH_SPOOL,
             static_cast<unsigned long>(segmentId));

    if (LittleFS.exists(legacyPath)) {
        return String(legacyPath);
    }

    return String(binPath);
}

String StorageManager::_uploadIndexPath(uint32_t segmentId) const {
    char path[48];
    snprintf(path, sizeof(path), "/spool/idx_%06lu.uix",
             static_cast<unsigned long>(segmentId));
    return String(path);
}

String StorageManager::_spoolSegmentPathForFormat(uint32_t segmentId, uint8_t format) const {
    if (format == SPOOL_SEGMENT_BIN_V2) {
        return _spoolBinarySegmentPath(segmentId);
    }
    return _spoolSegmentPath(segmentId);
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

bool StorageManager::_persistSpoolIndex(bool force, const char* reason) {
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
        o["p0"] = seg.p0Count;
        o["p1"] = seg.p1Count;
        o["p2"] = seg.p2Count;
        o["p3"] = seg.p3Count;
        o["t0"] = seg.minTimestampMs;
        o["t1"] = seg.maxTimestampMs;
        o["approx_bytes"] = seg.approxBytes;
        o["format"] = seg.format;
        o["trust"] = seg.trustState;
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
            seg.summaryValid = ((segObj["sok"] | 0U) != 0U) &&
                               (seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION);
            seg.recordCount = segObj["record_count"] | 0U;
            seg.eventCount = segObj["ev"] | 0U;
            seg.enrichDeltaCount = segObj["en"] | 0U;
            seg.missionCount = segObj["mi"] | 0U;
            seg.noiseCount = segObj["no"] | 0U;
            seg.pendingUploadMissionCount = segObj["pum"] | 0U;
            seg.pendingUploadNoiseCount = segObj["pun"] | 0U;
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
    const bool newSession = _uploadIndexBySession.find(sessionId) == _uploadIndexBySession.end();
    UploadIndexPagedSession& session = _uploadIndexBySession[sessionId];

    if (session.pages.empty() ||
        session.pages.back()->count >= UPLOAD_INDEX_PAGE_CAPACITY) {
        std::unique_ptr<UploadIndexPage> page(new (std::nothrow) UploadIndexPage());
        if (!page) {
            DLOG_WARN("STORAGE",
                      "Upload index page alloc failed session=%s count=%lu",
                      sessionId.c_str(),
                      static_cast<unsigned long>(session.count));
            if (newSession && session.count == 0 && session.pages.empty()) {
                _uploadIndexBySession.erase(sessionId);
            }
            return false;
        }
        session.pages.push_back(std::move(page));
    }

    UploadIndexPage* page = session.pages.back().get();
    page->records[page->count++] = rec;
    session.count++;

    for (const auto& existing : _uploadIndexSessions) {
        if (existing == sessionId) {
            return true;
        }
    }
    _uploadIndexSessions.push_back(sessionId);
    return true;
}

void StorageManager::_releaseUploadIndexMemory(const char* reason) {
    const uint32_t indexed = _uploadIndexStats.indexedEvents;
    const uint32_t sessions = static_cast<uint32_t>(_uploadIndexSessions.size());
    const bool hadResidentState =
        _uploadIndexResident ||
        indexed != 0 ||
        sessions != 0 ||
        !_uploadIndexBySession.empty();

    std::map<String, UploadIndexPagedSession>().swap(_uploadIndexBySession);
    std::map<String, std::map<uint32_t, SpoolEnrichmentDelta>>().swap(_uploadEnrichBySession);
    std::vector<String>().swap(_uploadIndexSessions);
    _uploadIndexStats = {};
    _uploadIndexResident = false;

    if (hadResidentState) {
        DLOG_INFO("STORAGE",
                  "Upload index released reason=%s indexed=%lu sessions=%lu",
                  (reason && reason[0]) ? reason : "-",
                  static_cast<unsigned long>(indexed),
                  static_cast<unsigned long>(sessions));
    }
}

bool StorageManager::_rebuildUploadIndexSegment(const SpoolSegmentInfo& seg) {
    if (seg.segmentId == 0) {
        return true;
    }

    const String indexPath = _uploadIndexPath(seg.segmentId);
    if (LittleFS.exists(indexPath) && !LittleFS.remove(indexPath)) {
        DLOG_WARN("STORAGE", "Upload index rebuild remove failed seg=%lu path=%s",
                  static_cast<unsigned long>(seg.segmentId),
                  indexPath.c_str());
        return false;
    }

    File indexFile = LittleFS.open(indexPath, "w");
    if (!indexFile) {
        DLOG_WARN("STORAGE", "Upload index rebuild open failed seg=%lu path=%s",
                  static_cast<unsigned long>(seg.segmentId),
                  indexPath.c_str());
        return false;
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

        if (indexFile.write(reinterpret_cast<const uint8_t*>(&record),
                            sizeof(record)) != sizeof(record)) {
            return false;
        }

        if (!_addUploadPtrToMemory(record)) {
            return false;
        }
        _uploadIndexStats.indexedEvents++;
        return true;
    };

    bool ok = true;
    uint32_t scanned = 0;

    if (seg.format == SPOOL_SEGMENT_BIN_V2) {
        const String spoolPath = _spoolBinarySegmentPath(seg.segmentId);
        File spool = LittleFS.open(spoolPath, "r");
        if (!spool) {
            indexFile.close();
            return false;
        }

        SpoolBin::SegmentHeaderV2 hdr;
        if (!SpoolBin::readSegmentHeaderV2(spool, hdr) ||
            hdr.magic != SpoolBin::SEGMENT_MAGIC ||
            hdr.version != 2 ||
            !spool.seek(sizeof(SpoolBin::SegmentHeaderV2))) {
            spool.close();
            indexFile.close();
            return false;
        }

        String lastSession;
        while (spool.position() < spool.size()) {
            const uint32_t offset = static_cast<uint32_t>(spool.position());
            SpoolBin::RecordPrefix prefix;
            if (!SpoolBin::readBytes(spool, &prefix, sizeof(prefix))) {
                ok = false;
                break;
            }

            const size_t remaining = static_cast<size_t>(spool.size() - spool.position());
            if (prefix.length > remaining) {
                ok = false;
                break;
            }

            std::vector<uint8_t> body(prefix.length);
            if (prefix.length > 0 &&
                !SpoolBin::readBytes(spool, body.data(), prefix.length)) {
                ok = false;
                break;
            }

            if (prefix.type == SpoolBin::REC_CHECKPOINT) {
                continue;
            }

            DecodedSpoolRecord rec;
            if (!_decodeBinarySpoolRecordBody(seg.segmentId,
                                              prefix.type,
                                              body.data(),
                                              body.size(),
                                              hdr.createdMs,
                                              lastSession,
                                              rec)) {
                ok = false;
                break;
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
            indexFile.close();
            return false;
        }

        while (spool.available()) {
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
                ok = false;
                break;
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

    indexFile.flush();
    indexFile.close();

    if (!ok) {
        DLOG_WARN("STORAGE", "Upload index rebuild failed seg=%lu",
                  static_cast<unsigned long>(seg.segmentId));
    }

    const bool summaryReady =
        seg.summaryValid &&
        seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;
    if (ok && summaryReady && seg.enrichDeltaCount > 0) {
        ok = _scanSegmentRecords(seg.segmentId,
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
                enrichment.tag = String((const char*)(rec.doc["tag"] | ""));
                enrichment.ts = rec.doc["ts"] | 0U;
                _uploadEnrichBySession[rec.sessionId][enrichment.id] = enrichment;
                return true;
            });

        if (!ok) {
            DLOG_WARN("STORAGE", "Upload enrich index rebuild failed seg=%lu",
                      static_cast<unsigned long>(seg.segmentId));
        }
    }
    return ok;
}

bool StorageManager::_rebuildUploadIndex() {
    _uploadIndexBySession.clear();
    _uploadEnrichBySession.clear();
    _uploadIndexSessions.clear();
    _uploadIndexStats = {};
    _uploadIndexResident = true;

    bool ok = true;
    for (const auto& seg : _spoolIndex.segments) {
        if (!_rebuildUploadIndexSegment(seg)) {
            ok = false;
        }
    }

    _uploadIndexStats.sessions = static_cast<uint32_t>(_uploadIndexSessions.size());

    return ok;
}

bool StorageManager::_openNewSpoolSegment() {
    const uint32_t segmentId = _spoolIndex.nextSegmentId++;
    const uint8_t format = _spoolIndex.format;

    String path;
    bool created = false;

    if (format == SPOOL_SEGMENT_BIN_V2) {
        path = _spoolBinarySegmentPath(segmentId);
        created = SpoolBin::createEmptySegmentV2(path, segmentId, millis());
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
    _spoolIndex.activeSegmentId = segmentId;
    _binaryLastSessionBySegment.erase(segmentId);
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

bool StorageManager::_ensureSpoolReady() {
    _ensureDir(_spoolDir());

    if (!_loadSpoolIndex()) {
        _spoolIndex.version = 2;
        _spoolIndex.format = SPOOL_SEGMENT_BIN_V2;
        _spoolIndex.nextSegmentId = 1;
        _spoolIndex.activeSegmentId = 0;
        _spoolIndex.oldestSegmentId = 0;
        _spoolIndex.pendingTotal = 0;
        _spoolIndex.sessions.clear();
        _spoolIndex.uploadedWatermarks.clear();
        _spoolIndex.segments.clear();
    }

    _spoolIndex.format = SPOOL_SEGMENT_BIN_V2;

    const bool resyncChanged = _resyncSpoolIndexFromFilesystem();
#if STORAGE_FAST_BOOT_DEFER_SPOOL_REPAIR
    const bool summaryChanged = false;
    if (_hasInvalidSpoolSummaries()) {
        _spoolSummaryRebuildPending = true;
    }
    if (resyncChanged) {
        _spoolAuditRepairRequired = true;
    }
#else
    const bool summaryChanged = _rebuildInvalidSegmentSummaries(false);
#endif

    if (!_loadEventCounter()) {
        DLOG_WARN("STORAGE", "Event counter recovery failed");
    }

    if (!_loadEventMeta()) {
        DLOG_WARN("STORAGE", "Event meta recovery required");
    }

    if (!_reconcileStorageMetadata()) {
        DLOG_WARN("STORAGE", "Storage metadata reconcile requested audit");
    }

    if (_spoolIndex.activeSegmentId == 0 || !_findSegmentInfo(_spoolIndex.activeSegmentId)) {
        if (!_openNewSpoolSegment()) {
            return false;
        }
    }

    const bool needPersist = resyncChanged || summaryChanged;
    if (needPersist &&
        RADIO_ARB.currentOwner() == RADIO_WIFI_CAPTURE &&
        _selectRepairMode() != REPAIR_EMERGENCY) {
        _spoolIndexDirty = true;
        _workerMetadataDirtyPending = true;
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

    _spoolIndex.segments = rebuilt;

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

    ScopedSemaphoreLock appendLock(_appendMutex);
    {
    const String segPath = _spoolBinarySegmentPath(seg->segmentId);

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

        _appendUVarintToBytes(body, recordId);
        _appendUVarintToBytes(body, tsDelta);

        const String sessionStr(e.sessionId);
        const bool sameSession = (sessionStr == lastSession);
        body.push_back(sameSession ? BIN_SESSION_SAME_AS_PREV : BIN_SESSION_INLINE);
        if (!sameSession) {
            _appendStringToBytes(body, sessionStr);
        }

        uint8_t enrichFlags = 0;
        const String tagStr(e.tag ? e.tag : "");
        if (tagStr.length()) enrichFlags |= 0x01;
        body.push_back(enrichFlags);

        _appendUVarintToBytes(body, e.eventId);
        _appendZigZag32ToBytes(body, _floatToE7(e.lat));
        _appendZigZag32ToBytes(body, _floatToE7(e.lon));
        _appendZigZag32ToBytes(body, _floatToCm(e.alt));
        _appendUVarintToBytes(body, _floatToDm(e.acc));
        if (enrichFlags & 0x01) {
            _appendStringToBytes(body, tagStr);
        }

        if (!SpoolBin::appendRecordToOpen(f,
                                          static_cast<uint8_t>(SpoolBin::REC_ENRICH_DELTA),
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
        _spoolIndex.nextEventId = _nextEventId;
    }

    _persistEventCounter(false, "append_enrich_batch");

    if (ok && seg->approxBytes >= SPOOL_SEGMENT_TARGET_BYTES) {
        if (!_openNewSpoolSegment()) ok = false;
    } else if (ok) {
        ok = _persistSpoolIndex(false, "append_enrich_batch");
    }

    if (appliedOut) *appliedOut = applied;
    if (failedOut)  *failedOut  = failed;
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

    const bool checkpointForce = (seg->approxBytes >= SPOOL_SEGMENT_TARGET_BYTES);
    (void)_maybeWriteBinarySegmentCheckpoint(
        *seg,
        checkpointForce ? "append_rotate" : "append_periodic",
        checkpointForce,
        nullptr);

    const bool shouldDeferIndex =
        deferIndexPersist || _workerAppendBatchActive ||
        _shouldDeferWorkerMetadataFlush();
    const uint32_t indexStartMs = timing ? millis() : 0U;
    if (seg->approxBytes >= SPOOL_SEGMENT_TARGET_BYTES) {
        if (!_openNewSpoolSegment()) {
            _setCounterTrustState(STORAGE_COUNTER_REPAIR_REQUIRED,
                                  "append_rotate_open_failed");
            requestSpoolRepair("append_rotate_open_failed");
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

    if ((seg->recordCount % 64U) == 0U) {
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
    static constexpr uint32_t CHECKPOINT_PENDING_DELTA = 8U;

    const auto it = _binaryCheckpointBySegment.find(seg.segmentId);
    const bool haveCheckpoint = (it != _binaryCheckpointBySegment.end());
    const SpoolBin::SpoolSegmentCheckpointV1* prev =
        haveCheckpoint ? &it->second : nullptr;

    const uint32_t currentPendingUpload =
        seg.pendingUploadMissionCount + seg.pendingUploadNoiseCount;
    const uint32_t currentPendingEnrichment = seg.pendingEnrichmentCount;

    uint32_t recordDelta = seg.recordCount;
    uint32_t pendingDelta = currentPendingUpload + currentPendingEnrichment;

    if (prev) {
        if (seg.recordCount >= prev->recordCount) {
            recordDelta = seg.recordCount - prev->recordCount;
        }
        const uint32_t prevPendingUpload =
            prev->pendingUploadMissionCount + prev->pendingUploadNoiseCount;
        const uint32_t prevPendingEnrichment = prev->pendingEnrichmentCount;
        pendingDelta =
            (currentPendingUpload > prevPendingUpload)
                ? (currentPendingUpload - prevPendingUpload)
                : (prevPendingUpload - currentPendingUpload);
        pendingDelta +=
            (currentPendingEnrichment > prevPendingEnrichment)
                ? (currentPendingEnrichment - prevPendingEnrichment)
                : (prevPendingEnrichment - currentPendingEnrichment);
    }

    const bool due =
        force ||
        (recordDelta >= CHECKPOINT_RECORD_INTERVAL) ||
        (pendingDelta >= CHECKPOINT_PENDING_DELTA);

    if (!due) {
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
    if (!SpoolBin::appendCheckpointRecordV1(path, checkpoint, &loc)) {
        DLOG_WARN("STORAGE",
                  "Binary checkpoint write failed seg=%lu reason=%s",
                  static_cast<unsigned long>(seg.segmentId),
                  (reason && reason[0]) ? reason : "-");
        return false;
    }

    seg.approxBytes = loc.offset + loc.len;
    _binaryCheckpointBySegment[seg.segmentId] = checkpoint;
    _binaryLastSessionBySegment[seg.segmentId] = "";

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

bool StorageManager::_appendSpoolEnrichmentDelta(const String& sessionId,
                                                 uint32_t eventId,
                                                 float lat, float lon,
                                                 float alt, float accuracy,
                                                 const char* tag) {
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

    _appendUVarintToBytes(body, recordId);
    _appendUVarintToBytes(body, tsDelta);

    const String lastSession = _binaryLastSessionBySegment[seg->segmentId];
    const bool sameSession = (sessionId == lastSession);
    body.push_back(sameSession ? BIN_SESSION_SAME_AS_PREV : BIN_SESSION_INLINE);
    if (!sameSession) {
        _appendStringToBytes(body, sessionId);
    }

    uint8_t enrichFlags = 0;
    const String tagStr = String(tag ? tag : "");
    if (tagStr.length()) {
        enrichFlags |= 0x01;
    }
    body.push_back(enrichFlags);

    _appendUVarintToBytes(body, eventId);
    _appendZigZag32ToBytes(body, _floatToE7(lat));
    _appendZigZag32ToBytes(body, _floatToE7(lon));
    _appendZigZag32ToBytes(body, _floatToCm(alt));
    _appendUVarintToBytes(body, _floatToDm(accuracy));
    if (enrichFlags & 0x01) {
        _appendStringToBytes(body, tagStr);
    }

    SpoolBin::SegmentHeaderV2 hdr;
    const uint32_t appendStartMs = millis();
    const bool appendOk = SpoolBin::appendRecordV2(
            _spoolBinarySegmentPath(seg->segmentId),
            static_cast<uint8_t>(SpoolBin::REC_ENRICH_DELTA),
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

    _binaryLastSessionBySegment[seg->segmentId] = sessionId;

    const bool checkpointForce = (seg->approxBytes >= SPOOL_SEGMENT_TARGET_BYTES);
    (void)_maybeWriteBinarySegmentCheckpoint(
        *seg,
        checkpointForce ? "append_rotate" : "append_periodic",
        checkpointForce,
        nullptr);

    if (seg->approxBytes >= SPOOL_SEGMENT_TARGET_BYTES) {
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

    if ((seg->recordCount % 64U) == 0U) {
        _logSpoolDiagnostics("append_enrich_batch");
    }
    }

#if SPECTRE_TRACE_ENRICH_DELTA
    DLOG_INFO("STORAGE", "Spool enrich_delta session=%s event=%lu record=%lu",
              sessionId.c_str(),
              static_cast<unsigned long>(eventId),
              static_cast<unsigned long>(recordId));
#endif
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
                    dst["lat"] = enrichment.lat;
                    dst["lon"] = enrichment.lon;
                    dst["alt"] = enrichment.alt;
                    dst["acc"] = enrichment.acc;
                    if (enrichment.tag.length()) {
                        dst["tag"] = enrichment.tag;
                    }
                    dst["enriched_ts"] = enrichment.ts;
                    dst[F_ENRICH_STATE] =
                        static_cast<uint8_t>(STORAGE_ENRICH_DONE);
                    status = EVT_ENRICHED;
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

bool StorageManager::_getUploadEventBatchForSessionFromIndex(const String& sessionId,
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
        JsonDocument doc;
    };

    auto resetOutRecord = [](DecodedSpoolRecord& rec) {
        rec.recordType = SPOOL_REC_UNKNOWN;
        rec.eventId = 0;
        rec.sessionId = "";
        rec.doc.clear();
    };

    std::vector<BatchedEvent> events;
    events.reserve(static_cast<size_t>(maxCount));
    std::vector<UploadIndexRecordV1> selected;
    selected.reserve(static_cast<size_t>(maxCount));
    auto sessionIt = _uploadIndexBySession.find(sessionId);
    if (sessionIt == _uploadIndexBySession.end()) {
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
            selected.push_back(rec);
            if (selected.size() >= static_cast<size_t>(maxCount)) {
                break;
            }
        }

        if (selected.size() >= static_cast<size_t>(maxCount)) {
            break;
        }
    }

    size_t selectedIndex = 0;
    while (selectedIndex < selected.size()) {
        const uint32_t segmentId = selected[selectedIndex].segmentId;
        const SpoolSegmentInfo* seg = _findSegmentInfo(segmentId);
        if (!seg) {
            _releaseUploadIndexMemory("index_missing_segment");
            return false;
        }

        const String path = _spoolSegmentPathForFormat(segmentId, seg->format);
        File f = LittleFS.open(path, "r");
        if (!f) {
            _releaseUploadIndexMemory("index_spool_open_failed");
            return false;
        }

        SpoolBin::SegmentHeaderV2 hdr;
        bool headerOk = true;
        if (seg->format == SPOOL_SEGMENT_BIN_V2) {
            headerOk =
                SpoolBin::readSegmentHeaderV2(f, hdr) &&
                hdr.magic == SpoolBin::SEGMENT_MAGIC &&
                hdr.version == 2;
        }

        if (!headerOk) {
            f.close();
            _releaseUploadIndexMemory("index_spool_header_invalid");
            return false;
        }

        while (selectedIndex < selected.size() &&
               selected[selectedIndex].segmentId == segmentId) {
            const UploadIndexRecordV1& ptr = selected[selectedIndex];
            DecodedSpoolRecord rec;
            resetOutRecord(rec);

            bool ok = _validateUploadIndexRecord(ptr, ptr.segmentId) &&
                      ptr.offset < static_cast<uint32_t>(f.size()) &&
                      ptr.len != 0 &&
                      ptr.offset + ptr.len <= static_cast<uint32_t>(f.size());

            if (ok && seg->format == SPOOL_SEGMENT_BIN_V2) {
                ok = f.seek(ptr.offset);

                SpoolBin::RecordPrefix prefix;
                if (ok) {
                    ok = SpoolBin::readBytes(f, &prefix, sizeof(prefix));
                }

                const uint32_t encodedLen =
                    static_cast<uint32_t>(sizeof(prefix)) +
                    static_cast<uint32_t>(prefix.length);
                if (ok && encodedLen != ptr.len) {
                    ok = false;
                }

                std::vector<uint8_t> body;
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
                f.close();
                _releaseUploadIndexMemory("index_read_failed");
                return false;
            }

            BatchedEvent event;
            event.eventId = rec.eventId;
            event.doc.set(rec.doc.as<JsonVariantConst>());
            events.push_back(std::move(event));
            selectedIndex++;
        }

        f.close();
    }

    const uint32_t watermark = _uploadedWatermarkForSession(sessionId);
    const auto enrichSessionIt = _uploadEnrichBySession.find(sessionId);

    for (const auto& event : events) {
        JsonObject dst = batch.add<JsonObject>();
        for (JsonPairConst kv : event.doc.as<JsonObjectConst>()) {
            dst[kv.key().c_str()].set(kv.value());
        }

        EventStatus status = EVT_RAW;
        auto it = (enrichSessionIt != _uploadEnrichBySession.end()) ?
            enrichSessionIt->second.find(event.eventId) :
            std::map<uint32_t, SpoolEnrichmentDelta>::const_iterator{};
        if (enrichSessionIt != _uploadEnrichBySession.end() &&
            it != enrichSessionIt->second.end()) {
            const SpoolEnrichmentDelta& enrichment = it->second;
            dst["lat"] = enrichment.lat;
            dst["lon"] = enrichment.lon;
            dst["alt"] = enrichment.alt;
            dst["acc"] = enrichment.acc;
            if (enrichment.tag.length()) {
                dst["tag"] = enrichment.tag;
            }
            dst["enriched_ts"] = enrichment.ts;
            dst[F_ENRICH_STATE] =
                static_cast<uint8_t>(STORAGE_ENRICH_DONE);
            status = EVT_ENRICHED;
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
                    enrichment.tag = String((const char*)(rec.doc["tag"] | ""));
                    enrichment.ts = rec.doc["ts"] | 0U;
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

    for (const auto& event : events) {
        JsonObject dst = batch.add<JsonObject>();
        for (JsonPairConst kv : event.doc.as<JsonObjectConst>()) {
            dst[kv.key().c_str()].set(kv.value());
        }

        EventStatus status = EVT_RAW;
        auto it = enrichById.find(event.eventId);
        if (it != enrichById.end()) {
            const SpoolEnrichmentDelta& enrichment = it->second;
            dst["lat"] = enrichment.lat;
            dst["lon"] = enrichment.lon;
            dst["alt"] = enrichment.alt;
            dst["acc"] = enrichment.acc;
            if (enrichment.tag.length()) {
                dst["tag"] = enrichment.tag;
            }
            dst["enriched_ts"] = enrichment.ts;
            dst[F_ENRICH_STATE] =
                static_cast<uint8_t>(STORAGE_ENRICH_DONE);
            status = EVT_ENRICHED;
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

    if (sessionMode == BIN_SESSION_SAME_AS_PREV) {
        sessionId = sessionSeed;
    } else {
        if (!_readStringFromBytes(p, end, sessionId)) {
            return false;
        }
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
    {
        char tsIso[24] = {};
        TIME_SVC.formatIsoForMillis(ts, tsIso, sizeof(tsIso));
        root[F_TIMESTAMP_ISO] = tsIso;
    }

    if (payloadFamily == BIN_PAYLOAD_PROBE_DEVICE) {
        String mac;
        String ssid;
        String ieFingerprint;
        String probeSetHash;
        int32_t rssi = 0;
        uint32_t channel = 0;
        bool isBroadcast = false;
        bool isRandomMac = false;

        if (p >= end) return false;
        const uint8_t flags = *p++;

        uint8_t lastOui[3] = {0, 0, 0};
        bool hasLastOui = false;

        if (!_readMacFieldFromBytes(p, end, mac, lastOui, hasLastOui)) {
            DLOG_WARN("STORAGE", "Indexed binary probe/device MAC decode failed seg=%lu",
                      static_cast<unsigned long>(segmentId));
            return false;
        }

        if ((flags & 0x01) && !_readStringFromBytes(p, end, ssid)) return false;
        if ((flags & 0x02) && !_readZigZag32FromBytes(p, end, rssi)) return false;

        root["mac"] = mac;
        if (flags & 0x01) {
            if (typeStr == "probe") root["probed_ssid"] = ssid;
            else root["ssid"] = ssid;
        }
        if (flags & 0x02) root["rssi"] = rssi;
        if (flags & 0x04) {
            if (!_readUVarintFromBytes(p, end, channel)) return false;
            root["channel"] = channel;
        }
        if (flags & 0x08) {
            if (!_readStringFromBytes(p, end, ieFingerprint)) return false;
            root["ie_fingerprint"] = ieFingerprint;
        }
        if (flags & 0x10) {
            if (!_readStringFromBytes(p, end, probeSetHash)) return false;
            root["probe_set_hash"] = probeSetHash;
        }
        if (flags & 0x20) {
            if (p >= end) return false;
            isRandomMac = (*p++ != 0);
            root["is_random_mac"] = isRandomMac ? 1 : 0;
        }
        if (flags & 0x40) {
            if (p >= end) return false;
            isBroadcast = (*p++ != 0);
            root["is_broadcast"] = isBroadcast ? 1 : 0;
        }

    } else if (payloadFamily == BIN_PAYLOAD_PMKID) {
        String ap;
        String sta;
        String ssid;
        String pmkidHex;
        String hashcatLine;
        int32_t rssi = 0;

        if (p >= end) return false;
        const uint8_t flags = *p++;

        uint8_t lastOui[3] = {0, 0, 0};
        bool hasLastOui = false;

        if (!_readMacFieldFromBytes(p, end, ap, lastOui, hasLastOui) ||
            !_readMacFieldFromBytes(p, end, sta, lastOui, hasLastOui)) {
            DLOG_WARN("STORAGE", "Indexed binary pmkid MAC decode failed seg=%lu",
                      static_cast<unsigned long>(segmentId));
            return false;
        }

        if ((flags & 0x01) && !_readStringFromBytes(p, end, ssid)) return false;

        root["ap"] = ap;
        root["sta"] = sta;
        root["bssid"] = ap;
        root["client_mac"] = sta;
        if (flags & 0x01) root["ssid"] = ssid;

        if (flags & 0x02) {
            if (!_readZigZag32FromBytes(p, end, rssi)) return false;
            root["rssi"] = rssi;
        }
        if (flags & 0x04) {
            if (!_readStringFromBytes(p, end, pmkidHex)) return false;
            root["pmkid_hex"] = pmkidHex;
        }
        if (flags & 0x08) {
            if (!_readStringFromBytes(p, end, hashcatLine)) return false;
            root["hashcat_line"] = hashcatLine;
        }

    } else if (payloadFamily == BIN_PAYLOAD_HANDSHAKE) {
        String ap;
        String sta;
        String ssid;
        uint32_t frameMask = 0;
        uint32_t messageNumber = 0;

        if (p >= end) return false;
        const uint8_t flags = *p++;

        uint8_t lastOui[3] = {0, 0, 0};
        bool hasLastOui = false;

        if (!_readMacFieldFromBytes(p, end, ap, lastOui, hasLastOui) ||
            !_readMacFieldFromBytes(p, end, sta, lastOui, hasLastOui)) {
            DLOG_WARN("STORAGE", "Indexed binary handshake MAC decode failed seg=%lu",
                      static_cast<unsigned long>(segmentId));
            return false;
        }

        if ((flags & 0x01) && !_readStringFromBytes(p, end, ssid)) return false;
        if (!_readUVarintFromBytes(p, end, frameMask)) return false;

        root["ap"] = ap;
        root["sta"] = sta;
        root["bssid"] = ap;
        root["client"] = sta;
        if (flags & 0x01) root["ssid"] = ssid;
        root["frame_mask"] = frameMask;
        root["event_type"] = "handshake";

        if (flags & 0x02) {
            int32_t rssi = 0;
            if (!_readZigZag32FromBytes(p, end, rssi)) return false;
            root["rssi"] = rssi;
        }
        if (flags & 0x04) {
            if (!_readUVarintFromBytes(p, end, messageNumber)) return false;
            root["message"] = messageNumber;
            root["msg"] = messageNumber;
        }

    } else if (payloadFamily == BIN_PAYLOAD_DRONE) {
        String droneId;
        String mac;
        String protocol;
        int32_t latitudeE7 = 0;
        int32_t longitudeE7 = 0;
        uint32_t channel = 0;
        uint8_t lastOui[3] = {0, 0, 0};
        bool hasLastOui = false;

        if (p >= end) return false;
        const uint8_t flags = *p++;

        if ((flags & 0x01) && !_readStringFromBytes(p, end, droneId)) return false;
        if ((flags & 0x02) &&
            !_readMacFieldFromBytes(p, end, mac, lastOui, hasLastOui)) {
            return false;
        }
        if (flags & 0x04) {
            int32_t rssi = 0;
            if (!_readZigZag32FromBytes(p, end, rssi)) return false;
            root["rssi"] = rssi;
        }
        if (flags & 0x08) {
            if (!_readUVarintFromBytes(p, end, channel)) return false;
            root["channel"] = channel;
        }
        if (flags & 0x10) {
            if (!_readStringFromBytes(p, end, protocol)) return false;
            root["protocol"] = protocol;
        }
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

    } else if (payloadFamily == BIN_PAYLOAD_FIELD_MAP) {
        if (!_readBinaryFieldMapFromBytes(p, end, root)) {
            DLOG_WARN("STORAGE", "Indexed binary field map decode failed seg=%lu",
                      static_cast<unsigned long>(segmentId));
            return false;
        }

    } else if (payloadFamily == BIN_PAYLOAD_JSON_FALLBACK) {
        if (eventFlags & BIN_EVENT_HAS_PAYLOAD) {
            String payloadJson;
            if (!_readStringFromBytes(p, end, payloadJson)) {
                return false;
            }

            if (payloadJson.length()) {
                JsonDocument payloadDoc;
                DeserializationError err = deserializeJson(payloadDoc, payloadJson);
                if (err || !payloadDoc.is<JsonObject>()) {
                    return false;
                }

                for (JsonPairConst kv : payloadDoc.as<JsonObjectConst>()) {
                    root[kv.key().c_str()].set(kv.value());
                }
            }
        }

    } else {
        DLOG_WARN("STORAGE", "Indexed binary unknown payload family seg=%lu family=%u",
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
                out.tag = "";
                JsonVariantConst tagField = rec.doc["tag"];
                if (!tagField.isNull()) {
                    if (tagField.is<const char*>()) {
                        const char* tagText = tagField.as<const char*>();
                        if (tagText) {
                            out.tag = tagText;
                        }
                    } else {
                        DLOG_WARN("STORAGE",
                                  "Enrich tag non-string session=%s event=%lu",
                                  sessionId.c_str(),
                                  static_cast<unsigned long>(out.id));
                    }
                }
                out.ts = rec.doc["ts"] | 0U;
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
    std::map<uint32_t, bool>& enrichedById) const {

    enrichedById.clear();
    if (filterBySession && !sessionId.length()) return true;

    for (const auto& seg : _spoolIndex.segments) {
        const bool summaryReady =
            seg.summaryValid &&
            seg.summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION;

        if (summaryReady && seg.enrichDeltaCount == 0) {
            continue;
        }

        const bool ok = _scanSegmentRecords(seg.segmentId,
            [&](const DecodedSpoolRecord& rec) -> bool {
                if (rec.recordType != SPOOL_REC_ENRICH_DELTA) {
                    return true;
                }

                if (filterBySession &&
                    (!rec.sessionId.length() || rec.sessionId != sessionId)) {
                    return true;
                }

                const uint32_t id = rec.doc["event_id"] | 0U;
                if (id != 0) {
                    enrichedById[id] = true;
                }
                return true;
            });

        if (!ok) {
            return false;
        }
    }

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

    if (seg->summaryValid &&
        seg->summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION &&
        seg->eventCount == 0) {
        return true;
    }

    bool foundPending = false;

    if (seg->format == SPOOL_SEGMENT_BIN_V2) {
        const bool ok = _scanBinarySegmentMetaRecords(
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
            });

        return ok && !foundPending;
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

StorageManager::SegmentPendingScanResult
StorageManager::_segmentPendingScanResult(uint32_t segmentId) const {
    const SpoolSegmentInfo* seg = _findSegmentInfo(segmentId);
    if (!seg) return SegmentPendingScanResult::SCAN_FAILED;

    if (seg->summaryValid &&
        seg->summaryVersion == SPOOL_SEGMENT_SUMMARY_VERSION &&
        seg->eventCount == 0) {
        return SegmentPendingScanResult::NO_PENDING;
    }

    bool foundPending = false;

    if (seg->format == SPOOL_SEGMENT_BIN_V2) {
        const bool ok = _scanBinarySegmentMetaRecords(
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
            });

        if (!ok) {
            DLOG_WARN("STORAGE", "Spool compact scan_failed seg=%lu format=bin",
                      static_cast<unsigned long>(segmentId));
            return SegmentPendingScanResult::SCAN_FAILED;
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
    for (const auto& seg : _spoolIndex.segments) {
        if (!_segmentScanReadable(seg.segmentId)) {
            DLOG_WARN("STORAGE", "Spool compact prune skipped scan_failed seg=%lu",
                      static_cast<unsigned long>(seg.segmentId));
            return false;
        }
    }

    const String activeSession = SESS.getId();

    auto sessionHasPending = [&](const String& sid) -> bool {
        if (sid == activeSession) return true;
        return _pendingEventCountForSessionFromSpool(sid) > 0;
    };

    const size_t sessionsBefore = _spoolIndex.sessions.size();
    _spoolIndex.sessions.erase(
        std::remove_if(_spoolIndex.sessions.begin(),
                       _spoolIndex.sessions.end(),
                       [&](const String& sid) {
                           return !sessionHasPending(sid);
                       }),
        _spoolIndex.sessions.end());

    const size_t watermarksBefore = _spoolIndex.uploadedWatermarks.size();
    _spoolIndex.uploadedWatermarks.erase(
        std::remove_if(_spoolIndex.uploadedWatermarks.begin(),
                       _spoolIndex.uploadedWatermarks.end(),
                       [&](const std::pair<String, uint32_t>& entry) {
                           return !sessionHasPending(entry.first);
                       }),
        _spoolIndex.uploadedWatermarks.end());

    const size_t sessionsRemoved =
        sessionsBefore - _spoolIndex.sessions.size();
    const size_t watermarksRemoved =
        watermarksBefore - _spoolIndex.uploadedWatermarks.size();

    if (sessionsRemoved || watermarksRemoved) {
        DLOG_INFO("STORAGE",
                  "Spool compact pruned sessions=%u watermarks=%u",
                  static_cast<unsigned>(sessionsRemoved),
                  static_cast<unsigned>(watermarksRemoved));
        return true;
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
        return _scanBinarySegmentMetaRecords(
            _spoolBinarySegmentPath(segmentId),
            [](const BinaryMetaRecord&) -> bool {
                return true;
            });
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
    doc["reason"] = _spoolCorruptionReasonText(reason);
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
              _spoolCorruptionReasonText(reason),
              (detail && detail[0]) ? detail : "-",
              quarantinePath.c_str());
    _setCounterTrustState(CounterTrust::EmergencyOnly, "segment_quarantined");
    return true;
}

void StorageManager::_logSpoolAuditResult(const char* reason,
                                          const SpoolAuditResult& audit) {
    const char* safeReason = (reason && reason[0] != '\0') ? reason : "-";
    if (audit.hadFatalSegmentError) {
        DLOG_WARN("STORAGE",
                  "Spool audit[%s] scannedSegs=%lu scannedRecords=%lu validEvents=%lu validEnrichDeltas=%lu invalidRecords=%lu skippedRecords=%lu quarantined=%lu unreadable=%lu pendingUpload=%lu->%lu nextEventId=%lu maxEventIdSeen=%lu repaired=%d fatal=%d",
                  safeReason,
                  static_cast<unsigned long>(audit.scannedSegments),
                  static_cast<unsigned long>(audit.scannedRecords),
                  static_cast<unsigned long>(audit.validEventRecords),
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
                  "Spool audit[%s] scannedSegs=%lu scannedRecords=%lu validEvents=%lu validEnrichDeltas=%lu invalidRecords=%lu skippedRecords=%lu quarantined=%lu unreadable=%lu pendingUpload=%lu->%lu nextEventId=%lu maxEventIdSeen=%lu repaired=%d fatal=%d",
                  safeReason,
                  static_cast<unsigned long>(audit.scannedSegments),
                  static_cast<unsigned long>(audit.scannedRecords),
                  static_cast<unsigned long>(audit.validEventRecords),
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

    std::vector<SpoolSegmentInfo> kept;
    bool changed = false;
    bool scanFailed = false;
    unsigned removedCount = 0;

    for (const auto& seg : _spoolIndex.segments) {
        const bool isActive = (seg.segmentId == _spoolIndex.activeSegmentId);

        if (isActive) {
            kept.push_back(seg);
            continue;
        }

        // Give the just-rotated segment one full grace cycle before trying
        // to unlink it. This avoids LittleFS "Has open FD" churn.
        if (graceSegmentId != 0 && seg.segmentId == graceSegmentId) {
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
            const bool removed = _removeSpoolSegmentFile(seg.segmentId);
            if (removed) {
                changed = true;
                removedCount++;
            } else {
                kept.push_back(seg);
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
        _spoolIndex.segments = kept;
        if (!_spoolIndex.segments.empty()) {
            _spoolIndex.oldestSegmentId = _spoolIndex.segments.front().segmentId;
        } else {
            _spoolIndex.oldestSegmentId = _spoolIndex.activeSegmentId;
        }

        if (!scanFailed) {
            changed |= _pruneUploadedSessionState();
            _spoolAuditRepairRequired = false;
        } else {
            DLOG_WARN("STORAGE", "Spool compact skipped prune after scan failure");
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

    if (!scanFailed && _pruneUploadedSessionState()) {
        _spoolAuditRepairRequired = false;
        const bool ok = _persistSpoolIndex(true, "compact_prune");
        if (ok) {
            DLOG_INFO("STORAGE", "Spool compact pruned metadata only");
            _logSpoolDiagnostics("compact_prune");
            _checkSpoolInvariants("compact_prune", false);
        }
        return ok;
    }

    if (scanFailed) {
        DLOG_WARN("STORAGE", "Spool compact no_change prune_skipped_scan_failed");
        _spoolAuditRepairRequired = true;
    } else {
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

    Serial.printf("[SPOOL] ok=%d scannedSegs=%lu scannedRecords=%lu\r\n",
                  ok ? 1 : 0,
                  static_cast<unsigned long>(audit.scannedSegments),
                  static_cast<unsigned long>(audit.scannedRecords));
    Serial.printf("[SPOOL] validEvents=%lu validEnrichDeltas=%lu invalidRecords=%lu skippedRecords=%lu\r\n",
                  static_cast<unsigned long>(audit.validEventRecords),
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
    const StorageLaneCounts pendingEnrich = getPendingEnrichmentCounts();
    Serial.printf("[SPOOL] pendingEnrich mission=%lu noise=%lu total=%lu\r\n",
                  static_cast<unsigned long>(pendingEnrich.mission),
                  static_cast<unsigned long>(pendingEnrich.noise),
                  static_cast<unsigned long>(pendingEnrich.total()));
    Serial.printf("[SPOOL] mismatch=%d repaired=%d fatal=%d\r\n",
                  audit.hadMismatch ? 1 : 0,
                  audit.repaired    ? 1 : 0,
                  audit.hadFatalSegmentError ? 1 : 0);

    if (repair) {
        _checkSpoolInvariants("post_repair", false);
    }
}

void StorageManager::spoolDiagToSerial() {
    if (!_ready) {
        Serial.println("[SPOOL] not ready");
        return;
    }

    uint32_t validSegments = 0;
    uint32_t untrustedSegments = 0;
    uint32_t invalidSegments = 0;
    for (const auto& seg : _spoolIndex.segments) {
        switch (seg.trustState) {
            case SPOOL_SEGMENT_TRUSTED:   validSegments++; break;
            case SPOOL_SEGMENT_UNTRUSTED: untrustedSegments++; break;
            case SPOOL_SEGMENT_INVALID:
            default:                      invalidSegments++; break;
        }
    }

    Serial.printf("[SPOOL] validSegments=%u untrustedSegments=%u invalidSegments=%u active=%lu oldest=%lu\r\n",
                  static_cast<unsigned>(validSegments),
                  static_cast<unsigned>(untrustedSegments),
                  static_cast<unsigned>(invalidSegments),
                  static_cast<unsigned long>(_spoolIndex.activeSegmentId),
                  static_cast<unsigned long>(_spoolIndex.oldestSegmentId));
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
    const StorageLaneCounts pendingEnrich = getPendingEnrichmentCounts();
    Serial.printf("[SPOOL] pendingEnrich mission=%lu noise=%lu total=%lu\r\n",
                  static_cast<unsigned long>(pendingEnrich.mission),
                  static_cast<unsigned long>(pendingEnrich.noise),
                  static_cast<unsigned long>(pendingEnrich.total()));
    Serial.printf("[SPOOL] used=%s auditRepairReq=%d summaryRebuildPending=%d\r\n",
                  getUsedString().c_str(),
                  _spoolAuditRepairRequired ? 1 : 0,
                  _spoolSummaryRebuildPending ? 1 : 0);

    for (const auto& seg : _spoolIndex.segments) {
        Serial.printf("[SPOOL]   seg=%lu fmt=%s trust=%s records=%lu storedEvents=%lu"
                      " storedLaneEvents=%lu summaryOk=%d active=%d\r\n",
                      static_cast<unsigned long>(seg.segmentId),
                      _segmentFormatText(seg.format),
                      _spoolTrustText(seg.trustState),
                      static_cast<unsigned long>(seg.recordCount),
                      static_cast<unsigned long>(seg.eventCount),
                      static_cast<unsigned long>(seg.missionCount + seg.noiseCount),
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
