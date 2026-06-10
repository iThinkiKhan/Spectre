
#include "MaintenanceContinuity.h"

#include <LittleFS.h>

#include "../core/DebugLog.h"
#include "../data/Schema.h"
#include "StorageFsUtil.h"

namespace MaintenanceContinuity {

uint32_t computeCrc(const MaintenanceContinuityRecord& rec) {
    return StorageFsUtil::crc32Bytes(reinterpret_cast<const uint8_t*>(&rec),
                                     sizeof(rec) - sizeof(rec.crc));
}

bool load(MaintenanceContinuityRecord& out) {
    out = {};

    File f = LittleFS.open(PATH_STORAGE_MAINT_LOG, "r");
    if (!f) {
        return false;
    }

    MaintenanceContinuityRecord rec{};
    const size_t got = f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec));
    f.close();
    if (got != sizeof(rec) ||
        rec.magic != LOG_MAGIC ||
        rec.version != LOG_VERSION ||
        rec.size != sizeof(MaintenanceContinuityRecord) ||
        rec.crc != computeCrc(rec)) {
        DLOG_WARN("STORAGE", "Maintenance continuity log invalid size=%u",
                  static_cast<unsigned>(got));
        return false;
    }

    out = rec;
    return true;
}

bool save(MaintenanceContinuityRecord& rec) {
    if (rec.magic == 0) {
        rec.magic = LOG_MAGIC;
    }
    if (rec.version == 0) {
        rec.version = LOG_VERSION;
    }
    if (rec.size == 0) {
        rec.size = sizeof(MaintenanceContinuityRecord);
    }
    rec.crc = computeCrc(rec);

    const String tmpPath = String(PATH_STORAGE_MAINT_LOG) + ".tmp";
    if (LittleFS.exists(tmpPath) && !LittleFS.remove(tmpPath)) {
        return false;
    }

    File tmp = LittleFS.open(tmpPath, "w");
    if (!tmp) {
        return false;
    }

    const size_t wrote =
        tmp.write(reinterpret_cast<const uint8_t*>(&rec), sizeof(rec));
    tmp.flush();
    tmp.close();
    if (wrote != sizeof(rec)) {
        LittleFS.remove(tmpPath);
        return false;
    }

    if (LittleFS.exists(PATH_STORAGE_MAINT_LOG) &&
        !LittleFS.remove(PATH_STORAGE_MAINT_LOG)) {
        LittleFS.remove(tmpPath);
        return false;
    }

    if (!LittleFS.rename(tmpPath, PATH_STORAGE_MAINT_LOG)) {
        LittleFS.remove(tmpPath);
        return false;
    }
    return true;
}

}  // namespace MaintenanceContinuity
