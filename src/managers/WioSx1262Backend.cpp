#include "WioSx1262Backend.h"

#include "../core/DebugLog.h"

namespace {
constexpr const char* TAG = "WIO_SX";
}

bool WioSx1262Backend::begin() {
    // SubGhzManager calls begin() once and keeps the first backend that
    // reports ready=true.  The WIO accessory's CAPS handshake hasn't run
    // yet at this point, so be conservative: report not-ready and rely on
    // the existing Reyax backend.  When the WIO firmware grows real
    // SUBGHZ_* verbs we can flip this to consult _wio.hasSx1262() and
    // negotiate the SX1262 profile through the UART.
    _ready = false;
    _mode = SubGhzMode::OFF;
    _frequencyHz = _profile.frequencyHz;
    DLOG_INFO(TAG, "Stub registered (waits on WIO SUBGHZ_* verbs)");
    return _ready;
}

void WioSx1262Backend::tick() {
    if (!_ready) {
        return;
    }
    // TODO: poll WIO_NRF for sub-GHz RX packets once the UART protocol exposes
    // SUBGHZ_RX / SUBGHZ_STATS lines.
}

SubGhzCapabilities WioSx1262Backend::capabilities() const {
    SubGhzCapabilities caps;
    // Conservative until the WIO firmware confirms.  The SX1262 silicon can
    // do all of these; we'll advertise once the bridge is alive.
    caps.flags = SUBGHZ_CAP_NONE;
    return caps;
}

bool WioSx1262Backend::setMode(SubGhzMode mode) {
    _mode = mode;
    return _ready;
}

bool WioSx1262Backend::readPacket(SubGhzPacket& outPacket) {
    (void)outPacket;
    return false;
}

bool WioSx1262Backend::send(const char* payload, uint16_t destination) {
    (void)payload;
    (void)destination;
    return false;
}

bool WioSx1262Backend::setFrequencyHz(uint32_t hz) {
    _frequencyHz = hz;
    _profile.frequencyHz = hz;
    return _ready;
}

bool WioSx1262Backend::applyProfile(const SubGhzRadioProfile& profile) {
    _profile = profile;
    _frequencyHz = profile.frequencyHz;
    return _ready;
}
