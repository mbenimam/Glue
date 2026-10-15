#pragma once
// GPU compute port of the WCSPH step (PCISPH stays CPU only), written against the engine's render
// hardware interface. The whole step runs on the GPU, grid included, so particle data never leaves
// video memory and the renderer draws straight from the same buffers. Initial state and constants
// come from a SphSolver that was reset() with the same Params, so both paths start from identical
// particles.
#include "SphSolver.h"
#include "rhi/Rhi.h"
#include <memory>
#include <string>

namespace sph {

class GpuSph {
public:
    bool init(glue::rhi::Device& device, std::string& err);   // compiles the compute passes
    void upload(const SphSolver& s);          // copies particles + derived constants
    void step(const Params& p);               // one WCSPH step on the GPU (no sync)
    void download(std::vector<Vec3>& x, std::vector<Vec3>& v, std::vector<float>& rho) const;
    void destroy();                           // buffers only (re-upload keeps the pipelines)
    void shutdown();                          // buffers and pipelines; call while the device is alive

    size_t count() const { return n_; }
    glue::rhi::Buffer& posBuffer() const { return *pos_[cur_]; }
    glue::rhi::Buffer& velBuffer() const { return *vel_; }
    glue::rhi::Buffer& rhoBuffer() const { return *rho_; }
    double simTime() const { return t_; }
    long long steps() const { return steps_; }

private:
    using PipelinePtr = std::unique_ptr<glue::rhi::Pipeline>;
    using BufferPtr = std::unique_ptr<glue::rhi::Buffer>;

    void bindAll() const;
    void setUniforms(glue::rhi::Pipeline& s, const Params& p) const;
    void dispatch(glue::rhi::Pipeline& s, uint32_t threads, uint32_t local = 128) const;
    BufferPtr makeBuffer(size_t bytes, const void* data, const char* name) const;

    glue::rhi::Device* device_ = nullptr;
    PipelinePtr clear_, hash_, scan_, scatter_, density_, force_, integrate_;
    BufferPtr pos_[2], vel_, rho_, velTmp_, press_;
    BufferPtr bucketOf_, start_, cursor_, sorted_;
    int cur_ = 0;
    size_t n_ = 0;
    uint32_t table_ = 0;
    float h_ = 0, mass_ = 0, taitB_ = 0, radius_ = 0;
    double t_ = 0;
    long long steps_ = 0;
};

} // namespace sph
