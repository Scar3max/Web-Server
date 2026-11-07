#include "RoomManager.h"
#include "ServerMetrics.h"
#include <algorithm>
#include <cctype>

RoomManager::RoomManager() {
    std::random_device rd;
    rng.seed(rd());
}

std::string RoomManager::generateUniqueCode() {
    static const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // Omit 0, O, 1, I to avoid confusion
    static const size_t charsetSize = sizeof(charset) - 1;
    std::uniform_int_distribution<size_t> dist(0, charsetSize - 1);

    for (int attempts = 0; attempts < 1000; attempts++) {
        std::string code;
        code.reserve(6);
        for (size_t i = 0; i < 6; i++) {
            code += charset[dist(rng)];
        }
        if (codeToRoomId.find(code) == codeToRoomId.end()) {
            return code;
        }
    }
    // Fallback if charset collision happens
    return "RM" + std::to_string(nextRoomId);
}

bool RoomManager::createRoom(uint32_t hostPlayerId, int socketFd, const std::string& hostName,
                             std::string& outCode, uint32_t& outRoomId, std::string& outError) {
    ServerMetrics::instance().multiplayerRoomCreationsAttemptedTotal.fetch_add(1, std::memory_order_relaxed);

    if (playerToRoomId.find(hostPlayerId) != playerToRoomId.end()) {
        outError = "Player is already member of a room";
        ServerMetrics::instance().multiplayerRoomCreationsFailedTotal.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (rooms.size() >= MAX_ROOMS) {
        outError = "Server room limit reached";
        ServerMetrics::instance().multiplayerRoomCreationsFailedTotal.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uint32_t roomId = nextRoomId++;
    std::string code = generateUniqueCode();

    Room room(roomId, code, hostPlayerId, DEFAULT_ROOM_CAPACITY);
    std::string finalName = hostName.empty() ? ("Player " + std::to_string(hostPlayerId)) : hostName;
    RoomMember hostMember(hostPlayerId, socketFd, finalName, true, 0);
    hostMember.ready = false;

    room.members[hostPlayerId] = hostMember;
    room.joinOrder.push_back(hostPlayerId);

    rooms[roomId] = std::move(room);
    codeToRoomId[code] = roomId;
    playerToRoomId[hostPlayerId] = roomId;

    outCode = code;
    outRoomId = roomId;

    ServerMetrics::instance().multiplayerRoomsActive.store(rooms.size(), std::memory_order_relaxed);
    return true;
}

bool RoomManager::joinRoom(const std::string& code, uint32_t playerId, int socketFd,
                           const std::string& playerName, uint32_t& outRoomId, std::string& outError) {
    ServerMetrics::instance().multiplayerRoomJoinsAttemptedTotal.fetch_add(1, std::memory_order_relaxed);

    if (playerToRoomId.find(playerId) != playerToRoomId.end()) {
        outError = "Player is already member of a room";
        ServerMetrics::instance().multiplayerRoomJoinsFailedTotal.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::string normCode = code;
    for (char& c : normCode) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    auto cIt = codeToRoomId.find(normCode);
    if (cIt == codeToRoomId.end()) {
        outError = "Room not found";
        ServerMetrics::instance().multiplayerRoomJoinsFailedTotal.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uint32_t roomId = cIt->second;
    Room& room = rooms[roomId];

    if (room.state != RoomState::WAITING) {
        outError = "Room is currently in progress or finished";
        ServerMetrics::instance().multiplayerRoomJoinsFailedTotal.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (room.isFull()) {
        outError = "Room is full";
        ServerMetrics::instance().multiplayerRoomCapacityRejectionsTotal.fetch_add(1, std::memory_order_relaxed);
        ServerMetrics::instance().multiplayerRoomJoinsFailedTotal.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Determine lane offset
    uint32_t lane = static_cast<uint32_t>(room.members.size());
    std::string finalName = playerName.empty() ? ("Player " + std::to_string(playerId)) : playerName;
    RoomMember member(playerId, socketFd, finalName, false, lane);
    member.ready = false;

    room.members[playerId] = member;
    room.joinOrder.push_back(playerId);
    playerToRoomId[playerId] = roomId;

    outRoomId = roomId;
    return true;
}

bool RoomManager::leaveRoom(uint32_t playerId, uint32_t& outOldRoomId, bool& outRoomDestroyed,
                            uint32_t& outNewHostId, bool& outCountdownCancelled) {
    ServerMetrics::instance().multiplayerPlayerDisconnectsTotal.fetch_add(1, std::memory_order_relaxed);

    outOldRoomId = 0;
    outRoomDestroyed = false;
    outNewHostId = 0;
    outCountdownCancelled = false;

    auto pIt = playerToRoomId.find(playerId);
    if (pIt == playerToRoomId.end()) {
        return false;
    }

    uint32_t roomId = pIt->second;
    outOldRoomId = roomId;
    playerToRoomId.erase(pIt);

    auto rIt = rooms.find(roomId);
    if (rIt == rooms.end()) {
        return false;
    }

    Room& room = rIt->second;
    bool wasHost = (room.hostPlayerId == playerId);

    room.members.erase(playerId);
    auto jIt = std::find(room.joinOrder.begin(), room.joinOrder.end(), playerId);
    if (jIt != room.joinOrder.end()) {
        room.joinOrder.erase(jIt);
    }

    // If room is empty, destroy it
    if (room.members.empty()) {
        codeToRoomId.erase(room.code);
        rooms.erase(rIt);
        outRoomDestroyed = true;
        ServerMetrics::instance().multiplayerRoomsActive.store(rooms.size(), std::memory_order_relaxed);
        return true;
    }

    // Host transfer to earliest joiner
    if (wasHost) {
        uint32_t newHostId = room.joinOrder.front();
        room.hostPlayerId = newHostId;
        room.members[newHostId].isHost = true;
        outNewHostId = newHostId;
    }

    // If room was in COUNTDOWN and any player leaves, cancel the countdown so the remaining racers return to lobby
    if (room.state == RoomState::COUNTDOWN) {
        room.state = RoomState::WAITING;
        outCountdownCancelled = true;
    }

    return true;
}

bool RoomManager::setReady(uint32_t playerId, bool ready, uint32_t& outRoomId,
                           bool& outCountdownCancelled, std::string& outError) {
    outRoomId = 0;
    outCountdownCancelled = false;

    auto pIt = playerToRoomId.find(playerId);
    if (pIt == playerToRoomId.end()) {
        outError = "Player is not in any room";
        return false;
    }

    uint32_t roomId = pIt->second;
    outRoomId = roomId;
    Room& room = rooms[roomId];

    auto mIt = room.members.find(playerId);
    if (mIt == room.members.end()) {
        outError = "Player not in room members";
        return false;
    }

    mIt->second.ready = ready;

    // If player unreadies during COUNTDOWN, abort countdown
    if (!ready && room.state == RoomState::COUNTDOWN) {
        room.state = RoomState::WAITING;
        outCountdownCancelled = true;
    }

    return true;
}

bool RoomManager::startRace(uint32_t hostPlayerId, uint32_t& outRoomId, std::string& outError) {
    outRoomId = 0;

    auto pIt = playerToRoomId.find(hostPlayerId);
    if (pIt == playerToRoomId.end()) {
        outError = "Player is not in any room";
        return false;
    }

    uint32_t roomId = pIt->second;
    outRoomId = roomId;
    Room& room = rooms[roomId];

    if (room.hostPlayerId != hostPlayerId) {
        outError = "Only the room host can start the race";
        return false;
    }

    if (room.state != RoomState::WAITING) {
        outError = "Race cannot be started in current state";
        return false;
    }

    if (room.members.size() < 2) {
        outError = "At least 2 players are required to start a race";
        return false;
    }

    for (const auto& [pid, member] : room.members) {
        if (!member.ready) {
            outError = "All players must be ready to start";
            return false;
        }
    }

    room.state = RoomState::COUNTDOWN;
    room.stateStartTime = std::chrono::steady_clock::now();
    room.results.clear();
    ServerMetrics::instance().multiplayerRaceStartsTotal.fetch_add(1, std::memory_order_relaxed);
    return true;
}

Room* RoomManager::getRoom(uint32_t roomId) {
    auto it = rooms.find(roomId);
    return (it != rooms.end()) ? &it->second : nullptr;
}

const Room* RoomManager::getRoom(uint32_t roomId) const {
    auto it = rooms.find(roomId);
    return (it != rooms.end()) ? &it->second : nullptr;
}

Room* RoomManager::getRoomByCode(const std::string& code) {
    std::string normCode = code;
    for (char& c : normCode) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    auto it = codeToRoomId.find(normCode);
    if (it != codeToRoomId.end()) {
        return getRoom(it->second);
    }
    return nullptr;
}

const Room* RoomManager::getRoomByCode(const std::string& code) const {
    std::string normCode = code;
    for (char& c : normCode) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    auto it = codeToRoomId.find(normCode);
    if (it != codeToRoomId.end()) {
        return getRoom(it->second);
    }
    return nullptr;
}

Room* RoomManager::getRoomForPlayer(uint32_t playerId) {
    auto it = playerToRoomId.find(playerId);
    if (it != playerToRoomId.end()) {
        return getRoom(it->second);
    }
    return nullptr;
}

const Room* RoomManager::getRoomForPlayer(uint32_t playerId) const {
    auto it = playerToRoomId.find(playerId);
    if (it != playerToRoomId.end()) {
        return getRoom(it->second);
    }
    return nullptr;
}

uint32_t RoomManager::getPlayerRoomId(uint32_t playerId) const {
    auto it = playerToRoomId.find(playerId);
    return (it != playerToRoomId.end()) ? it->second : 0;
}
