
#pragma once

#include <Arduino.h>

// BootInfo — tiny cross-boot summary persisted in its own NVS namespace.
//
// Tracks how many times the device has booted, why it last reset, and a
// snapshot of the previous run's capture/pending totals so the Boot Summary
// screen can show "what happened last time" even after a crash or power loss.
// Deliberately small and self-contained: one Preferences handle, a handful of
// u32 keys, no dynamic allocation.
namespace BootInfo {

struct LastSession {
    bool     valid = false;
    uint32_t records = 0;            // events captured in the prior run
    uint32_t pendUploadMission = 0;
    uint32_t pendUploadNoise = 0;
    uint32_t pendEnrichMission = 0;
    uint32_t pendEnrichNoise = 0;
    uint32_t durationSec = 0;        // prior run uptime at last snapshot
};

// Open NVS, increment the boot counter, latch the reset reason, and load the
// previous run's snapshot. Call once early in setup() (after the NVS subsystem
// is available — SETTINGS.begin() guarantees that).
void begin();

uint32_t    bootCount();
uint8_t     resetReason();           // esp_reset_reason() value latched at begin
const char* resetReasonName();
bool        resetWasCleanish();      // POWERON/SW/WAKE/EXT vs crash-like

const LastSession& lastSession();    // previous run (loaded at begin)

// Persist the current run's live totals as the "last session" snapshot for the
// next boot. NVS skips the write when values are unchanged, so this is cheap to
// call on a sparse cadence (we ride the FieldVault run-sample interval).
void snapshotCurrentSession(uint32_t records,
                            uint32_t pendUploadMission,
                            uint32_t pendUploadNoise,
                            uint32_t pendEnrichMission,
                            uint32_t pendEnrichNoise,
                            uint32_t durationSec);

}  // namespace BootInfo
