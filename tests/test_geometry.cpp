// Geometry processing: half-edge structure, discrete differential geometry, quad meshing.
#include "geometry/Differential.h"
#include "geometry/HalfEdgeMesh.h"
#include "geometry/QuadMesh.h"
#include "geometry/QuadSimplify.h"
#include "geometry/SpatialGrid.h"
#include "scene/MeshData.h"
#include <algorithm>
#include <map>
#include <set>
#include <string>

#include "TestFramework.h"

using namespace prism;

namespace {
// Tiny deterministic generator so the random-operation tests are reproducible.
struct Lcg {
    unsigned s;
    unsigned next() { s = s * 1664525u + 1013904223u; return s >> 8; }
    int below(int n) { return (int)(next() % (unsigned)n); }
};

bool valid(const HalfEdgeMesh& m) {
    std::string why;
    bool ok = m.validate(&why);
    if (!ok) std::printf("    invalid: %s\n", why.c_str());
    return ok;
}

// A cube of n x n quads per side, projected on the unit sphere: a pure, closed quad mesh.
HalfEdgeMesh quadSphere(int n) {
    std::vector<Vec3> pos;
    std::vector<std::vector<int>> faces;
    std::map<std::tuple<int, int, int>, int> ids;
    auto vid = [&](int x, int y, int z) {
        auto k = std::make_tuple(x, y, z);
        auto it = ids.find(k);
        if (it != ids.end()) return it->second;
        pos.push_back(normalize(Vec3{(float)x / n * 2 - 1, (float)y / n * 2 - 1, (float)z / n * 2 - 1}));
        return ids[k] = (int)pos.size() - 1;
    };
    // For each axis and side, a grid whose winding faces outward.
    for (int axis = 0; axis < 3; ++axis)
        for (int side = 0; side < 2; ++side)
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    int c[4][2] = {{i, j}, {i + 1, j}, {i + 1, j + 1}, {i, j + 1}};
                    std::vector<int> f;
                    for (auto& q : c) {
                        int p[3];
                        p[axis] = side * n; p[(axis + 1) % 3] = q[0]; p[(axis + 2) % 3] = q[1];
                        f.push_back(vid(p[0], p[1], p[2]));
                    }
                    if (side == 0) std::reverse(f.begin(), f.end());
                    faces.push_back(f);
                }
    return HalfEdgeMesh::fromPolygons(pos, faces);
}
} // namespace

TEST(geo_halfEdgeTeapot) {
    MeshData t = makeTeapot(8);
    BuildReport rep;
    HalfEdgeMesh m = HalfEdgeMesh::fromMeshData(t, &rep);
    CHECK(valid(m));
    CHECK(rep.facesKept == t.triangleCount());
    CHECK(rep.flippedFaces == 0);
    MeshStats s = m.stats();
    // V - E + F from the half-edge structure equals the count done directly on the index buffer.
    std::set<std::pair<uint32_t, uint32_t>> edges;
    for (size_t i = 0; i < t.indices.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            uint32_t a = t.indices[i + k], b = t.indices[i + (k + 1) % 3];
            edges.insert({std::min(a, b), std::max(a, b)});
        }
    CHECK(s.edges == edges.size());
    // (plus the vertices the builder had to duplicate where two sheets touched at a point)
    CHECK(s.euler == (long long)(t.positions.size() + rep.splitVertices) - (long long)edges.size() + (long long)t.triangleCount());
    CHECK(s.boundaryLoops > 0);                                  // the teapot is not watertight
    CHECK(s.euler == 2 * (long long)s.components - 2 * s.genus - (long long)s.boundaryLoops);
    // Closing every hole gives a closed surface whose components are all spheres.
    size_t holes = fillHoles(m);
    CHECK(holes == s.boundaryLoops);
    CHECK(valid(m));
    MeshStats c = m.stats();
    CHECK(c.boundaryLoops == 0 && c.boundaryEdges == 0);
    CHECK(c.euler == 2 * (long long)c.components);
    CHECK(c.genus == 0);
}

