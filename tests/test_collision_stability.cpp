#include <iostream>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <chrono>
#include <cassert>
#include <iomanip>
#include "Physics.h"
#include "Track.h"

using namespace physics;

static double calcKineticEnergy(const std::unordered_map<uint32_t, PlayerState>& vehicles) {
    double totalEnergy = 0.0;
    for (const auto& [_, p] : vehicles) {
        double v_ms = p.speed * (1000.0 / 3600.0);
        totalEnergy += 0.5 * (v_ms * v_ms);
    }
    return totalEnergy;
}

static double calcMaxPenetration(const std::unordered_map<uint32_t, PlayerState>& vehicles, const track::Track& track) {
    double halfWidth = track.getTrackWidth() * 0.5;
    double maxPen = 0.0;
    std::vector<uint32_t> ids;
    for (const auto& [id, _] : vehicles) ids.push_back(id);

    for (size_t i = 0; i < ids.size(); ++i) {
        for (size_t j = i + 1; j < ids.size(); ++j) {
            const auto& pA = vehicles.at(ids[i]);
            const auto& pB = vehicles.at(ids[j]);
            double ds = pB.distance - pA.distance;
            double dd = (pB.x - pA.x) * halfWidth;
            double dist = std::sqrt(ds * ds + dd * dd);
            if (dist < CollisionConfig::MIN_SEPARATION_DIST) {
                double pen = CollisionConfig::MIN_SEPARATION_DIST - dist;
                if (pen > maxPen) {
                    maxPen = pen;
                }
            }
        }
    }
    return maxPen;
}

static void checkFiniteAndBounded(const std::unordered_map<uint32_t, PlayerState>& vehicles) {
    for (const auto& [id, p] : vehicles) {
        assert(!std::isnan(p.distance) && !std::isinf(p.distance));
        assert(!std::isnan(p.speed) && !std::isinf(p.speed));
        assert(!std::isnan(p.x) && !std::isinf(p.x));
        assert(!std::isnan(p.worldX) && !std::isinf(p.worldX));
        assert(!std::isnan(p.worldY) && !std::isinf(p.worldY));
        assert(!std::isnan(p.heading) && !std::isinf(p.heading));
        assert(p.x >= -1.0 && p.x <= 1.0);
        assert(p.speed >= 0.0 && p.speed <= Config::MAX_FORWARD_SPEED + 1e-4);
    }
}

// 1. Two-vehicle head-on / reverse approach
void test_head_on_collision() {
    std::cout << "[TEST] 1. Head-On Approach Collision Stability..." << std::endl;
    const auto& track = track::getDefaultTrack();
    std::unordered_map<uint32_t, PlayerState> vehicles;

    PlayerState p1(1, 10, 1, 1, "Car1");
    p1.distance = 100.0;
    p1.speed = 100.0; // km/h moving forward
    p1.x = 0.0;

    PlayerState p2(2, 11, 2, 1, "Car2");
    p2.distance = 102.0; // 2m ahead (penetrating by 1.0m)
    p2.speed = 0.0;
    p2.x = 0.0;

    vehicles[1] = p1;
    vehicles[2] = p2;

    double eBefore = calcKineticEnergy(vehicles);
    std::vector<CollisionEvent> events;
    resolveRoomCollisions(vehicles, track, &events);
    double eAfter = calcKineticEnergy(vehicles);

    double pen = calcMaxPenetration(vehicles, track);
    checkFiniteAndBounded(vehicles);

    assert(events.size() == 1);
    assert(pen <= 1e-3);
    assert(eAfter <= eBefore + 1e-6);
    std::cout << "  -> Resolved: sep=" << (vehicles[2].distance - vehicles[1].distance)
              << "m, residual pen=" << pen << "m, E_before=" << eBefore << ", E_after=" << eAfter << std::endl;
}

