
#include "DedupFilter.h"

#include <algorithm>
#include <cstring>

#include "../config.h"

namespace {

// FNV-1a 64-bit. NUL byte mixed in after each field so concatenated inputs
// cannot collide ((a,bc) != (ab,c)).
constexpr uint64_t FNV1A64_OFFSET = 0xCBF29CE484222325ULL;
constexpr uint64_t FNV1A64_PRIME  = 0x100000001B3ULL;

inline uint64_t hashField(uint64_t h, const char* s) {
    if (s) {
        while (*s) {
            h ^= static_cast<uint8_t>(*s++);
            h *= FNV1A64_PRIME;
        }
    }
    h ^= 0;
    h *= FNV1A64_PRIME;
    return h;
}

inline uint64_t hashU32(uint64_t h, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        h ^= static_cast<uint8_t>(v & 0xFF);
        h *= FNV1A64_PRIME;
        v >>= 8;
    }
    return h;
}

}  // namespace

uint64_t DedupFilter::_makeDedupKey(const char* type, JsonObjectConst payload) const {
    const char* safeType = type ? type : "event";
    uint64_t h = hashField(FNV1A64_OFFSET, safeType);

    if (strcmp(safeType, "probe") == 0) {
        // Probe requests randomize the source MAC per burst, so keying on MAC
        // means real repeats from one device never collapse and dedup never
        // fires. Key on the device's IE fingerprint instead — it is stable
        // across MAC randomization — falling back to MAC only when no
        // fingerprint was computed, so fingerprint-less probes don't all
        // over-collapse onto one key. Same two-field arity as before.
        const char* fp = payload["ie_fingerprint"] | "";
        h = hashField(h, fp[0] ? fp : (payload["mac"] | ""));
        h = hashField(h, payload["probed_ssid"] | payload["ssid"] | "");
        // Channel intentionally omitted so the same probe seen on multiple
        // channels collapses to one entry under profile 1.
        return h;
    }

    if (strcmp(safeType, "device") == 0) {
        h = hashField(h, payload["mac"] | "");
        h = hashField(h, payload["probe_set_hash"] | "");
        h = hashField(h, payload["ie_fingerprint"] | "");
        return h;
    }

    if (strcmp(safeType, "drone") == 0) {
        h = hashField(h, payload["id"] | payload["drone_id"] | "");
        return h;
    }

    if (strcmp(safeType, "pmkid") == 0) {
        h = hashField(h, payload["ap"] | payload["bssid"] | "");
        h = hashField(h, payload["sta"] | payload["client"] | payload["client_mac"] | "");
        h = hashField(h, payload["pmkid_hex"] | "");
        return h;
    }

    if (strcmp(safeType, "subghz") == 0) {
        h = hashField(h, payload["source_addr"] | "");
        h = hashU32 (h, static_cast<uint32_t>(payload["frequency_hz"] | 0));
        h = hashField(h, payload["payload_hex"] | payload["payload"] | "");
        return h;
    }

    h = hashField(h, payload["mac"] | "");
    h = hashField(h, payload["ssid"] | "");
    h = hashField(h, payload["detail"] | "");
    return h;
}

void DedupFilter::trimWindows() {
    const uint32_t now = millis();

    _dedupWindow.erase(
        std::remove_if(_dedupWindow.begin(), _dedupWindow.end(),
            [now](const DedupWindowEntry& e) {
                return (now - e.lastSeenMs) > DEDUP_WINDOW_MS;
            }),
        _dedupWindow.end());

    _handshakeWindow.erase(
        std::remove_if(_handshakeWindow.begin(), _handshakeWindow.end(),
            [now](const HandshakeProgress& e) {
                return (now - e.lastUpdateMs) > HANDSHAKE_WINDOW_MS;
            }),
        _handshakeWindow.end());

    if (_dedupWindow.size() > DEDUP_WINDOW_MAX) {
        _dedupWindow.erase(_dedupWindow.begin(),
                           _dedupWindow.begin() + (_dedupWindow.size() - DEDUP_WINDOW_MAX));
    }
}

