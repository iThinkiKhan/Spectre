


#include "SubGhzRecordWriter.h"

#include <cstring>
#include <stdio.h>

#include "../core/Session.h"
#include "../core/SpectreState.h"
#include "MQTTManager.h"
#include "StorageManager.h"

// A phone GPS fix older than this is not trusted to locate a SubGHz packet
// inline; the record stays enrich-pending for companion timestamp backfill.
static constexpr uint32_t SUBGHZ_GPS_FRESH_MS = 20000UL;

bool SubGhzRecordWriter::logPacketRx(StorageManager& storage, const SubGhzPacket& pkt) {
    // The structured spool event below is the authoritative record. Avoid
    // duplicating every LoRa packet into legacy JSON log files, which can
    // consume LittleFS quickly during long missions.
    const bool legacyOk = true;
    SESS.incrementLoraPackets();

    char payloadHex[(sizeof(pkt.payload) * 2) + 1] = {};
    size_t payloadLen = strnlen(pkt.payload, sizeof(pkt.payload));
    for (size_t i = 0; i < payloadLen; i++) {
        snprintf(&payloadHex[i * 2], 3, "%02X", static_cast<uint8_t>(pkt.payload[i]));
    }

    char detail[96] = {};
    snprintf(detail, sizeof(detail),
             "src=%u freq=%lu kind=%s payload=%s",
             static_cast<unsigned>(pkt.source),
             static_cast<unsigned long>(pkt.frequencyHz),
             subGhzPacketKindName(pkt.kind),
             pkt.payload);

    JsonDocument doc;
    doc["sensor"] = MQTT_SENSOR_ID;
    doc["session_id"] = SESS.getId();
    STATE_READ_BEGIN();
    const bool tagSet = g_state.sessionTagSet;
    char tagBuf[32] = {};
    strlcpy(tagBuf, g_state.sessionTag, sizeof(tagBuf));
    STATE_READ_END();
    if (tagSet) {
        doc["session_tag"] = tagBuf;
    }
    doc["event_type"] = "subghz_signal";
    doc["category"] = "rf";
    doc["protocol"] = "subghz_lora";
    doc["backend"] = pkt.backendName;
    doc["module"] = pkt.moduleName;
    doc["mode"] = subGhzModeName(pkt.mode);
    doc["signal_kind"] = subGhzPacketKindName(pkt.kind);
    doc["source_addr"] = pkt.source;
    doc["destination_addr"] = pkt.destination;
    doc["network_id"] = pkt.networkId;
    doc["local_addr"] = pkt.localAddress;
    doc["packet_len"] = pkt.length;
    doc["broadcast"] = pkt.broadcast ? 1 : 0;
    doc["rssi"] = pkt.rssi;
    doc["snr"] = pkt.snr;
    doc["frequency_hz"] = pkt.frequencyHz;
    doc["sf"] = pkt.spreadingFactor;
    doc["bw"] = pkt.bandwidth;
    doc["cr"] = pkt.codingRate;
    doc["preamble"] = pkt.preamble;
    doc["payload"] = pkt.payload;
    doc["payload_hex"] = payloadHex;
    doc["detail"] = detail;

    const AppendEventResult result =
        storage.appendEventDetailed("subghz", doc.as<JsonObjectConst>());
    const bool eventOk = result.ok();

    if (eventOk) {
        // Only stamp the live fix inline when it is fresh. A stale fix would
        // mislocate this packet on the map; leaving the event enrich-pending
        // lets the companion backfill the correct location by matching this
        // record's capture timestamp against the phone's GPS track. Every
        // SubGHz packet is its own dedup-exempt observation, so the source
        // address, RSSI/SNR, frequency and per-observation location together
        // give the home database what it needs to trilaterate the node.
        const GPSFix gps = SESS.getGPS();
        const uint32_t nowMs = millis();
        const bool gpsFresh =
            gps.valid && (nowMs - gps.timestamp) <= SUBGHZ_GPS_FRESH_MS;
        if (gpsFresh) {
            storage.enrichEvent(result.eventId,
                                gps.lat, gps.lon,
                                0.0f, gps.accuracy,
                                tagSet ? tagBuf : "");
        }
        MQTT_MGR.noteExternalQueuedRecord();
    }

    return legacyOk && eventOk;
}




