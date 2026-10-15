#include "gfx/ShaderToy.h"
#include <cstdlib>
#include <regex>
#include <sstream>
#include <cctype>

namespace prism {

const char* kShadertoyVertex = R"(#version 450 core
out vec2 vUv;
void main() {
    // One oversized triangle covers the screen; no vertex buffer needed.
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* kPrelude = R"(#version 450 core
in vec2 vUv;
out vec4 _fragColor;
uniform vec3  iResolution;
uniform float iTime;
uniform float iTimeDelta;
uniform int   iFrame;
uniform vec4  iMouse;
uniform vec3  iChannelResolution[4];
layout(binding = 0) uniform sampler2D iChannel0;
layout(binding = 1) uniform sampler2D iChannel1;
layout(binding = 2) uniform sampler2D iChannel2;
layout(binding = 3) uniform sampler2D iChannel3;
)";

static const char* kEpilogue = R"(
void main() {
    vec4 c = vec4(0.0, 0.0, 0.0, 1.0);
    mainImage(c, vUv * iResolution.xy);
    _fragColor = c;
}
)";

std::string wrapShadertoy(const std::string& user) {
    return std::string(kPrelude) + "#line 1\n" + user + kEpilogue;
}

static bool isBuiltin(const std::string& n) {
    return n.size() > 1 && n[0] == 'i' && std::isupper((unsigned char)n[1]);
}

std::vector<UniformParam> parseUniformParams(const std::string& src) {
    std::vector<UniformParam> out;
    static const std::regex re(
        R"(uniform\s+(float|int|bool|vec3)\s+(\w+)\s*(?:=\s*([^;]+))?;[ \t]*(?://[ \t]*(@range[ \t]+\S+[ \t]+\S+|@color))?)");
    for (auto it = std::sregex_iterator(src.begin(), src.end(), re); it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        UniformParam p;
        std::string type = m[1], init = m[3], hint = m[4];
        p.name = m[2];
        if (isBuiltin(p.name)) continue;
        if (type == "float") p.kind = UniformParam::Float;
        else if (type == "int") p.kind = UniformParam::Int;
        else if (type == "bool") p.kind = UniformParam::Bool;
        else p.kind = hint == "@color" ? UniformParam::Color : UniformParam::Float;
        if (type == "vec3" && hint != "@color") continue;  // plain vec3 has no sensible widget
        if (!init.empty()) {
            if (type == "vec3") {
                std::smatch v;
                static const std::regex vec(R"(vec3\(\s*([^,)]+)\s*,\s*([^,)]+)\s*,\s*([^,)]+)\s*\))");
                if (std::regex_search(init, v, vec))
                    for (int k = 0; k < 3; ++k) p.value[k] = std::strtof(std::string(v[k + 1]).c_str(), nullptr);
            } else if (type == "bool") {
                p.value[0] = init.find("true") != std::string::npos ? 1.0f : 0.0f;
            } else {
                p.value[0] = std::strtof(init.c_str(), nullptr);
            }
        }
        p.hi = p.kind == UniformParam::Int ? 10.0f : 1.0f;
        if (hint.rfind("@range", 0) == 0) {
            std::istringstream is(hint.substr(6));
            is >> p.lo >> p.hi;
        }
        out.push_back(p);
    }
    return out;
}

} // namespace prism
