#include "WioSx1262Backend.h"

#include <string.h>

#include "SettingsManager.h"
#include "../core/DebugLog.h"

namespace {
constexpr const char* TAG = "WIO_SX";
}

uint32_t WioSx1262Backend::_bwIndexToHz(uint8_t index) {
    // RYLR998 / SX126x bandwidth index -> Hz (RuntimeSettings.loraBW uses this
    // index so a profile shared with the Reyax backend stays meaningful).
    switch (index) {
        case 0: return 7800UL;
        case 1: return 10400UL;
        case 2: return 15600UL;
        case 3: return 20800UL;
        case 4: return 31250UL;
        case 5: return 41700UL;
        case 6: return 62500UL;
        case 7: return 125000UL;
        case 8: return 250000UL;
        case 9: return 500000UL;
        default: return 125000UL;
    }
}

bool WioSx1262Backend::_pushConfig() {
    // RuntimeSettings coding rate is 1..4 (4/5..4/8); RadioLib wants 5..8.
    const uint8_t cr = (_profile.codingRate >= 1 && _profile.codingRate <= 4)
                           ? static_cast<uint8_t>(_profile.codingRate + 4)
                           : 5;
    const uint8_t sf = (_profile.spreadingFactor >= 7 && _profile.spreadingFactor <= 12)
                           ? _profile.spreadingFactor
                           : 9;
    const uint16_t preamble = _profile.preamble ? _profile.preamble : 8;
    return _wio.subghzConfigure(_frequencyHz, _bandwidthHz, sf, cr, preamble,
                                SUBGHZ_SX1262_NATIVE_SYNC, _powerDbm);
}

bool WioSx1262Backend::begin() {
    _ready = _wio.subghzAvailable();
    if (!_ready) {
        _mode = SubGhzMode::OFF;
        DLOG_INFO(TAG, "SX1262 not present; backend idle (Reyax fallback)");
        return false;
    }

    if (SETTINGS.isReady()) {
        const RuntimeSettings& s = SETTINGS.get();
        _profile.frequencyHz = s.loraFrequency ? s.loraFrequency
                                               : SUBGHZ_SX1262_DEFAULT_FREQ_HZ;
        _profile.networkId = s.loraNetworkId;
        _profile.address = s.loraAddress;
        _profile.spreadingFactor = s.loraSF;
        _profile.bandwidth = s.loraBW;
        _profile.codingRate = s.loraCR;
        _profile.preamble = s.loraPreamble;
    } else {
        _profile = SubGhzRadioProfile();
        _profile.frequencyHz = SUBGHZ_SX1262_DEFAULT_FREQ_HZ;
    }
    _frequencyHz = _profile.frequencyHz;
    _bandwidthHz = _bwIndexToHz(_profile.bandwidth);
    _fwVersion = "WIO-SX1262";

    _pushConfig();
    // Default to a listening radio so the SubGhz monitor screen sees traffic.
    _wio.subghzSetMode(WioNrfAccessory::SUBGHZ_MODEM_RX);
    _wio.subghzSetAppOwner(WioNrfAccessory::SUBGHZ_OWNER_NATIVE);
    _mode = SubGhzMode::MONITOR;

    DLOG_INFO(TAG, "Backend=%s ready freq=%lu bw=%lu sf=%u",
              backendName(),
              static_cast<unsigned long>(_frequencyHz),
              static_cast<unsigned long>(_bandwidthHz),
              static_cast<unsigned>(_profile.spreadingFactor));
    return _ready;
}

void WioSx1262Backend::tick() {
    if (!_ready) {
        return;
    }
    // The Meshtastic client may have taken the SX1262 (reconfigured it to the
    // LongFast PHY). Stay quiet while it owns the radio; re-assert our native
    // profile + receiver the moment it hands the chip back.
    if (_wio.subghzAppOwner() == WioNrfAccessory::SUBGHZ_OWNER_MESH) {
        _yielded = true;
        return;
    }
    if (_yielded) {
        _yielded = false;
        _hasPending = false;
        _pushConfig();
        _wio.subghzSetMode(_mode == SubGhzMode::OFF
                               ? WioNrfAccessory::SUBGHZ_MODEM_STANDBY
                               : WioNrfAccessory::SUBGHZ_MODEM_RX);
        _wio.subghzSetAppOwner(WioNrfAccessory::SUBGHZ_OWNER_NATIVE);
    }

    const uint32_t intervalMs = _beaconIntervalMsForMode();
    if (intervalMs == 0) {
        return;
    }
    const uint32_t now = millis();
    if (_lastBeaconMs != 0 && (now - _lastBeaconMs) < intervalMs) {
        return;
    }
    const char* label = (_mode == SubGhzMode::TEST)   ? "TEST" :
                        (_mode == SubGhzMode::BEACON)  ? "BEACON" :
                                                         "DISC";
    _emitBeacon(label);
    _lastBeaconMs = now;
}

SubGhzCapabilities WioSx1262Backend::capabilities() const {
    SubGhzCapabilities caps;
    // Raw SX1262: no link-layer addressing (frames are broadcast), but full
    // PHY control plus RSSI/SNR.
    caps.flags =
        SUBGHZ_CAP_RX |
        SUBGHZ_CAP_TX |
        SUBGHZ_CAP_BROADCAST |
        SUBGHZ_CAP_FREQ_CONFIG |
        SUBGHZ_CAP_PROFILE_CONFIG |
        SUBGHZ_CAP_RSSI_SNR;
    return caps;
}