// 2. Rear-end collisions at low, medium, and maximum relative velocity
void test_rear_end_speeds() {
    std::cout << "[TEST] 2. Rear-End Collisions at Varying Relative Speeds..." << std::endl;
    const auto& track = track::getDefaultTrack();

    double testSpeeds[] = {20.0, 100.0, 220.0};
    for (double relSpd : testSpeeds) {
        std::unordered_map<uint32_t, PlayerState> vehicles;
        PlayerState p1(1, 10, 1, 1, "FastChaser");
        p1.distance = 200.0;
        p1.speed = relSpd;
        p1.x = 0.0;

        PlayerState p2(2, 11, 2, 1, "SlowLeader");
        p2.distance = 201.5; // Overlapping by 1.5m
        p2.speed = 0.0;
        p2.x = 0.0;

        vehicles[1] = p1;
        vehicles[2] = p2;

        double eBefore = calcKineticEnergy(vehicles);
        resolveRoomCollisions(vehicles, track);
        double eAfter = calcKineticEnergy(vehicles);

        double pen = calcMaxPenetration(vehicles, track);
        checkFiniteAndBounded(vehicles);

        assert(pen <= 1e-3);
        assert(eAfter <= eBefore + 1e-6);
        assert(vehicles[2].speed > 0.0); // Leader gained forward momentum
        assert(vehicles[1].speed < relSpd); // Chaser decelerated

        std::cout << "  -> RelSpd=" << relSpd << " km/h | Residual Pen: " << pen
                  << "m | Chaser Spd: " << vehicles[1].speed << " km/h | Leader Spd: " << vehicles[2].speed
                  << " km/h | E_ratio: " << (eAfter / (eBefore > 0 ? eBefore : 1.0)) << std::endl;
    }
}

// 3. Side-impact collisions at various angles and lateral offsets
void test_side_impacts() {
    std::cout << "[TEST] 3. Side-Impact Collisions Across Lateral Angles..." << std::endl;
    const auto& track = track::getDefaultTrack();

    double offsets[] = {-0.3, -0.1, 0.1, 0.3};
    for (double off : offsets) {
        std::unordered_map<uint32_t, PlayerState> vehicles;
        PlayerState p1(1, 10, 1, 1, "Car1");
        p1.distance = 300.0;
        p1.speed = 80.0;
        p1.x = off;

        PlayerState p2(2, 11, 2, 1, "Car2");
        p2.distance = 301.0;
        p2.speed = 80.0;
        p2.x = off + 0.15; // Lateral overlap

        vehicles[1] = p1;
        vehicles[2] = p2;

        double eBefore = calcKineticEnergy(vehicles);
        resolveRoomCollisions(vehicles, track);
        double eAfter = calcKineticEnergy(vehicles);

        double pen = calcMaxPenetration(vehicles, track);
        checkFiniteAndBounded(vehicles);

        assert(pen <= 1e-3);
        assert(eAfter <= eBefore + 1e-6);
    }
    std::cout << "  -> All side-impact tests resolved with residual penetration <= 1e-3m and energy bounded." << std::endl;
}

// 4. Multi-vehicle collisions: 3, 4, 8 vehicles in a line / pack
void test_multi_vehicle_piles() {
    std::cout << "[TEST] 4. Multi-Vehicle Simultaneous Collisions (3, 4, 8 vehicles)..." << std::endl;
    const auto& track = track::getDefaultTrack();

    int carCounts[] = {3, 4, 8};
    for (int count : carCounts) {
        std::unordered_map<uint32_t, PlayerState> vehicles;
        for (int i = 0; i < count; ++i) {
            PlayerState p(i + 1, 10 + i, i + 1, 1, "Car" + std::to_string(i + 1));
            p.distance = 400.0 + i * 1.0; // Packed every 1.0m (separation needed = 3.0m)
            p.speed = 60.0;
            p.x = ((i % 2 == 0) ? -0.05 : 0.05);
            vehicles[i + 1] = p;
        }

        double eBefore = calcKineticEnergy(vehicles);
        resolveRoomCollisions(vehicles, track);
        double eAfter = calcKineticEnergy(vehicles);

        double pen = calcMaxPenetration(vehicles, track);
        checkFiniteAndBounded(vehicles);

        std::cout << "  -> " << count << "-Car Pack: Max Residual Pen = " << pen
                  << "m | E_before = " << eBefore << " | E_after = " << eAfter << std::endl;
        assert(pen <= 1e-3);
        assert(eAfter <= eBefore + 1e-6);
    }
}

