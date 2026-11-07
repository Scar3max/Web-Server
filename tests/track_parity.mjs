import { defaultTrack, Track } from '../static/js/track.js';

const out = {};
out.totalLength = defaultTrack.totalLength;
out.trackWidth = defaultTrack.trackWidth;

// 1. Scan across the entire track
out.scan = [];
const scanDistances = [];
for (let s = 0.0; s <= defaultTrack.totalLength; s += 10.0) {
    scanDistances.push(s);
}
scanDistances.push(defaultTrack.totalLength);

for (const s of scanDistances) {
    const smp = defaultTrack.sampleAtDistance(s);
    out.scan.push({
        s: s,
        cx: smp.center.x,
        cy: smp.center.y,
        tx: smp.tangent.x,
        ty: smp.tangent.y,
        nx: smp.normal.x,
        ny: smp.normal.y,
        angle: smp.angle
    });
}

// 2. Projections
out.projections = [];
const testDistances = [0.0, 50.0, 350.0, 800.0, 1500.0, 2300.0, 3200.0, defaultTrack.totalLength];
const testLaterals = [-1.0, -0.75, -0.5, 0.0, 0.5, 0.75, 1.0];
for (const s of testDistances) {
    for (const d of testLaterals) {
        const wPos = defaultTrack.toWorldPosition(s, d);
        const smp = defaultTrack.sampleAtDistance(s);
        out.projections.push({
            s: s,
            d: d,
            wx: wPos.x,
            wy: wPos.y,
            angle: smp.angle
        });
    }
}

// 3. Boundaries
out.boundaries = [];
for (const s of testDistances) {
    const b = defaultTrack.getRoadBoundaries(s);
    out.boundaries.push({
        s: s,
        lx: b.left.x,
        ly: b.left.y,
        rx: b.right.x,
        ry: b.right.y
    });
}

// 4. Wrap-around distance queries
out.wraps = [];
const wrapDistances = [-500.0, -50.0, -1.0, -0.001, defaultTrack.totalLength + 0.001, defaultTrack.totalLength + 50.0, defaultTrack.totalLength + 1000.0];
for (const s of wrapDistances) {
    const smp = defaultTrack.sampleAtDistance(s);
    out.wraps.push({
        s: s,
        cx: smp.center.x,
        cy: smp.center.y,
        tx: smp.tangent.x,
        ty: smp.tangent.y,
        nx: smp.normal.x,
        ny: smp.normal.y,
        angle: smp.angle
    });
}

// 5. Degenerate segment handling
const degenPoints = [
    { x: 100.0, y: 200.0 }, { x: 100.0, y: 200.0 }, { x: 100.0, y: 200.0 }, { x: 100.0, y: 200.0 }
];
const degenTrack = new Track(degenPoints, 15.0, true);
const degenSample = degenTrack.sampleAtDistance(0.0);
out.degenerate = {
    s: 0.0,
    cx: degenSample.center.x,
    cy: degenSample.center.y,
    tx: degenSample.tangent.x,
    ty: degenSample.tangent.y,
    nx: degenSample.normal.x,
    ny: degenSample.normal.y,
    angle: degenSample.angle
};

console.log(JSON.stringify(out));
