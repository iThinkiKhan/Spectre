#include "MeshtasticManager.h"

#include <string.h>

#include <esp_mac.h>
#include <esp_random.h>
#include <mbedtls/aes.h>

#include "MeshtasticProto.h"
#include "../core/DebugLog.h"

namespace {
constexpr const char* TAG = "MESH";

// Meshtastic default public-channel AES key (the key that the "AQ==" / index-1
// PSK expands to). 16 bytes -> AES-128.
constexpr uint8_t DEFAULT_KEY[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

// Channel name used to derive the channel hash for the default primary channel
// (Meshtastic uses the modem-preset name when the channel name is empty).
constexpr const char* CHANNEL_NAME = "LongFast";

// Meshtastic LoRa header (PacketHeader): to/from/id (4 each LE), flags,
// channel-hash, next_hop, relay_node.
constexpr size_t HEADER_SIZE = 16;

uint32_t rdU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void wrU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}
}  // namespace

bool MeshtasticManager::begin() {
    if (_begun) {
        return true;
    }
    _begun = true;
    memcpy(_channelKey, DEFAULT_KEY, sizeof(_channelKey));
    _computeChannelHash();
    _myNodeNum = _deriveNodeNum();
    DLOG_INFO(TAG, "Meshtastic client ready node=0x%08lX chanHash=0x%02X (disabled)",
              static_cast<unsigned long>(_myNodeNum),
              static_cast<unsigned>(_channelHash));
    return true;
}

bool MeshtasticManager::isAvailable() const {
    return _wio && _wio->subghzAvailable();
}

void MeshtasticManager::_computeChannelHash() {
    const uint8_t nameHash =
        meshproto::xorHash(reinterpret_cast<const uint8_t*>(CHANNEL_NAME),
                           strlen(CHANNEL_NAME));
    const uint8_t keyHash = meshproto::xorHash(_channelKey, sizeof(_channelKey));
    _channelHash = nameHash ^ keyHash;
}

uint32_t MeshtasticManager::_deriveNodeNum() const {
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        return 0x0BADF00DUL;
    }
    // Mirror Meshtastic: node number is the low 4 bytes of the MAC.
    return (static_cast<uint32_t>(mac[2]) << 24) |
           (static_cast<uint32_t>(mac[3]) << 16) |
           (static_cast<uint32_t>(mac[4]) << 8) |
           static_cast<uint32_t>(mac[5]);
}

uint32_t MeshtasticManager::_nextPacketId() {
    uint32_t id = esp_random();
    if (id == 0) {
        id = ++_txPacketId ? _txPacketId : 1;
    }
    return id;
}

void MeshtasticManager::_initNonce(uint8_t nonce[16], uint32_t fromNode,
                                   uint32_t packetId) const {
    memset(nonce, 0, 16);
    // nonce = packetId (64-bit LE, high word 0) ++ fromNode (LE) ++ counter(0).
    wrU32(&nonce[0], packetId);
    wrU32(&nonce[8], fromNode);
}

bool MeshtasticManager::_crypt(const uint8_t* in, uint8_t* out, size_t len,
                               uint32_t fromNode, uint32_t packetId) const {
    uint8_t nonce[16];
    _initNonce(nonce, fromNode, packetId);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, _channelKey, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return false;
    }
    size_t nc_off = 0;
    uint8_t stream_block[16] = {};
    // AES-CTR is symmetric — same call encrypts and decrypts.
    const int rc =
        mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonce, stream_block, in, out);
    mbedtls_aes_free(&ctx);
    return rc == 0;
}

bool MeshtasticManager::enable() {
    if (!isAvailable()) {
        DLOG_WARN(TAG, "Cannot enable: SX1262 not present");
        return false;
    }
    if (_enabled) {
        return true;
    }
    _wio->subghzSetAppOwner(WioNrfAccessory::SUBGHZ_OWNER_MESH);
    _wio->subghzConfigure(MESHTASTIC_FREQ_HZ, MESHTASTIC_BW_HZ, MESHTASTIC_SF,
                          MESHTASTIC_CR, MESHTASTIC_PREAMBLE, MESHTASTIC_SYNC_WORD,
                          MESHTASTIC_TX_POWER_DBM);
    _wio->subghzSetMode(WioNrfAccessory::SUBGHZ_MODEM_RX);
    _enabled = true;
    DLOG_INFO(TAG, "Meshtastic enabled (US915 LongFast, freq=%lu)",
              static_cast<unsigned long>(MESHTASTIC_FREQ_HZ));
    return true;
}

