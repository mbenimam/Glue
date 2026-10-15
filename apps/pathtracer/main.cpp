// PathTracer: an interactive, progressive GPU path tracer.
//
// Each frame runs assets/shaders/pt.comp one or more times; every dispatch traces one new
// path per pixel and adds it to an RGBA32F accumulation image (rgb = sum, a = count). The
// display pass divides, tonemaps and gamma-encodes. Any change to the camera, scene or a
// render setting clears the image, so orbiting shows a 1 spp preview that refines as soon as
// the camera stops.
//
// Flags (on top of --hidden / --frames / --screenshot):
//   --bench [sec]        1280x720, no vsync, prints samples/s, MRays/s and BVH build times
//   --size WxH           render resolution instead of the viewport size
//   --spp N              stop at N samples per pixel (with --render-shot: write it and quit)
//   --render-shot f.png  the tonemapped render only, without the editor around it
//   --spf N              samples (dispatches) per frame
//   --model f.obj        trace an OBJ instead of the teapot
//   --strategy 0|1|2     BSDF sampling / light sampling / MIS,   --no-sky   light only
//   --segments N         teapot tessellation (per patch edge)
//   --mesh-type N        mesh material: 0 diffuse, 1 metal, 2 glass, 3 emissive
#include "core/Application.h"
#include "core/Assets.h"
#include "core/File.h"
#include "core/Log.h"
#include "gfx/ImageIO.h"
#include "gfx/ShaderToy.h"
#include "rhi/Rhi.h"
#include "scene/Camera.h"
#include "scene/MeshData.h"
#include "spatial/Bvh.h"
#include <imgui.h>
#include "ui/Theme.h"
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <chrono>
#include <cstring>

using namespace glue;

namespace {

// std430 mirrors of the structs in pt.comp.
struct GpuTri { float v0[4], e1[4], e2[4], n0[4], n1[4], n2[4]; };
struct GpuSphere { float c[3]; float r; uint32_t mat, pad[3]; };
struct GpuMaterial {
    float albedo[3]; float type;          // 0 diffuse, 1 metal, 2 glass
    float emission[3]; float roughness;
    float ior, checker, pad[2];
};
static_assert(sizeof(GpuTri) == 96 && sizeof(GpuSphere) == 32 && sizeof(GpuMaterial) == 48, "std430 layout");

constexpr int kMaxSpheres = 8;
constexpr int kMaterials = 2 + kMaxSpheres;   // 0 = mesh, 1 = ground, 2.. = spheres

// Everything that changes the converged image. Compared byte for byte every frame: any
// difference restarts the accumulation. Only 4-byte members, so there is no padding to differ.
struct Settings {
    int maxBounces = 8, rrStart = 3, misMode = 2;
    float clamp = 0.0f;
    int skyOn = 1;
    float skyIntensity = 0.3f;
    float lightIntensity = 16.0f, lightColor[3] = {1.0f, 0.92f, 0.82f}, lightSize = 2.0f, lightHeight = 4.2f;
    float fovDeg = 38.0f, aperture = 0.035f, focusDist = 6.0f;
    int autoFocus = 1;
    float camTarget[3] = {}, yaw = 0, pitch = 0, distance = 0;
    int sphereCount = 0;
    GpuSphere spheres[kMaxSpheres] = {};
    GpuMaterial mats[kMaterials] = {};
};

GpuMaterial material(int type, float r, float g, float b, float rough = 0.0f, float ior = 1.5f) {
    GpuMaterial m{};
    m.albedo[0] = r; m.albedo[1] = g; m.albedo[2] = b; m.type = (float)type;
    m.roughness = rough; m.ior = ior;
    return m;
}

double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

class PathTracer : public Application {
public:
    explicit PathTracer(bool bench) : Application(AppConfig{"Glue PathTracer", 1600, 900, !bench, false}) {}

protected:
    bool onInit(int argc, char** argv) override {
        ImGui::GetIO().IniFilename = "pathtracer.ini";   // own layout, not GlueLab's
        assets_ = assetsDir();
        bool spfGiven = false;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            bool more = i + 1 < argc;
            if (a == "--bench") { bench_ = true; if (more && argv[i + 1][0] != '-') benchSeconds_ = std::atof(argv[++i]); }
            else if (a == "--size" && more) std::sscanf(argv[++i], "%dx%d", &fixedW_, &fixedH_);
            else if (a == "--spp" && more) targetSpp_ = std::atoi(argv[++i]);
            else if (a == "--render-shot" && more) renderShot_ = argv[++i];
            else if (a == "--spf" && more) samplesPerFrame_ = std::max(1, std::atoi(argv[++i])), spfGiven = true;
            else if (a == "--model" && more) objPath_ = argv[++i];
            else if (a == "--strategy" && more) strategy_ = std::atoi(argv[++i]);
            else if (a == "--no-sky") noSky_ = true;
            else if (a == "--mesh-type" && more) meshType_ = std::atoi(argv[++i]);
            else if (a == "--segments" && more) teapotSegments_ = std::clamp(std::atoi(argv[++i]), 2, 128);
        }
        if (targetSpp_ > 0) maxSpp_ = std::max(maxSpp_, targetSpp_);
        if (bench_) { fixedW_ = 1280; fixedH_ = 720; if (!spfGiven) samplesPerFrame_ = 4; }