TEST(geo_halfEdgeSphere) {
    // A UV sphere has a seam and collapsed poles: open before welding, closed (chi = 2) after.
    MeshData uv = makeUvSphere(16, 24, 1.0f);
    MeshStats before = HalfEdgeMesh::fromMeshData(uv).stats();
    CHECK(before.boundaryLoops > 0);
    weldVertices(uv);
    HalfEdgeMesh m = HalfEdgeMesh::fromMeshData(uv);
    CHECK(valid(m));
    MeshStats s = m.stats();
    CHECK(s.boundaryLoops == 0);
    CHECK(s.euler == 2);
    CHECK(s.components == 1 && s.genus == 0);
    HalfEdgeMesh ico = HalfEdgeMesh::fromMeshData(makeIcosphere(3));
    MeshStats si = ico.stats();
    CHECK(valid(ico));
    CHECK(si.faces == 20u * 64u && si.euler == 2 && si.triangles == si.faces);
    // Every vertex of an icosphere has valence 5 or 6.
    for (int v = 0; v < ico.vertexSlots(); ++v) { int k = ico.valence(v); CHECK(k == 5 || k == 6); }
}

TEST(geo_halfEdgeRepair) {
    // Flip a third of the icosphere's faces: the builder re-orients them.
    MeshData ico = makeIcosphere(2);
    std::vector<std::vector<int>> faces;
    for (size_t t = 0; t < ico.triangleCount(); ++t) {
        std::vector<int> f = {(int)ico.indices[3 * t], (int)ico.indices[3 * t + 1], (int)ico.indices[3 * t + 2]};
        if (t % 3 == 0) std::swap(f[1], f[2]);
        faces.push_back(f);
    }
    BuildReport rep;
    HalfEdgeMesh m = HalfEdgeMesh::fromPolygons(ico.positions, faces, &rep);
    CHECK(valid(m));
    CHECK(rep.orientable);
    CHECK(rep.flippedFaces == (ico.triangleCount() + 2) / 3);
    CHECK(m.stats().boundaryLoops == 0);
    // Moebius strip: 5 quads with a half twist cannot be oriented.
    std::vector<Vec3> mp;
    std::vector<std::vector<int>> mf;
    const int n = 5;
    for (int i = 0; i < n; ++i) {
        float a = 2 * kPi * i / n, tw = a * 0.5f;
        Vec3 c{std::cos(a), 0, std::sin(a)}, d{std::cos(a) * std::cos(tw), std::sin(tw), std::sin(a) * std::cos(tw)};
        mp.push_back(c + d * 0.3f); mp.push_back(c - d * 0.3f);
    }
    for (int i = 0; i < n; ++i) {
        int a = 2 * i, b = 2 * i + 1, c = 2 * ((i + 1) % n), d = c + 1;
        if (i == n - 1) std::swap(c, d);            // the twist
        mf.push_back({a, b, d, c});
    }
    HalfEdgeMesh mob = HalfEdgeMesh::fromPolygons(mp, mf, &rep);
    CHECK(!rep.orientable);
    CHECK(valid(mob));
    // Bow-tie (two triangles touching at one vertex) is split into two manifold vertices,
    // and a third face on an existing edge is rejected.
    std::vector<Vec3> bp = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0}, {0, 0, 1}};
    HalfEdgeMesh bow = HalfEdgeMesh::fromPolygons(bp, {{0, 1, 2}, {0, 3, 4}}, &rep);
    CHECK(valid(bow));
    CHECK(rep.splitVertices == 1);
    CHECK(bow.stats().vertices == 7 && bow.stats().isolated == 1 && bow.stats().components == 2);
    HalfEdgeMesh fin = HalfEdgeMesh::fromPolygons(bp, {{0, 1, 2}, {1, 0, 4}, {0, 1, 5}}, &rep);
    CHECK(rep.rejectedNonManifold == 1);
    CHECK(valid(fin));
}

