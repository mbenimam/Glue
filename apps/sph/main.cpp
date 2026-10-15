// SPH: a particle fluid in a box. Dam break, drop into a pool, double dam break.
//
// Each frame:   N solver steps (CPU WCSPH / CPU PCISPH / GPU WCSPH) --> particle buffers
//               --> sphere impostors into an offscreen target --> viewport panel
// Headless modes (all exit when done; run with --hidden):
//   --bench           ms/step for every solver over a range of particle counts
//   --check           2000 steps per solver and scene; fails on NaN, escape or blow-up
//   --verify-gpu      runs the CPU and GPU WCSPH side by side and compares them
// Scene flags: --scene dam|drop|double  --solver wcsph|pcisph|gpu  --count N  --steps N (pre-simulate)
#include "core/Application.h"
#include "core/Log.h"
#include "gfx/Shader.h"
#include "gfx/Texture.h"
#include "scene/Camera.h"
#include "GpuSph.h"
#include "SphShaders.h"
#include "SphSolver.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <omp.h>
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

using namespace glue;
using namespace sph;

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

const char* kSceneNames[] = {"Dam break", "Drop", "Double dam break"};
const char* kSolverNames[] = {"WCSPH (CPU)", "PCISPH (CPU)", "WCSPH (GPU compute)"};
enum Backend { CpuWcsph, CpuPcisph, GpuWcsph };

// Summary of a particle state, for comparing runs and for the stability check.
struct Summary {
    Vec3 com{};
    float meanSpeed = 0, maxSpeed = 0, meanDensity = 0, avgCompression = 0, maxX = 0;
    bool finite = true, inside = true;
};

Summary summarize(const std::vector<Vec3>& x, const std::vector<Vec3>& v, const std::vector<float>& rho,
                  const Params& p, float radius) {
    Summary s;
    size_t n = x.size();
    if (!n) return s;
    double cx = 0, cy = 0, cz = 0, sp = 0, d = 0, c = 0;
    for (size_t i = 0; i < n; ++i) {
        cx += x[i].x; cy += x[i].y; cz += x[i].z;
        float speed = length(v[i]);
        sp += speed;
        s.maxSpeed = std::max(s.maxSpeed, speed);
        d += rho[i];
        c += std::max(rho[i] / p.restDensity - 1.0f, 0.0f);
        s.maxX = std::max(s.maxX, x[i].x);
        s.finite = s.finite && std::isfinite(x[i].x + x[i].y + x[i].z + v[i].x + v[i].y + v[i].z);
        for (int a = 0; a < 3; ++a) {
            float xa = (&x[i].x)[a], hi = (&p.box.x)[a];
            if (xa < radius * 0.999f || xa > hi - radius * 0.999f) s.inside = false;
        }
    }
    s.com = {(float)(cx / n), (float)(cy / n), (float)(cz / n)};
    s.meanSpeed = (float)(sp / n);
    s.meanDensity = (float)(d / n);
    s.avgCompression = (float)(c / n);
    return s;
}

} // namespace

