#include "SnapshotQueue.h"
#include <unistd.h>
#include <sstream>
#include <iomanip>
#include <unordered_map>

std::string WorldSnapshot::toJson() const {
    std::ostringstream oss;
    oss << "{\"type\":\"snapshot\",\"roomId\":" << roomId
        << ",\"roomCode\":\"" << roomCode << "\""
        << ",\"state\":\"" << roomStateToString(roomState) << "\""
        << ",\"countdown\":" << std::fixed << std::setprecision(1) << countdown
        << ",\"tick\":" << tick
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
            << ",\"worldX\":" << std::fixed << std::setprecision(2) << p.worldX
            << ",\"worldY\":" << std::fixed << std::setprecision(2) << p.worldY
            << ",\"heading\":" << std::fixed << std::setprecision(4) << p.heading
            << "}";
    }
    oss << "],\"results\":[";
    for (size_t i = 0; i < results.size(); i++) {
        if (i > 0) oss << ",";
        const auto& r = results[i];
        oss << "{\"playerId\":" << r.playerId
            << ",\"name\":\"" << r.name << "\""
            << ",\"rank\":" << r.rank
            << ",\"finishTime\":" << std::fixed << std::setprecision(3) << r.finishTime
            << "}";
    }
    oss << "]}";
    return oss.str();
}

void SnapshotQueue::push(WorldSnapshot snapshot) {
    int targetFd = -1;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        totalPushed++;
        if (snapshots.size() >= MAX_CAPACITY) {
            snapshots.pop_front(); // Drop oldest to prevent unbounded queue bloat
            totalDropped++;
        }
        snapshots.push_back(std::move(snapshot));
        if (snapshots.size() > highWaterMark) {
            highWaterMark = snapshots.size();
        }
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

bool SnapshotQueue::popLatest(WorldSnapshot& outSnap) {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (snapshots.empty()) {
        return false;
    }
    outSnap = std::move(snapshots.back());
    snapshots.clear();
    return true;
}

std::vector<WorldSnapshot> SnapshotQueue::popLatestPerRoom() {
    std::lock_guard<std::mutex> lock(queueMutex);
    std::unordered_map<uint32_t, WorldSnapshot> latestByRoom;
    for (auto& snap : snapshots) {
        latestByRoom[snap.roomId] = std::move(snap);
    }
    snapshots.clear();
    std::vector<WorldSnapshot> result;
    result.reserve(latestByRoom.size());
    for (auto& [rid, snap] : latestByRoom) {
        result.push_back(std::move(snap));
    }
    return result;
}

size_t SnapshotQueue::size() {
    std::lock_guard<std::mutex> lock(queueMutex);
    return snapshots.size();
}

size_t SnapshotQueue::getHighWaterMark() {
    std::lock_guard<std::mutex> lock(queueMutex);
    return highWaterMark;
}

uint64_t SnapshotQueue::getTotalPushed() {
    std::lock_guard<std::mutex> lock(queueMutex);
    return totalPushed;
}

uint64_t SnapshotQueue::getTotalDropped() {
    std::lock_guard<std::mutex> lock(queueMutex);
    return totalDropped;
}
