#include "gfx/ShaderToy.h"
#include <cstdlib>
#include <regex>
#include <sstream>
#include <cctype>

namespace glue {

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

EffectParts splitEffect(const std::string& src) {
    EffectParts r;
    bool inVertex = false;
    size_t pos = 0;
    while (pos <= src.size()) {
        size_t nl = src.find('\n', pos);
        const bool last = nl == std::string::npos;
        const std::string line = src.substr(pos, last ? std::string::npos : nl - pos);
        // marker: `//` then optional spaces then @vertex or @fragment
        size_t i = line.find_first_not_of(" \t");
        bool marker = false;
        if (i != std::string::npos && line.compare(i, 2, "//") == 0) {
            size_t j = line.find_first_not_of(" \t", i + 2);
            if (j != std::string::npos && line.compare(j, 7, "@vertex") == 0) { inVertex = true; marker = true; }
            else if (j != std::string::npos && line.compare(j, 9, "@fragment") == 0) { inVertex = false; marker = true; }
        }
        if (!marker) {                       // a marker line itself is blank in both stages
            (inVertex ? r.vertex : r.fragment) += line;
            if (inVertex && line.find_first_not_of(" \t\r") != std::string::npos) r.hasVertex = true;
        }
        if (last) break;
        r.fragment += '\n';
        r.vertex += '\n';
        pos = nl + 1;
    }
    return r;
}

std::string wrapSceneVertex(const std::string& tpl, const std::string& vertexUser) {
    std::string out = tpl;
    auto replace = [&](const std::string& key, const std::string& value) {
        size_t at = out.find(key);
        if (at != std::string::npos) out.replace(at, key.size(), value);
    };
    const bool has = vertexUser.find_first_not_of(" \t\r\n") != std::string::npos;
    replace("/*GLUE_USER_DECLS*/", has ? "#line 1\n" + vertexUser + "\n" : "");
    replace("/*GLUE_USER_CALL*/", has ? "mainVertex(p, n, aUv);" : "");
    return out;
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

} // namespace glue