// 5. Simultaneous collisions: Car A hits B at same instant B hits C
void test_simultaneous_collisions() {
    std::cout << "[TEST] 5. Simultaneous Symmetrical Collision (A -> B <- C)..." << std::endl;
    const auto& track = track::getDefaultTrack();
    std::unordered_map<uint32_t, PlayerState> vehicles;

    PlayerState pA(1, 10, 1, 1, "CarA");
    pA.distance = 500.0;
    pA.speed = 100.0;
    pA.x = 0.0;

    PlayerState pB(2, 11, 2, 1, "CarB");
    pB.distance = 501.5; // Overlaps with A by 1.5m and with C by 1.5m
    pB.speed = 50.0;
    pB.x = 0.0;

    PlayerState pC(3, 12, 3, 1, "CarC");
    pC.distance = 503.0;
    pC.speed = 20.0;
    pC.x = 0.0;

    vehicles[1] = pA;
    vehicles[2] = pB;
    vehicles[3] = pC;

    double eBefore = calcKineticEnergy(vehicles);
    resolveRoomCollisions(vehicles, track);
    double eAfter = calcKineticEnergy(vehicles);

    double pen = calcMaxPenetration(vehicles, track);
    checkFiniteAndBounded(vehicles);

    assert(pen <= 1e-3);
    assert(eAfter <= eBefore + 1e-6);
    std::cout << "  -> Simultaneous collisions resolved: max pen=" << pen << "m, E_conserved/dissipated." << std::endl;
}

// 6. Collision at track boundaries (left and right curbs)
void test_boundary_collisions() {
    std::cout << "[TEST] 6. Hard Collisions at Track Boundaries (Shortfall Transfer)..." << std::endl;
    const auto& track = track::getDefaultTrack();
    double halfWidth = track.getTrackWidth() * 0.5;

    // Test right curb (x = 1.0)
    {
        std::unordered_map<uint32_t, PlayerState> vehicles;
        PlayerState p1(1, 10, 1, 1, "WallCar");
        p1.distance = 600.0;
        p1.speed = 70.0;
        p1.x = 0.95; // Near right curb

        PlayerState p2(2, 11, 2, 1, "PusherCar");
        p2.distance = 600.5;
        p2.speed = 90.0;
        p2.x = 0.85; // Pressing rightwards

        vehicles[1] = p1;
        vehicles[2] = p2;

        resolveRoomCollisions(vehicles, track);
        double pen = calcMaxPenetration(vehicles, track);
        checkFiniteAndBounded(vehicles);

        assert(vehicles[1].x <= 1.0);
        assert(vehicles[2].x <= 1.0);
        assert(pen <= 1e-3);
        std::cout << "  -> Right curb collision: p1.x=" << vehicles[1].x << ", p2.x=" << vehicles[2].x
                  << ", residual pen=" << pen << "m" << std::endl;
    }

    // Test left curb (x = -1.0)
    {
        std::unordered_map<uint32_t, PlayerState> vehicles;
        PlayerState p1(1, 10, 1, 1, "LeftWallCar");
        p1.distance = 650.0;
        p1.speed = 70.0;
        p1.x = -0.95; // Near left curb

        PlayerState p2(2, 11, 2, 1, "LeftPusherCar");
        p2.distance = 650.5;
        p2.speed = 90.0;
        p2.x = -0.85;

        vehicles[1] = p1;
        vehicles[2] = p2;

        resolveRoomCollisions(vehicles, track);
        double pen = calcMaxPenetration(vehicles, track);
        checkFiniteAndBounded(vehicles);

        assert(vehicles[1].x >= -1.0);
        assert(vehicles[2].x >= -1.0);
        assert(pen <= 1e-3);
        std::cout << "  -> Left curb collision: p1.x=" << vehicles[1].x << ", p2.x=" << vehicles[2].x
                  << ", residual pen=" << pen << "m" << std::endl;
    }
}

