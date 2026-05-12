
#include "SpoolBinaryCodec.h"

#include <cstddef>
#include <cstring>

namespace SpoolBin {

bool writeBytes(fs::File& f, const void* data, size_t len) {
    if (!data && len > 0) return false;
    return f.write(reinterpret_cast<const uint8_t*>(data), len) == len;
}

bool readBytes(fs::File& f, void* data, size_t len) {
    if (!data && len > 0) return false;
    return f.read(reinterpret_cast<uint8_t*>(data), len) == static_cast<int>(len);
}

bool writeUVarint(fs::File& f, uint32_t value) {
    uint8_t buf[5];
    size_t used = 0;

    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7FU);
        value >>= 7;
        if (value) byte |= 0x80U;
        buf[used++] = byte;
    } while (value && used < sizeof(buf));

    return writeBytes(f, buf, used);
}

bool readUVarint(fs::File& f, uint32_t& out) {
    out = 0;
    uint8_t shift = 0;

    for (int i = 0; i < 5; i++) {
        int raw = f.read();
        if (raw < 0) return false;

        const uint8_t byte = static_cast<uint8_t>(raw);
        out |= (static_cast<uint32_t>(byte & 0x7FU) << shift);

        if ((byte & 0x80U) == 0) {
            return true;
        }

        shift += 7;
    }

    return false;
}

bool writeVarintZigZag(fs::File& f, int32_t value) {
    const uint32_t zz = (static_cast<uint32_t>(value) << 1) ^
                        static_cast<uint32_t>(value >> 31);
    return writeUVarint(f, zz);
}

bool readVarintZigZag(fs::File& f, int32_t& out) {
    uint32_t zz = 0;
    if (!readUVarint(f, zz)) {
        return false;
    }

    out = static_cast<int32_t>((zz >> 1) ^ (~(zz & 1) + 1));
    return true;
}

static uint32_t crc32Bytes(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }
    return ~crc;
}

bool readSegmentHeaderV2(fs::File& f, SegmentHeaderV2& hdr) {
    if (!f.seek(0)) return false;
    return readBytes(f, &hdr, sizeof(hdr));
}

bool writeSegmentHeaderV2(fs::File& f, const SegmentHeaderV2& hdr) {
    if (!f.seek(0)) return false;
    return writeBytes(f, &hdr, sizeof(hdr));
}

bool appendRecordV2(const String& path,
                    uint8_t recType,
                    const uint8_t* payload,
                    uint16_t length,
                    uint32_t eventId,
                    SegmentHeaderV2* outHeader,
                    AppendRecordLocation* outLoc) {
    fs::File f = LittleFS.open(path, "r+");
    if (!f) return false;

    SegmentHeaderV2 hdr;
    if (!readSegmentHeaderV2(f, hdr)) {
        f.close();
        return false;
    }

    if (hdr.magic != SEGMENT_MAGIC || hdr.version != 2) {
        f.close();
        return false;
    }

    const uint32_t writeOffset = static_cast<uint32_t>(f.size());
    if (!f.seek(writeOffset)) {
        f.close();
        return false;
    }

    RecordPrefix prefix;
    prefix.type = recType;
    prefix.flags = 0;
    prefix.length = length;

    if (!writeBytes(f, &prefix, sizeof(prefix))) {
        f.close();
        return false;
    }

    if (length > 0 && !writeBytes(f, payload, length)) {
        f.close();
        return false;
    }

    const uint32_t encodedLen = static_cast<uint32_t>(sizeof(prefix)) + static_cast<uint32_t>(length);

    hdr.recordCount++;
    hdr.bodyBytes += encodedLen;

    if (eventId != 0) {
        if (hdr.firstEventId == 0 || eventId < hdr.firstEventId) {
            hdr.firstEventId = eventId;
        }
        if (eventId > hdr.lastEventId) {
            hdr.lastEventId = eventId;
        }
    }

    if (!writeSegmentHeaderV2(f, hdr)) {
        f.close();
        return false;
    }

    if (outHeader) {
        *outHeader = hdr;
    }

    if (outLoc) {
        outLoc->offset = writeOffset;
        outLoc->len    = encodedLen;
    }

    f.close();
    return true;
}

