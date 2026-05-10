// WiFiManager: promiscuous capture, IE fingerprinting, probe tracking,
// PMKID extraction, ASTM F3411 / DJI Remote ID, behavioral de-anonymization.
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <opendroneid.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "../core/SpectreState.h"

// OWNERSHIP CONTRACT
// - TaskHardware is the only caller that may tick or change WiFiManager mode.
// - RadioArbiter decides when WiFi capture/scan/pwny/upload modes may run.
// - Display code may only consume mirrored state and published events.

// ── WiFi operating modes ─────────────────────────────────────────────────────
typedef enum {
    WIFI_OP_IDLE,
    WIFI_OP_SCAN,
    WIFI_OP_PROMISCUOUS,
    WIFI_OP_PMKID,
    WIFI_OP_PWNY,
    WIFI_OP_CONNECT,
    WIFI_OP_CONNECTED
} WiFiOpMode;

// ── RSSI trajectory classification ───────────────────────────────────────────
enum RSSITrend : int8_t {
    RSSI_TREND_UNKNOWN     =  0,
    RSSI_TREND_APPROACHING =  1,   // slope > +0.3 dB/s — closing distance
    RSSI_TREND_RETREATING  = -1,   // slope < -0.3 dB/s — opening distance
    RSSI_TREND_STATIONARY  =  2,   // |slope| <= 0.3, low variance
    RSSI_TREND_ORBITING    =  3    // |slope| <= 0.5 but variance > 9 — circling
};

// ── Vendor behavior classification ───────────────────────────────────────────
enum VendorClass : uint8_t {
    VENDOR_UNKNOWN   = 0,
    VENDOR_APPLE     = 1,
    VENDOR_SAMSUNG   = 2,
    VENDOR_GOOGLE    = 3,
    VENDOR_MICROSOFT = 4,
    VENDOR_HUAWEI    = 5,
    VENDOR_INTEL     = 6,
    VENDOR_BROADCOM  = 7,
    VENDOR_QUALCOMM  = 8,
    VENDOR_MEDIATEK  = 9
};

// ── String helpers for enums ─────────────────────────────────────────────────
static inline const char* rssiTrendStr(int8_t t) {
    switch (t) {
        case RSSI_TREND_APPROACHING: return "APPROACHING";
        case RSSI_TREND_RETREATING:  return "RETREATING";
        case RSSI_TREND_STATIONARY:  return "STATIONARY";
        case RSSI_TREND_ORBITING:    return "ORBITING";
        default:                     return "UNKNOWN";
    }
}
static inline const char* vendorClassStr(uint8_t v) {
    switch (v) {
        case VENDOR_APPLE:     return "Apple";
        case VENDOR_SAMSUNG:   return "Samsung";
        case VENDOR_GOOGLE:    return "Google";
        case VENDOR_MICROSOFT: return "Microsoft";
        case VENDOR_HUAWEI:    return "Huawei";
        case VENDOR_INTEL:     return "Intel";
        case VENDOR_BROADCOM:  return "Broadcom";
        case VENDOR_QUALCOMM:  return "Qualcomm";
        case VENDOR_MEDIATEK:  return "MediaTek";
        default:               return "Unknown";
    }
}

// ── Captured network entry ───────────────────────────────────────────────────
struct WiFiNetwork {
    char     ssid[33];
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    bool     hasHandshake;
    bool     hasPMKID;
    bool     pmkidChecked;        // true after storage has been queried once
    char     security[12];       // WPA2 / WPA3 / OPEN etc.
    bool     isHidden;           // beacon with empty SSID
    bool     hasWPS;             // WPS IE present
    uint32_t firstSeen;          // millis() when first observed
    uint32_t lastSeen;           // millis() of last beacon
    // ── associated client tracking ───────────────────────────
    uint8_t  clientMACs[8][6];   // MACs seen talking to this AP
    uint8_t  clientRSSI[8];      // last RSSI per client
    uint32_t clientLastSeen[8];  // millis() per client
    uint8_t  clientCount;        // 0..8
};

// ── Tracked device (de-anonymization + behavioral) ───────────────────────────
struct TrackedDevice {
    // — identity —
    char     mac[18];
    uint8_t  rawMAC[6];
    char     ieFingerprint[33];  // MD5 hex of stable tagged params
    uint32_t ieOrderHash;        // hash of IE tag-ID sequence (vendor-specific)
    bool     isRandomMAC;

    // — probed SSIDs —
    char     probedSSIDs[8][33];
    uint8_t  probeCount;

    // — signal —
    int8_t   rssi;
    uint32_t firstSeen;
    uint32_t lastSeen;
    uint32_t frameCount;