// 7. Moving vehicle colliding with stationary finished vehicle
void test_finished_vehicle_collision() {
    std::cout << "[TEST] 7. Moving Vehicle Colliding with Stationary Finished Vehicle..." << std::endl;
    const auto& track = track::getDefaultTrack();
    std::unordered_map<uint32_t, PlayerState> vehicles;

    PlayerState pFinished(1, 10, 1, 1, "Winner");
    pFinished.distance = 2150.0;
    pFinished.speed = 0.0;
    pFinished.x = 0.0;
    pFinished.finished = true;
    pFinished.finishRank = 1;
    pFinished.finishTime = 42.125;
    track::TrackSample sFin = track.sampleAtDistance(pFinished.distance);
    pFinished.worldX = sFin.center.x;
    pFinished.worldY = sFin.center.y;
    pFinished.heading = sFin.angle;

    PlayerState pMoving(2, 11, 2, 1, "MovingLate");
    pMoving.distance = 2148.5; // Overlapping by 1.5m
    pMoving.speed = 120.0;
    pMoving.x = 0.0;
    pMoving.finished = false;

    vehicles[1] = pFinished;
    vehicles[2] = pMoving;

    double origDist = pFinished.distance;
    double origX = pFinished.x;
    double origSpeed = pFinished.speed;
    double origWorldX = pFinished.worldX;
    double origWorldY = pFinished.worldY;
    uint32_t origRank = pFinished.finishRank;
    double origTime = pFinished.finishTime;

    resolveRoomCollisions(vehicles, track);

    // Invariants for finished vehicle
    assert(vehicles[1].distance == origDist);
    assert(vehicles[1].x == origX);
    assert(vehicles[1].speed == origSpeed);
    assert(vehicles[1].worldX == origWorldX);
    assert(vehicles[1].worldY == origWorldY);
    assert(vehicles[1].finishRank == origRank);
    assert(vehicles[1].finishTime == origTime);

    // Moving vehicle took full displacement & momentum deflection
    assert(vehicles[2].distance < origDist);
    double pen = calcMaxPenetration(vehicles, track);
    assert(pen <= 1e-3);
    assert(vehicles[2].speed < 120.0); // Scrubbed

    std::cout << "  -> Finished vehicle strictly immutable (distance, x, speed, world pos, rank, time preserved). Moving car pushed back." << std::endl;
}

// 8. Extended contact / repeated collisions over many ticks
void test_extended_repeated_collisions() {
    std::cout << "[TEST] 8. Extended Contact Over 120 Ticks (2.0s Continuous Driving)..." << std::endl;
    const auto& track = track::getDefaultTrack();
    std::unordered_map<uint32_t, PlayerState> vehicles;

    PlayerState p1(1, 10, 1, 1, "Leader");
    p1.distance = 100.0;
    p1.speed = 80.0;
    p1.x = 0.0;

    PlayerState p2(2, 11, 2, 1, "Pusher");
    p2.distance = 96.0;
    p2.speed = 120.0;
    p2.x = 0.0;

    vehicles[1] = p1;
    vehicles[2] = p2;

    Controls fullThrottle{true, false, false, false};

    for (int tick = 0; tick < 120; ++tick) {
        updateVehicle(vehicles[1], fullThrottle, Config::FIXED_DT, track);
        updateVehicle(vehicles[2], fullThrottle, Config::FIXED_DT, track);

        resolveRoomCollisions(vehicles, track);

        double pen = calcMaxPenetration(vehicles, track);
        checkFiniteAndBounded(vehicles);
        assert(pen <= 1e-3);
    }
    std::cout << "  -> 120 consecutive ticks stable without oscillation, NaN, or penetration." << std::endl;
}

