// Night-vision goggles: light amplification, green phosphor, grain, scanlines, round lens mask.
uniform float uGain = 3.0;       // @range 1 8
uniform float uNoise = 0.25;     // @range 0 1
uniform vec3  uPhosphor = vec3(0.25, 1.0, 0.35); // @color

float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    vec3 c = texture(iChannel0, uv).rgb;
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    l = 1.0 - exp(-l * uGain);                         // soft gain, never clips hard
    l += (hash(fragCoord + iTime * 60.0) - 0.5) * uNoise;
    l *= 0.85 + 0.15 * sin(fragCoord.y * 1.6 + iTime * 8.0); // scanlines
    vec2 q = (uv - 0.5) * vec2(iResolution.x / iResolution.y, 1.0);
    float mask = smoothstep(0.52, 0.46, length(q));    // circular goggle view
    fragColor = vec4(uPhosphor * clamp(l, 0.0, 1.0) * mask, 1.0);
}