bool appendRecordToOpen(fs::File& f,
                        uint8_t recType,
                        const uint8_t* payload,
                        uint16_t length,
                        uint32_t eventId,
                        SegmentHeaderV2& hdr) {
    RecordPrefix prefix;
    prefix.type   = recType;
    prefix.flags  = 0;
    prefix.length = length;

    if (!writeBytes(f, &prefix, sizeof(prefix))) return false;
    if (length > 0 && !writeBytes(f, payload, length)) return false;

    hdr.recordCount++;
    hdr.bodyBytes += static_cast<uint32_t>(sizeof(prefix)) + static_cast<uint32_t>(length);

    if (eventId != 0) {
        if (hdr.firstEventId == 0 || eventId < hdr.firstEventId) {
            hdr.firstEventId = eventId;
        }
        if (eventId > hdr.lastEventId) {
            hdr.lastEventId = eventId;
        }
    }

    return true;
}

bool appendCheckpointRecordV1(const String& path,
                              SpoolSegmentCheckpointV1& checkpoint,
                              AppendRecordLocation* outLoc) {
    fs::File f = LittleFS.open(path, "r+");
    if (!f) return false;

    SegmentHeaderV2 hdr;
    if (!readSegmentHeaderV2(f, hdr)) {
        f.close();
        return false;
    }

    if (hdr.magic != SEGMENT_MAGIC || hdr.version != 2) {
        f.close();
        return false;
    }

    const uint32_t writeOffset = static_cast<uint32_t>(f.size());
    if (!f.seek(writeOffset)) {
        f.close();
        return false;
    }

    checkpoint.magic = CHECKPOINT_MAGIC;
    checkpoint.version = 1;
    checkpoint.bodyOffset = static_cast<uint32_t>(f.position() + sizeof(RecordPrefix));
    checkpoint.crc32 = 0;
    checkpoint.crc32 = crc32Bytes(reinterpret_cast<const uint8_t*>(&checkpoint),
                                  offsetof(SpoolSegmentCheckpointV1, crc32));

    RecordPrefix prefix;
    prefix.type = REC_CHECKPOINT;
    prefix.flags = 0;
    prefix.length = static_cast<uint16_t>(sizeof(checkpoint));

    if (!writeBytes(f, &prefix, sizeof(prefix))) {
        f.close();
        return false;
    }

    if (!writeBytes(f, &checkpoint, sizeof(checkpoint))) {
        f.close();
        return false;
    }

    if (outLoc) {
        outLoc->offset = writeOffset;
        outLoc->len = static_cast<uint32_t>(sizeof(prefix)) +
                      static_cast<uint32_t>(sizeof(checkpoint));
    }

    f.close();
    return true;
}

bool decodeCheckpointRecordV1(const uint8_t* data,
                              size_t length,
                              uint32_t bodyOffset,
                              SpoolSegmentCheckpointV1& out) {
    if (!data && length > 0) {
        return false;
    }
    if (length != sizeof(SpoolSegmentCheckpointV1)) {
        return false;
    }

    SpoolSegmentCheckpointV1 checkpoint;
    memcpy(&checkpoint, data, sizeof(checkpoint));

    if (checkpoint.magic != CHECKPOINT_MAGIC || checkpoint.version != 1) {
        return false;
    }
    if (checkpoint.bodyOffset != bodyOffset) {
        return false;
    }

    const uint32_t expected = crc32Bytes(
        reinterpret_cast<const uint8_t*>(&checkpoint),
        offsetof(SpoolSegmentCheckpointV1, crc32));
    if (checkpoint.crc32 != expected) {
        return false;
    }

    out = checkpoint;
    return true;
}

bool createEmptySegmentV2(const String& path, uint32_t segmentId, uint32_t createdMs) {
    File f = LittleFS.open(path, "w");
    if (!f) return false;

    SegmentHeaderV2 hdr;
    hdr.segmentId = segmentId;
    hdr.createdMs = createdMs;

    const bool ok = writeBytes(f, &hdr, sizeof(hdr));
    f.close();
    return ok;
}

} // namespace SpoolBin



