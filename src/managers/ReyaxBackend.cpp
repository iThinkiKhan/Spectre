


#include "ReyaxBackend.h"

#include "SettingsManager.h"
#include "../core/DebugLog.h"

bool ReyaxBackend::begin() {
    _ready = _radio.begin();
    _mode = _ready ? SubGhzMode::MONITOR : SubGhzMode::OFF;
    _fwVersion = _ready ? _radio.getFirmwareVersion() : String();

    if (SETTINGS.isReady()) {
        const RuntimeSettings& settings = SETTINGS.get();
        _profile.frequencyHz = settings.loraFrequency;
        _profile.networkId = settings.loraNetworkId;
        _profile.address = settings.loraAddress;
        _profile.spreadingFactor = settings.loraSF;
        _profile.bandwidth = settings.loraBW;
        _profile.codingRate = settings.loraCR;
        _profile.preamble = settings.loraPreamble;
        _frequencyHz = _profile.frequencyHz;
    } else {
        _profile = SubGhzRadioProfile();
        _frequencyHz = 0;
    }

    if (_ready) {
        DLOG_INFO("SUBGHZ", "Backend=%s fw=%s",
                  backendName(),
                  _fwVersion.c_str());
    } else {
        DLOG_ERROR("SUBGHZ", "Backend=%s init failed", backendName());
    }

    return _ready;
}

void ReyaxBackend::tick() {
    if (!_ready) {
        return;
    }

    const uint32_t intervalMs = _beaconIntervalMsForMode();
    if (intervalMs == 0) {
        return;
    }

    const uint32_t now = millis();
    if (_lastBeaconMs != 0 && (now - _lastBeaconMs) < intervalMs) {
        return;
    }

    const char* label = (_mode == SubGhzMode::TEST) ? "TEST" :
                        (_mode == SubGhzMode::BEACON) ? "BEACON" :
                        "DISC";
    _emitBeacon(label);
    _lastBeaconMs = now;
}

SubGhzCapabilities ReyaxBackend::capabilities() const {
    SubGhzCapabilities caps;
    caps.flags =
        SUBGHZ_CAP_RX |
        SUBGHZ_CAP_TX |
        SUBGHZ_CAP_ADDR_FILTER |
        SUBGHZ_CAP_BROADCAST |
        SUBGHZ_CAP_FREQ_CONFIG |
        SUBGHZ_CAP_PROFILE_CONFIG |
        SUBGHZ_CAP_RSSI_SNR;
    return caps;
}

bool ReyaxBackend::setMode(SubGhzMode mode) {
    if (!_ready) return false;

    if (_mode == mode) {
        return true;
    }

    switch (mode) {
        case SubGhzMode::OFF:
        case SubGhzMode::MONITOR:
        case SubGhzMode::DISCOVER:
        case SubGhzMode::BEACON:
        case SubGhzMode::MESSAGE:
        case SubGhzMode::TEST:
            _mode = mode;
            _lastBeaconMs = 0;
            DLOG_INFO("SUBGHZ", "Backend=%s mode=%s",
                      backendName(),
                      subGhzModeName(_mode));
            return true;
        default:
            return false;
    }
}

bool ReyaxBackend::available() {
    if (!_ready) return false;
    if (_mode == SubGhzMode::OFF) return false;
    return _radio.available();
}

