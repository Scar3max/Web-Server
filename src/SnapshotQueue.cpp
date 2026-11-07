#include "SnapshotQueue.h"
#include <unistd.h>
#include <cstdio>
#include <algorithm>
#include <unordered_map>

std::string WorldSnapshot::toJson() const {
    std::string out;
    out.reserve(256 + players.size() * 140 + results.size() * 64);
    char buf[256];
    int n = std::snprintf(buf, sizeof(buf),
        "{\"type\":\"snapshot\",\"roomId\":%u,\"roomCode\":\"%s\",\"state\":\"%s\",\"countdown\":%.1f,\"tick\":%llu,\"timestamp\":%llu,\"players\":[",
        roomId, roomCode.c_str(), roomStateToString(roomState), countdown,
        static_cast<unsigned long long>(tick), static_cast<unsigned long long>(timestamp));
    if (n > 0) {
        out.append(buf, static_cast<size_t>(n));
    }

    for (size_t i = 0; i < players.size(); i++) {
        if (i > 0) out.push_back(',');
        const auto& p = players[i];
        n = std::snprintf(buf, sizeof(buf),
            "{\"id\":%u,\"x\":%.4f,\"distance\":%.2f,\"speed\":%.2f,\"steer\":%.4f,\"worldX\":%.2f,\"worldY\":%.2f,\"heading\":%.4f,\"ackSeq\":%llu}",
            p.id, p.x, p.distance, p.speed, p.steer, p.worldX, p.worldY, p.heading,
            static_cast<unsigned long long>(p.ackSeq));
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
        }
    }
    out.append("],\"results\":[");
    for (size_t i = 0; i < results.size(); i++) {
        if (i > 0) out.push_back(',');
        const auto& r = results[i];
        n = std::snprintf(buf, sizeof(buf),
            "{\"playerId\":%u,\"name\":\"%s\",\"rank\":%u,\"finishTime\":%.3f}",
            r.playerId, r.name.c_str(), r.rank, r.finishTime);
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
        }
    }
    out.append("]}");
    return out;
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