bool WioSx1262Backend::setMode(SubGhzMode mode) {
    if (!_ready) {
        return false;
    }
    if (_mode == mode) {
        return true;
    }
    if (mode == SubGhzMode::OFF) {
        _wio.subghzSetMode(WioNrfAccessory::SUBGHZ_MODEM_STANDBY);
    } else {
        // MONITOR/DISCOVER/BEACON/MESSAGE/TEST all need the receiver armed
        // (TX is fired on demand from send()/_emitBeacon()).
        _wio.subghzSetMode(WioNrfAccessory::SUBGHZ_MODEM_RX);
    }
    _mode = mode;
    _lastBeaconMs = 0;
    DLOG_INFO(TAG, "Backend=%s mode=%s", backendName(), subGhzModeName(_mode));
    return true;
}

bool WioSx1262Backend::available() {
    if (!_ready || _mode == SubGhzMode::OFF) {
        return false;
    }
    // Don't drain the shared RX ring while Meshtastic owns the radio — those
    // frames belong to the mesh client, not the native SubGhz path.
    if (_wio.subghzAppOwner() == WioNrfAccessory::SUBGHZ_OWNER_MESH) {
        return false;
    }
    if (_hasPending) {
        return true;
    }
    _hasPending = _wio.subghzConsumeRx(_pending);
    return _hasPending;
}

bool WioSx1262Backend::readPacket(SubGhzPacket& outPacket) {
    if (!available()) {
        return false;
    }
    _hasPending = false;

    const size_t len = _pending.len;
    outPacket.source = 0;
    outPacket.destination = 0;
    outPacket.networkId = _profile.networkId;
    outPacket.localAddress = _profile.address;
    outPacket.length = static_cast<uint16_t>(len);
    outPacket.rssi = _pending.rssi;
    outPacket.snr = _pending.snr;
    outPacket.frequencyHz = _frequencyHz;
    outPacket.timestampMs = millis();
    outPacket.broadcast = true;
    outPacket.backendType = backendType();
    outPacket.mode = _mode;
    outPacket.spreadingFactor = _profile.spreadingFactor;
    outPacket.bandwidth = _profile.bandwidth;
    outPacket.codingRate = _profile.codingRate;
    outPacket.preamble = _profile.preamble;
    strlcpy(outPacket.backendName, backendName(), sizeof(outPacket.backendName));
    strlcpy(outPacket.moduleName, moduleName(), sizeof(outPacket.moduleName));

    // Copy the raw frame as a NUL-terminated string for the text path. Binary
    // (non-text) frames still surface their length/RSSI; the Meshtastic client
    // consumes raw bytes via the bridge directly rather than through here.
    const size_t copyLen = (len < sizeof(outPacket.payload))
                               ? len
                               : (sizeof(outPacket.payload) - 1);
    memcpy(outPacket.payload, _pending.data, copyLen);
    outPacket.payload[copyLen] = '\0';
    outPacket.kind = (strstr(outPacket.payload, "PING") != nullptr)
                         ? SubGhzPacketKind::TEST
                         : SubGhzPacketKind::DATA;

    _stats.rxPackets++;
    _stats.lastRSSI = outPacket.rssi;
    _stats.lastSNR = outPacket.snr;
    _stats.lastFrequencyHz = _frequencyHz;
    _stats.lastPacketMs = outPacket.timestampMs;
    return true;
}

bool WioSx1262Backend::send(const char* payload, uint16_t destination) {
    (void)destination;  // raw SX1262 frames are broadcast; no link addressing
    if (!_ready || _mode == SubGhzMode::OFF || !payload || !payload[0]) {
        return false;
    }
    const size_t len = strlen(payload);
    const bool ok = _wio.subghzSendRaw(reinterpret_cast<const uint8_t*>(payload), len);
    if (ok) {
        _stats.txPackets++;
    } else {
        _stats.txFailed++;
    }
    return ok;
}

bool WioSx1262Backend::setFrequencyHz(uint32_t hz) {
    if (!_ready || hz == 0) {
        return false;
    }
    _frequencyHz = hz;
    _profile.frequencyHz = hz;
    return _pushConfig();
}

bool WioSx1262Backend::applyProfile(const SubGhzRadioProfile& profile) {
    if (!_ready) {
        return false;
    }
    _profile = profile;
    if (_profile.frequencyHz > 0) {
        _frequencyHz = _profile.frequencyHz;
    }
    _bandwidthHz = _bwIndexToHz(_profile.bandwidth);

    const bool ok = _pushConfig();
    if (ok && SETTINGS.isReady()) {
        RuntimeSettings next = SETTINGS.snapshot();
        next.loraFrequency = _profile.frequencyHz;
        next.loraNetworkId = _profile.networkId;
        next.loraAddress = _profile.address;
        next.loraSF = _profile.spreadingFactor;
        next.loraBW = _profile.bandwidth;
        next.loraCR = _profile.codingRate;
        next.loraPreamble = _profile.preamble;
        SETTINGS.apply(next);
    }
    return ok;
}

uint32_t WioSx1262Backend::_beaconIntervalMsForMode() const {
    switch (_mode) {
        case SubGhzMode::DISCOVER: return 20000UL;
        case SubGhzMode::BEACON:   return 8000UL;
        case SubGhzMode::TEST:     return 5000UL;
        default:                   return 0;
    }
}

void WioSx1262Backend::_emitBeacon(const char* label) {
    if (!_ready || !label || !label[0]) {
        return;
    }
    char payload[80] = {};
    snprintf(payload, sizeof(payload),
             "SPC|%s|%u|%lu|%lu",
             label,
             static_cast<unsigned>(_profile.address),
             static_cast<unsigned long>(_frequencyHz / 1000000UL),
             static_cast<unsigned long>(_beaconSeq++));
    if (!send(payload, 0)) {
        DLOG_WARN(TAG, "Auto beacon failed mode=%s", label);
    }
}
