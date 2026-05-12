


#include "SubGhzManager.h"

#include "../core/DebugLog.h"

bool SubGhzManager::registerBackend(ISubGhzBackend* backend) {
    if (!backend) {
        return false;
    }

    for (size_t i = 0; i < _backendCount; ++i) {
        if (_backends[i] == backend) {
            return true;
        }
    }

    if (_backendCount >= MAX_BACKENDS) {
        DLOG_WARN("SUBGHZ", "Backend registry full");
        return false;
    }

    _backends[_backendCount++] = backend;
    if (!_backend) {
        _backend = backend;
    }
    return true;
}

bool SubGhzManager::begin() {
    if (_backendCount == 0 && _backend) {
        registerBackend(_backend);
    }

    if (_backendCount == 0) {
        DLOG_WARN("SUBGHZ", "No backend attached");
        _status = SubGhzStatus();
        return false;
    }

    for (size_t i = 0; i < MAX_NODES; ++i) {
        _nodes[i] = SubGhzNodeSeen();
    }
    _nodeCount = 0;

    _backend = nullptr;
    bool ok = false;
    for (size_t i = 0; i < _backendCount; ++i) {
        if (!_backends[i]) {
            continue;
        }
        if (_backends[i]->begin()) {
            _backend = _backends[i];
            ok = true;
            break;
        }
    }

    _refreshStatus();

    DLOG_INFO("SUBGHZ", "Init backend=%s ready=%d",
              _status.backendName,
              ok ? 1 : 0);
    return ok;
}

void SubGhzManager::tick() {
    if (!_backend) return;

    _backend->tick();
    _refreshStatus();
}

bool SubGhzManager::isReady() const {
    return _backend && _backend->isReady();
}

bool SubGhzManager::available() {
    return _backend && _backend->available();
}

bool SubGhzManager::readPacket(SubGhzPacket& outPacket) {
    if (!_backend || !_backend->readPacket(outPacket)) {
        return false;
    }
    _refreshStatus();
    return true;
}

bool SubGhzManager::send(const char* payload, uint16_t destination) {
    return _backend && _backend->send(payload, destination);
}

bool SubGhzManager::setMode(SubGhzMode mode) {
    if (!_backend) return false;
    const bool ok = _backend->setMode(mode);
    if (ok) {
        _refreshStatus();
    }
    return ok;
}

SubGhzMode SubGhzManager::mode() const {
    if (!_backend) return SubGhzMode::OFF;
    return _backend->mode();
}

const char* SubGhzManager::backendName() const {
    if (!_backend) return "NONE";
    return _backend->backendName();
}

uint32_t SubGhzManager::frequencyHz() const {
    if (!_backend) return 0;
    return _backend->frequencyHz();
}

String SubGhzManager::firmwareVersion() const {
    if (!_backend) return String();
    return _backend->firmwareVersion();
}

