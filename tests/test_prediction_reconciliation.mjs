/**
 * Phase 6.2 Client-Side Prediction, Server Reconciliation & Latency Simulation Test Suite
 *
 * Verifies:
 * 1. Immediate local prediction (0 ms responsiveness without waiting for server)
 * 2. Unacknowledged input replay during server reconciliation
 * 3. Removal of acknowledged inputs from history
 * 4. Prediction error bounds across 0 ms, 50 ms, 100 ms, 200 ms round-trip latency
 * 5. Visual smoothing & exponential error decay (no visual teleportation)
 * 6. Network jitter, packet loss, and delayed snapshot handling
 * 7. History buffer bounds (prevents memory leak during disconnect)
 * 8. Remote player snapshot interpolation and bounded extrapolation
 */

import { defaultTrack } from '../static/js/track.js';
import {
    PHYSICS_CONFIG,
    stepPrediction,
    LocalPlayerPredictor,
    RemotePlayerInterpolator
} from '../static/js/prediction.js';

let totalTests = 0;
let passedTests = 0;

function assert(cond, msg) {
    totalTests++;
    if (!cond) {
        console.error(`[FAIL] ${msg}`);
        throw new Error(msg);
    }
    passedTests++;
}

console.log("==================================================");
console.log("Phase 6.2 Client Prediction & Reconciliation Tests");
console.log("==================================================");

// 1. Immediate Local Prediction Test
(function testImmediatePrediction() {
    console.log("[TEST] 1. Immediate Local Prediction (0 ms Input-to-Movement)...");
    const predictor = new LocalPlayerPredictor(120);
    predictor.init({
        x: 0.0, distance: 100.0, speed: 0.0, steer: 0.0,
        worldX: 0.0, worldY: 100.0, heading: Math.PI / 2,
        ackSeq: 0, tick: 0
    }, defaultTrack);

    assert(predictor.predictedState.speed === 0.0, "Initial speed should be 0.0");
    assert(predictor.predictedState.distance === 100.0, "Initial distance should be 100.0");

    // Apply 1 throttle input tick (1/60s)
    const dt = 1.0 / 60.0;
    const throttleControls = { up: true, down: false, left: false, right: false };
    predictor.predictInput(1, throttleControls, dt, defaultTrack);

    // Car must respond immediately without server intervention
    const expectedSpeed = PHYSICS_CONFIG.ACCEL_RATE * dt;
    assert(Math.abs(predictor.predictedState.speed - expectedSpeed) < 1e-4,
        `Speed should accelerate immediately to ${expectedSpeed}, got ${predictor.predictedState.speed}`);
    assert(predictor.predictedState.distance > 100.0, "Distance should progress immediately");
    assert(predictor.inputHistory.length === 1, "Input should be recorded in inputHistory");

    console.log(`  -> Immediate response: speed=${predictor.predictedState.speed.toFixed(2)} km/h, dist=${predictor.predictedState.distance.toFixed(3)} m`);
})();

// 2. Reconciliation with 0 ms Latency (Exact Synchronization)
(function testReconciliationZeroLatency() {
    console.log("[TEST] 2. Reconciliation at 0 ms Latency (Exact Server Convergence)...");
    const predictor = new LocalPlayerPredictor(120);
    predictor.init({
        x: 0.0, distance: 0.0, speed: 0.0, steer: 0.0,
        worldX: 0.0, worldY: 0.0, heading: Math.PI / 2,
        ackSeq: 0, tick: 0
    }, defaultTrack);

    const dt = 1.0 / 60.0;
    const controls = { up: true, down: false, left: false, right: false };

    // Simulate 10 client inputs and matching server ticks
    const serverState = {
        x: 0.0, distance: 0.0, speed: 0.0, steer: 0.0,
        worldX: 0.0, worldY: 0.0, heading: Math.PI / 2
    };

    for (let seq = 1; seq <= 10; seq++) {
        predictor.predictInput(seq, controls, dt, defaultTrack);
        stepPrediction(serverState, controls, dt, defaultTrack);

        // Snapshot arriving immediately acknowledging seq
        predictor.reconcile({
            tick: seq,
            ackSeq: seq,
            x: serverState.x,
            distance: serverState.distance,
            speed: serverState.speed,
            steer: serverState.steer,
            worldX: serverState.worldX,
            worldY: serverState.worldY,
            heading: serverState.heading
        }, defaultTrack);

        assert(predictor.inputHistory.length === 0, "Acknowledged input should be cleared");
        const distErr = Math.abs(predictor.predictedState.distance - serverState.distance);
        assert(distErr < 1e-6, `Prediction error under 0ms latency should be 0, got ${distErr}`);
    }

    console.log("  -> 10 ticks reconciled with 0.0000 m prediction error.");
})();

