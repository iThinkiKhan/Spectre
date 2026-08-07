#pragma once

#include <stddef.h>
#include <stdint.h>

#include "protocol/CompanionProtocol.h"

// Transport-agnostic phone command dispatcher for decrypted request frames.
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
