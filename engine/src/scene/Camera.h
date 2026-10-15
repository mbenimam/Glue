#pragma once
#include "math/Math.h"

namespace glue {

// Orbit camera: yaw/pitch around a target at a given distance.
struct OrbitCamera {
    Vec3 target{0, 0, 0};
    float yaw = 0.6f, pitch = 0.35f, distance = 4.0f;
    float fovy = radians(45.0f), znear = 0.05f, zfar = 200.0f;

    Vec3 position() const {
        float cp = std::cos(pitch);
        return target + Vec3{distance * cp * std::sin(yaw), distance * std::sin(pitch), distance * cp * std::cos(yaw)};
    }
    Mat4 view() const { return lookAt(position(), target, {0, 1, 0}); }
    Mat4 projection(float aspect) const { return perspective(fovy, aspect, znear, zfar); }
    void rotate(float dx, float dy) {
        yaw -= dx * 0.01f;
        pitch = std::clamp(pitch + dy * 0.01f, -1.5f, 1.5f);
    }
    void zoom(float scroll) { distance = std::clamp(distance * std::pow(0.9f, scroll), 0.3f, 100.0f); }
};

} // namespace glue
