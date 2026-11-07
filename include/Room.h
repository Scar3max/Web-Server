#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cstdint>
#include "Json.h"

enum class RoomState {
    WAITING,
    COUNTDOWN,
    RACING,
    FINISHED
};

inline const char* roomStateToString(RoomState state) {
    switch (state) {
        case RoomState::WAITING:   return "WAITING";
        case RoomState::COUNTDOWN: return "COUNTDOWN";
        case RoomState::RACING:    return "RACING";
        case RoomState::FINISHED:  return "FINISHED";
        default:                   return "UNKNOWN";
    }
}

inline RoomState stringToRoomState(const std::string& str) {
    if (str == "COUNTDOWN") return RoomState::COUNTDOWN;
    if (str == "RACING")    return RoomState::RACING;
    if (str == "FINISHED")  return RoomState::FINISHED;
    return RoomState::WAITING;
}

struct RoomMember {
    uint32_t playerId = 0;
    int socketFd = -1;
    std::string name;
    bool ready = false;
    bool isHost = false;
    uint32_t lane = 0;
    bool finished = false;
    double finishTime = 0.0;
    uint32_t finishRank = 0;
    std::chrono::steady_clock::time_point joinTime{};

    RoomMember() = default;
    RoomMember(uint32_t pid, int fd, const std::string& playerName, bool host, uint32_t pLane)
        : playerId(pid), socketFd(fd), name(playerName), ready(false), isHost(host),
          lane(pLane), joinTime(std::chrono::steady_clock::now()) {}

    json::Value toJson() const {
        json::Value obj(json::Type::OBJECT);
        obj["playerId"] = static_cast<int64_t>(playerId);
        obj["name"] = name;
        obj["ready"] = ready;
        obj["isHost"] = isHost;
        obj["lane"] = static_cast<int64_t>(lane);
        obj["finished"] = finished;
        obj["finishTime"] = finishTime;
        obj["finishRank"] = static_cast<int64_t>(finishRank);
        return obj;
    }
};

struct FinishResult {
    uint32_t playerId = 0;
    std::string name;
    uint32_t rank = 0;
    double finishTime = 0.0;

    json::Value toJson() const {
        json::Value obj(json::Type::OBJECT);
        obj["playerId"] = static_cast<int64_t>(playerId);
        obj["name"] = name;
        obj["rank"] = static_cast<int64_t>(rank);
        obj["finishTime"] = finishTime;
        return obj;
    }
};

struct Room {
    uint32_t id = 0;
    std::string code;
    uint32_t hostPlayerId = 0;
    RoomState state = RoomState::WAITING;
    size_t capacity = 8;
    double trackLength = 1500.0;       // Configurable race distance in meters
    double countdownSeconds = 5.0;     // Authoritative 5.0 second countdown

    std::unordered_map<uint32_t, RoomMember> members;
    std::vector<uint32_t> joinOrder;   // Order in which players joined for deterministic host transfer
    std::vector<FinishResult> results;
    std::chrono::steady_clock::time_point stateStartTime{};

    Room() = default;
    Room(uint32_t roomId, const std::string& roomCode, uint32_t hostId, size_t cap = 8)
        : id(roomId), code(roomCode), hostPlayerId(hostId), state(RoomState::WAITING),
          capacity(cap), stateStartTime(std::chrono::steady_clock::now()) {}

    bool isFull() const {
        return members.size() >= capacity;
    }

    bool canStartRace() const {
        if (state != RoomState::WAITING) return false;
        if (members.size() < 2) return false;
        for (const auto& [pid, m] : members) {
            if (!m.ready) return false;
        }
        return true;
    }

    json::Value toJson() const {
        json::Value obj(json::Type::OBJECT);
        obj["id"] = static_cast<int64_t>(id);
        obj["code"] = code;
        obj["hostPlayerId"] = static_cast<int64_t>(hostPlayerId);
        obj["state"] = roomStateToString(state);
        obj["capacity"] = static_cast<int64_t>(capacity);
        obj["playerCount"] = static_cast<int64_t>(members.size());
        obj["trackLength"] = trackLength;
        obj["countdownSeconds"] = countdownSeconds;

        json::Value memberArr(json::Type::ARRAY);
        for (uint32_t pid : joinOrder) {
            auto it = members.find(pid);
            if (it != members.end()) {
                memberArr.push_back(it->second.toJson());
            }
        }
        obj["players"] = std::move(memberArr);

        json::Value resArr(json::Type::ARRAY);
        for (const auto& res : results) {
            resArr.push_back(res.toJson());
        }
        obj["results"] = std::move(resArr);

        return obj;
    }
};