// 3. Simulated Network Latency: 50 ms, 100 ms, 200 ms Round-Trip
(function testNetworkLatencyLevels() {
    console.log("[TEST] 3. Simulated Latencies (50 ms, 100 ms, 200 ms Round-Trip)...");

    const latencyLevels = [50, 100, 200]; // ms RTT

    for (const rtt of latencyLevels) {
        const predictor = new LocalPlayerPredictor(120);
        predictor.init({
            x: 0.0, distance: 0.0, speed: 0.0, steer: 0.0,
            worldX: 0.0, worldY: 0.0, heading: Math.PI / 2,
            ackSeq: 0, tick: 0
        }, defaultTrack);

        const dt = 1.0 / 60.0;
        const controls = { up: true, down: false, left: false, right: false };

        const serverState = {
            x: 0.0, distance: 0.0, speed: 0.0, steer: 0.0,
            worldX: 0.0, worldY: 0.0, heading: Math.PI / 2
        };
        const serverHistory = new Map(); // ackSeq -> serverState at that ackSeq

        const rttTicks = Math.round((rtt / 1000.0) / dt);
        let maxErrUnderLatency = 0.0;
        let maxUnackedInputs = 0;

        // Run 60 ticks (1 full second of driving)
        for (let tick = 1; tick <= 60; tick++) {
            predictor.predictInput(tick, controls, dt, defaultTrack);

            // Server receives and processes inputs delayed by one-way trip (rttTicks / 2)
            const serverInputSeq = Math.max(0, tick - Math.floor(rttTicks / 2));
            if (serverInputSeq > 0 && !serverHistory.has(serverInputSeq)) {
                stepPrediction(serverState, controls, dt, defaultTrack);
                serverHistory.set(serverInputSeq, { ...serverState });
            }

            // Snapshot returns delayed by full RTT
            const ackTick = Math.max(0, tick - rttTicks);
            if (ackTick > 0 && ackTick % 3 === 0 && serverHistory.has(ackTick)) {
                const authoritativeSnap = serverHistory.get(ackTick);
                predictor.reconcile({
                    tick: ackTick,
                    ackSeq: ackTick,
                    x: authoritativeSnap.x,
                    distance: authoritativeSnap.distance,
                    speed: authoritativeSnap.speed,
                    steer: authoritativeSnap.steer,
                    worldX: authoritativeSnap.worldX,
                    worldY: authoritativeSnap.worldY,
                    heading: authoritativeSnap.heading
                }, defaultTrack);

                if (predictor.inputHistory.length > maxUnackedInputs) {
                    maxUnackedInputs = predictor.inputHistory.length;
                }
                if (predictor.lastPredictionError > maxErrUnderLatency) {
                    maxErrUnderLatency = predictor.lastPredictionError;
                }
            }
        }

        console.log(`  -> ${rtt} ms RTT: Max Unacked Inputs in Flight=${maxUnackedInputs}, Max Recon Error=${maxErrUnderLatency.toFixed(4)} m`);
        assert(maxUnackedInputs <= 30, `Unacknowledged input buffer exceeded reasonable bound for ${rtt}ms RTT`);
        assert(maxErrUnderLatency <= 0.05, `Prediction error under clean ${rtt}ms latency should be minimal, got ${maxErrUnderLatency}`);
    }
})();

