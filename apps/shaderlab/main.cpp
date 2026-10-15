// ShaderLab: write a Shadertoy-style fragment shader and watch it run on a 3D scene.
//
// Pipeline each frame:
//   scene (teapot / OBJ) --> offscreen color+depth --> your effect shader --> viewport panel
// The effect sees the scene as iChannel0, a user texture as iChannel1 and depth as iChannel2.
#include "core/Application.h"
#include "core/Assets.h"
#include "core/File.h"
#include "core/Log.h"
#include "gfx/Shader.h"
#include "gfx/ShaderToy.h"
#include "gfx/Texture.h"
#include "scene/Camera.h"
#include "scene/Mesh.h"
#include <GLFW/glfw3.h>
#include <stb_image_write.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cstring>
#include <map>

using namespace prism;
namespace fs = std::filesystem;

class ShaderLab : public Application {
public:
    ShaderLab() : Application(AppConfig{"Prismatic ShaderLab", 1600, 900, true, false}) {}

protected:
    bool onInit(int argc, char** argv) override {
        assets_ = assetsDir();
        std::string vs, fs_, gfs;
        if (!readFile(assets_ + "/shaders/scene.vert", vs) || !readFile(assets_ + "/shaders/scene.frag", fs_) ||
            !readFile(assets_ + "/shaders/grid.frag", gfs)) {
            Log::error("assets/shaders not found (set PRISM_ASSETS)");
            return false;
        }
        if (!scene_.compileGraphics(vs, fs_) || !grid_.compileGraphics(vs, gfs)) {
            Log::error("scene shader: " + scene_.error() + grid_.error());
            return false;
        }
        fullscreen_.compileGraphics(kShadertoyVertex, wrapShadertoy("void mainImage(out vec4 c,in vec2 p){c=vec4(1,0,1,1);}"));
        glCreateVertexArrays(1, &emptyVao_);

        teapot_.upload(makeTeapot(16));
        plane_.upload(makePlane(40.0f, -0.5f));
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
        // GL objects must die while the context is still alive.
        scene_.destroy(); grid_.destroy(); effect_.destroy(); fullscreen_.destroy();
        teapot_.destroy(); custom_.destroy(); plane_.destroy(); user_.destroy();
        sceneFbo_.destroy(); outFbo_.destroy();
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
        effectsPanel();
        editorPanel();
        paramsPanel();
        consolePanel();
        viewportPanel();
    }

