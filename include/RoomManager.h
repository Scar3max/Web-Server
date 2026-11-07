#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <random>
#include <memory>
#include "Room.h"

class RoomManager {
public:
    static constexpr size_t MAX_ROOMS = 64;
    static constexpr size_t DEFAULT_ROOM_CAPACITY = 8;

private:
    std::unordered_map<uint32_t, Room> rooms;
    std::unordered_map<std::string, uint32_t> codeToRoomId;
    std::unordered_map<uint32_t, uint32_t> playerToRoomId;

    uint32_t nextRoomId = 1;
    std::mt19937 rng;

    std::string generateUniqueCode();

public:
    RoomManager();

    // Room operations
    bool createRoom(uint32_t hostPlayerId, int socketFd, const std::string& hostName,
                    std::string& outCode, uint32_t& outRoomId, std::string& outError);

    bool joinRoom(const std::string& code, uint32_t playerId, int socketFd,
                  const std::string& playerName, uint32_t& outRoomId, std::string& outError);

    bool leaveRoom(uint32_t playerId, uint32_t& outOldRoomId, bool& outRoomDestroyed,
                   uint32_t& outNewHostId, bool& outCountdownCancelled);

    bool setReady(uint32_t playerId, bool ready, uint32_t& outRoomId,
                  bool& outCountdownCancelled, std::string& outError);

    bool startRace(uint32_t hostPlayerId, uint32_t& outRoomId, std::string& outError);

    // Lookups
    Room* getRoom(uint32_t roomId);
    const Room* getRoom(uint32_t roomId) const;

    Room* getRoomByCode(const std::string& code);
    const Room* getRoomByCode(const std::string& code) const;

    Room* getRoomForPlayer(uint32_t playerId);
    const Room* getRoomForPlayer(uint32_t playerId) const;

    uint32_t getPlayerRoomId(uint32_t playerId) const;

    size_t getRoomCount() const { return rooms.size(); }
    const std::unordered_map<uint32_t, Room>& getAllRooms() const { return rooms; }
};
