#version 450 core
// Shows the path tracer's accumulation buffer: mean radiance -> exposure -> tone curve -> sRGB.
// Exposure and the tone curve only change how the mean is displayed, so editing them does not
// restart the accumulation.
in vec2 vUv;
out vec4 fragColor;
layout(binding = 0) uniform sampler2D uAccum;   // rgb = sum of samples, a = number of samples
uniform float uExposure;                        // stops (EV): radiance is multiplied by 2^EV
uniform int uTonemap;                           // 0 ACES, 1 filmic (Hable), 2 clamp

// ACES filmic fit (Narkowicz 2015).
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp(x * (a * x + b) / (x * (c * x + d) + e), 0.0, 1.0);
}
// Hable's "Uncharted 2" filmic curve, normalised so the white point W maps to 1.
vec3 hable(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return (x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F) - E / F;
}
vec3 srgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main() {
    vec4 s = texelFetch(uAccum, ivec2(gl_FragCoord.xy), 0);
    vec3 c = s.a > 0.0 ? s.rgb / s.a : vec3(0.0);
    c *= exp2(uExposure);
    if (uTonemap == 0) c = aces(c);
    else if (uTonemap == 1) c = clamp(hable(2.0 * c) / hable(vec3(11.2)), 0.0, 1.0);
    else c = clamp(c, 0.0, 1.0);
    fragColor = vec4(srgb(c), 1.0);
}