    // — sequence number tracking (MAC rotation detection) —
    uint16_t lastSeqNum;
    uint8_t  physicalDeviceID;   // 0 = unlinked; same ID = same physical radio

    // — behavioral biometrics —
    uint32_t ssidBloom;          // 32-bit bloom filter of probed SSIDs
    uint16_t probeIntervalAvg;   // EMA of inter-burst interval (ms)
    uint32_t lastProbeTime;
    uint8_t  burstCount;         // probes in current burst
    uint32_t burstStartTime;

    // — RSSI trajectory —
    int8_t   rssiHistory[8];
    uint32_t rssiTimestamps[8];
    uint8_t  rssiHead;
    uint8_t  rssiCount;
    int8_t   rssiTrend;          // RSSITrend enum
    float    rssiSlope;          // dB per second

    // — vendor classification —
    uint8_t  vendorClass;        // VendorClass enum
};

// ── PMKID capture ────────────────────────────────────────────────────────────
struct PMKIDCapture {
    char     ssid[33];
    char     bssid[18];
    char     clientMAC[18];
    uint8_t  pmkid[16];
    bool     valid;
};

// ── Social-graph affinity pair ───────────────────────────────────────────────
// Two devices that probe for overlapping SSIDs (Jaccard on bloom filters)
struct AffinityPair {
    uint8_t  indexA;             // index into _devices[]
    uint8_t  indexB;
    float    jaccard;            // 0.0 – 1.0
};

// ── Karma / Evil-Twin alert ──────────────────────────────────────────────────
// Fires when a new AP beacons an SSID that was just probed by a nearby device
struct KarmaAlert {
    char     ssid[33];
    uint8_t  suspectBSSID[6];
    uint8_t  victimMAC[6];
    int8_t   beaconRSSI;
    uint32_t timestamp;
};

// ── Device graveyard (for MAC-rotation correlation) ──────────────────────────
struct DeviceGrave {
    uint8_t  rawMAC[6];
    uint16_t lastSeqNum;
    char     ieFingerprint[33];
    uint32_t ieOrderHash;
    uint32_t ssidBloom;
    uint16_t probeIntervalAvg;
    uint8_t  physicalDeviceID;
    uint32_t deathTime;          // millis() when evicted
};

// ── Recent-probe record (for Karma detection) ────────────────────────────────
struct RecentProbe {
    char     ssid[33];
    uint8_t  mac[6];
    int8_t   rssi;
    uint32_t timestamp;
};

// ── Pwny mode target scoring ─────────────────────────────────────────────────
struct PwnyTarget {
    uint8_t  networkIdx;
    int16_t  score;
    uint8_t  bestClientIdx;
    uint8_t  attackCount;
    // EAPOL message tracking — bitmask: bit0=msg1 bit1=msg2 bit2=msg3 bit3=msg4
    uint8_t  eapolMsgMask;
    uint8_t  eapolMsgsSeen;      // total count
    bool     pmkidCaptured;
    bool     complete;
    bool     crackable;          // has msgs needed for hashcat
    uint8_t  phase;              // 0=passive 1=deauth 2=cooldown 3=done
    uint32_t cooldownUntil;
    uint32_t lastAttackMs;
    uint32_t attackWindowEnd;
    uint32_t passiveWindowEnd;   // passive listen before escalating
    // Temporal activity — 24-bit hourly bitmap
    uint32_t activityBitmap;     // bit N = clients seen during hour N
    uint32_t firstSeenMs;
};

static const int PWNY_MAX_TARGETS = 8;

// ── Capacity constants ───────────────────────────────────────────────────────
static const int WIFI_MAX_NETWORKS       = 16;
static const int WIFI_MAX_DEVICES        = 32;
static const int WIFI_MAX_PMKIDS         = 8;
static const int WIFI_MAX_AFFINITY       = 8;
static const int WIFI_MAX_KARMA          = 4;
static const int WIFI_GRAVE_SIZE         = 8;
static const int WIFI_RECENT_PROBE_COUNT = 16;

class WiFiManager {
public:
    // ── lifecycle ────────────────────────────────────────────────────────────
    void begin();
    bool setExternalAntenna(bool external);
    void tick();                                 // call every ~100 ms from TaskHardware

    // ── mode control ─────────────────────────────────────────────────────────
    bool startPromiscuous();
    bool startScan();
    void pauseRadio();
    void stopAll();
    void suspendRadio();
    bool prepareStationForUpload();
    bool startPMKIDHunt(const char* targetBSSID);
    bool startPwnyMode();
    void stopPwnyMode();
    bool forcePwnyDeauth();
    bool isPwnyActive() const { return _mode == WIFI_OP_PWNY; }
    int  getPwnyTargetCount() const { return _pwnyTargetCount; }
    const PwnyTarget* getPwnyTargets() const { return _pwnyTargets; }
    const char* getPwnyStatusText() const { return _pwnyStatusText; }

