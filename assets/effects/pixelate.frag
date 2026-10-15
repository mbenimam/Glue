// Pixelate with a limited palette per channel.
uniform float uPixel = 8.0;      // @range 1 40
uniform int uLevels = 5;         // @range 2 16

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 cell = floor(fragCoord / uPixel) * uPixel + 0.5 * uPixel;
    vec3 c = texture(iChannel0, cell / iResolution.xy).rgb;
    float n = float(uLevels - 1);
    fragColor = vec4(floor(c * n + 0.5) / n, 1.0);
}
