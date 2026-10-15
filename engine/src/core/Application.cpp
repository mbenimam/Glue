#include "core/Application.h"
#include "core/Log.h"
#include "core/Assets.h"
#include "gfx/ImageIO.h"
#include "ui/Theme.h"
#include <imgui.h>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace glue {

int Application::framebufferWidth() const { int w, h; window_->framebufferSize(w, h); return w; }
int Application::framebufferHeight() const { int w, h; window_->framebufferSize(w, h); return h; }

bool Application::saveScreenshot(const std::string& path) {
    int w, h;
    window_->framebufferSize(w, h);
    std::vector<uint8_t> px = device_->context().readColorRgba8(nullptr);
    // A backend that cannot read back (the null device) returns a different size: refuse instead of
    // letting the PNG writer read past the end of the array.
    if (px.size() != (size_t)w * h * 4) return false;
    return gfx::savePngRgba8(path, w, h, std::move(px));
}

int Application::run(int argc, char** argv) {
    // Common flags: --frames N (exit after N frames), --screenshot file.png (written on the last frame),
    // --hidden, --platform x11|wayland (window system), --backend gl|null (graphics API).
    int maxFrames = -1;
    std::string shot, platformName;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!std::strcmp(argv[i], "--hidden")) cfg_.hidden = true;
        else if (!std::strcmp(argv[i], "--platform") && i + 1 < argc) platformName = argv[++i];
        else if (!std::strcmp(argv[i], "--backend") && i + 1 < argc) {
            std::string b = argv[++i];
            if (b == "gl" || b == "opengl") cfg_.backend = rhi::Backend::OpenGL;
            else if (b == "vulkan") cfg_.backend = rhi::Backend::Vulkan;
            else if (b == "d3d12") cfg_.backend = rhi::Backend::D3D12;
            else if (b == "null") cfg_.backend = rhi::Backend::Null;
        }
    }
    if (!shot.empty() && maxFrames < 0) maxFrames = 5;

    std::string err;
    platform::WindowDesc wd;
    wd.title = cfg_.title;
    wd.width = cfg_.width;
    wd.height = cfg_.height;
    wd.visible = !cfg_.hidden;
    wd.preferredPlatform = platformName;
    wd.api = cfg_.backend == rhi::Backend::OpenGL ? platform::GraphicsApi::OpenGL
           : cfg_.backend == rhi::Backend::Vulkan ? platform::GraphicsApi::Vulkan
           : cfg_.backend == rhi::Backend::D3D12 ? platform::GraphicsApi::D3D12 : platform::GraphicsApi::None;
    window_ = platform::createWindow(wd, &err);
    if (!window_) { Log::error("window: " + err); return 1; }
    device_ = rhi::Device::create(cfg_.backend, window_.get(), &err);
    if (!device_) { Log::error("graphics device: " + err); return 1; }
    device_->setVsync(cfg_.vsync && shot.empty());
    const auto& info = device_->info();
    Log::info("window system: " + window_->platformName());
    Log::info(std::string(rhi::backendName(info.backend)) + " " + info.apiVersion + " | " + info.renderer);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "gluelab.ini";
    ui::install(assetsDir());
    window_->imguiInit();
    device_->imguiInit();
    window_->setScrollCallback([this](double dx, double dy) { onScroll(dx, dy); });

    int rc = 0;
    if (!onInit(argc, argv)) rc = 1;
    double last = window_->timeSeconds();
    while (rc == 0 && !quit_ && !window_->shouldClose()) {
        window_->pollEvents();
        double now = window_->timeSeconds();
        dt_ = (float)(now - last); last = now; time_ = now;
        onUpdate(dt_);
        device_->imguiNewFrame();
        window_->imguiNewFrame();
        ImGui::NewFrame();
        onGui();
        ImGui::Render();

        onRender();   // offscreen passes first, then the window's back buffer carries the GUI
        rhi::RenderPassDesc pass;
        pass.clearColorValue[0] = 0.07f; pass.clearColorValue[1] = 0.07f; pass.clearColorValue[2] = 0.09f;
        context().beginRenderPass(nullptr, pass);
        device_->imguiRender();
        ++frame_;
        bool lastFrame = maxFrames >= 0 && (long long)frame_ >= maxFrames;
        if (lastFrame && !shot.empty()) {
            if (saveScreenshot(shot)) Log::info("screenshot written: " + shot);
            else { Log::error("screenshot failed: " + shot); rc = 1; }
        }
        context().endRenderPass();
        device_->present();
        if (lastFrame) break;
    }
    onShutdown();
    device_->imguiShutdown();
    window_->imguiShutdown();
    ImGui::DestroyContext();
    device_.reset();     // GPU objects die before the window (and its context) does
    window_.reset();
    return rc;
}

} // namespace glue
