

#pragma once

#include "SubGhzTypes.h"

class StorageManager;

class SubGhzRecordWriter {
public:
    static bool logPacketRx(StorageManager& storage, const SubGhzPacket& pkt);
};