    // ── channel ──────────────────────────────────────────────────────────────
    void    setChannel(uint8_t ch);
    uint8_t getCurrentChannel() { return _channel; }

    // ── data access (base) ───────────────────────────────────────────────────
    int          getNetworkCount()  { return _networkCount; }
    int          getDeviceCount()   { return _deviceCount; }
    int          getProbeCount()    { return _probePacketCount; }
    int          getPMKIDCount()    { return _pmkidCount; }
    WiFiOpMode   getMode()          { return _mode; }
    WiFiNetwork*   getNetworks()    { return _networks; }
    TrackedDevice* getDevices()     { return _devices; }
    PMKIDCapture*  getPMKIDs()      { return _pmkids; }

    // ── drone detection ──────────────────────────────────────────────────────
    bool        droneDetected()     { return _droneDetected; }
    void        clearDroneAlert()   { _droneDetected = false; }
    const char* getLastDroneID()    { return _lastDroneID; }
    float       getLastDroneLat()   { return _lastDroneLat; }
    float       getLastDroneLon()   { return _lastDroneLon; }
    bool        isAllocated() {
        return _networks && _devices &&
               _pmkids && _deferredQueue;
    }
    float       getLastDroneAlt()   { return _lastDroneAlt; }

    // ── novel analytics (public API) ─────────────────────────────────────────
    int              getEstimatedPhysicalDevices();
    bool             isDeauthFloodDetected()        { return _deauthFlood; }
    int              getKarmaAlertCount()            { return _karmaAlertCount; }
    const KarmaAlert* getKarmaAlerts()              { return _karmaAlerts; }
    int              getAffinityPairCount()          { return _affinityPairCount; }
    const AffinityPair* getAffinityPairs()          { return _affinityPairs; }
    const uint32_t*  getChannelActivity()            { return _channelActivity; }
    uint32_t         getTotalFrames()                { return _totalFrames; }
    uint32_t         getMgmtFrames()                 { return _mgmtFrames; }
    uint32_t         getDataFrames()                 { return _dataFrames; }
    void             resetCounters();

    // ── promiscuous callback (must be public for static dispatch) ─────────────
    void handleFrame(void* buf, wifi_promiscuous_pkt_type_t type);

private:
    // ── operating state ──────────────────────────────────────────────────────
    WiFiOpMode  _mode             = WIFI_OP_IDLE;
    uint8_t     _channel          = 1;
    uint32_t    _lastChannelHop   = 0;
    uint32_t    _channelDwellMs   = 200;

    // Deferred frame queue — callback deposits here, tick() processes
    struct DeferredFrame {
        uint8_t  payload[128];  // truncated copy
        int      len;
        int8_t   rssi;
        uint8_t  channel;
        uint8_t  frameType;
        uint8_t  frameSubtype;
    };
    static const int DEFERRED_QUEUE_SIZE = 128;
    DeferredFrame* _deferredQueue = nullptr;
    volatile int  _deferredHead = 0;
    volatile int  _deferredTail = 0;

    // ── network catalog ──────────────────────────────────────────────────────
    WiFiNetwork*  _networks       = nullptr;
    int           _networkCount   = 0;

    // ── device tracking ──────────────────────────────────────────────────────
    TrackedDevice* _devices       = nullptr;
    int           _deviceCount    = 0;
    int           _probePacketCount = 0;

    // ── PMKID ────────────────────────────────────────────────────────────────
    PMKIDCapture* _pmkids         = nullptr;
    int           _pmkidCount     = 0;
    char          _pmkidTarget[18] = "";
    uint8_t       _pmkidTargetChannel = 0;

    // ── drone detection ──────────────────────────────────────────────────────
    bool          _droneDetected  = false;
    char          _lastDroneID[32] = "";
    float         _lastDroneLat   = 0.0f;
    float         _lastDroneLon   = 0.0f;
    float         _lastDroneAlt   = 0.0f;
    uint32_t      _lastDroneTime  = 0;

    // ── frame statistics ─────────────────────────────────────────────────────
    uint32_t      _totalFrames    = 0;
    uint32_t      _mgmtFrames     = 0;
    uint32_t      _dataFrames     = 0;
    uint32_t      _deferredDrops   = 0;
    portMUX_TYPE  _deferredStatsMux = portMUX_INITIALIZER_UNLOCKED;
    uint32_t      _channelActivity[14] = {};  // frames per channel 1-14

