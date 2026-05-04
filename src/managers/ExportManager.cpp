

#include "ExportManager.h"

#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>

#include "../core/DebugLog.h"
#include "../core/MissionRuntime.h"
#include "../core/Session.h"
#include "../core/SpectreState.h"
#include "../data/Schema.h"
#include "SettingsManager.h"
#include "StorageManager.h"
#include "TimeService.h"

namespace {
constexpr const char* FILE_MANIFEST = "manifest.json";
constexpr const char* FILE_EVENTS = "events.jsonl";
constexpr const char* FILE_PROBES = "probes.csv";
constexpr const char* FILE_DEVICES = "devices.csv";
constexpr const char* FILE_DRONES = "drones.csv";
constexpr const char* FILE_PMKIDS = "pmkids.csv";
constexpr const char* FILE_PMKID_HASHCAT = "pmkid.hc22000";
constexpr size_t EXPORT_INDEX_MAX_LINES = 128;

bool _trimJsonLinesFile(const char* path, size_t keepLastLines) {
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

constexpr const char* HEADER_PROBES =
    "ts_iso,mac,ssid,is_broadcast,rssi,channel,ie_fingerprint,tag,lat,lon,alt,acc,status";
constexpr const char* HEADER_DEVICES =
    "ts_iso,mac,rssi,is_random_mac,ie_fingerprint,probe_set_hash,tag,lat,lon,alt,acc,status";
constexpr const char* HEADER_DRONES =
    "ts_iso,drone_id,mac,rssi,channel,protocol,latitude,longitude,altitude_m,tag,status";
constexpr const char* HEADER_PMKIDS =
    "ts_iso,ssid,bssid,client_mac,pmkid_hex,tag,status";

class BufferedFileWriter {
public:
    bool open(const String& path, const char* mode = "w") {
        close();
        _file = LittleFS.open(path, mode);
        _len = 0;
        return static_cast<bool>(_file);
    }

    bool write(const char* text) {
        if (!_file || !text) return false;
        size_t remaining = strlen(text);
        const char* cursor = text;
        while (remaining > 0) {
            if (_len == sizeof(_buffer) && !flush()) return false;
            size_t space = sizeof(_buffer) - _len;
            size_t chunk = remaining < space ? remaining : space;
            memcpy(_buffer + _len, cursor, chunk);
            _len += chunk;
            cursor += chunk;
            remaining -= chunk;
        }
        return true;
    }

    bool writeLine(const char* text) { return write(text) && write("\n"); }
    bool writeLine(const String& text) { return write(text.c_str()) && write("\n"); }

    bool flush() {
        if (!_file) return false;
        if (_len == 0) return true;
        const size_t written = _file.write(reinterpret_cast<const uint8_t*>(_buffer), _len);
        if (written != _len) return false;
        _len = 0;
        return true;
    }

    void close() {
        if (_file) {
            flush();
            _file.close();
        }
        _len = 0;
    }

private:
    File _file;
    char _buffer[512] = {};
    size_t _len = 0;
};

String _joinPath(const String& dir, const char* leaf) { return dir + "/" + leaf; }
bool _ensureDir(const String& path) { return LittleFS.exists(path) || LittleFS.mkdir(path); }

const char* _statusName(int status) {
    switch (static_cast<EventStatus>(status)) {
        case EVT_ENRICHED: return "enriched";
        case EVT_UPLOADED: return "uploaded";
        case EVT_RAW:
        default:           return "raw";
    }
}

String _csvEscape(const char* value) {
    String out = "\"";
    const char* src = value ? value : "";
    while (*src) {
        if (*src == '"') out += "\"\"";
        else if (*src == '\n' || *src == '\r') out += ' ';
        else out += *src;
        src++;
    }
    out += '"';
    return out;
}

String _stringField(JsonObjectConst record, const char* key) { return _csvEscape(record[key] | ""); }
String _longField(JsonObjectConst record, const char* key) { return record[key].isNull() ? String() : String(record[key].as<long>()); }
String _floatField(JsonObjectConst record, const char* key, int decimals = 6) {
    return record[key].isNull() ? String() : String(record[key].as<float>(), decimals);
}

const char* _recordTag(JsonObjectConst record) {
    const char* tag = record["tag"] | "";
    return tag[0] ? tag : (record["session_tag"] | "");
}

size_t _fileSize(const String& path) {
    File f = LittleFS.open(path, "r");
    if (!f) return 0;
    size_t size = f.size();
    f.close();
    return size;
}

void _accumulateExportArtifact(const String& path, uint16_t& fileCount, uint32_t& totalBytes) {
    if (!LittleFS.exists(path)) return;
    fileCount++;
    totalBytes += static_cast<uint32_t>(_fileSize(path));
}

bool _loadSessionRecord(const char* sessionId, JsonDocument& out) {
    if (!sessionId || !sessionId[0] || !LittleFS.exists(PATH_SESSIONS)) return false;
    File f = LittleFS.open(PATH_SESSIONS, "r");
    if (!f) return false;

    bool found = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length()) continue;
        JsonDocument lineDoc;
        if (deserializeJson(lineDoc, line)) continue;
        if (strcmp(lineDoc["id"] | "", sessionId) != 0) continue;
        out.clear();
        for (JsonPair kv : lineDoc.as<JsonObject>()) {
            out[kv.key().c_str()].set(kv.value());
        }
        found = true;
    }
    f.close();
    return found;
}

