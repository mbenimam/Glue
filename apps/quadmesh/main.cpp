// QuadMesh: turn a triangle mesh into quads and simplify it, uniformly or adaptively, next to
// a triangle QEM baseline. Everything here is display and bookkeeping; the algorithms live in
// engine/src/geometry (HalfEdgeMesh, QuadMesh, QuadSimplify, Differential).
//
//   model --> weld --> half-edge triangles M --> reference surface (grid) + fitmaps + curvature
//         --> tri->quad --> quad simplification (uniform | adaptive)   vs   triangle QEM
//
// Flags (besides --hidden / --frames / --screenshot from Application):
//   --bench [model]         print the benchmark tables (docs/quadmesh-benchmarks.md) and exit
//   --model teapot|bumpy|box|file.obj   --segments N   --target N
//   --run quads|uniform|adaptive|baseline   (may repeat; applied in order at start-up)
//   --view shaded|curvature|fitmap|error   --no-wire   --camera yaw pitch distance
//   --compare [--left quads|uniform|adaptive|baseline]   show that method's result on the left
#include "core/Application.h"
#include "core/Log.h"
#include "geometry/Differential.h"
#include "geometry/HalfEdgeMesh.h"
#include "geometry/QuadMesh.h"
#include "geometry/QuadSimplify.h"
#include "rhi/Rhi.h"
#include "scene/Camera.h"
#include "scene/MeshData.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace glue;
using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ---------------------------------------------------------------- models and pipeline

// Unit sphere with three bumps of different sharpness: flat-ish areas next to curved ones,
// the easy case to see adaptivity.
static MeshData makeBumpySphere() {
    MeshData m = makeIcosphere(5, 1.0f);
    const Vec3 c[3] = {normalize(Vec3{1, 1, 0.3f}), normalize(Vec3{-1, 0.2f, 0.6f}), normalize(Vec3{0.1f, -1, -0.8f})};
    const float amp[3] = {0.35f, 0.2f, 0.12f}, width[3] = {0.25f, 0.12f, 0.06f};
    for (auto& p : m.positions) {
        Vec3 n = normalize(p);
        float r = 1.0f;
        for (int i = 0; i < 3; ++i) {
            Vec3 d = n - c[i];
            r += amp[i] * std::exp(-dot(d, d) / (width[i] * width[i]));
        }
        p = n * r;
    }
    m.normalizeToUnitBox();
    m.computeNormals();
    return m;
}

// Box with rounded edges: six flat faces, cylindrical edges and spherical corners. Built as a
// triangulated cube grid pushed onto the rounded surface (closest point of the inner box,
// plus `radius` along the direction to the cube point).
static MeshData makeRoundedBox(int n = 48, float radius = 0.3f) {
    MeshData m;
    const float h = 1.0f - radius;
    for (int axis = 0; axis < 3; ++axis)
        for (int side = 0; side < 2; ++side) {
            uint32_t base = (uint32_t)m.positions.size();
            for (int i = 0; i <= n; ++i)
                for (int j = 0; j <= n; ++j) {
                    float q[3];
                    q[axis] = side ? 1.0f : -1.0f;
                    q[(axis + 1) % 3] = -1.0f + 2.0f * i / n;
                    q[(axis + 2) % 3] = -1.0f + 2.0f * j / n;
                    Vec3 c{std::clamp(q[0], -h, h), std::clamp(q[1], -h, h), std::clamp(q[2], -h, h)};
                    m.positions.push_back(c + normalize(Vec3{q[0], q[1], q[2]} - c) * radius);
                }
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    uint32_t a = base + i * (n + 1) + j, b = a + 1, c = a + n + 1, d = c + 1;
                    if ((i + j) % 2) m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
                    else m.indices.insert(m.indices.end(), {a, c, d, a, d, b});
                }
        }
    weldVertices(m);
    if (m.signedVolume() < 0) m.flipWinding();
    m.normalizeToUnitBox();
    m.computeNormals();
    return m;
}

struct Model {
    std::string name;
    HalfEdgeMesh tri;
    ReferenceSurface ref;
    Fitmaps fit;
    Curvature curv;
    double prepMs = 0;
};

