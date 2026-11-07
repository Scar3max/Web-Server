#include <iostream>
#include <vector>
#include <iomanip>
#include "Track.h"

using namespace track;

static void printSampleJson(double s, const TrackSample& sample, bool isLast = false) {
    std::cout << "{\"s\":" << std::setprecision(12) << s
              << ",\"cx\":" << sample.center.x
              << ",\"cy\":" << sample.center.y
              << ",\"tx\":" << sample.tangent.x
              << ",\"ty\":" << sample.tangent.y
              << ",\"nx\":" << sample.normal.x
              << ",\"ny\":" << sample.normal.y
              << ",\"angle\":" << sample.angle
              << "}" << (isLast ? "" : ",");
}

int main() {
    const auto& track = getDefaultTrack();

    std::cout << "{\n";
    std::cout << "\"totalLength\":" << std::setprecision(14) << track.getTotalLength() << ",\n";
    std::cout << "\"trackWidth\":" << track.getTrackWidth() << ",\n";

    // 1. Scan across the entire track every 10 meters plus exact endpoints
    std::cout << "\"scan\":[\n";
    double totalLen = track.getTotalLength();
    std::vector<double> scanDistances;
    for (double s = 0.0; s <= totalLen; s += 10.0) {
        scanDistances.push_back(s);
    }
    scanDistances.push_back(totalLen);

    for (size_t i = 0; i < scanDistances.size(); ++i) {
        double s = scanDistances[i];
        TrackSample sample = track.sampleAtDistance(s);
        printSampleJson(s, sample, i + 1 == scanDistances.size());
    }
    std::cout << "],\n";

    // 2. Projections across distances and lateral offsets
    std::cout << "\"projections\":[\n";
    double testDistances[] = {0.0, 50.0, 350.0, 800.0, 1500.0, 2300.0, 3200.0, totalLen};
    double testLaterals[] = {-1.0, -0.75, -0.5, 0.0, 0.5, 0.75, 1.0};
    bool firstProj = true;
    for (double s : testDistances) {
        for (double d : testLaterals) {
            if (!firstProj) std::cout << ",";
            firstProj = false;
            Vector2D wPos = track.toWorldPosition(s, d);
            TrackSample smp = track.sampleAtDistance(s);
            std::cout << "{\"s\":" << s << ",\"d\":" << d
                      << ",\"wx\":" << std::setprecision(12) << wPos.x
                      << ",\"wy\":" << wPos.y
                      << ",\"angle\":" << smp.angle << "}";
        }
    }
    std::cout << "],\n";

    // 3. Boundaries across distances
    std::cout << "\"boundaries\":[\n";
    bool firstBound = true;
    for (double s : testDistances) {
        if (!firstBound) std::cout << ",";
        firstBound = false;
        Vector2D bLeft, bRight;
        track.getRoadBoundaries(s, bLeft, bRight);
        std::cout << "{\"s\":" << s
                  << ",\"lx\":" << std::setprecision(12) << bLeft.x
                  << ",\"ly\":" << bLeft.y
                  << ",\"rx\":" << bRight.x
                  << ",\"ry\":" << bRight.y << "}";
    }
    std::cout << "],\n";

    // 4. Wrap-around distance queries
    std::cout << "\"wraps\":[\n";
    double wrapDistances[] = {-500.0, -50.0, -1.0, -0.001, totalLen + 0.001, totalLen + 50.0, totalLen + 1000.0};
    for (size_t i = 0; i < sizeof(wrapDistances)/sizeof(wrapDistances[0]); ++i) {
        double s = wrapDistances[i];
        TrackSample sample = track.sampleAtDistance(s);
        printSampleJson(s, sample, i + 1 == sizeof(wrapDistances)/sizeof(wrapDistances[0]));
    }
    std::cout << "],\n";

    // 5. Degenerate segment handling
    std::vector<Vector2D> degenPoints = {
        {100.0, 200.0}, {100.0, 200.0}, {100.0, 200.0}, {100.0, 200.0}
    };
    Track degenTrack(degenPoints, 15.0, true);
    TrackSample degenSample = degenTrack.sampleAtDistance(0.0);
    std::cout << "\"degenerate\":";
    printSampleJson(0.0, degenSample, true);
    std::cout << "\n}\n";

    return 0;
}
