#pragma once

#include <stddef.h>
#include <stdint.h>

#include "protocol/CompanionProtocol.h"

// CommandDispatcher
// =================
//
// Transport-agnostic dispatcher for the phone-companion COMMAND channel.
// Both BLEManager (internal BLE) and WioNrfAccessory (WIO proxy) own their
// own secure session and call dispatch() with a decrypted request frame.
// The dispatcher writes a fully-formed PhoneCommandResponseV1 + payload to
// `response`; the caller then encrypts that as-is and notifies/writes it back
// out the appropriate characteristic.
//
// Read-only in slice #2: every handler is a pure read of existing state.
// Returns false only if the response buffer is too small to even hold the
// header (a degenerate, unreachable case in practice).
class CommandDispatcher {
public:
    static bool dispatch(const uint8_t* request,
                         size_t requestLen,
                         uint8_t* response,
                         size_t responseCap,
                         size_t& responseLen);

private:
    static size_t writeResponseHeader(uint8_t opcode,
                                      uint16_t requestId,
                                      uint8_t status,
                                      size_t payloadLen,
                                      uint8_t* response,
                                      size_t responseCap);

    static int handleStatus(uint8_t* payload, size_t cap);
    static int handleHealth(uint8_t* payload, size_t cap);
    static int handleStorage(uint8_t* payload, size_t cap);
    static int handleWioStatus(uint8_t* payload, size_t cap);
    static int handleLogTail(uint8_t* payload, size_t cap);
    static int handleDashboard(uint8_t* payload, size_t cap);

public:
    // Reusable by the dashboard streamer (slice #4) — fills the snapshot
    // struct via a single state-read window.  Returns true on success.
    static bool populateDashboardSnapshot(CmdDashboardSnapshotV1& out);

private:
    // Slice #5 — safe write commands.  Each returns true on success, false
    // on any precondition failure; callers fold that into CMD_STATUS_*.
    static bool handleEnrichNow();
    static bool handleUploadNow();
    static bool handleTagSession(const uint8_t* payload, size_t len);
    static bool handleSaveLocation(const uint8_t* payload, size_t len);
    static bool handleScreenChange(const uint8_t* payload, size_t len);
    static bool handleDebriefRequest();
};