static std::unique_ptr<Model> loadModel(const std::string& which, int segments, std::string* err) {
    MeshData md;
    std::string name;
    if (which == "teapot") { md = makeTeapot(segments); name = "teapot/" + std::to_string(segments); }
    else if (which == "bumpy") { md = makeBumpySphere(); name = "bumpy sphere"; }
    else if (which == "box") { md = makeRoundedBox(); name = "rounded box"; }
    else {
        if (!loadObj(which, md, err)) return nullptr;
        weldVertices(md);
        md.normalizeToUnitBox();
        name = which;
    }
    auto m = std::make_unique<Model>();
    m->name = name;
    auto t0 = Clock::now();
    m->tri = HalfEdgeMesh::fromMeshData(md);
    m->tri.garbageCollect();
    m->ref.build(m->tri);
    m->fit = computeFitmaps(m->ref);
    m->curv = computeCurvature(m->tri);
    m->prepMs = msSince(t0);
    return m;
}

struct Result {
    std::string label;
    HalfEdgeMesh mesh;
    double ms = 0;
    QuadQualityStats q;
    SurfaceDistance d;
    double homeo = 0;
    std::vector<float> faceErr;
};

static void measure(Result& r, const Model& m) {
    r.q = measureQuads(r.mesh);
    r.d = surfaceDistance(r.mesh, m.ref, 40000);
    r.homeo = homeometryEnergy(r.mesh);
    r.faceErr = faceErrors(r.mesh, m.ref);
}

enum class Method { Quads, Uniform, Adaptive, Baseline };

static Result runMethod(const Model& m, Method how, int targetQuads, double* convertMs = nullptr) {
    Result r;
    auto t0 = Clock::now();
    r.mesh = m.tri;
    if (how == Method::Baseline) {
        // Same vertex budget: a quad is worth two triangles.
        simplifyTrianglesQEM(r.mesh, (size_t)targetQuads * 2);
        r.label = "triangle QEM";
    } else {
        QuadConvertStats cs = trianglesToQuads(r.mesh);
        if (convertMs) *convertMs = cs.ms;
        r.label = "tri -> quad";
        if (how != Method::Quads) {
            QuadSimplifyOptions o;
            o.targetQuads = targetQuads;
            o.adaptive = how == Method::Adaptive;
            simplifyQuadMesh(r.mesh, m.ref, &m.fit, o);
            r.label = o.adaptive ? "adaptive quads" : "uniform quads";
        }
    }
    r.ms = msSince(t0);
    r.mesh.garbageCollect();
    measure(r, m);
    return r;
}

// ---------------------------------------------------------------- benchmark

static int runBench(const char* only) {
    struct Case { const char* model; int segments; std::vector<int> targets; };
    const Case cases[] = {{"teapot", 8, {1000, 500, 250}}, {"teapot", 16, {2000, 1000, 500}}, {"bumpy", 0, {2000, 800, 300}},
                          {"box", 0, {2000, 800, 300}}};
    std::printf("| model | method | target | faces | quads %% | regular %% | quality (mean / min) | angle dev | homeometry | Hausdorff %% | mean dist %% | time ms |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    std::vector<std::string> prep;
    for (const auto& c : cases) {
        if (only && !std::strstr(c.model, only)) continue;
        std::string err;
        auto m = loadModel(c.model, c.segments, &err);
        if (!m) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        char line[256];
        auto row = [&](const Result& r, int target) {
            const double dg = r.d.diagonal;
            char quad[160] = "- | - | - | -";             // quad-only columns mean nothing for triangles
            if (r.q.quads)
                std::snprintf(quad, sizeof quad, "%.1f | %.3f / %.3f | %.1f | %.3f", r.q.percentRegular, r.q.meanQuality,
                              r.q.minQuality, r.q.meanAngleDeviation, r.homeo);
            std::printf("| %s | %s | %d | %zu | %.1f | %s | %.3f | %.4f | %.1f |\n", m->name.c_str(), r.label.c_str(), target,
                        r.q.faces, r.q.percentQuads, quad, 100.0 * r.d.hausdorff / dg, 100.0 * 0.5 * (r.d.meanAB + r.d.meanBA) / dg, r.ms);
            std::fflush(stdout);
        };
        double convertMs = 0;
        Result q = runMethod(*m, Method::Quads, 0, &convertMs);
        row(q, 0);
        for (int t : c.targets) {
            row(runMethod(*m, Method::Uniform, t), t);
            row(runMethod(*m, Method::Adaptive, t), t);
            row(runMethod(*m, Method::Baseline, t), t);
        }
        std::snprintf(line, sizeof line, "| %s | %zu | %zu | %.1f | %.1f | %.1f |", m->name.c_str(), m->tri.liveFaces(),
                      m->ref.positions.size(), convertMs, m->fit.ms, m->prepMs);
        prep.push_back(line);
    }
    std::printf("\n| model | triangles | vertices | tri->quad ms | fitmaps ms | load + reference + fitmaps + curvature ms |\n");
    std::printf("|---|---|---|---|---|---|\n");
    for (auto& l : prep) std::printf("%s\n", l.c_str());
    return 0;
}

// ---------------------------------------------------------------- GPU side

static const char* kVert = R"(#version 450 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
uniform mat4 uViewProj;
uniform mat4 uModel;
out vec3 vWorld;
out vec3 vNormal;
out vec3 vColor;
void main() {
    vec4 w = uModel * vec4(aPos, 1.0);
    vWorld = w.xyz;
    vNormal = mat3(uModel) * aNormal;
    vColor = aColor;
    gl_Position = uViewProj * w;
}
)";

