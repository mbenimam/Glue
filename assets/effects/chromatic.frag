// Chromatic aberration: the red and blue channels sample radially away from the centre.
uniform float uAmount = 0.02;    // @range 0 0.08

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    vec2 d = (uv - 0.5) * uAmount;
    fragColor = vec4(texture(iChannel0, uv + d).r, texture(iChannel0, uv).g, texture(iChannel0, uv - d).b, 1.0);
}
