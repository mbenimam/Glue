// A vertex-stage effect: every vertex rides a travelling wave along its normal, so the silhouette
// moves, not just the colours. Lines after "// @vertex" run once per vertex of the scene mesh;
// lines after "// @fragment" run once per pixel as usual.
// @vertex
uniform float uAmp = 0.06;    // @range 0 0.2
uniform float uFreq = 7.0;    // @range 1 20
uniform float uSpeed = 3.0;   // @range 0 10

void mainVertex(inout vec3 pos, inout vec3 normal, in vec2 uv) {
    float phase = pos.y * uFreq - iTime * uSpeed;
    // slope of the displacement along y: tilt the normal against it so the lighting follows the wave
    float slope = uAmp * uFreq * cos(phase);
    pos += normal * (uAmp * sin(phase));
    normal = normalize(normal - vec3(0.0, slope, 0.0));
}

// @fragment
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    fragColor = texture(iChannel0, fragCoord / iResolution.xy);
}
