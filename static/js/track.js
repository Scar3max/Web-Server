/**
 * Track — Catmull-Rom curved track geometry and arc-length parameterization.
 * Perfectly mirrors server C++ Track implementation for deterministic rendering and vehicle positioning.
 */

export class Track {
    constructor(controlPoints, width = 15.0, closed = true) {
        this.trackWidth = width;
        this.isClosed = closed;

        // Default championship circuit matching C++ Track.cpp
        this.controlPoints = controlPoints || [
            { x: 0.0, y: 0.0 },         // P0: Start / Finish Line
            { x: 0.0, y: 350.0 },       // P1: Main Straightaway
            { x: 40.0, y: 600.0 },      // P2: Turn 1 Entry (Right)
            { x: 160.0, y: 800.0 },     // P3: Turn 1 Apex
            { x: 350.0, y: 900.0 },     // P4: Turn 1 Exit
            { x: 550.0, y: 850.0 },     // P5: Chicane Entry (Left)
            { x: 680.0, y: 720.0 },     // P6: Chicane Apex
            { x: 720.0, y: 520.0 },     // P7: Chicane Exit
            { x: 850.0, y: 350.0 },     // P8: Hairpin Approach
            { x: 960.0, y: 200.0 },     // P9: Hairpin Apex (Sharp right)
            { x: 920.0, y: 50.0 },      // P10: Hairpin Exit
            { x: 780.0, y: -80.0 },     // P11: Turn 4 Fast Sweeper
            { x: 550.0, y: -150.0 },    // P12: Turn 5 Long Curve
            { x: 280.0, y: -120.0 },    // P13: Turn 6
            { x: 60.0, y: -50.0 }       // P14: Final Turn returning to Main Straight
        ];

        this.samples = [];
        this.totalLength = 0.0;
        this.buildArcLengthTable(80);
    }

    static evaluateCatmullRom(p0, p1, p2, p3, t) {
        const t2 = t * t;
        const t3 = t2 * t;

        const c0 = -t3 + 2.0 * t2 - t;
        const c1 = 3.0 * t3 - 5.0 * t2 + 2.0;
        const c2 = -3.0 * t3 + 4.0 * t2 + t;
        const c3 = t3 - t2;

        return {
            x: 0.5 * (p0.x * c0 + p1.x * c1 + p2.x * c2 + p3.x * c3),
            y: 0.5 * (p0.y * c0 + p1.y * c1 + p2.y * c2 + p3.y * c3)
        };
    }

    static evaluateCatmullRomDerivative(p0, p1, p2, p3, t) {
        const t2 = t * t;

        const dc0 = -3.0 * t2 + 4.0 * t - 1.0;
        const dc1 = 9.0 * t2 - 10.0 * t;
        const dc2 = -9.0 * t2 + 8.0 * t + 1.0;
        const dc3 = 3.0 * t2 - 2.0 * t;

        return {
            x: 0.5 * (p0.x * dc0 + p1.x * dc1 + p2.x * dc2 + p3.x * dc3),
            y: 0.5 * (p0.y * dc0 + p1.y * dc1 + p2.y * dc2 + p3.y * dc3)
        };
    }

