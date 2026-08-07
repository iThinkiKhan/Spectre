#pragma once

#include <Arduino.h>

enum StorageWindowKind : uint8_t {
    STORAGE_WINDOW_UPLOAD = 0,
    STORAGE_WINDOW_MAINTENANCE,
    STORAGE_WINDOW_EXPORT,
    STORAGE_WINDOW_FIELDVAULT_UPLOAD,
    STORAGE_WINDOW_COMPACTION,
    STORAGE_WINDOW_ENRICHMENT
};

class StorageExclusiveWindow {
public:
    bool begin(StorageWindowKind kind, const char* reason);
    void end(const char* result);

    bool active() const { return _active; }
    bool displaySuspended() const { return _displaySuspended; }
    bool workerPaused() const { return _workerPaused; }
    StorageWindowKind kind() const { return _kind; }

private:
    const char* _kindName(StorageWindowKind kind) const;
    bool _ownerMatches(StorageWindowKind kind) const;

    bool _active = false;
    bool _workerPaused = false;
    bool _displaySuspended = false;
    StorageWindowKind _kind = STORAGE_WINDOW_UPLOAD;
    char _reason[40] = {};
};
