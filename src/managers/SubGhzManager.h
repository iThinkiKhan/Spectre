


#pragma once

#include "ISubGhzBackend.h"

class SubGhzManager {
public:
    static SubGhzManager& getInstance() {
        static SubGhzManager instance;
        return instance;
    }

    void attachBackend(ISubGhzBackend* backend) {
        registerBackend(backend);
    }
    bool registerBackend(ISubGhzBackend* backend);

    bool begin();
    void tick();

    bool isReady() const;
    bool available();
    bool readPacket(SubGhzPacket& outPacket);
    bool send(const char* payload, uint16_t destination = 0);

    bool setMode(SubGhzMode mode);
    SubGhzMode mode() const;

    SubGhzStatus status() const;
    SubGhzStats stats() const;
    SubGhzCapabilities capabilities() const;
    const char* backendName() const;
    uint32_t frequencyHz() const;
    String firmwareVersion() const;
    void notePacket(const SubGhzPacket& pkt);
    size_t nodeCount() const;
    bool getNode(size_t index, SubGhzNodeSeen& outNode) const;

private:
    static constexpr size_t MAX_NODES = 16;
    static constexpr size_t MAX_BACKENDS = 4;
    
    SubGhzManager() = default;
    void _refreshStatus();
    ISubGhzBackend* _backends[MAX_BACKENDS] = {};
    size_t _backendCount = 0;
    ISubGhzBackend* _backend = nullptr;
    SubGhzStatus _status;
    SubGhzNodeSeen _nodes[MAX_NODES] = {};
    size_t _nodeCount = 0;
};

#define SUBGHZ SubGhzManager::getInstance()




