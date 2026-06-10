#pragma once

#include <stdint.h>

// Identity + state-of-the-art for the phone-companion transport currently in
// use.  The router (PhoneTransportRouter) is the single source of truth for
// which transport carries the phone-companion link; callers consult these
// types rather than evaluating WIO/internal-BLE flags inline.

enum class PhoneTransportKind : uint8_t {
    None        = 0,
    WioBle      = 1,
    InternalBle = 2,
};

struct PhoneTransportState {
    PhoneTransportKind kind          = PhoneTransportKind::None;
    PhoneTransportKind previous      = PhoneTransportKind::None;
    uint32_t           selectedAtMs  = 0;
    uint32_t           lastChangeMs  = 0;
    uint32_t           transitions   = 0;
};