void SubGhzManager::notePacket(const SubGhzPacket& pkt) {
    if (pkt.source == 0) {
        _status.lastPacketMs = pkt.timestampMs;
        return;
    }

    for (size_t i = 0; i < MAX_NODES; ++i) {
        if (_nodes[i].valid && _nodes[i].address == pkt.source) {
            _nodes[i].lastRSSI = pkt.rssi;
            _nodes[i].lastSNR = pkt.snr;
            _nodes[i].lastSeenMs = pkt.timestampMs;
            _nodes[i].packetCount++;
            _status.lastSource = pkt.source;
            _status.lastPacketMs = pkt.timestampMs;
            _status.nodeCount = static_cast<uint16_t>(nodeCount());
            return;
        }
    }

    for (size_t i = 0; i < MAX_NODES; ++i) {
        if (!_nodes[i].valid) {
            _nodes[i].valid = true;
            _nodes[i].address = pkt.source;
            _nodes[i].lastRSSI = pkt.rssi;
            _nodes[i].lastSNR = pkt.snr;
            _nodes[i].firstSeenMs = pkt.timestampMs;
            _nodes[i].lastSeenMs = pkt.timestampMs;
            _nodes[i].packetCount = 1;
            if (i + 1 > _nodeCount) {
                _nodeCount = i + 1;
            }
            DLOG_INFO("SUBGHZ", "Node discovered addr=%u RSSI=%d SNR=%d",
                      static_cast<unsigned>(_nodes[i].address),
                      static_cast<int>(_nodes[i].lastRSSI),
                      static_cast<int>(_nodes[i].lastSNR));
            _status.lastSource = pkt.source;
            _status.lastPacketMs = pkt.timestampMs;
            _status.nodeCount = static_cast<uint16_t>(nodeCount());
            return;
        }
    }

    size_t oldest = 0;
    for (size_t i = 1; i < MAX_NODES; ++i) {
        if (_nodes[i].lastSeenMs < _nodes[oldest].lastSeenMs) {
            oldest = i;
        }
    }

    _nodes[oldest].valid = true;
    _nodes[oldest].address = pkt.source;
    _nodes[oldest].lastRSSI = pkt.rssi;
    _nodes[oldest].lastSNR = pkt.snr;
    _nodes[oldest].firstSeenMs = pkt.timestampMs;
    _nodes[oldest].lastSeenMs = pkt.timestampMs;
    _nodes[oldest].packetCount = 1;
    _status.lastSource = pkt.source;
    _status.lastPacketMs = pkt.timestampMs;
    _status.nodeCount = static_cast<uint16_t>(nodeCount());
}

size_t SubGhzManager::nodeCount() const {
    size_t count = 0;
    for (size_t i = 0; i < MAX_NODES; ++i) {
        if (_nodes[i].valid) {
            count++;
        }
    }
    return count;
}

bool SubGhzManager::getNode(size_t index, SubGhzNodeSeen& outNode) const {
    size_t seen = 0;
    for (size_t i = 0; i < MAX_NODES; ++i) {
        if (_nodes[i].valid) {
            if (seen == index) {
                outNode = _nodes[i];
                return true;
            }
            seen++;
        }
    }
    return false;
}

SubGhzStatus SubGhzManager::status() const {
    return _status;
}

SubGhzStats SubGhzManager::stats() const {
    if (!_backend) return SubGhzStats();
    SubGhzStats value = _backend->stats();
    value.nodesSeen = static_cast<uint16_t>(nodeCount());
    value.lastSource = _status.lastSource;
    value.lastFrequencyHz = _status.frequencyHz;
    value.lastPacketMs = _status.lastPacketMs;
    return value;
}

SubGhzCapabilities SubGhzManager::capabilities() const {
    if (!_backend) return SubGhzCapabilities();
    return _backend->capabilities();
}

void SubGhzManager::_refreshStatus() {
    if (!_backend) {
        _status = SubGhzStatus();
        return;
    }

    const SubGhzCapabilities caps = _backend->capabilities();
    _status.ready = _backend->isReady();
    _status.modulePresent = _status.ready;
    _status.rxActive = _status.ready && caps.has(SUBGHZ_CAP_RX) &&
                       (_backend->mode() != SubGhzMode::OFF);
    _status.txActive = _status.ready && caps.has(SUBGHZ_CAP_TX);
    _status.backendType = _backend->backendType();
    _status.mode = _backend->mode();
    _status.frequencyHz = _backend->frequencyHz();
    _status.profile = _backend->profile();
    _status.nodeCount = static_cast<uint16_t>(nodeCount());
    strlcpy(_status.backendName, _backend->backendName(), sizeof(_status.backendName));
    strlcpy(_status.moduleName, _backend->moduleName(), sizeof(_status.moduleName));
    strlcpy(_status.firmware, _backend->firmwareVersion().c_str(), sizeof(_status.firmware));
}




