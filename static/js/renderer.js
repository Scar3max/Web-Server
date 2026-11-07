/**
 * Renderer — Canvas-based curved track and multi-player vehicle rendering.
 * Transforms world coordinates to a smooth chase-camera view following the player.
 */

import { defaultTrack } from './track.js';

export class Renderer {
    constructor(canvas) {
        this.canvas = canvas;
        this.ctx = canvas.getContext('2d');
        this.width = canvas.width;
        this.height = canvas.height;

        this.pixelsPerMeter = 16.0; // World meters to screen pixels scale
        this.track = defaultTrack;

        this.carPalettes = [
            { main: '#0284c7', glow: '#38bdf8', label: 'YOU' },       // Cyan (Local Player)
            { main: '#ea580c', glow: '#fb923c', label: 'P2' },        // Orange
            { main: '#059669', glow: '#34d399', label: 'P3' },        // Emerald
            { main: '#7c3aed', glow: '#a78bfa', label: 'P4' },        // Purple
            { main: '#e11d48', glow: '#fb7185', label: 'P5' },        // Crimson
            { main: '#ca8a04', glow: '#facc15', label: 'P6' },        // Amber
        ];
    }

    resize(targetWidth, targetHeight) {
        const dpr = window.devicePixelRatio || 1;
        this.width = targetWidth;
        this.height = targetHeight;
        this.canvas.width = targetWidth * dpr;
        this.canvas.height = targetHeight * dpr;
        this.ctx.resetTransform();
        this.ctx.scale(dpr, dpr);
    }

    render(gameState) {
        const ctx = this.ctx;
        const w = this.width;
        const h = this.height;

        // 1. Clear Backdrop (Grass / terrain)
        ctx.fillStyle = '#08170e';
        ctx.fillRect(0, 0, w, h);

        const localCar = gameState.localPlayer;
        const localDist = localCar ? localCar.distance : 0.0;
        const localHeading = localCar ? localCar.heading : Math.PI / 2;
        const localWorldX = localCar ? localCar.worldX : 0.0;
        const localWorldY = localCar ? localCar.worldY : 0.0;

        // 2. Camera Setup: Center at screen (w/2, h * 0.72) and rotate to align with track forward
        ctx.save();
        ctx.translate(w * 0.5, h * 0.72);
        // Rotate so track tangent points UP (-Y in canvas space)
        ctx.rotate(-localHeading - Math.PI / 2);
        ctx.scale(this.pixelsPerMeter, this.pixelsPerMeter);
        ctx.translate(-localWorldX, -localWorldY);

        // 3. Render Curved Road Geometry
        this.renderCurvedRoad(ctx, localDist, gameState.trackLength || 1500.0);

        // 4. Render All Active Vehicles
        if (gameState.players && gameState.players.length > 0) {
            // Sort so remote players draw first, local player on top
            const sortedPlayers = [...gameState.players].sort((a, b) => {
                if (a.isLocal) return 1;
                if (b.isLocal) return -1;
                return b.distance - a.distance;
            });

            for (const p of sortedPlayers) {
                const paletteIdx = p.isLocal ? 0 : (((p.id - 1) % (this.carPalettes.length - 1)) + 1);
                const palette = this.carPalettes[paletteIdx];
                const label = p.isLocal ? "YOU" : (p.name || `Player ${p.id}`);

                this.drawVehicle(ctx, p.worldX, p.worldY, p.heading, p.controls, palette, label, p.isLocal);
            }
        }

        ctx.restore();

        // 5. Screen-Space Overlay Effects (Speed Lines at high velocity)
        if (gameState.speed > 80) {
            this.drawSpeedLines(ctx, w, h, gameState.speed);
        }

        // 6. Optional Developer Diagnostics Overlay
        if (gameState.diagnostics) {
            this.drawDiagnostics(ctx, w, h, gameState.diagnostics);
        }
    }

