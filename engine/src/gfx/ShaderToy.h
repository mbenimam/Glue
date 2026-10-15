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
// An effect file may hold a vertex stage and a fragment stage. Lines after `// @vertex` belong to
// the vertex stage (a `void mainVertex(inout vec3 pos, inout vec3 normal, in vec2 uv)` that moves
// the scene geometry), lines after `// @fragment` (or before any marker) to the fragment stage.
// Each stage gets the whole file with the other stage's lines blanked, so driver error line numbers
// still match the editor.
struct EffectParts {
    std::string fragment;
    std::string vertex;
    bool hasVertex = false;
};
EffectParts splitEffect(const std::string& source);
// Inserts the user's vertex code into the scene vertex shader (see assets/shaders/scene.vert):
// /*GLUE_USER_DECLS*/ gets the code, /*GLUE_USER_CALL*/ the call to mainVertex().
std::string wrapSceneVertex(const std::string& sceneVertexTemplate, const std::string& vertexUser);
// Number of lines the prelude adds before `#line 1` (for tests and tooling).
extern const char* kShadertoyVertex;

} // namespace glue
