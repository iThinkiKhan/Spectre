
#pragma once

#include <Arduino.h>

// BadUSB script vault: index.json plus per-script .txt files under the
// configured vault directory. Pure LittleFS + JSON; never touches the spool,
// counters, dedup, or UI mirror.

struct BadUsbScriptInfo {
    char name[32];
    char file[40];
    char desc[48];
    uint16_t lineCount;
    bool valid;
};

namespace BadUsbVault {

// Ensures the vault directory and index.json exist. Performs a one-shot
// migration from the legacy /vault/badusb path if the new index is missing.
bool ensureVault();

// Loads up to `maxCount` script descriptors. Falls back to a directory
// scan if index.json is missing or unparseable. Returns the number loaded.
int loadScriptIndex(BadUsbScriptInfo* out, int maxCount);

// Reads a script body by file name (anchored to the vault dir). Rejects
// path-traversal attempts.
bool readScript(const char* fileName, String& outScript);

// Writes a script body and updates the index. Adds an entry if the file
// name isn't already indexed; otherwise updates the existing slot.
bool writeScript(const char* fileName,
                 const char* scriptBody,
                 const char* displayName,
                 const char* desc);

}  // namespace BadUsbVault
