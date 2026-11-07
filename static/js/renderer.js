/**
 * Renderer — Canvas-based arcade racing track and multi-player car rendering.
 */
export class Renderer {
    constructor(canvas) {
        this.canvas = canvas;
        this.ctx = canvas.getContext('2d');
        this.width = canvas.width;
        this.height = canvas.height;

        this.roadWidth = 500;
        this.roadSegmentLength = 60; // Distance between lane stripes

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

        // Clear backdrop (grass / track shoulders)
        ctx.fillStyle = '#08170e';
        ctx.fillRect(0, 0, w, h);

        // Draw parallax grass / gravel shoulder patterns
        this.drawShoulders(ctx, w, h, gameState.roadOffset);

        // Draw main asphalt road
        const roadLeft = (w - this.roadWidth) / 2;
        const roadRight = roadLeft + this.roadWidth;

        ctx.fillStyle = '#1e2530';
        ctx.fillRect(roadLeft, 0, this.roadWidth, h);

        // Draw Rumble Strips (red & white curbs) on road borders
        this.drawCurb(ctx, roadLeft - 18, 18, h, gameState.roadOffset);
        this.drawCurb(ctx, roadRight, 18, h, gameState.roadOffset);

        // Draw Lane Markings (scrolling vertically based on roadOffset)
        this.drawLaneDashes(ctx, roadLeft, roadRight, h, gameState.roadOffset);

        // High speed wind/speed lines effect
        if (gameState.speed > 80) {
            this.drawSpeedLines(ctx, w, h, gameState.speed);
        }

        // Draw all active players in the authoritative simulation world
        if (gameState.players && gameState.players.length > 0) {
            // Draw remote players first, then local player on top
            const sortedPlayers = [...gameState.players].sort((a, b) => {
                if (a.isLocal) return 1;
                if (b.isLocal) return -1;
                return b.y - a.y; // Painter's algorithm
            });

            for (const p of sortedPlayers) {
                const paletteIdx = p.isLocal ? 0 : (((p.id - 1) % (this.carPalettes.length - 1)) + 1);
                const palette = this.carPalettes[paletteIdx];
                const label = p.isLocal ? "YOU" : `Player ${p.id}`;

                this.drawCar(ctx, p.x, p.y, p.steerAngle, p.controls, palette, label, p.isLocal);
            }
        }
    }

    drawShoulders(ctx, w, h, roadOffset) {
        const stripeHeight = 80;
        const offset = roadOffset % (stripeHeight * 2);

        ctx.fillStyle = '#0b1f13';
        for (let y = -stripeHeight * 2; y < h + stripeHeight; y += stripeHeight * 2) {
            ctx.fillRect(0, y + offset, w, stripeHeight);
        }
    }

    drawCurb(ctx, x, width, h, roadOffset) {
        const curbSegment = 40;
        const offset = roadOffset % (curbSegment * 2);

        for (let y = -curbSegment * 2; y < h + curbSegment; y += curbSegment) {
            const isRed = Math.floor((y - offset) / curbSegment) % 2 === 0;
            ctx.fillStyle = isRed ? '#dc2626' : '#f8fafc';
            ctx.fillRect(x, y + offset, width, curbSegment);
        }
    }

    drawLaneDashes(ctx, left, right, h, roadOffset) {
        const dashHeight = 40;
        const dashSpacing = 80;
        const cycle = dashHeight + dashSpacing;
        const offset = roadOffset % cycle;

        const laneWidth = this.roadWidth / 3;
        const lane1X = left + laneWidth;
        const lane2X = left + laneWidth * 2;

        ctx.fillStyle = '#fbbf24';
        for (let y = -cycle; y < h + cycle; y += cycle) {
            ctx.fillRect(lane1X - 3, y + offset, 6, dashHeight);
            ctx.fillRect(lane2X - 3, y + offset, 6, dashHeight);
        }

        // Outer solid white road edge lines
        ctx.fillStyle = '#f8fafc';
        ctx.fillRect(left + 6, 0, 4, h);
        ctx.fillRect(right - 10, 0, 4, h);
    }

