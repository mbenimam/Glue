// Kaleidoscope: fold the image around the centre into N mirrored wedges, slowly rotating.
uniform int uSegments = 6;       // @range 2 16
uniform float uSpin = 0.2;       // @range 0 2

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 q = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    float a = atan(q.y, q.x) + iTime * uSpin;
    float r = length(q);
    float seg = 6.28318530718 / float(uSegments);
    a = abs(mod(a, seg) - 0.5 * seg);                    // mirror within a wedge
    vec2 p = vec2(cos(a), sin(a)) * r;
    fragColor = vec4(texture(iChannel0, p * vec2(iResolution.y / iResolution.x, 1.0) + 0.5).rgb, 1.0);
}
