#pragma once

#include "Player.h"

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

// Advances a player's physical state by dt seconds given current inputs
void updateVehicle(PlayerState& player, const Controls& controls, double dt);

} // namespace physics