static const char* kFrag = R"(#version 450 core
in vec3 vWorld;
in vec3 vNormal;
in vec3 vColor;
out vec4 oColor;
uniform vec3 uCameraPos;
uniform int uLit;
uniform vec3 uLineColor;
void main() {
    if (uLit == 0) { oColor = vec4(uLineColor, 1.0); return; }
    vec3 n = normalize(vNormal);
    vec3 v = normalize(uCameraPos - vWorld);
    if (dot(n, v) < 0.0) n = -n;                      // open meshes: light the inside too
    vec3 L1 = normalize(vec3(0.6, 0.8, 0.5)), L2 = normalize(vec3(-0.7, 0.2, -0.4));
    float d = max(dot(n, L1), 0.0) * 0.75 + max(dot(n, L2), 0.0) * 0.25 + max(dot(n, v), 0.0) * 0.25;
    vec3 h = normalize(L1 + v);
    float spec = pow(max(dot(n, h), 0.0), 48.0) * 0.18;
    oColor = vec4(vColor * (0.18 + 0.82 * d) + spec, 1.0);
}
)";

// Interleaved vertex of PolyMeshGpu: position (location 0), normal (1), colour (2), 9 floats.
static rhi::VertexLayout polyVertexLayout() {
    rhi::VertexLayout l;
    l.stride = 9 * sizeof(float);
    for (uint32_t i = 0; i < 3; ++i) l.attributes.push_back({i, rhi::VertexFormat::Float3, i * 12});
    return l;
}

