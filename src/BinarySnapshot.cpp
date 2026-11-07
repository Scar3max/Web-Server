#include "BinarySnapshot.h"
#include <algorithm>
#include <cmath>

std::string BinarySnapshot::serialize(const WorldSnapshot& snap) {
    std::string buf;
    size_t estimatedSize = HEADER_SIZE + snap.players.size() * PLAYER_RECORD_SIZE + snap.results.size() * 32;
    buf.reserve(estimatedSize);

    // 1. Header (36 bytes)
    writeU32(buf, MAGIC);
    writeU8(buf, VERSION);
    writeU8(buf, static_cast<uint8_t>(snap.roomState));

    uint16_t countdownTenths = (snap.countdown > 0.0)
        ? static_cast<uint16_t>(std::clamp<double>(snap.countdown * 10.0 + 0.5, 0.0, 65535.0))
        : 0;
    writeU16(buf, countdownTenths);
    writeU32(buf, snap.roomId);

    // 8-byte fixed roomCode buffer
    char codeBuf[8] = {0};
    size_t copyLen = std::min<size_t>(snap.roomCode.size(), 8);
    if (copyLen > 0) {
        std::memcpy(codeBuf, snap.roomCode.data(), copyLen);
    }
    buf.append(codeBuf, 8);

    writeU32(buf, static_cast<uint32_t>(snap.tick & 0xFFFFFFFF));
    writeU64(buf, snap.timestamp);
    writeU16(buf, static_cast<uint16_t>(snap.players.size()));
    writeU16(buf, static_cast<uint16_t>(snap.results.size()));

    // 2. Players (36 bytes each)
    for (const auto& p : snap.players) {
        writeU32(buf, p.id);
        writeFloat(buf, static_cast<float>(p.x));
        writeFloat(buf, static_cast<float>(p.distance));
        writeFloat(buf, static_cast<float>(p.speed));
        writeFloat(buf, static_cast<float>(p.steer));
        writeFloat(buf, static_cast<float>(p.worldX));
        writeFloat(buf, static_cast<float>(p.worldY));
        writeFloat(buf, static_cast<float>(p.heading));
        writeU32(buf, static_cast<uint32_t>(p.ackSeq & 0xFFFFFFFF));
    }

    // 3. Results (11 + nameLen bytes each)
    for (const auto& r : snap.results) {
        writeU32(buf, r.playerId);
        writeU16(buf, static_cast<uint16_t>(r.rank));
        writeFloat(buf, static_cast<float>(r.finishTime));
        uint8_t nameLen = static_cast<uint8_t>(std::min<size_t>(r.name.size(), 255));
        writeU8(buf, nameLen);
        if (nameLen > 0) {
            buf.append(r.name.data(), nameLen);
        }
    }

    return buf;
}

