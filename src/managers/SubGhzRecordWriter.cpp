


#include "SubGhzRecordWriter.h"

#include <cstring>
#include <stdio.h>

#include "../core/Session.h"
#include "../core/SpectreState.h"
#include "MQTTManager.h"
#include "StorageManager.h"

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
        const GPSFix gps = SESS.getGPS();
        if (gps.valid) {
            storage.enrichEvent(result.eventId,
                                gps.lat, gps.lon,
                                0.0f, gps.accuracy,
                                "");
        }
        MQTT_MGR.noteExternalQueuedRecord();
    }

    return legacyOk && eventOk;
}