    // ── deauth flood detection ───────────────────────────────────────────────
    uint32_t      _deauthCount       = 0;
    uint32_t      _deauthWindowStart = 0;
    bool          _deauthFlood       = false;
    static const uint32_t DEAUTH_THRESHOLD  = 20;
    static const uint32_t DEAUTH_WINDOW_MS  = 5000;

    // ── device aging ─────────────────────────────────────────────────────────
    uint32_t      _lastAgingCheck = 0;
    static const uint32_t AGING_INTERVAL_MS  = 30000;   // check every 30 s
    static const uint32_t DEVICE_TIMEOUT_MS  = 300000;  // evict after 5 min

    // ── social graph ─────────────────────────────────────────────────────────
    AffinityPair  _affinityPairs[WIFI_MAX_AFFINITY];
    int           _affinityPairCount = 0;
    uint32_t      _lastGraphUpdate   = 0;

    // ── Karma / Evil-Twin ────────────────────────────────────────────────────
    KarmaAlert    _karmaAlerts[WIFI_MAX_KARMA];
    int           _karmaAlertCount   = 0;
    bool          _karmaAlertNew     = false;
    RecentProbe   _recentProbes[WIFI_RECENT_PROBE_COUNT];
    int           _recentProbeHead   = 0;

    // ── device graveyard (MAC-rotation correlation) ──────────────────────────
    DeviceGrave   _graveyard[WIFI_GRAVE_SIZE];
    int           _graveCount        = 0;
    uint8_t       _nextPhysicalID    = 1;   // 0 = unassigned

    // ── RSSI trend update timer ──────────────────────────────────────────────
    uint32_t      _lastTrendUpdate   = 0;

    // ── channel hopping ──────────────────────────────────────────────────────
    void          _hopChannel();

    // ── frame dispatchers ────────────────────────────────────────────────────
    void          _processManagementFrame(const uint8_t* payload, int len,
                                          int8_t rssi, uint8_t channel);
    void          _processProbeRequest(const uint8_t* payload, int len,
                                       int8_t rssi, uint8_t channel);
    void          _processBeacon(const uint8_t* payload, int len,
                                  int8_t rssi, uint8_t channel);
    void          _processActionFrame(const uint8_t* payload, int len,
                                      int8_t rssi, uint8_t channel);
    void          _processEAPOL(const uint8_t* frame, int len, int8_t rssi);

    // ── IE fingerprinting ────────────────────────────────────────────────────
    void          _computeIEFingerprint(const uint8_t* taggedParams, int len,
                                         char* outHex33);

    // ── Remote ID / drone parsers ────────────────────────────────────────────
    bool          _checkASTMRemoteID(const uint8_t* payload, int len,
                                      int8_t rssi, uint8_t channel);
    bool          _checkDJIDroneID(const uint8_t* vendorPayload, int len,
                                    int8_t rssi, uint8_t channel);
    void          _parseRemoteID(const uint8_t* payload, int len,
                                  int8_t rssi, uint8_t channel);
    void          _handleDecodedDrone(ODID_UAS_Data* data,
                                       int8_t rssi, uint8_t ch);
    bool          _validateCoordinates(float lat, float lon, float alt = 0.0f);

    // ── PMKID extraction ─────────────────────────────────────────────────────
    bool          _extractPMKID(const uint8_t* eapol, int len,
                                 const uint8_t* apMAC, const uint8_t* clientMAC,
                                 const char* ssid);
    void          _writeHC22000(const char* ssid,
                                 const char* bssid,
                                 const char* clientMAC,
                                 const uint8_t* pmkid);

    // ── MAC utilities ────────────────────────────────────────────────────────
    void          _macToStr(const uint8_t* mac, char* out18);
    bool          _isRandomMAC(const uint8_t* mac);
    bool          _macsEqual(const uint8_t* a, const uint8_t* b);

    // ── device / network management ──────────────────────────────────────────
    TrackedDevice* _findOrCreateDevice(const char* mac, const uint8_t* rawMAC,
                                        const char* ieFingerprint, int8_t rssi);
    void           _addProbedSSID(TrackedDevice* dev, const char* ssid);
    WiFiNetwork*   _findOrCreateNetwork(const char* ssid, const uint8_t* bssid,
                                         int8_t rssi, uint8_t channel);
    const char*    _findSSIDByBSSID(const uint8_t* bssid);

