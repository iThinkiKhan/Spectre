
#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

// Shared LittleFS helpers used by StorageManager and the various small
// persistence modules (MaintenanceContinuity, KnownLocationsStore,
// BadUsbVault). Pure stateless functions — no dependency on StorageManager.

namespace StorageFsUtil {

// CRC32 / polynomial 0xEDB88320, init 0xFFFFFFFF, output complement.
// Used for on-disk record integrity (UploadIndex, MaintenanceContinuity).
uint32_t crc32Bytes(const uint8_t* data, size_t len);

// Creates `path` as a directory if it does not exist. Returns true if the
// directory exists after the call.
bool ensureDir(const String& path);

// Removes a file at `path`, retrying up to 4 times with a 2 ms delay
// between attempts. Logs "Remove recovered" on attempt > 1, "Remove
// failed" on permanent failure.
bool removePathWithRetry(const String& path);

// Removes an empty directory at `path` with the same retry shape as
// removePathWithRetry.
bool rmdirWithRetry(const String& path);

// Recursively removes `path`. Returns true if the path is gone (or never
// existed). Returns false for the root path "/" as a safety guard. Uses
// removePathWithRetry for files and rmdirWithRetry for directories.
bool removeTree(const String& path);

}  // namespace StorageFsUtil
