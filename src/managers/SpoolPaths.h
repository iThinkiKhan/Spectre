
#pragma once

#include <Arduino.h>

// Spool filesystem paths.
//
// Pure string building (except binarySegmentPath, which probes LittleFS for
// the legacy .sp2 extension as a one-time fallback). Shared by StorageManager
// and future spool-adjacent extractions (SpoolRepair, UploadIndex, etc.) so
// the layout lives in one place.

inline constexpr const char* PATH_SPOOL       = "/spool";
inline constexpr const char* PATH_SPOOL_INDEX = "/spool/index.json";

namespace SpoolPaths {

// Match SpoolSegmentFormat::SPOOL_SEGMENT_BIN_V2 from StorageManager.h.
// Kept as a local constant so this header has no dependency on the god
// class.
inline constexpr uint8_t FORMAT_BIN_V2 = 2;

inline String dir()       { return String(PATH_SPOOL); }
inline String indexPath() { return String(PATH_SPOOL_INDEX); }

// JSONL segment file: /spool/seg_NNNNNN.jsonl
String segmentPath(uint32_t segmentId);

// Binary segment file. Returns /spool/seg_NNNNNN.bin if it exists, the
// legacy /spool/seg_NNNNNN.sp2 if .bin doesn't exist but .sp2 does, else
// the canonical .bin path (used as the destination for new writes).
// Performs LittleFS.exists() probes — caller pays one to two read-only
// directory-lookup costs.
String binarySegmentPath(uint32_t segmentId);

// Format-aware path resolver that trusts the recorded format byte and
// skips the LittleFS.exists probe for known-binary segments. Used on the
// upload bucket-fill path where probes inside a cache-disabled window
// have caused triple-faults.
String segmentPathForFormat(uint32_t segmentId, uint8_t format);

// Upload index file for a segment: /spool/idx_NNNNNN.uix
String uploadIndexPath(uint32_t segmentId);

}  // namespace SpoolPaths