DedupVerdict DedupFilter::classifyDedupCandidate(const char* type,
                                                  JsonObjectConst payload,
                                                  StoragePriority priority) {
    DedupVerdict v;
#if DEDUP_PROFILE == DEDUP_PROFILE_OFF
    (void)type; (void)payload; (void)priority;
    return v;
#else
    // WiFiManager already rate-limits localization observations by target and
    // RSSI window. They must not enter the long inventory dedup window or a
    // continuously transmitting target would never produce spatial samples.
    if (payload["localization_sample"] | false) {
        return v;
    }

    if (strcmp(type ? type : "", "subghz") == 0) {
        return v;
    }

    if (priority <= STORAGE_PRIO_P1) {
        return v;
    }

    trimWindows();

    const uint64_t key = _makeDedupKey(type, payload);
    const uint32_t now = millis();

    for (auto& entry : _dedupWindow) {
        if (entry.key == key) {
            if ((now - entry.lastSeenMs) <= DEDUP_WINDOW_MS) {
                entry.lastSeenMs = now;
                entry.count++;
                v.suppress = true;
                v.counter.apply = true;
                v.counter.reason = "duplicate_suppressed";
                v.counter.droppedDelta = 0;
                v.counter.suppressedDelta = 1;
                v.counter.lane = (priority <= STORAGE_PRIO_P1)
                                     ? STORAGE_LANE_MISSION
                                     : STORAGE_LANE_NOISE;
                v.counter.priority = priority;
                return v;
            }

            entry.firstSeenMs = now;
            entry.lastSeenMs = now;
            entry.count = 1;
            return v;
        }
    }

    DedupWindowEntry e;
    e.key = key;
    e.firstSeenMs = now;
    e.lastSeenMs = now;
    e.count = 1;
    _dedupWindow.push_back(e);
    return v;
#endif
}

HandshakeVerdict DedupFilter::classifyHandshakeFrame(const char* apMac,
                                                      const char* staMac,
                                                      const char* ssid,
                                                      uint8_t messageNumber) {
    HandshakeVerdict v;
    if (messageNumber < 1 || messageNumber > 4) {
        v.accept = false;
        return v;
    }

    trimWindows();

    uint64_t key = hashField(FNV1A64_OFFSET, apMac);
    key = hashField(key, staMac);
    key = hashField(key, ssid);

    const uint8_t bit = static_cast<uint8_t>(1u << (messageNumber - 1));
    const uint32_t now = millis();

    for (auto& hs : _handshakeWindow) {
        if (hs.key == key) {
            hs.lastUpdateMs = now;

            if (hs.complete && (hs.frameMask & bit)) {
                v.accept = false;
                v.counter.apply = true;
                v.counter.reason = "handshake_duplicate_drop";
                v.counter.droppedDelta = 1;
                v.counter.suppressedDelta = 0;
                v.counter.lane = STORAGE_LANE_MISSION;
                v.counter.priority = STORAGE_PRIO_P1;
                return v;
            }

            if (hs.frameMask & bit) {
                v.accept = false;
                v.counter.apply = true;
                v.counter.reason = "handshake_suppressed";
                v.counter.droppedDelta = 0;
                v.counter.suppressedDelta = 1;
                v.counter.lane = STORAGE_LANE_MISSION;
                v.counter.priority = STORAGE_PRIO_P1;
                return v;
            }

            hs.frameMask |= bit;
            hs.complete = ((hs.frameMask & 0x0F) == 0x0F);
            return v;
        }
    }

    HandshakeProgress hs;
    hs.key = key;
    hs.frameMask = bit;
    hs.lastUpdateMs = now;
    hs.complete = (bit == 0x0F);
    _handshakeWindow.push_back(hs);
    return v;
}