TEST(geo_collapseKeepsValid) {
    // Random collapses: each must either be refused or leave a valid mesh with chi unchanged.
    for (int which = 0; which < 2; ++which) {
        MeshData src = which ? makeTeapot(6) : makeIcosphere(3);
        HalfEdgeMesh m = HalfEdgeMesh::fromMeshData(src);
        MeshStats s0 = m.stats();
        Lcg rng{7u + which};
        int done = 0, refused = 0;
        bool allValid = true, chiKept = true;
        for (int it = 0; it < 4000 && m.liveFaces() > 60; ++it) {
            int e = rng.below(m.edgeSlots());
            if (!m.edgeAlive(e)) continue;
            int h = 2 * e + rng.below(2);
            Vec3 mid = (m.position(m.from(h)) + m.position(m.to(h))) * 0.5f;
            if (!m.collapseEdge(h, mid)) { ++refused; continue; }
            ++done;
            if (!valid(m)) { allValid = false; break; }
            MeshStats s = m.stats();
            if (s.euler != s0.euler || s.boundaryLoops != s0.boundaryLoops) chiKept = false;
        }
        CHECK(allValid);
        CHECK(chiKept);
        CHECK(done > 300);
        CHECK(refused > 0);
        // Flips keep validity too.
        int flips = 0;
        std::vector<int> alive;
        for (int e = 0; e < m.edgeSlots(); ++e) if (m.edgeAlive(e)) alive.push_back(e);
        for (int it = 0; it < 500; ++it) {
            int e = alive[rng.below((int)alive.size())];
            if (m.flipEdge(e)) ++flips;
        }
        CHECK(flips > 20);
        CHECK(valid(m));
        CHECK(m.stats().euler == s0.euler);
        m.garbageCollect();
        CHECK(valid(m));
    }
}

TEST(geo_quadOperators) {
    HalfEdgeMesh m = quadSphere(8);
    CHECK(valid(m));
    MeshStats s0 = m.stats();
    CHECK(s0.quads == 6u * 64u && s0.euler == 2);
    // Split + join round trip.
    int f = 0;
    int h0 = m.faceHalfEdge(f);
    int n = m.splitFace(h0, m.next(m.next(h0)));
    CHECK(n >= 0);
    CHECK(valid(m) && m.stats().triangles == 2);
    CHECK(m.joinFaces(n >> 1) >= 0);
    CHECK(valid(m) && m.stats().quads == s0.quads);
    // Random diagonal collapses with doublet / singlet cleaning, and rotations.
    Lcg rng{99};
    int collapses = 0, rotations = 0, doublets = 0;
    bool ok = true;
    for (int it = 0; it < 3000 && ok; ++it) {
        int fi = rng.below(m.faceSlots());
        if (!m.faceAlive(fi)) continue;
        if (rng.below(3) == 0) {
            int h = m.faceHalfEdge(fi);
            if (m.rotateEdge(h >> 1, rng.below(2) ? 1 : -1)) { ++rotations; ok = valid(m); }
            continue;
        }
        int h = m.faceHalfEdge(fi);
        if (rng.below(2)) h = m.next(h);
        int a = m.from(h), c = m.from(m.next(m.next(h)));
        Vec3 mid = (m.position(a) + m.position(c)) * 0.5f;
        int b = m.from(m.next(h)), d = m.from(m.prev(h));
        if (m.collapseDiagonal(h, mid) < 0) continue;
        ++collapses;
        for (int v : {b, d}) if (m.vertexAlive(v) && m.removeDoublet(v)) ++doublets;
        ok = valid(m);
        MeshStats s = m.stats();
        if (s.euler != 2 || s.triangles != 0) ok = false;
        if (m.liveFaces() < 30) break;
    }
    CHECK(ok);
    CHECK(collapses > 100);
    CHECK(rotations > 20);
    CHECK(doublets > 0);
    // Singlet: fold a face by hand. A quad (x, v, x', y) style fold is easiest to make with a
    // vertex hanging inside a face: build a square with a dangling edge and remove it.
    std::vector<Vec3> sp = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    HalfEdgeMesh sq = HalfEdgeMesh::fromPolygons(sp, {{0, 1, 2, 3}});
    CHECK(!sq.isSinglet(0));
}

// ---------------------------------------------------------------- differential geometry

