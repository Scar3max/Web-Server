#pragma once

#include <cstdint>
#include <chrono>

struct Controls {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;

    bool operator==(const Controls& other) const = default;
};

struct PlayerState {
    uint32_t id = 0;
    int socketFd = -1;
    uint64_t connectionId = 0;

    // Kinematics & Spatial Position
    double x = 0.0;          // Normalized lateral position [-1.0, 1.0] (0 = center)
    double distance = 0.0;   // Track progress in meters
    double speed = 0.0;      // Velocity in km/h
    double steerAngle = 0.0; // Heading tilt in radians

    Controls controls;
    uint64_t lastInputSeq = 0;
    std::chrono::steady_clock::time_point lastInputTime{};
    bool active = true;

    PlayerState() = default;
    PlayerState(uint32_t pId, int fd, uint64_t cId)
        : id(pId), socketFd(fd), connectionId(cId) {}
};
