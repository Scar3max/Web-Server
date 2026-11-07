#include "ServerMetrics.h"
#include <iomanip>
#include <algorithm>

ServerMetrics& ServerMetrics::instance() {
    static ServerMetrics inst;
    return inst;
}

void ServerMetrics::recordHttpRequest(int statusCode) {
    switch (statusCode) {
        case 200: httpRequests200.fetch_add(1, std::memory_order_relaxed); break;
        case 400: httpRequests400.fetch_add(1, std::memory_order_relaxed); break;
        case 404: httpRequests404.fetch_add(1, std::memory_order_relaxed); break;
        case 413: httpRequests413.fetch_add(1, std::memory_order_relaxed); break;
        case 431: httpRequests431.fetch_add(1, std::memory_order_relaxed); break;
        case 500: httpRequests500.fetch_add(1, std::memory_order_relaxed); break;
        default:  httpRequestsOther.fetch_add(1, std::memory_order_relaxed); break;
    }
}

void ServerMetrics::recordTcpAccept() {
    tcpConnectionsActive.fetch_add(1, std::memory_order_relaxed);
    tcpConnectionsAcceptedTotal.fetch_add(1, std::memory_order_relaxed);
}

void ServerMetrics::recordTcpClose() {
    int64_t prev = tcpConnectionsActive.fetch_sub(1, std::memory_order_relaxed);
    if (prev <= 0) {
        tcpConnectionsActive.store(0, std::memory_order_relaxed);
    }
    tcpConnectionsClosedTotal.fetch_add(1, std::memory_order_relaxed);
}

void ServerMetrics::recordWebSocketUpgrade() {
    wsConnectionsTotal.fetch_add(1, std::memory_order_relaxed);
    wsConnectionsActive.fetch_add(1, std::memory_order_relaxed);
}

void ServerMetrics::recordWebSocketClose() {
    int64_t prev = wsConnectionsActive.fetch_sub(1, std::memory_order_relaxed);
    if (prev <= 0) {
        wsConnectionsActive.store(0, std::memory_order_relaxed);
    }
}

void ServerMetrics::recordBytesReceived(size_t bytes) {
    networkBytesReceivedTotal.fetch_add(bytes, std::memory_order_relaxed);
}

void ServerMetrics::recordBytesTransmitted(size_t bytes) {
    networkBytesTransmittedTotal.fetch_add(bytes, std::memory_order_relaxed);
}

void ServerMetrics::recordPartialWrite() {
    networkPartialWritesTotal.fetch_add(1, std::memory_order_relaxed);
}

void ServerMetrics::recordEagain() {
    networkEagainTotal.fetch_add(1, std::memory_order_relaxed);
}

void ServerMetrics::recordConnectionDrop() {
    networkConnectionDropsTotal.fetch_add(1, std::memory_order_relaxed);
}

void ServerMetrics::recordOutputBufferSize(size_t bytes) {
    if (bytes <= 1024) {
        bufferBucket1K.fetch_add(1, std::memory_order_relaxed);
    } else if (bytes <= 8192) {
        bufferBucket8K.fetch_add(1, std::memory_order_relaxed);
    } else if (bytes <= 65536) {
        bufferBucket64K.fetch_add(1, std::memory_order_relaxed);
    } else if (bytes <= 524288) {
        bufferBucket512K.fetch_add(1, std::memory_order_relaxed);
    } else if (bytes <= 2097152) {
        bufferBucket2M.fetch_add(1, std::memory_order_relaxed);
    } else {
        bufferBucket10M.fetch_add(1, std::memory_order_relaxed);
    }
}

void ServerMetrics::recordCollisionSolver(uint64_t durationMicros, uint64_t eventsCount) {
    multiplayerCollisionEventsTotal.fetch_add(eventsCount, std::memory_order_relaxed);
    multiplayerCollisionSolverCallsTotal.fetch_add(1, std::memory_order_relaxed);
    multiplayerCollisionSolverDurationTotalMicros.fetch_add(durationMicros, std::memory_order_relaxed);

    uint64_t curMax = multiplayerCollisionSolverDurationMaxMicros.load(std::memory_order_relaxed);
    while (durationMicros > curMax &&
           !multiplayerCollisionSolverDurationMaxMicros.compare_exchange_weak(curMax, durationMicros, std::memory_order_relaxed)) {}
}