        // Display pass: one full-screen triangle (the vertex shader reads gl_VertexID only).
        rhi::GraphicsPipelineDesc dd;
        dd.vertexSource = kShadertoyVertex;
        dd.depth.test = false;
        dd.depth.write = false;
        dd.debugName = "pt display";
        std::string err;
        if (!readFile(assets_ + "/shaders/pt_display.frag", dd.fragmentSource) ||
            !(display_ = device().createGraphicsPipeline(dd, &err))) {
            Log::error("pt_display.frag: " + err);
            return false;
        }
        computePath_ = assets_ + "/shaders/pt.comp";
        if (!loadCompute()) return false;
        // Scene buffers. The BVH and triangle buffers are replaced by setMesh; spheres and
        // materials have a fixed size and are rewritten whenever the settings change.
        sphereBuf_ = storageBuffer(sizeof(Settings::spheres), nullptr, rhi::BufferUsage::CpuWrite, "pt spheres");
        matBuf_ = storageBuffer(sizeof(Settings::mats), nullptr, rhi::BufferUsage::CpuWrite, "pt materials");
        uint32_t zero = 0;
        counterBuf_ = storageBuffer(sizeof(uint32_t), &zero, rhi::BufferUsage::CpuWrite | rhi::BufferUsage::CpuRead,
                                    "pt ray counter");

        defaultScene();
        if (strategy_ >= 0) settings_.misMode = std::min(strategy_, 2);
        if (noSky_) settings_.skyOn = 0;
        if (meshType_ >= 0 && meshType_ <= 2) {
            settings_.mats[0].type = (float)meshType_;
            if (meshType_ == 2) { settings_.mats[0].albedo[0] = 0.95f; settings_.mats[0].albedo[1] = 0.97f; settings_.mats[0].albedo[2] = 1.0f; }
        }
        if (meshType_ == 3) { settings_.mats[0] = material(0, 0.8f, 0.8f, 0.8f); for (float& e : settings_.mats[0].emission) e = 4.0f; }
        if (bench_) benchBvh();
        if (objPath_.empty() || !loadModel(objPath_)) setMesh(makeTeapot(teapotSegments_), "Utah teapot");
        return true;
    }

    void onShutdown() override {
        // GPU objects must die while the device is still alive.
        display_.reset(); trace_.reset();
        accum_.reset(); outRt_.reset(); retired_.clear();
        nodeBuf_.reset(); triBuf_.reset(); sphereBuf_.reset(); matBuf_.reset(); counterBuf_.reset();
    }

    void onScroll(double, double dy) override {
        if (viewportHovered_) camera_.zoom((float)dy);
    }

    void onUpdate(float) override {
        // Images replaced during the last onRender were still referenced by that frame's GUI.
        retired_.clear();
        // Hot reload of the tracer, like GlueLab's effects: save pt.comp, see the result.
        if (frame() % 30 == 0) {
            long long s = fileStamp(computePath_);
            if (s && s != computeStamp_) { loadCompute(); reset(); }
        }
    }

    void onGui() override {
        ImGuiID dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (!layoutBuilt_) { layoutBuilt_ = true; buildDefaultLayout(dock); }
        mainMenu();
        renderPanel();
        scenePanel();
        statsPanel();
        consolePanel();
        viewportPanel();
    }

