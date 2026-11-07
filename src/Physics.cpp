#include "Physics.h"
#include <algorithm>
#include <cmath>

namespace physics {

void updateVehicle(PlayerState& player, const Controls& controls, double dt,
                   const track::Track& track) {
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

    // Smooth heading angle interpolation relative to track tangent
    player.steerAngle += (targetSteer - player.steerAngle) * Config::STEER_DAMPING * dt;

    // Enforce track boundary limits
    player.x = std::clamp(player.x, Config::MIN_X, Config::MAX_X);

    // 3. Track Distance Progress
    // 1 km/h = 1000m / 3600s = (1.0 / 3.6) m/s
    double metersPerSecond = player.speed * (1000.0 / 3600.0);
    player.distance += metersPerSecond * dt;

    // 4. Update World Position & Global Heading from Track Geometry
    track::TrackSample sample = track.sampleAtDistance(player.distance);
    double lateralMeters = player.x * (track.getTrackWidth() * 0.5);
    player.worldX = sample.center.x + sample.normal.x * lateralMeters;
    player.worldY = sample.center.y + sample.normal.y * lateralMeters;
    player.heading = sample.angle + player.steerAngle;
}

void resolveRoomCollisions(std::unordered_map<uint32_t, PlayerState>& vehicles,
                           const track::Track& track,
                           std::vector<CollisionEvent>* outEvents) {
    if (vehicles.size() < 2) return;

    std::vector<uint32_t> pids;
    pids.reserve(vehicles.size());
    for (const auto& [pid, _] : vehicles) {
        pids.push_back(pid);
    }

    double halfWidth = track.getTrackWidth() * 0.5;
    double dLimit = halfWidth - 0.2; // Keep at least 0.2m inside curbs

    // Run 2 relaxation solver passes for multi-car stability
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < pids.size(); i++) {
            for (size_t j = i + 1; j < pids.size(); j++) {
                auto& pA = vehicles[pids[i]];
                auto& pB = vehicles[pids[j]];

                double dA = pA.x * halfWidth;
                double dB = pB.x * halfWidth;

                double ds = pB.distance - pA.distance;
                double dd = dB - dA;

                // Broadphase bounding box check
                if (std::abs(ds) > 6.0 || std::abs(dd) > 4.5) {
                    continue;
                }

                double dist = std::sqrt(ds * ds + dd * dd);
                if (dist < CollisionConfig::MIN_SEPARATION_DIST) {
                    double penetration = CollisionConfig::MIN_SEPARATION_DIST - dist;

                    double ns = 0.0;
                    double nd = 1.0;
                    if (dist > 1e-4) {
                        ns = ds / dist;
                        nd = dd / dist;
                    }

                    if (outEvents && pass == 0) {
                        CollisionEvent ev;
                        ev.player1Id = pA.id;
                        ev.player2Id = pB.id;
                        ev.relativeSpeed = std::abs(pB.speed - pA.speed);
                        ev.penetration = penetration;
                        outEvents->push_back(ev);
                    }

                    // 1. Position Resolution (De-penetration)
                    if (!pA.finished && !pB.finished) {
                        pA.distance -= 0.5 * penetration * ns;
                        dA -= 0.5 * penetration * nd;
                        pB.distance += 0.5 * penetration * ns;
                        dB += 0.5 * penetration * nd;
                    } else if (pA.finished && !pB.finished) {
                        // A is finished; B takes full displacement
                        pB.distance += penetration * ns;
                        dB += penetration * nd;
                    } else if (!pA.finished && pB.finished) {
                        // B is finished; A takes full displacement
                        pA.distance -= penetration * ns;
                        dA -= penetration * nd;
                    }

                    // Boundary enforcement
                    dA = std::clamp(dA, -dLimit, dLimit);
                    dB = std::clamp(dB, -dLimit, dLimit);
                    pA.x = std::clamp(dA / halfWidth, -1.0, 1.0);
                    pB.x = std::clamp(dB / halfWidth, -1.0, 1.0);

                    // 2. Velocity Impulse Resolution (only on first pass to prevent over-damping)
                    if (pass == 0) {
                        double vsA = pA.speed * (1000.0 / 3600.0);
                        double vsB = pB.speed * (1000.0 / 3600.0);
                        double relV = (vsB - vsA) * ns;

                        if (relV < 0.0) { // Approaching each other
                            double impulse = -(1.0 + CollisionConfig::RESTITUTION) * relV * 0.5;
                            double maxImp = CollisionConfig::MAX_IMPULSE_SPEED * (1000.0 / 3600.0);
                            impulse = std::min(impulse, maxImp);

                            if (!pA.finished && !pB.finished) {
                                vsA -= impulse * ns;
                                vsB += impulse * ns;
                                vsA *= (1.0 - CollisionConfig::COLLISION_SPEED_SCRUB);
                                vsB *= (1.0 - CollisionConfig::COLLISION_SPEED_SCRUB);
                            } else if (pA.finished && !pB.finished) {
                                vsB += impulse * 1.5 * ns;
                                vsB *= (1.0 - CollisionConfig::COLLISION_SPEED_SCRUB * 2.0);
                            } else if (!pA.finished && pB.finished) {
                                vsA -= impulse * 1.5 * ns;
                                vsA *= (1.0 - CollisionConfig::COLLISION_SPEED_SCRUB * 2.0);
                            }

                            pA.speed = std::clamp(vsA * 3.6, 0.0, Config::MAX_FORWARD_SPEED);
                            pB.speed = std::clamp(vsB * 3.6, 0.0, Config::MAX_FORWARD_SPEED);
                        }
                    }

                    // Update world coordinates
                    track::TrackSample sA = track.sampleAtDistance(pA.distance);
                    pA.worldX = sA.center.x + sA.normal.x * dA;
                    pA.worldY = sA.center.y + sA.normal.y * dA;
                    pA.heading = sA.angle + pA.steerAngle;

                    track::TrackSample sB = track.sampleAtDistance(pB.distance);
                    pB.worldX = sB.center.x + sB.normal.x * dB;
                    pB.worldY = sB.center.y + sB.normal.y * dB;
                    pB.heading = sB.angle + pB.steerAngle;
                }
            }
        }
    }
}

} // namespace physics
