
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
    const uint32_t deviceShortId =
        static_cast<uint32_t>(ESP.getEfuseMac() & 0x00FFFFFFULL);
    const uint32_t randomPart = esp_random();

    char id[40];
    snprintf(id, sizeof(id), "%06lx-%lu-%08lx",
             static_cast<unsigned long>(deviceShortId),
             static_cast<unsigned long>(millis()),
             static_cast<unsigned long>(randomPart));
    return String(id);
}

