#pragma once

#include <mutex>
#include <vector>
#include <queue>
#include <cstdint>
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
    std::vector<PlayerInput> inputQueue;
    static constexpr size_t MAX_CAPACITY = 2048;

public:
    InputQueue() = default;

    bool pushEvent(PlayerEvent ev) {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (eventQueue.size() >= MAX_CAPACITY) {
            return false;
        }
        eventQueue.push_back(std::move(ev));
        return true;
    }

    bool pushInput(PlayerInput in) {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (inputQueue.size() >= MAX_CAPACITY) {
            return false;
        }
        inputQueue.push_back(std::move(in));
        return true;
    }

    void drain(std::vector<PlayerEvent>& outEvents, std::vector<PlayerInput>& outInputs) {
        std::lock_guard<std::mutex> lock(queueMutex);
        outEvents.swap(eventQueue);
        eventQueue.clear();
        outInputs.swap(inputQueue);
        inputQueue.clear();
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(queueMutex);
        return eventQueue.size() + inputQueue.size();
    }
};