class SphApp : public Application {
public:
    SphApp() : Application(AppConfig{"Glue SPH", 1600, 900, true, false}) {}

protected:
    bool onInit(int argc, char** argv) override {
        ImGui::GetIO().IniFilename = "glue_sph.ini";   // own layout file, not GlueLab's
        if (!particles_.compileGraphics(kParticleVs, kParticleFs) || !box_.compileGraphics(kBoxVs, kBoxFs)) {
            Log::error("render shaders: " + particles_.error() + box_.error());
            return false;
        }
        glCreateVertexArrays(1, &emptyVao_);
        glCreateQueries(GL_TIME_ELAPSED, 2, queries_);
        std::string err;
        gpuOk_ = gpu_.init(err);
        if (!gpuOk_) Log::warn("GPU compute path disabled: " + err);

        int preSteps = 0;
        bool bench = false, check = false, verify = false;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i], next = i + 1 < argc ? argv[i + 1] : "";
            if (a == "--scene") params_.scene = next == "drop" ? Scene::Drop : next == "double" ? Scene::DoubleDamBreak : Scene::DamBreak;
            if (a == "--solver") backend_ = next == "wcsph" ? CpuWcsph : next == "gpu" ? GpuWcsph : CpuPcisph;
            if (a == "--count") params_.targetCount = std::atoi(next.c_str());
            if (a == "--steps") preSteps = std::atoi(next.c_str());
            if (a == "--bench") bench = true;
            if (a == "--check") check = true;
            if (a == "--verify-gpu") verify = true;
        }
        Log::info("OpenMP threads: " + std::to_string(omp_get_max_threads()));
        if (bench || check || verify) {
            bool ok = true;
            if (verify) ok = verifyGpu() && ok;
            if (check) ok = stabilityCheck() && ok;
            if (bench) runBench();
            quit();
            return ok;
        }
        if (backend_ == GpuWcsph && !gpuOk_) backend_ = CpuWcsph;
        reset();
        for (int s = 0; s < preSteps; ++s) stepOnce();
        if (preSteps) Log::info("pre-simulated " + std::to_string(preSteps) + " steps, t = " + std::to_string(simTime()) + " s");
        return true;
    }

    void onUpdate(float) override {
        if (paused_ && !stepRequested_) return;
        int n = stepRequested_ ? 1 : stepsPerFrame_;
        stepRequested_ = false;
        if (backend_ == GpuWcsph) {
            // GPU time from a timer query, read back a frame later so we never stall on it.
            GLuint q = queries_[queryIdx_];
            if (queryPending_[queryIdx_]) {
                GLuint64 ns = 0;
                glGetQueryObjectui64v(q, GL_QUERY_RESULT, &ns);
                gpuMsPerStep_ = ns * 1e-6 / std::max(queryStepsIn_[queryIdx_], 1);
            }
            glBeginQuery(GL_TIME_ELAPSED, q);
            for (int s = 0; s < n; ++s) gpu_.step(params_);
            glEndQuery(GL_TIME_ELAPSED);
            queryPending_[queryIdx_] = true;
            queryStepsIn_[queryIdx_] = n;
            queryIdx_ = 1 - queryIdx_;
            if (frame() % 30 == 0) refreshGpuSummary();
        } else {
            for (int s = 0; s < n; ++s) cpu_.step();
            msHistory_[histIdx_] = (float)cpu_.stats().msTotal;
            histIdx_ = (histIdx_ + 1) % kHist;
        }
    }

    void onShutdown() override {
        gpu_.shutdown();
        particles_.destroy(); box_.destroy();
        fbo_.destroy();
        GLuint bufs[] = {cpuPos_, cpuVel_, cpuRho_};
        for (GLuint b : bufs) if (b) glDeleteBuffers(1, &b);
        if (queries_[0]) glDeleteQueries(2, queries_);
        if (emptyVao_) glDeleteVertexArrays(1, &emptyVao_);
        emptyVao_ = 0;
    }

    void onScroll(double, double dy) override {
        if (viewportHovered_) camera_.zoom((float)dy);
    }

    void onGui() override {
        ImGuiID dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (!layoutBuilt_) { layoutBuilt_ = true; buildDefaultLayout(dock); }
        mainMenu();
        simulationPanel();
        renderingPanel();
        statsPanel();
        consolePanel();
        viewportPanel();
    }

    void onRender() override {
        int w = std::max(vpW_, 16), h = std::max(vpH_, 16);
        fbo_.resize(w, h, GL_RGBA8);
        fbo_.bind();
        glEnable(GL_DEPTH_TEST);
        glClearColor(0.05f, 0.06f, 0.08f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        Mat4 proj = camera_.projection((float)w / h), view = camera_.view();
        glBindVertexArray(emptyVao_);

        // Container: floor quad, then the edges.
        box_.bind();
        box_.set("uViewProj", proj * view);
        box_.set("uBox", params_.box);
        box_.set("uFloor", 1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        box_.set("uFloor", 0);
        glDrawArrays(GL_LINES, 0, 24);

        // Particles. CPU solvers upload their arrays; the GPU solver's buffers are drawn as they are.
        size_t n = 0;
        if (backend_ == GpuWcsph) {
            n = gpu_.count();
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gpu_.posBuffer());
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gpu_.velBuffer());
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, gpu_.rhoBuffer());
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        } else {
            n = cpu_.count();
            uploadCpuParticles();
        }
        if (n) {
            particles_.bind();
            particles_.set("uView", view);
            particles_.set("uProj", proj);
            particles_.set("uRadius", 0.5f * cpu_.spacing() * radiusScale_);
            particles_.set("uRho0", params_.restDensity);
            particles_.set("uMaxSpeed", maxSpeedColor_);
            particles_.set("uDensRange", densRange_);
            particles_.set("uColorBy", colorBy_);
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)n);
        }
        Framebuffer::unbind();
    }

