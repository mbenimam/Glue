// Uses the scene's depth buffer (iChannel2): distant geometry fades into coloured fog.
// Depth is non-linear, so we first recover the eye-space distance (near 0.05, far 200 as in GlueLab).
uniform float uDensity = 2.0;    // @range 0 8
uniform vec3  uFog = vec3(0.1, 0.15, 0.3); // @color

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    float d = texture(iChannel2, uv).r * 2.0 - 1.0;      // back to NDC [-1,1]
    const float n = 0.05, f = 200.0;
    float z = 2.0 * n * f / (f + n - d * (f - n));       // eye-space distance
    float fog = 1.0 - exp(-uDensity * z * 0.03);
    fragColor = vec4(mix(texture(iChannel0, uv).rgb, uFog, fog), 1.0);
}