    renderCurvedRoad(ctx, localDist, trackLength) {
        const stepDist = 3.5; // Meters between curve evaluation samples
        const lookBack = 45.0; // Distance behind camera
        const lookAhead = 260.0; // Distance in front of camera

        const sStart = Math.max(0.0, localDist - lookBack);
        const sEnd = localDist + lookAhead;

        const halfWidth = this.track.getTrackWidth() * 0.5;
        const curbWidth = 1.2;

        let prevSample = this.track.sampleAtDistance(sStart);

        for (let s = sStart + stepDist; s <= sEnd; s += stepDist) {
            const currSample = this.track.sampleAtDistance(s);

            const p0_center = prevSample.center;
            const p0_norm = prevSample.normal;
            const p1_center = currSample.center;
            const p1_norm = currSample.normal;

            // Road Edge Vertices
            const p0_L = { x: p0_center.x - p0_norm.x * halfWidth, y: p0_center.y - p0_norm.y * halfWidth };
            const p0_R = { x: p0_center.x + p0_norm.x * halfWidth, y: p0_center.y + p0_norm.y * halfWidth };
            const p1_L = { x: p1_center.x - p1_norm.x * halfWidth, y: p1_center.y - p1_norm.y * halfWidth };
            const p1_R = { x: p1_center.x + p1_norm.x * halfWidth, y: p1_center.y + p1_norm.y * halfWidth };

            // Outer Curbs
            const p0_curbL = { x: p0_L.x - p0_norm.x * curbWidth, y: p0_L.y - p0_norm.y * curbWidth };
            const p1_curbL = { x: p1_L.x - p1_norm.x * curbWidth, y: p1_L.y - p1_norm.y * curbWidth };
            const p0_curbR = { x: p0_R.x + p0_norm.x * curbWidth, y: p0_R.y + p0_norm.y * curbWidth };
            const p1_curbR = { x: p1_R.x + p1_norm.x * curbWidth, y: p1_R.y + p1_norm.y * curbWidth };

            const isRedCurb = Math.floor(s / 6.0) % 2 === 0;

            // Draw Left Curb
            ctx.fillStyle = isRedCurb ? '#dc2626' : '#f8fafc';
            ctx.beginPath();
            ctx.moveTo(p0_curbL.x, p0_curbL.y);
            ctx.lineTo(p0_L.x, p0_L.y);
            ctx.lineTo(p1_L.x, p1_L.y);
            ctx.lineTo(p1_curbL.x, p1_curbL.y);
            ctx.closePath();
            ctx.fill();

            // Draw Right Curb
            ctx.beginPath();
            ctx.moveTo(p0_R.x, p0_R.y);
            ctx.lineTo(p0_curbR.x, p0_curbR.y);
            ctx.lineTo(p1_curbR.x, p1_curbR.y);
            ctx.lineTo(p1_R.x, p1_R.y);
            ctx.closePath();
            ctx.fill();

            // Draw Main Asphalt Surface
            ctx.fillStyle = '#1e2530';
            ctx.beginPath();
            ctx.moveTo(p0_L.x, p0_L.y);
            ctx.lineTo(p0_R.x, p0_R.y);
            ctx.lineTo(p1_R.x, p1_R.y);
            ctx.lineTo(p1_L.x, p1_L.y);
            ctx.closePath();
            ctx.fill();

            // Draw White Outer Road Edge Markings
            ctx.strokeStyle = '#f8fafc';
            ctx.lineWidth = 0.22;
            ctx.beginPath();
            ctx.moveTo(p0_L.x, p0_L.y);
            ctx.lineTo(p1_L.x, p1_L.y);
            ctx.moveTo(p0_R.x, p0_R.y);
            ctx.lineTo(p1_R.x, p1_R.y);
            ctx.stroke();

            // Draw Yellow Center Dashes
            const isDash = Math.floor(s / 5.0) % 2 === 0;
            if (isDash) {
                ctx.strokeStyle = '#fbbf24';
                ctx.lineWidth = 0.25;
                ctx.beginPath();
                ctx.moveTo(p0_center.x, p0_center.y);
                ctx.lineTo(p1_center.x, p1_center.y);
                ctx.stroke();
            }

            // Draw Finish Line Banner if within segment
            if (trackLength > 0 && prevSample.distance <= trackLength && currSample.distance >= trackLength) {
                this.drawFinishLine(ctx, currSample, halfWidth);
            }

            prevSample = currSample;
        }
    }

    drawFinishLine(ctx, sample, halfWidth) {
        ctx.save();
        ctx.translate(sample.center.x, sample.center.y);
        ctx.rotate(sample.angle + Math.PI / 2);

        const checkW = 1.0;
        const totalW = halfWidth * 2;
        const numChecks = Math.floor(totalW / checkW);

        for (let i = -numChecks / 2; i < numChecks / 2; i++) {
            const isWhite = (Math.floor(i) % 2 === 0);
            ctx.fillStyle = isWhite ? '#f8fafc' : '#0f172a';
            ctx.fillRect(i * checkW, -1.0, checkW, 2.0);
        }

        ctx.restore();
    }

