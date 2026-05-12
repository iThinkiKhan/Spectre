


#pragma once

#include <Arduino.h>

struct SessionExportSummary {
    char sessionId[40] = "";
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

    // Storage read-model snapshot at export time
    uint32_t missionEvents = 0;
    uint32_t noiseEvents = 0;

    uint32_t p0Events = 0;
    uint32_t p1Events = 0;
    uint32_t p2Events = 0;
    uint32_t p3Events = 0;

    uint32_t pendingUploadMission = 0;
    uint32_t pendingUploadNoise = 0;

    uint32_t pendingEnrichmentMission = 0;
    uint32_t pendingEnrichmentNoise = 0;

    uint32_t enrichmentDeltas = 0;

    uint32_t firstEventId = 0;
    uint32_t lastEventId = 0;
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



