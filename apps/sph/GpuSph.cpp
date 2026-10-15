#include "GpuSph.h"
#include "SphShaders.h"

namespace sph {

using prism::Shader;

bool GpuSph::init(std::string& err) {
    struct Pass { Shader* s; const char* src; const char* name; };
    for (Pass p : {Pass{&clear_, kCsClear, "clear"}, Pass{&hash_, kCsHash, "hash"}, Pass{&scan_, kCsScan, "scan"},
                   Pass{&scatter_, kCsScatter, "scatter"}, Pass{&density_, kCsDensity, "density"},
                   Pass{&force_, kCsForce, "force"}, Pass{&integrate_, kCsIntegrate, "integrate"}}) {
        if (!p.s->compileCompute(std::string("#version 450\n") + kCsCommon + p.src)) {
            err = std::string(p.name) + ": " + p.s->error();
            return false;
        }
    }
    return true;
}

static GLuint makeBuffer(size_t bytes, const void* data = nullptr) {
    GLuint b = 0;
    glCreateBuffers(1, &b);
    glNamedBufferStorage(b, (GLsizeiptr)std::max<size_t>(bytes, 16), data, GL_DYNAMIC_STORAGE_BIT);
    return b;
}

void GpuSph::destroy() {
    GLuint all[] = {pos_[0], pos_[1], vel_, rho_, velTmp_, press_, bucketOf_, start_, cursor_, sorted_};
    for (GLuint b : all) if (b) glDeleteBuffers(1, &b);
    pos_[0] = pos_[1] = vel_ = rho_ = velTmp_ = press_ = bucketOf_ = start_ = cursor_ = sorted_ = 0;
    n_ = 0;
}

void GpuSph::shutdown() {
    destroy();
    for (Shader* s : {&clear_, &hash_, &scan_, &scatter_, &density_, &force_, &integrate_}) s->destroy();
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
    table_ = prism::HashGrid::tableSizeFor(n_);
    pos_[0] = makeBuffer(n_ * 16, x4.data());
    pos_[1] = makeBuffer(n_ * 16, x4.data());
    vel_ = makeBuffer(n_ * 16, v4.data());
    rho_ = makeBuffer(n_ * 4, s.densities().data());
    velTmp_ = makeBuffer(n_ * 16);
    press_ = makeBuffer(n_ * 4);
    bucketOf_ = makeBuffer(n_ * 4);
    sorted_ = makeBuffer(n_ * 4);
    start_ = makeBuffer((table_ + 1) * 4);
    cursor_ = makeBuffer(table_ * 4);
    cur_ = 0;
    h_ = s.h(); mass_ = s.mass(); taitB_ = s.tait(); radius_ = 0.5f * s.spacing();
    t_ = s.simTime(); steps_ = s.steps();
}

void GpuSph::bindAll() const {
    GLuint b[10] = {pos_[cur_], vel_, rho_, pos_[1 - cur_], velTmp_, press_, bucketOf_, start_, cursor_, sorted_};
    for (GLuint i = 0; i < 10; ++i) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, b[i]);
}

void GpuSph::setUniforms(Shader& s, const Params& p) const {
    prism::SphKernels k(h_);
    // Uniforms a pass does not use have location -1, which GL silently ignores.
    glProgramUniform1ui(s.id(), s.loc("uN"), (GLuint)n_);
    glProgramUniform1ui(s.id(), s.loc("uMask"), table_ - 1);
    s.set("uInvCell", 1.0f / h_);
    s.set("uH", h_); s.set("uH2", h_ * h_);
    s.set("uMass", mass_); s.set("uRho0", p.restDensity); s.set("uTaitB", taitB_);
    s.set("uDt", p.dt); s.set("uMu", p.viscosity); s.set("uXsph", p.xsph);
    s.set("uRest", p.restitution); s.set("uFriction", p.wallFriction); s.set("uRadius", radius_);
    s.set("uPoly6C", k.poly6C); s.set("uSpikyGradC", k.spikyGradC); s.set("uViscLapC", k.viscLapC);
    s.set("uGravity", p.gravity); s.set("uBox", p.box);
}

void GpuSph::dispatch(Shader& s, uint32_t threads, uint32_t local) const {
    s.bind();
    glDispatchCompute((threads + local - 1) / local, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

void GpuSph::step(const Params& p) {
    if (!n_) return;
    // Stiffness may have changed in the UI; everything else is read from p each step.
    taitB_ = p.restDensity * p.soundSpeed * p.soundSpeed / 7.0f;
    bindAll();
    for (Shader* s : {&clear_, &hash_, &scan_, &scatter_, &density_, &force_, &integrate_}) setUniforms(*s, p);
    uint32_t n = (uint32_t)n_;
    dispatch(clear_, table_ + 1);       // 1-3: counting sort into hash buckets
    dispatch(hash_, n);
    dispatch(scan_, 1024, 1024);
    dispatch(scatter_, n);
    dispatch(density_, n);              // 4: density + Tait pressure
    dispatch(force_, n);                // 5: v* = v + dt a
    dispatch(integrate_, n);            // 6: XSPH, x += dt v, walls -> other position buffer
    cur_ = 1 - cur_;
    t_ += p.dt;
    ++steps_;
}

void GpuSph::download(std::vector<Vec3>& x, std::vector<Vec3>& v, std::vector<float>& rho) const {
    std::vector<float> x4(n_ * 4), v4(n_ * 4);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glGetNamedBufferSubData(pos_[cur_], 0, (GLsizeiptr)(n_ * 16), x4.data());
    glGetNamedBufferSubData(vel_, 0, (GLsizeiptr)(n_ * 16), v4.data());
    rho.resize(n_);
    glGetNamedBufferSubData(rho_, 0, (GLsizeiptr)(n_ * 4), rho.data());
    x.resize(n_); v.resize(n_);
    for (size_t i = 0; i < n_; ++i) {
        x[i] = {x4[4 * i], x4[4 * i + 1], x4[4 * i + 2]};
        v[i] = {v4[4 * i], v4[4 * i + 1], v4[4 * i + 2]};
    }
}

} // namespace sph