void MeshtasticManager::disable() {
    if (!_enabled) {
        return;
    }
    _enabled = false;
    // Hand the radio back; the native SubGhz backend re-asserts its profile when
    // it sees ownership is no longer MESH.
    if (_wio) {
        _wio->subghzSetAppOwner(WioNrfAccessory::SUBGHZ_OWNER_NATIVE);
    }
    DLOG_INFO(TAG, "Meshtastic disabled");
}

void MeshtasticManager::tick() {
    if (!_enabled || !_wio) {
        return;
    }
    WioNrfAccessory::SubGhzRxFrame f;
    while (_wio->subghzConsumeRx(f)) {
        _handleFrame(f.data, f.len, f.rssi, f.snr);
    }
}

void MeshtasticManager::_handleFrame(const uint8_t* data, size_t len, int rssi,
                                     int snr) {
    if (len <= HEADER_SIZE) {
        return;
    }
    ++_rxFrames;

    const uint32_t to = rdU32(&data[0]);
    const uint32_t from = rdU32(&data[4]);
    const uint32_t packetId = rdU32(&data[8]);
    const uint8_t flags = data[12];
    const uint8_t channel = data[13];
    (void)to;

    // Different channel (or a hash collision we can't key) — ignore.
    if (channel != _channelHash) {
        return;
    }

    const uint8_t hopLimit = flags & 0x07;
    const uint8_t hopStart = (flags >> 5) & 0x07;
    const uint8_t hopsAway = (hopStart >= hopLimit) ? (hopStart - hopLimit) : 0;

    const uint8_t* cipher = data + HEADER_SIZE;
    const size_t cipherLen = len - HEADER_SIZE;
    uint8_t plain[256] = {};
    if (cipherLen > sizeof(plain)) {
        return;
    }
    if (!_crypt(cipher, plain, cipherLen, from, packetId)) {
        ++_rxDecryptFail;
        return;
    }
    _handleDecoded(from, plain, cipherLen, rssi, snr, hopsAway);
}

void MeshtasticManager::_handleDecoded(uint32_t from, const uint8_t* plain,
                                       size_t len, int rssi, int snr,
                                       uint8_t hopsAway) {
    // Decrypted bytes are a meshtastic.Data message: portnum (1, varint) and
    // payload (2, bytes).
    meshproto::Reader r(plain, len);
    uint32_t portnum = 0;
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;

    while (!r.atEnd()) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!r.readTag(field, wire)) {
            break;
        }
        if (field == 1 && wire == meshproto::WIRE_VARINT) {
            uint64_t v = 0;
            if (!r.readVarint(v)) break;
            portnum = static_cast<uint32_t>(v);
        } else if (field == 2 && wire == meshproto::WIRE_LEN) {
            if (!r.readLen(payload, payloadLen)) break;
        } else if (!r.skip(wire)) {
            break;
        }
    }

    // CTR has no MAC, so a wrong-key frame decrypts to garbage. Treat an
    // unrecognized portnum as an undecodable frame rather than acting on noise.
    if (portnum != PORT_TEXT && portnum != PORT_NODEINFO &&
        portnum != PORT_POSITION && portnum != PORT_TELEMETRY) {
        ++_rxDecryptFail;
        return;
    }

    _upsertNode(from, rssi, snr, hopsAway);

    switch (portnum) {
        case PORT_TEXT:      _onText(from, payload, payloadLen); break;
        case PORT_NODEINFO:  _onNodeInfo(from, payload, payloadLen); break;
        case PORT_POSITION:  _onPosition(from, payload, payloadLen); break;
        case PORT_TELEMETRY: _onTelemetry(from, payload, payloadLen); break;
        default: break;
    }
}

void MeshtasticManager::_onText(uint32_t from, const uint8_t* payload,
                                size_t len) {
    if (!payload || len == 0) {
        return;
    }
    const size_t copyLen = (len < sizeof(_lastText) - 1) ? len : (sizeof(_lastText) - 1);
    memcpy(_lastText, payload, copyLen);
    _lastText[copyLen] = '\0';
    _lastTextFrom = from;
    ++_rxText;
    _recordText(from, _lastText, 0, 0);
}

