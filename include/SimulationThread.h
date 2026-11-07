#pragma once

#include <thread>
#include <atomic>
#include <unordered_map>
#include <chrono>
#include <cstdint>

#include "Player.h"
#include "Physics.h"
#include "InputQueue.h"
#include "SnapshotQueue.h"

class SimulationThread {
private:
    InputQueue& inputQueue;
    SnapshotQueue& snapshotQueue;

    std::thread workerThread;
    std::atomic<bool> running{false};

    std::unordered_map<uint32_t, PlayerState> players;
    uint64_t currentTick = 0;

    // Diagnostics
    std::atomic<double> actualTickRate{60.0};
    std::atomic<size_t> activePlayerCount{0};

    void runLoop();

public:
    SimulationThread(InputQueue& inQ, SnapshotQueue& snapQ);
    ~SimulationThread();

    void start();
    void stop();

    uint64_t getTick() const { return currentTick; }
    double getTickRate() const { return actualTickRate.load(); }
    size_t getPlayerCount() const { return activePlayerCount.load(); }
};