void ServerMetrics::recordSimulationTick(uint64_t durationMicros, bool missedDeadline, bool overrun) {
    if (missedDeadline) {
        simulationTickDeadlineMissesTotal.fetch_add(1, std::memory_order_relaxed);
    }
    if (overrun) {
        simulationOverrunsTotal.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t curMax = simulationTickDurationMaxMicros.load(std::memory_order_relaxed);
    while (durationMicros > curMax &&
           !simulationTickDurationMaxMicros.compare_exchange_weak(curMax, durationMicros, std::memory_order_relaxed)) {}

    // Exponential moving average for average tick duration (alpha = 0.05)
    uint64_t curAvg = simulationTickDurationAvgMicros.load(std::memory_order_relaxed);
    if (curAvg == 0) {
        simulationTickDurationAvgMicros.store(durationMicros, std::memory_order_relaxed);
    } else {
        uint64_t newAvg = static_cast<uint64_t>(curAvg * 0.95 + durationMicros * 0.05);
        simulationTickDurationAvgMicros.store(newAvg, std::memory_order_relaxed);
    }
}

std::string ServerMetrics::toPrometheusText() const {
    std::ostringstream oss;

    auto emitMetric = [&](const char* name, const char* help, const char* type, auto value) {
        oss << "# HELP " << name << " " << help << "\n";
        oss << "# TYPE " << name << " " << type << "\n";
        oss << name << " " << value << "\n";
    };

    auto emitFloat = [&](const char* name, const char* help, const char* type, double value, int prec) {
        oss << "# HELP " << name << " " << help << "\n";
        oss << "# TYPE " << name << " " << type << "\n";
        oss << name << " " << std::fixed << std::setprecision(prec) << value << "\n";
    };

    // 1. HTTP Metrics
    oss << "# HELP http_requests_total Total number of HTTP requests processed by response status code\n";
    oss << "# TYPE http_requests_total counter\n";
    oss << "http_requests_total{status=\"200\"} " << httpRequests200.load(std::memory_order_relaxed) << "\n";
    oss << "http_requests_total{status=\"400\"} " << httpRequests400.load(std::memory_order_relaxed) << "\n";
    oss << "http_requests_total{status=\"404\"} " << httpRequests404.load(std::memory_order_relaxed) << "\n";
    oss << "http_requests_total{status=\"413\"} " << httpRequests413.load(std::memory_order_relaxed) << "\n";
    oss << "http_requests_total{status=\"431\"} " << httpRequests431.load(std::memory_order_relaxed) << "\n";
    oss << "http_requests_total{status=\"500\"} " << httpRequests500.load(std::memory_order_relaxed) << "\n";
    oss << "http_requests_total{status=\"other\"} " << httpRequestsOther.load(std::memory_order_relaxed) << "\n";

    // 2. Network & Connection Metrics
    emitMetric("tcp_connections_active", "Number of currently active TCP connections", "gauge",
               std::max<int64_t>(0, tcpConnectionsActive.load(std::memory_order_relaxed)));
    emitMetric("tcp_connections_accepted_total", "Total accepted TCP connections", "counter",
               tcpConnectionsAcceptedTotal.load(std::memory_order_relaxed));
    emitMetric("tcp_connections_closed_total", "Total closed TCP connections", "counter",
               tcpConnectionsClosedTotal.load(std::memory_order_relaxed));

    emitMetric("websocket_connections_total", "Total WebSocket upgrades accepted", "counter",
               wsConnectionsTotal.load(std::memory_order_relaxed));
    emitMetric("websocket_connections_active", "Currently active WebSocket connections", "gauge",
               std::max<int64_t>(0, wsConnectionsActive.load(std::memory_order_relaxed)));
    emitMetric("websocket_frames_received_total", "Total WebSocket frames parsed from clients", "counter",
               wsFramesReceivedTotal.load(std::memory_order_relaxed));
    emitMetric("websocket_frames_sent_total", "Total WebSocket frames transmitted to clients", "counter",
               wsFramesSentTotal.load(std::memory_order_relaxed));
    emitMetric("websocket_protocol_errors_total", "Total WebSocket RFC 6455 protocol violations rejected", "counter",
               wsProtocolErrorsTotal.load(std::memory_order_relaxed));

    emitMetric("network_bytes_received_total", "Total bytes received across all network sockets", "counter",
               networkBytesReceivedTotal.load(std::memory_order_relaxed));
    emitMetric("network_bytes_transmitted_total", "Total bytes transmitted across all network sockets", "counter",
               networkBytesTransmittedTotal.load(std::memory_order_relaxed));
    emitMetric("network_partial_writes_total", "Total non-blocking socket partial write occurrences", "counter",
               networkPartialWritesTotal.load(std::memory_order_relaxed));
    emitMetric("network_eagain_occurrences_total", "Total non-blocking EAGAIN / EWOULDBLOCK encounters", "counter",
               networkEagainTotal.load(std::memory_order_relaxed));
    emitMetric("network_connection_drops_total", "Total abruptly closed or reset network connections", "counter",
               networkConnectionDropsTotal.load(std::memory_order_relaxed));
    emitFloat("server_shutdown_duration_seconds", "Duration of clean server graceful shutdown in seconds", "gauge",
              serverShutdownDurationSeconds.load(std::memory_order_relaxed), 4);

    // 3. Multiplayer Metrics
    emitMetric("multiplayer_rooms_active", "Number of active rooms currently managed", "gauge",
               std::max<int64_t>(0, multiplayerRoomsActive.load(std::memory_order_relaxed)));
    emitMetric("multiplayer_players_active", "Total active players across all rooms and practice mode", "gauge",
               std::max<int64_t>(0, multiplayerPlayersActive.load(std::memory_order_relaxed)));
    emitMetric("multiplayer_practice_players_active", "Number of players driving in global practice world", "gauge",
               std::max<int64_t>(0, multiplayerPracticePlayersActive.load(std::memory_order_relaxed)));

    int64_t rCount = multiplayerRoomsActive.load(std::memory_order_relaxed);
    int64_t pCount = multiplayerPlayersActive.load(std::memory_order_relaxed);
    double pPerRoom = (rCount > 0) ? (static_cast<double>(pCount) / rCount) : 0.0;
    emitFloat("multiplayer_players_per_room_avg", "Average number of players per active room", "gauge", pPerRoom, 2);

    emitMetric("multiplayer_room_creations_attempted_total", "Total room creation attempts", "counter",
               multiplayerRoomCreationsAttemptedTotal.load(std::memory_order_relaxed));
    emitMetric("multiplayer_room_creations_failed_total", "Total failed room creation attempts", "counter",
               multiplayerRoomCreationsFailedTotal.load(std::memory_order_relaxed));
    emitMetric("multiplayer_room_joins_attempted_total", "Total room join attempts", "counter",
               multiplayerRoomJoinsAttemptedTotal.load(std::memory_order_relaxed));
    emitMetric("multiplayer_room_joins_failed_total", "Total failed room join attempts", "counter",
               multiplayerRoomJoinsFailedTotal.load(std::memory_order_relaxed));
    emitMetric("multiplayer_room_capacity_rejections_total", "Total room join attempts rejected due to room capacity limit", "counter",
               multiplayerRoomCapacityRejectionsTotal.load(std::memory_order_relaxed));
    emitMetric("multiplayer_race_starts_total", "Total race countdown and start events", "counter",
               multiplayerRaceStartsTotal.load(std::memory_order_relaxed));
    emitMetric("multiplayer_race_completions_total", "Total completed multiplayer races", "counter",
               multiplayerRaceCompletionsTotal.load(std::memory_order_relaxed));
    emitMetric("multiplayer_player_disconnects_total", "Total player disconnection and leave events", "counter",
               multiplayerPlayerDisconnectsTotal.load(std::memory_order_relaxed));

    emitMetric("multiplayer_collision_events_total", "Total vehicle-to-vehicle collision contacts resolved", "counter",
               multiplayerCollisionEventsTotal.load(std::memory_order_relaxed));
    uint64_t solverCalls = multiplayerCollisionSolverCallsTotal.load(std::memory_order_relaxed);
    uint64_t solverDurTotal = multiplayerCollisionSolverDurationTotalMicros.load(std::memory_order_relaxed);
    double avgSolverSec = (solverCalls > 0) ? (static_cast<double>(solverDurTotal) / 1000000.0 / solverCalls) : 0.0;
    emitFloat("multiplayer_collision_solver_duration_seconds", "Average vehicle collision solver execution time in seconds", "gauge", avgSolverSec, 6);
    emitFloat("multiplayer_collision_solver_duration_max_seconds", "Maximum vehicle collision solver execution time in seconds", "gauge",
              static_cast<double>(multiplayerCollisionSolverDurationMaxMicros.load(std::memory_order_relaxed)) / 1000000.0, 6);

    // 4. Simulation Metrics
    emitFloat("simulation_tick_frequency_hz", "Measured frequency of the authoritative 60 Hz simulation loop", "gauge",
              simulationTickFrequencyHz.load(std::memory_order_relaxed), 2);
    emitFloat("simulation_tick_duration_seconds", "Average simulation tick execution time in seconds", "gauge",
              static_cast<double>(simulationTickDurationAvgMicros.load(std::memory_order_relaxed)) / 1000000.0, 6);
    emitFloat("simulation_tick_duration_max_seconds", "Maximum simulation tick execution time in seconds", "gauge",
              static_cast<double>(simulationTickDurationMaxMicros.load(std::memory_order_relaxed)) / 1000000.0, 6);
    emitMetric("simulation_tick_deadline_misses_total", "Total simulation ticks exceeding the 16.67ms timestep deadline", "counter",
               simulationTickDeadlineMissesTotal.load(std::memory_order_relaxed));
    emitMetric("simulation_overruns_total", "Total simulation catch-up clamp overruns", "counter",
               simulationOverrunsTotal.load(std::memory_order_relaxed));

    emitFloat("simulation_snapshot_frequency_hz", "Measured publication frequency of authoritative 20 Hz world snapshots", "gauge",
              simulationSnapshotFrequencyHz.load(std::memory_order_relaxed), 2);
    emitMetric("simulation_snapshot_queue_depth", "Current number of snapshots buffered in SnapshotQueue", "gauge",
               std::max<int64_t>(0, simulationSnapshotQueueDepth.load(std::memory_order_relaxed)));
    emitMetric("simulation_snapshots_published_total", "Total authoritative world snapshots published", "counter",
               simulationSnapshotsPublishedTotal.load(std::memory_order_relaxed));
    emitMetric("simulation_snapshots_dropped_total", "Total snapshots dropped or overwritten due to queue capacity", "counter",
               simulationSnapshotsDroppedTotal.load(std::memory_order_relaxed));

    emitMetric("simulation_input_queue_depth", "Current number of driving inputs buffered in InputQueue", "gauge",
               std::max<int64_t>(0, simulationInputQueueDepth.load(std::memory_order_relaxed)));
    emitMetric("simulation_inputs_processed_total", "Total client driving inputs processed by simulation thread", "counter",
               simulationInputsProcessedTotal.load(std::memory_order_relaxed));
    emitMetric("simulation_inputs_dropped_total", "Total client driving inputs dropped due to rate limit or saturation", "counter",
               simulationInputsDroppedTotal.load(std::memory_order_relaxed));

    emitMetric("simulation_lifecycle_queue_occupancy", "Current number of player lifecycle events queued", "gauge",
               std::max<int64_t>(0, simulationLifecycleQueueOccupancy.load(std::memory_order_relaxed)));
    emitMetric("simulation_lifecycle_events_rejected_total", "Total player lifecycle events rejected due to admission limits", "counter",
               simulationLifecycleEventsRejectedTotal.load(std::memory_order_relaxed));

    // 5. Backpressure & Reliability Metrics
    oss << "# HELP backpressure_output_buffer_bytes Distribution of connection outgoing buffer sizes\n";
    oss << "# TYPE backpressure_output_buffer_bytes histogram\n";
    uint64_t b1 = bufferBucket1K.load(std::memory_order_relaxed);
    uint64_t b8 = b1 + bufferBucket8K.load(std::memory_order_relaxed);
    uint64_t b64 = b8 + bufferBucket64K.load(std::memory_order_relaxed);
    uint64_t b512 = b64 + bufferBucket512K.load(std::memory_order_relaxed);
    uint64_t b2m = b512 + bufferBucket2M.load(std::memory_order_relaxed);
    uint64_t b10m = b2m + bufferBucket10M.load(std::memory_order_relaxed);
    oss << "backpressure_output_buffer_bytes_bucket{le=\"1024\"} " << b1 << "\n";
    oss << "backpressure_output_buffer_bytes_bucket{le=\"8192\"} " << b8 << "\n";
    oss << "backpressure_output_buffer_bytes_bucket{le=\"65536\"} " << b64 << "\n";
    oss << "backpressure_output_buffer_bytes_bucket{le=\"524288\"} " << b512 << "\n";
    oss << "backpressure_output_buffer_bytes_bucket{le=\"2097152\"} " << b2m << "\n";
    oss << "backpressure_output_buffer_bytes_bucket{le=\"10485760\"} " << b10m << "\n";
    oss << "backpressure_output_buffer_bytes_bucket{le=\"+Inf\"} " << b10m << "\n";
    oss << "backpressure_output_buffer_bytes_count " << b10m << "\n";

    emitMetric("backpressure_slow_client_disconnects_total", "Total slow clients disconnected due to persistent write congestion", "counter",
               backpressureSlowClientDisconnectsTotal.load(std::memory_order_relaxed));
    emitMetric("backpressure_snapshot_drops_total", "Total snapshots dropped for individual congested client connections", "counter",
               backpressureSnapshotDropsTotal.load(std::memory_order_relaxed));
    emitMetric("backpressure_control_frame_congestion_total", "Total WebSocket control frames discarded due to buffer overflow", "counter",
               backpressureControlFrameCongestionTotal.load(std::memory_order_relaxed));
    emitMetric("backpressure_input_history_overflow_total", "Total input queue overflow events", "counter",
               backpressureInputHistoryOverflowTotal.load(std::memory_order_relaxed));
    emitMetric("backpressure_input_acks_total", "Total authoritative input acknowledgements serialized in snapshots", "counter",
               backpressureInputAcksTotal.load(std::memory_order_relaxed));
    emitMetric("server_shutdown_cleanup_failures_total", "Total failed resource cleanup attempts during shutdown", "counter",
               serverShutdownCleanupFailuresTotal.load(std::memory_order_relaxed));

    return oss.str();
}

std::string ServerMetrics::toJson() const {
    std::ostringstream oss;
    int64_t rCount = multiplayerRoomsActive.load(std::memory_order_relaxed);
    int64_t pCount = multiplayerPlayersActive.load(std::memory_order_relaxed);
    double pPerRoom = (rCount > 0) ? (static_cast<double>(pCount) / rCount) : 0.0;

    uint64_t solverCalls = multiplayerCollisionSolverCallsTotal.load(std::memory_order_relaxed);
    uint64_t solverDurTotal = multiplayerCollisionSolverDurationTotalMicros.load(std::memory_order_relaxed);
    double avgSolverSec = (solverCalls > 0) ? (static_cast<double>(solverDurTotal) / 1000000.0 / solverCalls) : 0.0;

    oss << "{"
        // Backward compatibility fields for Phase 4/5 audit suites
        << "\"tick\":" << simulationTick.load(std::memory_order_relaxed) << ","
        << "\"tickRate\":" << std::fixed << std::setprecision(2) << simulationTickFrequencyHz.load(std::memory_order_relaxed) << ","
        << "\"snapshotRate\":" << std::fixed << std::setprecision(2) << simulationSnapshotFrequencyHz.load(std::memory_order_relaxed) << ","
        << "\"activePlayers\":" << std::max<int64_t>(0, pCount) << ","
        << "\"roomCount\":" << std::max<int64_t>(0, rCount) << ","
        << "\"totalSnapshotsPublished\":" << simulationSnapshotsPublishedTotal.load(std::memory_order_relaxed) << ","
        << "\"inputQueueLifecycleSize\":" << std::max<int64_t>(0, simulationLifecycleQueueOccupancy.load(std::memory_order_relaxed)) << ","
        << "\"inputQueueHighWaterEvents\":" << std::max<int64_t>(0, simulationLifecycleQueueOccupancy.load(std::memory_order_relaxed)) << ","
        << "\"inputQueueHighWaterInputs\":" << std::max<int64_t>(0, simulationInputQueueDepth.load(std::memory_order_relaxed)) << ","
        << "\"totalJoinsAttempted\":" << multiplayerRoomJoinsAttemptedTotal.load(std::memory_order_relaxed) + multiplayerRoomCreationsAttemptedTotal.load(std::memory_order_relaxed) << ","
        << "\"totalJoinsAdmitted\":" << tcpConnectionsAcceptedTotal.load(std::memory_order_relaxed) << ","
        << "\"totalJoinsRejected\":" << simulationLifecycleEventsRejectedTotal.load(std::memory_order_relaxed) << ","
        << "\"totalLeavesPushed\":" << multiplayerPlayerDisconnectsTotal.load(std::memory_order_relaxed) << ","
        << "\"snapshotQueueHighWater\":" << std::max<int64_t>(0, simulationSnapshotQueueDepth.load(std::memory_order_relaxed)) << ","
        << "\"snapshotQueueTotalDropped\":" << simulationSnapshotsDroppedTotal.load(std::memory_order_relaxed) << ","

        // Extended Phase 7 metrics
        << "\"httpRequests\":{"
            << "\"200\":" << httpRequests200.load(std::memory_order_relaxed) << ","
            << "\"400\":" << httpRequests400.load(std::memory_order_relaxed) << ","
            << "\"404\":" << httpRequests404.load(std::memory_order_relaxed) << ","
            << "\"413\":" << httpRequests413.load(std::memory_order_relaxed) << ","
            << "\"431\":" << httpRequests431.load(std::memory_order_relaxed) << ","
            << "\"500\":" << httpRequests500.load(std::memory_order_relaxed) << ","
            << "\"other\":" << httpRequestsOther.load(std::memory_order_relaxed)
        << "},"
        << "\"network\":{"
            << "\"tcpActive\":" << std::max<int64_t>(0, tcpConnectionsActive.load(std::memory_order_relaxed)) << ","
            << "\"tcpAcceptedTotal\":" << tcpConnectionsAcceptedTotal.load(std::memory_order_relaxed) << ","
            << "\"tcpClosedTotal\":" << tcpConnectionsClosedTotal.load(std::memory_order_relaxed) << ","
            << "\"wsTotal\":" << wsConnectionsTotal.load(std::memory_order_relaxed) << ","
            << "\"wsActive\":" << std::max<int64_t>(0, wsConnectionsActive.load(std::memory_order_relaxed)) << ","
            << "\"wsFramesReceived\":" << wsFramesReceivedTotal.load(std::memory_order_relaxed) << ","
            << "\"wsFramesSent\":" << wsFramesSentTotal.load(std::memory_order_relaxed) << ","
            << "\"wsProtocolErrors\":" << wsProtocolErrorsTotal.load(std::memory_order_relaxed) << ","
            << "\"bytesReceived\":" << networkBytesReceivedTotal.load(std::memory_order_relaxed) << ","
            << "\"bytesTransmitted\":" << networkBytesTransmittedTotal.load(std::memory_order_relaxed) << ","
            << "\"partialWrites\":" << networkPartialWritesTotal.load(std::memory_order_relaxed) << ","
            << "\"eagainOccurrences\":" << networkEagainTotal.load(std::memory_order_relaxed) << ","
            << "\"connectionDrops\":" << networkConnectionDropsTotal.load(std::memory_order_relaxed)
        << "},"
        << "\"multiplayer\":{"
            << "\"roomsActive\":" << std::max<int64_t>(0, rCount) << ","
            << "\"playersActive\":" << std::max<int64_t>(0, pCount) << ","
            << "\"practicePlayersActive\":" << std::max<int64_t>(0, multiplayerPracticePlayersActive.load(std::memory_order_relaxed)) << ","
            << "\"playersPerRoomAvg\":" << std::fixed << std::setprecision(2) << pPerRoom << ","
            << "\"roomCreationsAttempted\":" << multiplayerRoomCreationsAttemptedTotal.load(std::memory_order_relaxed) << ","
            << "\"roomCreationsFailed\":" << multiplayerRoomCreationsFailedTotal.load(std::memory_order_relaxed) << ","
            << "\"roomJoinsAttempted\":" << multiplayerRoomJoinsAttemptedTotal.load(std::memory_order_relaxed) << ","
            << "\"roomJoinsFailed\":" << multiplayerRoomJoinsFailedTotal.load(std::memory_order_relaxed) << ","
            << "\"roomCapacityRejections\":" << multiplayerRoomCapacityRejectionsTotal.load(std::memory_order_relaxed) << ","
            << "\"raceStartsTotal\":" << multiplayerRaceStartsTotal.load(std::memory_order_relaxed) << ","
            << "\"raceCompletionsTotal\":" << multiplayerRaceCompletionsTotal.load(std::memory_order_relaxed) << ","
            << "\"playerDisconnectsTotal\":" << multiplayerPlayerDisconnectsTotal.load(std::memory_order_relaxed) << ","
            << "\"collisionEventsTotal\":" << multiplayerCollisionEventsTotal.load(std::memory_order_relaxed) << ","
            << "\"collisionSolverDurationAvgSeconds\":" << std::fixed << std::setprecision(6) << avgSolverSec << ","
            << "\"collisionSolverDurationMaxSeconds\":" << std::fixed << std::setprecision(6) << (static_cast<double>(multiplayerCollisionSolverDurationMaxMicros.load(std::memory_order_relaxed)) / 1000000.0)
        << "},"
        << "\"simulation\":{"
            << "\"tickDurationAvgSeconds\":" << std::fixed << std::setprecision(6) << (static_cast<double>(simulationTickDurationAvgMicros.load(std::memory_order_relaxed)) / 1000000.0) << ","
            << "\"tickDurationMaxSeconds\":" << std::fixed << std::setprecision(6) << (static_cast<double>(simulationTickDurationMaxMicros.load(std::memory_order_relaxed)) / 1000000.0) << ","
            << "\"tickDeadlineMisses\":" << simulationTickDeadlineMissesTotal.load(std::memory_order_relaxed) << ","
            << "\"overrunsTotal\":" << simulationOverrunsTotal.load(std::memory_order_relaxed) << ","
            << "\"inputsProcessedTotal\":" << simulationInputsProcessedTotal.load(std::memory_order_relaxed) << ","
            << "\"inputsDroppedTotal\":" << simulationInputsDroppedTotal.load(std::memory_order_relaxed)
        << "},"
        << "\"backpressure\":{"
            << "\"slowClientDisconnectsTotal\":" << backpressureSlowClientDisconnectsTotal.load(std::memory_order_relaxed) << ","
            << "\"snapshotDropsTotal\":" << backpressureSnapshotDropsTotal.load(std::memory_order_relaxed) << ","
            << "\"controlFrameCongestionTotal\":" << backpressureControlFrameCongestionTotal.load(std::memory_order_relaxed) << ","
            << "\"inputAcksTotal\":" << backpressureInputAcksTotal.load(std::memory_order_relaxed)
        << "}"
        << "}";

    return oss.str();
}
