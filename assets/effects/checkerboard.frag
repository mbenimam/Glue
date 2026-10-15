// Checkerboard: alternate cells show the scene inverted, tinted or pixelated, and the board can scroll.
uniform float uSize = 48.0;      // @range 8 200
uniform float uMix = 0.7;        // @range 0 1
uniform float uScroll = 0.4;     // @range 0 3
uniform vec3  uTint = vec3(1.0, 0.2, 0.6); // @color

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    vec2 cell = floor((fragCoord + vec2(iTime * uScroll * uSize, 0.0)) / uSize);
    float odd = mod(cell.x + cell.y, 2.0);               // 0 or 1 like a chessboard
    vec3 c = texture(iChannel0, uv).rgb;
    vec3 alt = (1.0 - c) * uTint;                        // odd cells: inverted and tinted
    fragColor = vec4(mix(c, alt, odd * uMix), 1.0);
}
