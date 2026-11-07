/**
 * Prediction & Reconciliation Engine for Multiplayer Racing Game.
 * Implements client-side prediction, input history buffering, authoritative
 * server reconciliation, visual smoothing, and remote player snapshot interpolation.
 */

export const PHYSICS_CONFIG = {
    ACCEL_RATE: 90.0,          // km/h per second
    BRAKE_RATE: 180.0,         // km/h per second
    FRICTION: 25.0,            // km/h per second rolling resistance
    MAX_FORWARD_SPEED: 220.0,  // km/h top forward speed
    MAX_REVERSE_SPEED: 0.0,    // No reverse in arcade mode
    STEER_LATERAL_SPEED: 1.6,  // Normalized units per second at speed
    STEER_DAMPING: 12.0,       // Angular tilt smoothing rate
    MAX_STEER_ANGLE: 0.14,     // Radians (~8 degrees)
    MIN_X: -1.0,               // Left track boundary
    MAX_X: 1.0,                // Right track boundary
    FIXED_DT: 1.0 / 60.0       // 60 Hz simulation timestep
};

function getTrackWidth(track) {
    if (!track) return 15.0;
    if (typeof track.getTrackWidth === 'function') return track.getTrackWidth();
    if (track.trackWidth !== undefined) return track.trackWidth;
    return 15.0;
}

/**
 * Step vehicle state forward by dt seconds given controls and track geometry.
 * Mirrors physics::updateVehicle in src/Physics.cpp bit-for-bit.
 */
export function stepPrediction(state, controls, dt, track) {
    // 1. Longitudinal Acceleration & Braking
    if (controls.up) {
        state.speed += PHYSICS_CONFIG.ACCEL_RATE * dt;
        if (state.speed > PHYSICS_CONFIG.MAX_FORWARD_SPEED) {
            state.speed = PHYSICS_CONFIG.MAX_FORWARD_SPEED;
        }
    } else if (controls.down) {
        state.speed -= PHYSICS_CONFIG.BRAKE_RATE * dt;
        if (state.speed < PHYSICS_CONFIG.MAX_REVERSE_SPEED) {
            state.speed = PHYSICS_CONFIG.MAX_REVERSE_SPEED;
        }
    } else {
        if (state.speed > 0.0) {
            state.speed -= PHYSICS_CONFIG.FRICTION * dt;
            if (state.speed < 0.0) {
                state.speed = 0.0;
            }
        }
    }

    // 2. Lateral Steering (effectiveness scales with forward velocity factor)
    const speedFactor = Math.min(1.0, Math.max(0.0, state.speed / 40.0));
    let targetSteer = 0.0;
    if (controls.left) {
        state.x -= PHYSICS_CONFIG.STEER_LATERAL_SPEED * speedFactor * dt;
        targetSteer = -PHYSICS_CONFIG.MAX_STEER_ANGLE;
    } else if (controls.right) {
        state.x += PHYSICS_CONFIG.STEER_LATERAL_SPEED * speedFactor * dt;
        targetSteer = PHYSICS_CONFIG.MAX_STEER_ANGLE;
    }

    // Smooth heading angle interpolation relative to track tangent
    state.steer += (targetSteer - state.steer) * PHYSICS_CONFIG.STEER_DAMPING * dt;
    state.x = Math.max(PHYSICS_CONFIG.MIN_X, Math.min(PHYSICS_CONFIG.MAX_X, state.x));

    // 3. Track Distance Progress
    const metersPerSecond = state.speed * (1000.0 / 3600.0);
    state.distance += metersPerSecond * dt;

    // 4. Update World Position & Global Heading from Track Geometry
    if (track) {
        const sample = track.sampleAtDistance(state.distance);
        const lateralMeters = state.x * (getTrackWidth(track) * 0.5);
        state.worldX = sample.center.x + sample.normal.x * lateralMeters;
        state.worldY = sample.center.y + sample.normal.y * lateralMeters;
        state.heading = sample.angle + state.steer;
    }
}