// 4. Jitter and Packet Drops Simulation
(function testJitterAndPacketDrops() {
    console.log("[TEST] 4. Network Jitter & Dropped Snapshots Simulation...");
    const predictor = new LocalPlayerPredictor(120);
    predictor.init({
        x: 0.0, distance: 0.0, speed: 0.0, steer: 0.0,
        worldX: 0.0, worldY: 0.0, heading: Math.PI / 2,
        ackSeq: 0, tick: 0
    }, defaultTrack);

    const dt = 1.0 / 60.0;
    const controls = { up: true, down: false, left: false, right: false };
    const serverState = {
        x: 0.0, distance: 0.0, speed: 0.0, steer: 0.0,
        worldX: 0.0, worldY: 0.0, heading: Math.PI / 2
    };

    // Client sends 60 inputs
    for (let tick = 1; tick <= 60; tick++) {
        predictor.predictInput(tick, controls, dt, defaultTrack);
        stepPrediction(serverState, controls, dt, defaultTrack);
    }

    // Simulate dropping 3 consecutive snapshots (delayed packet burst)
    // Server snapshot arrives suddenly acknowledging seq=50 (burst jump)
    assert(predictor.inputHistory.length === 60, "History should hold 60 inputs before ack");

    predictor.reconcile({
        tick: 50,
        ackSeq: 50,
        x: serverState.x,
        distance: serverState.distance,
        speed: serverState.speed,
        steer: serverState.steer,
        worldX: serverState.worldX,
        worldY: serverState.worldY,
        heading: serverState.heading
    }, defaultTrack);

    // Inputs 1..50 must be purged; 51..60 (10 inputs) must remain and be replayed cleanly
    assert(predictor.inputHistory.length === 10,
        `Expected 10 unacknowledged inputs after ack=50, got ${predictor.inputHistory.length}`);
    assert(predictor.inputHistory[0].seq === 51, `First unacked seq should be 51, got ${predictor.inputHistory[0].seq}`);
    assert(predictor.lastReplayedInputsCount === 10, `Should have replayed exactly 10 inputs, got ${predictor.lastReplayedInputsCount}`);

    console.log("  -> Dropped snapshots burst-reconciled cleanly (10 inputs replayed in sequence).");
})();

// 5. Visual Error Smoothing & Exponential Decay
(function testVisualErrorSmoothing() {
    console.log("[TEST] 5. Visual Error Smoothing (No Teleportation)...");
    const predictor = new LocalPlayerPredictor(120);
    predictor.init({
        x: 0.0, distance: 200.0, speed: 60.0, steer: 0.0,
        worldX: 0.0, worldY: 200.0, heading: Math.PI / 2,
        ackSeq: 0, tick: 0
    }, defaultTrack);

    // Simulate an external nudge / correction from server (e.g. 1.0 m bump from collision)
    predictor.reconcile({
        tick: 1,
        ackSeq: 0,
        x: 0.0,
        distance: 199.0, // 1.0 m difference
        speed: 60.0,
        steer: 0.0,
        worldX: 0.0,
        worldY: 199.0,
        heading: Math.PI / 2
    }, defaultTrack);

    // Visual error must absorb the 1.0 m difference immediately
    assert(Math.abs(predictor.visualError.distance - 1.0) < 1e-4,
        `Visual error distance should be ~1.0m, got ${predictor.visualError.distance}`);

    // Render state must smoothly decay visual error over frames
    let prevVisualDist = predictor.visualError.distance;
    const dt = 1.0 / 60.0;
    for (let frame = 0; frame < 20; frame++) {
        const renderState = predictor.getRenderState(dt, defaultTrack);
        assert(predictor.visualError.distance < prevVisualDist, "Visual error must decay over time");
        prevVisualDist = predictor.visualError.distance;
    }

    assert(predictor.visualError.distance < 0.1,
        `After 20 frames (~333ms), visual error should be < 0.1m, got ${predictor.visualError.distance}`);
    console.log(`  -> Visual error decayed smoothly to ${predictor.visualError.distance.toFixed(4)} m without teleport.`);
})();

