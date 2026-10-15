#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "math/Math.h"

namespace glue {

// CPU-side triangle mesh. Everything geometry-related (teapot, OBJ, later the
// half-edge structure) starts from this plain indexed representation.
struct MeshData {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    std::vector<uint32_t> indices;  // 3 per triangle

    size_t triangleCount() const { return indices.size() / 3; }
    void computeNormals();                       // area-weighted vertex normals
    void bounds(Vec3& lo, Vec3& hi) const;
    void normalizeToUnitBox();                   // center at origin, largest extent 2
    float signedVolume() const;                  // > 0 when triangles face outward
    void flipWinding();
};

// Utah teapot tessellated from its 32 bicubic Bezier patches (Y up, centred, largest extent 2 units).
// `segments` is the subdivision count per patch edge. Vertices are welded across patches.
MeshData makeTeapot(int segments = 12);
MeshData makeUvSphere(int rings, int sectors, float radius = 1.0f);
// Subdivided icosahedron: near-uniform triangles, 20 * 4^subdivisions faces, closed and welded.
MeshData makeIcosphere(int subdivisions, float radius = 1.0f);
MeshData makePlane(float halfSize, float y = 0.0f);

// Wavefront OBJ: v/vt/vn, polygons (fan-triangulated), negative indices.
bool loadObj(const std::string& path, MeshData& out, std::string* err = nullptr);
bool parseObj(const std::string& text, MeshData& out, std::string* err = nullptr);

// Merge vertices whose positions coincide within `eps`. Returns the number removed.
size_t weldVertices(MeshData& m, float eps = 1e-4f);

} // namespace glue
