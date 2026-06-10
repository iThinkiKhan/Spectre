
#include "SpoolPaths.h"

#include <LittleFS.h>

namespace SpoolPaths {

String segmentPath(uint32_t segmentId) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%s/seg_%06lu.jsonl",
             PATH_SPOOL,
             static_cast<unsigned long>(segmentId));
    return String(buf);
}

String binarySegmentPath(uint32_t segmentId) {
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

String segmentPathForFormat(uint32_t segmentId, uint8_t format) {
    if (format == FORMAT_BIN_V2) {
        // Trust the recorded format — skip the LittleFS.exists() probe inside
        // binarySegmentPath. The .bin file is canonical for BIN_V2 segments,
        // and the probe is what was triple-faulting during upload bucket fill
        // (flash read inside a cache-disabled window). Callers that don't
        // know the format still go through binarySegmentPath.
        char buf[48];
        snprintf(buf, sizeof(buf), "%s/seg_%06lu.bin",
                 PATH_SPOOL,
                 static_cast<unsigned long>(segmentId));
        return String(buf);
    }
    return segmentPath(segmentId);
}

String uploadIndexPath(uint32_t segmentId) {
    char path[48];
    snprintf(path, sizeof(path), "/spool/idx_%06lu.uix",
             static_cast<unsigned long>(segmentId));
    return String(path);
}

}  // namespace SpoolPaths
