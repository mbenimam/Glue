// Sobel edge detection on luminance, drawn as an ink outline over the scene.
uniform float uStrength = 4.0;   // @range 0.5 12
uniform vec3  uInk = vec3(0.0, 0.0, 0.0); // @color
uniform float uBlend = 0.8;      // @range 0 1

float lum(vec2 p) { return dot(texture(iChannel0, p).rgb, vec3(0.299, 0.587, 0.114)); }

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    vec2 e = 1.0 / iResolution.xy;
    float tl = lum(uv + vec2(-e.x, e.y)),  t = lum(uv + vec2(0, e.y)),  tr = lum(uv + vec2(e.x, e.y));
    float l  = lum(uv + vec2(-e.x, 0)),                                 r  = lum(uv + vec2(e.x, 0));
    float bl = lum(uv + vec2(-e.x, -e.y)), b = lum(uv + vec2(0, -e.y)), br = lum(uv + vec2(e.x, -e.y));
    float gx = -tl - 2.0 * l - bl + tr + 2.0 * r + br;
    float gy = -tl - 2.0 * t - tr + bl + 2.0 * b + br;
    float edge = clamp(length(vec2(gx, gy)) * uStrength, 0.0, 1.0);
    fragColor = vec4(mix(texture(iChannel0, uv).rgb, uInk, edge * uBlend), 1.0);
}
