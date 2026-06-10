
#include "BadUsbVault.h"

#include <LittleFS.h>
#include <ArduinoJson.h>

#include "../core/DebugLog.h"
#include "../data/Schema.h"
#include "StorageFsUtil.h"

namespace {

uint16_t countScriptLines(const String& body) {
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
}

String makeDisplayName(const String& fileName) {
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
}

}  // namespace

namespace BadUsbVault {

bool ensureVault() {
    StorageFsUtil::ensureDir(PATH_STORE_CONFIG_DIR);
    StorageFsUtil::ensureDir(PATH_STORE_VAULT_DIR);
    StorageFsUtil::ensureDir(PATH_BADUSB_DIR);

    if (!LittleFS.exists(PATH_BADUSB_INDEX) && LittleFS.exists(PATH_LEGACY_BADUSB_DIR)) {
        StorageFsUtil::ensureDir(PATH_BADUSB_DIR);

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

int loadScriptIndex(BadUsbScriptInfo* out, int maxCount) {
    if (!out || maxCount <= 0) {
        return 0;
    }

    for (int i = 0; i < maxCount; ++i) {
        out[i] = {};
    }

    ensureVault();

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

                strlcpy(out[count].name,
                        script["name"] | makeDisplayName(fileName).c_str(),
                        sizeof(out[count].name));
                strlcpy(out[count].file,
                        fileName,
                        sizeof(out[count].file));
                strlcpy(out[count].desc,
                        script["desc"] | "",
                        sizeof(out[count].desc));
                out[count].lineCount =
                    static_cast<uint16_t>(script["lines"] | 0);
                if (out[count].lineCount == 0) {
                    String body;
                    if (readScript(fileName, body)) {
                        out[count].lineCount = countScriptLines(body);
                    }
                }
                out[count].valid = script["valid"] | true;
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
                strlcpy(out[count].name,
                        makeDisplayName(fileName).c_str(),
                        sizeof(out[count].name));
                strlcpy(out[count].file,
                        fileName.c_str(),
                        sizeof(out[count].file));
                strlcpy(out[count].desc,
                        "Imported script",
                        sizeof(out[count].desc));
                out[count].lineCount = countScriptLines(body);
                out[count].valid = true;
                ++count;
            }
        }

        entry = dir.openNextFile();
    }

    return count;
}

bool readScript(const char* fileName, String& outScript) {
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

bool writeScript(const char* fileName,
                 const char* scriptBody,
                 const char* displayName,
                 const char* desc) {
    if (!fileName || !fileName[0] || !scriptBody) {
        return false;
    }

    ensureVault();

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
    int count = loadScriptIndex(scripts, 16);
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
            scripts[slot].lineCount = countScriptLines(scriptText);
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

}  // namespace BadUsbVault
