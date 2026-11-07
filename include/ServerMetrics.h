#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <sstream>
#include <vector>
#include <chrono>
#include <array>

/**
 * ServerMetrics — High-performance, lock-free observability and instrumentation.
 *
 * Utilizes atomic counters, gauges, and timing accumulators to provide
 * zero-allocation, non-blocking metrics collection across HTTP, WebSocket,
 * Room, Simulation, and Backpressure subsystems.
 */
class ServerMetrics {
public:
    static ServerMetrics& instance();

    // ==========================================================
    // 1. HTTP & Network Metrics
    // ==========================================================
    std::atomic<uint64_t> httpRequests200{0};
    std::atomic<uint64_t> httpRequests400{0};
    std::atomic<uint64_t> httpRequests404{0};
    std::atomic<uint64_t> httpRequests413{0};
    std::atomic<uint64_t> httpRequests431{0};
    std::atomic<uint64_t> httpRequests500{0};
    std::atomic<uint64_t> httpRequestsOther{0};

    std::atomic<int64_t>  tcpConnectionsActive{0};
    std::atomic<uint64_t> tcpConnectionsAcceptedTotal{0};
    std::atomic<uint64_t> tcpConnectionsClosedTotal{0};

    std::atomic<uint64_t> wsConnectionsTotal{0};
    std::atomic<int64_t>  wsConnectionsActive{0};
    std::atomic<uint64_t> wsFramesReceivedTotal{0};
    std::atomic<uint64_t> wsFramesSentTotal{0};
    std::atomic<uint64_t> wsProtocolErrorsTotal{0};

    std::atomic<uint64_t> networkBytesReceivedTotal{0};
    std::atomic<uint64_t> networkBytesTransmittedTotal{0};
    std::atomic<uint64_t> networkPartialWritesTotal{0};
    std::atomic<uint64_t> networkEagainTotal{0};
    std::atomic<uint64_t> networkConnectionDropsTotal{0};
    std::atomic<double>   serverShutdownDurationSeconds{0.0};

    // ==========================================================
    // 2. Multiplayer Metrics
    // ==========================================================
    std::atomic<int64_t>  multiplayerRoomsActive{0};
    std::atomic<int64_t>  multiplayerPlayersActive{0};
    std::atomic<int64_t>  multiplayerPracticePlayersActive{0};
    std::atomic<uint64_t> multiplayerRoomCreationsAttemptedTotal{0};
    std::atomic<uint64_t> multiplayerRoomCreationsFailedTotal{0};
    std::atomic<uint64_t> multiplayerRoomJoinsAttemptedTotal{0};
    std::atomic<uint64_t> multiplayerRoomJoinsFailedTotal{0};
    std::atomic<uint64_t> multiplayerRoomCapacityRejectionsTotal{0};
    std::atomic<uint64_t> multiplayerRaceStartsTotal{0};
    std::atomic<uint64_t> multiplayerRaceCompletionsTotal{0};
    std::atomic<uint64_t> multiplayerPlayerDisconnectsTotal{0};

    std::atomic<uint64_t> multiplayerCollisionEventsTotal{0};
    std::atomic<uint64_t> multiplayerCollisionSolverCallsTotal{0};
    std::atomic<uint64_t> multiplayerCollisionSolverDurationTotalMicros{0};
    std::atomic<uint64_t> multiplayerCollisionSolverDurationMaxMicros{0};

    // ==========================================================
    // 3. Simulation Metrics
    // ==========================================================
    std::atomic<uint64_t> simulationTick{0};
    std::atomic<double>   simulationTickFrequencyHz{60.0};
    std::atomic<uint64_t> simulationTickDurationAvgMicros{0};
    std::atomic<uint64_t> simulationTickDurationMaxMicros{0};
    std::atomic<uint64_t> simulationTickDeadlineMissesTotal{0};
    std::atomic<uint64_t> simulationOverrunsTotal{0};

    std::atomic<double>   simulationSnapshotFrequencyHz{20.0};
    std::atomic<int64_t>  simulationSnapshotQueueDepth{0};
    std::atomic<uint64_t> simulationSnapshotsPublishedTotal{0};
    std::atomic<uint64_t> simulationSnapshotsDroppedTotal{0};

    std::atomic<int64_t>  simulationInputQueueDepth{0};
    std::atomic<uint64_t> simulationInputsProcessedTotal{0};
    std::atomic<uint64_t> simulationInputsDroppedTotal{0};

    std::atomic<int64_t>  simulationLifecycleQueueOccupancy{0};
    std::atomic<uint64_t> simulationLifecycleEventsRejectedTotal{0};

    // ==========================================================
    // 4. Backpressure & Reliability Metrics
    // ==========================================================
    // Output buffer histogram buckets: <=1KB, <=8KB, <=64KB, <=512KB, <=2MB, <=10MB
    std::atomic<uint64_t> bufferBucket1K{0};
    std::atomic<uint64_t> bufferBucket8K{0};
    std::atomic<uint64_t> bufferBucket64K{0};
    std::atomic<uint64_t> bufferBucket512K{0};
    std::atomic<uint64_t> bufferBucket2M{0};
    std::atomic<uint64_t> bufferBucket10M{0};

    std::atomic<uint64_t> backpressureSlowClientDisconnectsTotal{0};
    std::atomic<uint64_t> backpressureSnapshotDropsTotal{0};
    std::atomic<uint64_t> backpressureControlFrameCongestionTotal{0};
    std::atomic<uint64_t> backpressureInputHistoryOverflowTotal{0};
    std::atomic<uint64_t> backpressureInputAcksTotal{0};
    std::atomic<uint64_t> serverShutdownCleanupFailuresTotal{0};

    // ==========================================================
    // 5. Snapshot Fan-Out & Optimization Metrics (Phase 7.1)
    // ==========================================================
    std::atomic<uint64_t> snapshotJsonPayloadBytesTotal{0};
    std::atomic<uint64_t> snapshotBinaryPayloadBytesTotal{0};
    std::atomic<uint64_t> snapshotJsonFramesSentTotal{0};
    std::atomic<uint64_t> snapshotBinaryFramesSentTotal{0};
    std::atomic<uint64_t> snapshotFanoutDeliveriesTotal{0};
    std::atomic<uint64_t> snapshotDirectSendsTotal{0};
    std::atomic<uint64_t> snapshotBufferedSendsTotal{0};
    std::atomic<uint64_t> snapshotSerializationMicrosTotal{0};

    // ==========================================================
    // Helper Recording Functions (Thread-safe, non-blocking)
    // ==========================================================
    void recordHttpRequest(int statusCode);
    void recordTcpAccept();
    void recordTcpClose();
    void recordWebSocketUpgrade();
    void recordWebSocketClose();
    void recordBytesReceived(size_t bytes);
    void recordBytesTransmitted(size_t bytes);
    void recordPartialWrite();
    void recordEagain();
    void recordConnectionDrop();
    void recordOutputBufferSize(size_t bytes);

    void recordCollisionSolver(uint64_t durationMicros, uint64_t eventsCount);
    void recordSimulationTick(uint64_t durationMicros, bool missedDeadline, bool overrun);

    // ==========================================================
    // Exposition Formatters
    // ==========================================================
    std::string toPrometheusText() const;
    std::string toJson() const;

private:
    ServerMetrics() = default;
};
