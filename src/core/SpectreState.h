
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "Session.h"
#include "ScreenEnum.h"
#include "RunContext.h"
#include "../ui/Theme.h"
#include "../ui/MascotState.h"

typedef enum {
    UI_CMD_NONE = 0,
    UI_CMD_OPEN_WIFI_LIST,
    UI_CMD_CLOSE_WIFI_LIST,
    UI_CMD_SCROLL_WIFI_LIST_DOWN,
    UI_CMD_WIFI_LIST_SELECT,
    UI_CMD_WIFI_LIST_HUNT,
    UI_CMD_OPEN_MISSION_LIST,
    UI_CMD_CLOSE_MISSION_LIST,
    UI_CMD_SCROLL_MISSION_LIST_DOWN,
    UI_CMD_MISSION_LIST_SELECT,
    UI_CMD_OPEN_BADUSB_LIST,
    UI_CMD_CLOSE_BADUSB_LIST,
    UI_CMD_SCROLL_BADUSB_LIST_DOWN,
    UI_CMD_BADUSB_LIST_SELECT,
    UI_CMD_BADUSB_ARM,
    UI_CMD_BADUSB_RUN,
    UI_CMD_BADUSB_CANCEL,
    UI_CMD_OPEN_DEBRIEF,
    UI_CMD_CLOSE_DEBRIEF
} UICommand;

enum PowerSource : uint8_t {
    POWER_SOURCE_UNKNOWN = 0,
    POWER_SOURCE_BATTERY,
    POWER_SOURCE_USB
};

enum PowerState : uint8_t {
    POWER_STATE_USB = 0,
    POWER_STATE_BATTERY_NORMAL,
    POWER_STATE_BATTERY_ECONOMY,
    POWER_STATE_BATTERY_CRITICAL
};

struct SpectreState {
    int     loraRSSI        = 0;
    int     loraSNR         = 0;
    int     loraPacketCount = 0;
    int     subGhzNodeCount   = 0;
    char    loraLastPayload[64] = "--";
    bool    loraReady       = false;
    bool    loraNewPacket   = false;
    uint8_t subGhzMode      = 0;
    char    subGhzBackend[24] = "";
    char    subGhzModule[24] = "";
    uint32_t subGhzFrequencyHz = 0;

    bool     wifiConnected     = false;
    char     wifiSSID[32]      = "";
    int      wifiNetworkCount  = 0;
    uint8_t  wifiChannel       = 1;
    int      wifiOpMode        = 0;
    int      probeDeviceCount  = 0;
    int      probePacketCount  = 0;
    int      pmkidCaptured     = 0;
    char     lastProbedSSID[33] = "";
    char     lastProbedMAC[18]  = "";
    bool     wifiListActive    = false;
    int      wifiListSelected  = 0;
    int      wifiListScroll    = 0;
    bool     missionListActive = false;
    int      missionListScroll = 0;

    bool     badUsbListActive      = false;
    int      badUsbListSelected    = 0;
    int      badUsbListScroll      = 0;
    int      badUsbScriptCount     = 0;
    char     badUsbScriptName[32]  = "";
    char     badUsbScriptDesc[48]  = "";
    bool     badUsbArmed           = false;
    bool     badUsbRunning         = false;
    bool     badUsbReady           = false;
    bool     badUsbHasError        = false;
    uint16_t badUsbCountdownMs     = 0;
    uint16_t badUsbProgressLine    = 0;
    char     badUsbStatus[24]      = "IDLE";
    char     badUsbError[48]       = "";
    bool     wifiScanPending   = false;
    bool     wifiHuntRequest   = false;
    char     wifiHuntBSSID[18] = "";
    char     wifiHuntSSID[33]  = "";

