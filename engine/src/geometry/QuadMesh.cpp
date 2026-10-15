#include "geometry/QuadMesh.h"
#include <algorithm>
#include <chrono>

namespace prism {

float quadSquareness(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d) {
    Vec3 p[4] = {a, b, c, d};
    float s = 0;
    for (int i = 0; i < 4; ++i) {
        Vec3 e1 = normalize(p[(i + 1) % 4] - p[i]), e2 = normalize(p[(i + 3) % 4] - p[i]);
        s += std::fabs(dot(e1, e2));
    }
    return 1.0f - s * 0.25f;
}

// Quality of a polygon given its corner positions and a reference normal (see header).
static float polygonQuality(const Vec3* p, int n, const Vec3& normal) {
    if (n == 3) {
        float area = 0.5f * length(cross(p[1] - p[0], p[2] - p[0]));
        float s = dot(p[1] - p[0], p[1] - p[0]) + dot(p[2] - p[1], p[2] - p[1]) + dot(p[0] - p[2], p[0] - p[2]);
        float q = s > 0 ? 4.0f * std::sqrt(3.0f) * area / s : 0.0f;
        return dot(cross(p[1] - p[0], p[2] - p[0]), normal) < 0 ? -q : q;
    }
    float q = 1.0f;
    for (int i = 0; i < n; ++i) {
        Vec3 e1 = p[(i + 1) % n] - p[i], e2 = p[(i + n - 1) % n] - p[i];
        float l = dot(e1, e1) + dot(e2, e2);
        float s = l > 0 ? 2.0f * dot(cross(e1, e2), normal) / l : 0.0f;   // = 2 |e1||e2| sin / (|e1|^2+|e2|^2)
        q = std::min(q, s);
    }
    return q;
}

float faceQuality(const HalfEdgeMesh& m, int f) {
    Vec3 p[16];
    int n = 0;
    m.forFaceHalfEdges(f, [&](int h) { if (n < 16) p[n++] = m.position(m.from(h)); });
    return polygonQuality(p, n, normalize(m.faceVectorArea(f)));
}

QuadConvertStats trianglesToQuads(HalfEdgeMesh& m, const QuadConvertOptions& opt) {
    auto t0 = std::chrono::steady_clock::now();
    QuadConvertStats st;
    auto isTri = [&](int f) { return f >= 0 && m.faceAlive(f) && m.faceDegree(f) == 3; };
    for (int f = 0; f < m.faceSlots(); ++f) if (isTri(f)) ++st.trianglesIn;

    // ---- step 1: score and flag edges
    struct Cand { float score; int e; };
    std::vector<Cand> cands;
    for (int e = 0; e < m.edgeSlots(); ++e) {
        if (!m.edgeAlive(e) || m.isBoundaryEdge(e)) continue;
        int h = 2 * e, t = h + 1;
        if (!isTri(m.face(h)) || !isTri(m.face(t)) || !m.canJoin(e)) continue;
        // Triangles (a, b, c) and (b, a, d) become the quad (a, d, b, c).
        Vec3 a = m.position(m.from(h)), b = m.position(m.to(h));
        Vec3 c = m.position(m.from(m.prev(h))), d = m.position(m.from(m.prev(t)));
        Vec3 q[4] = {a, d, b, c};
        Vec3 n = normalize(m.faceVectorArea(m.face(h)) + m.faceVectorArea(m.face(t)));
        if (polygonQuality(q, 4, n) <= 0.05f) continue;          // folded or nearly degenerate
        cands.push_back({quadSquareness(a, d, b, c), e});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.score > y.score; });
    std::vector<char> taken(m.faceSlots(), 0);
    std::vector<int> flagged;
    for (const auto& c : cands) {
        int f1 = m.face(2 * c.e), f2 = m.face(2 * c.e + 1);
        if (taken[f1] || taken[f2]) continue;
        taken[f1] = taken[f2] = 1;
        flagged.push_back(c.e);
    }
    for (int e : flagged) if (m.joinFaces(e) >= 0) ++st.pairedQuads;