// 6. Memory Bounds Test (Disconnect Scenario)
(function testHistoryMemoryBounds() {
    console.log("[TEST] 6. History Buffer Memory Bounds Under Disconnect...");
    const maxHistory = 120;
    const predictor = new LocalPlayerPredictor(maxHistory);
    predictor.init({
        x: 0.0, distance: 0.0, speed: 0.0, steer: 0.0,
        worldX: 0.0, worldY: 0.0, heading: Math.PI / 2,
        ackSeq: 0, tick: 0
    }, defaultTrack);

    const dt = 1.0 / 60.0;
    const controls = { up: true, down: false, left: false, right: false };

    // Simulate sending 1,000 inputs without any server acks (e.g. connection broken)
    for (let i = 1; i <= 1000; i++) {
        predictor.predictInput(i, controls, dt, defaultTrack);
    }

    assert(predictor.inputHistory.length === maxHistory,
        `inputHistory must strictly cap at ${maxHistory}, got ${predictor.inputHistory.length}`);
    console.log(`  -> 1,000 unacknowledged inputs strictly bounded to ${predictor.inputHistory.length} items (zero memory leak).`);
})();

// 7. Remote Player Snapshot Interpolation
(function testRemoteInterpolation() {
    console.log("[TEST] 7. Remote Player Snapshot Interpolation & Extrapolation Limit...");
    const interpolator = new RemotePlayerInterpolator(50.0);

    const t0 = 1000.0;
    const t1 = 1050.0; // 50 ms later (20 Hz snapshot interval)

    interpolator.pushSnapshot({
        tick: 10,
        timestamp: t0,
        x: 0.0,
        distance: 100.0,
        speed: 72.0, // 20 m/s
        steer: 0.0,
        worldX: 0.0,
        worldY: 100.0,
        heading: Math.PI / 2
    });

    interpolator.pushSnapshot({
        tick: 13,
        timestamp: t1,
        x: 0.2,
        distance: 101.0,
        speed: 72.0,
        steer: 0.05,
        worldX: 1.5,
        worldY: 101.0,
        heading: Math.PI / 2
    });

    // Query halfway through interpolation: t = t1 + bufferDelay / 2 -> render target midpoint
    const renderTime = t0 + 50.0 + 25.0; // targetTime = renderTime - 50 = t0 + 25 (exact midpoint)
    const midState = interpolator.getInterpolatedState(renderTime, defaultTrack);

    assert(midState !== null, "Interpolated state must not be null");
    assert(Math.abs(midState.distance - 100.5) < 1e-3,
        `Midpoint distance should be ~100.5m, got ${midState.distance}`);
    assert(Math.abs(midState.x - 0.1) < 1e-3,
        `Midpoint x should be ~0.1, got ${midState.x}`);

    // Test stale packet: targetTime 500 ms after newest snapshot (network lag / disconnect)
    const staleTime = t1 + 50.0 + 500.0;
    const staleState = interpolator.getInterpolatedState(staleTime, defaultTrack);

    // Extrapolation must be clamped to at most 100 ms (20 m/s * 0.1s = +2.0m max extrapolation)
    const maxAllowedDist = 101.0 + (72.0 * 1000.0 / 3600.0) * 0.10 + 0.1;
    assert(staleState.distance <= maxAllowedDist,
        `Stale extrapolation runaway detected: ${staleState.distance} > ${maxAllowedDist}`);

    console.log(`  -> Midpoint interpolated: dist=${midState.distance.toFixed(2)}m, x=${midState.x.toFixed(2)}.`);
    console.log(`  -> Stale extrapolation safely clamped at dist=${staleState.distance.toFixed(2)}m (no runaway).`);
})();

console.log("==================================================");
console.log(`ALL ${passedTests} PREDICTION & RECONCILIATION TESTS PASSED!`);
console.log("==================================================");
