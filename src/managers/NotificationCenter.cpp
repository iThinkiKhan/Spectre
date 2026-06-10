#include "NotificationCenter.h"

#include <string.h>

#include "core/DebugLog.h"
#include "core/NotifTypes.h"
#include "PhoneTransportRouter.h"

namespace {
constexpr const char* TAG = "NOTIF";
}

NotificationCenter NOTIF_CENTER;

void NotificationCenter::begin() {
    if (_begun) return;
    portENTER_CRITICAL(&_mux);
    _begun = true;
    _head = 0;
    _depth = 0;
    _nextSeq = 1;
    memset(_lastEmittedMs, 0, sizeof(_lastEmittedMs));
    portEXIT_CRITICAL(&_mux);
}

uint8_t NotificationCenter::_severityForType(uint8_t type) {
    switch (type) {
        case NOTIF_DRONE:      return PHONE_NOTIF_SEVERITY_WARN;
        case NOTIF_PMKID:      return PHONE_NOTIF_SEVERITY_WARN;
        case NOTIF_HANDSHAKE:  return PHONE_NOTIF_SEVERITY_WARN;
        case NOTIF_DEAUTH:     return PHONE_NOTIF_SEVERITY_INFO;
        case NOTIF_DEVICE_NEW: return PHONE_NOTIF_SEVERITY_INFO;
        case NOTIF_HOMELAB_SYNC:
        case NOTIF_EXPORT:     return PHONE_NOTIF_SEVERITY_INFO;
        case NOTIF_STORAGE:    return PHONE_NOTIF_SEVERITY_WARN;
        case NOTIF_POWER:      return PHONE_NOTIF_SEVERITY_CRITICAL;
        default:               return PHONE_NOTIF_SEVERITY_INFO;
    }
}

uint32_t NotificationCenter::_throttleMsForType(uint8_t type) {
    switch (type) {
        case NOTIF_DRONE:        return 5000UL;
        case NOTIF_PMKID:        return 2000UL;
        case NOTIF_HANDSHAKE:    return 2000UL;
        case NOTIF_DEAUTH:       return 5000UL;
        case NOTIF_DEVICE_NEW:   return 1000UL;
        case NOTIF_HOMELAB_SYNC: return 30000UL;
        case NOTIF_EXPORT:       return 5000UL;
        case NOTIF_STORAGE:      return 30000UL;
        case NOTIF_POWER:        return 30000UL;
        default:                 return 1000UL;
    }
}

bool NotificationCenter::_tryCollapseLocked(uint8_t type, const char* text, uint8_t textLen) {
    // Caller holds _mux.  Walk the queue from head; if any pending entry
    // matches (type, text), bump its collapsedCount and return true so the
    // producer's call doesn't add a new slot.
    for (uint8_t i = 0; i < _depth; ++i) {
        const uint8_t idx = (_head + i) % QUEUE_DEPTH;
        Entry& e = _ring[idx];
        if (e.type != type) continue;
        if (e.textLen != textLen) continue;
        if (textLen > 0 && memcmp(e.text, text, textLen) != 0) continue;
        if (e.collapsedCount != 0xFFFF) {
            e.collapsedCount++;
        }
        return true;
    }
    return false;
}

void NotificationCenter::_popHeadLocked() {
    if (_depth == 0) return;
    _head = (_head + 1) % QUEUE_DEPTH;
    _depth--;
}

