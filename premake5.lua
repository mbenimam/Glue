-- Prismatic build script. Generate project files with:  premake5 gmake   (or vs2022, xcode4)
-- Then:  make -C build config=release -j
workspace "Prismatic"
    architecture "x86_64"
    configurations { "Debug", "Release" }
    startproject "shaderlab"
    location "build"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"
    targetdir "bin/%{cfg.buildcfg}"
    objdir "build/obj/%{cfg.buildcfg}/%{prj.name}"
    warnings "Extra"

    filter "configurations:Debug"
        defines { "PRISM_DEBUG" }
        symbols "On"
    filter "configurations:Release"
        optimize "Speed"
        symbols "Off"
    filter {}

project "glad"
    kind "StaticLib"
    language "C"
    warnings "Default"
    files { "third_party/glad/src/gl.c" }
    includedirs { "third_party/glad/include" }

project "glfw"
    kind "StaticLib"
    language "C"
    warnings "Off"
    files {
        "third_party/glfw/include/GLFW/*.h",
        "third_party/glfw/src/context.c", "third_party/glfw/src/init.c", "third_party/glfw/src/input.c",
        "third_party/glfw/src/monitor.c", "third_party/glfw/src/platform.c", "third_party/glfw/src/vulkan.c",
        "third_party/glfw/src/window.c", "third_party/glfw/src/egl_context.c", "third_party/glfw/src/osmesa_context.c",
        "third_party/glfw/src/null_init.c", "third_party/glfw/src/null_monitor.c",
        "third_party/glfw/src/null_window.c", "third_party/glfw/src/null_joystick.c",
    }
    includedirs { "third_party/glfw/include" }
    filter "system:linux"
        defines { "_GLFW_X11" }
        files {
            "third_party/glfw/src/posix_module.c", "third_party/glfw/src/posix_thread.c", "third_party/glfw/src/posix_time.c",
            "third_party/glfw/src/posix_poll.c", "third_party/glfw/src/linux_joystick.c", "third_party/glfw/src/xkb_unicode.c",
            "third_party/glfw/src/x11_init.c", "third_party/glfw/src/x11_monitor.c", "third_party/glfw/src/x11_window.c",
            "third_party/glfw/src/glx_context.c",
        }
        pic "On"
    filter "system:windows"
        defines { "_GLFW_WIN32", "_CRT_SECURE_NO_WARNINGS" }
        files {
            "third_party/glfw/src/win32_*.c", "third_party/glfw/src/wgl_context.c",
        }
    filter {}

project "imgui"
    kind "StaticLib"
    warnings "Off"
    files {
        "third_party/imgui/*.cpp",
        "third_party/imgui/backends/imgui_impl_glfw.cpp",
        "third_party/imgui/backends/imgui_impl_opengl3.cpp",
        "third_party/imgui/misc/cpp/imgui_stdlib.cpp",
    }
    includedirs { "third_party/imgui", "third_party/glfw/include" }

project "prism"
    kind "StaticLib"
    files { "engine/src/**.h", "engine/src/**.cpp" }
    includedirs { "engine/src", "third_party/glad/include", "third_party/glfw/include",
                  "third_party/imgui", "third_party/stb" }
    links { "glad", "glfw", "imgui" }

local function app(name, dir)
    project(name)
        kind "ConsoleApp"
        files { dir .. "/**.cpp", dir .. "/**.h" }
        includedirs { "engine/src", "third_party/glad/include", "third_party/glfw/include",
                      "third_party/imgui", "third_party/stb" }
        links { "prism", "imgui", "glfw", "glad" }
        filter "system:linux"
            links { "dl", "pthread", "X11" }
        filter "system:windows"
            links { "opengl32", "gdi32", "user32", "shell32" }
        filter {}
end

-- Every folder under apps/ is built as its own executable that links the engine.
for _, dir in ipairs(os.matchdirs("apps/*")) do
    app(path.getname(dir), dir)
end

-- The SPH app parallelises its CPU solver with OpenMP (GCC/Clang: -fopenmp at compile and link).
project "sph"
    filter "system:linux"
        buildoptions { "-fopenmp" }
        linkoptions { "-fopenmp" }
    filter {}

project "tests"
    kind "ConsoleApp"
    files { "tests/**.cpp" }
    includedirs { "engine/src", "third_party/stb" }
    links { "prism", "imgui", "glfw", "glad" }
    filter "system:linux"
        links { "dl", "pthread", "X11" }
    filter {}