/**
 * LocalPlayerPredictor — Manages immediate local prediction, bounded input history,
 * server reconciliation on authoritative snapshots, and visual correction decay.
 */
export class LocalPlayerPredictor {
    constructor(maxHistory = 120) {
        this.maxHistory = maxHistory; // Up to 120 inputs (~2 seconds at 60 Hz)
        this.inputHistory = [];       // Array of { seq, controls, dt, timestamp }

        // Authoritative server state (last received from server)
        this.authoritativeState = {
            tick: 0,
            ackSeq: 0,
            x: 0.0,
            distance: 0.0,
            speed: 0.0,
            steer: 0.0,
            worldX: 0.0,
            worldY: 0.0,
            heading: Math.PI / 2
        };

        // Current predicted simulation state
        this.predictedState = {
            x: 0.0,
            distance: 0.0,
            speed: 0.0,
            steer: 0.0,
            worldX: 0.0,
            worldY: 0.0,
            heading: Math.PI / 2
        };

        // Separate visual smoothing offset (decays exponentially to 0)
        this.visualError = {
            distance: 0.0,
            x: 0.0
        };

        this.lastAckSeq = 0;
        this.reconciliationCount = 0;
        this.lastReplayedInputsCount = 0;
        this.totalReplayedInputs = 0;
        this.maxPredictionError = 0.0;
        this.lastPredictionError = 0.0;
        this.initialized = false;
    }

    /**
     * Initializes state from first authoritative snapshot or starting grid.
     */
    init(sp, track) {
        this.authoritativeState = {
            tick: sp.tick || 0,
            ackSeq: sp.ackSeq || 0,
            x: sp.x || 0.0,
            distance: sp.distance || 0.0,
            speed: sp.speed || 0.0,
            steer: sp.steer || 0.0,
            worldX: sp.worldX || 0.0,
            worldY: sp.worldY || 0.0,
            heading: sp.heading || Math.PI / 2
        };

        this.predictedState = { ...this.authoritativeState };
        this.visualError = { distance: 0.0, x: 0.0 };
        this.inputHistory = [];
        this.lastAckSeq = sp.ackSeq || 0;
        this.initialized = true;

        if (track) {
            const sample = track.sampleAtDistance(this.predictedState.distance);
            const lateralMeters = this.predictedState.x * (getTrackWidth(track) * 0.5);
            this.predictedState.worldX = sample.center.x + sample.normal.x * lateralMeters;
            this.predictedState.worldY = sample.center.y + sample.normal.y * lateralMeters;
            this.predictedState.heading = sample.angle + this.predictedState.steer;
        }
    }

    /**
     * Records a local input and predicts movement immediately.
     */
    predictInput(seq, controls, dt, track) {
        // Enforce bounded history: if connection stalled, discard oldest
        if (this.inputHistory.length >= this.maxHistory) {
            this.inputHistory.shift();
        }

        const inputEntry = {
            seq: seq,
            controls: {
                up: !!controls.up,
                down: !!controls.down,
                left: !!controls.left,
                right: !!controls.right
            },
            dt: dt,
            timestamp: performance.now()
        };

        this.inputHistory.push(inputEntry);

        // Apply physics to predicted state immediately
        stepPrediction(this.predictedState, inputEntry.controls, dt, track);
    }

