#pragma once

#include <mutex>
#include <deque>
#include <vector>
#include <string>
#include <cstdint>

struct PlayerSnapshot {
    uint32_t id = 0;
    double x = 0.0;
    double distance = 0.0;
    double speed = 0.0;
    double steer = 0.0;
};

struct WorldSnapshot {
    uint64_t tick = 0;
    uint64_t timestamp = 0;
    std::vector<PlayerSnapshot> players;

    std::string toJson() const;
};

class SnapshotQueue {
private:
    std::mutex queueMutex;
    std::deque<WorldSnapshot> snapshots;
    int wakeupFd = -1;
    static constexpr size_t MAX_CAPACITY = 32;

public:
    SnapshotQueue() = default;

    void setWakeupFd(int fd) {
        std::lock_guard<std::mutex> lock(queueMutex);
        wakeupFd = fd;
    }

    void push(WorldSnapshot snapshot);
    std::vector<WorldSnapshot> popAll();
    size_t size();
};