// Flat-shaded polygons (fan-triangulated) plus a line buffer with the polygon edges only,
// so quads show as quads in the wireframe. The buffers are kept and refilled on each upload.
class PolyMeshGpu {
public:
    void release() { faces_.reset(); lines_.reset(); triVerts_ = lineVerts_ = 0; }
    // colors: per vertex slot (vertexColors) or per face slot (faceColors), one of them non-empty.
    void upload(rhi::Device& dev, const HalfEdgeMesh& m, const std::vector<Vec3>& vertexColors,
                const std::vector<Vec3>& faceColors) {
        std::vector<float> vb, lb;
        std::vector<int> fv;
        for (int f = 0; f < m.faceSlots(); ++f) {
            if (!m.faceAlive(f)) continue;
            m.faceVertices(f, fv);
            Vec3 n = normalize(m.faceVectorArea(f));
            auto put = [&](int v) {
                const Vec3& p = m.position(v);
                Vec3 c = faceColors.empty() ? vertexColors[v] : faceColors[f];
                vb.insert(vb.end(), {p.x, p.y, p.z, n.x, n.y, n.z, c.x, c.y, c.z});
            };
            for (size_t i = 1; i + 1 < fv.size(); ++i) { put(fv[0]); put(fv[i]); put(fv[i + 1]); }
        }
        for (int e = 0; e < m.edgeSlots(); ++e) {
            if (!m.edgeAlive(e)) continue;
            for (int v : {m.from(2 * e), m.from(2 * e + 1)}) {
                const Vec3& p = m.position(v);
                lb.insert(lb.end(), {p.x, p.y, p.z, 0, 0, 0, 0, 0, 0});
            }
        }
        triVerts_ = (uint32_t)(vb.size() / 9);
        lineVerts_ = (uint32_t)(lb.size() / 9);
        fill(dev, faces_, vb, "quadmesh faces");
        fill(dev, lines_, lb, "quadmesh edges");
    }
    // A pipeline created with polyVertexLayout() and the matching topology must be bound.
    void drawFaces(rhi::CommandContext& ctx) const { if (triVerts_) { ctx.bindVertexBuffer(*faces_); ctx.draw(triVerts_); } }
    void drawLines(rhi::CommandContext& ctx) const { if (lineVerts_) { ctx.bindVertexBuffer(*lines_); ctx.draw(lineVerts_); } }

private:
    static void fill(rhi::Device& dev, std::unique_ptr<rhi::Buffer>& buf, const std::vector<float>& data, const char* name) {
        if (data.empty()) return;
        size_t bytes = data.size() * sizeof(float);
        if (!buf) {
            rhi::BufferDesc d;
            d.size = bytes;
            d.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CpuWrite;
            d.initialData = data.data();
            d.debugName = name;
            buf = dev.createBuffer(d);
            return;
        }
        if (buf->size() != bytes) buf->resizeDiscard(bytes);
        buf->write(0, data.data(), bytes);
    }
    std::unique_ptr<rhi::Buffer> faces_, lines_;
    uint32_t triVerts_ = 0, lineVerts_ = 0;
};

// Blue -> cyan -> green -> yellow -> red, t in [0, 1].
static Vec3 heat(float t) {
    static const Vec3 stops[5] = {{0.13f, 0.20f, 0.55f}, {0.10f, 0.60f, 0.80f}, {0.35f, 0.78f, 0.35f},
                                  {0.98f, 0.80f, 0.20f}, {0.86f, 0.18f, 0.12f}};
    t = std::clamp(t, 0.0f, 1.0f) * 4.0f;
    int i = std::min((int)t, 3);
    float f = t - i;
    return stops[i] * (1 - f) + stops[i + 1] * f;
}

// ---------------------------------------------------------------- application

enum class View { Shaded, Curvature, Fitmap, Error };

