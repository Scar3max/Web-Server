#pragma once

#include <mutex>
#include <deque>
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>
#include "Room.h"

struct PlayerSnapshot {
    uint32_t id = 0;
    double x = 0.0;
    double distance = 0.0;
    double speed = 0.0;
    double steer = 0.0;
    double worldX = 0.0;
    double worldY = 0.0;
    double heading = 0.0;
    uint64_t ackSeq = 0; // Sequence number of latest client input applied by server
};

struct WorldSnapshot {
    uint32_t roomId = 0;
    std::string roomCode;
    RoomState roomState = RoomState::WAITING;
    double countdown = 0.0;
    uint64_t tick = 0;
    uint64_t timestamp = 0;
    std::vector<PlayerSnapshot> players;
    std::vector<FinishResult> results;

    std::string toJson() const;
};

class SnapshotQueue {
private:
    std::mutex queueMutex;
    std::deque<WorldSnapshot> snapshots;
    int wakeupFd = -1;
    static constexpr size_t MAX_CAPACITY = 256;
    size_t highWaterMark = 0;
    uint64_t totalPushed = 0;
    uint64_t totalDropped = 0;

public:
    SnapshotQueue() = default;

    void setWakeupFd(int fd) {
        std::lock_guard<std::mutex> lock(queueMutex);
        wakeupFd = fd;
    }

    void push(WorldSnapshot snapshot);
    std::vector<WorldSnapshot> popAll();
    bool popLatest(WorldSnapshot& outSnap);
    std::vector<WorldSnapshot> popLatestPerRoom();
    size_t size();
    size_t getHighWaterMark();
    uint64_t getTotalPushed();
    uint64_t getTotalDropped();
};
