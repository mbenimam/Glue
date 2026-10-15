#include "SphSolver.h"
#include <omp.h>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace sph {

using glue::kPi;

namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }
bool finite3(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
}

// ---- scene setup ----

// Fills [lo, hi) with a cubic lattice of spacing d_ (cell centres), with a tiny deterministic
// jitter: a perfect lattice is an unstable equilibrium that WCSPH keeps for a while, then breaks
// all at once.
void SphSolver::addBlock(Vec3 lo, Vec3 hi, std::vector<Vec3>& out) const {
    int nx = (int)std::floor((hi.x - lo.x) / d_), ny = (int)std::floor((hi.y - lo.y) / d_), nz = (int)std::floor((hi.z - lo.z) / d_);
    uint32_t s = 2463534242u + (uint32_t)out.size();
    auto jitter = [&]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return ((s & 0xffff) / 65535.0f - 0.5f) * 0.02f * d_; };
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                out.push_back(lo + Vec3{(i + 0.5f) * d_ + jitter(), (j + 0.5f) * d_ + jitter(), (k + 0.5f) * d_ + jitter()});
}

void SphSolver::addSphere(Vec3 c, float r, std::vector<Vec3>& out) const {
    std::vector<Vec3> cube;
    addBlock(c - Vec3{r, r, r}, c + Vec3{r, r, r}, cube);
    for (const Vec3& q : cube) if (glue::length(q - c) <= r) out.push_back(q);
}

void SphSolver::reset(const Params& p) {
    p_ = p;
    const Vec3 B = p_.box;
    // Fluid volume of the scene, to derive the spacing that gives ~targetCount particles.
    float vol = 0, poolH = 0.15f, dropR = 0.16f;
    switch (p_.scene) {
        case Scene::DamBreak: vol = 0.5f * 0.7f * B.z; break;
        case Scene::DoubleDamBreak: vol = 2 * 0.45f * 0.7f * B.z; break;
        case Scene::Drop: vol = B.x * poolH * B.z + 4.0f / 3.0f * kPi * dropR * dropR * dropR; break;
    }
    d_ = std::cbrt(vol / (float)std::max(p_.targetCount, 100));
    k_.setRadius(p_.hRatio * d_);

    x_.clear();
    switch (p_.scene) {
        case Scene::DamBreak: addBlock({0, 0, 0}, {0.5f, 0.7f, B.z}, x_); break;
        case Scene::DoubleDamBreak:
            addBlock({0, 0, 0}, {0.45f, 0.7f, B.z}, x_);
            addBlock({B.x - 0.45f, 0, 0}, {B.x, 0.7f, B.z}, x_);
            break;
        case Scene::Drop:
            addBlock({0, 0, 0}, {B.x, poolH, B.z}, x_);
            addSphere({B.x * 0.5f, 0.62f, B.z * 0.5f}, dropR, x_);
            break;
    }
    size_t n = x_.size();
    v_.assign(n, Vec3{});
    aNp_.assign(n, Vec3{}); aP_.assign(n, Vec3{}); xPred_.assign(n, Vec3{}); vTmp_.assign(n, Vec3{});
    rho_.assign(n, p_.restDensity); press_.assign(n, 0.0f); rhoPred_.assign(n, p_.restDensity);
    t_ = 0; steps_ = 0; st_ = {};
    refreshDerived();
}

