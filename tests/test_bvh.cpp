// BVH tests: the tree must return exactly what testing every triangle returns, and the
// SAH build must produce a cheaper tree (under its own cost model) than a median split.
#include "scene/MeshData.h"
#include "spatial/Bvh.h"

#include "TestFramework.h"

using namespace glue;

namespace {
struct Rng {   // small deterministic LCG so failures reproduce
    uint32_t s;
    float next() { s = s * 1664525u + 1013904223u; return (s >> 8) * (1.0f / 16777216.0f); }
    float range(float a, float b) { return a + (b - a) * next(); }
};

std::vector<Triangle> soup(int n, uint32_t seed) {
    Rng r{seed};
    std::vector<Triangle> t(n);
    for (auto& tri : t) {
        Vec3 c{r.range(-5, 5), r.range(-5, 5), r.range(-5, 5)};
        float s = r.range(0.05f, 1.0f);
        tri = {c + Vec3{r.range(-s, s), r.range(-s, s), r.range(-s, s)}, c + Vec3{r.range(-s, s), r.range(-s, s), r.range(-s, s)},
               c + Vec3{r.range(-s, s), r.range(-s, s), r.range(-s, s)}};
    }
    return t;
}

// Fires `rays` random rays from a sphere around the scene towards random points inside it and
// counts disagreements between the BVH and brute force (hit/miss, distance, which triangle).
int mismatches(const Bvh& bvh, const std::vector<Triangle>& input, int rays, float radius, uint32_t seed, int* hits) {
    Rng r{seed};
    int bad = 0;
    *hits = 0;
    for (int i = 0; i < rays; ++i) {
        Vec3 o = normalize(Vec3{r.range(-1, 1), r.range(-1, 1), r.range(-1, 1)}) * radius;
        Vec3 target{r.range(-radius, radius) * 0.3f, r.range(-radius, radius) * 0.15f, r.range(-radius, radius) * 0.3f};
        Vec3 d = normalize(target - o);
        if (i % 4 == 0) o = target;   // some rays start inside the scene
        RayHit a, b;
        bool ha = bvh.intersect(o, d, a), hb = Bvh::intersectBrute(input, o, d, b);
        if (ha != hb) { ++bad; continue; }
        if (!ha) continue;
        ++*hits;
        // Same distance, and the BVH's reordered triangle maps back to the brute-force one
        // (unless two triangles happen to share the hit point).
        if (std::fabs(a.t - b.t) > 1e-5f * std::max(1.0f, b.t)) ++bad;
        else if (bvh.triIndex[a.tri] != b.tri && std::fabs(a.t - b.t) > 0.0f) ++bad;
    }
    return bad;
}
} // namespace

TEST(bvh_testTriangle) {
    Triangle t{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    RayHit h;
    CHECK(intersectTriangle({0.25f, 0.25f, 1}, {0, 0, -1}, t, 1e-5f, h, 7));
    CHECK_NEAR(h.t, 1.0, 1e-6);
    CHECK_NEAR(h.u, 0.25, 1e-6); CHECK_NEAR(h.v, 0.25, 1e-6);
    CHECK(h.tri == 7u);
    RayHit m;
    CHECK(!intersectTriangle({0.8f, 0.8f, 1}, {0, 0, -1}, t, 1e-5f, m, 0));   // outside u + v <= 1
    CHECK(!intersectTriangle({0.2f, 0.2f, 1}, {1, 0, 0}, t, 1e-5f, m, 0));    // parallel
    CHECK(!intersectTriangle({0.2f, 0.2f, -1}, {0, 0, -1}, t, 1e-5f, m, 0));  // behind the origin
}

TEST(bvh_testTeapotMatchesBruteForce) {
    MeshData teapot = makeTeapot(10);
    Bvh bvh;
    bvh.build(teapot);
    std::vector<Triangle> input(teapot.triangleCount());
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = {teapot.positions[teapot.indices[3 * i]], teapot.positions[teapot.indices[3 * i + 1]],
                    teapot.positions[teapot.indices[3 * i + 2]]};
    CHECK(bvh.tris.size() == input.size());
    // Every triangle sits in exactly one leaf.
    size_t inLeaves = 0;
    for (const auto& n : bvh.nodes) if (n.leaf()) inLeaves += n.count;
    CHECK(inLeaves == input.size());
    int hits = 0;
    CHECK(mismatches(bvh, input, 4000, 3.0f, 11u, &hits) == 0);
    std::printf("    teapot: %d of 4000 rays hit\n", hits);
    CHECK(hits > 1000);   // the test is meaningless if most rays miss
    CHECK(bvh.depth() < 40);
}

TEST(bvh_testSoupMatchesBruteForce) {
    for (uint32_t seed : {1u, 2u, 3u}) {
        auto input = soup(1500, seed);
        for (auto split : {Bvh::Split::Sah, Bvh::Split::Median}) {
            Bvh::Options opt;
            opt.split = split;
            Bvh bvh;
            bvh.build(input, opt);
            int hits = 0;
            CHECK(mismatches(bvh, input, 1500, 9.0f, seed * 77u, &hits) == 0);
            CHECK(hits > 300);
        }
    }
    // Degenerate input: all triangles stacked on the same spot must still build and trace.
    std::vector<Triangle> same(50, Triangle{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}});
    Bvh bvh;
    bvh.build(same);
    RayHit h;
    CHECK(bvh.intersect({0.2f, 0.2f, 1}, {0, 0, -1}, h));
    CHECK_NEAR(h.t, 1.0, 1e-6);
}

TEST(bvh_testSahBeatsMedian) {
    MeshData teapot = makeTeapot(12);
    auto input = soup(4000, 9u);
    for (int scene = 0; scene < 2; ++scene) {
        Bvh sah, median;
        Bvh::Options o;
        o.maxLeaf = 4;
        if (scene == 0) sah.build(teapot, o); else sah.build(input, o);
        o.split = Bvh::Split::Median;
        if (scene == 0) median.build(teapot, o); else median.build(input, o);
        std::printf("    %s: SAH cost %.2f (%zu nodes)  median cost %.2f (%zu nodes)\n", scene ? "soup" : "teapot",
                    sah.sahCost(), sah.nodes.size(), median.sahCost(), median.nodes.size());
        CHECK(sah.sahCost() < median.sahCost());
    }
}
