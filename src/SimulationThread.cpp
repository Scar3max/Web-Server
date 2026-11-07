#include "SimulationThread.h"
#include "ServerMetrics.h"
#include <chrono>
#include <iostream>
#include <algorithm>

SimulationThread::SimulationThread(InputQueue& inQ, SnapshotQueue& snapQ)
    : inputQueue(inQ), snapshotQueue(snapQ) {
    // Initialize default global world (roomId = 0) for Phase 4 backward compatibility
    RoomSim defaultRoom;
    defaultRoom.roomId = 0;
    defaultRoom.roomCode = "DEFAULT";
    defaultRoom.state = RoomState::RACING;
    defaultRoom.trackLength = 1000000.0; // effectively continuous driving
    roomSims[0] = std::move(defaultRoom);
}

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
        auto tickStartTime = clock::now();
        uint64_t tick = currentTick.fetch_add(1, std::memory_order_relaxed) + 1;
        ticksInWindow++;

        // 1. Drain incoming player events and inputs
        events.clear();
        inputs.clear();
        inputQueue.drain(events, inputs);

        // Process Lifecycle Events
        for (const auto& ev : events) {
            if (ev.type == PlayerEvent::Type::JOIN) {
                // Default global room join (Phase 4 compatibility)
                PlayerState newState(ev.playerId, ev.socketFd, ev.connectionId, 0, ev.playerName);
                double laneOffset = static_cast<double>(static_cast<int>(ev.playerId - 1) % 3 - 1) * 0.45;
                newState.x = laneOffset;
                roomSims[0].vehicles[ev.playerId] = newState;
                roomSims[0].playerNames[ev.playerId] = ev.playerName;
            } else if (ev.type == PlayerEvent::Type::LEAVE) {
                // Leave from any room containing this player
                for (auto& [rid, rsim] : roomSims) {
                    rsim.vehicles.erase(ev.playerId);
                    rsim.playerNames.erase(ev.playerId);
                    if (rid != 0 && rsim.state == RoomState::COUNTDOWN && rsim.vehicles.size() < 2) {
                        rsim.state = RoomState::WAITING;
                        rsim.countdownRemaining = 5.0;
                    }
                }
            } else if (ev.type == PlayerEvent::Type::ROOM_JOIN) {
                // Ensure player is cleanly removed from practice world (room 0) to avoid ghost vehicles
                roomSims[0].vehicles.erase(ev.playerId);
                roomSims[0].playerNames.erase(ev.playerId);

                auto& rsim = roomSims[ev.roomId];
                rsim.roomId = ev.roomId;
                rsim.roomCode = ev.roomCode;
                rsim.trackLength = ev.trackLength;

                PlayerState newState(ev.playerId, ev.socketFd, ev.connectionId, ev.roomId, ev.playerName);
                double laneOffset = static_cast<double>(static_cast<int>(ev.lane % 3) - 1) * 0.45;
                newState.x = laneOffset;
                rsim.vehicles[ev.playerId] = newState;
                rsim.playerNames[ev.playerId] = ev.playerName;
            } else if (ev.type == PlayerEvent::Type::ROOM_LEAVE) {
                auto rIt = roomSims.find(ev.roomId);
                if (rIt != roomSims.end()) {
                    rIt->second.vehicles.erase(ev.playerId);
                    rIt->second.playerNames.erase(ev.playerId);
                    if (rIt->second.state == RoomState::COUNTDOWN) {
                        rIt->second.state = RoomState::WAITING;
                        rIt->second.countdownRemaining = 5.0;
                    }
                }
                // Restore clean practice state in global practice room (room 0)
                PlayerState practiceState(ev.playerId, ev.socketFd, ev.connectionId, 0, ev.playerName);
                double laneOffset = static_cast<double>(static_cast<int>(ev.playerId - 1) % 3 - 1) * 0.45;
                practiceState.x = laneOffset;
                roomSims[0].vehicles[ev.playerId] = practiceState;
                roomSims[0].playerNames[ev.playerId] = ev.playerName;
            } else if (ev.type == PlayerEvent::Type::START_COUNTDOWN) {
                auto rIt = roomSims.find(ev.roomId);
                if (rIt != roomSims.end()) {
                    auto& rsim = rIt->second;
                    rsim.state = RoomState::COUNTDOWN;
                    rsim.countdownRemaining = 5.0;
                    rsim.results.clear();
                    rsim.nextFinishRank = 1;
                    rsim.trackLength = ev.trackLength;

                    // Reset vehicle kinematics and lanes for starting grid
                    uint32_t laneIdx = 0;
                    for (auto& [pid, v] : rsim.vehicles) {
                        v.distance = 0.0;
                        v.speed = 0.0;
                        v.steerAngle = 0.0;
                        v.finished = false;
                        v.finishTime = 0.0;
                        v.finishRank = 0;
                        v.controls = {};
                        v.x = static_cast<double>(static_cast<int>(laneIdx % 3) - 1) * 0.45;
                        laneIdx++;
                    }
                }
            } else if (ev.type == PlayerEvent::Type::CANCEL_COUNTDOWN) {
                auto rIt = roomSims.find(ev.roomId);
                if (rIt != roomSims.end()) {
                    rIt->second.state = RoomState::WAITING;
                    rIt->second.countdownRemaining = 5.0;
                }
            } else if (ev.type == PlayerEvent::Type::DESTROY_ROOM) {
                roomSims.erase(ev.roomId);
            }
        }

        // Process Input Commands
        for (const auto& in : inputs) {
            auto rIt = roomSims.find(in.roomId);
            if (rIt != roomSims.end()) {
                auto& rsim = rIt->second;
                // Allow driving inputs only during RACING state
                if (rsim.state == RoomState::RACING) {
                    auto pIt = rsim.vehicles.find(in.playerId);
                    if (pIt != rsim.vehicles.end() && !pIt->second.finished) {
                        if (in.seq > pIt->second.lastInputSeq) {
                            pIt->second.lastInputSeq = in.seq;
                            pIt->second.controls = in.controls;
                            pIt->second.lastInputTime = clock::now();
                        }
                    }
                }
            }
        }

        // 2. Advance Authoritative Simulation for all Rooms
        size_t totalPlayers = 0;

        for (auto& [rid, rsim] : roomSims) {
            totalPlayers += rsim.vehicles.size();

            if (rsim.state == RoomState::COUNTDOWN) {
                rsim.countdownRemaining -= physics::Config::FIXED_DT;
                if (rsim.countdownRemaining <= 0.0) {
                    rsim.countdownRemaining = 0.0;
                    rsim.state = RoomState::RACING;
                    rsim.raceStartTime = clock::now();
                }
            } else if (rsim.state == RoomState::RACING) {
                bool allFinished = !rsim.vehicles.empty();

                for (auto& [pid, player] : rsim.vehicles) {
                    if (rid == 0) {
                        // Global practice world: continuous driving
                        physics::updateVehicle(player, player.controls, physics::Config::FIXED_DT);
                        allFinished = false;
                    } else {
                        if (player.finished) {
                            // Finished player: apply braking to bring car to rest cleanly
                            Controls stopControls{false, true, false, false};
                            physics::updateVehicle(player, stopControls, physics::Config::FIXED_DT);
                        } else {
                            physics::updateVehicle(player, player.controls, physics::Config::FIXED_DT);
                            if (player.distance >= rsim.trackLength) {
                                player.finished = true;
                                player.speed = std::min(player.speed, 60.0);
                                double elapsed = static_cast<double>(
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        clock::now() - rsim.raceStartTime).count()
                                ) / 1000.0;
                                player.finishTime = elapsed;
                                player.finishRank = rsim.nextFinishRank++;

                                FinishResult res;
                                res.playerId = pid;
                                res.name = rsim.playerNames[pid];
                                res.rank = player.finishRank;
                                res.finishTime = player.finishTime;
                                rsim.results.push_back(res);
                            } else {
                                allFinished = false;
                            }
                        }
                    }
                }

                if (rid != 0 && allFinished) {
                    rsim.state = RoomState::FINISHED;
                }

                // Resolve authoritative vehicle-to-vehicle collisions within this room
                auto colStart = clock::now();
                std::vector<physics::CollisionEvent> colEvents;
                physics::resolveRoomCollisions(rsim.vehicles, track::getDefaultTrack(), &colEvents);
                auto colEnd = clock::now();
                uint64_t colMicros = std::chrono::duration_cast<std::chrono::microseconds>(colEnd - colStart).count();
                ServerMetrics::instance().recordCollisionSolver(colMicros, colEvents.size());
            } else if (rid == 0 && !rsim.vehicles.empty()) {
                // Practice mode: resolve collisions between practice drivers
                auto colStart = clock::now();
                std::vector<physics::CollisionEvent> colEvents;
                physics::resolveRoomCollisions(rsim.vehicles, track::getDefaultTrack(), &colEvents);
                auto colEnd = clock::now();
                uint64_t colMicros = std::chrono::duration_cast<std::chrono::microseconds>(colEnd - colStart).count();
                ServerMetrics::instance().recordCollisionSolver(colMicros, colEvents.size());
            }
        }

        activePlayerCount.store(totalPlayers, std::memory_order_relaxed);
        ServerMetrics::instance().multiplayerPlayersActive.store(totalPlayers, std::memory_order_relaxed);
        size_t practiceCount = (roomSims.find(0) != roomSims.end()) ? roomSims[0].vehicles.size() : 0;
        ServerMetrics::instance().multiplayerPracticePlayersActive.store(practiceCount, std::memory_order_relaxed);
        ServerMetrics::instance().simulationInputsProcessedTotal.fetch_add(inputs.size(), std::memory_order_relaxed);

        // 3. Publish Authoritative Room Snapshots at 20 Hz (every 3 ticks)
        if (tick % physics::Config::TICKS_PER_SNAPSHOT == 0) {
            auto nowEpoch = std::chrono::system_clock::now().time_since_epoch();
            uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(nowEpoch).count();

            for (const auto& [rid, rsim] : roomSims) {
                if (rid != 0 && (rsim.vehicles.empty() || rsim.state == RoomState::WAITING)) continue;

                WorldSnapshot snapshot;
                snapshot.roomId = rid;
                snapshot.roomCode = rsim.roomCode;
                snapshot.roomState = rsim.state;
                snapshot.countdown = rsim.countdownRemaining;
                snapshot.tick = tick;
                snapshot.timestamp = timestamp;
                snapshot.results = rsim.results;

                snapshot.players.reserve(rsim.vehicles.size());
                for (const auto& [id, player] : rsim.vehicles) {
                    PlayerSnapshot ps;
                    ps.id = player.id;
                    ps.x = player.x;
                    ps.distance = player.distance;
                    ps.speed = player.speed;
                    ps.steer = player.steerAngle;
                    ps.worldX = player.worldX;
                    ps.worldY = player.worldY;
                    ps.heading = player.heading;
                    ps.ackSeq = player.lastInputSeq;
                    snapshot.players.push_back(ps);
                }

                snapshotQueue.push(std::move(snapshot));
                totalSnapshotsPublished.fetch_add(1, std::memory_order_relaxed);
                snapshotsInWindow++;
            }
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

        // Measure tick duration before pacing sleep
        auto tickWorkEnd = clock::now();
        uint64_t tickMicros = std::chrono::duration_cast<std::chrono::microseconds>(tickWorkEnd - tickStartTime).count();
        bool missedDeadline = (tickMicros > 16667);
        bool overrun = (now - nextTickTime > std::chrono::milliseconds(100));

        ServerMetrics::instance().recordSimulationTick(tickMicros, missedDeadline, overrun);
        ServerMetrics::instance().simulationTick.store(tick, std::memory_order_relaxed);
        ServerMetrics::instance().simulationTickFrequencyHz.store(actualTickRate.load(std::memory_order_relaxed), std::memory_order_relaxed);
        ServerMetrics::instance().simulationSnapshotFrequencyHz.store(actualSnapshotRate.load(std::memory_order_relaxed), std::memory_order_relaxed);
        ServerMetrics::instance().simulationSnapshotsPublishedTotal.store(totalSnapshotsPublished.load(std::memory_order_relaxed), std::memory_order_relaxed);
        ServerMetrics::instance().simulationSnapshotQueueDepth.store(snapshotQueue.size(), std::memory_order_relaxed);
        ServerMetrics::instance().simulationSnapshotsDroppedTotal.store(snapshotQueue.getTotalDropped(), std::memory_order_relaxed);
        ServerMetrics::instance().simulationInputQueueDepth.store(inputQueue.size(), std::memory_order_relaxed);
        ServerMetrics::instance().simulationLifecycleQueueOccupancy.store(inputQueue.getLifecycleQueueSize(), std::memory_order_relaxed);
        ServerMetrics::instance().simulationLifecycleEventsRejectedTotal.store(inputQueue.getTotalJoinsRejected(), std::memory_order_relaxed);

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
