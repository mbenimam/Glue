#pragma once
#include <string>
#include <vector>

namespace glue {

// A user-declared uniform that the editor turns into a widget:
//   uniform float uAmount = 0.5; // @range 0 1
//   uniform vec3  uTint   = vec3(0.2, 1.0, 0.3); // @color
struct UniformParam {
    enum Kind { Float, Int, Bool, Color } kind = Float;
    std::string name;
    float value[3] = {0, 0, 0};
    float lo = 0.0f, hi = 1.0f;
};

// Finds every `uniform float|int|bool|vec3` the user declared (the built-in i* names are skipped).
std::vector<UniformParam> parseUniformParams(const std::string& userSource);

// Wraps a Shadertoy-style `mainImage(out vec4, in vec2)` into a full #version 450 fragment shader.
// Built-ins: iResolution, iTime, iTimeDelta, iFrame, iMouse, iChannel0..3, iChannelResolution.
// `#line 1` makes driver error lines refer to the user's own text.
std::string wrapShadertoy(const std::string& userSource);
// Number of lines the prelude adds before `#line 1` (for tests and tooling).
extern const char* kShadertoyVertex;

} // namespace glue