    void onRender() override {
        int w = std::max(vpW_, 16), h = std::max(vpH_, 16);
        sceneFbo_.resize(w, h, GL_RGBA16F);
        outFbo_.resize(w, h, GL_RGBA8);

        // 1. Scene pass
        sceneFbo_.bind();
        glEnable(GL_DEPTH_TEST);
        glClearColor(0.05f, 0.06f, 0.08f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        Mat4 vp = camera_.projection((float)w / h) * camera_.view();
        Vec3 cam = camera_.position();
        Mat4 model = rotateY(modelSpin_ ? (float)shaderTime_ * 0.4f : 0.0f);
        scene_.bind();
        scene_.set("uViewProj", vp);
        scene_.set("uCameraPos", cam);
        scene_.set("uTime", (float)shaderTime_);
        scene_.set("uBase", baseColor_[0], baseColor_[1], baseColor_[2]);
        scene_.set("uCheckerOn", checker_ ? 1 : 0);
        scene_.set("uHasAlbedo", 0);
        scene_.set("uModel", model);
        (useCustom_ ? custom_ : teapot_).draw();
        if (showGrid_) {
            grid_.bind();
            grid_.set("uViewProj", vp);
            grid_.set("uCameraPos", cam);
            grid_.set("uModel", Mat4{});
            plane_.draw();
        }

        // 2. Effect pass
        outFbo_.bind();
        glDisable(GL_DEPTH_TEST);
        glClear(GL_COLOR_BUFFER_BIT);
        Shader& fx = effect_.valid() ? effect_ : fullscreen_;
        fx.bind();
        glBindTextureUnit(0, sceneFbo_.colorTexture());
        user_.bind(1);
        glBindTextureUnit(2, sceneFbo_.depthTexture());
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
        glBindVertexArray(emptyVao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (!viewShot_.empty() && (int)frame() == viewShotFrame_) {
            // Only the effect's output, without the editor chrome: used for README images.
            auto px = outFbo_.readback();
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            stbi_flip_vertically_on_write(1);
            stbi_write_png(viewShot_.c_str(), w, h, 4, px.data(), w * 4);
            Log::info("view screenshot: " + viewShot_);
        }
        Framebuffer::unbind();
        glEnable(GL_DEPTH_TEST);
    }

private:
    // ---- UI ----
    // First run (no prismatic.ini yet): left = effects + parameters, centre = viewport,
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
        for (const auto& name : effects_) {
            bool sel = fs::path(current_).filename() == name;
            if (ImGui::Selectable(name.c_str(), sel)) selectEffect(name);
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Model (.obj)");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##obj", &objPath_, ImGuiInputTextFlags_EnterReturnsTrue)) loadModel(objPath_);
        ImGui::TextUnformatted("iChannel1 texture");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##tex", &texPath_, ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (!user_.loadFile(texPath_)) makeNoiseTexture();
        }
        ImGui::End();
    }

    void editorPanel() {
        ImGui::Begin("Shader");
        ImGui::Text("%s%s", fs::path(current_).filename().string().c_str(), dirty_ ? "  *" : "");
        ImGui::SameLine();
        if (ImGui::SmallButton("Save")) saveEffect();
        ImGui::SameLine();
        ImGui::Checkbox("Pause", &paused_);
        ImGui::SameLine(); ImGui::SetNextItemWidth(90);
        ImGui::SliderFloat("speed", &timeScale_, 0.0f, 3.0f);
        ImVec2 sz = ImGui::GetContentRegionAvail();
        sz.y -= 70;
        if (ImGui::InputTextMultiline("##src", &source_, {sz.x, sz.y}, ImGuiInputTextFlags_AllowTabInput)) {
            dirty_ = true;
            lastEdit_ = time();
        }
        if (!error_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, {1, 0.4f, 0.4f, 1});
            ImGui::TextWrapped("%s", error_.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextColored({0.4f, 1, 0.5f, 1}, "compiled");
        }
        ImGui::End();
    }

    void paramsPanel() {
        ImGui::Begin("Parameters");
        ImGui::Text("iTime %.2f   iFrame %llu", shaderTime_, frame());
        ImGui::Text("iMouse %.0f %.0f  (right-drag in viewport)", mouse_[0], mouse_[1]);
        ImGui::Separator();
        if (params_.empty()) ImGui::TextDisabled("Declare `uniform float uX = 0.5; // @range 0 1`\nto get a slider here.");
        for (auto& p : params_) {
            switch (p.kind) {
                case UniformParam::Float: ImGui::SliderFloat(p.name.c_str(), &p.value[0], p.lo, p.hi); break;
                case UniformParam::Int: {
                    int v = (int)p.value[0];
                    if (ImGui::SliderInt(p.name.c_str(), &v, (int)p.lo, (int)p.hi)) p.value[0] = (float)v;
                    break;
                }
                case UniformParam::Bool: {
                    bool b = p.value[0] > 0.5f;
                    if (ImGui::Checkbox(p.name.c_str(), &b)) p.value[0] = b ? 1.0f : 0.0f;
                    break;
                }
                case UniformParam::Color: ImGui::ColorEdit3(p.name.c_str(), p.value); break;
            }
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
        ImVec2 origin = ImGui::GetCursorScreenPos();
        if (outFbo_.colorTexture())
            ImGui::Image((ImTextureID)(intptr_t)outFbo_.colorTexture(), sz, {0, 1}, {1, 0});
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

    void compileEffect() {
        Shader next;
        if (next.compileGraphics(kShadertoyVertex, wrapShadertoy(source_))) {
            // Keep slider values the user already moved when the same uniform still exists.
            auto fresh = parseUniformParams(source_);
            for (auto& p : fresh)
                for (const auto& old : params_)
                    if (old.name == p.name && old.kind == p.kind) { for (int i = 0; i < 3; ++i) p.value[i] = old.value[i]; }
            params_ = fresh;
            effect_.compileGraphics(kShadertoyVertex, wrapShadertoy(source_));
            error_.clear();
        } else {
            error_ = next.error();  // previous program stays active
        }
    }

    // ---- scene ----
    void loadModel(const std::string& path) {
        MeshData m; std::string err;
        if (!loadObj(path, m, &err)) { Log::warn("OBJ: " + err); return; }
        m.normalizeToUnitBox();
        custom_.upload(m);
        useCustom_ = true;
        Log::info("loaded " + path + ": " + std::to_string(m.triangleCount()) + " triangles");
    }

    void makeNoiseTexture() {
        std::vector<unsigned char> px(64 * 64 * 4);
        unsigned s = 12345u;
        for (size_t i = 0; i < px.size(); i += 4) {
            s = s * 1664525u + 1013904223u;
            unsigned char v = (unsigned char)(s >> 24);
            px[i] = px[i + 1] = px[i + 2] = v; px[i + 3] = 255;
        }
        user_.upload(64, 64, 4, px.data());
    }

    std::string assets_;
    Shader scene_, grid_, effect_, fullscreen_;
    Mesh teapot_, custom_, plane_;
    Texture2D user_;
    Framebuffer sceneFbo_, outFbo_;
    GLuint emptyVao_ = 0;
    OrbitCamera camera_{{0, 0.0f, 0}, 0.6f, 0.3f, 3.1f};

    std::vector<std::string> effects_;
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
    ShaderLab app;
    return app.run(argc, argv);
}