bool _removeIfNoRows(const String& path, uint32_t rowCount) {
    return !(rowCount == 0 && LittleFS.exists(path)) || LittleFS.remove(path);
}

bool _writeProbeLine(BufferedFileWriter& writer, JsonObjectConst record) {
    String line;
    line.reserve(256);
    line += _stringField(record, F_TIMESTAMP_ISO);
    line += ","; line += _stringField(record, "mac");
    line += ","; line += _stringField(record, "probed_ssid");
    line += ","; line += _longField(record, "is_broadcast");
    line += ","; line += _longField(record, "rssi");
    line += ","; line += _longField(record, "channel");
    line += ","; line += _stringField(record, "ie_fingerprint");
    line += ","; line += _csvEscape(_recordTag(record));
    line += ","; line += _floatField(record, "lat");
    line += ","; line += _floatField(record, "lon");
    line += ","; line += _floatField(record, "alt");
    line += ","; line += _floatField(record, "acc");
    line += ","; line += _csvEscape(_statusName(record["status"] | 0));
    return writer.writeLine(line);
}

bool _writeDeviceLine(BufferedFileWriter& writer, JsonObjectConst record) {
    String line;
    line.reserve(256);
    line += _stringField(record, F_TIMESTAMP_ISO);
    line += ","; line += _stringField(record, "mac");
    line += ","; line += _longField(record, "rssi");
    line += ","; line += _longField(record, "is_random_mac");
    line += ","; line += _stringField(record, "ie_fingerprint");
    line += ","; line += _stringField(record, "probe_set_hash");
    line += ","; line += _csvEscape(_recordTag(record));
    line += ","; line += _floatField(record, "lat");
    line += ","; line += _floatField(record, "lon");
    line += ","; line += _floatField(record, "alt");
    line += ","; line += _floatField(record, "acc");
    line += ","; line += _csvEscape(_statusName(record["status"] | 0));
    return writer.writeLine(line);
}

bool _writeDroneLine(BufferedFileWriter& writer, JsonObjectConst record) {
    String line;
    line.reserve(256);
    line += _stringField(record, F_TIMESTAMP_ISO);
    line += ","; line += _stringField(record, "drone_id");
    line += ","; line += _stringField(record, "mac");
    line += ","; line += _longField(record, "rssi");
    line += ","; line += _longField(record, "channel");
    line += ","; line += _stringField(record, "protocol");
    line += ","; line += _floatField(record, "latitude");
    line += ","; line += _floatField(record, "longitude");
    line += ","; line += _floatField(record, "altitude_m");
    line += ","; line += _csvEscape(_recordTag(record));
    line += ","; line += _csvEscape(_statusName(record["status"] | 0));
    return writer.writeLine(line);
}

