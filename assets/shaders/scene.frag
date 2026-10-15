#version 450 core
in vec3 vWorld;
in vec3 vNormal;
in vec3 vObj;
in vec2 vUv;
out vec4 oColor;
uniform vec3 uCameraPos;
uniform vec3 uBase;
uniform float uTime;
uniform int uCheckerOn;
layout(binding = 1) uniform sampler2D uAlbedo;
uniform int uHasAlbedo;

void main() {
    vec3 n = normalize(vNormal);
    vec3 v = normalize(uCameraPos - vWorld);
    vec3 albedo = uBase;
    if (uCheckerOn == 1) {
        // Object-space checker so it is independent of mesh UVs (teapot patches have seams).
        vec3 p = floor(vObj * 4.0);
        float c = mod(p.x + p.y + p.z, 2.0);
        albedo = mix(uBase * 0.35, uBase, c);
    }
    if (uHasAlbedo == 1) albedo *= texture(uAlbedo, vUv).rgb;
    vec3 L1 = normalize(vec3(0.6, 0.8, 0.5)), L2 = normalize(vec3(-0.7, 0.2, -0.4));
    float d1 = max(dot(n, L1), 0.0), d2 = max(dot(n, L2), 0.0) * 0.35;
    vec3 h = normalize(L1 + v);
    float spec = pow(max(dot(n, h), 0.0), 64.0) * 0.6;
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0) * 0.25;
    vec3 col = albedo * (0.12 + d1 + d2) + spec + rim;
    oColor = vec4(col, 1.0);
}
