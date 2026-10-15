-- Glue build script. Generate project files with:  premake5 gmake   (or vs2022, xcode4)
-- Then:  make -C build config=release -j
workspace "Glue"
    architecture "x86_64"
    configurations { "Debug", "Release" }
    startproject "gluelab"
    location "build"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"
    targetdir "bin/%{cfg.buildcfg}"
    objdir "build/obj/%{cfg.buildcfg}/%{prj.name}"
    warnings "Extra"
    -- Third-party headers are not ours to fix: keep their warnings out of the build log.
    externalincludedirs { "third_party/stb", "third_party/imgui", "third_party/glfw/include", "third_party/glad/include" }

    filter "configurations:Debug"
        defines { "GLUE_DEBUG" }
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

-- GLFW's Wayland backend needs client headers and glue code generated from the protocol XML.
-- The XML files ship with GLFW (third_party/glfw/deps/wayland); wayland-scanner is the system tool.
local wlGen = "build/glfw-wayland"
local wlProtocols = { "wayland", "viewporter", "xdg-shell", "idle-inhibit-unstable-v1",
                      "pointer-constraints-unstable-v1", "relative-pointer-unstable-v1",
                      "fractional-scale-v1", "xdg-activation-v1", "xdg-decoration-unstable-v1" }
local wlEnabled = false
if os.target() == "linux" and os.getenv("GLUE_NO_WAYLAND") == nil then
    os.mkdir(wlGen)
    wlEnabled = true
    for _, name in ipairs(wlProtocols) do
        local xml = "third_party/glfw/deps/wayland/" .. name .. ".xml"
        local ok1 = os.execute("wayland-scanner client-header " .. xml .. " " .. wlGen .. "/" .. name .. "-client-protocol.h")
        local ok2 = os.execute("wayland-scanner private-code " .. xml .. " " .. wlGen .. "/" .. name .. "-client-protocol-code.h")
        if not (ok1 and ok2) then
            print("wayland-scanner failed for " .. name .. "; building X11 only")
            wlEnabled = false
            break
        end
    end
end

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
        if wlEnabled then
            defines { "_GLFW_WAYLAND" }
            files { "third_party/glfw/src/wl_init.c", "third_party/glfw/src/wl_monitor.c", "third_party/glfw/src/wl_window.c" }
            includedirs { wlGen }
        end
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

project "glue"
    kind "StaticLib"
    files { "engine/src/**.h", "engine/src/**.cpp" }
    includedirs { "engine/src", "third_party/glad/include", "third_party/glfw/include",
                  "third_party/imgui", "third_party/stb", "third_party/spdlog/include" }
    links { "glad", "glfw", "imgui" }

local function app(name, dir)
    project(name)
        kind "ConsoleApp"
        files { dir .. "/**.cpp", dir .. "/**.h" }
        includedirs { "engine/src", "third_party/glad/include", "third_party/glfw/include",
                      "third_party/imgui", "third_party/stb", "third_party/spdlog/include" }
        links { "glue", "imgui", "glfw", "glad" }
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
    links { "glue", "imgui", "glfw", "glad" }
    filter "system:linux"
        links { "dl", "pthread", "X11" }
    filter {}
