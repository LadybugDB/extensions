#pragma once

#include <cstddef>
#include <cstdint>

namespace lbug {
namespace sqlite_extension {
namespace cdc {

// SQLite WAL layout (https://sqlite.org/wal.html). All multi-byte integers
// are big-endian. This header is dependency-free (STL only) so the CDC
// tailer/decoder can be unit-tested without the query engine.

constexpr uint32_t WAL_MAGIC_NATIVE = 0x377f0682;
constexpr uint32_t WAL_MAGIC_NON_NATIVE = 0x377f0683;
constexpr uint32_t WAL_FORMAT_VERSION = 3007000;
constexpr size_t WAL_HDR_SIZE = 32;
constexpr size_t WAL_FRAME_HDR_SIZE = 24;

struct WalHeader {
    uint32_t magic = 0;
    uint32_t format = 0;
    uint32_t pageSize = 0;
    uint32_t checkpointSeq = 0;
    uint32_t salt1 = 0;
    uint32_t salt2 = 0;
    uint32_t checksum1 = 0;
    uint32_t checksum2 = 0;
};

struct WalFrameHeader {
    uint32_t pageNo = 0;
    // Nonzero on commit frames: the database size in pages after commit.
    uint32_t commitSize = 0;
    uint32_t salt1 = 0;
    uint32_t salt2 = 0;
    uint32_t checksum1 = 0;
    uint32_t checksum2 = 0;
};

inline uint16_t readU16BE(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t readU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// SQLite WAL checksum accumulator. The word order and stepping follow the
// WAL magic: native magic (0x377f0682, what little-endian hosts write)
// checksums little-endian words two at a time (s0 += w0+s1; s1 += w1+s0);
// non-native magic checksums big-endian words sequentially.
struct WalChecksum {
    bool native = true;
    uint32_t s0 = 0;
    uint32_t s1 = 0;

    void update(const uint8_t* data, size_t nByte) {
        if (native) {
            for (size_t i = 0; i + 8 <= nByte; i += 8) {
                s0 += readU32LE(data + i) + s1;
                s1 += readU32LE(data + i + 4) + s0;
            }
        } else {
            for (size_t i = 0; i + 4 <= nByte; i += 4) {
                uint32_t x = readU32BE(data + i);
                s0 += x + s1;
                s1 += x + s0;
            }
        }
    }
};

// Reads a SQLite big-endian base-128 varint (max 9 bytes; the 9th byte
// contributes all 8 bits). Returns false on truncation.
inline bool readVarint(const uint8_t* p, size_t maxLen, uint64_t& value, size_t& len) {
    value = 0;
    for (size_t i = 0; i < 9 && i < maxLen; i++) {
        if (i == 8) {
            value = (value << 8) | p[i];
            len = 9;
            return true;
        }
        value = (value << 7) | (p[i] & 0x7f);
        if ((p[i] & 0x80) == 0) {
            len = i + 1;
            return true;
        }
    }
    return false;
}

} // namespace cdc
} // namespace sqlite_extension
} // namespace lbug