    drawVehicle(ctx, x, y, heading, controls, palette, label, isLocal) {
        ctx.save();
        ctx.translate(x, y);

        // Nametag in world space (rendered readable above vehicle)
        ctx.save();
        ctx.scale(1 / this.pixelsPerMeter, 1 / this.pixelsPerMeter);
        ctx.font = '600 12px Outfit, sans-serif';
        ctx.textAlign = 'center';
        ctx.fillStyle = isLocal ? '#38bdf8' : '#cbd5e1';
        ctx.fillText(label, 0, -42);
        ctx.restore();

        // Rotate vehicle along world heading
        ctx.rotate(heading);

        const carLength = 4.2; // meters
        const carWidth = 2.0;  // meters

        // Drop shadow
        ctx.fillStyle = 'rgba(0, 0, 0, 0.45)';
        ctx.beginPath();
        ctx.roundRect(-carLength * 0.5 - 0.2, -carWidth * 0.5 + 0.2, carLength + 0.4, carWidth + 0.2, 0.4);
        ctx.fill();

        // 4 Wheels
        ctx.fillStyle = '#0f172a';
        ctx.fillRect(-carLength * 0.35, -carWidth * 0.5 - 0.15, 0.9, 0.3);
        ctx.fillRect(carLength * 0.35 - 0.9, -carWidth * 0.5 - 0.15, 0.9, 0.3);
        ctx.fillRect(-carLength * 0.35, carWidth * 0.5 - 0.15, 0.9, 0.3);
        ctx.fillRect(carLength * 0.35 - 0.9, carWidth * 0.5 - 0.15, 0.9, 0.3);

        // Main Car Body (Length along +X in local car space)
        const bodyGrad = ctx.createLinearGradient(0, -carWidth * 0.5, 0, carWidth * 0.5);
        bodyGrad.addColorStop(0, palette.main);
        bodyGrad.addColorStop(0.5, palette.glow);
        bodyGrad.addColorStop(1, palette.main);

        ctx.fillStyle = bodyGrad;
        ctx.beginPath();
        ctx.roundRect(-carLength * 0.5, -carWidth * 0.5, carLength, carWidth, 0.5);
        ctx.fill();

        // Racing Stripe
        ctx.fillStyle = '#f8fafc';
        ctx.fillRect(-carLength * 0.5, -0.18, carLength, 0.36);

        // Cockpit / Windshield
        ctx.fillStyle = '#0f172a';
        ctx.beginPath();
        ctx.roundRect(-carLength * 0.15, -carWidth * 0.35, carLength * 0.45, carWidth * 0.7, 0.25);
        ctx.fill();

        // Headlights (Front is +X in local car coordinate frame)
        ctx.fillStyle = '#fef08a';
        ctx.fillRect(carLength * 0.5 - 0.15, -carWidth * 0.42, 0.2, 0.3);
        ctx.fillRect(carLength * 0.5 - 0.15, carWidth * 0.12, 0.2, 0.3);

        // Taillights
        const isBraking = controls && controls.down;
        ctx.fillStyle = isBraking ? '#ff1e1e' : '#991b1b';
        ctx.fillRect(-carLength * 0.5 - 0.05, -carWidth * 0.42, 0.15, 0.3);
        ctx.fillRect(-carLength * 0.5 - 0.05, carWidth * 0.12, 0.15, 0.3);

        ctx.restore();
    }

    drawSpeedLines(ctx, w, h, speed) {
        const count = Math.min(20, Math.floor((speed - 80) / 8));
        ctx.strokeStyle = 'rgba(255, 255, 255, 0.15)';
        ctx.lineWidth = 2;
        ctx.beginPath();
        for (let i = 0; i < count; i++) {
            const lx = Math.random() * (w * 0.25);
            const rx = w - Math.random() * (w * 0.25);
            const y1 = Math.random() * h;
            const len = 30 + Math.random() * 80;
            ctx.moveTo(lx, y1);
            ctx.lineTo(lx, y1 + len);
            ctx.moveTo(rx, y1);
            ctx.lineTo(rx, y1 + len);
        }
        ctx.stroke();
    }

    drawDiagnostics(ctx, w, h, diag) {
        ctx.save();
        const boxW = 240;
        const boxH = 145;
        const x = 16;
        const y = h - boxH - 16;

        ctx.fillStyle = 'rgba(15, 23, 42, 0.88)';
        ctx.strokeStyle = 'rgba(56, 189, 248, 0.5)';
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.roundRect(x, y, boxW, boxH, 8);
        ctx.fill();
        ctx.stroke();

        ctx.fillStyle = '#38bdf8';
        ctx.font = '600 12px monospace';
        ctx.fillText('DIAGNOSTICS (F3 / `)', x + 12, y + 20);

        ctx.fillStyle = '#cbd5e1';
        ctx.font = '11px monospace';
        ctx.fillText(`RTT: ${diag.rtt.toFixed(1)} ms`, x + 12, y + 38);
        ctx.fillText(`Snap Interval: ${diag.snapInterval.toFixed(1)} ms`, x + 12, y + 54);
        ctx.fillText(`Reconciles: ${diag.reconciles}`, x + 12, y + 70);
        ctx.fillText(`Last Corr: ${diag.lastCorrection.toFixed(4)} m`, x + 12, y + 86);
        ctx.fillText(`Max Corr: ${diag.maxCorrection.toFixed(4)} m`, x + 12, y + 102);
        ctx.fillText(`Unacked Inputs: ${diag.unackedInputs}`, x + 12, y + 118);
        ctx.fillText(`Remote Extrap: ${diag.extrapolations}`, x + 12, y + 134);

        ctx.restore();
    }
}