    struct WiFiNetworkSnapshot {
        char    ssid[33];
        uint8_t bssid[6];
        int8_t  rssi;
        uint8_t channel;
        bool    hasPMKID;
        char    security[12];
        bool    isHidden;
        uint8_t clientCount;   // associated clients seen
    };
    static const int WIFI_SNAP_COUNT = 12;
    WiFiNetworkSnapshot wifiSnap[WIFI_SNAP_COUNT];
    int wifiSnapCount = 0;

    bool    kaliSyncAvailable   = false;
    bool    kaliSyncPending     = false;
    int     sessionFilesPending = 0;

    int     sessionNetworks     = 0;
    int     sessionDevices      = 0;
    int     sessionProbes       = 0;
    int     sessionPMKIDs       = 0;
    int     sessionDrones       = 0;

    int     droneCount          = 0;
    char    lastDroneID[32]     = "";
    bool    droneAlert          = false;

    bool     bleConnected      = false;
    char     bleDeviceName[32] = "";
    uint8_t  companionEnabled  = 0;   // 0/1
    uint8_t  companionPhone    = 0;   // 0 unknown, 1 available, 2 unavailable
    uint8_t  companionWork     = 0;   // 0 idle, 1 probing, 2 enriching
    uint32_t companionPending  = 0;
    uint32_t companionLastSeenMs = 0;

    bool     gpsAvailable      = false;
    float    gpsLat            = 0.0f;
    float    gpsLon            = 0.0f;
    float    gpsAlt            = 0.0f;
    float    gpsAccuracy       = 0.0f;
    uint32_t gpsLastFix        = 0;
    char     gpsTimeISO[24]    = "";
    bool     gpsValid          = false;

    bool     textInputPending   = false;
    char     textInputPrompt[24] = "";
    char     textInputResult[64] = "";
    bool     textInputReady     = false;
    char     wifiConnectPass[64] = "";

    bool     wgDumpTriggered   = false;

    int      battPercent       = 0;
    float    battVoltage       = 0.0f;
    uint16_t battVoltageMv     = 0;
    int16_t  battTrendMvPerMin = 0;
    uint16_t battCapacityMah   = 0;
    uint16_t battRuntimeMin    = 0;
    uint8_t  powerSource       = POWER_SOURCE_UNKNOWN;
    uint8_t  powerState        = POWER_STATE_BATTERY_NORMAL;
    bool     charging          = false;
    uint32_t criticalSinceMs   = 0;
    uint32_t criticalSleepAtMs = 0;
    bool    storageReady    = false;
    bool    storageNearlyFull = false;
    bool    storageFull       = false;
    bool    storageOverrun    = false;
    uint8_t storageMode       = 0;   // StoragePressureMode
    uint8_t storagePolicy     = 0;   // StorageRetentionPolicy
    uint16_t storageUsedPct   = 0;
    uint32_t storageFreeBytes = 0;
    uint32_t storagePending   = 0;
    uint32_t storageDropped   = 0;
    uint32_t storageDeduped   = 0;
    uint32_t storageOldestPendingMs = 0;
    bool     storageDumpAdvised = false;
    bool     storageRepairRequired = false;  // CounterTrust::RepairRequired/EmergencyOnly
    uint8_t  storageCounterTrust = 0;         // CounterTrust value
    char     storagePolicyText[20] = "NORMAL";
    char    sessionId[40]   = "";
    unsigned long uptimeMs  = 0;
    char    storageStr[32]  = "0KB";
    bool    timeValid       = false;
    char    timeSource[12]  = "none";
    char    timeISO[24]     = "";
    char    timeLocal[24]   = "";
    uint8_t generalSubGhzMode = 0;
    uint8_t runContext = RUN_CONTEXT_GENERAL;
    uint8_t activeMissionProfile = MISSION_RECON;
    uint8_t missionSelection = MISSION_RECON;
    uint8_t generalScreen = SCREEN_LORA;
    bool    exportLastOk    = false;
    uint32_t exportLastEvents = 0;
    uint16_t exportLastFiles = 0;
    uint32_t exportLastBytes = 0;
    uint32_t exportLastPending = 0;
    char    exportLastISO[24] = "";
    char    exportLastSessionId[40] = "";