    buildArcLengthTable(samplesPerSegment = 80) {
        this.samples = [];
        this.totalLength = 0.0;

        const numPoints = this.controlPoints.length;
        if (numPoints < 2) return;

        const numSegments = this.isClosed ? numPoints : (numPoints - 1);
        let prevPos = this.controlPoints[0];

        // Initial sample at s = 0
        let initialTan = { x: 0.0, y: 1.0 };
        if (numPoints >= 2) {
            const dx = this.controlPoints[1].x - this.controlPoints[0].x;
            const dy = this.controlPoints[1].y - this.controlPoints[0].y;
            const len = Math.hypot(dx, dy) || 1.0;
            initialTan = { x: dx / len, y: dy / len };
        }
        const initialNorm = { x: initialTan.y, y: -initialTan.x };
        this.samples.push({
            distance: 0.0,
            position: prevPos,
            tangent: initialTan,
            normal: initialNorm,
            angle: Math.atan2(initialTan.y, initialTan.x)
        });

        for (let seg = 0; seg < numSegments; seg++) {
            let idx0, idx1, idx2, idx3;

            if (this.isClosed) {
                idx0 = (seg + numPoints - 1) % numPoints;
                idx1 = seg % numPoints;
                idx2 = (seg + 1) % numPoints;
                idx3 = (seg + 2) % numPoints;
            } else {
                idx0 = (seg > 0) ? (seg - 1) : 0;
                idx1 = seg;
                idx2 = Math.min(seg + 1, numPoints - 1);
                idx3 = Math.min(seg + 2, numPoints - 1);
            }

            const p0 = this.controlPoints[idx0];
            const p1 = this.controlPoints[idx1];
            const p2 = this.controlPoints[idx2];
            const p3 = this.controlPoints[idx3];

            for (let step = 1; step <= samplesPerSegment; step++) {
                const t = step / samplesPerSegment;
                const pos = Track.evaluateCatmullRom(p0, p1, p2, p3, t);
                const deriv = Track.evaluateCatmullRomDerivative(p0, p1, p2, p3, t);
                const dLen = Math.hypot(deriv.x, deriv.y) || 1.0;
                const tan = { x: deriv.x / dLen, y: deriv.y / dLen };
                const norm = { x: tan.y, y: -tan.x };

                const segDist = Math.hypot(pos.x - prevPos.x, pos.y - prevPos.y);
                this.totalLength += segDist;
                prevPos = pos;

                const angle = Math.atan2(tan.y, tan.x);
                this.samples.push({
                    distance: this.totalLength,
                    position: pos,
                    tangent: tan,
                    normal: norm,
                    angle: angle
                });
            }
        }
    }

    sampleAtDistance(distance) {
        if (this.samples.length === 0) {
            return {
                center: { x: 0, y: 0 },
                tangent: { x: 0, y: 1 },
                normal: { x: 1, y: 0 },
                angle: Math.PI / 2,
                distance: 0,
                curvature: 0
            };
        }

        let s = distance;
        if (this.isClosed && this.totalLength > 0.0) {
            s = s % this.totalLength;
            if (s < 0.0) s += this.totalLength;
        } else {
            s = Math.max(0.0, Math.min(s, this.totalLength));
        }

        // Binary search for interval
        let low = 0;
        let high = this.samples.length - 1;
        while (low < high) {
            const mid = (low + high) >> 1;
            if (this.samples[mid].distance < s) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }

        if (low === 0) {
            const sp = this.samples[0];
            return { center: sp.position, tangent: sp.tangent, normal: sp.normal, angle: sp.angle, distance: s, curvature: 0 };
        }

        const s0 = this.samples[low - 1];
        const s1 = this.samples[low];

        const segLen = s1.distance - s0.distance;
        const alpha = (segLen > 1e-9) ? Math.max(0.0, Math.min(1.0, (s - s0.distance) / segLen)) : 0.0;

        const cx = s0.position.x + (s1.position.x - s0.position.x) * alpha;
        const cy = s0.position.y + (s1.position.y - s0.position.y) * alpha;

        const tx = s0.tangent.x + (s1.tangent.x - s0.tangent.x) * alpha;
        const ty = s0.tangent.y + (s1.tangent.y - s0.tangent.y) * alpha;
        const tLen = Math.hypot(tx, ty) || 1.0;
        const tangent = { x: tx / tLen, y: ty / tLen };
        const normal = { x: tangent.y, y: -tangent.x };
        const angle = Math.atan2(tangent.y, tangent.x);

        return {
            center: { x: cx, y: cy },
            tangent: tangent,
            normal: normal,
            angle: angle,
            distance: s,
            curvature: 0
        };
    }

    toWorldPosition(distance, normalizedLateral) {
        const sample = this.sampleAtDistance(distance);
        const lateralMeters = normalizedLateral * (this.trackWidth * 0.5);
        return {
            x: sample.center.x + sample.normal.x * lateralMeters,
            y: sample.center.y + sample.normal.y * lateralMeters,
            angle: sample.angle
        };
    }

    getRoadBoundaries(distance) {
        const sample = this.sampleAtDistance(distance);
        const halfWidth = this.trackWidth * 0.5;
        return {
            left: {
                x: sample.center.x - sample.normal.x * halfWidth,
                y: sample.center.y - sample.normal.y * halfWidth
            },
            right: {
                x: sample.center.x + sample.normal.x * halfWidth,
                y: sample.center.y + sample.normal.y * halfWidth
            }
        };
    }
}

export const defaultTrack = new Track();