    // ---- step 2: crawl leftover triangles towards each other
    if (opt.crawl) {
        std::vector<int> parentEdge(m.faceSlots(), -1), seen(m.faceSlots(), 0);
        int stampId = 0;
        for (int f0 = 0; f0 < m.faceSlots(); ++f0) {
            if (!isTri(f0)) continue;
            // Breadth-first search over faces for the nearest other triangle.
            ++stampId;
            if ((int)seen.size() < m.faceSlots()) { seen.resize(m.faceSlots(), 0); parentEdge.resize(m.faceSlots(), -1); }
            std::vector<int> queue{f0}, depth{0};
            seen[f0] = stampId;
            int target = -1;
            for (size_t qi = 0; qi < queue.size() && target < 0; ++qi) {
                int f = queue[qi];
                if (depth[qi] >= opt.maxCrawlDepth) continue;
                m.forFaceHalfEdges(f, [&](int h) {
                    int g = m.face(h ^ 1);
                    if (target >= 0 || g < 0 || seen[g] == stampId) return;
                    seen[g] = stampId;
                    parentEdge[g] = h ^ 1;                       // half-edge of g facing its parent
                    int deg = m.faceDegree(g);
                    if (deg == 3) target = g;
                    else if (deg == 4) { queue.push_back(g); depth.push_back(depth[qi] + 1); }
                });
            }
            if (target < 0) continue;
            // Path f0 -> ... -> target, as the sequence of faces after f0.
            std::vector<int> path;
            for (int g = target; g != f0; g = m.face(parentEdge[g] ^ 1)) path.push_back(g);
            std::reverse(path.begin(), path.end());
            int cur = f0;
            bool ok = true;
            for (size_t i = 0; i < path.size() && ok; ++i) {
                int nextFace = path[i];
                int g = -1;
                m.forFaceHalfEdges(cur, [&](int h) { if (g < 0 && m.face(h ^ 1) == nextFace) g = h; });
                if (g < 0) { ok = false; break; }
                if (i + 1 == path.size()) {
                    if (m.joinFaces(g >> 1) >= 0) ++st.crawledPairs; else ok = false;
                    break;
                }
                // Triangle + quad = pentagon; re-split it so the triangle touches the next face.
                int P = m.joinFaces(g >> 1);
                if (P < 0) { ok = false; break; }
                int after = path[i + 1], g2 = -1;
                m.forFaceHalfEdges(P, [&](int h) { if (g2 < 0 && m.face(h ^ 1) == after) g2 = h; });
                if (g2 < 0) { ok = false; break; }
                // Option A: triangle (prev corner, g2, next corner); option B: triangle (g2, next, next-next).
                auto pos = [&](int h) { return m.position(m.from(h)); };
                int n1 = m.next(g2), n2 = m.next(n1), n3 = m.next(n2), n4 = m.next(n3);   // n4 = prev(g2)
                float qa = quadSquareness(pos(n1), pos(n2), pos(n3), pos(n4));
                float qb = quadSquareness(pos(n2), pos(n3), pos(n4), pos(g2));
                bool split = false;
                if (qa >= qb) split = m.splitFace(n4, n1) >= 0 || m.splitFace(g2, n2) >= 0;
                else split = m.splitFace(g2, n2) >= 0 || m.splitFace(n4, n1) >= 0;
                if (!split) {
                    // Restore a triangle + quad with any free diagonal and give up on this one.
                    int h = n1;
                    for (int k = 0; k < 5 && m.splitFace(h, m.next(m.next(h))) < 0; ++k) h = m.next(h);
                    ok = false;
                    break;
                }
                cur = m.face(g2);
            }
        }
    }
    for (int f = 0; f < m.faceSlots(); ++f) {
        if (!m.faceAlive(f)) continue;
        int d = m.faceDegree(f);
        if (d == 4) ++st.quads; else if (d == 3) ++st.triangles; else ++st.others;
    }
    size_t faces = st.quads + st.triangles + st.others;
    st.percentQuads = faces ? 100.0 * st.quads / faces : 0;
    st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return st;
}

QuadQualityStats measureQuads(const HalfEdgeMesh& m) {
    QuadQualityStats s;
    s.minQuality = 1;
    double angleDev = 0;
    size_t corners = 0;
    for (int f = 0; f < m.faceSlots(); ++f) {
        if (!m.faceAlive(f)) continue;
        ++s.faces;
        int d = m.faceDegree(f);
        if (d == 3) { ++s.triangles; continue; }
        if (d != 4) { ++s.others; continue; }
        ++s.quads;
        float q = faceQuality(m, f);
        s.meanQuality += q;
        s.minQuality = std::min(s.minQuality, (double)q);
        m.forFaceHalfEdges(f, [&](int h) {
            Vec3 p = m.position(m.from(h));
            Vec3 e1 = normalize(m.position(m.to(h)) - p), e2 = normalize(m.position(m.from(m.prev(h))) - p);
            float ang = std::acos(std::clamp(dot(e1, e2), -1.0f, 1.0f)) * 180.0f / kPi;
            angleDev += std::fabs(ang - 90.0f);
            ++corners;
        });
    }
    if (s.quads) s.meanQuality /= s.quads; else s.minQuality = 0;
    s.meanAngleDeviation = corners ? angleDev / corners : 0;
    s.percentQuads = s.faces ? 100.0 * s.quads / s.faces : 0;
    size_t regular = 0, verts = 0;
    for (int v = 0; v < m.vertexSlots(); ++v) {
        if (!m.vertexAlive(v) || m.vertexHalfEdge(v) < 0) continue;
        ++verts;
        if (m.valence(v) == (m.isBoundaryVertex(v) ? 3 : 4)) ++regular;
    }
    s.percentRegular = verts ? 100.0 * regular / verts : 0;
    return s;
}

} // namespace prism