    // Storage read-model mirror — refreshed periodically, never scanned directly
    bool     storageSummaryValid        = false;
    uint32_t storageSummaryUpdatedMs    = 0;

    uint32_t storageMissionTotal        = 0;
    uint32_t storageNoiseTotal          = 0;

    uint32_t storageP0Total             = 0;
    uint32_t storageP1Total             = 0;
    uint32_t storageP2Total             = 0;
    uint32_t storageP3Total             = 0;

    uint32_t storagePendingUploadMission  = 0;
    uint32_t storagePendingUploadNoise    = 0;

    uint32_t storagePendingEnrichMission  = 0;
    uint32_t storagePendingEnrichNoise    = 0;

    uint32_t storageEnrichmentDeltas    = 0;

    uint32_t storageFirstEventId        = 0;
    uint32_t storageLastEventId         = 0;

    bool     uploadActive      = false;
    bool     radioBusy         = false;
    uint16_t uploadPercent     = 0;
    uint32_t uploadPublished   = 0;
    uint32_t uploadTotal       = 0;
    char     uploadPhase[16]   = "";

    // UI control — Core 0 writes, Core 1 reads
    Screen      currentScreen   = SCREEN_LORA;
    MascotState mascotState     = MASCOT_STANDBY;
    bool        requestSleep    = false;
    uint32_t    sleepRequestSeq = 0;
    bool        sleepPresentationAcked = false;
    uint32_t    sleepRequestAtMs = 0;
    uint32_t    sleepPresentationAckMs = 0;
    uint32_t    sleepCommitAtMs = 0;
    uint32_t    sleepForceAtMs = 0;
    bool        screenChanged   = true;  // force initial draw
    bool        dataRefresh     = false;  // refresh data only, no transition
    bool        hwInitDone      = false;
    bool        debriefActive   = false;

    bool    antennaExternal = true;

    uint8_t radioOwner = 0;  // matches RadioOwner enum

    struct PwnyTargetDisplay {
        char    ssid[24]       = "";
        int16_t score          = 0;
        uint8_t clients        = 0;
        bool    complete       = false;
        bool    onCooldown     = false;
        bool    pmkid          = false;
        uint8_t eapolSeen      = 0;     // 0-4 messages captured
        uint8_t eapolMask      = 0;     // bitmask: bit0=M1 bit1=M2 bit2=M3 bit3=M4
        bool    crackable      = false; // has msgs 2+4 or 1+2
        int8_t  rssi           = -100;
        uint8_t attackCount    = 0;
        uint8_t phase          = 0;     // 0=passive 1=deauth 2=cooldown 3=done
    };
    PwnyTargetDisplay pwnyTargets[8];
    uint8_t           pwnyTargetCount  = 0;
    char              pwnyStatus[48]   = "IDLE";
    uint8_t           pwnyCurrentIdx   = 0;
    uint16_t          pwnyTotalCaptures = 0;
    uint16_t          pwnyTotalAttempts = 0;
    uint32_t          pwnySessionMs    = 0; // millis when pwny started

    char    sessionTag[32]       = "";
    bool    sessionTagSet        = false;

    struct KnownLocation {
        char  tag[24];
        float lat;
        float lon;
        float radiusM;
    };
    static const int KNOWN_LOC_COUNT = 8;
    KnownLocation knownLocations[KNOWN_LOC_COUNT];
    int           knownLocCount = 0;
};

extern SpectreState g_state;
extern portMUX_TYPE g_stateMux;

#define STATE_READ_BEGIN()  portENTER_CRITICAL(&g_stateMux)
#define STATE_READ_END()    portEXIT_CRITICAL(&g_stateMux)
#define STATE_WRITE_BEGIN() portENTER_CRITICAL(&g_stateMux)
#define STATE_WRITE_END()   portEXIT_CRITICAL(&g_stateMux)

