
#include "KnownLocationsStore.h"

#include <LittleFS.h>
#include <ArduinoJson.h>

#include "../core/DebugLog.h"
#include "../data/Schema.h"
#include "StorageFsUtil.h"

namespace {

int loadFromPath(const char* path,
                 bool isVaultFormat,
                 SpectreState::KnownLocation* out,
                 int maxCount) {
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
        strlcpy(out[count].tag, o["tag"] | "", sizeof(out[count].tag));
        out[count].lat     = o["lat"] | 0.0f;
        out[count].lon     = o["lon"] | 0.0f;
        out[count].radiusM = o["r"]   | 50.0f;
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
}

}  // namespace

namespace KnownLocationsStore {

void save(const SpectreState::KnownLocation* locs, int count) {
    StorageFsUtil::ensureDir(PATH_STORE_CONFIG_DIR);
    StorageFsUtil::ensureDir(PATH_STORE_VAULT_DIR);
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

int load(SpectreState::KnownLocation* out, int maxCount) {
    if (!out || maxCount <= 0) return 0;

    int count = loadFromPath(PATH_STORE_KNOWN_LOCATIONS, true, out, maxCount);
    if (count > 0) {
        return count;
    }

    count = loadFromPath(PATH_STORE_LEGACY_KNOWN_LOCATIONS, false, out, maxCount);
    if (count > 0) {
        save(out, count);
        DLOG_INFO("STORAGE", "Known locations migrated legacy->vault count=%d", count);
        return count;
    }

    return 0;
}

}  // namespace KnownLocationsStore