void SphSolver::refreshDerived() {
    // Mass such that a particle inside the initial lattice sums to exactly the rest density:
    // m * sum_j W(x_ij) = rho0. Without this the fluid starts compressed or stretched and jumps.
    // The same full-neighbourhood "prototype" gives PCISPH its pressure scaling delta.
    int K = (int)std::ceil(p_.hRatio);
    float sumW = 0, sumGG = 0;
    Vec3 sumG{};
    for (int k = -K; k <= K; ++k)
        for (int j = -K; j <= K; ++j)
            for (int i = -K; i <= K; ++i) {
                Vec3 r{i * d_, j * d_, k * d_};
                float rl = glue::length(r);
                sumW += k_.poly6(rl * rl);
                Vec3 g = k_.spikyGradient(r, rl);
                sumG += g;
                sumGG += glue::dot(g, g);
            }
    m_ = p_.restDensity / sumW;
    // Tait: p = B ((rho/rho0)^7 - 1), with B = rho0 c0^2 / 7 so that dp/drho = c0^2 at rest.
    taitB_ = p_.restDensity * p_.soundSpeed * p_.soundSpeed / 7.0f;
    // PCISPH: a density error e is removed in one step by the pressure delta * e, where
    // delta = 1 / (beta (sum grad W . sum grad W + sum grad W . grad W)), beta = 2 (dt m / rho0)^2.
    float beta = 2.0f * (p_.dt * m_ / p_.restDensity) * (p_.dt * m_ / p_.restDensity);
    pciDelta_ = 1.0f / (beta * (glue::dot(sumG, sumG) + sumGG));
}

float SphSolver::suggestedDt() const {
    // CFL: nothing should travel more than 0.4 h per step. WCSPH must resolve sound (c0);
    // PCISPH only the flow, bounded by free fall over the container height.
    float vmax = p_.solver == Solver::WCSPH ? p_.soundSpeed
                                            : std::sqrt(2.0f * glue::length(p_.gravity) * p_.box.y) + 1e-3f;
    return std::min(0.4f * k_.h / vmax, 0.005f);
}

// ---- building blocks ----

void SphSolver::buildNeighbors() {
    const int n = (int)x_.size();
    auto t0 = Clock::now();
    grid_.build(x_.data(), x_.size(), k_.h);
    st_.msGrid = msSince(t0);
    nStart_.assign(n + 1, 0);
    // One query per particle: each thread takes a contiguous range, appends its neighbours to its
    // own buffer and records the counts; after a prefix sum the buffers are copied into place.
    if ((int)threadNbr_.size() < omp_get_max_threads()) threadNbr_.resize(omp_get_max_threads());
#pragma omp parallel
    {
        int t = omp_get_thread_num(), nt = omp_get_num_threads();
        int i0 = (int)((long long)n * t / nt), i1 = (int)((long long)n * (t + 1) / nt);
        std::vector<uint32_t>& buf = threadNbr_[t];
        buf.clear();
        for (int i = i0; i < i1; ++i) {
            size_t before = buf.size();
            grid_.forEachNeighbor(x_[i], k_.h, [&](uint32_t j) { buf.push_back(j); });
            nStart_[i + 1] = (uint32_t)(buf.size() - before);
        }
#pragma omp barrier
#pragma omp single
        {
            for (int i = 0; i < n; ++i) nStart_[i + 1] += nStart_[i];
            nbr_.resize(nStart_[n]);
        }
        if (i1 > i0) std::copy(buf.begin(), buf.end(), nbr_.begin() + nStart_[i0]);
    }
    st_.avgNeighbors = n ? (float)nStart_[n] / n : 0.0f;
}

// rho_i = sum_j m W(x_i - x_j), self included (that is where most of a lone particle's density comes from).
void SphSolver::computeDensity(const std::vector<Vec3>& pos, std::vector<float>& out) const {
    const int n = (int)pos.size();
#pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        float s = 0;
        for (uint32_t k = nStart_[i]; k < nStart_[i + 1]; ++k) {
            Vec3 r = pos[i] - pos[nbr_[k]];
            s += k_.poly6(glue::dot(r, r));
        }
        out[i] = m_ * s;
    }
}

// Gravity + viscosity: a_i = g + (mu / rho_i) sum_j m (v_j - v_i) / rho_j lap W(r_ij).
void SphSolver::nonPressureAccel() {
    const int n = (int)x_.size();
    const float mu = p_.viscosity;
#pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        Vec3 a{};
        if (mu > 0) {
            for (uint32_t k = nStart_[i]; k < nStart_[i + 1]; ++k) {
                uint32_t j = nbr_[k];
                if (j == (uint32_t)i) continue;
                float r = glue::length(x_[i] - x_[j]);
                a += (v_[j] - v_[i]) * (m_ / rho_[j] * k_.viscosityLaplacian(r));
            }
            a = a * (mu / rho_[i]);
        }
        aNp_[i] = a + p_.gravity;
    }
}

