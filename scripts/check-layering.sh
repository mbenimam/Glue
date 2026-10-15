#!/usr/bin/env bash
# Layering guard: only the OpenGL backend may touch OpenGL, only the GLFW window may touch GLFW.
# Run from the repository root. Exits 1 and lists offenders on a violation.
set -u
fail=0

check() {   # pattern  description  allowed-path-regex
    local pat="$1" what="$2" allow="$3"
    local hits
    hits=$(grep -rnE "$pat" engine apps tests --include='*.h' --include='*.cpp' --include='*.inc' \
           | grep -vE "$allow" || true)
    if [ -n "$hits" ]; then
        echo "layering violation: $what outside its backend:"
        echo "$hits" | sed 's/^/  /'
        fail=1
    fi
}

check '#include *[<"](glad|GL)/|\bgl[A-Z][A-Za-z0-9]*\(|\bGL_[A-Z0-9_]{3,}' \
      "OpenGL API use" '^engine/src/rhi/gl/|glMakeCurrent|glProcAddress|glSwapBuffers|glSetSwapInterval'
check '#include *[<"]GLFW/|\bglfw[A-Z][A-Za-z0-9]*\(|\bGLFW_[A-Z0-9_]+' \
      "GLFW API use" '^engine/src/platform/glfw/'
check '#include *[<"]backends/imgui_impl_(glfw|opengl3|vulkan|dx12)' \
      "ImGui backend include" '^engine/src/(rhi/gl|platform/glfw)/'

[ $fail -eq 0 ] && echo "layering ok"
exit $fail