    /**
     * Reconciles predicted state with an authoritative snapshot.
     * Discards acknowledged inputs, resets to authoritative state, and replays unacknowledged inputs.
     */
    reconcile(sp, track) {
        if (!this.initialized) {
            this.init(sp, track);
            return;
        }

        const ackSeq = sp.ackSeq || 0;

        // Ignore out-of-order or duplicate snapshots
        if (ackSeq < this.lastAckSeq) {
            return;
        }
        this.lastAckSeq = ackSeq;

        // Update authoritative baseline
        this.authoritativeState = {
            tick: sp.tick || 0,
            ackSeq: ackSeq,
            x: sp.x,
            distance: sp.distance,
            speed: sp.speed,
            steer: sp.steer,
            worldX: sp.worldX,
            worldY: sp.worldY,
            heading: sp.heading
        };

        // Remove all acknowledged inputs
        this.inputHistory = this.inputHistory.filter(inCmd => inCmd.seq > ackSeq);

        // Save pre-reconciliation predicted state for error measurement
        const oldPredDist = this.predictedState.distance;
        const oldPredX = this.predictedState.x;

        // Reset simulation to authoritative snapshot
        const replayedState = {
            x: sp.x,
            distance: sp.distance,
            speed: sp.speed,
            steer: sp.steer,
            worldX: sp.worldX,
            worldY: sp.worldY,
            heading: sp.heading
        };

        // Replay remaining unacknowledged inputs in exact sequential order
        const replayedCount = this.inputHistory.length;
        for (let i = 0; i < replayedCount; i++) {
            const inCmd = this.inputHistory[i];
            stepPrediction(replayedState, inCmd.controls, inCmd.dt, track);
        }

        this.reconciliationCount++;
        this.lastReplayedInputsCount = replayedCount;
        this.totalReplayedInputs += replayedCount;

        // Calculate prediction error between previous prediction and authoritative replay
        const halfWidth = getTrackWidth(track) * 0.5;
        const errDist = replayedState.distance - oldPredDist;
        const errX_m = (replayedState.x - oldPredX) * halfWidth;
        const totalErr = Math.hypot(errDist, errX_m);

        this.lastPredictionError = totalErr;
        if (totalErr > this.maxPredictionError) {
            this.maxPredictionError = totalErr;
        }

        // Visual smoothing: absorb correction into visualError without mutating physics
        if (totalErr > 15.0) {
            // Large discrepancy (e.g. major crash deflection or finish teleport): snap immediately
            this.visualError.distance = 0.0;
            this.visualError.x = 0.0;
        } else {
            // Add correction delta to visualError to avoid visual popping
            this.visualError.distance += (oldPredDist - replayedState.distance);
            this.visualError.x += (oldPredX - replayedState.x);
        }

        // Commit replayed state as new authoritative predicted state
        this.predictedState = replayedState;
    }

    /**
     * Decays visual smoothing error and returns smoothed render state.
     */
    getRenderState(dt, track) {
        // Exponential decay of visual smoothing offsets (~12.0/s half-life ~58 ms)
        const decay = Math.exp(-14.0 * Math.min(dt, 0.1));
        this.visualError.distance *= decay;
        this.visualError.x *= decay;

        if (Math.abs(this.visualError.distance) < 0.001) this.visualError.distance = 0.0;
        if (Math.abs(this.visualError.x) < 0.0001) this.visualError.x = 0.0;

        const renderDist = this.predictedState.distance + this.visualError.distance;
        const renderX = Math.max(PHYSICS_CONFIG.MIN_X, Math.min(PHYSICS_CONFIG.MAX_X,
            this.predictedState.x + this.visualError.x));

        let worldX = this.predictedState.worldX;
        let worldY = this.predictedState.worldY;
        let heading = this.predictedState.heading;

        if (track) {
            const curPos = track.toWorldPosition(renderDist, renderX);
            worldX = curPos.x;
            worldY = curPos.y;
            heading = curPos.angle + this.predictedState.steer;
        }

        return {
            x: renderX,
            distance: renderDist,
            speed: this.predictedState.speed,
            steer: this.predictedState.steer,
            worldX: worldX,
            worldY: worldY,
            heading: heading
        };
    }
}

/**
 * RemotePlayerInterpolator — Keeps remote vehicles strictly server-authoritative
 * by interpolating between buffered authoritative snapshots.
 */
