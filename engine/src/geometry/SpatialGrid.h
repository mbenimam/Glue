#pragma once
// Uniform grid over the triangles of a mesh, for closest-point queries.
//
// Each triangle is registered in every cell its bounding box touches. A query visits cubic
// shells of cells around the query point, growing outwards, and stops once the best distance
// found is shorter than the distance from the point to the outside of the visited block: no
// triangle further out can be closer. With about one triangle per cell this costs a few dozen
// point-triangle tests per query, independent of the mesh size.
#include <cstdint>
#include <vector>
#include "math/Math.h"

namespace prism {

// Closest point of triangle (a, b, c) to p, with barycentric weights (Ericson, Real-Time
// Collision Detection, 5.1.5).
Vec3 closestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c, float bary[3]);

class TriangleGrid {
public:
    struct Hit {
        Vec3 point;
        int triangle = -1;
        float bary[3] = {0, 0, 0};
        float dist2 = 0;
    };

    // `tris` holds 3 indices per triangle into `pos`. `trianglesPerCell` sets the resolution.
    void build(const std::vector<Vec3>& pos, const std::vector<uint32_t>& tris, float trianglesPerCell = 0.15f);
    Hit closest(const Vec3& q) const;
    bool empty() const { return tris_.empty(); }
    const std::vector<Vec3>& positions() const { return pos_; }
    const std::vector<uint32_t>& triangles() const { return tris_; }

private:
    int cellIndex(int x, int y, int z) const { return (z * ny_ + y) * nx_ + x; }

    std::vector<Vec3> pos_;
    std::vector<uint32_t> tris_;
    std::vector<uint32_t> cellStart_, cellItems_;   // CSR layout: items of cell i are [start[i], start[i+1])
    Vec3 lo_, hi_;
    float cell_ = 1.0f;
    int nx_ = 1, ny_ = 1, nz_ = 1;
    mutable std::vector<uint32_t> stamp_;           // per triangle: last query that tested it
    mutable uint32_t query_ = 0;
};

} // namespace prism
