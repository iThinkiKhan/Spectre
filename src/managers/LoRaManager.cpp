

#include "LoRaManager.h"
#include "SettingsManager.h"

bool LoRaManager::begin() {
    _serial->begin(115200, SERIAL_8N1, LORA_RX, LORA_TX);
    delay(100);

    if (!_sendATOK("AT")) {
        return false;
    }

    uint16_t address = SPECTRE_LORA_ADDRESS;
    uint16_t networkId = SPECTRE_LORA_NETWORK_ID;
    uint32_t frequency = SPECTRE_LORA_FREQUENCY;
    uint8_t sf = SPECTRE_LORA_SF;
    uint8_t bw = SPECTRE_LORA_BW;
    uint8_t cr = SPECTRE_LORA_CR;
    uint8_t preamble = SPECTRE_LORA_PREAMBLE;

    if (SETTINGS.isReady()) {
        const RuntimeSettings& settings = SETTINGS.get();
        address = settings.loraAddress;
        networkId = settings.loraNetworkId;
        frequency = settings.loraFrequency;
        sf = settings.loraSF;
        bw = settings.loraBW;
        cr = settings.loraCR;
        preamble = settings.loraPreamble;
    }

    _sendATOK("AT+ADDRESS=" + String(address));
    _sendATOK("AT+NETWORKID=" + String(networkId));
    _sendATOK("AT+BAND=" + String(frequency));
    setParameters(sf, bw, cr, preamble);

    _ready = true;
    return true;
}

bool LoRaManager::isReady() {
    return _ready;
}

bool LoRaManager::send(String payload, int address) {
    if (!_ready) return false;
    String cmd = "AT+SEND=" + String(address) + "," + String(payload.length()) + "," + payload;
    return _sendATOK(cmd, 2000);
}

bool LoRaManager::available() {
    return _serial->available();
}

bool LoRaManager::readPacket(LoRaPacket& outPacket) {
    if (!_serial->available()) {
        return false;
    }

    String line = _serial->readStringUntil('\n');
    line.trim();

    if (!line.startsWith("+RCV=")) {
        return false;
    }

    if (!_parseRCV(line, outPacket)) {
        return false;
    }

    _lastPacket = outPacket;
    _packetCount++;
    return true;
}

LoRaPacket LoRaManager::getPacket() {
    LoRaPacket packet = _lastPacket;
    readPacket(packet);
    return _lastPacket;
}

bool LoRaManager::setFrequency(float mhz) {
    long hz = (long)(mhz * 1000000);
    if (!_sendATOK("AT+BAND=" + String(hz))) {
        return false;
    }
    if (SETTINGS.isReady()) {
        RuntimeSettings next = SETTINGS.snapshot();
        next.loraFrequency = static_cast<uint32_t>(hz);
        SETTINGS.apply(next);
    }
    return true;
}

bool LoRaManager::setParameters(int sf, int bw, int cr, int preamble) {
    return _sendATOK("AT+PARAMETER=" + String(sf) + "," + String(bw) + "," +
                     String(cr) + "," + String(preamble));
}

bool LoRaManager::setSpreadingFactor(int sf) {
    RuntimeSettings next = SETTINGS.snapshot();
    next.loraSF = static_cast<uint8_t>(sf);
    if (!setParameters(next.loraSF, next.loraBW, next.loraCR, next.loraPreamble)) {
        return false;
    }
    if (SETTINGS.isReady()) {
        SETTINGS.apply(next);
    }
    return true;
}

bool LoRaManager::setBandwidth(int bw) {
    RuntimeSettings next = SETTINGS.snapshot();
    next.loraBW = static_cast<uint8_t>(bw);
    if (!setParameters(next.loraSF, next.loraBW, next.loraCR, next.loraPreamble)) {
        return false;
    }
    if (SETTINGS.isReady()) {
        SETTINGS.apply(next);
    }
    return true;
}

bool LoRaManager::setTxPower(int dbm) {
    return _sendATOK("AT+CRFOP=" + String(dbm));
}

bool LoRaManager::setAddress(int addr) {
    if (!_sendATOK("AT+ADDRESS=" + String(addr))) {
        return false;
    }
    if (SETTINGS.isReady()) {
        RuntimeSettings next = SETTINGS.snapshot();
        next.loraAddress = static_cast<uint16_t>(addr);
        SETTINGS.apply(next);
    }
    return true;
}

bool LoRaManager::setNetworkId(int id) {
    if (!_sendATOK("AT+NETWORKID=" + String(id))) {
        return false;
    }
    if (SETTINGS.isReady()) {
        RuntimeSettings next = SETTINGS.snapshot();
        next.loraNetworkId = static_cast<uint16_t>(id);
        SETTINGS.apply(next);
    }
    return true;
}

int LoRaManager::getLastRSSI() {
    return _lastPacket.rssi;
}

int LoRaManager::getLastSNR() {
    return _lastPacket.snr;
}

int LoRaManager::getPacketCount() {
    return _packetCount;
}

String LoRaManager::getFirmwareVersion() {
    return _sendAT("AT+VER?");
}

String LoRaManager::_sendAT(String cmd, int timeoutMs) {
    while (_serial->available()) _serial->read();
    _serial->println(cmd);
    unsigned long start = millis();
    String response = "";
    while (millis() - start < timeoutMs) {
        while (_serial->available()) {
            char c = _serial->read();
            response += c;
        }
        if (response.length() && millis() - start > 50) break;
    }
    response.trim();
    return response;
}

bool LoRaManager::_sendATOK(String cmd, int timeoutMs) {
    String resp = _sendAT(cmd, timeoutMs);
    return resp.indexOf("+OK") >= 0;
}

bool LoRaManager::_parseRCV(const String& line, LoRaPacket& outPacket) {
    // Format: +RCV=address,length,payload,rssi,snr
    const String body = line.substring(5);
    const int p1 = body.indexOf(',');
    const int p2 = body.indexOf(',', p1 + 1);
    const int p4 = body.lastIndexOf(',');
    const int p3 = (p4 >= 0) ? body.lastIndexOf(',', p4 - 1) : -1;

    if (p1 < 0 || p2 < 0 || p3 < 0 || p4 < 0 || p3 <= p2) {
        return false;
    }

    outPacket.address   = body.substring(0, p1).toInt();
    outPacket.length    = body.substring(p1 + 1, p2).toInt();
    outPacket.payload   = body.substring(p2 + 1, p3);
    outPacket.rssi      = body.substring(p3 + 1, p4).toInt();
    outPacket.snr       = body.substring(p4 + 1).toInt();
    outPacket.timestamp = millis();
    return true;
}



