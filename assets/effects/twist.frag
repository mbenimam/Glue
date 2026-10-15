// A vertex-stage effect: the mesh is twisted about the vertical axis, more the higher the vertex.
// @vertex
uniform float uTwist = 1.6;   // @range -4 4
uniform float uSwing = 0.8;   // @range 0 3

void mainVertex(inout vec3 pos, inout vec3 normal, in vec2 uv) {
    float a = pos.y * uTwist * sin(iTime * uSwing);
    float c = cos(a), s = sin(a);
    mat2 r = mat2(c, -s, s, c);      // rotation of the (x, z) plane
    pos.xz = r * pos.xz;
    normal.xz = r * normal.xz;       // a pure rotation turns normals the same way
}

// @fragment
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    fragColor = texture(iChannel0, fragCoord / iResolution.xy);
}