TEST(geo_curvatureSphere) {
    const float R = 2.0f;
    HalfEdgeMesh m = HalfEdgeMesh::fromMeshData(makeIcosphere(4, R));
    Curvature c = computeCurvature(m);
    double hSum = 0, kSum = 0, hMax = 0, kMax = 0, area = 0;
    int n = 0;
    for (int v = 0; v < m.vertexSlots(); ++v) {
        hSum += c.mean[v]; kSum += c.gauss[v]; area += c.area[v]; ++n;
        hMax = std::max(hMax, std::fabs(c.mean[v] - 1.0 / R));
        kMax = std::max(kMax, std::fabs(c.gauss[v] - 1.0 / (R * R)));
    }
    CHECK_NEAR(hSum / n, 1.0 / R, 0.01 / R);
    CHECK_NEAR(kSum / n, 1.0 / (R * R), 0.01 / (R * R));
    CHECK(hMax < 0.05 / R);
    CHECK(kMax < 0.10 / (R * R));
    CHECK_NEAR(area, 4 * kPi * R * R, 0.01 * 4 * kPi * R * R);   // mixed areas tile the polyhedron
    // Normals point outward and match the radial direction.
    for (int v = 0; v < m.vertexSlots(); ++v) CHECK(dot(c.normal[v], normalize(m.position(v))) > 0.999f);
    // UV sphere (after welding): obtuse pole triangles; away from the poles still ~1/R.
    MeshData uv = makeUvSphere(32, 48, R);
    weldVertices(uv);
    HalfEdgeMesh mu = HalfEdgeMesh::fromMeshData(uv);
    Curvature cu = computeCurvature(mu);
    double worst = 0;
    for (int v = 0; v < mu.vertexSlots(); ++v)
        if (std::fabs(mu.position(v).y) < 0.8f * R) worst = std::max(worst, std::fabs(cu.mean[v] - 1.0 / R));
    CHECK(worst < 0.03 / R);
}

TEST(geo_gaussBonnet) {
    // Sum of angle defects = 2 pi chi, closed and open, for any valid triangle mesh.
    auto total = [](const HalfEdgeMesh& m) {
        double s = 0;
        for (float d : angleDefects(m)) s += d;
        return s;
    };
    HalfEdgeMesh ico = HalfEdgeMesh::fromMeshData(makeIcosphere(3));
    CHECK_NEAR(total(ico), 2 * kPi * 2, 1e-3);
    HalfEdgeMesh tea = HalfEdgeMesh::fromMeshData(makeTeapot(8));
    MeshStats s = tea.stats();
    CHECK_NEAR(total(tea), 2 * kPi * (double)s.euler, 2e-2);
    fillHoles(tea);
    CHECK_NEAR(total(tea), 2 * kPi * (double)tea.stats().euler, 2e-2);
    // A flat disc has zero curvature inside: chi = 1, all of 2 pi sits on the boundary.
    HalfEdgeMesh disc = HalfEdgeMesh::fromMeshData(makePlane(1.0f));
    CHECK_NEAR(total(disc), 2 * kPi, 1e-4);
}

TEST(geo_smoothing) {
    // Noisy sphere: both smoothers reduce the radius variance; cotan keeps the shape closer to round.
    MeshData src = makeIcosphere(3);
    Lcg rng{3};
    for (auto& p : src.positions) p = p * (1.0f + ((float)rng.below(1000) / 1000.0f - 0.5f) * 0.1f);
    auto spread = [](const HalfEdgeMesh& m) {
        double mean = 0, var = 0; int n = 0;
        for (int v = 0; v < m.vertexSlots(); ++v) { mean += length(m.position(v)); ++n; }
        mean /= n;
        for (int v = 0; v < m.vertexSlots(); ++v) { double d = length(m.position(v)) - mean; var += d * d; }
        return std::sqrt(var / n) / mean;
    };
    HalfEdgeMesh a = HalfEdgeMesh::fromMeshData(src), b = HalfEdgeMesh::fromMeshData(src);
    double s0 = spread(a);
    smoothUniform(a, 5, 0.5f);
    smoothCotan(b, 5, 0.5f);
    CHECK(spread(a) < s0 * 0.5);
    CHECK(spread(b) < s0 * 0.5);
    CHECK(valid(a) && valid(b));
}

