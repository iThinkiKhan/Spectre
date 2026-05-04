

#pragma once
#include <Arduino.h>

struct GPSFix {
    float lat       = 0.0f;
    float lon       = 0.0f;
    float accuracy  = 0.0f;
    bool  valid     = false;
    unsigned long timestamp = 0;
};

struct SessionData {
    String          id;
    unsigned long   startTime;
    int             loraPackets;
    int             wifiScans;
    int             probesCaptured;
    int             handshakes;
    GPSFix          lastGPS;
};

class Session {
public:
    static Session& getInstance() {
        static Session instance;
        return instance;
    }

    void begin();
    void newSession();
    void endSession();

    String          getId()           { return _data.id; }
    unsigned long   getStartTime()    { return _data.startTime; }
    unsigned long   getUptime()       { return millis() - _data.startTime; }
    int             getLoraPackets()  { return _data.loraPackets; }
    int             getWifiScans()    { return _data.wifiScans; }
    int             getProbes()       { return _data.probesCaptured; }
    int             getHandshakes()   { return _data.handshakes; }
    GPSFix          getGPS()          { return _data.lastGPS; }

    void incrementLoraPackets()   { _data.loraPackets++; }
    void incrementWifiScans()     { _data.wifiScans++; }
    void incrementProbes()        { _data.probesCaptured++; }
    void incrementHandshakes()    { _data.handshakes++; }
    void updateGPS(GPSFix fix)    { _data.lastGPS = fix; }

private:
    Session() {}
    SessionData _data;
    String _generateId();
};

#define SESS Session::getInstance()



