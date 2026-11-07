#pragma once

#include "SnapshotQueue.h"
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <arpa/inet.h>

/**
 * BinarySnapshot — High-efficiency, versioned binary serialization for WorldSnapshots.
 *
 * Binary Layout Specification (v1):
 * Header (36 bytes):
 *   [0..3]   magic:         uint32 ('SNAP' = 0x534E4150, network byte order)
 *   [4]      version:       uint8  (1)
 *   [5]      roomState:     uint8  (0=WAITING, 1=COUNTDOWN, 2=RACING, 3=FINISHED)
 *   [6..7]   countdown:     uint16 (tenths of a second, big-endian)
 *   [8..11]  roomId:        uint32 (big-endian)
 *   [12..19] roomCode:      char[8] (fixed ASCII null-padded/null-terminated)
 *   [20..23] tick:          uint32 (low 32 bits, big-endian)
 *   [24..31] timestamp:     uint64 (milliseconds epoch, big-endian)
 *   [32..33] playerCount:   uint16 (big-endian)
 *   [34..35] resultCount:   uint16 (big-endian)
 *
 * Per-Player Record (36 bytes):
 *   [0..3]   id:            uint32 (big-endian)
 *   [4..7]   x:             float32 (IEEE-754 single precision, big-endian)
 *   [8..11]  distance:      float32 (IEEE-754 single precision, big-endian)
 *   [12..15] speed:         float32 (IEEE-754 single precision, big-endian)
 *   [16..19] steer:         float32 (IEEE-754 single precision, big-endian)
 *   [20..23] worldX:        float32 (IEEE-754 single precision, big-endian)
 *   [24..27] worldY:        float32 (IEEE-754 single precision, big-endian)
 *   [28..31] heading:       float32 (IEEE-754 single precision, big-endian)
 *   [32..35] ackSeq:        uint32 (big-endian)
 *
 * Per-Result Record (11 + nameLen bytes):
 *   [0..3]   playerId:      uint32 (big-endian)
 *   [4..5]   rank:          uint16 (big-endian)
 *   [6..9]   finishTime:    float32 (IEEE-754 single precision, big-endian)
 *   [10]     nameLen:       uint8
 *   [11..]   name:          char[nameLen] (UTF-8)
 */
class BinarySnapshot {
public:
    static constexpr uint32_t MAGIC = 0x534E4150; // "SNAP"
    static constexpr uint8_t VERSION = 1;
    static constexpr size_t HEADER_SIZE = 36;
    static constexpr size_t PLAYER_RECORD_SIZE = 36;
    static constexpr size_t RESULT_HEADER_SIZE = 11;

    // Serializes a WorldSnapshot into a compact binary buffer
    static std::string serialize(const WorldSnapshot& snap);

    // Deserializes a binary buffer back into a WorldSnapshot (validates bounds and integrity)
    static bool deserialize(const uint8_t* data, size_t len, WorldSnapshot& outSnap);
    static bool deserialize(const std::string& buf, WorldSnapshot& outSnap);

private:
    static inline void writeU8(std::string& buf, uint8_t val) {
        buf.push_back(static_cast<char>(val));
    }

    static inline void writeU16(std::string& buf, uint16_t val) {
        uint16_t be = htons(val);
        buf.append(reinterpret_cast<const char*>(&be), sizeof(be));
    }

    static inline void writeU32(std::string& buf, uint32_t val) {
        uint32_t be = htonl(val);
        buf.append(reinterpret_cast<const char*>(&be), sizeof(be));
    }

    static inline void writeU64(std::string& buf, uint64_t val) {
        uint32_t high = htonl(static_cast<uint32_t>(val >> 32));
        uint32_t low = htonl(static_cast<uint32_t>(val & 0xFFFFFFFF));
        buf.append(reinterpret_cast<const char*>(&high), sizeof(high));
        buf.append(reinterpret_cast<const char*>(&low), sizeof(low));
    }

    static inline void writeFloat(std::string& buf, float val) {
        uint32_t u;
        std::memcpy(&u, &val, sizeof(float));
        writeU32(buf, u);
    }
};