// ---------------------------------------------------------------- spatial grid

TEST(geo_closestPointGrid) {
    MeshData m = makeTeapot(6);
    TriangleGrid g;
    g.build(m.positions, m.indices);
    Lcg rng{11};
    int agree = 0;
    const int n = 300;
    for (int i = 0; i < n; ++i) {
        Vec3 q{(rng.below(2000) - 1000) / 600.0f, (rng.below(2000) - 1000) / 600.0f, (rng.below(2000) - 1000) / 600.0f};
        float best = 1e30f, w[3];
        for (size_t t = 0; t < m.indices.size(); t += 3) {
            Vec3 p = closestPointOnTriangle(q, m.positions[m.indices[t]], m.positions[m.indices[t + 1]], m.positions[m.indices[t + 2]], w);
            best = std::min(best, dot(p - q, p - q));
        }
        TriangleGrid::Hit h = g.closest(q);
        if (std::fabs(h.dist2 - best) <= 1e-6f * std::max(1.0f, best)) ++agree;
        CHECK_NEAR(h.bary[0] + h.bary[1] + h.bary[2], 1.0, 1e-4);
    }
    CHECK(agree == n);                                   // the grid search is exact, not approximate
}

// ---------------------------------------------------------------- tri -> quad

TEST(geo_triToQuad) {
    CHECK_NEAR(quadSquareness({0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {0, 1, 0}), 1.0, 1e-6);   // rectangle
    CHECK(quadSquareness({0, 0, 0}, {2, 0, 0}, {3, 1, 0}, {1, 1, 0}) < 0.75f);              // parallelogram
    for (int which = 0; which < 2; ++which) {
        HalfEdgeMesh m = HalfEdgeMesh::fromMeshData(which ? makeTeapot(8) : makeIcosphere(3));
        MeshStats s0 = m.stats();
        QuadConvertStats st = trianglesToQuads(m);
        CHECK(valid(m));
        MeshStats s1 = m.stats();
        CHECK(s1.vertices == s0.vertices);               // no vertex added or moved
        CHECK(s1.euler == s0.euler && s1.boundaryLoops == s0.boundaryLoops);
        CHECK(st.others == 0);
        // Closed components have an even triangle count, so crawling pairs every triangle.
        CHECK(st.triangles == 0);
        CHECK(st.quads * 2 == s0.faces);
        CHECK(st.crawledPairs > 0);
        CHECK(st.pairedQuads > st.quads * 9 / 10);       // greedy pairing does most of the work
    }
}

// ---------------------------------------------------------------- simplification

namespace {
// Sphere with one sharp bump: curvature concentrated in one place.
MeshData bumpSphere() {
    MeshData m = makeIcosphere(4, 1.0f);
    Vec3 c = normalize(Vec3{1, 1, 0});
    for (auto& p : m.positions) {
        Vec3 d = normalize(p) - c;
        p = normalize(p) * (1.0f + 0.3f * std::exp(-dot(d, d) / 0.04f));
    }
    m.computeNormals();
    return m;
}

struct Prepared {
    HalfEdgeMesh tri, quads;
    ReferenceSurface ref;
    Fitmaps fit;
};
Prepared prepare(const MeshData& md) {
    Prepared p;
    p.tri = HalfEdgeMesh::fromMeshData(md);
    p.ref.build(p.tri);
    p.fit = computeFitmaps(p.ref);
    p.quads = p.tri;
    trianglesToQuads(p.quads);
    return p;
}
} // namespace

TEST(geo_quadSimplifyInvariants) {
    Prepared p = prepare(makeTeapot(8));
    MeshStats s0 = p.quads.stats();
    // Boundary vertices must not move: remember their positions.
    std::set<std::tuple<float, float, float>> boundary;
    for (int v = 0; v < p.quads.vertexSlots(); ++v)
        if (p.quads.isBoundaryVertex(v)) boundary.insert({p.quads.position(v).x, p.quads.position(v).y, p.quads.position(v).z});
    for (int adaptive = 0; adaptive < 2; ++adaptive) {
        HalfEdgeMesh m = p.quads;
        QuadSimplifyOptions o;
        o.targetQuads = 400;
        o.adaptive = adaptive;
        QuadSimplifyStats st = simplifyQuadMesh(m, p.ref, &p.fit, o);
        CHECK(valid(m));
        MeshStats s = m.stats();
        CHECK(s.euler == s0.euler && s.boundaryLoops == s0.boundaryLoops && s.components == s0.components);
        CHECK(st.quadsOut <= 400 && st.quadsOut >= 380);
        CHECK(s.triangles == 0);
        CHECK(st.collapses > 1000 && st.rotations > 0 && st.doublets > 0);
        bool fixed = true;
        for (int v = 0; v < m.vertexSlots(); ++v)
            if (m.vertexAlive(v) && m.isBoundaryVertex(v) &&
                !boundary.count({m.position(v).x, m.position(v).y, m.position(v).z})) fixed = false;
        CHECK(fixed);
        // Every vertex stays on the input surface (projection after each move).
        float worst = 0;
        for (int v = 0; v < m.vertexSlots(); ++v)
            if (m.vertexAlive(v)) worst = std::max(worst, p.ref.project(m.position(v)).distance);
        CHECK(worst < 1e-4f);
        QuadQualityStats q = measureQuads(m);
        CHECK(q.percentRegular > 60);
        CHECK(q.meanQuality > 0.5);
        SurfaceDistance d = surfaceDistance(m, p.ref, 20000);
        CHECK(d.hausdorff < 0.06 * d.diagonal);
        CHECK(0.5 * (d.meanAB + d.meanBA) < 0.005 * d.diagonal);
    }
}

TEST(geo_quadSimplifyAdaptivity) {
    // Adaptive keeps small faces where the S-fitmap is high (the bump), large ones elsewhere;
    // uniform spreads the same budget evenly.
    Prepared p = prepare(bumpSphere());
    std::vector<float> sorted = p.fit.scale;
    std::sort(sorted.begin(), sorted.end());
    const float median = sorted[sorted.size() / 2], top = sorted.back();
    auto areaRatio = [&](bool adaptive) {
        HalfEdgeMesh m = p.quads;
        QuadSimplifyOptions o;
        o.targetQuads = 300;
        o.adaptive = adaptive;
        simplifyQuadMesh(m, p.ref, &p.fit, o);
        // Mean face area on the plain sphere (S near its median) over mean area on the bump.
        double lo = 0, hi = 0;
        int nlo = 0, nhi = 0;
        for (int f = 0; f < m.faceSlots(); ++f) {
            if (!m.faceAlive(f)) continue;
            float s = p.ref.sample(p.fit.scale, p.ref.project(m.faceCentroid(f)));
            double a = length(m.faceVectorArea(f));
            if (s < median * 1.02f) { lo += a; ++nlo; }
            else if (s > median + 0.5f * (top - median)) { hi += a; ++nhi; }
        }
        return nlo && nhi ? (lo / nlo) / (hi / nhi) : 0.0;
    };
    double uniform = areaRatio(false), adaptive = areaRatio(true);
    CHECK(uniform < 1.5);
    CHECK(adaptive > 1.5 * uniform);
}

TEST(geo_triangleQem) {
    MeshData md = makeTeapot(8);
    HalfEdgeMesh m = HalfEdgeMesh::fromMeshData(md);
    ReferenceSurface ref;
    ref.build(m);
    MeshStats s0 = m.stats();
    TriQemStats st = simplifyTrianglesQEM(m, 1000);
    CHECK(valid(m));
    CHECK(st.facesOut <= 1000 && st.facesOut >= 990);
    MeshStats s = m.stats();
    CHECK(s.euler == s0.euler && s.boundaryLoops == s0.boundaryLoops);
    SurfaceDistance d = surfaceDistance(m, ref, 20000);
    CHECK(d.hausdorff < 0.01 * d.diagonal);
}
