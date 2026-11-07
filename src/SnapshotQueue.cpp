#include "SnapshotQueue.h"
#include <unistd.h>
#include <sstream>
#include <iomanip>

std::string WorldSnapshot::toJson() const {
    std::ostringstream oss;
    oss << "{\"type\":\"snapshot\",\"tick\":" << tick
        << ",\"timestamp\":" << timestamp
        << ",\"players\":[";

    for (size_t i = 0; i < players.size(); i++) {
        if (i > 0) oss << ",";
        const auto& p = players[i];
        oss << "{\"id\":" << p.id
            << ",\"x\":" << std::fixed << std::setprecision(4) << p.x
            << ",\"distance\":" << std::fixed << std::setprecision(2) << p.distance
            << ",\"speed\":" << std::fixed << std::setprecision(2) << p.speed
            << ",\"steer\":" << std::fixed << std::setprecision(4) << p.steer
            << "}";
    }
    oss << "]}";
    return oss.str();
}

void SnapshotQueue::push(WorldSnapshot snapshot) {
    int targetFd = -1;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (snapshots.size() >= MAX_CAPACITY) {
            snapshots.pop_front(); // Drop oldest to prevent unbounded queue bloat
        }
        snapshots.push_back(std::move(snapshot));
        targetFd = wakeupFd;
    }

    if (targetFd != -1) {
        uint64_t one = 1;
        ssize_t written = write(targetFd, &one, sizeof(one));
        (void)written;
    }
}

std::vector<WorldSnapshot> SnapshotQueue::popAll() {
    std::lock_guard<std::mutex> lock(queueMutex);
    std::vector<WorldSnapshot> result(snapshots.begin(), snapshots.end());
    snapshots.clear();
    return result;
}

size_t SnapshotQueue::size() {
    std::lock_guard<std::mutex> lock(queueMutex);
    return snapshots.size();
}
