
#include "SpoolBinaryCodec.h"

#include <cstddef>
#include <cstring>

namespace SpoolBin {

namespace {

enum FieldMapType : uint8_t {
    FIELD_NULL = 0,
    FIELD_STRING = 1,
    FIELD_INT = 2,
    FIELD_UINT = 3,
    FIELD_FLOAT = 4,
    FIELD_BOOL = 5
};

struct BoundedWriter {
    uint8_t* out = nullptr;
    size_t capacity = 0;
    size_t pos = 0;
    bool ok = true;

    bool write(uint8_t value) {
        if (!ok || !out || pos >= capacity) {
            ok = false;
            return false;
        }
        out[pos++] = value;
        return true;
    }

    bool writeBytes(const void* data, size_t len) {
        if (!ok || (!data && len > 0) || !out || capacity - pos < len) {
            ok = false;
            return false;
        }
        memcpy(out + pos, data, len);
        pos += len;
        return true;
    }
};

bool writeUVarintTo(BoundedWriter& w, uint32_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7FU);
        value >>= 7;
        if (value) byte |= 0x80U;
        if (!w.write(byte)) return false;
    } while (value);
    return true;
}

bool readUVarintFrom(const uint8_t*& p, const uint8_t* end, uint32_t& out) {
    out = 0;
    uint8_t shift = 0;

    for (int i = 0; i < 5; i++) {
        if (p >= end) return false;
        const uint8_t byte = *p++;
        out |= (static_cast<uint32_t>(byte & 0x7FU) << shift);
        if ((byte & 0x80U) == 0) {
            return true;
        }
        shift += 7;
    }

    return false;
}

bool writeZigZag32To(BoundedWriter& w, int32_t value) {
    const uint32_t zz =
        (static_cast<uint32_t>(value) << 1) ^
        static_cast<uint32_t>(value >> 31);
    return writeUVarintTo(w, zz);
}

bool readZigZag32From(const uint8_t*& p, const uint8_t* end, int32_t& out) {
    uint32_t zz = 0;
    if (!readUVarintFrom(p, end, zz)) return false;
    out = static_cast<int32_t>((zz >> 1) ^ (~(zz & 1) + 1));
    return true;
}

bool writeStringTo(BoundedWriter& w, const String& s) {
    if (!writeUVarintTo(w, static_cast<uint32_t>(s.length()))) {
        return false;
    }
    for (size_t i = 0; i < s.length(); i++) {
        if (!w.write(static_cast<uint8_t>(s[i]))) return false;
    }
    return true;
}

bool readStringFrom(const uint8_t*& p, const uint8_t* end, String& out) {
    uint32_t len = 0;
    if (!readUVarintFrom(p, end, len)) return false;
    if (static_cast<size_t>(end - p) < len) return false;

    out = "";
    out.reserve(len);
    for (uint32_t i = 0; i < len; i++) {
        out += static_cast<char>(*p++);
    }
    return true;
}

} // namespace

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

bool encodeFieldMapV1(JsonObjectConst doc,
                      uint8_t* out,
                      size_t capacity,
                      size_t& written) {
    written = 0;
    if (!out || capacity == 0) {
        return false;
    }

    BoundedWriter w{out, capacity, 0, true};

    uint32_t fieldCount = 0;
    for (JsonPairConst kv : doc) {
        const char* key = kv.key().c_str();
        if (key && key[0]) {
            fieldCount++;
        }
    }

    if (!writeUVarintTo(w, fieldCount)) {
        return false;
    }

    for (JsonPairConst kv : doc) {
        const char* key = kv.key().c_str();
        if (!key || !key[0]) {
            continue;
        }

        if (!writeStringTo(w, String(key))) {
            return false;
        }

        JsonVariantConst value = kv.value();
        if (value.isNull()) {
            if (!w.write(FIELD_NULL)) return false;
            continue;
        }

        if (value.is<bool>()) {
            if (!w.write(FIELD_BOOL) ||
                !w.write(value.as<bool>() ? 1U : 0U)) {
                return false;
            }
            continue;
        }

        if (value.is<float>() || value.is<double>()) {
            const float v = value.as<float>();
            if (!w.write(FIELD_FLOAT) || !w.writeBytes(&v, sizeof(v))) {
                return false;
            }
            continue;
        }

        if (value.is<long long>() || value.is<long>() || value.is<int>()) {
            const long long v = value.as<long long>();
            if (v < 0) {
                if (!w.write(FIELD_INT) ||
                    !writeZigZag32To(w, static_cast<int32_t>(v))) {
                    return false;
                }
            } else {
                if (!w.write(FIELD_UINT) ||
                    !writeUVarintTo(w, static_cast<uint32_t>(v))) {
                    return false;
                }
            }
            continue;
        }

        if (value.is<unsigned long long>() ||
            value.is<unsigned long>() ||
            value.is<unsigned int>()) {
            if (!w.write(FIELD_UINT) ||
                !writeUVarintTo(w, static_cast<uint32_t>(
                    value.as<unsigned long long>()))) {
                return false;
            }
            continue;
        }

        if (!w.write(FIELD_STRING) || !writeStringTo(w, value.as<String>())) {
            return false;
        }
    }

    written = w.pos;
    return w.ok;
}

bool decodeFieldMapV1(const uint8_t* data,
                      size_t length,
                      JsonObject out) {
    if (!data && length > 0) {
        return false;
    }

    const uint8_t* p = data;
    const uint8_t* end = data + length;

    uint32_t fieldCount = 0;
    if (!readUVarintFrom(p, end, fieldCount)) {
        return false;
    }

    for (uint32_t i = 0; i < fieldCount; i++) {
        String key;
        if (!readStringFrom(p, end, key) || p >= end) {
            return false;
        }

        const uint8_t fieldType = *p++;
        switch (fieldType) {
            case FIELD_NULL:
                out[key].set(nullptr);
                break;

            case FIELD_STRING: {
                String value;
                if (!readStringFrom(p, end, value)) return false;
                out[key] = value;
                break;
            }

            case FIELD_INT: {
                int32_t value = 0;
                if (!readZigZag32From(p, end, value)) return false;
                out[key] = value;
                break;
            }

            case FIELD_UINT: {
                uint32_t value = 0;
                if (!readUVarintFrom(p, end, value)) return false;
                out[key] = value;
                break;
            }

            case FIELD_FLOAT: {
                if (static_cast<size_t>(end - p) < sizeof(float)) return false;
                float value = 0.0f;
                memcpy(&value, p, sizeof(float));
                p += sizeof(float);
                out[key] = value;
                break;
            }

            case FIELD_BOOL:
                if (p >= end) return false;
                out[key] = (*p++ != 0);
                break;

            default:
                return false;
        }
    }

    return p == end;
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