bool _writePMKIDLine(BufferedFileWriter& csvWriter, BufferedFileWriter& hashWriter, JsonObjectConst record) {
    String line;
    line.reserve(256);
    line += _stringField(record, F_TIMESTAMP_ISO);
    line += ","; line += _stringField(record, "ssid");
    line += ","; line += _stringField(record, "bssid");
    line += ","; line += _stringField(record, "client_mac");
    line += ","; line += _stringField(record, "pmkid_hex");
    line += ","; line += _csvEscape(_recordTag(record));
    line += ","; line += _csvEscape(_statusName(record["status"] | 0));
    const char* hashcatLine = record["hashcat_line"] | "";
    return csvWriter.writeLine(line) && (!hashcatLine[0] || hashWriter.writeLine(hashcatLine));
}

bool _writeManifest(const char* sessionId,
                    const SessionExportSummary& summary,
                    const JsonDocument* sessionRecord,
                    const String& eventsPath,
                    const String& probesPath,
                    const String& devicesPath,
                    const String& dronesPath,
                    const String& pmkidsPath,
                    const String& pmkidHashcatPath) {
    JsonDocument doc;
    const RuntimeSettings& settings = SETTINGS.get();
    doc["export_version"] = 1;
    doc["session_id"] = sessionId;
    doc["generated_at"] = summary.generatedIso;
    doc["device"]["name"] = settings.deviceName;
    doc["device"]["owner"] = settings.deviceOwner;
    doc["device"]["version"] = settings.deviceVersion;
    doc["device"]["lora_freq"] = settings.loraFrequency;

    bool timeValid = false;
    char timeSource[12] = "";
    STATE_READ_BEGIN();
    timeValid = g_state.timeValid;
    strlcpy(timeSource, g_state.timeSource, sizeof(timeSource));
    STATE_READ_END();
    doc["time"]["valid"] = timeValid;
    doc["time"]["source"] = timeSource;

    if (sessionRecord) {
        JsonObject sessionOut = doc["session"].to<JsonObject>();
        for (JsonPairConst kv : sessionRecord->as<JsonObjectConst>()) sessionOut[kv.key().c_str()].set(kv.value());
        doc["session"]["active"] = false;
    } else {
        char startIso[24] = {};
        TIME_SVC.formatIsoForMillis(SESS.getStartTime(), startIso, sizeof(startIso));
        doc["session"]["id"] = SESS.getId();
        doc["session"][F_START] = SESS.getStartTime();
        doc["session"][F_START_ISO] = startIso;
        doc["session"][F_MODE] = currentSessionContextLabel();
        doc["session"]["lora"] = SESS.getLoraPackets();
        doc["session"]["wifi"] = SESS.getWifiScans();
        doc["session"]["probes"] = SESS.getProbes();
        doc["session"]["active"] = true;
    }

    doc["event_counts"]["total"] = summary.totalEvents;
    doc["event_counts"]["probe"] = summary.probeEvents;
    doc["event_counts"]["device"] = summary.deviceEvents;
    doc["event_counts"]["drone"] = summary.droneEvents;
    doc["event_counts"]["pmkid"] = summary.pmkidEvents;
    doc["event_counts"]["other"] = summary.otherEvents;
    doc["event_counts"]["pending_uploads"] = summary.pendingUploads;

    {
        JsonObject storage = doc["storage"].to<JsonObject>();
        storage["mission_events"] = summary.missionEvents;
        storage["noise_events"]   = summary.noiseEvents;

        JsonObject priority = storage["priority"].to<JsonObject>();
        priority["p0"] = summary.p0Events;
        priority["p1"] = summary.p1Events;
        priority["p2"] = summary.p2Events;
        priority["p3"] = summary.p3Events;

        JsonObject pending = storage["pending"].to<JsonObject>();
        pending["upload_mission"]  = summary.pendingUploadMission;
        pending["upload_noise"]    = summary.pendingUploadNoise;
        pending["enrich_mission"]  = summary.pendingEnrichmentMission;
        pending["enrich_noise"]    = summary.pendingEnrichmentNoise;

        storage["enrichment_deltas"] = summary.enrichmentDeltas;
        storage["first_event_id"]    = summary.firstEventId;
        storage["last_event_id"]     = summary.lastEventId;
    }

    doc["files"]["events"] = eventsPath;
    doc["files"]["events_size"] = _fileSize(eventsPath);
    if (summary.probeEvents > 0) { doc["files"]["probes"] = probesPath; doc["files"]["probes_size"] = _fileSize(probesPath); }
    if (summary.deviceEvents > 0) { doc["files"]["devices"] = devicesPath; doc["files"]["devices_size"] = _fileSize(devicesPath); }
    if (summary.droneEvents > 0) { doc["files"]["drones"] = dronesPath; doc["files"]["drones_size"] = _fileSize(dronesPath); }
    if (summary.pmkidEvents > 0) {
        doc["files"]["pmkids_csv"] = pmkidsPath;
        doc["files"]["pmkids_csv_size"] = _fileSize(pmkidsPath);
        doc["files"]["pmkid_hashcat"] = pmkidHashcatPath;
        doc["files"]["pmkid_hashcat_size"] = _fileSize(pmkidHashcatPath);
    }

    File manifestFile = LittleFS.open(summary.manifestPath, "w");
    if (!manifestFile) return false;
    const size_t written = serializeJsonPretty(doc, manifestFile);
    manifestFile.close();
    return written > 0;
}

