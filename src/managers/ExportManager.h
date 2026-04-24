
#pragma once

#include <Arduino.h>

struct SessionExportSummary {
    char sessionId[20] = "";
    uint32_t totalEvents = 0;
    uint16_t probeEvents = 0;
    uint16_t deviceEvents = 0;
    uint16_t droneEvents = 0;
    uint16_t pmkidEvents = 0;
    uint16_t otherEvents = 0;
    uint16_t exportedFiles = 0;
    uint32_t exportedBytes = 0;
    uint32_t pendingUploads = 0;
    bool activeSession = false;
    char sessionDir[64] = "";
    char manifestPath[80] = "";
    char generatedIso[24] = "";
};

class ExportManager {
public:
    static ExportManager& getInstance() {
        static ExportManager instance;
        return instance;
    }

    bool begin();
    bool isReady() const { return _ready; }
    bool exportCurrentSession(SessionExportSummary* outSummary = nullptr);
    bool loadLatestSummary(SessionExportSummary* outSummary) const;

private:
    ExportManager() = default;
    bool _ready = false;
};

#define EXPORT_MGR ExportManager::getInstance()


