// GLFW implementation of platform::Window. Everything GLFW-specific in the engine lives in this file.
#include "platform/Window.h"
#include "core/Log.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>

namespace glue::platform {
namespace {

int g_windowCount = 0;

class GlfwWindow final : public Window {
public:
    explicit GlfwWindow(GLFWwindow* w, GraphicsApi api) : win_(w), api_(api) {
        glfwSetWindowUserPointer(win_, this);
    }
    ~GlfwWindow() override {
        glfwDestroyWindow(win_);
        if (--g_windowCount == 0) glfwTerminate();
    }

    void pollEvents() override { glfwPollEvents(); }
    bool shouldClose() const override { return glfwWindowShouldClose(win_) != 0; }
    void requestClose() override { glfwSetWindowShouldClose(win_, GLFW_TRUE); }
    void framebufferSize(int& w, int& h) const override { glfwGetFramebufferSize(win_, &w, &h); }
    double timeSeconds() const override { return glfwGetTime(); }

    std::string platformName() const override {
        switch (glfwGetPlatform()) {
            case GLFW_PLATFORM_WAYLAND: return "Wayland (native)";
            case GLFW_PLATFORM_X11: return "X11";
            case GLFW_PLATFORM_WIN32: return "Win32";
            case GLFW_PLATFORM_COCOA: return "Cocoa";
            default: return "other";
        }
    }

    void setScrollCallback(ScrollCallback cb) override { scroll_ = std::move(cb); }

    void imguiInit() override {
        // Callbacks are installed by hand (install_callbacks = false) so scroll can be chained.
        if (api_ == GraphicsApi::OpenGL) ImGui_ImplGlfw_InitForOpenGL(win_, false);
        else ImGui_ImplGlfw_InitForOther(win_, false);
        glfwSetMouseButtonCallback(win_, ImGui_ImplGlfw_MouseButtonCallback);
        glfwSetCursorPosCallback(win_, ImGui_ImplGlfw_CursorPosCallback);
        glfwSetKeyCallback(win_, ImGui_ImplGlfw_KeyCallback);
        glfwSetCharCallback(win_, ImGui_ImplGlfw_CharCallback);
        glfwSetScrollCallback(win_, [](GLFWwindow* w, double dx, double dy) {
            ImGui_ImplGlfw_ScrollCallback(w, dx, dy);
            auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(w));
            if (self && self->scroll_ && !ImGui::GetIO().WantCaptureMouse) self->scroll_(dx, dy);
        });
    }
    void imguiNewFrame() override { ImGui_ImplGlfw_NewFrame(); }
    void imguiShutdown() override { ImGui_ImplGlfw_Shutdown(); }

    void* nativeWindowHandle() const override { return win_; }
    void glMakeCurrent() override { glfwMakeContextCurrent(win_); }
    void* glProcAddress(const char* name) const override { return (void*)glfwGetProcAddress(name); }
    void glSwapBuffers() override { glfwSwapBuffers(win_); }
    void glSetSwapInterval(int i) override { glfwSwapInterval(i); }

private:
    GLFWwindow* win_;
    GraphicsApi api_;
    ScrollCallback scroll_;
};

} // namespace

std::unique_ptr<Window> createWindow(const WindowDesc& d, std::string* error) {
    if (g_windowCount == 0) {
        if (d.preferredPlatform == "x11") glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
        else if (d.preferredPlatform == "wayland") glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
        glfwSetErrorCallback([](int code, const char* msg) { Log::error("glfw " + std::to_string(code) + ": " + msg); });
        if (!glfwInit()) { if (error) *error = "glfwInit failed"; return nullptr; }
    }
    if (d.api == GraphicsApi::OpenGL) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, d.glMajor);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, d.glMinor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
    } else {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);   // Vulkan / D3D12 create their own surface
    }
    glfwWindowHint(GLFW_VISIBLE, d.visible ? GLFW_TRUE : GLFW_FALSE);
    GLFWwindow* w = glfwCreateWindow(d.width, d.height, d.title.c_str(), nullptr, nullptr);
    if (!w) {
        if (error) *error = "could not create a window for the requested graphics API";
        if (g_windowCount == 0) glfwTerminate();
        return nullptr;
    }
    ++g_windowCount;
    return std::make_unique<GlfwWindow>(w, d.api);
}

} // namespace glue::platform