void SphSolver::clampToBox(Vec3& x, Vec3* v) const {
    const float r = 0.5f * d_;
    float* xp = &x.x;
    float* vp = v ? &v->x : nullptr;
    const float* hi = &p_.box.x;
    for (int a = 0; a < 3; ++a) {
        bool lo = xp[a] < r, up = xp[a] > hi[a] - r;
        if (!lo && !up) continue;
        xp[a] = lo ? r : hi[a] - r;
        if (!vp) continue;
        // Reflect the velocity going into the wall, scaled by the restitution; damp the rest.
        if ((lo && vp[a] < 0) || (up && vp[a] > 0)) vp[a] = -p_.restitution * vp[a];
        for (int b = 0; b < 3; ++b) if (b != a) vp[b] *= 1.0f - p_.wallFriction;
    }
}

// Shared tail of both solvers. vStar holds v + dt a. XSPH replaces each velocity by a blend with
// its neighbours' (v_i += eps sum_j m / rho_ij (v_j - v_i) W_ij), which damps particle noise
// without slowing bulk flow; then positions move with the smoothed velocity, walls clamp.
void SphSolver::finishStep(std::vector<Vec3>& vStar) {
    const int n = (int)x_.size();
    const float eps = p_.xsph, dt = p_.dt;
#pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        Vec3 dv{};
        if (eps > 0)
            for (uint32_t k = nStart_[i]; k < nStart_[i + 1]; ++k) {
                uint32_t j = nbr_[k];
                Vec3 r = x_[i] - x_[j];
                dv += (vStar[j] - vStar[i]) * (2.0f * m_ / (rho_[i] + rho_[j]) * k_.poly6(glue::dot(r, r)));
            }
        v_[i] = vStar[i] + dv * eps;
    }
    float maxV2 = 0, errSum = 0, errMax = 0;
    bool ok = true;
#pragma omp parallel for schedule(static) reduction(max : maxV2, errMax) reduction(+ : errSum) reduction(&& : ok)
    for (int i = 0; i < n; ++i) {
        x_[i] += v_[i] * dt;
        clampToBox(x_[i], &v_[i]);
        maxV2 = std::max(maxV2, glue::dot(v_[i], v_[i]));
        float e = std::max(rho_[i] / p_.restDensity - 1.0f, 0.0f);
        errSum += e;
        errMax = std::max(errMax, e);
        ok = ok && finite3(x_[i]) && finite3(v_[i]);
    }
    st_.maxSpeed = std::sqrt(maxV2);
    if (p_.solver == Solver::WCSPH) { st_.avgDensityError = n ? errSum / n : 0; st_.maxDensityError = errMax; }
    st_.finite = st_.finite && ok;
    t_ += dt;
    ++steps_;
}

// ---- WCSPH ----
void SphSolver::stepWcsph() {
    const int n = (int)x_.size();
    auto t0 = Clock::now();
    computeDensity(x_, rho_);
    // Tait equation of state, clamped at 0: a free surface must not pull particles together
    // (negative pressure clumps particles: the "tensile instability").
    const float rho0 = p_.restDensity, B = taitB_;
#pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        float q = rho_[i] / rho0, q2 = q * q, q7 = q2 * q2 * q2 * q;
        press_[i] = std::max(B * (q7 - 1.0f), 0.0f);
    }
    st_.msDensity = msSince(t0);
    t0 = Clock::now();
    nonPressureAccel();
    // Symmetric pressure force: a_i = -sum_j m (p_i / rho_i^2 + p_j / rho_j^2) grad W_ij.
    // Symmetric in i,j so momentum is conserved pairwise.
    const float dt = p_.dt;
#pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        Vec3 a{};
        float pi = press_[i] / (rho_[i] * rho_[i]);
        for (uint32_t k = nStart_[i]; k < nStart_[i + 1]; ++k) {
            uint32_t j = nbr_[k];
            if (j == (uint32_t)i) continue;
            Vec3 r = x_[i] - x_[j];
            float rl = glue::length(r);
            a -= k_.spikyGradient(r, rl) * (m_ * (pi + press_[j] / (rho_[j] * rho_[j])));
        }
        aP_[i] = a;
        vTmp_[i] = v_[i] + (aNp_[i] + a) * dt;
    }
    st_.msPressure = msSince(t0);
    st_.iterations = 1;
}

// ---- PCISPH ----
void SphSolver::stepPcisph() {
    const int n = (int)x_.size();
    auto t0 = Clock::now();
    computeDensity(x_, rho_);
    nonPressureAccel();
    st_.msDensity = msSince(t0);
    t0 = Clock::now();
    const float dt = p_.dt, rho0 = p_.restDensity, invRho02 = 1.0f / (rho0 * rho0);
    std::fill(press_.begin(), press_.end(), 0.0f);
    std::fill(aP_.begin(), aP_.end(), Vec3{});
    int it = 0;
    float avgErr = 0, maxErr = 0;
    while (it < p_.pciMaxIter) {
        // 1. Predict where every particle would go with the current pressure guess.
#pragma omp parallel for schedule(static)
        for (int i = 0; i < n; ++i) {
            Vec3 v = v_[i] + (aNp_[i] + aP_[i]) * dt;
            xPred_[i] = x_[i] + v * dt;
            clampToBox(xPred_[i], nullptr);
        }
        // 2. Density at the predicted positions (same neighbour lists: particles move < h/2 per step),
        //    and raise pressure in proportion to the compression it would cause.
        float errSum = 0, errMax = 0;
#pragma omp parallel for schedule(static) reduction(+ : errSum) reduction(max : errMax)
        for (int i = 0; i < n; ++i) {
            float s = 0;
            for (uint32_t k = nStart_[i]; k < nStart_[i + 1]; ++k) {
                Vec3 r = xPred_[i] - xPred_[nbr_[k]];
                s += k_.poly6(glue::dot(r, r));
            }
            float err = m_ * s - rho0;
            rhoPred_[i] = m_ * s;
            press_[i] = std::max(press_[i] + pciDelta_ * err, 0.0f);
            float rel = std::max(err / rho0, 0.0f);
            errSum += rel;
            errMax = std::max(errMax, rel);
        }
        avgErr = errSum / std::max(n, 1);
        maxErr = errMax;
        // 3. Pressure acceleration from the updated pressures: -m sum (p_i + p_j) / rho0^2 grad W.
#pragma omp parallel for schedule(static)
        for (int i = 0; i < n; ++i) {
            Vec3 a{};
            for (uint32_t k = nStart_[i]; k < nStart_[i + 1]; ++k) {
                uint32_t j = nbr_[k];
                if (j == (uint32_t)i) continue;
                Vec3 r = x_[i] - x_[j];
                a -= k_.spikyGradient(r, glue::length(r)) * (m_ * (press_[i] + press_[j]) * invRho02);
            }
            aP_[i] = a;
        }
        ++it;
        if (it >= p_.pciMinIter && avgErr < p_.pciMaxError) break;
    }
    st_.iterations = it;
    st_.avgDensityError = avgErr;
    st_.maxDensityError = maxErr;
#pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) vTmp_[i] = v_[i] + (aNp_[i] + aP_[i]) * dt;
    st_.msPressure = msSince(t0);
}

void SphSolver::step() {
    if (x_.empty()) return;
    auto t0 = Clock::now(), ts = t0;
    buildNeighbors();
    st_.msNeighbors = msSince(t0);
    if (p_.solver == Solver::WCSPH) stepWcsph();
    else stepPcisph();
    t0 = Clock::now();
    finishStep(vTmp_);
    st_.msIntegrate = msSince(t0);
    st_.msTotal = msSince(ts);
}

} // namespace sph
