#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdint>

namespace track {

struct Vector2D {
    double x = 0.0;
    double y = 0.0;

    Vector2D() = default;
    Vector2D(double px, double py) : x(px), y(py) {}

    Vector2D operator+(const Vector2D& o) const { return {x + o.x, y + o.y}; }
    Vector2D operator-(const Vector2D& o) const { return {x - o.x, y - o.y}; }
    Vector2D operator*(double s) const { return {x * s, y * s}; }
    Vector2D operator/(double s) const { return {x / s, y / s}; }

    double length() const { return std::sqrt(x * x + y * y); }
    double lengthSq() const { return x * x + y * y; }

    Vector2D normalized() const {
        double len = length();
        return (len > 1e-9) ? Vector2D(x / len, y / len) : Vector2D(0.0, 1.0);
    }
};

struct TrackSample {
    Vector2D center;          // World coordinates of centerline
    Vector2D tangent;         // Unit tangent vector in direction of travel
    Vector2D normal;          // Unit normal vector (pointing right from travel direction)
    double angle = 0.0;       // Orientation in radians (atan2(tangent.y, tangent.x))
    double distance = 0.0;    // Distance along centerline in meters
    double curvature = 0.0;   // Signed local curvature (positive = right turn, negative = left turn)
};

class Track {
public:
    static constexpr double DEFAULT_TRACK_WIDTH = 15.0; // 15 meters width

private:
    std::vector<Vector2D> controlPoints;
    double trackWidth = DEFAULT_TRACK_WIDTH;
    bool isClosed = true;

    struct SamplePoint {
        double distance = 0.0;
        Vector2D position;
        Vector2D tangent;
        Vector2D normal;
        double angle = 0.0;
    };

    std::vector<SamplePoint> samples;
    double totalLength = 0.0;

    void buildArcLengthTable(size_t samplesPerSegment = 60);

public:
    Track();
    Track(const std::vector<Vector2D>& points, double width = DEFAULT_TRACK_WIDTH, bool closed = true);

    // Queries
    TrackSample sampleAtDistance(double distance) const;
    Vector2D toWorldPosition(double distance, double normalizedLateral) const;
    void getRoadBoundaries(double distance, Vector2D& outLeft, Vector2D& outRight) const;

    double getTotalLength() const { return totalLength; }
    double getTrackWidth() const { return trackWidth; }
    const std::vector<Vector2D>& getControlPoints() const { return controlPoints; }

    // Catmull-Rom spline helper
    static Vector2D evaluateCatmullRom(const Vector2D& p0, const Vector2D& p1,
                                       const Vector2D& p2, const Vector2D& p3, double t);
    static Vector2D evaluateCatmullRomDerivative(const Vector2D& p0, const Vector2D& p1,
                                               const Vector2D& p2, const Vector2D& p3, double t);
};

// Global default track singleton
const Track& getDefaultTrack();

} // namespace track