void NotificationCenter::enqueue(uint8_t type, const char* text) {
    if (!_begun || type == 0) return;
    // Clamp + measure the text payload outside the lock.
    uint8_t textLen = 0;
    char clipped[PHONE_NOTIF_TEXT_MAX] = {};
    if (text) {
        const size_t srcLen = strnlen(text, PHONE_NOTIF_TEXT_MAX);
        memcpy(clipped, text, srcLen);
        textLen = static_cast<uint8_t>(srcLen);
    }

    portENTER_CRITICAL(&_mux);
    if (_tryCollapseLocked(type, clipped, textLen)) {
        portEXIT_CRITICAL(&_mux);
        return;
    }

    if (_depth >= QUEUE_DEPTH) {
        // Drop oldest to make room.  Capture health > notification fidelity.
        _popHeadLocked();
    }

    const uint8_t writeIdx = (_head + _depth) % QUEUE_DEPTH;
    Entry& slot = _ring[writeIdx];
    slot.type           = type;
    slot.severity       = _severityForType(type);
    slot.collapsedCount = 0;
    slot.textLen        = textLen;
    if (textLen > 0) memcpy(slot.text, clipped, textLen);
    slot.text[textLen]  = '\0';
    slot.enqueuedMs     = millis();
    _depth++;
    portEXIT_CRITICAL(&_mux);
}

void NotificationCenter::tick() {
    if (!_begun) return;

    // Snapshot the head if it's eligible (past its throttle window).  Done
    // under the lock so a concurrent enqueue can't race the pop.
    Entry pending = {};
    bool   hasPending = false;
    uint16_t seq = 0;
    const uint32_t now = millis();

    portENTER_CRITICAL(&_mux);
    while (_depth > 0) {
        const Entry& head = _ring[_head];
        const uint32_t lastMs = _lastEmittedMs[head.type & 0x0F];
        const uint32_t windowMs = _throttleMsForType(head.type);
        // First-ever notification of this type emits immediately.
        if (lastMs != 0 && (now - lastMs) < windowMs) {
            // Still throttled.  Leave the entry; tick again later.
            break;
        }
        // Don't pop or update the throttle map yet — only commit those
        // mutations after the publish actually succeeds.  Otherwise a
        // failed publish silently consumes the entry AND blocks the next
        // unique notification for one whole throttle window.
        pending = head;
        seq = _nextSeq;
        hasPending = true;
        break;
    }
    portEXIT_CRITICAL(&_mux);

    if (!hasPending) return;

    PhoneNotificationV1Header hdr = {};
    hdr.version        = PHONE_NOTIF_VERSION;
    hdr.type           = pending.type;
    hdr.severity       = pending.severity;
    hdr.flags          = (pending.collapsedCount > 0) ? PHONE_NOTIF_FLAG_COLLAPSED : 0;
    hdr.seq            = seq;
    hdr.collapsedCount = pending.collapsedCount;
    hdr.deviceUptimeMs = now;
    hdr.textLen        = pending.textLen;
    memcpy(_txBuf, &hdr, sizeof(hdr));
    if (pending.textLen > 0) {
        memcpy(_txBuf + sizeof(hdr), pending.text, pending.textLen);
    }
    const size_t plainLen = sizeof(hdr) + pending.textLen;

    if (!PHONE_XPORT.publishNotification(_txBuf, plainLen)) {
        // Transport isn't ready (no session yet, no link).  Leave the head
        // entry in place — once the session establishes we'll publish it on
        // the next tick.  Don't update the throttle map either, so duplicates
        // arriving in the meantime continue to collapse onto the head entry.
        DLOG_DEBUG(TAG, "notif retry-later type=%u (no transport)",
                   static_cast<unsigned>(pending.type));
        return;
    }

    // Commit: bump seq, update throttle, pop head.
    portENTER_CRITICAL(&_mux);
    _nextSeq++;
    _lastEmittedMs[pending.type & 0x0F] = now;
    _popHeadLocked();
    portEXIT_CRITICAL(&_mux);

    DLOG_INFO(TAG,
              "notif emit type=%u sev=%u seq=%u textLen=%u collapsed=%u text=%s",
              static_cast<unsigned>(pending.type),
              static_cast<unsigned>(pending.severity),
              static_cast<unsigned>(seq),
              static_cast<unsigned>(pending.textLen),
              static_cast<unsigned>(pending.collapsedCount),
              pending.text);
}
