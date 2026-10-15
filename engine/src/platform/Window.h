#pragma once
// Platform layer: a window, its event loop and its clock, behind one interface.
// The GLFW implementation lives in platform/glfw; a native Win32, Cocoa or Wayland window would be
// another class implementing Window, chosen in createWindow(), and nothing else would change.
#include <functional>
#include <memory>
#include <string>

namespace glue::platform {

// Which graphics API the window must be able to present for. GL needs a context made with the window;
// Vulkan and D3D12 windows carry no context and expose a native handle for surface creation instead.
enum class GraphicsApi { None, OpenGL, Vulkan, D3D12 };

struct WindowDesc {
    std::string title = "Glue";
    int width = 1600, height = 900;
    bool visible = true;
    GraphicsApi api = GraphicsApi::OpenGL;
    int glMajor = 4, glMinor = 5;           // used when api == OpenGL
    std::string preferredPlatform;          // "", "x11" or "wayland" (backends that have a choice)
};

class Window {
public:
    virtual ~Window() = default;

    virtual void pollEvents() = 0;
    virtual bool shouldClose() const = 0;
    virtual void requestClose() = 0;
    virtual void framebufferSize(int& width, int& height) const = 0;
    virtual double timeSeconds() const = 0;
    // Human-readable name of the window system in use, for logs ("Wayland (native)", "X11", ...).
    virtual std::string platformName() const = 0;

    // Mouse wheel, delivered after the GUI had its chance to take it.
    using ScrollCallback = std::function<void(double dx, double dy)>;
    virtual void setScrollCallback(ScrollCallback cb) = 0;

    // Dear ImGui platform half (input, display size, clipboard).
    virtual void imguiInit() = 0;
    virtual void imguiNewFrame() = 0;
    virtual void imguiShutdown() = 0;

    // ---- hooks used by graphics backends only ----
    virtual void* nativeWindowHandle() const = 0;                // HWND, wl_surface*, Window, GLFWwindow*...
    virtual void glMakeCurrent() = 0;
    virtual void* glProcAddress(const char* name) const = 0;
    virtual void glSwapBuffers() = 0;
    virtual void glSetSwapInterval(int interval) = 0;
};

// Creates a window; null and *error filled when the platform cannot provide one.
std::unique_ptr<Window> createWindow(const WindowDesc& desc, std::string* error);

} // namespace glue::platform
