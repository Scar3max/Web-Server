#include "Physics.h"
#include <algorithm>
#include <cmath>

namespace physics {

void updateVehicle(PlayerState& player, const Controls& controls, double dt) {
    player.controls = controls;

    // 1. Longitudinal Acceleration & Braking
    if (controls.up) {
        player.speed += Config::ACCEL_RATE * dt;
        if (player.speed > Config::MAX_FORWARD_SPEED) {
            player.speed = Config::MAX_FORWARD_SPEED;
        }
    } else if (controls.down) {
        player.speed -= Config::BRAKE_RATE * dt;
        if (player.speed < Config::MAX_REVERSE_SPEED) {
            player.speed = Config::MAX_REVERSE_SPEED;
        }
    } else {
        // Natural rolling friction / aerodynamic drag
        if (player.speed > 0.0) {
            player.speed -= Config::FRICTION * dt;
            if (player.speed < 0.0) {
                player.speed = 0.0;
            }
        }
    }

    // 2. Lateral Steering (effectiveness scales with forward velocity factor)
    // At 0 km/h, steering has zero effect; reaches full effectiveness by 40 km/h.
    double speedFactor = std::min(1.0, player.speed / 40.0);
    double targetSteer = 0.0;

    if (controls.left) {
        player.x -= Config::STEER_LATERAL_SPEED * speedFactor * dt;
        targetSteer = -Config::MAX_STEER_ANGLE;
    } else if (controls.right) {
        player.x += Config::STEER_LATERAL_SPEED * speedFactor * dt;
        targetSteer = Config::MAX_STEER_ANGLE;
    }

    // Smooth heading angle interpolation
    player.steerAngle += (targetSteer - player.steerAngle) * Config::STEER_DAMPING * dt;

    // Enforce track boundary limits
    player.x = std::clamp(player.x, Config::MIN_X, Config::MAX_X);

    // 3. Track Distance Progress
    // 1 km/h = 1000m / 3600s = (1.0 / 3.6) m/s
    double metersPerSecond = player.speed * (1000.0 / 3600.0);
    player.distance += metersPerSecond * dt;
}

} // namespace physics
