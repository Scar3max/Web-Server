/**
 * Game — Client-Side Prediction, Server Reconciliation,
 * and Remote Player Snapshot Interpolation Engine.
 */

import { defaultTrack } from './track.js';
import { LocalPlayerPredictor, RemotePlayerInterpolator } from './prediction.js';

export class Game {
    constructor(canvas, inputManager, renderer, networkManager, uiElements) {
        this.canvas = canvas;
        this.input = inputManager;
        this.renderer = renderer;
        this.network = networkManager;
        this.ui = uiElements;

        this.track = defaultTrack;
        this.isPaused = false;
        this.isRunning = false;
        this.allowDriving = false;
        this.lastTime = 0;
        this.animationFrameId = null;

        this.localPlayerId = null;
        this.latestServerTick = 0;

        // Fixed 60 Hz simulation accumulator for deterministic client prediction
        this.fixedDt = 1.0 / 60.0;
        this.accumulatedTime = 0.0;

        // Local Player Prediction & Reconciliation Engine
        this.localPredictor = new LocalPlayerPredictor(120);

        // Remote Player Snapshot Interpolators (Map<playerId, RemotePlayerInterpolator>)
        this.remoteInterpolators = new Map();
        this.playerNames = new Map();

        // Visual tracking for local player
        this.localX = 0.0;          // Normalized [-1.0, 1.0]
        this.localSpeed = 0.0;      // km/h
        this.localDistance = 0.0;   // meters
        this.localSteer = 0.0;      // radians
        this.localWorldX = 0.0;
        this.localWorldY = 0.0;
        this.localHeading = Math.PI / 2;
        this.roadOffset = 0.0;
        this.currentControls = { up: false, down: false, left: false, right: false };

        this.trackLength = this.track.getTotalLength();
        this.lapLength = this.trackLength;
        this.pixelsPerMeter = 16.0;

        // Initialize local predictor at start line
        this.localPredictor.init({
            x: 0.0,
            distance: 0.0,
            speed: 0.0,
            steer: 0.0,
            worldX: 0.0,
            worldY: 0.0,
            heading: Math.PI / 2,
            ackSeq: 0,
            tick: 0
        }, this.track);

        // Wire network handlers
        this.network.onWelcome = (playerId) => {
            this.localPlayerId = playerId;
        };

        this.network.onSnapshot = (snapshot, rtt) => {
            this.handleSnapshot(snapshot, rtt);
        };

        // Wire pause toggle
        this.input.onPauseToggle = () => this.togglePause();
    }

    start() {
        this.isRunning = true;
        this.lastTime = performance.now();
        this.network.connect();
        this.loop(this.lastTime);
    }

    stop() {
        this.isRunning = false;
        if (this.animationFrameId) {
            cancelAnimationFrame(this.animationFrameId);
            this.animationFrameId = null;
        }
        this.network.disconnect();
    }

    togglePause() {
        this.isPaused = !this.isPaused;
        if (this.ui.overlay) {
            if (this.isPaused) {
                this.ui.overlay.classList.add('active');
            } else {
                this.ui.overlay.classList.remove('active');
                this.lastTime = performance.now();
            }
        }
    }

    handleSnapshot(snapshot, rtt) {
        this.latestServerTick = snapshot.tick;
        const activeIds = new Set();

        for (const sp of snapshot.players) {
            activeIds.add(sp.id);
            if (sp.name) {
                this.playerNames.set(sp.id, sp.name);
            }

            if (sp.id === this.localPlayerId) {
                // Reconcile local player prediction with authoritative server state
                this.localPredictor.reconcile(sp, this.track);
            } else {
                // Buffer remote player snapshot for smooth authoritative interpolation
                let interpolator = this.remoteInterpolators.get(sp.id);
                if (!interpolator) {
                    interpolator = new RemotePlayerInterpolator(60.0);
                    this.remoteInterpolators.set(sp.id, interpolator);
                }
                interpolator.pushSnapshot({
                    ...sp,
                    tick: snapshot.tick,
                    timestamp: snapshot.timestamp || performance.now()
                });
            }
        }

        // Clean up disconnected remote players
        for (const id of this.remoteInterpolators.keys()) {
            if (!activeIds.has(id)) {
                this.remoteInterpolators.delete(id);
                this.playerNames.delete(id);
            }
        }
    }

