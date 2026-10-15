#pragma once
// The three classic 3D SPH smoothing kernels (Mueller, Charypar & Gross 2003), support radius h.
//
//   poly6      W(r) = 315 / (64 pi h^9) (h^2 - r^2)^3          smooth, cheap (only r^2): density
//   spiky      W(r) =  15 / (pi h^6)    (h - r)^3              gradient does not vanish at r -> 0:
//             dW/dr = -45 / (pi h^6)    (h - r)^2              pressure forces keep particles apart
//   viscosity  W(r) =  15 / (2 pi h^3) (-r^3/(2h^3) + r^2/h^2 + h/(2r) - 1)
//         lap W(r) =  45 / (pi h^6)    (h - r)                 Laplacian is positive everywhere:
//                                                              viscosity only ever damps
// All are zero for r > h and normalised so that the integral of W over the ball of radius h is 1
// (tests/test_physics.cpp checks this numerically). Gradients are with respect to x_i, taking
// r_ij = x_i - x_j, so grad W points from j to i scaled by dW/dr (negative: W falls with distance).
#include "math/Math.h"

namespace glue {

struct SphKernels {
    float h = 1, h2 = 1;
    float poly6C = 0, spikyC = 0, spikyGradC = 0, viscC = 0, viscLapC = 0;

    SphKernels() { setRadius(1.0f); }
    explicit SphKernels(float radius) { setRadius(radius); }

    void setRadius(float radius) {
        h = radius; h2 = h * h;
        float h3 = h2 * h, h6 = h3 * h3, h9 = h6 * h3;
        poly6C = 315.0f / (64.0f * kPi * h9);
        spikyC = 15.0f / (kPi * h6);
        spikyGradC = -45.0f / (kPi * h6);
        viscC = 15.0f / (2.0f * kPi * h3);
        viscLapC = 45.0f / (kPi * h6);
    }

    // poly6 takes r^2 so density loops never need a square root.
    float poly6(float r2) const {
        if (r2 >= h2) return 0.0f;
        float d = h2 - r2;
        return poly6C * d * d * d;
    }
    float spiky(float r) const {
        if (r >= h) return 0.0f;
        float d = h - r;
        return spikyC * d * d * d;
    }
    // dW/dr of the spiky kernel (a scalar, <= 0).
    float spikyDerivative(float r) const {
        if (r >= h) return 0.0f;
        float d = h - r;
        return spikyGradC * d * d;
    }
    // grad_i W(x_i - x_j) = dW/dr * r_ij / r. Zero at r = 0 (direction undefined there).
    Vec3 spikyGradient(const Vec3& rij, float r) const {
        if (r >= h || r <= 1e-12f) return {};
        return rij * (spikyDerivative(r) / r);
    }
    float viscosity(float r) const {
        if (r >= h || r <= 0.0f) return 0.0f;
        float q = r / h;
        return viscC * (-0.5f * q * q * q + q * q + 0.5f / q - 1.0f);
    }
    float viscosityLaplacian(float r) const {
        if (r >= h) return 0.0f;
        return viscLapC * (h - r);
    }
};

} // namespace glue
