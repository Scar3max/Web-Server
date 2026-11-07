#include "SimulationThread.h"
#include <chrono>
#include <iostream>

SimulationThread::SimulationThread(InputQueue& inQ, SnapshotQueue& snapQ)
    : inputQueue(inQ), snapshotQueue(snapQ) {}

SimulationThread::~SimulationThread() {
    stop();
}

void SimulationThread::start() {
    if (running.exchange(true)) {
        return; // Already running
    }
    workerThread = std::thread(&SimulationThread::runLoop, this);
}

void SimulationThread::stop() {
    if (!running.exchange(false)) {
        return;
    }
    if (workerThread.joinable()) {
        workerThread.join();
    }
}

void SimulationThread::runLoop() {
    using clock = std::chrono::steady_clock;
    // 60.000 Hz fixed timestep (1 / 60 s = 16,666,667 ns)
    const auto tickDuration = std::chrono::nanoseconds(16666667);
    auto nextTickTime = clock::now();

    // Diagnostics trackers
    auto lastDiagnosticTime = clock::now();
    uint64_t ticksInWindow = 0;
    uint64_t snapshotsInWindow = 0;

    std::vector<PlayerEvent> events;
    std::vector<PlayerInput> inputs;

    while (running.load(std::memory_order_relaxed)) {
        uint64_t tick = currentTick.fetch_add(1, std::memory_order_relaxed) + 1;
        ticksInWindow++;

        // 1. Drain incoming player events and inputs
        events.clear();
        inputs.clear();
        inputQueue.drain(events, inputs);

        // Process Joins and Leaves
        for (const auto& ev : events) {
            if (ev.type == PlayerEvent::Type::JOIN) {
                PlayerState newState(ev.playerId, ev.socketFd, ev.connectionId);
                // Stagger starting lateral positions across 3 lanes
                double laneOffset = static_cast<double>((ev.playerId - 1) % 3 - 1) * 0.45;
                newState.x = laneOffset;
                players[ev.playerId] = newState;
            } else if (ev.type == PlayerEvent::Type::LEAVE) {
                players.erase(ev.playerId);
            }
        }

        // Process Input Commands
        for (const auto& in : inputs) {
            auto it = players.find(in.playerId);
            if (it != players.end()) {
                // Validate monotonically increasing sequence number
                if (in.seq > it->second.lastInputSeq) {
                    it->second.lastInputSeq = in.seq;
                    it->second.controls = in.controls;
                    it->second.lastInputTime = clock::now();
                }
            }
        }

        // 2. Advance Authoritative Vehicle Physics for all active players
        for (auto& [id, player] : players) {
            physics::updateVehicle(player, player.controls, physics::Config::FIXED_DT);
        }

        activePlayerCount.store(players.size(), std::memory_order_relaxed);

        // 3. Publish Authoritative World Snapshot at 20 Hz (every 3 ticks)
        if (tick % physics::Config::TICKS_PER_SNAPSHOT == 0) {
            WorldSnapshot snapshot;
            snapshot.tick = tick;

            auto nowEpoch = std::chrono::system_clock::now().time_since_epoch();
            snapshot.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(nowEpoch).count();

            snapshot.players.reserve(players.size());
            for (const auto& [id, player] : players) {
                PlayerSnapshot ps;
                ps.id = player.id;
                ps.x = player.x;
                ps.distance = player.distance;
                ps.speed = player.speed;
                ps.steer = player.steerAngle;
                snapshot.players.push_back(ps);
            }

            snapshotQueue.push(std::move(snapshot));
            totalSnapshotsPublished.fetch_add(1, std::memory_order_relaxed);
            snapshotsInWindow++;
        }

        // 4. Calculate actual tick rate and snapshot rate diagnostic every 1 second
        auto now = clock::now();
        auto elapsedDiag = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDiagnosticTime).count();
        if (elapsedDiag >= 1000) {
            double tickRate = (static_cast<double>(ticksInWindow) * 1000.0) / static_cast<double>(elapsedDiag);
            actualTickRate.store(tickRate, std::memory_order_relaxed);

            double snapRate = (static_cast<double>(snapshotsInWindow) * 1000.0) / static_cast<double>(elapsedDiag);
            actualSnapshotRate.store(snapRate, std::memory_order_relaxed);

            ticksInWindow = 0;
            snapshotsInWindow = 0;
            lastDiagnosticTime = now;
        }

        // 5. Fixed-timestep pacing
        nextTickTime += tickDuration;
        if (nextTickTime > now) {
            std::this_thread::sleep_until(nextTickTime);
        } else if (now - nextTickTime > std::chrono::milliseconds(100)) {
            // Prevent spiral-of-death catch-up loops during extreme stalls (bounded catch-up)
            nextTickTime = now;
        }
    }
}