class QuadMeshApp : public Application {
public:
    QuadMeshApp() : Application(AppConfig{"Glue QuadMesh", 1600, 900, true, false}) {}

protected:
    bool onInit(int argc, char** argv) override {
        // Same shaders twice: filled faces pushed back by a polygon offset, and the edges as lines on top.
        rhi::GraphicsPipelineDesc pd;
        pd.vertexSource = kVert;
        pd.fragmentSource = kFrag;
        pd.vertexLayout = polyVertexLayout();
        pd.polygonOffset = true;
        pd.polygonOffsetFactor = pd.polygonOffsetUnits = 1.0f;
        pd.debugName = "quadmesh faces";
        std::string err;
        faces_ = device().createGraphicsPipeline(pd, &err);
        if (!faces_) { Log::error("shader: " + err); return false; }
        pd.topology = rhi::Topology::Lines;
        pd.polygonOffset = false;
        pd.debugName = "quadmesh wireframe";
        lines_ = device().createGraphicsPipeline(pd, &err);
        if (!lines_) { Log::error("shader: " + err); return false; }
        std::string model = "teapot";
        std::vector<Method> runs;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            bool more = i + 1 < argc;
            if (a == "--model" && more) model = argv[++i];
            else if (a == "--segments" && more) segments_ = std::atoi(argv[++i]);
            else if (a == "--target" && more) target_ = std::atoi(argv[++i]);
            else if (a == "--compare") compare_ = true;
            else if (a == "--left" && more) compareWith_ = (int)methodFromName(argv[++i]);
            else if (a == "--no-wire") wire_ = false;
            else if (a == "--view" && more) {
                std::string v = argv[++i];
                view_ = v == "curvature" ? View::Curvature : v == "fitmap" ? View::Fitmap : v == "error" ? View::Error : View::Shaded;
            } else if (a == "--run" && more) {
                runs.push_back(methodFromName(argv[++i]));
            }
        }
        modelIndex_ = model == "teapot" ? 0 : model == "bumpy" ? 1 : model == "box" ? 2 : 3;
        if (modelIndex_ == 3) objPath_ = model;
        load(model);
        for (int i = 1; i + 3 < argc; ++i)
            if (!std::strcmp(argv[i], "--camera")) {
                camera_.yaw = (float)std::atof(argv[i + 1]);
                camera_.pitch = (float)std::atof(argv[i + 2]);
                camera_.distance = (float)std::atof(argv[i + 3]);
            }
        for (Method r : runs) apply(r);
        return true;
    }

    void onShutdown() override {
        // GPU objects must die while the device is still alive.
        faces_.reset(); lines_.reset(); viewRt_.reset();
        gpuMain_.release(); gpuSide_.release();
    }

    void onUpdate(float) override {
        // The render target follows the viewport panel size of the previous frame. It is recreated
        // here, before this frame's GUI refers to its texture, never between onGui and the draw.
        int w = std::max(vpW_, 16), h = std::max(vpH_, 16);
        if (!viewRt_ || viewRt_->width() != w || viewRt_->height() != h) {
            rhi::RenderTargetDesc d;
            d.width = w; d.height = h; d.colorFormat = rhi::Format::RGBA8; d.depth = true; d.debugName = "quadmesh view";
            viewRt_ = device().createRenderTarget(d);
        }
    }

    void onScroll(double, double dy) override { if (viewportHovered_) camera_.zoom((float)dy); }

    void onGui() override {
        ImGuiID dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (!layoutBuilt_) { layoutBuilt_ = true; buildDefaultLayout(dock); }
        modelPanel();
        simplifyPanel();
        displayPanel();
        statsPanel();
        viewportPanel();
        if (dirtyGpu_) { refreshGpu(); dirtyGpu_ = false; }
    }

    void onRender() override {
        int w = viewRt_->width(), h = viewRt_->height();
        rhi::CommandContext& ctx = context();
        rhi::RenderPassDesc pass;
        pass.clearColorValue[0] = 0.075f; pass.clearColorValue[1] = 0.08f; pass.clearColorValue[2] = 0.10f;
        ctx.beginRenderPass(viewRt_.get(), pass);
        Mat4 vp = camera_.projection((float)w / h) * camera_.view();
        Vec3 cam = camera_.position();
        faces_->set("uViewProj", vp);
        faces_->set("uCameraPos", cam);
        faces_->set("uLit", 1);
        lines_->set("uViewProj", vp);
        lines_->set("uCameraPos", cam);
        lines_->set("uLit", 0);
        lines_->set("uLineColor", 0.06f, 0.06f, 0.08f);
        const Result* side = leftResult();
        // Side by side along the screen's horizontal, whatever the orbit angle.
        Vec3 right = normalize(cross(camera_.target - cam, {0, 1, 0}));
        auto drawOne = [&](const PolyMeshGpu& g, float dx) {
            Mat4 model = translate(right * dx);
            ctx.bindPipeline(*faces_);
            faces_->set("uModel", model);
            g.drawFaces(ctx);
            if (wire_) {
                ctx.bindPipeline(*lines_);
                lines_->set("uModel", model);
                g.drawLines(ctx);
            }
        };
        if (shownResult()) drawOne(gpuMain_, side ? 1.3f : 0.0f);
        if (side) drawOne(gpuSide_, -1.3f);
        ctx.endRenderPass();
    }

