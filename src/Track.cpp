#include "Track.h"
#include <cmath>
#include <algorithm>
#include <iostream>

namespace track {

Vector2D Track::evaluateCatmullRom(const Vector2D& p0, const Vector2D& p1,
                                   const Vector2D& p2, const Vector2D& p3, double t) {
    double t2 = t * t;
    double t3 = t2 * t;

    double c0 = -t3 + 2.0 * t2 - t;
    double c1 = 3.0 * t3 - 5.0 * t2 + 2.0;
    double c2 = -3.0 * t3 + 4.0 * t2 + t;
    double c3 = t3 - t2;

    return (p0 * c0 + p1 * c1 + p2 * c2 + p3 * c3) * 0.5;
}

Vector2D Track::evaluateCatmullRomDerivative(const Vector2D& p0, const Vector2D& p1,
                                           const Vector2D& p2, const Vector2D& p3, double t) {
    double t2 = t * t;

    double dc0 = -3.0 * t2 + 4.0 * t - 1.0;
    double dc1 = 9.0 * t2 - 10.0 * t;
    double dc2 = -9.0 * t2 + 8.0 * t + 1.0;
    double dc3 = 3.0 * t2 - 2.0 * t;

    return (p0 * dc0 + p1 * dc1 + p2 * dc2 + p3 * dc3) * 0.5;
}

Track::Track()
    : trackWidth(DEFAULT_TRACK_WIDTH), isClosed(true) {
    // Default championship circuit: Straightaway, high-speed sweepers, S-bends, and hairpin
    controlPoints = {
        {0.0, 0.0},         // P0: Start / Finish Line
        {0.0, 350.0},       // P1: Main Straightaway
        {40.0, 600.0},      // P2: Turn 1 Entry (Right)
        {160.0, 800.0},     // P3: Turn 1 Apex
        {350.0, 900.0},     // P4: Turn 1 Exit
        {550.0, 850.0},     // P5: Chicane Entry (Left)
        {680.0, 720.0},     // P6: Chicane Apex
        {720.0, 520.0},     // P7: Chicane Exit
        {850.0, 350.0},     // P8: Hairpin Approach
        {960.0, 200.0},     // P9: Hairpin Apex (Sharp right)
        {920.0, 50.0},      // P10: Hairpin Exit
        {780.0, -80.0},     // P11: Turn 4 Fast Sweeper
        {550.0, -150.0},    // P12: Turn 5 Long Curve
        {280.0, -120.0},    // P13: Turn 6
        {60.0, -50.0}       // P14: Final Turn returning to Main Straight
    };

    buildArcLengthTable(80);
}

Track::Track(const std::vector<Vector2D>& points, double width, bool closed)
    : controlPoints(points), trackWidth(width), isClosed(closed) {
    if (controlPoints.size() < 3) {
        // Fallback for degenerate control points (straight line of 5000m)
        controlPoints = {
            {0.0, 0.0},
            {0.0, 1000.0},
            {0.0, 2000.0},
            {0.0, 5000.0}
        };
        isClosed = false;
    }
    buildArcLengthTable(80);
}

void Track::buildArcLengthTable(size_t samplesPerSegment) {
    samples.clear();
    totalLength = 0.0;

    size_t numPoints = controlPoints.size();
    if (numPoints < 2) return;

    size_t numSegments = isClosed ? numPoints : (numPoints - 1);
    Vector2D prevPos = controlPoints[0];

    // First sample at distance 0
    Vector2D initialTan(0.0, 1.0);
    if (numPoints >= 2) {
        initialTan = (controlPoints[1] - controlPoints[0]).normalized();
    }
    Vector2D initialNorm(initialTan.y, -initialTan.x);
    samples.push_back({0.0, prevPos, initialTan, initialNorm, std::atan2(initialTan.y, initialTan.x)});

    for (size_t seg = 0; seg < numSegments; seg++) {
        size_t idx0, idx1, idx2, idx3;

        if (isClosed) {
            idx0 = (seg + numPoints - 1) % numPoints;
            idx1 = seg % numPoints;
            idx2 = (seg + 1) % numPoints;
            idx3 = (seg + 2) % numPoints;
        } else {
            idx0 = (seg > 0) ? (seg - 1) : 0;
            idx1 = seg;
            idx2 = std::min(seg + 1, numPoints - 1);
            idx3 = std::min(seg + 2, numPoints - 1);
        }

        const auto& p0 = controlPoints[idx0];
        const auto& p1 = controlPoints[idx1];
        const auto& p2 = controlPoints[idx2];
        const auto& p3 = controlPoints[idx3];

        for (size_t step = 1; step <= samplesPerSegment; step++) {
            double t = static_cast<double>(step) / static_cast<double>(samplesPerSegment);
            Vector2D pos = evaluateCatmullRom(p0, p1, p2, p3, t);
            Vector2D deriv = evaluateCatmullRomDerivative(p0, p1, p2, p3, t);
            Vector2D tan = deriv.normalized();
            Vector2D norm(tan.y, -tan.x); // Right-hand normal

            double segDist = (pos - prevPos).length();
            totalLength += segDist;
            prevPos = pos;

            double angle = std::atan2(tan.y, tan.x);
            samples.push_back({totalLength, pos, tan, norm, angle});
        }
    }
}

TrackSample Track::sampleAtDistance(double distance) const {
    if (samples.empty()) {
        TrackSample def;
        def.tangent = {0.0, 1.0};
        def.normal = {1.0, 0.0};
        def.angle = std::atan2(1.0, 0.0);
        return def;
    }

    double s = distance;
    if (isClosed && totalLength > 0.0) {
        s = std::fmod(s, totalLength);
        if (s < 0.0) s += totalLength;
    } else {
        s = std::clamp(s, 0.0, totalLength);
    }

    // Binary search for segment in samples table
    auto it = std::upper_bound(samples.begin(), samples.end(), s,
                               [](double val, const SamplePoint& sp) {
                                   return val < sp.distance;
                               });

    if (it == samples.begin()) {
        const auto& sp = samples.front();
        return {sp.position, sp.tangent, sp.normal, sp.angle, s, 0.0};
    }
    if (it == samples.end()) {
        const auto& sp = samples.back();
        return {sp.position, sp.tangent, sp.normal, sp.angle, s, 0.0};
    }

    const auto& s0 = *(it - 1);
    const auto& s1 = *it;

    double segLen = s1.distance - s0.distance;
    double alpha = (segLen > 1e-9) ? ((s - s0.distance) / segLen) : 0.0;
    alpha = std::clamp(alpha, 0.0, 1.0);

    // Interpolate position, tangent, and normal
    Vector2D center = s0.position + (s1.position - s0.position) * alpha;
    Vector2D tangent = (s0.tangent + (s1.tangent - s0.tangent) * alpha).normalized();
    Vector2D normal(tangent.y, -tangent.x);
    double angle = std::atan2(tangent.y, tangent.x);

    // Curvature estimate kappa = d(angle) / ds
    double dAngle = s1.angle - s0.angle;
    // Unwrap angular discontinuity [-pi, pi]
    while (dAngle > M_PI) dAngle -= 2.0 * M_PI;
    while (dAngle < -M_PI) dAngle += 2.0 * M_PI;
    double curvature = (segLen > 1e-9) ? (dAngle / segLen) : 0.0;

    TrackSample res;
    res.center = center;
    res.tangent = tangent;
    res.normal = normal;
    res.angle = angle;
    res.distance = s;
    res.curvature = curvature;
    return res;
}

Vector2D Track::toWorldPosition(double distance, double normalizedLateral) const {
    TrackSample sample = sampleAtDistance(distance);
    double lateralMeters = normalizedLateral * (trackWidth * 0.5);
    return sample.center + sample.normal * lateralMeters;
}

void Track::getRoadBoundaries(double distance, Vector2D& outLeft, Vector2D& outRight) const {
    TrackSample sample = sampleAtDistance(distance);
    double halfWidth = trackWidth * 0.5;
    outLeft = sample.center - sample.normal * halfWidth;  // Lateral = -1.0
    outRight = sample.center + sample.normal * halfWidth; // Lateral = +1.0
}

static Track g_defaultTrack;

const Track& getDefaultTrack() {
    return g_defaultTrack;
}

} // namespace track