bool _appendExportIndex(const SessionExportSummary& summary) {
    BufferedFileWriter writer;
    if (!writer.open(PATH_EXPORT_INDEX, "a")) return false;
    JsonDocument doc;
    doc["session_id"] = summary.sessionId;
    doc["generated_at"] = summary.generatedIso;
    doc["events"] = summary.totalEvents;
    doc["probe"] = summary.probeEvents;
    doc["device"] = summary.deviceEvents;
    doc["drone"] = summary.droneEvents;
    doc["pmkid"] = summary.pmkidEvents;
    doc["other"] = summary.otherEvents;
    doc["pending_uploads"] = summary.pendingUploads;
    doc["files"] = summary.exportedFiles;
    doc["bytes"] = summary.exportedBytes;
    doc["active"] = summary.activeSession;
    doc["session_dir"] = summary.sessionDir;
    doc["manifest_path"] = summary.manifestPath;

    doc["mission_events"] = summary.missionEvents;
    doc["noise_events"]   = summary.noiseEvents;

    doc["p0_events"] = summary.p0Events;
    doc["p1_events"] = summary.p1Events;
    doc["p2_events"] = summary.p2Events;
    doc["p3_events"] = summary.p3Events;

    doc["pending_upload_mission"] = summary.pendingUploadMission;
    doc["pending_upload_noise"]   = summary.pendingUploadNoise;

    doc["pending_enrich_mission"] = summary.pendingEnrichmentMission;
    doc["pending_enrich_noise"]   = summary.pendingEnrichmentNoise;

    doc["enrichment_deltas"] = summary.enrichmentDeltas;

    doc["first_event_id"] = summary.firstEventId;
    doc["last_event_id"]  = summary.lastEventId;

    String line;
    serializeJson(doc, line);
    const bool ok = writer.writeLine(line);
    writer.close();
    if (ok) {
        _trimJsonLinesFile(PATH_EXPORT_INDEX, EXPORT_INDEX_MAX_LINES);
    }
    return ok;
}

