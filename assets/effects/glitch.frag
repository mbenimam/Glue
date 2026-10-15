// Digital glitch: horizontal blocks slide sideways, channels split, and a few scanlines jump.
uniform float uIntensity = 0.6;  // @range 0 1
uniform float uBlock = 24.0;     // @range 4 80
uniform float uSpeed = 8.0;      // @range 0 20

float hash(float n) { return fract(sin(n * 127.1) * 43758.5453); }

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    float t = floor(iTime * uSpeed);
    float band = floor(uv.y * uBlock);
    // A band glitches only some of the time; the threshold falls as intensity grows.
    float on = step(1.0 - 0.5 * uIntensity, hash(band + t * 7.13));
    float shift = (hash(band * 3.7 + t) - 0.5) * 0.25 * uIntensity * on;
    float jump = step(0.985, hash(floor(uv.y * 400.0) + t)) * 0.08 * uIntensity;
    vec2 p = vec2(uv.x + shift + jump, uv.y);
    float split = 0.012 * uIntensity * (0.3 + on);
    vec3 col;
    col.r = texture(iChannel0, p + vec2(split, 0.0)).r;
    col.g = texture(iChannel0, p).g;
    col.b = texture(iChannel0, p - vec2(split, 0.0)).b;
    // Faint horizontal tearing noise inside active bands.
    col += on * 0.08 * uIntensity * hash(uv.x * 91.0 + band + t);
    fragColor = vec4(col, 1.0);
}