    drawCar(ctx, x, y, steerAngle, controls, palette, label, isLocal) {
        ctx.save();
        ctx.translate(x, y);

        // Player Tag / Nametag
        ctx.font = '600 11px Outfit, sans-serif';
        ctx.textAlign = 'center';
        ctx.fillStyle = isLocal ? '#38bdf8' : '#cbd5e1';
        ctx.fillText(label, 0, -56);

        ctx.rotate(steerAngle);

        const carW = 44;
        const carH = 78;

        // Headlight Beams
        const beamGradient = ctx.createLinearGradient(0, -carH / 2, 0, -carH / 2 - 160);
        beamGradient.addColorStop(0, 'rgba(255, 255, 255, 0.45)');
        beamGradient.addColorStop(1, 'rgba(255, 255, 255, 0)');

        ctx.fillStyle = beamGradient;
        ctx.beginPath();
        ctx.moveTo(-16, -carH / 2);
        ctx.lineTo(-45, -carH / 2 - 160);
        ctx.lineTo(45, -carH / 2 - 160);
        ctx.lineTo(16, -carH / 2);
        ctx.closePath();
        ctx.fill();

        // Drop shadow
        ctx.fillStyle = 'rgba(0, 0, 0, 0.45)';
        ctx.beginPath();
        ctx.roundRect(-carW / 2 - 4, -carH / 2 + 6, carW + 8, carH + 4, 10);
        ctx.fill();

        // 4 Wheels
        ctx.fillStyle = '#0f172a';
        this.drawWheel(ctx, -carW / 2 - 3, -carH / 2 + 14, steerAngle * 1.5);
        this.drawWheel(ctx, carW / 2 + 3, -carH / 2 + 14, steerAngle * 1.5);
        this.drawWheel(ctx, -carW / 2 - 3, carH / 2 - 20, 0);
        this.drawWheel(ctx, carW / 2 + 3, carH / 2 - 20, 0);

        // Main Car Body
        const bodyGrad = ctx.createLinearGradient(-carW / 2, 0, carW / 2, 0);
        bodyGrad.addColorStop(0, palette.main);
        bodyGrad.addColorStop(0.5, palette.glow);
        bodyGrad.addColorStop(1, palette.main);

        ctx.fillStyle = bodyGrad;
        ctx.beginPath();
        ctx.roundRect(-carW / 2, -carH / 2, carW, carH, [12, 12, 8, 8]);
        ctx.fill();

        // Racing Stripe
        ctx.fillStyle = '#f8fafc';
        ctx.fillRect(-4, -carH / 2, 8, carH);

        // Cockpit / Windshield
        ctx.fillStyle = '#0f172a';
        ctx.beginPath();
        ctx.roundRect(-carW / 2 + 6, -carH / 2 + 20, carW - 12, 28, [6, 6, 4, 4]);
        ctx.fill();

        // Windshield reflection
        ctx.fillStyle = 'rgba(255, 255, 255, 0.25)';
        ctx.beginPath();
        ctx.moveTo(-carW / 2 + 8, -carH / 2 + 22);
        ctx.lineTo(0, -carH / 2 + 22);
        ctx.lineTo(-6, -carH / 2 + 34);
        ctx.lineTo(-carW / 2 + 8, -carH / 2 + 34);
        ctx.closePath();
        ctx.fill();

        // Headlights
        ctx.fillStyle = '#fef08a';
        ctx.fillRect(-carW / 2 + 4, -carH / 2 + 2, 8, 4);
        ctx.fillRect(carW / 2 - 12, -carH / 2 + 2, 8, 4);

        // Taillights
        const isBraking = controls && controls.down;
        ctx.fillStyle = isBraking ? '#ff1e1e' : '#991b1b';
        ctx.fillRect(-carW / 2 + 4, carH / 2 - 4, 10, 4);
        ctx.fillRect(carW / 2 - 14, carH / 2 - 4, 10, 4);

        ctx.restore();
    }

    drawWheel(ctx, x, y, angle) {
        ctx.save();
        ctx.translate(x, y);
        ctx.rotate(angle);
        ctx.fillStyle = '#1e293b';
        ctx.fillRect(-4, -8, 8, 16);
        ctx.fillStyle = '#64748b';
        ctx.fillRect(-1, -6, 2, 12);
        ctx.restore();
    }

    drawSpeedLines(ctx, w, h, speed) {
        const count = Math.min(18, Math.floor((speed - 80) / 10));
        ctx.strokeStyle = 'rgba(255, 255, 255, 0.12)';
        ctx.lineWidth = 2;
        ctx.beginPath();
        for (let i = 0; i < count; i++) {
            const lx = Math.random() * (w * 0.2);
            const rx = w - Math.random() * (w * 0.2);
            const y1 = Math.random() * h;
            const len = 30 + Math.random() * 60;
            ctx.moveTo(lx, y1);
            ctx.lineTo(lx, y1 + len);
            ctx.moveTo(rx, y1);
            ctx.lineTo(rx, y1 + len);
        }
        ctx.stroke();
    }
}
