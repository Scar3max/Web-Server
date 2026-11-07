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
public:
    static constexpr size_t MAX_LIFECYCLE_CAPACITY = 256;
    static constexpr size_t MAX_ACTIVE_PLAYERS     = 128;

private:
    std::mutex queueMutex;
    std::vector<PlayerEvent> eventQueue;
    std::unordered_map<uint32_t, PlayerInput> pendingInputs;

    // Capacity reservation: we reserve 1 LEAVE slot for every active admitted player
    // to guarantee that accepted players are NEVER blocked or dropped when leaving.
    size_t activeAdmittedPlayers = 0;

    // Metrics & Diagnostics
    size_t highWaterEvents = 0;
    size_t highWaterInputs = 0;
    uint64_t totalJoinsAttempted = 0;
    uint64_t totalJoinsAdmitted = 0;
    uint64_t totalJoinsRejected = 0;
    uint64_t totalLeavesPushed = 0;

public:
    InputQueue() = default;

    // Attempts to admit a new player JOIN.
    // Rejects the JOIN if total admitted players reaches MAX_ACTIVE_PLAYERS
    // or if the event queue cannot accommodate the JOIN plus reserved LEAVE capacity.
    bool pushJoin(PlayerEvent ev) {
        std::lock_guard<std::mutex> lock(queueMutex);
        totalJoinsAttempted++;

        // Reject if active player limit reached
        if (activeAdmittedPlayers >= MAX_ACTIVE_PLAYERS) {
            totalJoinsRejected++;
            return false;
        }

        // Bounded capacity check:
        // Current pending events + 1 (for this JOIN) + reserved slots for all active players (including this new one)
        // must fit within MAX_LIFECYCLE_CAPACITY.
        if (eventQueue.size() + 1 + (activeAdmittedPlayers + 1) > MAX_LIFECYCLE_CAPACITY) {
            totalJoinsRejected++;
            return false;
        }

        activeAdmittedPlayers++;
        totalJoinsAdmitted++;
        eventQueue.push_back(std::move(ev));
        highWaterEvents = std::max(highWaterEvents, eventQueue.size());
        return true;
    }

    // Pushes a player LEAVE event.
    // Guaranteed to succeed because capacity was explicitly reserved during pushJoin.
    // Immediately purges any unconsumed pending inputs for the departing player.
    bool pushLeave(PlayerEvent ev) {
        std::lock_guard<std::mutex> lock(queueMutex);
        totalLeavesPushed++;

        // 1. Immediately purge any unconsumed inputs so disconnected player cannot move vehicle
        pendingInputs.erase(ev.playerId);

        // 2. Decrement admitted player count
        if (activeAdmittedPlayers > 0) {
            activeAdmittedPlayers--;
        }

        // 3. Enqueue LEAVE event
        if (eventQueue.size() < MAX_LIFECYCLE_CAPACITY) {
            eventQueue.push_back(std::move(ev));
            highWaterEvents = std::max(highWaterEvents, eventQueue.size());
            return true;
        }
        return false;
    }

    // Input commands are coalesced per-player to the latest sequence number.
    // This prevents input flooding from consuming unbounded memory or lagging the tick loop.
    bool pushInput(PlayerInput in) {
        std::lock_guard<std::mutex> lock(queueMutex);
        auto it = pendingInputs.find(in.playerId);
        if (it == pendingInputs.end()) {
            if (pendingInputs.size() >= MAX_ACTIVE_PLAYERS) {
                return false;
            }
            pendingInputs[in.playerId] = std::move(in);
        } else if (in.seq > it->second.seq) {
            it->second = std::move(in);
        }
        highWaterInputs = std::max(highWaterInputs, pendingInputs.size());
        return true;
    }

    // Drains all lifecycle events and consolidated inputs for the simulation tick.
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

    size_t getLifecycleQueueSize() {
        std::lock_guard<std::mutex> lock(queueMutex);
        return eventQueue.size();
    }

    size_t getHighWaterEvents() {
        std::lock_guard<std::mutex> lock(queueMutex);
        return highWaterEvents;
    }

    size_t getHighWaterInputs() {
        std::lock_guard<std::mutex> lock(queueMutex);
        return highWaterInputs;
    }

    size_t getActiveAdmittedPlayers() {
        std::lock_guard<std::mutex> lock(queueMutex);
        return activeAdmittedPlayers;
    }

    uint64_t getTotalJoinsAttempted() {
        std::lock_guard<std::mutex> lock(queueMutex);
        return totalJoinsAttempted;
    }

    uint64_t getTotalJoinsAdmitted() {
        std::lock_guard<std::mutex> lock(queueMutex);
        return totalJoinsAdmitted;
    }

    uint64_t getTotalJoinsRejected() {
        std::lock_guard<std::mutex> lock(queueMutex);
        return totalJoinsRejected;
    }

    uint64_t getTotalLeavesPushed() {
        std::lock_guard<std::mutex> lock(queueMutex);
        return totalLeavesPushed;
    }
};