    void onRender() override {
        int w = fixedW_ > 0 ? fixedW_ : std::max(vpW_, 16);
        int h = fixedH_ > 0 ? fixedH_ : std::max(vpH_, 16);
        if (!accum_ || w != accum_->width() || h != accum_->height()) {
            rhi::TextureDesc a;
            a.width = w; a.height = h; a.format = rhi::Format::RGBA32F;
            a.minFilter = a.magFilter = rhi::Filter::Nearest; a.wrap = rhi::Wrap::ClampToEdge;
            a.storage = true; a.debugName = "pt accumulation";
            accum_ = device().createTexture(a);
            // This frame's GUI already refers to the old output image: keep it alive until the next frame.
            if (outRt_) retired_.push_back(std::move(outRt_));
            rhi::RenderTargetDesc o;
            o.width = w; o.height = h; o.colorFormat = rhi::Format::RGBA8; o.depth = false; o.debugName = "pt output";
            outRt_ = device().createRenderTarget(o);
            reset();
        }
        // Change detection: the whole settings block, including the camera, byte for byte.
        Settings s = settings_;
        s.camTarget[0] = camera_.target.x; s.camTarget[1] = camera_.target.y; s.camTarget[2] = camera_.target.z;
        s.yaw = camera_.yaw; s.pitch = camera_.pitch; s.distance = camera_.distance;
        if (s.autoFocus) s.focusDist = camera_.distance;
        settings_ = s;
        bool changed = std::memcmp(&s, &applied_, sizeof(Settings)) != 0;
        if (changed) {
            applied_ = s;
            uploadScene();
            reset();
        }

        // While something is moving show the cheapest preview: one sample per frame.
        int dispatches = changed ? 1 : samplesPerFrame_;
        if (targetSpp_ > 0) dispatches = std::min(dispatches, targetSpp_ - spp_);
        if (maxSpp_ > 0 && !bench_) dispatches = std::min(dispatches, maxSpp_ - spp_);
        rhi::CommandContext& ctx = context();
        if (trace_ && dispatches > 0) {
            if (bench_ && benchStart_ < 0) benchStart_ = nowMs();
            setUniforms(w, h);
            ctx.bindStorageImage(0, *accum_, rhi::Access::ReadWrite);
            for (int i = 0; i < dispatches; ++i) {
                trace_->set("uFrame", spp_);
                ctx.dispatch((w + 7) / 8, (h + 7) / 8, 1);
                ctx.barrier(rhi::Barrier::StorageImage);
                ++spp_;
            }
            ctx.barrier(rhi::Barrier::TextureFetch);
            samplesThisWindow_ += (double)dispatches * w * h;
        }

        // Display pass: accumulation -> tonemapped RGBA8 shown in the viewport panel.
        rhi::RenderPassDesc pass;
        pass.clearColor = false;    // the triangle covers every pixel
        pass.clearDepth = false;
        ctx.beginRenderPass(outRt_.get(), pass);
        ctx.bindPipeline(*display_);
        ctx.bindTexture(0, *accum_);
        display_->set("uExposure", exposure_);
        display_->set("uTonemap", tonemap_);
        ctx.draw(3);
        ctx.endRenderPass();

        // Throughput over half-second windows of wall clock time.
        double t = time();
        if (t - windowStart_ > 0.5) {
            samplesPerSec_ = samplesThisWindow_ / (t - windowStart_);
            samplesThisWindow_ = 0;
            windowStart_ = t;
        }
        if (bench_) benchFrame(w, h);
        if (saveRequested_) { saveRequested_ = false; savePng("pathtracer_" + std::to_string(spp_) + "spp.png"); }
        if (!bench_ && targetSpp_ > 0 && spp_ >= targetSpp_ && !renderShot_.empty()) {
            savePng(renderShot_);
            quit();
        }
    }

private:
    // ---- scene ----
    void defaultScene() {
        Settings& s = settings_;
        // Materials: copper teapot, checker floor, then the spheres around it.
        s.mats[0] = material(1, 0.95f, 0.64f, 0.54f, 0.28f);
        s.mats[1] = material(0, 0.62f, 0.62f, 0.60f);
        s.mats[1].checker = 1.0f;
        struct S { float x, y, z, r; GpuMaterial m; };
        const S list[] = {
            {-2.3f, 0.8f, -1.1f, 0.8f, material(1, 0.92f, 0.92f, 0.92f, 0.0f)},         // mirror
            {1.75f, 0.55f, 1.25f, 0.55f, material(2, 1.0f, 1.0f, 1.0f, 0.0f, 1.5f)},    // glass
            {-1.45f, 0.42f, 1.45f, 0.42f, material(0, 0.33f, 0.43f, 0.99f)},            // matte blue
            {2.45f, 0.7f, -1.35f, 0.7f, material(1, 0.91f, 0.12f, 0.39f, 0.35f)},       // rough rose metal
            {0.25f, 0.35f, -2.3f, 0.35f, material(0, 0.80f, 0.86f, 0.22f)},             // matte green
        };
        s.sphereCount = 0;
        for (const S& e : list) {
            GpuSphere& sp = s.spheres[s.sphereCount];
            sp = {};
            sp.c[0] = e.x; sp.c[1] = e.y; sp.c[2] = e.z; sp.r = e.r;
            sp.mat = (uint32_t)(2 + s.sphereCount);
            s.mats[2 + s.sphereCount] = e.m;
            ++s.sphereCount;
        }
        camera_ = OrbitCamera{{0, 0.5f, 0}, 0.55f, 0.32f, 5.9f};
    }

