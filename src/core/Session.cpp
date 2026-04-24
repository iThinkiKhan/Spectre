
#include "Session.h"

void Session::begin() {
    _data.startTime = millis();
    _data.loraPackets = 0;
    _data.wifiScans = 0;
    _data.probesCaptured = 0;
    _data.handshakes = 0;
    _data.id = _generateId();
}

void Session::newSession() {
    _data.id = _generateId();
    _data.startTime = millis();
    _data.loraPackets = 0;
    _data.wifiScans = 0;
    _data.probesCaptured = 0;
    _data.handshakes = 0;
    _data.lastGPS = GPSFix();
}

void Session::endSession() {
}

String Session::_generateId() {
    // Simple ID from millis + random
    randomSeed(esp_random());
    return String(millis()) + "-" + String(random(0xFFFF), HEX);
}



