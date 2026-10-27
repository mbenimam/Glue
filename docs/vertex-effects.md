# Vertex-stage effects in GlueLab

An effect file is normally one fragment shader (`mainImage`) that post-processes the rendered scene.
It can also move the scene's vertices.

```glsl
// @vertex
uniform float uAmp = 0.06;   // @range 0 0.2

void mainVertex(inout vec3 pos, inout vec3 normal, in vec2 uv) {
    pos += normal * (uAmp * sin(pos.y * 7.0 - iTime * 3.0));
}

// @fragment
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    fragColor = texture(iChannel0, fragCoord / iResolution.xy);
}
```

## How it works

- `splitEffect()` (`engine/src/gfx/ShaderToy.cpp`) cuts the file at `// @vertex` and `// @fragment`.
  Text before any marker is fragment code, so every existing effect is unchanged.
- Each stage receives the *whole file with the other stage's lines blanked*, not a shorter file.
  Line numbers stay the same, so a driver error "line 12" is line 12 in the editor, and the
  error-line marks in the gutter point at the right place.
- `wrapSceneVertex()` inserts the user's code into `assets/shaders/scene.vert` at the
  `/*GLUE_USER_DECLS*/` marker and adds a call to `mainVertex(p, n, aUv)` at `/*GLUE_USER_CALL*/`,
  *before* the model matrix is applied. The checker pattern uses the undisplaced position, so it stays
  glued to the surface instead of swimming.
- `uniform float uX = 0.5; // @range 0 1` lines work in either part: the parameter panel parses the whole
  file, and each frame the values are set on both the effect pipeline and the scene pipeline.
  `iTime` is available in the vertex part.
- Both pipelines are compiled together; if either fails, the error is shown ("vertex stage: ...") and
  the previous working pair stays active.

## Limits

- It moves the vertices of the scene mesh (the teapot or a loaded OBJ), not the ground grid.
- Normals are yours to update: a displacement that changes the surface slope needs a tilted normal
  or the lighting will look flat (see `wobble.frag`).
- A displacement can only be as smooth as the mesh is dense; the teapot has 16 segments per patch.