void MeshtasticManager::_onNodeInfo(uint32_t from, const uint8_t* payload,
                                    size_t len) {
    if (!payload) {
        return;
    }
    MeshtasticNode* node = nullptr;
    for (size_t i = 0; i < MAX_NODES; ++i) {
        if (_nodes[i].valid && _nodes[i].num == from) {
            node = &_nodes[i];
            break;
        }
    }
    if (!node) {
        return;
    }
    // User: id(1,string), long_name(2,string), short_name(3,string).
    meshproto::Reader r(payload, len);
    while (!r.atEnd()) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!r.readTag(field, wire)) break;
        if (wire == meshproto::WIRE_LEN && (field == 2 || field == 3)) {
            const uint8_t* s = nullptr;
            size_t sl = 0;
            if (!r.readLen(s, sl)) break;
            char* dst = (field == 2) ? node->longName : node->shortName;
            const size_t cap = (field == 2) ? sizeof(node->longName) : sizeof(node->shortName);
            const size_t n = (sl < cap - 1) ? sl : (cap - 1);
            memcpy(dst, s, n);
            dst[n] = '\0';
        } else if (!r.skip(wire)) {
            break;
        }
    }
    DLOG_INFO(TAG, "Node 0x%08lX = '%s' (%s)",
              static_cast<unsigned long>(from), node->longName, node->shortName);
}

void MeshtasticManager::_onPosition(uint32_t from, const uint8_t* payload,
                                    size_t len) {
    if (!payload) {
        return;
    }
    MeshtasticNode* node = nullptr;
    for (size_t i = 0; i < MAX_NODES; ++i) {
        if (_nodes[i].valid && _nodes[i].num == from) {
            node = &_nodes[i];
            break;
        }
    }
    if (!node) {
        return;
    }
    // Position: latitude_i(1, sfixed32), longitude_i(2, sfixed32),
    // altitude(3, int32 varint).
    meshproto::Reader r(payload, len);
    bool gotLat = false;
    bool gotLon = false;
    while (!r.atEnd()) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!r.readTag(field, wire)) break;
        if (field == 1 && wire == meshproto::WIRE_32BIT) {
            uint32_t v = 0;
            if (!r.readFixed32(v)) break;
            node->lat = static_cast<int32_t>(v) * 1e-7;
            gotLat = true;
        } else if (field == 2 && wire == meshproto::WIRE_32BIT) {
            uint32_t v = 0;
            if (!r.readFixed32(v)) break;
            node->lon = static_cast<int32_t>(v) * 1e-7;
            gotLon = true;
        } else if (field == 3 && wire == meshproto::WIRE_VARINT) {
            uint64_t v = 0;
            if (!r.readVarint(v)) break;
            node->altM = static_cast<int32_t>(v);
        } else if (!r.skip(wire)) {
            break;
        }
    }
    if (gotLat && gotLon) {
        node->hasPosition = true;
        DLOG_INFO(TAG, "Node 0x%08lX pos %.5f,%.5f alt=%ldm",
                  static_cast<unsigned long>(from), node->lat, node->lon,
                  static_cast<long>(node->altM));
    }
}

void MeshtasticManager::_onTelemetry(uint32_t from, const uint8_t* payload,
                                     size_t len) {
    if (!payload) {
        return;
    }
    MeshtasticNode* node = nullptr;
    for (size_t i = 0; i < MAX_NODES; ++i) {
        if (_nodes[i].valid && _nodes[i].num == from) {
            node = &_nodes[i];
            break;
        }
    }
    if (!node) {
        return;
    }
    // Telemetry: device_metrics(2, message) { battery_level(1,varint),
    // voltage(2, float/fixed32) }.
    meshproto::Reader r(payload, len);
    while (!r.atEnd()) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!r.readTag(field, wire)) break;
        if (field == 2 && wire == meshproto::WIRE_LEN) {
            const uint8_t* dm = nullptr;
            size_t dmLen = 0;
            if (!r.readLen(dm, dmLen)) break;
            meshproto::Reader m(dm, dmLen);
            while (!m.atEnd()) {
                uint32_t mf = 0;
                uint8_t mw = 0;
                if (!m.readTag(mf, mw)) break;
                if (mf == 1 && mw == meshproto::WIRE_VARINT) {
                    uint64_t v = 0;
                    if (!m.readVarint(v)) break;
                    node->batteryLevel = static_cast<uint8_t>(v);
                    node->hasTelemetry = true;
                } else if (mf == 2 && mw == meshproto::WIRE_32BIT) {
                    uint32_t v = 0;
                    if (!m.readFixed32(v)) break;
                    float fv = 0.0f;
                    memcpy(&fv, &v, sizeof(fv));
                    node->voltage = fv;
                    node->hasTelemetry = true;
                } else if (!m.skip(mw)) {
                    break;
                }
            }
        } else if (!r.skip(wire)) {
            break;
        }
    }
}