    startDriving() {
        this.allowDriving = true;
    }

    stopDriving() {
        this.allowDriving = false;
        this.currentControls = { up: false, down: false, left: false, right: false };
        this.network.sendInput(this.currentControls, true);
    }

    loop(currentTime) {
        if (!this.isRunning) return;

        const dt = Math.min((currentTime - this.lastTime) / 1000, 0.1);
        this.lastTime = currentTime;

        if (!this.isPaused) {
            this.update(dt);
        }

        this.render();

        this.animationFrameId = requestAnimationFrame((t) => this.loop(t));
    }

    update(dt) {
        const rawControls = this.input.getState();

        this.currentControls = {
            up: this.allowDriving ? !!rawControls.throttle : false,
            down: this.allowDriving ? !!rawControls.brake : false,
            left: this.allowDriving ? !!rawControls.left : false,
            right: this.allowDriving ? !!rawControls.right : false
        };

        // 1. Fixed 60 Hz Local Player Prediction Loop
        // Accumulate frame delta and step prediction in lockstep with server physics
        this.accumulatedTime += Math.min(dt, 0.1);

        while (this.accumulatedTime >= this.fixedDt) {
            if (this.allowDriving) {
                const seq = this.network.sendInput(this.currentControls, true);
                if (seq !== null) {
                    this.localPredictor.predictInput(seq, this.currentControls, this.fixedDt, this.track);
                }
            }
            this.accumulatedTime -= this.fixedDt;
        }

        // 2. Obtain Visual Smoothed Local Player State
        const localRender = this.localPredictor.getRenderState(dt, this.track);
        this.localX = localRender.x;
        this.localDistance = localRender.distance;
        this.localSpeed = localRender.speed;
        this.localSteer = localRender.steer;
        this.localWorldX = localRender.worldX;
        this.localWorldY = localRender.worldY;
        this.localHeading = localRender.heading;

        // 3. Drive road scroll offset from local speed
        const worldSpeedMps = (this.localSpeed * 1000.0 / 3600.0);
        this.roadOffset += worldSpeedMps * 12.0 * dt;

        // 4. Update HUD
        this.updateHUD();
    }

    render() {
        const renderList = [];

        // 1. Local Player (immediate prediction + visual smoothing)
        renderList.push({
            id: this.localPlayerId || 1,
            isLocal: true,
            name: 'YOU',
            worldX: this.localWorldX,
            worldY: this.localWorldY,
            heading: this.localHeading,
            distance: this.localDistance,
            speed: this.localSpeed,
            controls: this.currentControls
        });

        // 2. Remote Players (authoritative snapshot interpolation)
        const renderTime = performance.now();
        for (const [id, interp] of this.remoteInterpolators) {
            if (id === this.localPlayerId) continue;
            const state = interp.getInterpolatedState(renderTime, this.track);
            if (state) {
                renderList.push({
                    id: id,
                    isLocal: false,
                    name: this.playerNames.get(id) || `Player ${id}`,
                    worldX: state.worldX,
                    worldY: state.worldY,
                    heading: state.heading,
                    distance: state.distance,
                    speed: state.speed,
                    controls: { up: false, down: false, left: false, right: false }
                });
            }
        }

        const localPlayer = renderList[0];

        const gameState = {
            players: renderList,
            localPlayer: localPlayer,
            speed: this.localSpeed,
            trackLength: this.trackLength,
            roadOffset: this.roadOffset
        };

        this.renderer.render(gameState);
    }

    updateHUD() {
        if (this.ui.speedVal) {
            this.ui.speedVal.textContent = Math.round(this.localSpeed);
        }
        if (this.ui.distanceVal) {
            this.ui.distanceVal.textContent = Math.round(this.localDistance) + 'm';
        }
        if (this.ui.lapVal) {
            const currentLap = Math.floor(this.localDistance / this.lapLength) + 1;
            this.ui.lapVal.textContent = `${currentLap}`;
        }
        if (this.ui.tickVal) {
            this.ui.tickVal.textContent = `${this.latestServerTick}`;
        }
    }
}
