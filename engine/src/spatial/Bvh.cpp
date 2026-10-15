#include "spatial/Bvh.h"
#include <algorithm>
#include <numeric>

namespace glue {

bool intersectTriangle(const Vec3& org, const Vec3& dir, const Triangle& tri, float tmin, RayHit& hit, uint32_t id) {
    // Solve org + t*dir = v0 + u*e1 + v*e2 with Cramer's rule, sharing the cross products.
    Vec3 e1 = tri.v1 - tri.v0, e2 = tri.v2 - tri.v0;
    Vec3 p = cross(dir, e2);
    float det = dot(e1, p);
    if (std::fabs(det) < 1e-12f) return false;          // ray parallel to the triangle plane
    float inv = 1.0f / det;
    Vec3 s = org - tri.v0;
    float u = dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    Vec3 q = cross(s, e1);
    float v = dot(dir, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    float t = dot(e2, q) * inv;
    if (t <= tmin || t >= hit.t) return false;
    hit.t = t; hit.u = u; hit.v = v; hit.tri = id;
    return true;
}

bool intersectAabb(const Vec3& org, const Vec3& invDir, const Vec3& lo, const Vec3& hi, float tmax, float& tEnter) {
    // Slab test: the ray is inside the box between the largest entry and the smallest exit.
    float tx1 = (lo.x - org.x) * invDir.x, tx2 = (hi.x - org.x) * invDir.x;
    float ty1 = (lo.y - org.y) * invDir.y, ty2 = (hi.y - org.y) * invDir.y;
    float tz1 = (lo.z - org.z) * invDir.z, tz2 = (hi.z - org.z) * invDir.z;
    float tnear = std::max({std::min(tx1, tx2), std::min(ty1, ty2), std::min(tz1, tz2)});
    float tfar = std::min({std::max(tx1, tx2), std::max(ty1, ty2), std::max(tz1, tz2)});
    tEnter = tnear;
    return tfar >= std::max(tnear, 0.0f) && tnear < tmax;
}

static float axisOf(const Vec3& v, int a) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); }

void Bvh::build(const MeshData& mesh, const Options& opt) {
    std::vector<Triangle> t(mesh.triangleCount());
    for (size_t i = 0; i < t.size(); ++i)
        t[i] = {mesh.positions[mesh.indices[3 * i]], mesh.positions[mesh.indices[3 * i + 1]],
                mesh.positions[mesh.indices[3 * i + 2]]};
    build(t, opt);
}

void Bvh::build(const std::vector<Triangle>& input, const Options& opt) {
    opt_ = opt;
    nodes.clear();
    tris.clear();
    triIndex.resize(input.size());
    std::iota(triIndex.begin(), triIndex.end(), 0u);
    if (input.empty()) return;
    std::vector<Ref> refs(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        refs[i].box.grow(input[i].v0); refs[i].box.grow(input[i].v1); refs[i].box.grow(input[i].v2);
        refs[i].centroid = (refs[i].box.lo + refs[i].box.hi) * 0.5f;
    }
    nodes.reserve(input.size() * 2);   // a binary tree with n leaves has 2n - 1 nodes at most
    buildNode(refs, 0, (uint32_t)input.size(), 0);
    tris.resize(input.size());
    for (size_t i = 0; i < input.size(); ++i) tris[i] = input[triIndex[i]];
}

