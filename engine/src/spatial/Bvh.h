#pragma once
// Bounding volume hierarchy over triangles, built on the CPU and flattened so the same
// array can be copied into a shader storage buffer and walked by a GPU ray tracer.
#include <cstdint>
#include <vector>
#include "math/Math.h"
#include "scene/MeshData.h"

namespace prism {

struct Aabb {
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    void grow(const Vec3& p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    void grow(const Aabb& b) { if (b.valid()) { grow(b.lo); grow(b.hi); } }
    bool valid() const { return lo.x <= hi.x; }
    float area() const {                     // surface area; 0 for an empty box
        if (!valid()) return 0.0f;
        Vec3 d = hi - lo;
        return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
    }
};

struct Triangle { Vec3 v0, v1, v2; };

// 32 bytes, laid out like the GLSL std430 struct { vec3 lo; uint a; vec3 hi; uint count; }.
// Nodes are stored depth first: an interior node's left child is the next node (index + 1)
// and `leftOrFirst` is its right child. A leaf has count > 0 and its triangles are
// tris[leftOrFirst .. leftOrFirst + count) in the BVH's reordered triangle list.
struct BvhNode {
    float lo[3]; uint32_t leftOrFirst;
    float hi[3]; uint32_t count;
    bool leaf() const { return count > 0; }
};
static_assert(sizeof(BvhNode) == 32, "BvhNode must match the std430 layout");

struct RayHit {
    float t = 1e30f;          // distance along the (unnormalised) direction
    float u = 0, v = 0;       // barycentrics of v1 and v2
    uint32_t tri = ~0u;       // index into Bvh::tris (reordered), ~0 when nothing was hit
    bool hit() const { return tri != ~0u; }
};

// Moller-Trumbore: true when the ray hits the (double sided) triangle with tmin < t < hit.t.
bool intersectTriangle(const Vec3& org, const Vec3& dir, const Triangle& tri, float tmin, RayHit& hit, uint32_t id);
bool intersectAabb(const Vec3& org, const Vec3& invDir, const Vec3& lo, const Vec3& hi, float tmax, float& tEnter);

enum class BvhSplit { Sah, Median };
struct BvhOptions {
    BvhSplit split = BvhSplit::Sah;
    int bins = 16;            // SAH candidate planes per axis = bins - 1
    int maxLeaf = 8;          // never make leaves larger than this (median: always split above it)
    float traversalCost = 1.0f, intersectCost = 1.0f;   // SAH constants relative to one triangle test
};

class Bvh {
public:
    using Split = BvhSplit;
    using Options = BvhOptions;

    void build(const std::vector<Triangle>& tris, const Options& opt);
    void build(const std::vector<Triangle>& tris) { build(tris, Options{}); }
    void build(const MeshData& mesh, const Options& opt = Options{});

    // Closest hit along org + t*dir for t in (tmin, hit.t). `hit.tri` indexes `tris` below.
    bool intersect(const Vec3& org, const Vec3& dir, RayHit& hit, float tmin = 1e-5f) const;
    // Same query by testing every triangle: the reference the BVH must agree with.
    static bool intersectBrute(const std::vector<Triangle>& tris, const Vec3& org, const Vec3& dir, RayHit& hit,
                               float tmin = 1e-5f);

    // Expected cost of a random ray under the SAH model: sum over nodes of
    // P(hit node) * (traversal cost, or count * intersect cost for a leaf), P = area / root area.
    float sahCost() const;
    int depth() const;

    std::vector<BvhNode> nodes;
    std::vector<Triangle> tris;       // input triangles reordered so every leaf owns a contiguous range
    std::vector<uint32_t> triIndex;   // tris[i] is input triangle triIndex[i]

private:
    struct Ref { Aabb box; Vec3 centroid; };
    uint32_t buildNode(std::vector<Ref>& refs, uint32_t first, uint32_t count, int depth);
    Options opt_;
};

} // namespace prism
