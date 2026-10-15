#include "core/Application.h"
#include "core/Log.h"
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <stb_image_write.h>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace prism {

void scrollThunk(GLFWwindow* w, double dx, double dy) {
    ImGui_ImplGlfw_ScrollCallback(w, dx, dy);
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(w));
    if (app && !ImGui::GetIO().WantCaptureMouse) app->onScroll(dx, dy);
}

static void glDebug(GLenum, GLenum type, GLuint, GLenum severity, GLsizei len, const char* msg, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    Log::write(type == GL_DEBUG_TYPE_ERROR ? "gl-error" : "gl", std::string(msg, len));
}

int Application::framebufferWidth() const { int w, h; glfwGetFramebufferSize(window_, &w, &h); return w; }
int Application::framebufferHeight() const { int w, h; glfwGetFramebufferSize(window_, &w, &h); return h; }

bool Application::saveScreenshot(const std::string& path) const {
    int w = framebufferWidth(), h = framebufferHeight();
    std::vector<unsigned char> px((size_t)w * h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    stbi_flip_vertically_on_write(1);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    return stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4) != 0;
}

int Application::run(int argc, char** argv) {
    // Common flags: --frames N (exit after N frames), --screenshot file.png (written on the last frame).
    int maxFrames = -1;
    std::string shot, platform;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!std::strcmp(argv[i], "--hidden")) cfg_.hidden = true;
        else if (!std::strcmp(argv[i], "--platform") && i + 1 < argc) platform = argv[++i];
    }
    if (!shot.empty() && maxFrames < 0) maxFrames = 5;

    // Window-system backend: by default GLFW picks Wayland when WAYLAND_DISPLAY is set, else X11.
    if (platform == "x11") glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    else if (platform == "wayland") glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
    glfwSetErrorCallback([](int code, const char* msg) { Log::error("glfw " + std::to_string(code) + ": " + msg); });
    if (!glfwInit()) { Log::error("glfwInit failed"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, cfg_.hidden ? GLFW_FALSE : GLFW_TRUE);
    window_ = glfwCreateWindow(cfg_.width, cfg_.height, cfg_.title.c_str(), nullptr, nullptr);
    if (!window_) { Log::error("could not create a 4.5 core OpenGL window"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(cfg_.vsync && shot.empty() ? 1 : 0);
    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) { Log::error("glad failed"); return 1; }
    glEnable(GL_DEBUG_OUTPUT);
    glDebugMessageCallback(glDebug, nullptr);
    {
        int p = glfwGetPlatform();
        Log::info(std::string("window system: ") + (p == GLFW_PLATFORM_WAYLAND ? "Wayland (native)" : p == GLFW_PLATFORM_X11 ? "X11" : "other"));
    }
    Log::info(std::string("GL ") + (const char*)glGetString(GL_VERSION) + " | " + (const char*)glGetString(GL_RENDERER));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "prismatic.ini";
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 4; st.FrameRounding = 3; st.TabRounding = 3; st.FramePadding = {8, 4};
    ImGui_ImplGlfw_InitForOpenGL(window_, false);
    ImGui_ImplOpenGL3_Init("#version 450");
    glfwSetWindowUserPointer(window_, this);
    glfwSetMouseButtonCallback(window_, ImGui_ImplGlfw_MouseButtonCallback);
    glfwSetCursorPosCallback(window_, ImGui_ImplGlfw_CursorPosCallback);
    glfwSetKeyCallback(window_, ImGui_ImplGlfw_KeyCallback);
    glfwSetCharCallback(window_, ImGui_ImplGlfw_CharCallback);
    glfwSetScrollCallback(window_, scrollThunk);

    int rc = 0;
    if (!onInit(argc, argv)) rc = 1;
    double last = glfwGetTime();
    while (rc == 0 && !quit_ && !glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        double now = glfwGetTime();
        dt_ = (float)(now - last); last = now; time_ = now;
        onUpdate(dt_);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        onGui();
        ImGui::Render();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, framebufferWidth(), framebufferHeight());
        glClearColor(0.07f, 0.07f, 0.09f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        onRender();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        ++frame_;
        bool last_frame = maxFrames >= 0 && (long long)frame_ >= maxFrames;
        if (last_frame && !shot.empty()) {
            if (saveScreenshot(shot)) Log::info("screenshot written: " + shot);
            else { Log::error("screenshot failed: " + shot); rc = 1; }
        }
        glfwSwapBuffers(window_);
        if (last_frame) break;
    }
    onShutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    glfwTerminate();
    return rc;
}

} // namespace prism
