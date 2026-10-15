#version 450 core
in vec3 vWorld;
in vec3 vNormal;
in vec3 vObj;
in vec2 vUv;
out vec4 oColor;
uniform vec3 uCameraPos;
void main() {
    vec2 g = abs(fract(vWorld.xz * 0.5 - 0.5) - 0.5) / fwidth(vWorld.xz * 0.5);
    float line = 1.0 - min(min(g.x, g.y), 1.0);
    float fade = exp(-length(vWorld.xz - uCameraPos.xz) * 0.06);
    vec3 c = mix(vec3(0.10, 0.11, 0.14), vec3(0.35, 0.38, 0.5), line);
    oColor = vec4(c * (0.4 + 0.6 * fade), 1.0);
}
