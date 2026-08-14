#pragma once

#include <Arduino.h>

#include "protocol/CompanionProtocol.h"

// Phone notification queue with throttling, duplicate collapse, and overflow drop.
class NotificationCenter {
public:
    static constexpr uint8_t QUEUE_DEPTH = 16;

    void begin();

    // Producer entry point.  Called from any task; uses portMUX for the
    // queue mutation so it's safe inside DebugLog-style critical paths.
    void enqueue(uint8_t type, const char* text);

    // Hardware-loop consumer.  At most one chunk per tick.  Drops silently
    // if no transport is ready (capture health > notification fidelity).
    void tick();

    bool isActive() const { return _begun; }

private:
    struct Entry {
        uint8_t  type;
        uint8_t  severity;
        uint16_t collapsedCount;
        char     text[PHONE_NOTIF_TEXT_MAX + 1];  // +1 for ergonomic null
        uint8_t  textLen;
        uint32_t enqueuedMs;
    };

    static uint8_t _severityForType(uint8_t type);
    static uint32_t _throttleMsForType(uint8_t type);

    bool _tryCollapseLocked(uint8_t type, const char* text, uint8_t textLen);
    void _popHeadLocked();

    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    bool         _begun = false;

    Entry    _ring[QUEUE_DEPTH] = {};
    uint8_t  _head = 0;
    uint8_t  _depth = 0;
    uint16_t _nextSeq = 1;

    // Per-type throttling state.  Indexed by notification type (1..9); slot
    // 0 unused so the type id maps directly.
    uint32_t _lastEmittedMs[16] = {};

    uint8_t _txBuf[PHONE_NOTIFICATION_FRAME_MAX] = {};
};

NotificationCenter& getNotificationCenter();
#define NOTIF_CENTER getNotificationCenter()
