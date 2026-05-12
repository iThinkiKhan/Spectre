


#pragma once
#include <Arduino.h>
#include "config.h"

struct LoRaPacket {
    int address;
    int length;
    String payload;
    int rssi;
    int snr;
    unsigned long timestamp;
};

class LoRaManager {
public:
    bool begin();
    bool isReady();

    // TX
    bool send(String payload, int address = 0);

    // RX
    bool available();
    bool readPacket(LoRaPacket& outPacket);
    LoRaPacket getPacket();

    // Config
    bool setFrequency(float mhz);
    bool setParameters(int sf, int bw, int cr, int preamble);
    bool setSpreadingFactor(int sf);
    bool setBandwidth(int bw);
    bool setTxPower(int dbm);
    bool setAddress(int addr);
    bool setNetworkId(int id);

    // Stats
    int getLastRSSI();
    int getLastSNR();
    int getPacketCount();
    String getFirmwareVersion();

private:
    HardwareSerial* _serial = &Serial1;
    LoRaPacket _lastPacket;
    int _packetCount = 0;
    bool _ready = false;

    String _sendAT(String cmd, int timeoutMs = 500);
    bool _sendATOK(String cmd, int timeoutMs = 500);
    bool _parseRCV(const String& line, LoRaPacket& outPacket);
};




