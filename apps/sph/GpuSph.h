#pragma once
// OpenGL 4.5 compute port of the WCSPH step (PCISPH stays CPU only). The whole step runs on the
// GPU, grid included, so particle data never leaves video memory and the renderer draws straight
// from the same buffers. Initial state and constants come from a SphSolver that was reset() with
// the same Params, so both paths start from identical particles.
#include "SphSolver.h"
#include "gfx/Shader.h"
#include <string>

namespace sph {

class GpuSph {
public:
    bool init(std::string& err);              // compiles the compute passes
    void upload(const SphSolver& s);          // copies particles + derived constants
    void step(const Params& p);               // one WCSPH step on the GPU (no sync)
    void download(std::vector<Vec3>& x, std::vector<Vec3>& v, std::vector<float>& rho) const;
    void destroy();                           // buffers only (re-upload keeps the programs)
    void shutdown();                          // buffers and programs; call while the context is alive

    size_t count() const { return n_; }
    GLuint posBuffer() const { return pos_[cur_]; }
    GLuint velBuffer() const { return vel_; }
    GLuint rhoBuffer() const { return rho_; }
    double simTime() const { return t_; }
    long long steps() const { return steps_; }

private:
    void bindAll() const;
    void setUniforms(prism::Shader& s, const Params& p) const;
    void dispatch(prism::Shader& s, uint32_t threads, uint32_t local = 128) const;

    prism::Shader clear_, hash_, scan_, scatter_, density_, force_, integrate_;
    GLuint pos_[2] = {0, 0}, vel_ = 0, rho_ = 0, velTmp_ = 0, press_ = 0;
    GLuint bucketOf_ = 0, start_ = 0, cursor_ = 0, sorted_ = 0;
    int cur_ = 0;
    size_t n_ = 0;
    uint32_t table_ = 0;
    float h_ = 0, mass_ = 0, taitB_ = 0, radius_ = 0;
    double t_ = 0;
    long long steps_ = 0;
};

} // namespace sph
