#pragma once

#include <vector>
#include <unordered_map>
#include <cstdint>
#include "Player.h"
#include "Track.h"

namespace physics {

struct Config {
    static constexpr double ACCEL_RATE          = 90.0;    // km/h per second
    static constexpr double BRAKE_RATE          = 180.0;   // km/h per second
    static constexpr double FRICTION            = 25.0;    // km/h per second rolling resistance
    static constexpr double MAX_FORWARD_SPEED   = 220.0;   // km/h
    static constexpr double MAX_REVERSE_SPEED   = 0.0;     // No reverse in arcade mode
    static constexpr double STEER_LATERAL_SPEED = 1.6;     // Normalized units per second at speed
    static constexpr double STEER_DAMPING       = 12.0;    // Angular tilt smoothing rate
    static constexpr double MAX_STEER_ANGLE     = 0.14;    // Radians (~8 degrees)
    static constexpr double MIN_X               = -1.0;    // Left road boundary
    static constexpr double MAX_X               = 1.0;     // Right road boundary
    static constexpr double FIXED_DT            = 1.0 / 60.0; // 60 Hz simulation timestep (~0.016667 s)
    static constexpr uint32_t TICK_RATE         = 60;      // 60 ticks per second
    static constexpr uint32_t SNAPSHOT_RATE     = 20;      // 20 snapshots per second
    static constexpr uint32_t TICKS_PER_SNAPSHOT= TICK_RATE / SNAPSHOT_RATE; // 3 ticks
};

struct CollisionConfig {
    static constexpr double CAR_LENGTH            = 4.2;   // meters
    static constexpr double CAR_WIDTH             = 2.0;   // meters
    static constexpr double COLLISION_RADIUS      = 1.5;   // Collision circle radius in meters
    static constexpr double MIN_SEPARATION_DIST   = 2.0 * COLLISION_RADIUS; // 3.0 meters
    static constexpr double RESTITUTION           = 0.35;  // Collision bounciness
    static constexpr double COLLISION_SPEED_SCRUB = 0.08;  // 8% kinetic energy scrub on collision
    static constexpr double MAX_IMPULSE_SPEED     = 40.0;  // km/h max impulse velocity change
};

struct CollisionEvent {
    uint32_t player1Id = 0;
    uint32_t player2Id = 0;
    double relativeSpeed = 0.0;
    double penetration = 0.0;
};

// Advances a player's physical state by dt seconds given current inputs along curved track
void updateVehicle(PlayerState& player, const Controls& controls, double dt,
                   const track::Track& track = track::getDefaultTrack());

// Resolves authoritative vehicle-to-vehicle collisions within a single room
void resolveRoomCollisions(std::unordered_map<uint32_t, PlayerState>& vehicles,
                           const track::Track& track = track::getDefaultTrack(),
                           std::vector<CollisionEvent>* outEvents = nullptr);

} // namespace physics