// Builds the subtree over triIndex[first .. first+count) and returns its node index.
uint32_t Bvh::buildNode(std::vector<Ref>& refs, uint32_t first, uint32_t count, int depth) {
    Aabb box, cbox;
    for (uint32_t i = first; i < first + count; ++i) {
        box.grow(refs[triIndex[i]].box);
        cbox.grow(refs[triIndex[i]].centroid);
    }
    uint32_t idx = (uint32_t)nodes.size();
    nodes.push_back({{box.lo.x, box.lo.y, box.lo.z}, first, {box.hi.x, box.hi.y, box.hi.z}, count});
    auto leaf = [&]() { nodes[idx].leftOrFirst = first; nodes[idx].count = count; return idx; };

    if (count == 1 || depth > 60) return leaf();
    Vec3 ext = cbox.hi - cbox.lo;
    uint32_t* begin = triIndex.data() + first;
    uint32_t* end = begin + count;
    uint32_t* mid = nullptr;

    if (opt_.split == Split::Sah) {
        // Binned SAH: drop centroids into `bins` equal slices per axis, then sweep the bin
        // boundaries from both sides to get the cost of every candidate plane in O(bins).
        struct Bin { Aabb box; uint32_t n = 0; };
        const int B = std::max(2, opt_.bins);
        float best = 1e30f;
        int bestAxis = -1, bestPlane = 0;
        for (int a = 0; a < 3; ++a) {
            float lo = axisOf(cbox.lo, a), extent = axisOf(ext, a);
            if (extent <= 1e-9f) continue;
            std::vector<Bin> bins(B);
            float k = B / extent;
            for (uint32_t i = first; i < first + count; ++i) {
                const Ref& r = refs[triIndex[i]];
                int b = std::min(B - 1, (int)((axisOf(r.centroid, a) - lo) * k));
                bins[b].box.grow(r.box);
                bins[b].n++;
            }
            std::vector<float> rightArea(B);
            std::vector<uint32_t> rightCount(B);
            Aabb acc; uint32_t n = 0;
            for (int b = B - 1; b > 0; --b) {
                acc.grow(bins[b].box); n += bins[b].n;
                rightArea[b] = acc.area(); rightCount[b] = n;
            }
            acc = Aabb{}; n = 0;
            for (int b = 0; b < B - 1; ++b) {       // plane between bin b and b + 1
                acc.grow(bins[b].box); n += bins[b].n;
                float c = acc.area() * n + rightArea[b + 1] * rightCount[b + 1];
                if (n && rightCount[b + 1] && c < best) { best = c; bestAxis = a; bestPlane = b; }
            }
        }
        float leafCost = count * opt_.intersectCost;
        float splitCost = opt_.traversalCost + opt_.intersectCost * best / box.area();
        if (count <= (uint32_t)opt_.maxLeaf && (bestAxis < 0 || splitCost >= leafCost)) return leaf();
        if (bestAxis >= 0) {
            float lo = axisOf(cbox.lo, bestAxis), k = B / axisOf(ext, bestAxis);
            mid = std::partition(begin, end, [&](uint32_t t) {
                return std::min(B - 1, (int)((axisOf(refs[t].centroid, bestAxis) - lo) * k)) <= bestPlane;
            });
        }
    } else if (count <= (uint32_t)opt_.maxLeaf) {
        return leaf();
    }
    if (!mid || mid == begin || mid == end) {
        // Median split (and the fallback when SAH cannot separate): half the triangles on
        // each side of the median centroid along the longest axis.
        int a = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
        mid = begin + count / 2;
        std::nth_element(begin, mid, end, [&](uint32_t l, uint32_t r) {
            return axisOf(refs[l].centroid, a) < axisOf(refs[r].centroid, a);
        });
    }
    uint32_t leftCount = (uint32_t)(mid - begin);
    nodes[idx].count = 0;
    buildNode(refs, first, leftCount, depth + 1);                                      // lands at idx + 1
    uint32_t right = buildNode(refs, first + leftCount, count - leftCount, depth + 1);
    nodes[idx].leftOrFirst = right;
    return idx;
}

bool Bvh::intersect(const Vec3& org, const Vec3& dir, RayHit& hit, float tmin) const {
    if (nodes.empty()) return false;
    Vec3 inv{1.0f / dir.x, 1.0f / dir.y, 1.0f / dir.z};
    uint32_t stack[64];
    int sp = 0;
    uint32_t n = 0;
    bool any = false;
    float tEnter;
    if (!intersectAabb(org, inv, {nodes[0].lo[0], nodes[0].lo[1], nodes[0].lo[2]},
                       {nodes[0].hi[0], nodes[0].hi[1], nodes[0].hi[2]}, hit.t, tEnter)) return false;
    for (;;) {
        const BvhNode& node = nodes[n];
        if (node.leaf()) {
            for (uint32_t i = node.leftOrFirst; i < node.leftOrFirst + node.count; ++i)
                any |= intersectTriangle(org, dir, tris[i], tmin, hit, i);
        } else {
            // Visit the nearer child first so the far one is often culled by the shorter hit.t.
            uint32_t a = n + 1, b = node.leftOrFirst;
            float ta, tb;
            const BvhNode &A = nodes[a], &B = nodes[b];
            bool ha = intersectAabb(org, inv, {A.lo[0], A.lo[1], A.lo[2]}, {A.hi[0], A.hi[1], A.hi[2]}, hit.t, ta);
            bool hb = intersectAabb(org, inv, {B.lo[0], B.lo[1], B.lo[2]}, {B.hi[0], B.hi[1], B.hi[2]}, hit.t, tb);
            if (ha && hb) {
                if (tb < ta) std::swap(a, b);
                stack[sp++] = b;
                n = a;
                continue;
            }
            if (ha) { n = a; continue; }
            if (hb) { n = b; continue; }
        }
        if (sp == 0) break;
        n = stack[--sp];
    }
    return any;
}

bool Bvh::intersectBrute(const std::vector<Triangle>& tris, const Vec3& org, const Vec3& dir, RayHit& hit, float tmin) {
    bool any = false;
    for (uint32_t i = 0; i < (uint32_t)tris.size(); ++i) any |= intersectTriangle(org, dir, tris[i], tmin, hit, i);
    return any;
}

float Bvh::sahCost() const {
    if (nodes.empty()) return 0.0f;
    auto area = [](const BvhNode& n) {
        Aabb b; b.lo = {n.lo[0], n.lo[1], n.lo[2]}; b.hi = {n.hi[0], n.hi[1], n.hi[2]};
        return b.area();
    };
    float root = area(nodes[0]), cost = 0.0f;
    for (const BvhNode& n : nodes)
        cost += area(n) / root * (n.leaf() ? n.count * opt_.intersectCost : opt_.traversalCost);
    return cost;
}

int Bvh::depth() const {
    if (nodes.empty()) return 0;
    std::vector<std::pair<uint32_t, int>> stack{{0u, 1}};
    int best = 0;
    while (!stack.empty()) {
        auto [n, d] = stack.back();
        stack.pop_back();
        best = std::max(best, d);
        if (!nodes[n].leaf()) { stack.push_back({n + 1, d + 1}); stack.push_back({nodes[n].leftOrFirst, d + 1}); }
    }
    return best;
}

} // namespace glue