    // Puts the mesh on the floor at the origin, builds the BVH and uploads it.
    void setMesh(MeshData m, const std::string& name) {
        if (m.normals.size() != m.positions.size()) m.computeNormals();
        Vec3 lo, hi; m.bounds(lo, hi);
        for (auto& p : m.positions) p = p - Vec3{(lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f};
        double t0 = nowMs();
        Bvh bvh;
        bvh.build(m);
        bvhMs_ = nowMs() - t0;
        meshName_ = name;
        meshTris_ = m.triangleCount();
        bvhNodes_ = bvh.nodes.size();
        bvhDepth_ = bvh.depth();

        // The BVH reordered the triangles; fetch normals through triIndex.
        std::vector<GpuTri> gt(bvh.tris.size());
        for (size_t i = 0; i < gt.size(); ++i) {
            const Triangle& t = bvh.tris[i];
            const uint32_t* id = &m.indices[3 * bvh.triIndex[i]];
            auto put = [](float* d, const Vec3& v) { d[0] = v.x; d[1] = v.y; d[2] = v.z; d[3] = 0; };
            put(gt[i].v0, t.v0); put(gt[i].e1, t.v1 - t.v0); put(gt[i].e2, t.v2 - t.v0);
            put(gt[i].n0, m.normals[id[0]]); put(gt[i].n1, m.normals[id[1]]); put(gt[i].n2, m.normals[id[2]]);
        }
        // Never zero-sized: an empty mesh still binds a (zero-filled) buffer.
        nodeBuf_ = bvh.nodes.empty() ? storageBuffer(sizeof(BvhNode), nullptr, {}, "pt bvh nodes")
                                     : storageBuffer(bvh.nodes.size() * sizeof(BvhNode), bvh.nodes.data(), {}, "pt bvh nodes");
        triBuf_ = gt.empty() ? storageBuffer(sizeof(GpuTri), nullptr, {}, "pt triangles")
                             : storageBuffer(gt.size() * sizeof(GpuTri), gt.data(), {}, "pt triangles");
        hasMesh_ = !bvh.nodes.empty();
        Log::info(name + ": " + std::to_string(meshTris_) + " triangles, BVH " + std::to_string(bvhNodes_) +
                  " nodes, depth " + std::to_string(bvhDepth_) + ", built in " + std::to_string(bvhMs_) + " ms");
        reset();
    }

    bool loadModel(const std::string& path) {
        MeshData m; std::string err;
        if (!loadObj(path, m, &err)) { Log::warn("OBJ: " + err); return false; }
        m.normalizeToUnitBox();
        setMesh(std::move(m), path);
        return true;
    }

    void uploadScene() {
        const Settings& s = applied_;
        sphereBuf_->write(0, s.spheres, sizeof(s.spheres));
        matBuf_->write(0, s.mats, sizeof(s.mats));
    }

    // A storage buffer of `bytes`, zero-filled when `data` is null. `extra` adds CPU access flags.
    std::unique_ptr<rhi::Buffer> storageBuffer(size_t bytes, const void* data, rhi::BufferUsage extra, const char* name) {
        rhi::BufferDesc d;
        d.size = bytes;
        d.usage = rhi::BufferUsage::Storage | extra;
        d.initialData = data;
        d.debugName = name;
        return device().createBuffer(d);
    }

    bool loadCompute() {
        std::string src;
        computeStamp_ = fileStamp(computePath_);
        if (!readFile(computePath_, src)) { Log::error("cannot read " + computePath_); return false; }
        rhi::ComputePipelineDesc d;
        d.source = src;
        d.debugName = "pt.comp";
        std::string err;
        auto p = device().createComputePipeline(d, &err);
        if (!p) {                              // the previous pipeline keeps running
            Log::error("pt.comp: " + err);
            return trace_ != nullptr;
        }
        trace_ = std::move(p);
        Log::info("compiled " + computePath_);
        return true;
    }

    void reset() { spp_ = 0; }

    void setUniforms(int w, int h) {
        const Settings& s = applied_;
        rhi::Pipeline& p = *trace_;
        Vec3 pos = camera_.position();
        Vec3 fwd = normalize(camera_.target - pos);
        Vec3 right = normalize(cross(fwd, {0, 1, 0}));
        Vec3 up = cross(right, fwd);
        // The field of view is vertical for 16:9; narrower viewports keep that horizontal extent
        // instead of cropping the scene at the sides.
        float aspect = (float)w / h;
        float tanY = std::tan(radians(s.fovDeg) * 0.5f) * std::max(1.0f, (16.0f / 9.0f) / aspect);
        p.set("uWidth", w); p.set("uHeight", h);
        p.set("uSeed", seed_);
        p.set("uCamPos", pos); p.set("uCamRight", right); p.set("uCamUp", up); p.set("uCamFwd", fwd);
        p.set("uTanHalfX", tanY * aspect); p.set("uTanHalfY", tanY);
        p.set("uLensRadius", s.aperture); p.set("uFocusDist", s.focusDist);
        p.set("uMaxBounces", s.maxBounces); p.set("uRRStart", s.rrStart);
        p.set("uMisMode", s.misMode); p.set("uClamp", s.clamp);
        p.set("uSkyOn", s.skyOn); p.set("uSkyIntensity", s.skyIntensity);
        // Light: a square of side lightSize centred above the scene, normal pointing down.
        float half = s.lightSize * 0.5f;
        p.set("uLightCorner", Vec3{0.4f - half, s.lightHeight, 0.4f - half});
        p.set("uLightU", Vec3{s.lightSize, 0, 0});
        p.set("uLightV", Vec3{0, 0, s.lightSize});
        p.set("uLightRadiance", Vec3{s.lightColor[0], s.lightColor[1], s.lightColor[2]} * s.lightIntensity);
        p.set("uSphereCount", s.sphereCount); p.set("uHasMesh", hasMesh_ ? 1 : 0);
        p.set("uMeshMat", 0); p.set("uGroundMat", 1);
        p.set("uCountRays", bench_ ? 1 : 0);
        rhi::CommandContext& ctx = context();
        ctx.bindPipeline(p);
        ctx.bindStorageBuffer(0, *nodeBuf_);
        ctx.bindStorageBuffer(1, *triBuf_);
        ctx.bindStorageBuffer(2, *sphereBuf_);
        ctx.bindStorageBuffer(3, *matBuf_);
        ctx.bindStorageBuffer(4, *counterBuf_);
    }

    void savePng(const std::string& path) {
        if (gfx::savePngRgba8(path, outRt_->width(), outRt_->height(), context().readColorRgba8(outRt_.get())))
            Log::info("saved " + path + " (" + std::to_string(spp_) + " spp)");
        else
            Log::error("could not write " + path);
    }

    // ---- benchmark ----
    // BVH build time for the teapot at two tessellations (median of 5 builds each).
    void benchBvh() {
        for (int segs : {16, 64}) {
            MeshData m = makeTeapot(segs);
            std::vector<double> ms;
            Bvh bvh;
            for (int k = 0; k < 5; ++k) {
                double t0 = nowMs();
                bvh.build(m);
                ms.push_back(nowMs() - t0);
            }
            std::sort(ms.begin(), ms.end());
            std::printf("BENCH bvh teapot segments=%d triangles=%zu nodes=%zu depth=%d sah=%.2f build_ms=%.2f\n", segs,
                        m.triangleCount(), bvh.nodes.size(), bvh.depth(), bvh.sahCost(), ms[2]);
        }
    }

    // Reads the GPU ray counter every frame (it would overflow 32 bits in a few seconds).
    void benchFrame(int w, int h) {
        context().waitIdle();
        uint32_t rays = 0;
        counterBuf_->read(0, &rays, sizeof(uint32_t));
        uint32_t zero = 0;
        counterBuf_->write(0, &zero, sizeof(uint32_t));
        totalRays_ += rays;
        if (benchStart_ < 0) return;
        double elapsed = (nowMs() - benchStart_) / 1000.0;
        if (elapsed < benchSeconds_ && (targetSpp_ <= 0 || spp_ < targetSpp_)) return;
        double samples = (double)spp_ * w * h;
        std::printf("BENCH render %dx%d mesh=\"%s\" triangles=%zu seconds=%.2f spp=%d samples_per_s=%.1fM "
                    "rays=%.0fM mrays_per_s=%.1f rays_per_sample=%.2f ms_per_spp=%.2f\n",
                    w, h, meshName_.c_str(), meshTris_, elapsed, spp_, samples / elapsed / 1e6, totalRays_ / 1e6,
                    totalRays_ / elapsed / 1e6, totalRays_ / samples, elapsed * 1000.0 / spp_);
        std::fflush(stdout);
        if (!renderShot_.empty()) savePng(renderShot_);
        quit();
    }

    // ---- UI ----
    // Left: render settings above the scene/material editor, centre: viewport, right: stats + console.
    void buildDefaultLayout(ImGuiID root) {
        if (ImGui::DockBuilderGetNode(root) && ImGui::DockBuilderGetNode(root)->IsSplitNode()) return;  // restored from ini
        ImGui::DockBuilderRemoveNode(root);
        ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->Size);
        ImGuiID left, rest, right, center, leftBottom, rightBottom;
        left = ImGui::DockBuilderSplitNode(root, ImGuiDir_Left, 0.22f, nullptr, &rest);
        right = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.22f, nullptr, &center);
        leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.36f, nullptr, &left);
        rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);
        ImGui::DockBuilderDockWindow("Render", left);
        ImGui::DockBuilderDockWindow("Scene", leftBottom);
        ImGui::DockBuilderDockWindow("Stats", right);
        ImGui::DockBuilderDockWindow("Console", rightBottom);
        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderFinish(root);
    }

    void mainMenu() {
        if (!ImGui::BeginMainMenuBar()) return;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save PNG")) saveRequested_ = true;
            if (ImGui::MenuItem("Reload pt.comp")) { loadCompute(); reset(); }
            if (ImGui::MenuItem("Quit")) quit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scene")) {
            if (ImGui::MenuItem("Utah teapot")) setMesh(makeTeapot(teapotSegments_), "Utah teapot");
            if (ImGui::MenuItem("Default scene")) defaultScene();
            ImGui::EndMenu();
        }
        ImGui::Text("   %d spp   %.1f fps", spp_, ImGui::GetIO().Framerate);
        ImGui::EndMainMenuBar();
    }

    void renderPanel() {
        Settings& s = settings_;
        ImGui::Begin("Render");
        ImGui::PushItemWidth(-120);
        if (ImGui::Button("Reset")) reset();
        ImGui::SameLine();
        if (ImGui::Button("Save PNG")) saveRequested_ = true;
        ImGui::SliderInt("Max bounces", &s.maxBounces, 1, 32);
        ImGui::SliderInt("Roulette from", &s.rrStart, 1, 16);
        ImGui::SliderInt("Samples/frame", &samplesPerFrame_, 1, 32);
        ImGui::SliderInt("Stop at spp", &maxSpp_, 0, 16384);
        const char* modes[] = {"BSDF sampling", "Light sampling (NEE)", "MIS (power)"};
        ImGui::Combo("Strategy", &s.misMode, modes, 3);
        ImGui::SliderFloat("Clamp", &s.clamp, 0.0f, 50.0f, s.clamp > 0 ? "%.1f" : "off");
        ImGui::SeparatorText("Display");
        ui::slider("Exposure (EV)", &exposure_, -4.0f, 4.0f);
        const char* tm[] = {"ACES", "Filmic (Hable)", "Clamp"};
        ImGui::Combo("Tone curve", &tonemap_, tm, 3);
        ImGui::SeparatorText("Camera");
        ui::slider("Field of view", &s.fovDeg, 15.0f, 90.0f, "%.0f deg");
        ui::slider("Aperture", &s.aperture, 0.0f, 0.3f, "%.3f");
        bool af = s.autoFocus != 0;
        if (ImGui::Checkbox("Focus on orbit target", &af)) s.autoFocus = af;
        if (!af) ui::slider("Focus distance", &s.focusDist, 0.5f, 30.0f);
        ImGui::SeparatorText("Lighting");
        ui::slider("Light intensity", &s.lightIntensity, 0.0f, 60.0f);
        ImGui::ColorEdit3("Light colour", s.lightColor);
        ui::slider("Light size", &s.lightSize, 0.1f, 5.0f);
        ui::slider("Light height", &s.lightHeight, 1.5f, 10.0f);
        bool sky = s.skyOn != 0;
        if (ImGui::Checkbox("Sky", &sky)) s.skyOn = sky;
        if (sky) { ImGui::SameLine(); ImGui::SetNextItemWidth(-1); ui::slider("##sky", &s.skyIntensity, 0.0f, 3.0f); }
        ImGui::PopItemWidth();
        ImGui::End();
    }

    void materialEditor(GpuMaterial& m) {
        const char* types[] = {"Diffuse (Lambert)", "Metal (GGX)", "Glass (dielectric)"};
        int type = (int)m.type;
        if (ImGui::Combo("Type", &type, types, 3)) m.type = (float)type;
        ImGui::ColorEdit3(type == 2 ? "Tint" : "Colour", m.albedo);
        if (type == 1) ui::slider("Roughness", &m.roughness, 0.0f, 1.0f);
        if (type == 2) ui::slider("IOR", &m.ior, 1.0f, 2.5f);
        // Emission as colour * strength so the colour picker stays in [0, 1].
        float strength = std::max({m.emission[0], m.emission[1], m.emission[2]});
        float col[3] = {1, 1, 1};
        if (strength > 0) for (int i = 0; i < 3; ++i) col[i] = m.emission[i] / strength;
        bool e = ui::slider("Emission", &strength, 0.0f, 20.0f);
        e |= ImGui::ColorEdit3("Emission colour", col);
        if (e) for (int i = 0; i < 3; ++i) m.emission[i] = col[i] * strength;
    }

    void scenePanel() {
        Settings& s = settings_;
        ImGui::Begin("Scene");
        ImGui::PushItemWidth(-120);
        ImGui::TextUnformatted("Model (.obj)");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##obj", &objPath_, ImGuiInputTextFlags_EnterReturnsTrue)) loadModel(objPath_);
        ImGui::SetNextItemWidth(-90);
        ImGui::SliderInt("##segs", &teapotSegments_, 2, 64, "teapot %d segs");
        ImGui::SameLine();
        if (ImGui::Button("Rebuild")) setMesh(makeTeapot(teapotSegments_), "Utah teapot");
        ImGui::SeparatorText("Material");
        std::vector<std::string> names = {"Mesh", "Ground"};
        for (int i = 0; i < s.sphereCount; ++i) names.push_back("Sphere " + std::to_string(i + 1));
        if (ImGui::BeginCombo("Edit", names[editMat_].c_str())) {
            for (int i = 0; i < (int)names.size(); ++i)
                if (ImGui::Selectable(names[i].c_str(), editMat_ == i)) editMat_ = i;
            ImGui::EndCombo();
        }
        materialEditor(s.mats[editMat_]);
        if (editMat_ >= 2) {
            GpuSphere& sp = s.spheres[editMat_ - 2];
            ImGui::DragFloat3("Centre", sp.c, 0.02f);
            ImGui::DragFloat("Radius", &sp.r, 0.01f, 0.05f, 5.0f);
        }
        ImGui::PopItemWidth();
        ImGui::End();
    }

    void statsPanel() {
        ImGui::Begin("Stats");
        ImGui::Text("Resolution     %d x %d", accum_ ? accum_->width() : 0, accum_ ? accum_->height() : 0);
        ImGui::Text("Accumulated    %d spp", spp_);
        ImGui::Text("Throughput     %.1f M samples/s", samplesPerSec_ / 1e6);
        ImGui::Text("Frame          %.2f ms", 1000.0f / std::max(ImGui::GetIO().Framerate, 1e-3f));
        ImGui::Separator();
        ImGui::TextWrapped("%s", meshName_.c_str());
        ImGui::Text("Triangles      %zu", meshTris_);
        ImGui::Text("BVH nodes      %zu (depth %d)", bvhNodes_, bvhDepth_);
        ImGui::Text("BVH build      %.2f ms", bvhMs_);
        ImGui::Separator();
        ImGui::TextDisabled("Left-drag: orbit, wheel: zoom,\nmiddle-drag: pan");
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
        if (outRt_) {
            // Fixed render size: letterbox it into the panel instead of stretching.
            ImVec2 img = sz;
            if (fixedW_ > 0 && fixedH_ > 0) {
                float k = std::min(sz.x / fixedW_, sz.y / fixedH_);
                img = {fixedW_ * k, fixedH_ * k};
                ImGui::SetCursorPos({(sz.x - img.x) * 0.5f, (sz.y - img.y) * 0.5f});
            }
            ImGui::Image((ImTextureID)(intptr_t)outRt_->color().imguiId(), img, {0, 1}, {1, 0});
        }
        viewportHovered_ = ImGui::IsItemHovered();
        ImGuiIO& io = ImGui::GetIO();
        if (viewportHovered_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) camera_.rotate(io.MouseDelta.x, io.MouseDelta.y);
        if (viewportHovered_ && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            // Pan the orbit target in the camera plane.
            Vec3 fwd = normalize(camera_.target - camera_.position());
            Vec3 right = normalize(cross(fwd, {0, 1, 0})), up = cross(right, fwd);
            float k = camera_.distance * 0.0015f;
            camera_.target = camera_.target - right * (io.MouseDelta.x * k) + up * (io.MouseDelta.y * k);
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    std::string assets_, computePath_, objPath_, meshName_, renderShot_;
    long long computeStamp_ = 0;
    std::unique_ptr<rhi::Pipeline> trace_, display_;
    std::unique_ptr<rhi::Texture> accum_;                     // rgb = sum of samples, a = count
    std::unique_ptr<rhi::RenderTarget> outRt_;                // tonemapped RGBA8 shown in the viewport
    std::vector<std::unique_ptr<rhi::RenderTarget>> retired_; // replaced outputs, freed next frame
    std::unique_ptr<rhi::Buffer> nodeBuf_, triBuf_, sphereBuf_, matBuf_, counterBuf_;
    OrbitCamera camera_;
    Settings settings_, applied_;
    int strategy_ = -1, meshType_ = -1;
    bool noSky_ = false;
    bool hasMesh_ = false, layoutBuilt_ = false, viewportHovered_ = false, saveRequested_ = false;
    int spp_ = 0, samplesPerFrame_ = 1, maxSpp_ = 8192, targetSpp_ = 0, editMat_ = 0, teapotSegments_ = 16;
    int tonemap_ = 0;
    float exposure_ = 0.0f;
    uint32_t seed_ = 1u;
    int vpW_ = 960, vpH_ = 540, fixedW_ = 0, fixedH_ = 0;
    size_t meshTris_ = 0, bvhNodes_ = 0;
    int bvhDepth_ = 0;
    double bvhMs_ = 0, samplesPerSec_ = 0, samplesThisWindow_ = 0, windowStart_ = 0;
    // benchmark
    bool bench_ = false;
    double benchSeconds_ = 10.0, benchStart_ = -1.0, totalRays_ = 0;
};

int main(int argc, char** argv) {
    bool bench = false;
    for (int i = 1; i < argc; ++i) bench |= !std::strcmp(argv[i], "--bench");
    PathTracer app(bench);   // the benchmark runs without vsync so it is not capped at the refresh rate
    return app.run(argc, argv);
}
