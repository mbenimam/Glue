// GlueLab: write a Shadertoy-style fragment shader and watch it run on a 3D scene.
//
// Pipeline each frame:
//   scene (teapot / OBJ) --> offscreen color+depth --> your effect shader --> viewport panel
// The effect sees the scene as iChannel0, a user texture as iChannel1 and depth as iChannel2.
#include "core/Application.h"
#include "core/Assets.h"
#include "core/File.h"
#include "core/Log.h"
#include "gfx/ImageIO.h"
#include "gfx/ShaderToy.h"
#include "rhi/Rhi.h"
#include "scene/Camera.h"
#include "scene/Mesh.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include "ui/GlslEditor.h"
#include "ui/Theme.h"
#include <algorithm>
#include <cstring>
#include <map>

using namespace glue;
namespace fs = std::filesystem;

class GlueLab : public Application {
public:
    GlueLab() : Application(AppConfig{"GlueLab", 1600, 900, true, false}) {}

protected:
    bool onInit(int argc, char** argv) override {
        assets_ = assetsDir();
        std::string vs, fs_, gfs;
        if (!readFile(assets_ + "/shaders/scene.vert", vs) || !readFile(assets_ + "/shaders/scene.frag", fs_) ||
            !readFile(assets_ + "/shaders/grid.frag", gfs)) {
            Log::error("assets/shaders not found (set GLUE_ASSETS)");
            return false;
        }
        sceneVertexTemplate_ = vs;
        sceneFragmentSource_ = fs_;
        std::string err;
        if (!buildScenePipeline("", scene_, &err)) { Log::error("scene shader: " + err); return false; }
        rhi::GraphicsPipelineDesc sd;
        sd.vertexSource = vs;
        sd.vertexLayout = meshVertexLayout();
        sd.fragmentSource = gfs;
        sd.debugName = "grid";
        grid_ = device().createGraphicsPipeline(sd, &err);
        if (!grid_) { Log::error("grid shader: " + err); return false; }
        buildEffectPipeline("void mainImage(out vec4 c,in vec2 p){c=vec4(1,0,1,1);}", fallback_, nullptr);

        teapot_.upload(device(), makeTeapot(16));
        plane_.upload(device(), makePlane(40.0f, -0.5f));
        makeNoiseTexture();
        refreshEffectList();

        std::string effect = "glitch.frag";
        for (int i = 1; i + 1 < argc; ++i) {
            if (!std::strcmp(argv[i], "--effect")) effect = std::string(argv[i + 1]) + (std::string(argv[i + 1]).find('.') == std::string::npos ? ".frag" : "");
            if (!std::strcmp(argv[i], "--model")) loadModel(argv[i + 1]);
            if (!std::strcmp(argv[i], "--view-shot")) viewShot_ = argv[i + 1];
            if (!std::strcmp(argv[i], "--frames")) viewShotFrame_ = std::atoi(argv[i + 1]) - 1;
        }
        selectEffect(effect);
        return true;
    }

    void onUpdate(float dt) override {
        // Render targets follow the viewport panel size of the previous frame. They are recreated here,
        // before the GUI of this frame refers to their textures, never between onGui and the draw.
        int tw = std::max(vpW_, 16), th = std::max(vpH_, 16);
        if (!sceneRt_ || sceneRt_->width() != tw || sceneRt_->height() != th) {
            rhi::RenderTargetDesc d;
            d.width = tw; d.height = th; d.colorFormat = rhi::Format::RGBA16F; d.depth = true; d.debugName = "scene";
            sceneRt_ = device().createRenderTarget(d);
            d.colorFormat = rhi::Format::RGBA8; d.depth = false; d.debugName = "effect output";
            outRt_ = device().createRenderTarget(d);
        }
        // Hot reload: re-read the file when it changes on disk and the editor has no unsaved edits.
        if (!current_.empty() && !dirty_ && (frame() % 30 == 0)) {
            long long s = fileStamp(current_);
            if (s && s != stamp_) { loadEffectFile(current_); }
        }
        if (dirty_ && time() - lastEdit_ > 0.35) { compileEffect(); dirty_ = false; }
        if (!paused_) shaderTime_ += dt * timeScale_;
        (void)dt;
    }