private:
    // ---- pipeline ----
    void load(const std::string& which) {
        std::string err;
        auto m = loadModel(which, segments_, &err);
        if (!m) { Log::warn("load: " + err); return; }
        model_ = std::move(m);
        for (auto& r : results_) r.reset();
        shown_ = -1;
        input_ = std::make_unique<Result>();
        input_->label = "input triangles";
        input_->mesh = model_->tri;
        measure(*input_, *model_);
        // Slider range follows the model; default target about a quarter of the quads.
        int quads = (int)model_->tri.liveFaces() / 2;
        if (target_ <= 0 || target_ > quads) target_ = std::max(50, quads / 4);
        float sMax = 0;
        for (float s : model_->fit.scale) sMax = std::max(sMax, s);
        sScale_ = sMax > 0 ? sMax : 1.0f;
        std::vector<float> h;
        for (float x : model_->curv.mean) h.push_back(std::fabs(x));
        std::sort(h.begin(), h.end());
        // 99th percentile: a few sharp spots should saturate, not the whole smooth part.
        hScale_ = h.empty() ? 1.0f : std::max(h[h.size() * 99 / 100], 1e-6f);
        Log::info(model_->name + ": " + std::to_string(model_->tri.liveFaces()) + " triangles, prep " +
                  std::to_string((int)model_->prepMs) + " ms (fitmaps " + std::to_string((int)model_->fit.ms) + " ms)");
        camera_.distance = compare_ ? 6.2f : 3.6f;
        dirtyGpu_ = true;
    }

    void apply(Method how) {
        if (!model_) return;
        Result r = runMethod(*model_, how, target_);
        Log::info(r.label + ": " + std::to_string(r.q.faces) + " faces in " + std::to_string((int)r.ms) + " ms");
        results_[(int)how] = std::make_unique<Result>(std::move(r));
        // The triangle baseline goes to the left slot when a quad result is on screen.
        if (how == Method::Baseline && shown_ >= 0 && shown_ != (int)Method::Baseline) compareWith_ = (int)how;
        else shown_ = (int)how;
        dirtyGpu_ = true;
    }

    static Method methodFromName(const std::string& r) {
        return r == "quads" ? Method::Quads : r == "uniform" ? Method::Uniform : r == "baseline" ? Method::Baseline : Method::Adaptive;
    }
    const Result* shownResult() const { return shown_ >= 0 && results_[shown_] ? results_[shown_].get() : input_.get(); }
    const Result* leftResult() const {
        if (!compare_ || compareWith_ == shown_ || !results_[compareWith_]) return nullptr;
        return results_[compareWith_].get();
    }

    std::vector<Vec3> colorsFor(const Result& r, std::vector<Vec3>& faceColors) const {
        const HalfEdgeMesh& m = r.mesh;
        std::vector<Vec3> vc(m.vertexSlots(), Vec3{0.8f, 0.8f, 0.8f});
        faceColors.clear();
        if (view_ == View::Shaded) {
            faceColors.assign(m.faceSlots(), Vec3{});
            for (int f = 0; f < m.faceSlots(); ++f) {
                if (!m.faceAlive(f)) continue;
                int d = m.faceDegree(f);
                faceColors[f] = d == 4 ? Vec3{0.93f, 0.62f, 0.32f} : d == 3 ? Vec3{0.30f, 0.66f, 0.72f} : Vec3{0.85f, 0.2f, 0.3f};
            }
        } else if (view_ == View::Error) {
            faceColors.assign(m.faceSlots(), Vec3{});
            float scale = errScale_ * 0.01f * (float)r.d.diagonal;
            for (int f = 0; f < m.faceSlots(); ++f)
                if (m.faceAlive(f)) faceColors[f] = heat(r.faceErr[f] / scale);
        } else {
            // Fields of the reference surface, sampled at each vertex's closest point.
            for (int v = 0; v < m.vertexSlots(); ++v) {
                if (!m.vertexAlive(v)) continue;
                auto pr = model_->ref.project(m.position(v));
                float t = view_ == View::Fitmap ? model_->ref.sample(model_->fit.scale, pr) / sScale_
                                                : std::fabs(model_->ref.sample(model_->curv.mean, pr)) / hScale_;
                vc[v] = heat(t);
            }
        }
        return vc;
    }

    void refreshGpu() {
        std::vector<Vec3> fc;
        if (const Result* r = shownResult()) { auto vc = colorsFor(*r, fc); gpuMain_.upload(device(), r->mesh, vc, fc); }
        if (const Result* r = leftResult()) { auto vc = colorsFor(*r, fc); gpuSide_.upload(device(), r->mesh, vc, fc); }
    }

    // ---- UI ----
    // First run: left column = model / simplify / display, centre = viewport, right = stats.
    void buildDefaultLayout(ImGuiID root) {
        if (ImGui::DockBuilderGetNode(root) && ImGui::DockBuilderGetNode(root)->IsSplitNode()) return;
        ImGui::DockBuilderRemoveNode(root);
        ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->Size);
        ImGuiID left, rest, right, center, leftMid, leftBottom;
        left = ImGui::DockBuilderSplitNode(root, ImGuiDir_Left, 0.20f, nullptr, &rest);
        right = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.27f, nullptr, &center);
        leftMid = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.70f, nullptr, &left);
        leftBottom = ImGui::DockBuilderSplitNode(leftMid, ImGuiDir_Down, 0.42f, nullptr, &leftMid);
        ImGui::DockBuilderDockWindow("Model", left);
        ImGui::DockBuilderDockWindow("Simplify", leftMid);
        ImGui::DockBuilderDockWindow("Display", leftBottom);
        ImGui::DockBuilderDockWindow("Stats", right);
        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderFinish(root);
    }

    void modelPanel() {
        ImGui::Begin("Model");
        const char* names[] = {"Utah teapot", "Bumpy sphere", "Rounded box", "OBJ file"};
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##model", &modelIndex_, names, 4);
        if (modelIndex_ == 0) { ImGui::SetNextItemWidth(-80); ImGui::SliderInt("segments", &segments_, 4, 24); }
        if (modelIndex_ == 3) { ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##obj", "path/to/model.obj", &objPath_); }
        static const char* keys[] = {"teapot", "bumpy", "box"};
        if (ImGui::Button("Load", {-1, 0})) load(modelIndex_ < 3 ? keys[modelIndex_] : objPath_);
        if (model_) {
            ImGui::TextDisabled("%s", model_->name.c_str());
            ImGui::Text("%zu triangles, %zu vertices", model_->tri.liveFaces(), model_->ref.positions.size());
            ImGui::Text("prep %.0f ms (fitmaps %.0f ms)", model_->prepMs, model_->fit.ms);
        }
        ImGui::End();
    }

    void simplifyPanel() {
        ImGui::Begin("Simplify");
        int maxQ = model_ ? std::max(60, (int)model_->tri.liveFaces() / 2) : 1000;
        ImGui::TextUnformatted("Target quads");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##target", &target_, 50, maxQ, "%d", ImGuiSliderFlags_Logarithmic);
        ImGui::Spacing();
        if (ImGui::Button("Tri -> quad", {-1, 0})) apply(Method::Quads);
        if (ImGui::Button("Simplify: adaptive", {-1, 0})) apply(Method::Adaptive);
        if (ImGui::Button("Simplify: uniform", {-1, 0})) apply(Method::Uniform);
        if (ImGui::Button("Baseline: triangle QEM", {-1, 0})) apply(Method::Baseline);
        ImGui::TextDisabled("baseline gets 2x target triangles");
        ImGui::End();
    }

    void displayPanel() {
        ImGui::Begin("Display");
        int v = (int)view_;
        const char* views[] = {"Faces (quads / triangles)", "Mean curvature |H|", "S-fitmap", "Error to input"};
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##view", &v, views, 4)) { view_ = (View)v; dirtyGpu_ = true; }
        if (view_ == View::Error) {
            ImGui::SetNextItemWidth(-90);
            if (ImGui::SliderFloat("red at %diag", &errScale_, 0.05f, 2.0f, "%.2f")) dirtyGpu_ = true;
        }
        ImGui::Checkbox("Wireframe", &wire_);
        if (ImGui::Checkbox("Side by side", &compare_)) { camera_.distance = compare_ ? 6.2f : 3.6f; dirtyGpu_ = true; }
        if (compare_) {
            const char* methods[] = {"tri -> quad", "uniform quads", "adaptive quads", "triangle QEM"};
            ImGui::SetNextItemWidth(-60);
            if (ImGui::Combo("on left", &compareWith_, methods, 4)) dirtyGpu_ = true;
            ImGui::SetNextItemWidth(-60);
            int shown = std::max(shown_, 0);
            if (ImGui::Combo("on right", &shown, methods, 4) && results_[shown]) { shown_ = shown; dirtyGpu_ = true; }
            ImGui::TextDisabled("run a method once to fill its slot");
        }
        ImGui::End();
    }

    void statsRows(const Result& r) {
        auto row = [](const char* k, const char* fmt, auto... args) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn(); ImGui::Text(fmt, args...);
        };
        double dg = r.d.diagonal > 0 ? r.d.diagonal : 1;
        row("faces", "%zu", r.q.faces);
        row("quads / tris", "%zu / %zu", r.q.quads, r.q.triangles);
        row("quads", "%.1f %%", r.q.percentQuads);
        if (r.q.quads) {                                   // quad-only measures
            row("regular vertices", "%.1f %%", r.q.percentRegular);
            row("quad quality", "%.3f (min %.2f)", r.q.meanQuality, r.q.minQuality);
            row("angle deviation", "%.1f deg", r.q.meanAngleDeviation);
            row("homeometry", "%.3f", r.homeo);
        }
        row("Hausdorff", "%.3f %% diag", 100.0 * r.d.hausdorff / dg);
        row("mean distance", "%.4f %% diag", 100.0 * 0.5 * (r.d.meanAB + r.d.meanBA) / dg);
        row("time", "%.1f ms", r.ms);
    }

    void statsPanel() {
        ImGui::Begin("Stats");
        for (const Result* r : {shownResult(), leftResult()}) {
            if (!r) continue;
            ImGui::SeparatorText(r->label.c_str());
            if (ImGui::BeginTable(r->label.c_str(), 2, ImGuiTableFlags_SizingStretchProp)) {
                statsRows(*r);
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    void viewportPanel() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("Viewport");
        ImVec2 sz = ImGui::GetContentRegionAvail();
        vpW_ = (int)sz.x; vpH_ = (int)sz.y;
        ImVec2 origin = ImGui::GetCursorScreenPos();
        if (viewRt_) ImGui::Image((ImTextureID)(intptr_t)viewRt_->color().imguiId(), sz, {0, 1}, {1, 0});
        viewportHovered_ = ImGui::IsItemHovered();
        if (viewportHovered_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            camera_.rotate(ImGui::GetIO().MouseDelta.x, ImGui::GetIO().MouseDelta.y);
        // Captions over the meshes.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto caption = [&](const Result* r, float x) {
            if (!r) return;
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s  -  %zu faces", r->label.c_str(), r->q.faces);
            dl->AddText({origin.x + x, origin.y + 10}, IM_COL32(220, 220, 230, 255), buf);
        };
        if (leftResult()) { caption(leftResult(), 14); caption(shownResult(), sz.x * 0.5f + 14); }
        else caption(shownResult(), 14);
        ImGui::End();
        ImGui::PopStyleVar();
    }

    std::unique_ptr<rhi::Pipeline> faces_, lines_;
    std::unique_ptr<rhi::RenderTarget> viewRt_;
    PolyMeshGpu gpuMain_, gpuSide_;
    OrbitCamera camera_{{0, 0, 0}, 0.6f, 0.45f, 3.6f};
    std::unique_ptr<Model> model_;
    std::unique_ptr<Result> input_, results_[4];        // one slot per Method
    int shown_ = -1, compareWith_ = (int)Method::Baseline;
    View view_ = View::Shaded;
    int modelIndex_ = 0, segments_ = 16, target_ = 0;
    std::string objPath_;
    float errScale_ = 0.5f, sScale_ = 1.0f, hScale_ = 1.0f;
    bool wire_ = true, compare_ = false, dirtyGpu_ = false, layoutBuilt_ = false, viewportHovered_ = false;
    int vpW_ = 960, vpH_ = 540;
};

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--bench")) return runBench(i + 1 < argc && argv[i + 1][0] != '-' ? argv[i + 1] : nullptr);
    QuadMeshApp app;
    return app.run(argc, argv);
}