MeshtasticNode& MeshtasticManager::_upsertNode(uint32_t num, int rssi, int snr,
                                               uint8_t hops) {
    const uint32_t now = millis();
    for (size_t i = 0; i < MAX_NODES; ++i) {
        if (_nodes[i].valid && _nodes[i].num == num) {
            _nodes[i].rssi = static_cast<int16_t>(rssi);
            _nodes[i].snr = static_cast<int16_t>(snr);
            _nodes[i].hopsAway = hops;
            _nodes[i].lastHeardMs = now;
            return _nodes[i];
        }
    }
    // Find a free slot, else evict the least-recently-heard node.
    size_t slot = 0;
    bool found = false;
    for (size_t i = 0; i < MAX_NODES; ++i) {
        if (!_nodes[i].valid) {
            slot = i;
            found = true;
            break;
        }
    }
    if (!found) {
        int32_t oldestAge = static_cast<int32_t>(now - _nodes[0].lastHeardMs);
        for (size_t i = 1; i < MAX_NODES; ++i) {
            const int32_t age = static_cast<int32_t>(now - _nodes[i].lastHeardMs);
            if (age > oldestAge) {
                oldestAge = age;
                slot = i;
            }
        }
    }
    _nodes[slot] = MeshtasticNode();
    _nodes[slot].valid = true;
    _nodes[slot].num = num;
    _nodes[slot].rssi = static_cast<int16_t>(rssi);
    _nodes[slot].snr = static_cast<int16_t>(snr);
    _nodes[slot].hopsAway = hops;
    _nodes[slot].lastHeardMs = now;

    _nodeCount = 0;
    for (size_t i = 0; i < MAX_NODES; ++i) {
        if (_nodes[i].valid) {
            ++_nodeCount;
        }
    }
    DLOG_INFO(TAG, "Mesh node discovered 0x%08lX rssi=%d snr=%d hops=%u",
              static_cast<unsigned long>(num), rssi, snr,
              static_cast<unsigned>(hops));
    return _nodes[slot];
}

void MeshtasticManager::_recordText(uint32_t from, const char* text, int rssi,
                                    int snr) {
    (void)rssi;
    (void)snr;
    // TODO: emit a structured spool/MQTT event (mirror SubGhzRecordWriter) and a
    // dedicated phone notification type once a PHONE_NOTIF_TYPE_MESH is added.
    DLOG_INFO(TAG, "Mesh text from 0x%08lX: %s",
              static_cast<unsigned long>(from), text ? text : "");
}

bool MeshtasticManager::sendText(const char* text) {
    if (!_enabled || !_wio || !text || !text[0]) {
        return false;
    }
    const size_t textLen = strlen(text);

    // Build the Data plaintext: portnum=TEXT_MESSAGE_APP, payload=UTF-8 bytes.
    uint8_t plain[256] = {};
    meshproto::Writer w(plain, sizeof(plain));
    w.varintField(1, PORT_TEXT);
    w.bytesField(2, reinterpret_cast<const uint8_t*>(text), textLen);
    if (!w.ok()) {
        return false;
    }
    const size_t plainLen = w.length();

    const uint32_t packetId = _nextPacketId();

    uint8_t frame[HEADER_SIZE + 256] = {};
    wrU32(&frame[0], BROADCAST_ADDR);  // to
    wrU32(&frame[4], _myNodeNum);      // from
    wrU32(&frame[8], packetId);        // id
    // flags: hop_limit=3 in bits 0-2, hop_start=3 in bits 5-7.
    frame[12] = static_cast<uint8_t>(0x03 | (0x03 << 5));
    frame[13] = _channelHash;          // channel hash
    frame[14] = 0;                     // next_hop
    frame[15] = 0;                     // relay_node

    if (!_crypt(plain, &frame[HEADER_SIZE], plainLen, _myNodeNum, packetId)) {
        return false;
    }
    const size_t frameLen = HEADER_SIZE + plainLen;

    const bool ok = _wio->subghzSendRaw(frame, frameLen);
    if (ok) {
        ++_txText;
        DLOG_INFO(TAG, "Mesh text sent id=0x%08lX len=%u",
                  static_cast<unsigned long>(packetId),
                  static_cast<unsigned>(frameLen));
    }
    return ok;
}

bool MeshtasticManager::getNode(size_t index, MeshtasticNode& out) const {
    size_t seen = 0;
    for (size_t i = 0; i < MAX_NODES; ++i) {
        if (_nodes[i].valid) {
            if (seen == index) {
                out = _nodes[i];
                return true;
            }
            ++seen;
        }
    }
    return false;
}