export class RemotePlayerInterpolator {
    constructor(bufferDelayMs = 60.0) {
        this.bufferDelayMs = bufferDelayMs; // Playback delay behind server time (~1 snapshot + jitter buffer)
        this.snapshots = [];               // Ring buffer of { tick, timestamp, x, distance, speed, steer, worldX, worldY, heading }
        this.maxBuffer = 12;
        this.lastRenderState = null;
    }

    pushSnapshot(sp) {
        const entry = {
            tick: sp.tick || 0,
            timestamp: sp.timestamp || performance.now(),
            x: sp.x,
            distance: sp.distance,
            speed: sp.speed,
            steer: sp.steer,
            worldX: sp.worldX,
            worldY: sp.worldY,
            heading: sp.heading
        };

        // Discard out-of-order snapshots
        if (this.snapshots.length > 0 && entry.tick <= this.snapshots[this.snapshots.length - 1].tick) {
            return;
        }

        this.snapshots.push(entry);
        if (this.snapshots.length > this.maxBuffer) {
            this.snapshots.shift();
        }
    }

    getInterpolatedState(renderTime, track) {
        if (this.snapshots.length === 0) {
            return null;
        }

        // If only 1 snapshot available, hold position
        if (this.snapshots.length === 1) {
            const s = this.snapshots[0];
            return { ...s };
        }

        const targetTime = renderTime - this.bufferDelayMs;

        // Target before oldest snapshot in buffer: clamp to oldest
        if (targetTime <= this.snapshots[0].timestamp) {
            const s0 = this.snapshots[0];
            return { ...s0 };
        }

        // Target after newest snapshot: extrapolate linearly up to 100 ms, then freeze
        const newest = this.snapshots[this.snapshots.length - 1];
        if (targetTime >= newest.timestamp) {
            const extraTime = (targetTime - newest.timestamp) / 1000.0;
            const maxExtra = 0.10; // Cap extrapolation at 100 ms
            const clampedExtra = Math.min(extraTime, maxExtra);

            const mps = newest.speed * (1000.0 / 3600.0);
            const extDist = newest.distance + mps * clampedExtra;
            const extPos = track ? track.toWorldPosition(extDist, newest.x) : { x: newest.worldX, y: newest.worldY, angle: newest.heading };

            return {
                x: newest.x,
                distance: extDist,
                speed: newest.speed,
                steer: newest.steer,
                worldX: extPos.x,
                worldY: extPos.y,
                heading: extPos.angle !== undefined ? (extPos.angle + newest.steer) : newest.heading
            };
        }

        // Find enclosing snapshots S0 and S1
        let s0 = this.snapshots[0];
        let s1 = this.snapshots[1];
        for (let i = 0; i < this.snapshots.length - 1; i++) {
            if (this.snapshots[i].timestamp <= targetTime && targetTime <= this.snapshots[i + 1].timestamp) {
                s0 = this.snapshots[i];
                s1 = this.snapshots[i + 1];
                break;
            }
        }

        const duration = Math.max(1, s1.timestamp - s0.timestamp);
        const alpha = Math.max(0.0, Math.min(1.0, (targetTime - s0.timestamp) / duration));

        const interpDist = s0.distance + alpha * (s1.distance - s0.distance);
        const interpX = s0.x + alpha * (s1.x - s0.x);
        const interpSpeed = s0.speed + alpha * (s1.speed - s0.speed);
        const interpSteer = s0.steer + alpha * (s1.steer - s0.steer);

        let worldX = s0.worldX + alpha * (s1.worldX - s0.worldX);
        let worldY = s0.worldY + alpha * (s1.worldY - s0.worldY);
        let heading = s0.heading + alpha * (s1.heading - s0.heading);

        if (track) {
            const pos = track.toWorldPosition(interpDist, interpX);
            worldX = pos.x;
            worldY = pos.y;
            heading = pos.angle + interpSteer;
        }

        return {
            x: interpX,
            distance: interpDist,
            speed: interpSpeed,
            steer: interpSteer,
            worldX: worldX,
            worldY: worldY,
            heading: heading
        };
    }
}
