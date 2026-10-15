#pragma once
#include <string>

struct GLFWwindow;

namespace prism {

struct AppConfig {
    std::string title = "Prismatic";
    int width = 1600, height = 900;
    bool vsync = true;
    bool hidden = false;           // render without showing the window (screenshots, CI)
};

// Frame-driven application base. Derive, override the hooks, call run().
class Application {
public:
    explicit Application(const AppConfig& cfg) : cfg_(cfg) {}
    virtual ~Application() = default;

    int run(int argc, char** argv);
    void quit() { quit_ = true; }
    GLFWwindow* window() const { return window_; }
    double time() const { return time_; }
    float deltaTime() const { return dt_; }
    unsigned long long frame() const { return frame_; }
    // Writes the default framebuffer to a PNG. Returns false on failure.
    bool saveScreenshot(const std::string& path) const;
    int framebufferWidth() const;
    int framebufferHeight() const;

protected:
    virtual bool onInit(int argc, char** argv) { (void)argc; (void)argv; return true; }
    virtual void onUpdate(float dt) { (void)dt; }
    virtual void onRender() {}
    virtual void onGui() {}
    virtual void onShutdown() {}
    virtual void onScroll(double dx, double dy) { (void)dx; (void)dy; }

    AppConfig cfg_;

private:
    friend void scrollThunk(GLFWwindow*, double, double);
    GLFWwindow* window_ = nullptr;
    bool quit_ = false;
    double time_ = 0.0;
    float dt_ = 0.0f;
    unsigned long long frame_ = 0;
};

} // namespace prism