// 9. Two vehicles initialized at identical coordinates
void test_identical_coordinates() {
    std::cout << "[TEST] 9. Identical and Nearly Identical Coordinates Collision..." << std::endl;
    const auto& track = track::getDefaultTrack();

    // Exact identical coordinates
    {
        std::unordered_map<uint32_t, PlayerState> vehicles;
        PlayerState p1(1, 10, 1, 1, "Clone1");
        p1.distance = 700.0;
        p1.speed = 60.0;
        p1.x = 0.0;

        PlayerState p2(2, 11, 2, 1, "Clone2");
        p2.distance = 700.0;
        p2.speed = 60.0;
        p2.x = 0.0;

        vehicles[1] = p1;
        vehicles[2] = p2;

        resolveRoomCollisions(vehicles, track);
        double pen = calcMaxPenetration(vehicles, track);
        checkFiniteAndBounded(vehicles);

        assert(pen <= 1e-3);
        double sep = std::hypot(vehicles[2].distance - vehicles[1].distance,
                                (vehicles[2].x - vehicles[1].x) * track.getTrackWidth() * 0.5);
        assert(sep >= CollisionConfig::MIN_SEPARATION_DIST - 1e-3);
        std::cout << "  -> Identical coords: separated to " << sep << "m (pen=" << pen << "m)" << std::endl;
    }

    // Nearly identical coords (1e-5 m separation)
    {
        std::unordered_map<uint32_t, PlayerState> vehicles;
        PlayerState p1(1, 10, 1, 1, "NearClone1");
        p1.distance = 700.0;
        p1.speed = 60.0;
        p1.x = 0.0;

        PlayerState p2(2, 11, 2, 1, "NearClone2");
        p2.distance = 700.00001;
        p2.speed = 60.0;
        p2.x = 0.000001;

        vehicles[1] = p1;
        vehicles[2] = p2;

        resolveRoomCollisions(vehicles, track);
        double pen = calcMaxPenetration(vehicles, track);
        checkFiniteAndBounded(vehicles);

        assert(pen <= 1e-3);
        double sep = std::hypot(vehicles[2].distance - vehicles[1].distance,
                                (vehicles[2].x - vehicles[1].x) * track.getTrackWidth() * 0.5);
        assert(sep >= CollisionConfig::MIN_SEPARATION_DIST - 1e-3);
        std::cout << "  -> Near-identical coords: separated to " << sep << "m (pen=" << pen << "m)" << std::endl;
    }
}

// 10. Performance Benchmarks: 2, 8, 16, 32 vehicles in collision pack
void benchmark_collision_performance() {
    std::cout << "\n==================================================" << std::endl;
    std::cout << "Performance Benchmark: Collision Processing Under Load" << std::endl;
    std::cout << "==================================================" << std::endl;

    const auto& track = track::getDefaultTrack();
    int vehicleCounts[] = {2, 8, 16, 32};
    const int NUM_TICKS = 1000;

    for (int count : vehicleCounts) {
        std::unordered_map<uint32_t, PlayerState> vehicles;
        for (int i = 0; i < count; ++i) {
            PlayerState p(i + 1, 10 + i, i + 1, 1, "BenchmarkCar" + std::to_string(i + 1));
            p.distance = 1000.0 + (i * 1.5); // Pack in tight cluster (penetrating within contact radius)
            p.speed = 80.0 + (i % 5) * 5.0;
            p.x = ((i % 3) - 1) * 0.3;
            vehicles[i + 1] = p;
        }

        // Warm-up relaxation tick to settle initial dense spawning
        resolveRoomCollisions(vehicles, track);

        auto start = std::chrono::high_resolution_clock::now();
        double maxPenObserved = 0.0;

        for (int t = 0; t < NUM_TICKS; ++t) {
            resolveRoomCollisions(vehicles, track);
            double pen = calcMaxPenetration(vehicles, track);
            if (pen > maxPenObserved) maxPenObserved = pen;
        }

        auto elapsed = std::chrono::high_resolution_clock::now() - start;
        double totalMicros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
        double avgMicrosPerTick = totalMicros / NUM_TICKS;

        std::cout << "  " << std::setw(2) << count << " Vehicles in Pack: "
                  << std::fixed << std::setprecision(2) << avgMicrosPerTick << " us/tick "
                  << "(" << (avgMicrosPerTick / 1000.0) << " ms/tick | "
                  << std::setprecision(1) << ((avgMicrosPerTick / 16666.7) * 100.0) << "% of 60Hz tick budget) "
                  << "| Max Residual Pen: " << std::setprecision(4) << maxPenObserved << "m" << std::endl;

        assert(avgMicrosPerTick < 16666.7); // Must easily fit in 16.667 ms fixed timestep
        assert(maxPenObserved <= 1e-3);
    }
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "Phase 6.1 Collision Stability & Penetration Tests" << std::endl;
    std::cout << "==================================================" << std::endl;

    test_head_on_collision();
    test_rear_end_speeds();
    test_side_impacts();
    test_multi_vehicle_piles();
    test_simultaneous_collisions();
    test_boundary_collisions();
    test_finished_vehicle_collision();
    test_extended_repeated_collisions();
    test_identical_coordinates();
    benchmark_collision_performance();

    std::cout << "\n==================================================" << std::endl;
    std::cout << "ALL COLLISION STABILITY AUDIT TESTS PASSED!" << std::endl;
    std::cout << "==================================================" << std::endl;
    return 0;
}
