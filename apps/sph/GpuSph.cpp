#include "GpuSph.h"
#include "SphShaders.h"
#include <algorithm>

namespace sph {

namespace rhi = glue::rhi;

bool GpuSph::init(rhi::Device& device, std::string& err) {
    device_ = &device;
    struct Pass { PipelinePtr* p; const char* src; const char* name; };
    for (Pass p : {Pass{&clear_, kCsClear, "clear"}, Pass{&hash_, kCsHash, "hash"}, Pass{&scan_, kCsScan, "scan"},
                   Pass{&scatter_, kCsScatter, "scatter"}, Pass{&density_, kCsDensity, "density"},
                   Pass{&force_, kCsForce, "force"}, Pass{&integrate_, kCsIntegrate, "integrate"}}) {
        rhi::ComputePipelineDesc d;
        d.source = std::string("#version 450\n") + kCsCommon + p.src;
        d.debugName = p.name;
        std::string e;
        *p.p = device.createComputePipeline(d, &e);
        if (!*p.p) {
            err = std::string(p.name) + ": " + e;
            return false;
        }
    }
    return true;
}

GpuSph::BufferPtr GpuSph::makeBuffer(size_t bytes, const void* data, const char* name) const {
    rhi::BufferDesc d;
    d.size = std::max<size_t>(bytes, 16);
    // Initial data, when given, holds exactly `bytes`; pad tiny buffers with zeros.
    std::vector<uint8_t> padded;
    if (data && bytes < d.size) {
        padded.assign(d.size, 0);
        std::copy_n((const uint8_t*)data, bytes, padded.data());
        data = padded.data();
    }
    d.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CpuWrite | rhi::BufferUsage::CpuRead;
    d.initialData = data;
    d.debugName = name;
    return device_->createBuffer(d);
}

void GpuSph::destroy() {
    for (BufferPtr* b : {&pos_[0], &pos_[1], &vel_, &rho_, &velTmp_, &press_, &bucketOf_, &start_, &cursor_, &sorted_})
        b->reset();
    n_ = 0;
}

void GpuSph::shutdown() {
    destroy();
    for (PipelinePtr* p : {&clear_, &hash_, &scan_, &scatter_, &density_, &force_, &integrate_}) p->reset();
    device_ = nullptr;
}

void GpuSph::upload(const SphSolver& s) {
    destroy();
    n_ = s.count();
    std::vector<float> x4(n_ * 4), v4(n_ * 4);
    for (size_t i = 0; i < n_; ++i) {
        const Vec3 &x = s.positions()[i], &v = s.velocities()[i];
        x4[4 * i] = x.x; x4[4 * i + 1] = x.y; x4[4 * i + 2] = x.z; x4[4 * i + 3] = 1.0f;
        v4[4 * i] = v.x; v4[4 * i + 1] = v.y; v4[4 * i + 2] = v.z; v4[4 * i + 3] = 0.0f;
    }
    table_ = glue::HashGrid::tableSizeFor(n_);
    pos_[0] = makeBuffer(n_ * 16, x4.data(), "sph pos 0");
    pos_[1] = makeBuffer(n_ * 16, x4.data(), "sph pos 1");
    vel_ = makeBuffer(n_ * 16, v4.data(), "sph vel");
    rho_ = makeBuffer(n_ * 4, s.densities().data(), "sph rho");
    velTmp_ = makeBuffer(n_ * 16, nullptr, "sph vel tmp");
    press_ = makeBuffer(n_ * 4, nullptr, "sph pressure");
    bucketOf_ = makeBuffer(n_ * 4, nullptr, "sph bucket of");
    sorted_ = makeBuffer(n_ * 4, nullptr, "sph sorted");
    start_ = makeBuffer((table_ + 1) * 4, nullptr, "sph bucket start");
    cursor_ = makeBuffer(table_ * 4, nullptr, "sph bucket cursor");
    cur_ = 0;
    h_ = s.h(); mass_ = s.mass(); taitB_ = s.tait(); radius_ = 0.5f * s.spacing();
    t_ = s.simTime(); steps_ = s.steps();
}

void GpuSph::bindAll() const {
    rhi::Buffer* b[10] = {pos_[cur_].get(), vel_.get(), rho_.get(), pos_[1 - cur_].get(), velTmp_.get(),
                          press_.get(), bucketOf_.get(), start_.get(), cursor_.get(), sorted_.get()};
    rhi::CommandContext& ctx = device_->context();
    for (uint32_t i = 0; i < 10; ++i) ctx.bindStorageBuffer(i, *b[i]);
}

void GpuSph::setUniforms(rhi::Pipeline& s, const Params& p) const {
    glue::SphKernels k(h_);
    // Uniforms a pass does not use are ignored by the backend.
    s.set("uN", (uint32_t)n_);
    s.set("uMask", table_ - 1);
    s.set("uInvCell", 1.0f / h_);
    s.set("uH", h_); s.set("uH2", h_ * h_);
    s.set("uMass", mass_); s.set("uRho0", p.restDensity); s.set("uTaitB", taitB_);
    s.set("uDt", p.dt); s.set("uMu", p.viscosity); s.set("uXsph", p.xsph);
    s.set("uRest", p.restitution); s.set("uFriction", p.wallFriction); s.set("uRadius", radius_);
    s.set("uPoly6C", k.poly6C); s.set("uSpikyGradC", k.spikyGradC); s.set("uViscLapC", k.viscLapC);
    s.set("uGravity", p.gravity); s.set("uBox", p.box);
}

void GpuSph::dispatch(rhi::Pipeline& s, uint32_t threads, uint32_t local) const {
    rhi::CommandContext& ctx = device_->context();
    ctx.bindPipeline(s);
    ctx.dispatch((threads + local - 1) / local, 1, 1);
    ctx.barrier(rhi::Barrier::StorageBuffer);
}

void GpuSph::step(const Params& p) {
    if (!n_) return;
    // Stiffness may have changed in the UI; everything else is read from p each step.
    taitB_ = p.restDensity * p.soundSpeed * p.soundSpeed / 7.0f;
    bindAll();
    for (PipelinePtr* s : {&clear_, &hash_, &scan_, &scatter_, &density_, &force_, &integrate_}) setUniforms(**s, p);
    uint32_t n = (uint32_t)n_;
    dispatch(*clear_, table_ + 1);       // 1-3: counting sort into hash buckets
    dispatch(*hash_, n);
    dispatch(*scan_, 1024, 1024);
    dispatch(*scatter_, n);
    dispatch(*density_, n);              // 4: density + Tait pressure
    dispatch(*force_, n);                // 5: v* = v + dt a
    dispatch(*integrate_, n);            // 6: XSPH, x += dt v, walls -> other position buffer
    cur_ = 1 - cur_;
    t_ += p.dt;
    ++steps_;
}

void GpuSph::download(std::vector<Vec3>& x, std::vector<Vec3>& v, std::vector<float>& rho) const {
    std::vector<float> x4(n_ * 4), v4(n_ * 4);
    rho.resize(n_);
    x.resize(n_); v.resize(n_);
    if (!n_) return;
    device_->context().barrier(rhi::Barrier::BufferUpdate);
    pos_[cur_]->read(0, x4.data(), n_ * 16);
    vel_->read(0, v4.data(), n_ * 16);
    rho_->read(0, rho.data(), n_ * 4);
    for (size_t i = 0; i < n_; ++i) {
        x[i] = {x4[4 * i], x4[4 * i + 1], x4[4 * i + 2]};
        v[i] = {v4[4 * i], v4[4 * i + 1], v4[4 * i + 2]};
    }
}

} // namespace sph
