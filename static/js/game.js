/**
 * Game — Consumes server-authoritative snapshots and drives
 * smooth client-side interpolation and input streaming.
 */

import { defaultTrack } from './track.js';

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

        // Visual tracking for local player
        this.localX = 0.0;          // Normalized [-1.0, 1.0]
        this.localSpeed = 0.0;      // km/h
        this.localDistance = 0.0;   // meters
        this.localSteer = 0.0;      // radians
        this.localWorldX = 0.0;
        this.localWorldY = 0.0;
        this.localHeading = Math.PI / 2;
        this.roadOffset = 0.0;

        this.trackLength = this.track.getTotalLength();
        this.lapLength = this.trackLength;
        this.pixelsPerMeter = 16.0;  // Scale matching renderer

        // Player interpolation states: Map<id, { renderX, renderDist, renderSpeed, renderSteer, renderWorldX, renderWorldY, renderHeading, ... }>
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
            const wPos = (sp.worldX !== undefined && sp.worldY !== undefined)
                ? { x: sp.worldX, y: sp.worldY, angle: (sp.heading !== undefined ? sp.heading : 0) }
                : this.track.toWorldPosition(sp.distance, sp.x);
            const heading = (sp.heading !== undefined) ? sp.heading : (wPos.angle + (sp.steer || 0));

            if (!state) {
                state = {
                    id: sp.id,
                    name: sp.name || null,
                    renderX: sp.x,
                    renderDist: sp.distance,
                    renderSpeed: sp.speed,
                    renderSteer: sp.steer,
                    renderWorldX: wPos.x,
                    renderWorldY: wPos.y,
                    renderHeading: heading,
                    targetX: sp.x,
                    targetDist: sp.distance,
                    targetSpeed: sp.speed,
                    targetSteer: sp.steer,
                    targetWorldX: wPos.x,
                    targetWorldY: wPos.y,
                    targetHeading: heading,
                    controls: { up: false, down: false, left: false, right: false }
                };
                this.players.set(sp.id, state);
            } else {
                if (sp.name) state.name = sp.name;
                state.targetX = sp.x;
                state.targetDist = sp.distance;
                state.targetSpeed = sp.speed;
                state.targetSteer = sp.steer;
                state.targetWorldX = wPos.x;
                state.targetWorldY = wPos.y;
                state.targetHeading = heading;
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

            // Project smoothly onto curved track centerline and normal
            const curPos = this.track.toWorldPosition(p.renderDist, p.renderX);
            p.renderWorldX = curPos.x;
            p.renderWorldY = curPos.y;
            p.renderHeading = curPos.angle + p.renderSteer;

            if (id === this.localPlayerId) {
                this.localX = p.renderX;
                this.localDistance = p.renderDist;
                this.localSpeed = p.renderSpeed;
                this.localSteer = p.renderSteer;
                this.localWorldX = p.renderWorldX;
                this.localWorldY = p.renderWorldY;
                this.localHeading = p.renderHeading;
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
        const renderList = [];

        // Build list of all players with world-space coordinates
        for (const [id, p] of this.players) {
            const isLocal = (id === this.localPlayerId);
            renderList.push({
                id: p.id,
                isLocal: isLocal,
                name: p.name,
                worldX: p.renderWorldX,
                worldY: p.renderWorldY,
                heading: p.renderHeading,
                distance: p.renderDist,
                speed: p.renderSpeed,
                controls: p.controls
            });
        }

        // If local player hasn't received first snapshot yet, render placeholder at start
        if (renderList.length === 0) {
            const startPos = this.track.toWorldPosition(0, 0);
            renderList.push({
                id: 1,
                isLocal: true,
                name: 'YOU',
                worldX: startPos.x,
                worldY: startPos.y,
                heading: startPos.angle,
                distance: 0,
                speed: 0,
                controls: { up: false, down: false, left: false, right: false }
            });
        }

        const localPlayer = renderList.find(p => p.isLocal) || renderList[0];

        const gameState = {
            players: renderList,
            localPlayer: localPlayer,
            speed: this.localSpeed,
            trackLength: this.trackLength,
            roadOffset: this.roadOffset
        };

        this.renderer.render(gameState);
    }

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
