#pragma once
// Discrete differential geometry on triangle meshes (Meyer, Desbrun, Schroeder, Barr 2003).
//
// All per-vertex arrays are indexed by vertex slot (HalfEdgeMesh::vertexSlots()), per-face arrays
// by face slot, per-edge arrays by edge slot; removed elements get zeros.
// Curvature operators need triangles: call HalfEdgeMesh::triangulate() on a copy for quad meshes
// (it adds no vertices, so the results map back one to one).
#include <vector>
#include "geometry/HalfEdgeMesh.h"

namespace prism {

std::vector<Vec3> faceNormals(const HalfEdgeMesh& m);       // unit Newell normals
std::vector<float> faceAreas(const HalfEdgeMesh& m);
// Each incident face contributes its normal weighted by the corner angle at the vertex
// (Thuermer and Wuethrich): insensitive to how a fan happens to be triangulated.
std::vector<Vec3> vertexNormals(const HalfEdgeMesh& m);

// w_ij = (cot alpha_ij + cot beta_ij) / 2, the angles opposite the edge (one term on the boundary).
std::vector<float> cotanWeights(const HalfEdgeMesh& m);
// Mixed Voronoi area: Voronoi region of the vertex inside non-obtuse triangles, and a fixed
// fraction (1/2 at the obtuse corner, 1/4 elsewhere) of obtuse ones, so areas tile the surface.
std::vector<float> mixedAreas(const HalfEdgeMesh& m);
// K(v) = 1 / (2 A_v) * sum_j (cot a + cot b) (x_v - x_j) = 2 H n  (zero on the boundary)
std::vector<Vec3> meanCurvatureNormals(const HalfEdgeMesh& m);
// Interior: 2 pi - sum of corner angles. Boundary: pi - sum (the turning angle of the boundary),
// so the total equals 2 pi chi on any valid mesh (discrete Gauss-Bonnet).
std::vector<float> angleDefects(const HalfEdgeMesh& m);

struct Curvature {
    std::vector<float> area;       // mixed area
    std::vector<float> mean;       // signed H (positive on a sphere with outward normals)
    std::vector<float> gauss;      // K = defect / area
    std::vector<float> k1, k2;     // principal curvatures, k1 >= k2
    std::vector<Vec3> normal;      // angle-weighted vertex normal
};
Curvature computeCurvature(const HalfEdgeMesh& m);

// Explicit Laplacian smoothing, boundary vertices fixed. Uniform: move towards the neighbour
// average. Cotan: towards the cotan-weighted average, which moves vertices along the normal
// (mean curvature flow) and hardly slides them tangentially.
void smoothUniform(HalfEdgeMesh& m, int iterations, float lambda = 0.5f);
void smoothCotan(HalfEdgeMesh& m, int iterations, float lambda = 0.5f);

} // namespace prism
