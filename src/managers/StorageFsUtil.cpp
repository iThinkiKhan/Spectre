
#include "StorageFsUtil.h"

#include <LittleFS.h>
#include "../core/DebugLog.h"

namespace StorageFsUtil {

uint32_t crc32Bytes(const uint8_t* data, size_t len) {
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

bool ensureDir(const String& path) {
    if (!LittleFS.exists(path)) {
        return LittleFS.mkdir(path);
    }
    return true;
}

bool removePathWithRetry(const String& path) {
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

bool rmdirWithRetry(const String& path) {
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

bool removeTree(const String& path) {
    if (!path.length() || path == "/") {
        return false;
    }

    if (!LittleFS.exists(path)) {
        return true;
    }

    File node = LittleFS.open(path);
    if (!node) {
        return removePathWithRetry(path);
    }

    if (!node.isDirectory()) {
        node.close();
        return removePathWithRetry(path);
    }

    node.close();

    while (true) {
        File dir = LittleFS.open(path);
        if (!dir) {
            return LittleFS.exists(path) ? removePathWithRetry(path) : true;
        }

        File child = dir.openNextFile();
        if (!child) {
            dir.close();
            break;
        }

        String childPath = String(child.name());
        if (!childPath.startsWith("/")) {
            childPath = path;
            if (!childPath.endsWith("/")) {
                childPath += "/";
            }
            childPath += String(child.name());
        }
        child.close();
        dir.close();

        if (!removeTree(childPath)) {
            DLOG_WARN("STORAGE",
                      "Remove tree child failed parent=%s child=%s",
                      path.c_str(),
                      childPath.c_str());
            return false;
        }

        delay(0);
    }

    return rmdirWithRetry(path);
}

}  // namespace StorageFsUtil
