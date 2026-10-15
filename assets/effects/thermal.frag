// Thermal camera: luminance becomes "temperature" and is mapped through a hot-metal palette.
uniform float uContrast = 1.4;   // @range 0.5 3

vec3 palette(float t) {
    vec3 a = vec3(0.0, 0.0, 0.25), b = vec3(0.6, 0.0, 0.6), c = vec3(1.0, 0.3, 0.0), d = vec3(1.0, 0.9, 0.2), e = vec3(1.0);
    if (t < 0.25) return mix(a, b, t * 4.0);
    if (t < 0.5)  return mix(b, c, (t - 0.25) * 4.0);
    if (t < 0.75) return mix(c, d, (t - 0.5) * 4.0);
    return mix(d, e, (t - 0.75) * 4.0);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    float l = dot(texture(iChannel0, uv).rgb, vec3(0.299, 0.587, 0.114));
    fragColor = vec4(palette(clamp(pow(l, 1.0 / uContrast), 0.0, 1.0)), 1.0);
}
