#pragma once
// CPU SPH fluid solver: weakly compressible SPH (WCSPH) and predictive-corrective incompressible
// SPH (PCISPH, Solenthaler & Pajarola 2009). Both share the neighbour search, the kernels, the
// viscosity / XSPH terms and the box boundary; they differ only in how pressure is obtained:
//   WCSPH   pressure from density through a stiff equation of state (Tait), one pass per step.
//           Needs a small time step: sound must not cross more than a fraction of h per step.
//   PCISPH  pressure is iterated: predict positions, measure the density error that would result,
//           raise pressure where the fluid would be compressed, repeat until the error is small.
//           Several neighbour passes per step, but the step can be ~5x larger.
// Loops are parallel over particles with OpenMP (each particle only writes its own entries).
#include "math/Math.h"
#include "physics/SphKernels.h"
#include "spatial/HashGrid.h"
#include <cstdint>
#include <vector>

namespace sph {

using glue::Vec3;

enum class Solver { WCSPH, PCISPH };
enum class Scene { DamBreak, Drop, DoubleDamBreak };

struct Params {
    Scene scene = Scene::DamBreak;
    Solver solver = Solver::PCISPH;
    int targetCount = 20000;        // particles; spacing is derived so the scene's fluid volume holds this many
    float hRatio = 2.0f;            // kernel radius h as a multiple of the particle spacing (2 -> ~30 neighbours)
    float restDensity = 1000.0f;    // kg/m^3
    float viscosity = 1.0f;         // dynamic viscosity mu (Pa s), Laplacian form
    float xsph = 0.05f;             // XSPH velocity smoothing epsilon (0 = off)
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    float dt = 0.002f;              // s; see suggestedDt()
    float soundSpeed = 40.0f;       // WCSPH: numerical speed of sound c0 (m/s), sets Tait stiffness
    float restitution = 0.2f;       // box walls: fraction of normal velocity kept after a bounce
    float wallFriction = 0.02f;     // box walls: fraction of tangential velocity lost at contact
    float pciMaxError = 0.005f;     // PCISPH: stop when the mean density error is below this (relative)
    int pciMinIter = 3, pciMaxIter = 40;
    Vec3 box{1.6f, 1.0f, 0.6f};     // container [0, box]
};

struct Stats {
    double msGrid = 0;              // hash grid build alone (part of msNeighbors)
    double msNeighbors = 0, msDensity = 0, msPressure = 0, msIntegrate = 0, msTotal = 0;
    int iterations = 0;             // PCISPH correction iterations of the last step
    float avgDensityError = 0;      // mean of max(rho - rho0, 0) / rho0: compression only, the free
    float maxDensityError = 0;      // surface is under-dense by construction and does not count
    float maxSpeed = 0;
    float avgNeighbors = 0;
    bool finite = true;             // false once a NaN/inf shows up in positions or velocities
};

class SphSolver {
public:
    void reset(const Params& p);    // rebuilds the scene from scratch
    void step();                    // advances one time step params().dt
    // Largest stable-ish dt for the current solver/resolution (CFL with lambda = 0.4).
    float suggestedDt() const;

    Params& params() { return p_; }
    const Params& params() const { return p_; }
    size_t count() const { return x_.size(); }
    const std::vector<Vec3>& positions() const { return x_; }
    const std::vector<Vec3>& velocities() const { return v_; }
    const std::vector<float>& densities() const { return rho_; }
    float spacing() const { return d_; }
    float h() const { return k_.h; }
    float mass() const { return m_; }
    float tait() const { return taitB_; }
    float pciDelta() const { return pciDelta_; }
    double simTime() const { return t_; }
    long long steps() const { return steps_; }
    const Stats& stats() const { return st_; }
    // Re-derives what depends on dt / stiffness / rest density after the UI changed them mid-run.
    void refreshDerived();

private:
    void buildNeighbors();
    void computeDensity(const std::vector<Vec3>& pos, std::vector<float>& out) const;
    void nonPressureAccel();        // gravity + viscosity -> aNp_
    void stepWcsph();
    void stepPcisph();
    void finishStep(std::vector<Vec3>& vStar);   // XSPH, positions, walls, stats
    void clampToBox(Vec3& x, Vec3* v) const;
    void addBlock(Vec3 lo, Vec3 hi, std::vector<Vec3>& out) const;
    void addSphere(Vec3 c, float r, std::vector<Vec3>& out) const;

    Params p_;
    glue::SphKernels k_;
    glue::HashGrid grid_;
    float d_ = 0.02f, m_ = 0.008f, taitB_ = 0, pciDelta_ = 0;
    double t_ = 0;
    long long steps_ = 0;
    Stats st_;

    std::vector<Vec3> x_, v_, aNp_, aP_, xPred_, vTmp_;
    std::vector<float> rho_, press_, rhoPred_;
    // Neighbour lists in CSR form: neighbours of i are nbr_[nStart_[i] .. nStart_[i+1]).
    std::vector<uint32_t> nStart_, nbr_;
    std::vector<std::vector<uint32_t>> threadNbr_;   // per-thread scratch for buildNeighbors()
};

} // namespace sph
