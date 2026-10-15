#pragma once
#include <memory>
#include <string>
#include "platform/Window.h"
#include "rhi/Rhi.h"

namespace glue {

struct AppConfig {
    std::string title = "Glue";
    int width = 1600, height = 900;
    bool vsync = true;
    bool hidden = false;                              // render without showing the window (screenshots, CI)
    rhi::Backend backend = rhi::Backend::OpenGL;      // graphics API; --backend gl|null selects it at run time
};

// Frame-driven application base. Derive, override the hooks, call run(). It owns a platform window
// and a render device and knows nothing about which window system or graphics API is behind them.
class Application {
public:
    explicit Application(const AppConfig& cfg) : cfg_(cfg) {}
    virtual ~Application() = default;

    int run(int argc, char** argv);
    void quit() { quit_ = true; }
    platform::Window& window() { return *window_; }
    rhi::Device& device() { return *device_; }
    rhi::CommandContext& context() { return device_->context(); }
    double time() const { return time_; }
    float deltaTime() const { return dt_; }
    unsigned long long frame() const { return frame_; }
    // Writes the window's back buffer to a PNG. Returns false on failure.
    bool saveScreenshot(const std::string& path);
    int framebufferWidth() const;
    int framebufferHeight() const;

protected:
    virtual bool onInit(int argc, char** argv) { (void)argc; (void)argv; return true; }
    virtual void onUpdate(float dt) { (void)dt; }
    // Offscreen work for this frame (scene passes, compute). Runs before the back buffer is drawn.
    virtual void onRender() {}
    virtual void onGui() {}
    virtual void onShutdown() {}
    virtual void onScroll(double dx, double dy) { (void)dx; (void)dy; }

    AppConfig cfg_;

private:
    std::unique_ptr<platform::Window> window_;
    std::unique_ptr<rhi::Device> device_;
    bool quit_ = false;
    double time_ = 0.0;
    float dt_ = 0.0f;
    unsigned long long frame_ = 0;
};

} // namespace glue
