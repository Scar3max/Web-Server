/**
 * Game — Controls local car simulation, delta-time game loop, and integration.
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
        this.lastTime = 0;
        this.animationFrameId = null;

        // Vehicle & Simulation State
        this.playerX = canvas.width / 2;
        this.playerY = canvas.height * 0.75;
        this.speed = 0;             // km/h
        this.maxSpeed = 220;        // km/h
        this.accelRate = 90;        // km/h per sec
        this.brakeRate = 180;       // km/h per sec
        this.friction = 25;         // Natural drag
        this.steerSpeed = 320;      // px/sec at speed
        this.steerAngle = 0;        // radians
        this.maxSteerAngle = 0.14;  // ~8 degrees tilt

        this.roadOffset = 0;
        this.distance = 0;          // meters
        this.lapLength = 1500;      // meters

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
                this.lastTime = performance.now(); // Avoid huge dt jump on resume
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

    update(dt) {
        const controls = this.input.getState();

        // Acceleration & Braking
        if (controls.throttle) {
            this.speed = Math.min(this.maxSpeed, this.speed + this.accelRate * dt);
        } else if (controls.brake) {
            this.speed = Math.max(0, this.speed - this.brakeRate * dt);
        } else {
            // Natural rolling resistance
            this.speed = Math.max(0, this.speed - this.friction * dt);
        }

        // Steering (effective only when moving)
        const speedFactor = Math.min(1.0, this.speed / 40);
        let targetSteer = 0;

        if (controls.left) {
            this.playerX -= this.steerSpeed * speedFactor * dt;
            targetSteer = -this.maxSteerAngle;
        } else if (controls.right) {
            this.playerX += this.steerSpeed * speedFactor * dt;
            targetSteer = this.maxSteerAngle;
        }

        // Smooth steering tilt interpolation
        this.steerAngle += (targetSteer - this.steerAngle) * 12 * dt;

        // Keep car within road boundaries
        const roadHalf = this.renderer.roadWidth / 2;
        const roadCenter = this.renderer.width / 2;
        const minX = roadCenter - roadHalf + 26;
        const maxX = roadCenter + roadHalf - 26;
        this.playerX = Math.max(minX, Math.min(maxX, this.playerX));

        // Advance road scroll & distance
        const worldSpeed = (this.speed * 1000 / 3600); // convert km/h to m/s
        this.roadOffset += worldSpeed * 12 * dt;       // visual scroll multiplier
        this.distance += worldSpeed * dt;

        // Send telemetry through WebSocket echo channel
        this.network.sendTelemetry(controls, this.speed, this.distance);

        // Update HUD
        this.updateHUD();
    }

    render() {
        const gameState = {
            playerX: this.playerX,
            playerY: this.playerY,
            steerAngle: this.steerAngle,
            speed: this.speed,
            roadOffset: this.roadOffset,
            controls: this.input.getState()
        };

        this.renderer.render(gameState);
    }

    updateHUD() {
        if (this.ui.speedVal) {
            this.ui.speedVal.textContent = Math.round(this.speed);
        }
        if (this.ui.distanceVal) {
            this.ui.distanceVal.textContent = Math.round(this.distance) + 'm';
        }
        if (this.ui.lapVal) {
            const currentLap = Math.floor(this.distance / this.lapLength) + 1;
            this.ui.lapVal.textContent = `${currentLap}`;
        }
    }
}
