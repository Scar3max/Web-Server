#pragma once

#include <string>
#include <vector>
#include "Room.h"
#include "Json.h"

namespace RoomProtocol {

inline std::string makeError(const std::string& message) {
    json::Value root(json::Type::OBJECT);
    root["type"] = "room_error";
    root["message"] = message;
    return root.serialize();
}

inline std::string makeRoomCreated(const Room& room, uint32_t playerId) {
    json::Value root(json::Type::OBJECT);
    root["type"] = "room_created";
    root["roomId"] = static_cast<int64_t>(room.id);
    root["roomCode"] = room.code;
    root["playerId"] = static_cast<int64_t>(playerId);
    root["hostId"] = static_cast<int64_t>(room.hostPlayerId);
    root["room"] = room.toJson();
    return root.serialize();
}

inline std::string makeRoomJoined(const Room& room, uint32_t playerId) {
    json::Value root(json::Type::OBJECT);
    root["type"] = "room_joined";
    root["roomId"] = static_cast<int64_t>(room.id);
    root["roomCode"] = room.code;
    root["playerId"] = static_cast<int64_t>(playerId);
    root["room"] = room.toJson();
    return root.serialize();
}

inline std::string makeRoomUpdated(const Room& room) {
    json::Value root(json::Type::OBJECT);
    root["type"] = "room_updated";
    root["roomId"] = static_cast<int64_t>(room.id);
    root["roomCode"] = room.code;
    root["room"] = room.toJson();
    return root.serialize();
}

inline std::string makePlayerJoined(const Room& room, uint32_t joinedPlayerId, const std::string& name) {
    json::Value root(json::Type::OBJECT);
    root["type"] = "player_joined";
    root["roomId"] = static_cast<int64_t>(room.id);
    root["roomCode"] = room.code;
    root["playerId"] = static_cast<int64_t>(joinedPlayerId);
    root["name"] = name;
    root["room"] = room.toJson();
    return root.serialize();
}

inline std::string makePlayerLeft(const Room& room, uint32_t leftPlayerId, uint32_t newHostId) {
    json::Value root(json::Type::OBJECT);
    root["type"] = "player_left";
    root["roomId"] = static_cast<int64_t>(room.id);
    root["roomCode"] = room.code;
    root["playerId"] = static_cast<int64_t>(leftPlayerId);
    root["newHostId"] = static_cast<int64_t>(newHostId);
    root["room"] = room.toJson();
    return root.serialize();
}

inline std::string makeReadyUpdated(const Room& room, uint32_t playerId, bool ready) {
    json::Value root(json::Type::OBJECT);
    root["type"] = "ready_updated";
    root["roomId"] = static_cast<int64_t>(room.id);
    root["roomCode"] = room.code;
    root["playerId"] = static_cast<int64_t>(playerId);
    root["ready"] = ready;
    root["room"] = room.toJson();
    return root.serialize();
}

inline std::string makeCountdown(const Room& room, double countdownRemaining) {
    json::Value root(json::Type::OBJECT);
    root["type"] = "countdown";
    root["roomId"] = static_cast<int64_t>(room.id);
    root["roomCode"] = room.code;
    root["countdown"] = countdownRemaining;
    root["room"] = room.toJson();
    return root.serialize();
}

inline std::string makeRaceStarted(const Room& room) {
    json::Value root(json::Type::OBJECT);
    root["type"] = "race_started";
    root["roomId"] = static_cast<int64_t>(room.id);
    root["roomCode"] = room.code;
    root["trackLength"] = room.trackLength;
    root["room"] = room.toJson();
    return root.serialize();
}

inline std::string makeRaceFinished(const Room& room) {
    json::Value root(json::Type::OBJECT);
    root["type"] = "race_finished";
    root["roomId"] = static_cast<int64_t>(room.id);
    root["roomCode"] = room.code;
    json::Value resArr(json::Type::ARRAY);
    for (const auto& res : room.results) {
        resArr.push_back(res.toJson());
    }
    root["results"] = std::move(resArr);
    root["room"] = room.toJson();
    return root.serialize();
}

} // namespace RoomProtocol
