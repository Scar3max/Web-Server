#pragma once

#include <mutex>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <algorithm>
#include "Player.h"

struct PlayerEvent {
    enum class Type { JOIN, LEAVE };
    Type type;
    uint32_t playerId = 0;
    int socketFd = -1;
    uint64_t connectionId = 0;
};

struct PlayerInput {
    uint32_t playerId = 0;
    uint64_t seq = 0;
    Controls controls;
};

class InputQueue {
private:
    std::mutex queueMutex;
    std::vector<PlayerEvent> eventQueue;
    std::unordered_map<uint32_t, PlayerInput> pendingInputs;

    size_t highWaterEvents = 0;
    size_t highWaterInputs = 0;

public:
    InputQueue() = default;

    // Critical lifecycle events (JOIN / LEAVE) are guaranteed and never dropped.
    void pushEvent(PlayerEvent ev) {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (ev.type == PlayerEvent::Type::LEAVE) {
            // Prune any unconsumed inputs for the departing player
            pendingInputs.erase(ev.playerId);
        }
        eventQueue.push_back(std::move(ev));
        highWaterEvents = std::max(highWaterEvents, eventQueue.size());
    }

    // Input commands are coalesced per-player to the latest sequence number.
    // This prevents input flooding from consuming unbounded memory or lagging the tick loop.
    void pushInput(PlayerInput in) {
        std::lock_guard<std::mutex> lock(queueMutex);
        auto it = pendingInputs.find(in.playerId);
        if (it == pendingInputs.end()) {
            pendingInputs[in.playerId] = std::move(in);
        } else if (in.seq > it->second.seq) {
            it->second = std::move(in);
        }
        highWaterInputs = std::max(highWaterInputs, pendingInputs.size());
    }

    // Drains all lifecycle events and the latest consolidated inputs for all active players.
    void drain(std::vector<PlayerEvent>& outEvents, std::vector<PlayerInput>& outInputs) {
        std::lock_guard<std::mutex> lock(queueMutex);
        outEvents.swap(eventQueue);
        eventQueue.clear();

        outInputs.clear();
        outInputs.reserve(pendingInputs.size());
        for (auto& [pid, in] : pendingInputs) {
            outInputs.push_back(std::move(in));
        }
        pendingInputs.clear();
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(queueMutex);
        return eventQueue.size() + pendingInputs.size();
    }

    size_t getHighWaterEvents() {
        std::lock_guard<std::mutex> lock(queueMutex);
        return highWaterEvents;
    }

    size_t getHighWaterInputs() {
        std::lock_guard<std::mutex> lock(queueMutex);
        return highWaterInputs;
    }
};