void _summaryFromJson(JsonObjectConst src, SessionExportSummary& outSummary) {
    strlcpy(outSummary.sessionId, src["session_id"] | "", sizeof(outSummary.sessionId));
    outSummary.totalEvents = src["events"] | 0U;
    outSummary.probeEvents = src["probe"] | 0U;
    outSummary.deviceEvents = src["device"] | 0U;
    outSummary.droneEvents = src["drone"] | 0U;
    outSummary.pmkidEvents = src["pmkid"] | 0U;
    outSummary.otherEvents = src["other"] | 0U;
    outSummary.pendingUploads = src["pending_uploads"] | 0U;
    outSummary.exportedFiles = src["files"] | 0U;
    outSummary.exportedBytes = src["bytes"] | 0U;
    outSummary.activeSession = src["active"] | false;
    strlcpy(outSummary.sessionDir, src["session_dir"] | "", sizeof(outSummary.sessionDir));
    strlcpy(outSummary.manifestPath, src["manifest_path"] | "", sizeof(outSummary.manifestPath));
    strlcpy(outSummary.generatedIso, src["generated_at"] | "", sizeof(outSummary.generatedIso));

    outSummary.missionEvents = src["mission_events"] | 0U;
    outSummary.noiseEvents   = src["noise_events"]   | 0U;

    outSummary.p0Events = src["p0_events"] | 0U;
    outSummary.p1Events = src["p1_events"] | 0U;
    outSummary.p2Events = src["p2_events"] | 0U;
    outSummary.p3Events = src["p3_events"] | 0U;

    outSummary.pendingUploadMission = src["pending_upload_mission"] | 0U;
    outSummary.pendingUploadNoise   = src["pending_upload_noise"]   | 0U;

    outSummary.pendingEnrichmentMission = src["pending_enrich_mission"] | 0U;
    outSummary.pendingEnrichmentNoise   = src["pending_enrich_noise"]   | 0U;

    outSummary.enrichmentDeltas = src["enrichment_deltas"] | 0U;

    outSummary.firstEventId = src["first_event_id"] | 0U;
    outSummary.lastEventId  = src["last_event_id"]  | 0U;
}
}  // namespace

bool ExportManager::begin() {
    if (_ready) return true;
    if (!STORAGE.isReady() && !LittleFS.begin(false)) {
        DLOG_ERROR("EXPORT", "LittleFS unavailable");
        return false;
    }
    _ready = _ensureDir(PATH_EXPORTS);
    if (_ready) DLOG_INFO("EXPORT", "Ready");
    return _ready;
}

