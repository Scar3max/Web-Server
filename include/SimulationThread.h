#pragma once

#include <thread>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <string>
#include <chrono>
#include <cstdint>

#include "Player.h"
#include "Physics.h"
#include "InputQueue.h"
#include "SnapshotQueue.h"
#include "Room.h"

struct RoomSim {
    uint32_t roomId = 0;
    std::string roomCode;
    RoomState state = RoomState::WAITING;
    double countdownRemaining = 5.0;
    double trackLength = 1500.0;
    std::chrono::steady_clock::time_point raceStartTime{};
    std::unordered_map<uint32_t, PlayerState> vehicles;
    std::unordered_map<uint32_t, std::string> playerNames;
    std::vector<FinishResult> results;
    uint32_t nextFinishRank = 1;
};

class SimulationThread {
private:
    InputQueue& inputQueue;
    SnapshotQueue& snapshotQueue;

    std::thread workerThread;
    std::atomic<bool> running{false};

    std::unordered_map<uint32_t, RoomSim> roomSims;
    std::atomic<uint64_t> currentTick{0};

    // Diagnostics
    std::atomic<double> actualTickRate{60.0};
    std::atomic<double> actualSnapshotRate{20.0};
    std::atomic<size_t> activePlayerCount{0};
    std::atomic<uint64_t> totalSnapshotsPublished{0};

    void runLoop();

public:
    SimulationThread(InputQueue& inQ, SnapshotQueue& snapQ);
    ~SimulationThread();

    void start();
    void stop();

    uint64_t getTick() const { return currentTick.load(std::memory_order_relaxed); }
    double getTickRate() const { return actualTickRate.load(std::memory_order_relaxed); }
    double getSnapshotRate() const { return actualSnapshotRate.load(std::memory_order_relaxed); }
    size_t getPlayerCount() const { return activePlayerCount.load(std::memory_order_relaxed); }
    uint64_t getTotalSnapshots() const { return totalSnapshotsPublished.load(std::memory_order_relaxed); }

    InputQueue& getInputQueue() { return inputQueue; }
    SnapshotQueue& getSnapshotQueue() { return snapshotQueue; }
};