    // ── behavioral analysis (novel) ──────────────────────────────────────────
    void           _updateBehavior(TrackedDevice* dev);
    void           _updateRSSIHistory(TrackedDevice* dev, int8_t rssi);
    void           _computeRSSITrends();         // batch update all devices
    void           _classifyVendor(TrackedDevice* dev, const uint8_t* taggedParams,
                                    int tagLen);
    uint32_t       _ssidBloomHash(const char* ssid);
    float          _bloomJaccard(uint32_t a, uint32_t b);

    // ── sequence-number correlation (novel) ──────────────────────────────────
    void           _checkGraveyard(TrackedDevice* dev);
    void           _buryDevice(int deviceIndex);

    // ── social graph (novel) ─────────────────────────────────────────────────
    void           _updateSocialGraph();

    // ── Karma detection (novel) ──────────────────────────────────────────────
    void           _recordRecentProbe(const char* ssid, const uint8_t* mac,
                                       int8_t rssi);
    void           _checkKarma(const char* ssid, const uint8_t* bssid,
                                int8_t rssi, bool isNewNetwork);

    // ── device aging ─────────────────────────────────────────────────────────
    void           _ageDevices();

    // ── state sync ───────────────────────────────────────────────────────────
    void           _syncState();
    bool           _ensureRadioReady();
    bool           _enablePromiscuousCapture(const char* tag);
    bool           _isTrustedSSID(const char* ssid) const;
    bool           _hasStoredCapture(const uint8_t* bssid) const;
    bool           _refreshStoredCaptureFlag(WiFiNetwork& net);

    bool           _radioReady = false;
    uint32_t       _nextRadioInitAttemptMs = 0;
    // ── Pwny mode ────────────────────────────────────────────────────────────
    PwnyTarget   _pwnyTargets[PWNY_MAX_TARGETS];
    uint8_t      _pwnyTargetCount    = 0;
    uint8_t      _pwnyCurrentIdx     = 0;
    uint32_t     _pwnyLastScoreMs    = 0;
    uint32_t     _pwnyLastRotateMs   = 0;
    bool         _pwnyAttacking      = false;
    char         _pwnyStatusText[48] = "IDLE";

    static const uint32_t PWNY_SCORE_INTERVAL_MS   = 10000;
    static const uint32_t PWNY_ATTACK_MIN_MS        = 3000;
    static const uint32_t PWNY_ATTACK_MAX_MS        = 8000;
    static const uint32_t PWNY_COOLDOWN_MIN_MS      = 20000;
    static const uint32_t PWNY_COOLDOWN_MAX_MS      = 60000;
    static const uint32_t PWNY_PASSIVE_WINDOW_MS    = 8000;  // passive before deauth
    static const uint32_t PWNY_PASSIVE_BUSY_MS      = 3000;  // passive on busy nets
    static const uint16_t PWNY_MAX_TX_PACKETS_PER_SEC = 200;
    uint32_t              _pwnyStartMs    = 0;
    uint16_t              _pwnyCaptures   = 0;
    uint16_t              _pwnyAttempts   = 0;
    uint32_t              _pwnyTxWindowStartMs = 0;
    uint16_t              _pwnyTxPacketCount = 0;

    void          _pwnyTick();
    void          _syncPwnyState(uint32_t now);
    void          _pwnyLoadPriorCaptures();
    bool          _pwnyIsHandshakeCrackable(uint8_t msgMask) const;
    uint8_t       _pwnyAdaptiveBurstCount(int8_t rssi) const;
    uint32_t      _pwnyAdaptivePassiveWindow(int networkIdx) const;
    void          _pwnyUpdateActivityBitmap(int networkIdx);
    int16_t       _pwnyTemporalBonus(const PwnyTarget& t) const;
    void          _pwnyRebuildTargets();
    void          _pwnySelectNext();
    void          _pwnyStartAttack(uint8_t targetIdx);
    void          _pwnyEndAttack(uint8_t targetIdx, bool success);
    int16_t       _pwnyScore(int networkIdx);
    bool          _pwnyTxRateAllowed();
    bool          _sendPwnyMgmtFrame(uint8_t frameControl,
                                     const uint8_t* sourceMAC,
                                     const uint8_t* destMAC,
                                     const uint8_t* bssid,
                                     uint8_t channel);
    bool          _sendPwnyAttackBurst(const uint8_t* apMAC,
                                       const uint8_t* clientMAC,
                                       uint8_t channel);
    void          _registerClientOnNetwork(const uint8_t* apMAC,
                                           const uint8_t* clientMAC,
                                           int8_t rssi);
    void          _disarmPwny(const char* status = "IDLE");
    bool          _pwnyActiveAttacksAllowed() const;

    bool          _pwnyManualDeauthRequested = false;
};

extern WiFiManager WIFI_MGR;