bool ExportManager::exportCurrentSession(SessionExportSummary* outSummary) {
    if (!_ready && !begin()) return false;
    const String sessionId = SESS.getId();
    if (!sessionId.length()) return false;

    const String sessionDir = _joinPath(PATH_EXPORTS, sessionId.c_str());
    if (!_ensureDir(sessionDir)) return false;

    const String manifestPath = _joinPath(sessionDir, FILE_MANIFEST);
    const String eventsPath = _joinPath(sessionDir, FILE_EVENTS);
    const String probesPath = _joinPath(sessionDir, FILE_PROBES);
    const String devicesPath = _joinPath(sessionDir, FILE_DEVICES);
    const String dronesPath = _joinPath(sessionDir, FILE_DRONES);
    const String pmkidsPath = _joinPath(sessionDir, FILE_PMKIDS);
    const String pmkidHashcatPath = _joinPath(sessionDir, FILE_PMKID_HASHCAT);

    BufferedFileWriter eventsWriter, probesWriter, devicesWriter, dronesWriter, pmkidsWriter, pmkidHashWriter;
    if (!eventsWriter.open(eventsPath) || !probesWriter.open(probesPath) ||
        !devicesWriter.open(devicesPath) || !dronesWriter.open(dronesPath) ||
        !pmkidsWriter.open(pmkidsPath) || !pmkidHashWriter.open(pmkidHashcatPath)) {
        return false;
    }

    probesWriter.writeLine(HEADER_PROBES);
    devicesWriter.writeLine(HEADER_DEVICES);
    dronesWriter.writeLine(HEADER_DRONES);
    pmkidsWriter.writeLine(HEADER_PMKIDS);

    SessionExportSummary summary;
    strlcpy(summary.sessionId, sessionId.c_str(), sizeof(summary.sessionId));
    summary.pendingUploads = STORAGE.getSessionPendingEventCount();
    strlcpy(summary.sessionDir, sessionDir.c_str(), sizeof(summary.sessionDir));
    strlcpy(summary.manifestPath, manifestPath.c_str(), sizeof(summary.manifestPath));
    TIME_SVC.formatNowIso(summary.generatedIso, sizeof(summary.generatedIso));

    // Single read-model snapshot before the per-event loop.
    {
        StorageSessionSummary storageSummary;
        if (STORAGE.getSessionStorageSummary(sessionId.c_str(), storageSummary)) {
            summary.missionEvents = storageSummary.missionTotal;
            summary.noiseEvents   = storageSummary.noiseTotal;

            summary.p0Events = storageSummary.p0Total;
            summary.p1Events = storageSummary.p1Total;
            summary.p2Events = storageSummary.p2Total;
            summary.p3Events = storageSummary.p3Total;

            summary.pendingUploadMission = storageSummary.pendingUploadMission;
            summary.pendingUploadNoise   = storageSummary.pendingUploadNoise;

            summary.pendingEnrichmentMission = storageSummary.pendingEnrichmentMission;
            summary.pendingEnrichmentNoise   = storageSummary.pendingEnrichmentNoise;

            summary.enrichmentDeltas = storageSummary.enrichmentDeltas;

            summary.firstEventId = storageSummary.firstEventId;
            summary.lastEventId  = storageSummary.lastEventId;
        }
    }

    const bool ok = STORAGE.forEachEventForSession(
        sessionId.c_str(),
        [&](JsonObjectConst record) -> bool {
            String jsonLine;
            serializeJson(record, jsonLine);
            if (!eventsWriter.writeLine(jsonLine)) {
                return false;
            }

            summary.totalEvents++;
            const char* type = record["type"] | "";
            if (strcmp(type, "probe") == 0) {
                summary.probeEvents++;
                return _writeProbeLine(probesWriter, record);
            }
            if (strcmp(type, "device") == 0) {
                summary.deviceEvents++;
                return _writeDeviceLine(devicesWriter, record);
            }
            if (strcmp(type, "drone") == 0) {
                summary.droneEvents++;
                return _writeDroneLine(dronesWriter, record);
            }
            if (strcmp(type, "pmkid") == 0) {
                summary.pmkidEvents++;
                return _writePMKIDLine(pmkidsWriter, pmkidHashWriter, record);
            }

            summary.otherEvents++;
            return true;
        });

    eventsWriter.close();
    probesWriter.close();
    devicesWriter.close();
    dronesWriter.close();
    pmkidsWriter.close();
    pmkidHashWriter.close();
    if (!ok) return false;

    _removeIfNoRows(probesPath, summary.probeEvents);
    _removeIfNoRows(devicesPath, summary.deviceEvents);
    _removeIfNoRows(dronesPath, summary.droneEvents);
    _removeIfNoRows(pmkidsPath, summary.pmkidEvents);
    _removeIfNoRows(pmkidHashcatPath, summary.pmkidEvents);

    JsonDocument sessionRecord;
    JsonDocument* sessionRecordPtr = nullptr;
    if (_loadSessionRecord(sessionId.c_str(), sessionRecord)) sessionRecordPtr = &sessionRecord;
    summary.activeSession = (sessionRecordPtr == nullptr);

    if (!_writeManifest(sessionId.c_str(), summary, sessionRecordPtr, eventsPath, probesPath, devicesPath, dronesPath, pmkidsPath, pmkidHashcatPath)) {
        return false;
    }

    _accumulateExportArtifact(manifestPath, summary.exportedFiles, summary.exportedBytes);
    _accumulateExportArtifact(eventsPath, summary.exportedFiles, summary.exportedBytes);
    _accumulateExportArtifact(probesPath, summary.exportedFiles, summary.exportedBytes);
    _accumulateExportArtifact(devicesPath, summary.exportedFiles, summary.exportedBytes);
    _accumulateExportArtifact(dronesPath, summary.exportedFiles, summary.exportedBytes);
    _accumulateExportArtifact(pmkidsPath, summary.exportedFiles, summary.exportedBytes);
    _accumulateExportArtifact(pmkidHashcatPath, summary.exportedFiles, summary.exportedBytes);

    if (!_appendExportIndex(summary)) return false;

    if (outSummary) *outSummary = summary;
    return true;
}

bool ExportManager::loadLatestSummary(SessionExportSummary* outSummary) const {
    if (!outSummary || !LittleFS.exists(PATH_EXPORT_INDEX)) return false;
    File f = LittleFS.open(PATH_EXPORT_INDEX, "r");
    if (!f) return false;
    String lastLine;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length()) lastLine = line;
    }
    f.close();
    if (!lastLine.length()) return false;

    JsonDocument doc;
    if (deserializeJson(doc, lastLine)) return false;
    *outSummary = SessionExportSummary();
    _summaryFromJson(doc.as<JsonObjectConst>(), *outSummary);
    return true;
}