private:
    // ---- simulation control ----
    void reset() {
        params_.solver = backend_ == CpuPcisph ? Solver::PCISPH : Solver::WCSPH;
        cpu_.reset(params_);
        if (autoDt_) {
            params_.dt = cpu_.suggestedDt();
            cpu_.params().dt = params_.dt;
            cpu_.refreshDerived();
            stepsPerFrame_ = std::clamp((int)std::lround((1.0 / 60.0) / params_.dt), 1, maxAutoSteps_);
        }
        if (backend_ == GpuWcsph) gpu_.upload(cpu_);
        gpuSummary_ = {};
        Log::info(std::string(kSceneNames[(int)params_.scene]) + ", " + kSolverNames[backend_] + ": " +
                  std::to_string(cpu_.count()) + " particles, h = " + std::to_string(cpu_.h()) +
                  " m, dt = " + std::to_string(params_.dt * 1000.0f) + " ms");
    }

    void stepOnce() {
        if (backend_ == GpuWcsph) gpu_.step(params_);
        else cpu_.step();
    }

    double simTime() const { return backend_ == GpuWcsph ? gpu_.simTime() : cpu_.simTime(); }

    // Live parameter edits that do not need a reset go straight to the running solver.
    void applyLiveParams() {
        Params& p = cpu_.params();
        p.viscosity = params_.viscosity; p.xsph = params_.xsph; p.gravity = params_.gravity;
        p.dt = params_.dt; p.soundSpeed = params_.soundSpeed; p.restitution = params_.restitution;
        p.wallFriction = params_.wallFriction; p.pciMaxError = params_.pciMaxError;
        p.pciMaxIter = params_.pciMaxIter; p.restDensity = params_.restDensity;
        cpu_.refreshDerived();
    }

    void refreshGpuSummary() {
        std::vector<Vec3> x, v;
        std::vector<float> rho;
        gpu_.download(x, v, rho);
        gpuSummary_ = summarize(x, v, rho, params_, 0.5f * cpu_.spacing());
    }

    void uploadCpuParticles() {
        size_t n = cpu_.count();
        if (n != cpuCapacity_) {
            GLuint bufs[] = {cpuPos_, cpuVel_, cpuRho_};
            for (GLuint b : bufs) if (b) glDeleteBuffers(1, &b);
            glCreateBuffers(1, &cpuPos_); glCreateBuffers(1, &cpuVel_); glCreateBuffers(1, &cpuRho_);
            glNamedBufferData(cpuPos_, (GLsizeiptr)(std::max<size_t>(n, 1) * 16), nullptr, GL_STREAM_DRAW);
            glNamedBufferData(cpuVel_, (GLsizeiptr)(std::max<size_t>(n, 1) * 16), nullptr, GL_STREAM_DRAW);
            glNamedBufferData(cpuRho_, (GLsizeiptr)(std::max<size_t>(n, 1) * 4), nullptr, GL_STREAM_DRAW);
            cpuCapacity_ = n;
        }
        staging_.resize(n * 4);
        auto put = [&](const std::vector<Vec3>& src, GLuint buf) {
            for (size_t i = 0; i < n; ++i) {
                staging_[4 * i] = src[i].x; staging_[4 * i + 1] = src[i].y; staging_[4 * i + 2] = src[i].z; staging_[4 * i + 3] = 1;
            }
            glNamedBufferSubData(buf, 0, (GLsizeiptr)(n * 16), staging_.data());
        };
        put(cpu_.positions(), cpuPos_);
        put(cpu_.velocities(), cpuVel_);
        glNamedBufferSubData(cpuRho_, 0, (GLsizeiptr)(n * 4), cpu_.densities().data());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, cpuPos_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, cpuVel_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, cpuRho_);
    }

    // ---- headless modes ----

    // Same scene on CPU WCSPH and GPU WCSPH. After one step the per-particle densities must match
    // to float rounding (same neighbours, same formulas, different summation order). Later the
    // two trajectories diverge chaotically, so only bulk statistics are compared.
    bool verifyGpu() {
        if (!gpuOk_) { Log::error("verify-gpu: no compute path"); return false; }
        Params p = params_;
        p.solver = Solver::WCSPH;
        p.targetCount = 20000;
        cpu_.reset(p);
        p.dt = cpu_.suggestedDt();
        cpu_.params().dt = p.dt;
        cpu_.refreshDerived();
        gpu_.upload(cpu_);
        std::vector<Vec3> gx, gv;
        std::vector<float> grho;
        cpu_.step();
        gpu_.step(p);
        gpu_.download(gx, gv, grho);
        double maxRel = 0, maxDx = 0;
        for (size_t i = 0; i < cpu_.count(); ++i) {
            maxRel = std::max(maxRel, (double)std::fabs(grho[i] - cpu_.densities()[i]) / cpu_.densities()[i]);
            maxDx = std::max(maxDx, (double)length(gx[i] - cpu_.positions()[i]));
        }
        std::printf("verify-gpu: %zu particles, dt = %.3f ms\n", cpu_.count(), p.dt * 1000);
        std::printf("  after 1 step: max |rho_gpu - rho_cpu| / rho = %.2e, max |x_gpu - x_cpu| = %.2e m\n", maxRel, maxDx);
        bool ok = maxRel < 1e-4 && maxDx < 1e-5;
        std::printf("  %-8s %10s %10s %10s %10s %10s %10s %10s\n", "t (s)", "", "com.x", "com.y", "mean|v|", "max|v|", "mean rho", "front x");
        int total = 1;
        for (int checkpoint : {250, 500, 1000, 2000}) {
            for (; total < checkpoint; ++total) { cpu_.step(); gpu_.step(p); }
            gpu_.download(gx, gv, grho);
            Summary c = summarize(cpu_.positions(), cpu_.velocities(), cpu_.densities(), p, 0.5f * cpu_.spacing());
            Summary g = summarize(gx, gv, grho, p, 0.5f * cpu_.spacing());
            std::printf("  %-8.3f %10s %10.4f %10.4f %10.4f %10.4f %10.2f %10.4f\n", cpu_.simTime(), "CPU", c.com.x, c.com.y, c.meanSpeed, c.maxSpeed, c.meanDensity, c.maxX);
            std::printf("  %-8s %10s %10.4f %10.4f %10.4f %10.4f %10.2f %10.4f\n", "", "GPU", g.com.x, g.com.y, g.meanSpeed, g.maxSpeed, g.meanDensity, g.maxX);
            // Bulk agreement: centre of mass within 1 cm, mean speed within 5%, mean density within 0.5%.
            bool agree = length(c.com - g.com) < 0.01f && std::fabs(c.meanSpeed - g.meanSpeed) <= 0.05f * std::max(c.meanSpeed, 0.05f) &&
                         std::fabs(c.meanDensity - g.meanDensity) <= 0.005f * c.meanDensity && g.finite;
            if (!agree) std::printf("  ^ statistics disagree\n");
            ok = ok && agree;
        }
        std::printf("verify-gpu: %s\n", ok ? "PASS" : "FAIL");
        return ok;
    }

    // 2000 steps of every solver on every scene. Fails on NaN, a particle outside the box or a
    // blow-up (max speed beyond 3x free fall from the box height).
    bool stabilityCheck() {
        bool all = true;
        float vFall = std::sqrt(2.0f * 9.81f * params_.box.y);
        std::printf("stability: 2000 steps each, %d target particles\n", params_.targetCount);
        std::printf("  %-18s %-20s %8s %8s %9s %9s %10s %8s\n", "scene", "solver", "n", "dt (ms)", "t end (s)", "max|v|", "avg comp", "result");
        for (int b = 0; b < 3; ++b) {
            if (b == GpuWcsph && !gpuOk_) continue;
            for (int sc = 0; sc < 3; ++sc) {
                Params p = params_;
                p.scene = (Scene)sc;
                p.solver = b == CpuPcisph ? Solver::PCISPH : Solver::WCSPH;
                cpu_.reset(p);
                p.dt = cpu_.suggestedDt();
                cpu_.params().dt = p.dt;
                cpu_.refreshDerived();
                if (b == GpuWcsph) gpu_.upload(cpu_);
                float maxV = 0;
                bool ok = true;
                Summary s;
                for (int k = 0; k < 2000 && ok; ++k) {
                    if (b == GpuWcsph) gpu_.step(p); else cpu_.step();
                    if (k % 100 == 99) {
                        if (b == GpuWcsph) {
                            std::vector<Vec3> x, v; std::vector<float> rho;
                            gpu_.download(x, v, rho);
                            s = summarize(x, v, rho, p, 0.5f * cpu_.spacing());
                        } else {
                            s = summarize(cpu_.positions(), cpu_.velocities(), cpu_.densities(), p, 0.5f * cpu_.spacing());
                        }
                        maxV = std::max(maxV, s.maxSpeed);
                        ok = s.finite && s.inside && s.maxSpeed < 3.0f * vFall;
                    }
                }
                std::printf("  %-18s %-20s %8zu %8.3f %9.3f %9.3f %9.2f%% %8s\n", kSceneNames[sc], kSolverNames[b], cpu_.count(),
                            p.dt * 1000, 2000 * p.dt, maxV, 100 * s.avgCompression, ok ? "ok" : "FAILED");
                all = all && ok;
            }
        }
        std::printf("stability: %s\n", all ? "PASS" : "FAIL");
        return all;
    }

    // Timings are medians over the timed steps (GPU: over batches of 10 steps between glFinish),
    // so a background process stealing a core for a moment does not skew a row.
    void runBench() {
        const int warm = 20, timed = 100;
        auto median = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v.empty() ? 0.0 : v[v.size() / 2]; };
        std::printf("bench: dam break, %d warm-up + %d timed steps per row, %d OpenMP threads, medians\n", warm, timed, omp_get_max_threads());
        std::printf("| particles | solver | dt (ms) | ms/step | neighbours | grid build | neighbour lists | density | pressure | integrate | iters | ms per simulated second |\n");
        std::printf("|---|---|---|---|---|---|---|---|---|---|---|---|\n");
        for (int target : {5000, 10000, 20000, 40000, 80000, 160000}) {
            for (int b = 0; b < 3; ++b) {
                if (b == GpuWcsph && !gpuOk_) continue;
                if (b == CpuPcisph && target > 80000) continue;   // minutes per row; not worth it
                Params p = params_;
                p.scene = Scene::DamBreak;
                p.targetCount = target;
                p.solver = b == CpuPcisph ? Solver::PCISPH : Solver::WCSPH;
                cpu_.reset(p);
                p.dt = cpu_.suggestedDt();
                cpu_.params().dt = p.dt;
                cpu_.refreshDerived();
                if (b == GpuWcsph) {
                    gpu_.upload(cpu_);
                    for (int k = 0; k < warm; ++k) gpu_.step(p);
                    glFinish();
                    std::vector<double> batches;
                    for (int k = 0; k < timed / 10; ++k) {
                        auto t0 = Clock::now();
                        for (int s = 0; s < 10; ++s) gpu_.step(p);
                        glFinish();
                        batches.push_back(msSince(t0) / 10);
                    }
                    double ms = median(batches);
                    std::printf("| %zu | %s | %.3f | %.3f | | | | | | | 1 | %.0f |\n", gpu_.count(), kSolverNames[b], p.dt * 1000, ms, ms / p.dt);
                    std::fflush(stdout);
                    continue;
                }
                for (int k = 0; k < warm; ++k) cpu_.step();
                std::vector<double> tot, gr, nb, de, pr, in;
                double it = 0, neigh = 0;
                for (int k = 0; k < timed; ++k) {
                    cpu_.step();
                    const Stats& s = cpu_.stats();
                    tot.push_back(s.msTotal); gr.push_back(s.msGrid); nb.push_back(s.msNeighbors - s.msGrid);
                    de.push_back(s.msDensity); pr.push_back(s.msPressure); in.push_back(s.msIntegrate);
                    it += s.iterations; neigh += s.avgNeighbors;
                }
                double ms = median(tot);
                std::printf("| %zu | %s | %.3f | %.2f | %.1f | %.2f | %.2f | %.2f | %.2f | %.2f | %.1f | %.0f |\n", cpu_.count(), kSolverNames[b],
                            p.dt * 1000, ms, neigh / timed, median(gr), median(nb), median(de), median(pr), median(in), it / timed, ms / p.dt);
                std::fflush(stdout);
            }
        }
    }

    // ---- UI ----
    // First run: left = simulation + rendering settings, centre = viewport, right = stats + console.
    void buildDefaultLayout(ImGuiID root) {
        if (ImGui::DockBuilderGetNode(root) && ImGui::DockBuilderGetNode(root)->IsSplitNode()) return;  // restored from ini
        ImGui::DockBuilderRemoveNode(root);
        ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->Size);
        ImGuiID left, rest, right, center, leftBottom, rightBottom;
        left = ImGui::DockBuilderSplitNode(root, ImGuiDir_Left, 0.21f, nullptr, &rest);
        right = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.24f, nullptr, &center);
        leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.30f, nullptr, &left);
        rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.40f, nullptr, &right);
        ImGui::DockBuilderDockWindow("Simulation", left);
        ImGui::DockBuilderDockWindow("Rendering", leftBottom);
        ImGui::DockBuilderDockWindow("Stats", right);
        ImGui::DockBuilderDockWindow("Console", rightBottom);
        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderFinish(root);
    }

    void mainMenu() {
        if (!ImGui::BeginMainMenuBar()) return;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Quit")) quit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scene")) {
            for (int s = 0; s < 3; ++s)
                if (ImGui::MenuItem(kSceneNames[s], nullptr, (int)params_.scene == s)) { params_.scene = (Scene)s; reset(); }
            ImGui::EndMenu();
        }
        ImGui::Text("   %.1f fps   t = %.3f s", ImGui::GetIO().Framerate, simTime());
        ImGui::EndMainMenuBar();
    }

    void simulationPanel() {
        ImGui::Begin("Simulation");
        int scene = (int)params_.scene, backend = backend_;
        bool needReset = false;
        needReset |= ImGui::Combo("Scene", &scene, kSceneNames, 3);
        if (ImGui::Combo("Solver", &backend, kSolverNames, gpuOk_ ? 3 : 2)) { backend_ = (Backend)backend; needReset = true; }
        params_.scene = (Scene)scene;
        if (ImGui::Button(paused_ ? "Run" : "Pause", {70, 0})) paused_ = !paused_;
        ImGui::SameLine();
        if (ImGui::Button("Step", {70, 0})) { paused_ = true; stepRequested_ = true; }
        ImGui::SameLine();
        if (ImGui::Button("Reset", {70, 0})) needReset = true;
        ImGui::SliderInt("steps / frame", &stepsPerFrame_, 1, 60);

        ImGui::SeparatorText("Resolution (resets)");
        ImGui::SliderInt("particles", &params_.targetCount, 1000, 200000, "%d", ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemDeactivatedAfterEdit()) needReset = true;
        ImGui::SliderFloat("h / spacing", &params_.hRatio, 1.5f, 3.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) needReset = true;
        ImGui::TextDisabled("h = %.4f m, spacing %.4f m, mass %.3g kg", cpu_.h(), cpu_.spacing(), cpu_.mass());

        ImGui::SeparatorText("Fluid");
        bool live = false;
        live |= ImGui::SliderFloat("rest density", &params_.restDensity, 100.0f, 3000.0f, "%.0f kg/m3");
        if (ImGui::IsItemDeactivatedAfterEdit()) needReset = true;   // mass is derived from it
        live |= ImGui::SliderFloat("viscosity mu", &params_.viscosity, 0.0f, 20.0f, "%.2f Pa s", ImGuiSliderFlags_Logarithmic);
        live |= ImGui::SliderFloat("XSPH eps", &params_.xsph, 0.0f, 0.5f, "%.3f");
        live |= ImGui::SliderFloat("gravity y", &params_.gravity.y, -20.0f, 5.0f, "%.2f m/s2");
        live |= ImGui::SliderFloat("restitution", &params_.restitution, 0.0f, 1.0f, "%.2f");

        ImGui::SeparatorText("Time step");
        float dtMs = params_.dt * 1000.0f;
        if (ImGui::SliderFloat("dt", &dtMs, 0.05f, 5.0f, "%.3f ms", ImGuiSliderFlags_Logarithmic)) { params_.dt = dtMs * 0.001f; live = true; autoDt_ = false; }
        ImGui::Checkbox("CFL dt on reset", &autoDt_);
        ImGui::SameLine();
        if (ImGui::SmallButton("CFL now")) { params_.dt = cpu_.suggestedDt(); live = true; }
        if (backend_ != CpuPcisph) {
            live |= ImGui::SliderFloat("sound speed c0", &params_.soundSpeed, 5.0f, 100.0f, "%.1f m/s");
            ImGui::TextDisabled("Tait stiffness B = %.3g Pa", params_.restDensity * params_.soundSpeed * params_.soundSpeed / 7);
        } else {
            float pct = params_.pciMaxError * 100.0f;
            if (ImGui::SliderFloat("max density err", &pct, 0.05f, 5.0f, "%.2f %%", ImGuiSliderFlags_Logarithmic)) { params_.pciMaxError = pct * 0.01f; live = true; }
            live |= ImGui::SliderInt("max iterations", &params_.pciMaxIter, 3, 100);
        }
        if (live) applyLiveParams();
        if (needReset) reset();
        ImGui::End();
    }

    void renderingPanel() {
        ImGui::Begin("Rendering");
        ImGui::RadioButton("speed", &colorBy_, 0);
        ImGui::SameLine();
        ImGui::RadioButton("density", &colorBy_, 1);
        if (colorBy_ == 0) ImGui::SliderFloat("speed range", &maxSpeedColor_, 0.5f, 10.0f, "%.1f m/s");
        else ImGui::SliderFloat("density range", &densRange_, 0.005f, 0.2f, "+-%.3f");
        ImGui::SliderFloat("radius scale", &radiusScale_, 0.3f, 1.5f, "%.2f");
        ImGui::End();
    }

    void statsPanel() {
        ImGui::Begin("Stats");
        ImGui::Text("%s", kSolverNames[backend_]);
        ImGui::Text("particles   %zu", backend_ == GpuWcsph ? gpu_.count() : cpu_.count());
        ImGui::Text("sim time    %.3f s (%lld steps)", simTime(), backend_ == GpuWcsph ? gpu_.steps() : cpu_.steps());
        if (backend_ == GpuWcsph) {
            ImGui::Text("GPU         %.3f ms/step", gpuMsPerStep_);
            ImGui::SeparatorText("Every 30 frames (readback)");
            ImGui::Text("max |v|     %.3f m/s", gpuSummary_.maxSpeed);
            ImGui::Text("compression %.3f %% avg", 100 * gpuSummary_.avgCompression);
            ImGui::Text("state       %s", gpuSummary_.finite ? "finite" : "NaN!");
        } else {
            const Stats& s = cpu_.stats();
            ImGui::Text("threads     %d", omp_get_max_threads());
            ImGui::Text("step        %.2f ms", s.msTotal);
            ImGui::PlotLines("##ms", msHistory_, kHist, histIdx_, nullptr, 0.0f, FLT_MAX, {-1, 50});
            ImGui::Text("  grid      %.2f ms (+ lists %.2f)", s.msGrid, s.msNeighbors - s.msGrid);
            ImGui::Text("  density   %.2f ms", s.msDensity);
            ImGui::Text("  pressure  %.2f ms", s.msPressure);
            ImGui::Text("  integrate %.2f ms", s.msIntegrate);
            ImGui::Separator();
            ImGui::Text("neighbours  %.1f avg", s.avgNeighbors);
            if (backend_ == CpuPcisph) ImGui::Text("iterations  %d", s.iterations);
            ImGui::Text("compression %.3f %% avg, %.2f %% max", 100 * s.avgDensityError, 100 * s.maxDensityError);
            ImGui::Text("max |v|     %.3f m/s", s.maxSpeed);
            if (!s.finite) ImGui::TextColored({1, 0.4f, 0.4f, 1}, "NaN detected: reduce dt and reset");
        }
        ImGui::End();
    }

    void consolePanel() {
        ImGui::Begin("Console");
        for (const auto& l : Log::snapshot()) ImGui::TextUnformatted(l.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::End();
    }

    void viewportPanel() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("Viewport");
        ImVec2 sz = ImGui::GetContentRegionAvail();
        vpW_ = (int)sz.x; vpH_ = (int)sz.y;
        if (fbo_.colorTexture())
            ImGui::Image((ImTextureID)(intptr_t)fbo_.colorTexture(), sz, {0, 1}, {1, 0});
        viewportHovered_ = ImGui::IsItemHovered();
        ImGuiIO& io = ImGui::GetIO();
        if (viewportHovered_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) camera_.rotate(io.MouseDelta.x, io.MouseDelta.y);
        ImGui::End();
        ImGui::PopStyleVar();
    }

    Params params_;
    Backend backend_ = CpuPcisph;
    SphSolver cpu_;
    GpuSph gpu_;
    bool gpuOk_ = false;
    Summary gpuSummary_;
    Shader particles_, box_;
    Framebuffer fbo_;
    GLuint emptyVao_ = 0, cpuPos_ = 0, cpuVel_ = 0, cpuRho_ = 0;
    size_t cpuCapacity_ = 0;
    std::vector<float> staging_;
    GLuint queries_[2] = {0, 0};
    bool queryPending_[2] = {false, false};
    int queryStepsIn_[2] = {1, 1}, queryIdx_ = 0;
    double gpuMsPerStep_ = 0;
    OrbitCamera camera_{{0.8f, 0.3f, 0.3f}, 0.75f, 0.42f, 2.6f};

    static constexpr int kHist = 120;
    float msHistory_[kHist] = {};
    int histIdx_ = 0;
    bool paused_ = false, stepRequested_ = false, autoDt_ = true, layoutBuilt_ = false, viewportHovered_ = false;
    int stepsPerFrame_ = 4, maxAutoSteps_ = 12, colorBy_ = 0;
    float maxSpeedColor_ = 3.0f, densRange_ = 0.03f, radiusScale_ = 1.0f;
    int vpW_ = 960, vpH_ = 540;
};

int main(int argc, char** argv) {
    SphApp app;
    return app.run(argc, argv);
}
