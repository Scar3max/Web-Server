/**
 * Game — Consumes server-authoritative snapshots and drives
 * smooth client-side interpolation and input streaming.
 */
export class Game {
    constructor(canvas, inputManager, renderer, networkManager, uiElements) {
        this.canvas = canvas;
        this.input = inputManager;
        this.renderer = renderer;
        this.network = networkManager;
        this.ui = uiElements;

        this.isPaused = false;
        this.isRunning = false;
        this.allowDriving = false;
        this.lastTime = 0;
        this.animationFrameId = null;

        this.localPlayerId = null;
        this.latestServerTick = 0;

        // Visual tracking for local player
        this.localX = 0.0;          // Normalized [-1.0, 1.0]
        this.localSpeed = 0.0;      // km/h
        this.localDistance = 0.0;   // meters
        this.localSteer = 0.0;      // radians
        this.roadOffset = 0.0;

        this.lapLength = 1500;      // meters per lap
        this.pixelsPerMeter = 6.0;  // Visual scale for relative distance

        // Player interpolation states: Map<id, { renderX, renderDist, renderSpeed, renderSteer, targetX, targetDist, targetSpeed, targetSteer, lastSeen }>
        this.players = new Map();

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

            let state = this.players.get(sp.id);
            if (!state) {
                state = {
                    id: sp.id,
                    renderX: sp.x,
                    renderDist: sp.distance,
                    renderSpeed: sp.speed,
                    renderSteer: sp.steer,
                    targetX: sp.x,
                    targetDist: sp.distance,
                    targetSpeed: sp.speed,
                    targetSteer: sp.steer,
                    controls: { up: false, down: false, left: false, right: false }
                };
                this.players.set(sp.id, state);
            } else {
                state.targetX = sp.x;
                state.targetDist = sp.distance;
                state.targetSpeed = sp.speed;
                state.targetSteer = sp.steer;
            }
        }

        // Clean up disconnected players
        for (const [id] of this.players) {
            if (!activeIds.has(id)) {
                this.players.delete(id);
            }
        }
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

    startDriving() {
        this.allowDriving = true;
    }

    stopDriving() {
        this.allowDriving = false;
        // Zero out controls
        this.network.sendInput({ up: false, down: false, left: false, right: false }, true);
    }

    update(dt) {
        const rawControls = this.input.getState();

        // 1. Send authoritative input intention to server only when actively driving
        const controls = {
            up: this.allowDriving ? !!rawControls.throttle : false,
            down: this.allowDriving ? !!rawControls.brake : false,
            left: this.allowDriving ? !!rawControls.left : false,
            right: this.allowDriving ? !!rawControls.right : false
        };
        if (this.allowDriving) {
            this.network.sendInput(controls);
        }

        // 2. Smoothly interpolate all players towards latest authoritative server targets
        const lerpFactor = Math.min(1.0, 18.0 * dt);

        for (const [id, p] of this.players) {
            p.renderX += (p.targetX - p.renderX) * lerpFactor;
            p.renderDist += (p.targetDist - p.renderDist) * lerpFactor;
            p.renderSpeed += (p.targetSpeed - p.renderSpeed) * lerpFactor;
            p.renderSteer += (p.targetSteer - p.renderSteer) * lerpFactor;

            if (id === this.localPlayerId) {
                this.localX = p.renderX;
                this.localDistance = p.renderDist;
                this.localSpeed = p.renderSpeed;
                this.localSteer = p.renderSteer;
                p.controls = controls;
            }
        }

        // 3. Drive road scroll offset from local authoritative speed
        const worldSpeedMps = (this.localSpeed * 1000.0 / 3600.0);
        this.roadOffset += worldSpeedMps * 12.0 * dt;

        // 4. Update HUD
        this.updateHUD();
    }

    render() {
        const w = this.renderer.width;
        const h = this.renderer.height;
        const roadCenter = w / 2;
        const roadHalf = this.renderer.roadWidth / 2;
        const usableHalf = roadHalf - 30;
        const localY = h * 0.75;

        const renderList = [];

        // Compute screen coordinates for each active player
        for (const [id, p] of this.players) {
            const isLocal = (id === this.localPlayerId);
            const pixelX = roadCenter + (p.renderX * usableHalf);

            let pixelY = localY;
            if (!isLocal) {
                const deltaMeters = p.renderDist - this.localDistance;
                pixelY = localY - (deltaMeters * this.pixelsPerMeter);
            }

            // Cull vehicles far off-screen
            if (pixelY >= -120 && pixelY <= h + 120) {
                renderList.push({
                    id: p.id,
                    isLocal: isLocal,
                    x: pixelX,
                    y: pixelY,
                    steerAngle: p.renderSteer,
                    speed: p.renderSpeed,
                    controls: p.controls
                });
            }
        }

        // If local player hasn't received first snapshot yet, render placeholder at start
        if (renderList.length === 0) {
            renderList.push({
                id: 1,
                isLocal: true,
                x: roadCenter,
                y: localY,
                steerAngle: 0,
                speed: 0,
                controls: { up: false, down: false, left: false, right: false }
            });
        }

        const gameState = {
            players: renderList,
            speed: this.localSpeed,
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
