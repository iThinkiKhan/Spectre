#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "../config.h"
#include "WioNrfAccessory.h"

// Basic Meshtastic client. Drives the shared SX1262 (via the WIO nRF modem
// bridge) in the mutually-exclusive MESHTASTIC application mode: US-915
// LongFast, default public channel + key. Decodes text / nodeinfo / position /
// telemetry into a node DB, and can transmit text messages onto the mesh.
//
// All framing, AES-CTR crypto (mbedtls), and protobuf parsing live here; the
// nRF only moves raw LoRa frames. See [[wio-nrf-as-sx1262-modem-bridge]].

struct MeshtasticNode {
    bool     valid = false;
    uint32_t num = 0;
    char     longName[20] = {};
    char     shortName[8] = {};
    bool     hasPosition = false;
    double   lat = 0.0;
    double   lon = 0.0;
    int32_t  altM = 0;
    bool     hasTelemetry = false;
    uint8_t  batteryLevel = 255;  // 0..100, 255 = unknown
    float    voltage = 0.0f;
    uint32_t lastHeardMs = 0;
    int16_t  rssi = 0;
    int16_t  snr = 0;
    uint8_t  hopsAway = 0;
};

class MeshtasticManager {
public:
    static MeshtasticManager& getInstance() {
        static MeshtasticManager instance;
        return instance;
    }

    void attach(WioNrfAccessory* wio) { _wio = wio; }
    bool begin();

    // SX1262 hardware present (mesh can be enabled).
    bool isAvailable() const;
    bool isEnabled() const { return _enabled; }

    // Take/release the shared SX1262 for Meshtastic. enable() reconfigures the
    // radio to the LongFast PHY and arms RX; disable() hands ownership back so
    // the native SubGhz backend re-asserts its profile.
    bool enable();
    void disable();

    void tick();

    // Transmit a text message on the primary channel (broadcast). Returns false
    // if mesh is not enabled or the frame couldn't be queued.
    bool sendText(const char* text);

    uint32_t nodeNum() const { return _myNodeNum; }
    size_t   nodeCount() const { return _nodeCount; }
    bool     getNode(size_t index, MeshtasticNode& out) const;

    uint32_t rxFrames() const { return _rxFrames; }
    uint32_t rxText() const { return _rxText; }
    uint32_t rxDecryptFail() const { return _rxDecryptFail; }
    uint32_t txText() const { return _txText; }
    const char* lastText() const { return _lastText; }
    uint32_t lastTextFrom() const { return _lastTextFrom; }

private:
    static constexpr size_t MAX_NODES = MESHTASTIC_MAX_NODES;
    static constexpr uint32_t BROADCAST_ADDR = 0xFFFFFFFFUL;
    // Meshtastic PortNums we decode.
    static constexpr uint8_t PORT_TEXT = 1;
    static constexpr uint8_t PORT_POSITION = 3;
    static constexpr uint8_t PORT_NODEINFO = 4;
    static constexpr uint8_t PORT_TELEMETRY = 67;

    MeshtasticManager() = default;

    void _computeChannelHash();
    uint32_t _deriveNodeNum() const;
    uint32_t _nextPacketId();
    void _initNonce(uint8_t nonce[16], uint32_t fromNode, uint32_t packetId) const;
    bool _crypt(const uint8_t* in, uint8_t* out, size_t len,
                uint32_t fromNode, uint32_t packetId) const;

    void _handleFrame(const uint8_t* data, size_t len, int rssi, int snr);
    void _handleDecoded(uint32_t from, const uint8_t* plain, size_t len,
                        int rssi, int snr, uint8_t hopsAway);
    void _onText(uint32_t from, const uint8_t* payload, size_t len);
    void _onNodeInfo(uint32_t from, const uint8_t* payload, size_t len);
    void _onPosition(uint32_t from, const uint8_t* payload, size_t len);
    void _onTelemetry(uint32_t from, const uint8_t* payload, size_t len);

    MeshtasticNode& _upsertNode(uint32_t num, int rssi, int snr, uint8_t hops);
    void _recordText(uint32_t from, const char* text, int rssi, int snr);

    WioNrfAccessory* _wio = nullptr;
    bool     _begun = false;
    bool     _enabled = false;
    uint8_t  _channelKey[16] = {};
    uint8_t  _channelHash = 0;
    uint32_t _myNodeNum = 0;
    uint32_t _txPacketId = 0;

    MeshtasticNode _nodes[MAX_NODES] = {};
    size_t   _nodeCount = 0;

    uint32_t _rxFrames = 0;
    uint32_t _rxText = 0;
    uint32_t _rxDecryptFail = 0;
    uint32_t _txText = 0;
    uint32_t _lastTextFrom = 0;
    char     _lastText[160] = {};
};

#define MESHTASTIC MeshtasticManager::getInstance()