bool BinarySnapshot::deserialize(const uint8_t* data, size_t len, WorldSnapshot& outSnap) {
    if (!data || len < HEADER_SIZE) {
        return false;
    }

    const uint8_t* ptr = data;
    const uint8_t* end = data + len;

    // 1. Magic
    uint32_t beMagic;
    std::memcpy(&beMagic, ptr, sizeof(beMagic));
    ptr += sizeof(beMagic);
    if (ntohl(beMagic) != MAGIC) {
        return false;
    }

    // 2. Version
    uint8_t version = *ptr++;
    if (version != VERSION) {
        return false;
    }

    // 3. RoomState
    uint8_t rawState = *ptr++;
    if (rawState > static_cast<uint8_t>(RoomState::FINISHED)) {
        return false;
    }
    outSnap.roomState = static_cast<RoomState>(rawState);

    // 4. Countdown
    uint16_t beCountdown;
    std::memcpy(&beCountdown, ptr, sizeof(beCountdown));
    ptr += sizeof(beCountdown);
    outSnap.countdown = static_cast<double>(ntohs(beCountdown)) / 10.0;

    // 5. RoomId
    uint32_t beRoomId;
    std::memcpy(&beRoomId, ptr, sizeof(beRoomId));
    ptr += sizeof(beRoomId);
    outSnap.roomId = ntohl(beRoomId);

    // 6. RoomCode
    char codeBuf[9] = {0};
    std::memcpy(codeBuf, ptr, 8);
    ptr += 8;
    outSnap.roomCode = std::string(codeBuf);
    auto nullPos = outSnap.roomCode.find('\0');
    if (nullPos != std::string::npos) {
        outSnap.roomCode.erase(nullPos);
    }

    // 7. Tick
    uint32_t beTick;
    std::memcpy(&beTick, ptr, sizeof(beTick));
    ptr += sizeof(beTick);
    outSnap.tick = ntohl(beTick);

    // 8. Timestamp
    uint32_t beTimeHigh, beTimeLow;
    std::memcpy(&beTimeHigh, ptr, sizeof(beTimeHigh));
    ptr += sizeof(beTimeHigh);
    std::memcpy(&beTimeLow, ptr, sizeof(beTimeLow));
    ptr += sizeof(beTimeLow);
    outSnap.timestamp = (static_cast<uint64_t>(ntohl(beTimeHigh)) << 32) | ntohl(beTimeLow);

    // 9. Counts
    uint16_t bePlayerCount, beResultCount;
    std::memcpy(&bePlayerCount, ptr, sizeof(bePlayerCount));
    ptr += sizeof(bePlayerCount);
    std::memcpy(&beResultCount, ptr, sizeof(beResultCount));
    ptr += sizeof(beResultCount);
    uint16_t playerCount = ntohs(bePlayerCount);
    uint16_t resultCount = ntohs(beResultCount);

    // Verify sufficient length for players
    if (static_cast<size_t>(end - ptr) < playerCount * PLAYER_RECORD_SIZE) {
        return false;
    }

    outSnap.players.clear();
    outSnap.players.reserve(playerCount);

    for (uint16_t i = 0; i < playerCount; i++) {
        PlayerSnapshot ps;
        uint32_t beId;
        std::memcpy(&beId, ptr, sizeof(beId));
        ptr += sizeof(beId);
        ps.id = ntohl(beId);

        auto readF = [&ptr]() -> float {
            uint32_t u;
            std::memcpy(&u, ptr, sizeof(u));
            ptr += sizeof(u);
            u = ntohl(u);
            float f;
            std::memcpy(&f, &u, sizeof(f));
            return f;
        };

        ps.x = readF();
        ps.distance = readF();
        ps.speed = readF();
        ps.steer = readF();
        ps.worldX = readF();
        ps.worldY = readF();
        ps.heading = readF();

        uint32_t beAck;
        std::memcpy(&beAck, ptr, sizeof(beAck));
        ptr += sizeof(beAck);
        ps.ackSeq = ntohl(beAck);

        outSnap.players.push_back(ps);
    }

    outSnap.results.clear();
    outSnap.results.reserve(resultCount);

    for (uint16_t i = 0; i < resultCount; i++) {
        if (static_cast<size_t>(end - ptr) < RESULT_HEADER_SIZE) {
            return false;
        }

        FinishResult res;
        uint32_t bePid;
        std::memcpy(&bePid, ptr, sizeof(bePid));
        ptr += sizeof(bePid);
        res.playerId = ntohl(bePid);

        uint16_t beRank;
        std::memcpy(&beRank, ptr, sizeof(beRank));
        ptr += sizeof(beRank);
        res.rank = ntohs(beRank);

        uint32_t uTime;
        std::memcpy(&uTime, ptr, sizeof(uTime));
        ptr += sizeof(uTime);
        uTime = ntohl(uTime);
        float finishTime;
        std::memcpy(&finishTime, &uTime, sizeof(finishTime));
        res.finishTime = finishTime;

        uint8_t nameLen = *ptr++;
        if (static_cast<size_t>(end - ptr) < nameLen) {
            return false;
        }

        if (nameLen > 0) {
            res.name = std::string(reinterpret_cast<const char*>(ptr), nameLen);
            ptr += nameLen;
        }
        outSnap.results.push_back(res);
    }

    return true;
}

bool BinarySnapshot::deserialize(const std::string& buf, WorldSnapshot& outSnap) {
    return deserialize(reinterpret_cast<const uint8_t*>(buf.data()), buf.size(), outSnap);
}