bool ReyaxBackend::readPacket(SubGhzPacket& outPacket) {
    if (!_ready || !available()) return false;

    LoRaPacket pkt;
    if (!_radio.readPacket(pkt)) {
        return false;
    }

    outPacket.source = static_cast<uint16_t>(pkt.address);
    outPacket.destination = 0;
    outPacket.networkId = _profile.networkId;
    outPacket.localAddress = _profile.address;
    outPacket.length = static_cast<uint16_t>(pkt.length);
    outPacket.rssi = static_cast<int16_t>(pkt.rssi);
    outPacket.snr = static_cast<int16_t>(pkt.snr);
    outPacket.frequencyHz = _frequencyHz;
    outPacket.timestampMs = static_cast<uint32_t>(pkt.timestamp);
    outPacket.broadcast = (pkt.address == 0);
    outPacket.backendType = backendType();
    outPacket.mode = _mode;
    outPacket.kind =
        (pkt.payload.indexOf("PING") >= 0) ? SubGhzPacketKind::TEST :
        (pkt.address == 0 ? SubGhzPacketKind::BEACON : SubGhzPacketKind::DATA);
    strlcpy(outPacket.backendName, backendName(), sizeof(outPacket.backendName));
    strlcpy(outPacket.moduleName, moduleName(), sizeof(outPacket.moduleName));
    outPacket.spreadingFactor = _profile.spreadingFactor;
    outPacket.bandwidth = _profile.bandwidth;
    outPacket.codingRate = _profile.codingRate;
    outPacket.preamble = _profile.preamble;

    strlcpy(outPacket.payload, pkt.payload.c_str(), sizeof(outPacket.payload));

    _stats.rxPackets++;
    _stats.lastRSSI = outPacket.rssi;
    _stats.lastSNR = outPacket.snr;
    _stats.lastSource = outPacket.source;
    _stats.lastDestination = outPacket.destination;
    _stats.lastFrequencyHz = outPacket.frequencyHz;
    _stats.lastPacketMs = outPacket.timestampMs;
    return true;
}

bool ReyaxBackend::send(const char* payload, uint16_t destination) {
    if (!_ready || !payload || !payload[0]) return false;
    if (_mode == SubGhzMode::OFF) return false;

    const bool ok = _radio.send(String(payload), static_cast<int>(destination));
    if (ok) {
        _stats.txPackets++;
    } else {
        _stats.txFailed++;
    }
    return ok;
}

bool ReyaxBackend::setFrequencyHz(uint32_t hz) {
    if (!_ready || hz == 0) return false;

    const float mhz = static_cast<float>(hz) / 1000000.0f;
    if (!_radio.setFrequency(mhz)) {
        return false;
    }

    _frequencyHz = hz;
    _profile.frequencyHz = hz;
    return true;
}

bool ReyaxBackend::applyProfile(const SubGhzRadioProfile& profile) {
    if (!_ready) return false;

    bool ok = true;

    if (profile.address > 0) {
        ok &= _radio.setAddress(profile.address);
    }
    if (profile.networkId > 0) {
        ok &= _radio.setNetworkId(profile.networkId);
    }
    if (profile.frequencyHz > 0) {
        ok &= setFrequencyHz(profile.frequencyHz);
    }
    if (profile.spreadingFactor > 0 &&
        profile.bandwidth <= 9 &&
        profile.codingRate > 0 &&
        profile.preamble > 0) {
        ok &= _radio.setParameters(profile.spreadingFactor,
                                   profile.bandwidth,
                                   profile.codingRate,
                                   profile.preamble);
    }

    if (ok) {
        _profile = profile;
        _frequencyHz = profile.frequencyHz;
        if (SETTINGS.isReady()) {
            RuntimeSettings next = SETTINGS.snapshot();
            next.loraFrequency = profile.frequencyHz;
            next.loraNetworkId = profile.networkId;
            next.loraAddress = profile.address;
            next.loraSF = profile.spreadingFactor;
            next.loraBW = profile.bandwidth;
            next.loraCR = profile.codingRate;
            next.loraPreamble = profile.preamble;
            SETTINGS.apply(next);
        }
    }

    return ok;
}

uint32_t ReyaxBackend::_beaconIntervalMsForMode() const {
    switch (_mode) {
        case SubGhzMode::DISCOVER:
            return 20000UL;
        case SubGhzMode::BEACON:
            return 8000UL;
        case SubGhzMode::TEST:
            return 5000UL;
        case SubGhzMode::OFF:
        case SubGhzMode::MONITOR:
        case SubGhzMode::MESSAGE:
        default:
            return 0;
    }
}

void ReyaxBackend::_emitBeacon(const char* label) {
    if (!_ready || !label || !label[0]) {
        return;
    }

    char payload[80] = {};
    snprintf(payload, sizeof(payload),
             "SPC|%s|%u|%lu|%lu",
             label,
             static_cast<unsigned>(_profile.address),
             static_cast<unsigned long>(_profile.frequencyHz / 1000000UL),
             static_cast<unsigned long>(_beaconSeq++));

    if (!send(payload, 0)) {
        DLOG_WARN("SUBGHZ", "Auto beacon failed mode=%s", label);
    }
}




