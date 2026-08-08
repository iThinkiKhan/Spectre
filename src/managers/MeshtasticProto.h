#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

// Minimal protobuf wire-format reader/writer for the handful of Meshtastic
// messages this client needs (Data / User / Position / Telemetry on decode, and
// a Data text message on encode). This intentionally avoids pulling in nanopb +
// the full meshtastic .proto set — we only touch a few well-known field numbers.
//
// Wire types: 0 = varint, 1 = 64-bit, 2 = length-delimited, 5 = 32-bit.
namespace meshproto {

enum WireType : uint8_t {
    WIRE_VARINT = 0,
    WIRE_64BIT  = 1,
    WIRE_LEN    = 2,
    WIRE_32BIT  = 5,
};

class Reader {
public:
    Reader(const uint8_t* data, size_t len) : _p(data), _end(data + len) {}

    bool atEnd() const { return _p >= _end; }

    // Reads a field tag, returning the field number and wire type.
    bool readTag(uint32_t& field, uint8_t& wire) {
        uint64_t key = 0;
        if (!readVarint(key)) {
            return false;
        }
        field = static_cast<uint32_t>(key >> 3);
        wire = static_cast<uint8_t>(key & 0x07);
        return true;
    }

    bool readVarint(uint64_t& out) {
        out = 0;
        uint8_t shift = 0;
        while (_p < _end && shift < 64) {
            const uint8_t b = *_p++;
            out |= static_cast<uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) {
                return true;
            }
            shift += 7;
        }
        return false;
    }

    bool readFixed32(uint32_t& out) {
        if (_end - _p < 4) {
            return false;
        }
        out = static_cast<uint32_t>(_p[0]) |
              (static_cast<uint32_t>(_p[1]) << 8) |
              (static_cast<uint32_t>(_p[2]) << 16) |
              (static_cast<uint32_t>(_p[3]) << 24);
        _p += 4;
        return true;
    }

    // Returns a view (pointer + length) into the underlying buffer.
    bool readLen(const uint8_t*& data, size_t& len) {
        uint64_t n = 0;
        if (!readVarint(n)) {
            return false;
        }
        if (static_cast<uint64_t>(_end - _p) < n) {
            return false;
        }
        data = _p;
        len = static_cast<size_t>(n);
        _p += n;
        return true;
    }

    // Skips a field of the given wire type (for fields we don't care about).
    bool skip(uint8_t wire) {
        switch (wire) {
            case WIRE_VARINT: {
                uint64_t tmp = 0;
                return readVarint(tmp);
            }
            case WIRE_64BIT:
                if (_end - _p < 8) return false;
                _p += 8;
                return true;
            case WIRE_LEN: {
                const uint8_t* d = nullptr;
                size_t l = 0;
                return readLen(d, l);
            }
            case WIRE_32BIT:
                if (_end - _p < 4) return false;
                _p += 4;
                return true;
            default:
                return false;
        }
    }

private:
    const uint8_t* _p;
    const uint8_t* _end;
};

class Writer {
public:
    Writer(uint8_t* buf, size_t cap) : _buf(buf), _cap(cap), _len(0) {}

    size_t length() const { return _len; }
    bool ok() const { return !_overflow; }

    bool varintField(uint32_t field, uint64_t value) {
        return tag(field, WIRE_VARINT) && varint(value);
    }

    bool bytesField(uint32_t field, const uint8_t* data, size_t n) {
        if (!tag(field, WIRE_LEN) || !varint(n)) {
            return false;
        }
        return raw(data, n);
    }

private:
    bool tag(uint32_t field, uint8_t wire) {
        return varint((static_cast<uint64_t>(field) << 3) | wire);
    }

    bool varint(uint64_t value) {
        do {
            uint8_t b = value & 0x7F;
            value >>= 7;
            if (value) {
                b |= 0x80;
            }
            if (!put(b)) {
                return false;
            }
        } while (value);
        return true;
    }

    bool raw(const uint8_t* data, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            if (!put(data[i])) {
                return false;
            }
        }
        return true;
    }

    bool put(uint8_t b) {
        if (_len >= _cap) {
            _overflow = true;
            return false;
        }
        _buf[_len++] = b;
        return true;
    }

    uint8_t* _buf;
    size_t _cap;
    size_t _len;
    bool _overflow = false;
};

// XOR-fold of a byte buffer — Meshtastic's channel-hash primitive.
inline uint8_t xorHash(const uint8_t* data, size_t len) {
    uint8_t h = 0;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
    }
    return h;
}

}  // namespace meshproto
