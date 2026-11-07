#pragma once

#include <cstdint>
#include <chrono>
#include <string>

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
    uint32_t roomId = 0;
    std::string name;

    // Kinematics & Spatial Position
    double x = 0.0;          // Normalized lateral position [-1.0, 1.0] (0 = center)
    double distance = 0.0;   // Track progress in meters
    double speed = 0.0;      // Velocity in km/h
    double steerAngle = 0.0; // Heading tilt relative to track tangent in radians

    // World Coordinates & Global Heading
    double worldX = 0.0;     // World Cartesian X coordinate
    double worldY = 0.0;     // World Cartesian Y coordinate
    double heading = 0.0;    // Absolute world orientation in radians

    Controls controls;
    uint64_t lastInputSeq = 0;
    std::chrono::steady_clock::time_point lastInputTime{};
    bool active = true;
    bool finished = false;
    double finishTime = 0.0;
    uint32_t finishRank = 0;

    PlayerState() = default;
    PlayerState(uint32_t pId, int fd, uint64_t cId, uint32_t rId = 0, const std::string& pName = "")
        : id(pId), socketFd(fd), connectionId(cId), roomId(rId), name(pName) {}
};
