// Old CRT monitor: barrel distortion, scanlines, RGB shadow mask, vignette.
uniform float uCurve = 0.15;     // @range 0 0.5
uniform float uScan = 0.35;      // @range 0 1

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    vec2 q = uv * 2.0 - 1.0;
    q *= 1.0 + uCurve * dot(q, q);                       // barrel distortion
    vec2 p = q * 0.5 + 0.5;
    if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    vec3 c = texture(iChannel0, p).rgb;
    float scan = 1.0 - uScan * (0.5 + 0.5 * sin(p.y * iResolution.y * 3.14159));
    float m = mod(floor(fragCoord.x), 3.0);              // shadow mask: one phosphor per pixel column
    vec3 mask = vec3(m == 0.0 ? 1.0 : 0.7, m == 1.0 ? 1.0 : 0.7, m == 2.0 ? 1.0 : 0.7);
    float vig = smoothstep(1.5, 0.4, length(q));
    fragColor = vec4(c * scan * mask * vig * 1.15, 1.0);
}
