// Hash grid neighbour queries against brute force on random point clouds.
#include "spatial/HashGrid.h"
#include <algorithm>
#include <random>

#include "TestFramework.h"

using namespace glue;

static std::vector<uint32_t> bruteForce(const std::vector<Vec3>& p, const Vec3& x, float r) {
    std::vector<uint32_t> out;
    for (size_t j = 0; j < p.size(); ++j) {
        Vec3 d = p[j] - x;
        if (dot(d, d) <= r * r) out.push_back((uint32_t)j);
    }
    return out;
}

// Every query must return exactly the brute-force set: nothing missing, nothing twice.
static bool sameNeighbors(const HashGrid& g, const std::vector<Vec3>& p, const Vec3& x, float r) {
    std::vector<uint32_t> a;
    g.query(x, r, a);
    std::vector<uint32_t> b = bruteForce(p, x, r);
    std::sort(a.begin(), a.end());
    return a == b;
}

TEST(spatial_testHashGridRandom) {
    std::mt19937 rng(7);
    // Several densities and cell sizes; negative coordinates exercise floor() on both sides of 0.
    struct Case { int n; float extent, h; };
    for (Case c : {Case{500, 1.0f, 0.1f}, Case{3000, 2.0f, 0.15f}, Case{4000, 0.5f, 0.05f}, Case{200, 10.0f, 0.7f}}) {
        std::uniform_real_distribution<float> u(-c.extent, c.extent);
        std::vector<Vec3> p(c.n);
        for (auto& v : p) v = {u(rng), u(rng), u(rng)};
        HashGrid g;
        g.build(p.data(), p.size(), c.h);
        CHECK(g.tableSize() >= 2u * (uint32_t)c.n);
        CHECK(g.sortedIndices().size() == p.size());
        CHECK(g.bucketStart().back() == (uint32_t)p.size());
        int bad = 0;
        for (size_t i = 0; i < p.size(); i += 3) {
            if (!sameNeighbors(g, p, p[i], c.h)) ++bad;            // at the points themselves
            if (!sameNeighbors(g, p, p[i], c.h * 0.5f)) ++bad;     // smaller radius than the cell
        }
        for (int k = 0; k < 200; ++k)                             // arbitrary query positions
            if (!sameNeighbors(g, p, {u(rng), u(rng), u(rng)}, c.h)) ++bad;
        CHECK(bad == 0);
    }
}

TEST(spatial_testHashGridSortedIsPermutation) {
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    std::vector<Vec3> p(1500);
    for (auto& v : p) v = {u(rng), u(rng), u(rng)};
    HashGrid g;
    g.build(p.data(), p.size(), 0.08f);
    std::vector<uint32_t> s = g.sortedIndices();
    std::sort(s.begin(), s.end());
    bool perm = true;
    for (size_t i = 0; i < s.size(); ++i) perm = perm && s[i] == i;
    CHECK(perm);
    // Every index sits in the bucket its cell hashes to.
    const auto& st = g.bucketStart();
    int misplaced = 0;
    for (uint32_t b = 0; b < g.tableSize(); ++b)
        for (uint32_t k = st[b]; k < st[b + 1]; ++k) {
            const Vec3& q = p[g.sortedIndices()[k]];
            float inv = 1.0f / g.cellSize();
            uint32_t want = HashGrid::hashCell(HashGrid::cellCoord(q.x, inv), HashGrid::cellCoord(q.y, inv),
                                               HashGrid::cellCoord(q.z, inv), g.tableSize() - 1);
            if (want != b) ++misplaced;
        }
    CHECK(misplaced == 0);
}

TEST(spatial_testHashGridDegenerate) {
    HashGrid g;
    std::vector<Vec3> none;
    g.build(none.data(), 0, 0.1f);
    std::vector<uint32_t> out;
    g.query({0, 0, 0}, 0.1f, out);
    CHECK(out.empty());
    // All points in one cell: the query must still return each exactly once.
    std::vector<Vec3> same(50, Vec3{0.01f, 0.01f, 0.01f});
    g.build(same.data(), same.size(), 0.1f);
    g.query({0, 0, 0}, 0.1f, out);
    CHECK(out.size() == 50);
}