    void onShutdown() override {
        // GPU objects must die while the device is still alive.
        scene_.reset(); grid_.reset(); effect_.reset(); fallback_.reset();
        teapot_.release(); custom_.release(); plane_.release(); user_.reset();
        sceneRt_.reset(); outRt_.reset();
    }

    void onScroll(double, double dy) override {
        if (viewportHovered_) camera_.zoom((float)dy);
    }

    void onGui() override {
        ImGuiID dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (!layoutBuilt_) { layoutBuilt_ = true; buildDefaultLayout(dock); }
        mainMenu();
        effectsPanel();
        editorPanel();
        paramsPanel();
        consolePanel();
        viewportPanel();
    }

    void onRender() override {
        int w = sceneRt_->width(), h = sceneRt_->height();
        rhi::CommandContext& ctx = context();

        // 1. Scene pass
        rhi::RenderPassDesc scenePass;
        scenePass.clearColorValue[0] = 0.05f; scenePass.clearColorValue[1] = 0.06f; scenePass.clearColorValue[2] = 0.08f;
        ctx.beginRenderPass(sceneRt_.get(), scenePass);
        Mat4 vp = camera_.projection((float)w / h) * camera_.view();
        Vec3 cam = camera_.position();
        Mat4 model = rotateY(modelSpin_ ? (float)shaderTime_ * 0.4f : 0.0f);
        ctx.bindPipeline(*scene_);
        scene_->set("uViewProj", vp);
        scene_->set("uCameraPos", cam);
        scene_->set("uTime", (float)shaderTime_);
        scene_->set("uBase", baseColor_[0], baseColor_[1], baseColor_[2]);
        scene_->set("uCheckerOn", checker_ ? 1 : 0);
        scene_->set("uHasAlbedo", 0);
        scene_->set("uModel", model);
        scene_->set("iTime", (float)shaderTime_);
        for (const auto& p : params_) {            // uniforms declared in the effect's @vertex part
            switch (p.kind) {
                case UniformParam::Float: scene_->set(p.name.c_str(), p.value[0]); break;
                case UniformParam::Int: scene_->set(p.name.c_str(), (int)p.value[0]); break;
                case UniformParam::Bool: scene_->set(p.name.c_str(), p.value[0] > 0.5f ? 1 : 0); break;
                case UniformParam::Color: scene_->set(p.name.c_str(), p.value[0], p.value[1], p.value[2]); break;
            }
        }
        (useCustom_ ? custom_ : teapot_).draw(ctx);
        if (showGrid_) {
            ctx.bindPipeline(*grid_);
            grid_->set("uViewProj", vp);
            grid_->set("uCameraPos", cam);
            grid_->set("uModel", Mat4{});
            plane_.draw(ctx);
        }
        ctx.endRenderPass();

        // 2. Effect pass: one full-screen triangle, the scene colour as iChannel0 and its depth as iChannel2
        rhi::RenderPassDesc fxPass;
        fxPass.clearDepth = false;
        ctx.beginRenderPass(outRt_.get(), fxPass);
        rhi::Pipeline& fx = effect_ ? *effect_ : *fallback_;
        ctx.bindPipeline(fx);
        ctx.bindTexture(0, sceneRt_->color());
        ctx.bindTexture(1, *user_);
        ctx.bindTexture(2, *sceneRt_->depth());
        fx.set("iResolution", (float)w, (float)h, 1.0f);
        fx.set("iTime", (float)shaderTime_);
        fx.set("iTimeDelta", deltaTime());
        fx.set("iFrame", (int)frame());
        fx.set("iMouse", mouse_[0], mouse_[1], mouse_[2], mouse_[3]);
        for (const auto& p : params_) {
            switch (p.kind) {
                case UniformParam::Float: fx.set(p.name.c_str(), p.value[0]); break;
                case UniformParam::Int: fx.set(p.name.c_str(), (int)p.value[0]); break;
                case UniformParam::Bool: fx.set(p.name.c_str(), p.value[0] > 0.5f ? 1 : 0); break;
                case UniformParam::Color: fx.set(p.name.c_str(), p.value[0], p.value[1], p.value[2]); break;
            }
        }
        ctx.draw(3);
        if (!viewShot_.empty() && (int)frame() == viewShotFrame_) {
            // Only the effect's output, without the editor chrome: used for README images.
            gfx::savePngRgba8(viewShot_, w, h, ctx.readColorRgba8(outRt_.get()));
            Log::info("view screenshot: " + viewShot_);
        }
        ctx.endRenderPass();
    }

private:
    // ---- UI ----
    // First run (no gluelab.ini yet): left = effects + parameters, centre = viewport,
    // right = shader editor + console.
    void buildDefaultLayout(ImGuiID root) {
        if (ImGui::DockBuilderGetNode(root) && ImGui::DockBuilderGetNode(root)->IsSplitNode()) return;  // restored from ini
        ImGui::DockBuilderRemoveNode(root);
        ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->Size);
        ImGuiID left, rest, right, center, leftBottom, rightBottom;
        left = ImGui::DockBuilderSplitNode(root, ImGuiDir_Left, 0.19f, nullptr, &rest);
        right = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.30f, nullptr, &center);
        leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.50f, nullptr, &left);
        rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.22f, nullptr, &right);
        ImGui::DockBuilderDockWindow("Effects", left);
        ImGui::DockBuilderDockWindow("Parameters", leftBottom);
        ImGui::DockBuilderDockWindow("Shader", right);
        ImGui::DockBuilderDockWindow("Console", rightBottom);
        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderFinish(root);
    }

    void mainMenu() {
        if (!ImGui::BeginMainMenuBar()) return;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save effect", "Ctrl+S", false, !current_.empty())) saveEffect();
            if (ImGui::MenuItem("Screenshot")) { shotRequested_ = true; }
            if (ImGui::MenuItem("Quit")) quit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scene")) {
            if (ImGui::MenuItem("Utah teapot", nullptr, !useCustom_)) useCustom_ = false;
            ImGui::MenuItem("Spin model", nullptr, &modelSpin_);
            ImGui::MenuItem("Object-space checker", nullptr, &checker_);
            ImGui::MenuItem("Ground grid", nullptr, &showGrid_);
            ImGui::ColorEdit3("Base colour", baseColor_);
            ImGui::EndMenu();
        }
        ImGui::Text("   %.1f fps", ImGui::GetIO().Framerate);
        ImGui::EndMainMenuBar();
    }

    void effectsPanel() {
        ImGui::Begin("Effects");
        ui::sectionHeader("Effects");
        for (const auto& name : effects_) {
            bool sel = fs::path(current_).filename() == name;
            if (ui::listRow(name.c_str(), sel)) selectEffect(name);
        }
        ui::sectionHeader("Model (.obj)");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##obj", &objPath_, ImGuiInputTextFlags_EnterReturnsTrue)) loadModel(objPath_);
        ui::sectionHeader("iChannel1 texture");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##tex", &texPath_, ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (!loadUserTexture(texPath_)) makeNoiseTexture();
        }
        ImGui::End();
    }

    void editorPanel() {
        ImGui::Begin("Shader");
        ImGui::PushFont(ui::fonts().strong);
        ImGui::TextUnformatted(fs::path(current_).filename().string().c_str());
        ImGui::PopFont();
        if (dirty_) { ImGui::SameLine(); ImGui::TextColored(ui::palette().warning, "unsaved"); }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 160.0f);
        ui::toggle("Pause", &paused_);
        ImGui::SameLine();
        if (ui::accentButton("Save")) saveEffect();
        ImGui::SetNextItemWidth(220);
        ui::slider("Speed", &timeScale_, 0.0f, 3.0f, "%.2fx");
        ImVec2 sz = ImGui::GetContentRegionAvail();
        sz.y -= 44;
        if (editor_.draw("##src", &source_, {sz.x, sz.y})) {
            dirty_ = true;
            lastEdit_ = time();
        }
        editor_.setErrorLines(error_.empty() ? std::vector<int>{} : ui::GlslEditor::parseErrorLines(error_));
        if (!error_.empty()) {
            ui::chip("error", ui::palette().danger);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ui::palette().danger);
            ImGui::PushFont(ui::fonts().code);
            ImGui::TextWrapped("%s", error_.c_str());
            ImGui::PopFont();
            ImGui::PopStyleColor();
        } else {
            ui::chip("compiled", ui::palette().success);
        }
        ImGui::End();
    }

    void paramsPanel() {
        ImGui::Begin("Parameters");
        ui::sectionHeader("Inputs");
        ImGui::PushFont(ui::fonts().code);
        ImGui::TextColored(ui::palette().textDim, "iTime  %.2f", shaderTime_);
        ImGui::TextColored(ui::palette().textDim, "iFrame %llu", frame());
        ImGui::TextColored(ui::palette().textDim, "iMouse %.0f %.0f", mouse_[0], mouse_[1]);
        ImGui::PopFont();
        ImGui::TextDisabled("right-drag in the viewport");
        ui::sectionHeader("Uniforms");
        if (params_.empty()) ImGui::TextDisabled("Declare `uniform float uX = 0.5; // @range 0 1`\nto get a slider here.");
        for (auto& p : params_) {
            switch (p.kind) {
                case UniformParam::Float: ui::slider(p.name.c_str(), &p.value[0], p.lo, p.hi); break;
                case UniformParam::Int: {
                    int v = (int)p.value[0];
                    if (ui::sliderInt(p.name.c_str(), &v, (int)p.lo, (int)p.hi)) p.value[0] = (float)v;
                    break;
                }
                case UniformParam::Bool: {
                    bool b = p.value[0] > 0.5f;
                    if (ui::toggle(p.name.c_str(), &b)) p.value[0] = b ? 1.0f : 0.0f;
                    break;
                }
                case UniformParam::Color: ImGui::ColorEdit3(p.name.c_str(), p.value); break;
            }
        }
        ImGui::End();
    }

    void consolePanel() {
        ImGui::Begin("Console");
        ImGui::PushFont(ui::fonts().code);
        ui::logLevelSelector();
        ImGui::Separator();
        ui::logLines();
        ImGui::PopFont();
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::End();
    }

    void viewportPanel() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("Viewport");
        ImVec2 sz = ImGui::GetContentRegionAvail();
        vpW_ = (int)sz.x; vpH_ = (int)sz.y;
        ImVec2 origin = ImGui::GetCursorScreenPos();
        if (outRt_)
            ImGui::Image((ImTextureID)(intptr_t)outRt_->color().imguiId(), sz, {0, 1}, {1, 0});
        viewportHovered_ = ImGui::IsItemHovered();
        ImGuiIO& io = ImGui::GetIO();
        if (viewportHovered_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) camera_.rotate(io.MouseDelta.x, io.MouseDelta.y);
        if (viewportHovered_ && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            float x = io.MousePos.x - origin.x, y = vpH_ - (io.MousePos.y - origin.y);
            if (mouse_[2] <= 0) { mouse_[2] = x; mouse_[3] = y; }
            mouse_[0] = x; mouse_[1] = y;
        } else {
            mouse_[2] = -std::abs(mouse_[2]);  // shadertoy: sign of z says whether the button is held
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // ---- effects ----
    void refreshEffectList() {
        effects_.clear();
        for (auto& e : fs::directory_iterator(assets_ + "/effects"))
            if (e.path().extension() == ".frag") effects_.push_back(e.path().filename().string());
        std::sort(effects_.begin(), effects_.end());
    }

    void selectEffect(const std::string& name) {
        std::string path = assets_ + "/effects/" + name;
        if (fs::exists(path)) loadEffectFile(path);
        else Log::warn("effect not found: " + name);
    }

    void loadEffectFile(const std::string& path) {
        if (!readFile(path, source_)) { Log::warn("cannot read " + path); return; }
        current_ = path;
        stamp_ = fileStamp(path);
        dirty_ = false;
        compileEffect();
    }

    void saveEffect() {
        if (current_.empty()) return;
        if (writeFile(current_, source_)) { stamp_ = fileStamp(current_); Log::info("saved " + current_); }
    }

    // The scene pass: scene.vert with the effect's optional `mainVertex` inserted (see splitEffect).
    bool buildScenePipeline(const std::string& userVertex, std::unique_ptr<rhi::Pipeline>& out, std::string* err) {
        rhi::GraphicsPipelineDesc d;
        d.vertexSource = wrapSceneVertex(sceneVertexTemplate_, userVertex);
        d.fragmentSource = sceneFragmentSource_;
        d.vertexLayout = meshVertexLayout();
        d.debugName = "scene";
        std::string e;
        auto p = device().createGraphicsPipeline(d, &e);
        if (!p) { if (err) *err = e; return false; }
        out = std::move(p);
        return true;
    }

    // Builds the full-screen effect pipeline for `userSource`. The same wrapper serves any backend.
    bool buildEffectPipeline(const std::string& userSource, std::unique_ptr<rhi::Pipeline>& out, std::string* err) {
        rhi::GraphicsPipelineDesc d;
        d.vertexSource = kShadertoyVertex;
        d.fragmentSource = wrapShadertoy(userSource);
        d.depth.test = false;
        d.depth.write = false;
        d.debugName = "effect";
        std::string e;
        auto p = device().createGraphicsPipeline(d, &e);
        if (!p) { if (err) *err = e; return false; }
        out = std::move(p);
        return true;
    }

    void compileEffect() {
        const EffectParts parts = splitEffect(source_);
        std::string e;
        std::unique_ptr<rhi::Pipeline> fx, scene;
        bool ok = true;
        if (!buildEffectPipeline(parts.fragment, fx, &e)) ok = false;
        else if (!buildScenePipeline(parts.vertex, scene, &e)) e = "vertex stage: " + e, ok = false;
        if (ok) {
            effect_ = std::move(fx);
            scene_ = std::move(scene);
            // Keep slider values the user already moved when the same uniform still exists.
            auto fresh = parseUniformParams(source_);
            for (auto& p : fresh)
                for (const auto& old : params_)
                    if (old.name == p.name && old.kind == p.kind) { for (int i = 0; i < 3; ++i) p.value[i] = old.value[i]; }
            params_ = fresh;
            error_.clear();
        } else {
            error_ = e;  // the previous pipelines stay active
        }
    }

    // ---- scene ----
    void loadModel(const std::string& path) {
        MeshData m; std::string err;
        if (!loadObj(path, m, &err)) { Log::warn("OBJ: " + err); return; }
        m.normalizeToUnitBox();
        custom_.upload(device(), m);
        useCustom_ = true;
        Log::info("loaded " + path + ": " + std::to_string(m.triangleCount()) + " triangles");
    }

    void setUserPixels(int w, int h, const uint8_t* px) {
        rhi::TextureDesc d;
        d.width = w; d.height = h; d.format = rhi::Format::RGBA8; d.mipmaps = true;
        d.initialData = px; d.debugName = "iChannel1";
        user_ = device().createTexture(d);
    }

    bool loadUserTexture(const std::string& path) {
        int w, h;
        std::vector<uint8_t> px;
        if (!gfx::loadImageRgba8(path, w, h, px)) { Log::warn("texture not loaded: " + path); return false; }
        setUserPixels(w, h, px.data());
        return true;
    }

    void makeNoiseTexture() {
        std::vector<uint8_t> px(64 * 64 * 4);
        unsigned s = 12345u;
        for (size_t i = 0; i < px.size(); i += 4) {
            s = s * 1664525u + 1013904223u;
            uint8_t v = (uint8_t)(s >> 24);
            px[i] = px[i + 1] = px[i + 2] = v; px[i + 3] = 255;
        }
        setUserPixels(64, 64, px.data());
    }

    std::string assets_;
    std::unique_ptr<rhi::Pipeline> scene_, grid_, effect_, fallback_;
    Mesh teapot_, custom_, plane_;
    std::unique_ptr<rhi::Texture> user_;
    std::unique_ptr<rhi::RenderTarget> sceneRt_, outRt_;
    OrbitCamera camera_{{0, 0.0f, 0}, 0.6f, 0.3f, 3.1f};

    std::vector<std::string> effects_;
    ui::GlslEditor editor_;
    std::string sceneVertexTemplate_, sceneFragmentSource_;
    std::string current_, source_, error_, objPath_, texPath_;
    std::vector<UniformParam> params_;
    long long stamp_ = 0;
    bool dirty_ = false, paused_ = false, useCustom_ = false, checker_ = true, showGrid_ = true, modelSpin_ = true;
    bool layoutBuilt_ = false, viewportHovered_ = false, shotRequested_ = false;
    double lastEdit_ = 0, shaderTime_ = 0;
    float timeScale_ = 1.0f;
    float baseColor_[3] = {0.95f, 0.55f, 0.25f};
    float mouse_[4] = {0, 0, 0, 0};
    std::string viewShot_;
    int viewShotFrame_ = -1;
    int vpW_ = 960, vpH_ = 540;
};

int main(int argc, char** argv) {
    GlueLab app;
    return app.run(argc, argv);
}
