// SPH kernels: normalisation by numerical radial integration, and derivative consistency.
#include "physics/SphKernels.h"

#include "TestFramework.h"

using namespace glue;

// Integral of a radial function over the ball of radius h: int_0^h 4 pi r^2 f(r) dr.
// Composite Simpson on many intervals; the integrands are smooth on (0, h].
template <class F>
static double radialIntegral(F f, double h, int n = 20000) {
    double dr = h / n, s = 0.0;
    for (int i = 0; i <= n; ++i) {
        double r = i * dr;
        double w = (i == 0 || i == n) ? 1.0 : (i % 2 ? 4.0 : 2.0);
        double v = r > 0.0 ? 4.0 * 3.14159265358979323846 * r * r * f((float)r) : 0.0;
        s += w * v;
    }
    return s * dr / 3.0;
}

TEST(physics_testKernelNormalization) {
    for (float h : {0.05f, 0.1f, 1.0f, 2.5f}) {
        SphKernels k(h);
        CHECK_NEAR(radialIntegral([&](float r) { return k.poly6(r * r); }, h), 1.0, 1e-3);
        CHECK_NEAR(radialIntegral([&](float r) { return k.spiky(r); }, h), 1.0, 1e-3);
        // The viscosity kernel has an h/(2r) term, but r^2 * W stays finite at r = 0.
        CHECK_NEAR(radialIntegral([&](float r) { return k.viscosity(r); }, h), 1.0, 1e-3);
        // Compact support.
        CHECK(k.poly6(h * h * 1.01f) == 0.0f && k.spiky(h * 1.01f) == 0.0f && k.viscosity(h * 1.01f) == 0.0f);
    }
}

TEST(physics_testSpikyGradientIsDerivative) {
    SphKernels k(0.2f);
    int bad = 0;
    for (int i = 1; i < 200; ++i) {
        double r = 0.2 * i / 200.0, e = 1e-4 * 0.2;
        double fd = ((double)k.spiky((float)(r + e)) - k.spiky((float)(r - e))) / (2 * e);
        double an = k.spikyDerivative((float)r);
        if (std::fabs(fd - an) > 2e-3 * std::fabs(k.spikyGradC * 0.04)) ++bad;   // relative to |dW/dr(0)|
    }
    CHECK(bad == 0);
    // The vector gradient points from j to i scaled by dW/dr, i.e. towards j (dW/dr < 0).
    Vec3 rij{0.05f, 0.02f, -0.03f};
    float r = length(rij);
    Vec3 g = k.spikyGradient(rij, r);
    CHECK_NEAR(length(g), std::fabs(k.spikyDerivative(r)), 1e-3 * std::fabs(k.spikyDerivative(r)));
    CHECK(dot(g, rij) < 0.0f);
    // Finite differences of W(|x|) along each axis agree with the vector gradient.
    double e = 1e-4;
    double gx = (k.spiky(length(rij + Vec3{(float)e, 0, 0})) - k.spiky(length(rij - Vec3{(float)e, 0, 0}))) / (2 * e);
    CHECK_NEAR(gx, g.x, 1e-2 * length(g));
}

TEST(physics_testViscosityLaplacian) {
    // lap W = (1/r^2) d/dr (r^2 dW/dr) for a radial function; compare with the closed form on (0, h).
    double h = 0.5;
    SphKernels k((float)h);
    auto W = [&](double r) {   // double-precision copy of the kernel so second differences are clean
        double q = r / h;
        return 15.0 / (2.0 * 3.14159265358979323846 * h * h * h) * (-0.5 * q * q * q + q * q + 0.5 / q - 1.0);
    };
    CHECK_NEAR(W(0.3), k.viscosity(0.3f), 1e-4 * W(0.3));
    int bad = 0;
    for (int i = 10; i < 95; ++i) {
        double r = h * i / 100.0, e = 1e-3 * r;   // step scaled with r: W ~ 1/r near the origin
        auto dW = [&](double x) { return (W(x + e) - W(x - e)) / (2 * e); };
        double lap = ((r + e) * (r + e) * dW(r + e) - (r - e) * (r - e) * dW(r - e)) / (2 * e) / (r * r);
        if (std::fabs(lap - k.viscosityLaplacian((float)r)) > 1e-3 * k.viscosityLaplacian((float)r) + 1e-3) ++bad;
    }
    CHECK(bad == 0);
    CHECK(k.viscosityLaplacian(0.25f) > 0.0f);    // damping only: never pumps energy in
}
