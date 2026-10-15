#include "geometry/HalfEdgeMesh.h"
#include "scene/MeshData.h"
#include <algorithm>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace glue {

// ---------------------------------------------------------------- allocation

int HalfEdgeMesh::newVertex(const Vec3& p) {
    verts_.push_back({p, -1, false});
    return (int)verts_.size() - 1;
}

int HalfEdgeMesh::newEdge() {
    he_.push_back({});
    he_.push_back({});
    return (int)he_.size() - 2;
}

int HalfEdgeMesh::newFace() {
    faces_.push_back({});
    ++liveFaces_;
    return (int)faces_.size() - 1;
}

void HalfEdgeMesh::killEdge(int e) {
    for (int h : {2 * e, 2 * e + 1}) he_[h] = {-1, -1, -1, -1};
}
void HalfEdgeMesh::killFace(int f) {
    if (!faces_[f].dead) --liveFaces_;
    faces_[f].dead = true;
    faces_[f].he = -1;
}
void HalfEdgeMesh::killVertex(int v) {
    verts_[v].dead = true;
    verts_[v].he = -1;
}

void HalfEdgeMesh::fixVertexHalfEdge(int v) {
    // The caller made sure `he` is a live outgoing half-edge; rotate to a boundary one if there is one.
    int h0 = verts_[v].he;
    if (h0 < 0) return;
    int h = h0, guard = 0;
    do {
        if (he_[h].face < 0) { verts_[v].he = h; return; }
        h = he_[h ^ 1].next;
    } while (h != h0 && ++guard < (1 << 20));
}

// ---------------------------------------------------------------- construction

static uint64_t edgeKey(int a, int b) {
    uint32_t lo = (uint32_t)std::min(a, b), hi = (uint32_t)std::max(a, b);
    return ((uint64_t)lo << 32) | hi;
}

HalfEdgeMesh HalfEdgeMesh::fromPolygons(const std::vector<Vec3>& positions,
                                        const std::vector<std::vector<int>>& facesIn,
                                        BuildReport* report) {
    BuildReport rep;
    rep.facesIn = facesIn.size();
    const int nv = (int)positions.size();

    // 1. Drop faces that cannot be polygons: < 3 corners, out-of-range or repeated vertex.
    std::vector<std::vector<int>> faces;
    faces.reserve(facesIn.size());
    for (const auto& f : facesIn) {
        bool ok = f.size() >= 3;
        for (size_t i = 0; ok && i < f.size(); ++i) {
            if (f[i] < 0 || f[i] >= nv) ok = false;
            for (size_t j = i + 1; ok && j < f.size(); ++j) ok = f[i] != f[j];
        }
        if (ok) faces.push_back(f); else ++rep.rejectedDegenerate;
    }

    // 2. Orientation repair. Two faces agree across a shared edge when they walk it in opposite
    //    directions. Breadth-first search assigns flip bits; a contradiction means the component
    //    cannot be oriented (Moebius strip, Klein bottle).
    struct Use { int face; bool forward; };
    std::unordered_map<uint64_t, std::vector<Use>> uses;
    uses.reserve(faces.size() * 3);
    for (int f = 0; f < (int)faces.size(); ++f)
        for (size_t i = 0; i < faces[f].size(); ++i) {
            int a = faces[f][i], b = faces[f][(i + 1) % faces[f].size()];
            uses[edgeKey(a, b)].push_back({f, a < b});
        }
    std::vector<int> flip(faces.size(), -1);
    for (int seed = 0; seed < (int)faces.size(); ++seed) {
        if (flip[seed] >= 0) continue;
        std::vector<int> comp{seed};
        flip[seed] = 0;
        for (size_t qi = 0; qi < comp.size(); ++qi) {
            int f = comp[qi];
            const auto& fv = faces[f];
            for (size_t i = 0; i < fv.size(); ++i) {
                const auto& u = uses[edgeKey(fv[i], fv[(i + 1) % fv.size()])];
                if (u.size() != 2) continue;            // boundary or non-manifold: no constraint
                const Use& me = u[0].face == f ? u[0] : u[1];
                const Use& other = u[0].face == f ? u[1] : u[0];
                if (other.face == f) continue;
                int want = (me.forward ^ (bool)flip[f]) == other.forward ? 1 : 0;
                if (flip[other.face] < 0) { flip[other.face] = want; comp.push_back(other.face); }
                else if (flip[other.face] != want) rep.orientable = false;
            }
        }
        // Keep the orientation most of the input already had.
        size_t flipped = 0;
        for (int f : comp) flipped += flip[f];
        if (flipped * 2 > comp.size()) for (int f : comp) flip[f] ^= 1;
    }
    for (size_t f = 0; f < faces.size(); ++f)
        if (flip[f] == 1) { std::reverse(faces[f].begin(), faces[f].end()); ++rep.flippedFaces; }

    // 3. Insert faces. A directed edge a->b may be used once; a face that needs a used one is skipped.
    HalfEdgeMesh m;
    m.verts_.reserve(nv);
    for (const auto& p : positions) m.newVertex(p);
    std::unordered_map<uint64_t, int> edgeOf;
    edgeOf.reserve(faces.size() * 2);
    std::vector<char> used;
    std::vector<int> loop;
    for (const auto& fv : faces) {
        bool ok = true;
        for (size_t i = 0; ok && i < fv.size(); ++i) {
            int a = fv[i], b = fv[(i + 1) % fv.size()];
            auto it = edgeOf.find(edgeKey(a, b));
            if (it == edgeOf.end()) continue;
            int h = m.he_[2 * it->second].from == a ? 2 * it->second : 2 * it->second + 1;
            if (used[h]) ok = false;
        }
        if (!ok) { ++rep.rejectedNonManifold; continue; }
        int f = m.newFace();
        loop.clear();
        for (size_t i = 0; i < fv.size(); ++i) {
            int a = fv[i], b = fv[(i + 1) % fv.size()];
            auto it = edgeOf.find(edgeKey(a, b));
            int h;
            if (it == edgeOf.end()) {
                h = m.newEdge();
                m.he_[h].from = a;
                m.he_[h + 1].from = b;
                used.push_back(0); used.push_back(0);
                edgeOf.emplace(edgeKey(a, b), h / 2);
            } else {
                h = m.he_[2 * it->second].from == a ? 2 * it->second : 2 * it->second + 1;
            }
            used[h] = 1;
            m.he_[h].face = f;
            loop.push_back(h);
        }
        for (size_t i = 0; i < loop.size(); ++i) m.link(loop[i], loop[(i + 1) % loop.size()]);
        m.faces_[f].he = loop[0];
        ++rep.facesKept;
    }

    // 4. Boundary loops: the boundary half-edge after h (a->b) is the first boundary half-edge met
    //    when rotating around b through the faces, starting from twin(h).
    for (int h = 0; h < (int)m.he_.size(); ++h) {
        if (m.he_[h].face >= 0) continue;
        int g = h ^ 1, guard = 0;
        while (m.he_[g].face >= 0 && ++guard < (1 << 20)) g = m.he_[g].prev ^ 1;
        m.link(h, g);
    }
    for (int h = 0; h < (int)m.he_.size(); ++h)
        if (m.verts_[m.he_[h].from].he < 0 || m.he_[h].face < 0) m.verts_[m.he_[h].from].he = h;

    // 5. Split non-manifold vertices (two fans touching at one vertex) into one copy per fan.
    std::vector<int> visitMark(m.he_.size(), 0);
    std::vector<std::vector<int>> outgoing(m.verts_.size());
    for (int h = 0; h < (int)m.he_.size(); ++h) outgoing[m.he_[h].from].push_back(h);
    for (int v = 0; v < nv; ++v) {
        bool first = true;
        for (int h0 : outgoing[v]) {
            if (visitMark[h0]) continue;
            // The fan of h0: rotate both ways is unnecessary because boundary half-edges close it.
            int start = h0, h = h0, guard = 0;
            // Prefer to start the fan at its boundary half-edge.
            do { if (m.he_[h].face < 0) start = h; h = m.he_[h ^ 1].next; } while (h != h0 && ++guard < (1 << 20));
            int target = v;
            if (!first) { target = m.newVertex(m.verts_[v].p); ++rep.splitVertices; }
            h = start;
            do { visitMark[h] = 1; m.he_[h].from = target; h = m.he_[h ^ 1].next; } while (h != start);
            m.verts_[target].he = start;
            first = false;
        }
    }
    if (report) *report = rep;
    return m;
}

HalfEdgeMesh HalfEdgeMesh::fromMeshData(const MeshData& md, BuildReport* report) {
    std::vector<std::vector<int>> faces(md.triangleCount());
    for (size_t t = 0; t < faces.size(); ++t)
        faces[t] = {(int)md.indices[3 * t], (int)md.indices[3 * t + 1], (int)md.indices[3 * t + 2]};
    return fromPolygons(md.positions, faces, report);
}

void HalfEdgeMesh::toPolygons(std::vector<Vec3>& positions, std::vector<std::vector<int>>& faces) const {
    std::vector<int> remap(verts_.size(), -1);
    positions.clear(); faces.clear();
    for (int v = 0; v < (int)verts_.size(); ++v)
        if (!verts_[v].dead) { remap[v] = (int)positions.size(); positions.push_back(verts_[v].p); }
    for (int f = 0; f < (int)faces_.size(); ++f) {
        if (faces_[f].dead) continue;
        std::vector<int> fv;
        forFaceHalfEdges(f, [&](int h) { fv.push_back(remap[he_[h].from]); });
        faces.push_back(std::move(fv));
    }
}

MeshData HalfEdgeMesh::toMeshData() const {
    MeshData md;
    std::vector<std::vector<int>> faces;
    toPolygons(md.positions, faces);
    for (const auto& f : faces)
        for (size_t i = 1; i + 1 < f.size(); ++i)
            md.indices.insert(md.indices.end(), {(uint32_t)f[0], (uint32_t)f[i], (uint32_t)f[i + 1]});
    md.computeNormals();
    return md;
}

// ---------------------------------------------------------------- queries

int HalfEdgeMesh::valence(int v) const {
    int n = 0;
    forOutgoing(v, [&](int) { ++n; });
    return n;
}

int HalfEdgeMesh::faceDegree(int f) const {
    int n = 0;
    forFaceHalfEdges(f, [&](int) { ++n; });
    return n;
}

void HalfEdgeMesh::faceVertices(int f, std::vector<int>& out) const {
    out.clear();
    forFaceHalfEdges(f, [&](int h) { out.push_back(he_[h].from); });
}

int HalfEdgeMesh::findHalfEdge(int a, int b) const {
    int found = -1;
    forOutgoing(a, [&](int h) { if (found < 0 && to(h) == b) found = h; });
    return found;
}

Vec3 HalfEdgeMesh::faceCentroid(int f) const {
    Vec3 c; int n = 0;
    forFaceHalfEdges(f, [&](int h) { c += verts_[he_[h].from].p; ++n; });
    return n ? c / (float)n : c;
}

Vec3 HalfEdgeMesh::faceVectorArea(int f) const {
    Vec3 n;
    forFaceHalfEdges(f, [&](int h) { n += cross(verts_[he_[h].from].p, verts_[to(h)].p); });
    return n * 0.5f;
}

bool HalfEdgeMesh::sharesFaceOtherThan(int u, int v, int f0, int f1) const {
    bool shared = false;
    forOutgoing(u, [&](int h) {
        int f = he_[h].face;
        if (shared || f < 0 || f == f0 || f == f1) return;
        forFaceHalfEdges(f, [&](int g) { if (he_[g].from == v) shared = true; });
    });
    return shared;
}

// ---------------------------------------------------------------- validation

bool HalfEdgeMesh::validate(std::string* why) const {
    auto fail = [&](const std::string& s) { if (why) *why = s; return false; };
    const int nh = (int)he_.size();
    std::vector<int> outCount(verts_.size(), 0);
    for (int h = 0; h < nh; ++h) {
        const HalfEdge& x = he_[h];
        if (x.from < 0) { if (he_[h ^ 1].from >= 0) return fail("half-edge pair half dead"); continue; }
        if (x.from >= (int)verts_.size() || verts_[x.from].dead) return fail("half-edge from dead vertex");
        if (x.next < 0 || x.prev < 0 || he_[x.next].from < 0 || he_[x.prev].from < 0) return fail("dangling next/prev");
        if (he_[x.next].prev != h || he_[x.prev].next != h) return fail("next/prev not inverse");
        if (he_[x.next].from != he_[h ^ 1].from) return fail("next does not start where h ends");
        if (he_[x.next].face != x.face) return fail("face changes along a loop");
        if (x.face >= 0 && (x.face >= (int)faces_.size() || faces_[x.face].dead)) return fail("half-edge of dead face");
        if (x.from == he_[h ^ 1].from) return fail("edge is a loop (both ends equal)");
        ++outCount[x.from];
    }
    // Loops: length >= 3, no repeated vertex (faces and boundary loops alike).
    std::vector<char> seen(nh, 0);
    std::unordered_set<int> loopVerts;
    for (int h0 = 0; h0 < nh; ++h0) {
        if (he_[h0].from < 0 || seen[h0]) continue;
        int h = h0, n = 0;
        loopVerts.clear();
        do {
            seen[h] = 1; ++n;
            if (!loopVerts.insert(he_[h].from).second) return fail("loop visits a vertex twice");
            h = he_[h].next;
        } while (h != h0 && n <= nh);
        if (n < 3) return fail("loop with fewer than 3 sides");
    }
    for (int f = 0; f < (int)faces_.size(); ++f) {
        if (faces_[f].dead) continue;
        int h = faces_[f].he;
        if (h < 0 || he_[h].from < 0 || he_[h].face != f) return fail("face half-edge mismatch");
    }
    // Vertices: one fan each, boundary half-edge stored first.
    for (int v = 0; v < (int)verts_.size(); ++v) {
        if (verts_[v].dead) continue;
        int h0 = verts_[v].he;
        if (h0 < 0) { if (outCount[v]) return fail("vertex without half-edge has edges"); continue; }
        if (he_[h0].from != v) return fail("vertex half-edge does not start at it");
        int n = 0, boundary = 0, h = h0;
        do { ++n; if (he_[h].face < 0) ++boundary; h = he_[h ^ 1].next; } while (h != h0 && n <= nh);
        if (n != outCount[v]) return fail("non-manifold vertex " + std::to_string(v) + " (several fans)");
        if (boundary > 1) return fail("vertex on two boundary loops");
        if (boundary == 1 && he_[h0].face >= 0) return fail("boundary vertex does not store its boundary half-edge");
    }
    // Single edge per vertex pair.
    std::unordered_set<uint64_t> keys;
    for (int e = 0; e < nh / 2; ++e) {
        if (he_[2 * e].from < 0) continue;
        if (!keys.insert(edgeKey(he_[2 * e].from, he_[2 * e + 1].from)).second) return fail("duplicate edge");
    }
    return true;
}

MeshStats HalfEdgeMesh::stats() const {
    MeshStats s;
    std::vector<int> parent(verts_.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](int x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
    for (int v = 0; v < (int)verts_.size(); ++v) {
        if (verts_[v].dead) continue;
        ++s.vertices;
        if (verts_[v].he < 0) ++s.isolated;
    }
    for (int e = 0; e < (int)he_.size() / 2; ++e) {
        if (he_[2 * e].from < 0) continue;
        ++s.edges;
        if (isBoundaryEdge(e)) ++s.boundaryEdges;
        int a = find(he_[2 * e].from), b = find(he_[2 * e + 1].from);
        if (a != b) parent[a] = b;
    }
    for (int f = 0; f < (int)faces_.size(); ++f) {
        if (faces_[f].dead) continue;
        ++s.faces;
        int d = faceDegree(f);
        if (d == 3) ++s.triangles; else if (d == 4) ++s.quads; else ++s.polygons;
    }
    for (int v = 0; v < (int)verts_.size(); ++v)
        if (!verts_[v].dead && verts_[v].he >= 0 && find(v) == v) ++s.components;
    s.boundaryLoops = boundaryLoops().size();
    s.euler = (long long)(s.vertices - s.isolated) - (long long)s.edges + (long long)s.faces;
    s.genus = (int)((2 * (long long)s.components - (long long)s.boundaryLoops - s.euler) / 2);
    return s;
}

std::vector<std::vector<int>> HalfEdgeMesh::boundaryLoops() const {
    std::vector<std::vector<int>> loops;
    std::vector<char> seen(he_.size(), 0);
    for (int h0 = 0; h0 < (int)he_.size(); ++h0) {
        if (he_[h0].from < 0 || he_[h0].face >= 0 || seen[h0]) continue;
        std::vector<int> loop;
        int h = h0;
        do { seen[h] = 1; loop.push_back(h); h = he_[h].next; } while (h != h0 && loop.size() <= he_.size());
        loops.push_back(std::move(loop));
    }
    return loops;
}

// ---------------------------------------------------------------- edge flip

bool HalfEdgeMesh::canFlip(int e) const {
    int h = 2 * e, t = h + 1;
    if (he_[h].from < 0 || he_[h].face < 0 || he_[t].face < 0) return false;
    if (faceDegree(he_[h].face) != 3 || faceDegree(he_[t].face) != 3) return false;
    int c = to(next(h)), d = to(next(t));
    if (c == d || findHalfEdge(c, d) >= 0) return false;
    // Both ends lose an edge; an interior vertex must keep at least 3.
    for (int v : {from(h), from(t)})
        if (valence(v) <= (isBoundaryVertex(v) ? 2 : 3)) return false;
    return true;
}

bool HalfEdgeMesh::flipEdge(int e) {
    if (!canFlip(e)) return false;
    int h = 2 * e, t = h + 1;
    int h1 = next(h), h2 = next(h1), t1 = next(t), t2 = next(t1);
    int a = from(h), b = from(t), c = from(h2), d = from(t2);
    int f0 = he_[h].face, f1 = he_[t].face;
    // f0 = (d -> c -> a), f1 = (c -> d -> b)
    he_[h].from = d; he_[t].from = c;
    link(h, h2); link(h2, t1); link(t1, h);
    link(t, t2); link(t2, h1); link(h1, t);
    he_[t1].face = f0; he_[h1].face = f1;
    faces_[f0].he = h; faces_[f1].he = t;
    if (verts_[a].he == h) verts_[a].he = t1;
    if (verts_[b].he == t) verts_[b].he = h1;
    return true;
}

// ---------------------------------------------------------------- edge collapse

// Generalised link condition. `allowValence2` lets the apex of a removed triangle drop to
// valence 2: diagonal collapse relies on that and removes the resulting doublet afterwards.
static bool canCollapseImpl(const HalfEdgeMesh& m, int h, bool allowValence2) {
    if (h < 0 || !m.edgeAlive(HalfEdgeMesh::edgeOf(h))) return false;
    int t = h ^ 1, u = m.from(h), v = m.to(h);
    bool bu = m.isBoundaryVertex(u), bv = m.isBoundaryVertex(v);
    bool boundaryEdge = m.isBoundary(h) || m.isBoundary(t);
    if (bu && bv && !boundaryEdge) return false;              // would pinch the surface
    int apex[2] = {-1, -1};
    for (int side = 0; side < 2; ++side) {
        int s = side ? t : h;
        int deg = 0;
        for (int g = s, n = 0; n == 0 || g != s; g = m.next(g), ++n) ++deg;
        if (m.isBoundary(s)) { if (deg <= 3) return false; continue; }
        if (deg != 3) continue;
        int hn = m.next(s), hp = m.prev(s), c = m.to(hn);
        apex[side] = c;
        if (m.isBoundary(hn ^ 1) && m.isBoundary(hp ^ 1)) return false;   // a dangling edge would remain
        int minVal = m.isBoundaryVertex(c) ? 2 : (allowValence2 ? 2 : 3);
        if (m.valence(c) <= minVal) return false;
    }
    if (apex[0] >= 0 && apex[0] == apex[1]) return false;      // two triangles on the same three vertices
    // Common neighbours of u and v must be exactly the apexes of the triangles that vanish.
    bool ok = true;
    m.forOutgoing(u, [&](int gu) {
        if (!ok) return;
        int x = m.to(gu);
        if (x == v || x == apex[0] || x == apex[1]) return;
        m.forOutgoing(v, [&](int gv) { if (m.to(gv) == x) ok = false; });
    });
    if (!ok) return false;
    // A face holding both u and v without the edge between them would fold onto itself.
    int fh = m.face(h), ft = m.face(t);
    bool shared = false;
    m.forOutgoing(u, [&](int gu) {
        int f = m.face(gu);
        if (shared || f < 0 || f == fh || f == ft) return;
        m.forFaceHalfEdges(f, [&](int g) { if (m.from(g) == v) shared = true; });
    });
    return !shared;
}

bool HalfEdgeMesh::canCollapse(int h) const { return canCollapseImpl(*this, h, false); }

bool HalfEdgeMesh::collapseEdge(int h, const Vec3& p) {
    if (!canCollapse(h)) return false;
    // Body shared with collapseDiagonal (which validated with its own, looser, rules).
    int t = h ^ 1, u = from(h), v = to(h);
    std::vector<int> outU;
    forOutgoing(u, [&](int g) { outU.push_back(g); });
    std::vector<int> touched;          // vertices whose `he` may need repair, with a live hint
    for (int s : {h, t}) {
        int f = he_[s].face;
        int deg = 0;
        for (int g = s, n = 0; n == 0 || g != s; g = next(g), ++n) ++deg;
        if (f >= 0 && deg == 3) {
            // Triangle vanishes. Keep the edge of hn, delete the edge of hp; hn takes over
            // the place of twin(hp) in the neighbouring loop.
            int hn = next(s), hp = prev(s), o = hp ^ 1, c = from(hp);
            int op = prev(o), on = next(o), of = he_[o].face;
            he_[hn].from = v;
            link(op, hn); link(hn, on);
            he_[hn].face = of;
            if (of >= 0 && faces_[of].he == o) faces_[of].he = hn;
            verts_[c].he = hn ^ 1;
            touched.push_back(c);
            killEdge(hp >> 1);
            killFace(f);
        } else {
            int ps = prev(s), ns = next(s);
            link(ps, ns);
            if (f >= 0 && faces_[f].he == s) faces_[f].he = ns;
        }
    }
    int hint = -1;
    for (int g : outU) {
        if (he_[g].from < 0 || (g >> 1) == (h >> 1)) continue;
        he_[g].from = v;
        hint = g;
    }
    killEdge(h >> 1);
    killVertex(u);
    if (hint < 0) {
        // u had no other edge; find any live half-edge still leaving v.
        for (int s : touched) { int g = verts_[s].he; if (to(g) == v) hint = g ^ 1; }
    }
    verts_[v].he = hint;
    verts_[v].p = p;
    fixVertexHalfEdge(v);
    for (int c : touched) fixVertexHalfEdge(c);
    return true;
}

// ---------------------------------------------------------------- split / join

int HalfEdgeMesh::splitFace(int ha, int hb) {
    int f = he_[ha].face;
    if (f < 0 || ha == hb || he_[hb].face != f || next(ha) == hb || next(hb) == ha) return -1;
    int a = from(ha), b = from(hb);
    if (findHalfEdge(a, b) >= 0) return -1;
    int pa = prev(ha), pb = prev(hb);
    int n = newEdge(), mm = n + 1;
    he_[n].from = a; he_[mm].from = b;
    int g = newFace();
    // g = [ha .. pb, m],  f = [hb .. pa, n]
    link(pb, mm); link(mm, ha);
    link(pa, n); link(n, hb);
    for (int x = ha;; x = next(x)) { he_[x].face = g; if (x == mm) break; }
    he_[n].face = f;
    faces_[f].he = n;
    faces_[g].he = mm;
    return n;
}

bool HalfEdgeMesh::canJoin(int e) const {
    int h = 2 * e, t = h + 1;
    if (he_[h].from < 0) return false;
    int f1 = he_[h].face, f2 = he_[t].face;
    if (f1 < 0 || f2 < 0 || f1 == f2) return false;
    bool other = false;
    forFaceHalfEdges(f1, [&](int g) { if (g != h && he_[g ^ 1].face == f2) other = true; });
    if (other) return false;
    // Merged loop must not visit a vertex twice: the two faces may only share the edge's ends.
    int a = from(h), b = from(t);
    std::vector<int> vs;
    faceVertices(f1, vs);
    bool rep = false;
    forFaceHalfEdges(f2, [&](int g) {
        int x = he_[g].from;
        if (x != a && x != b && std::find(vs.begin(), vs.end(), x) != vs.end()) rep = true;
    });
    if (rep) return false;
    return valence(a) >= 3 && valence(b) >= 3;
}

int HalfEdgeMesh::joinFaces(int e) {
    if (!canJoin(e)) return -1;
    int h = 2 * e, t = h + 1;
    int f1 = he_[h].face, f2 = he_[t].face;
    int ph = prev(h), nh = next(h), pt = prev(t), nt = next(t);
    int a = from(h), b = from(t);
    link(ph, nt); link(pt, nh);
    for (int x = nt; x != nh; x = next(x)) he_[x].face = f1;
    faces_[f1].he = nh;
    if (verts_[a].he == h) verts_[a].he = nt;
    if (verts_[b].he == t) verts_[b].he = nh;
    killEdge(e);
    killFace(f2);
    return f1;
}

// ---------------------------------------------------------------- quad operators

bool HalfEdgeMesh::canCollapseDiagonal(int hc) const {
    if (hc < 0 || he_[hc].from < 0) return false;
    int f = he_[hc].face;
    if (f < 0 || faces_[f].dead || faceDegree(f) != 4) return false;
    int h0 = hc, h1 = next(h0), h2 = next(h1), h3 = next(h2);
    int a = from(h0), b = from(h1), c = from(h2), d = from(h3);
    if (isBoundaryVertex(a) && isBoundaryVertex(c)) return false;
    if (findHalfEdge(a, c) >= 0) return false;
    if (sharesFaceOtherThan(a, c, f, f)) return false;
    // b and d each lose one edge: they must not become dangling.
    for (int s : {h0, h2}) {      // corner b sits between h0 and h1, d between h2 and h3
        int x = next(s);
        int vtx = from(x);
        if (isBoundary(s ^ 1) && isBoundary(x ^ 1)) return false;
        if (valence(vtx) < 3) return false;
    }
    // Common neighbours of a and c must be b and d only.
    bool ok = true;
    forOutgoing(a, [&](int ga) {
        int x = to(ga);
        if (!ok || x == b || x == d) return;
        forOutgoing(c, [&](int gc) { if (to(gc) == x) ok = false; });
    });
    return ok;
}

int HalfEdgeMesh::collapseDiagonal(int hc, const Vec3& p) {
    if (!canCollapseDiagonal(hc)) return -1;
    int a = from(hc);
    int h2 = next(next(hc));
    int n = splitFace(hc, h2);                 // a -> c, two triangles (a,b,c) and (a,c,d)
    if (n < 0) return -1;
    int back = n ^ 1;                          // c -> a: collapsing it keeps a
    if (!canCollapseImpl(*this, back, true)) { joinFaces(n >> 1); return -1; }
    // Same body as collapseEdge, run with the relaxed check above.
    int t = back ^ 1, u = from(back), v = to(back);
    (void)t; (void)u;
    std::vector<int> outU;
    forOutgoing(u, [&](int g) { outU.push_back(g); });
    std::vector<int> touched;
    for (int s : {back, back ^ 1}) {
        int f = he_[s].face;
        int hn = next(s), hp = prev(s), o = hp ^ 1, c = from(hp);
        int op = prev(o), on = next(o), of = he_[o].face;
        he_[hn].from = v;
        link(op, hn); link(hn, on);
        he_[hn].face = of;
        if (of >= 0 && faces_[of].he == o) faces_[of].he = hn;
        verts_[c].he = hn ^ 1;
        touched.push_back(c);
        killEdge(hp >> 1);
        killFace(f);
    }
    int hint = -1;
    for (int g : outU) {
        if (he_[g].from < 0 || (g >> 1) == (back >> 1)) continue;
        he_[g].from = v;
        hint = g;
    }
    killEdge(back >> 1);
    killVertex(u);
    verts_[v].he = hint;
    verts_[v].p = p;
    fixVertexHalfEdge(v);
    for (int c : touched) fixVertexHalfEdge(c);
    return a;
}

bool HalfEdgeMesh::canRotate(int e, int dir) const {
    int h = 2 * e, t = h + 1;
    if (he_[h].from < 0 || he_[h].face < 0 || he_[t].face < 0 || he_[h].face == he_[t].face) return false;
    if (faceDegree(he_[h].face) != 4 || faceDegree(he_[t].face) != 4) return false;
    int h1 = next(h), h2 = next(h1), t1 = next(t), t2 = next(t1);
    int p = dir > 0 ? to(h1) : to(h2);        // x1 or x2
    int q = dir > 0 ? to(t1) : to(t2);        // y1 or y2
    if (p == q || findHalfEdge(p, q) >= 0) return false;
    // The hexagon must be a simple loop (the two quads share only this edge).
    std::vector<int> a, b;
    faceVertices(he_[h].face, a);
    faceVertices(he_[t].face, b);
    int shared = 0;
    for (int x : a) if (std::find(b.begin(), b.end(), x) != b.end()) ++shared;
    if (shared != 2) return false;
    for (int v : {from(h), from(t)})
        if (valence(v) <= (isBoundaryVertex(v) ? 2 : 3)) return false;
    return true;
}

bool HalfEdgeMesh::rotateEdge(int e, int dir) {
    if (!canRotate(e, dir)) return false;
    int h = 2 * e, t = h + 1;
    int h1 = next(h), h2 = next(h1), h3 = next(h2);
    int t1 = next(t), t2 = next(t1), t3 = next(t2);
    int u = from(h), v = from(t);
    int f1 = he_[h].face, f2 = he_[t].face;
    // Hexagon v x1 x2 u y1 y2 (h1 h2 h3 t1 t2 t3); the new edge joins x1-y1 or x2-y2.
    if (dir > 0) {
        he_[h].from = to(t1); he_[t].from = to(h1);           // h: y1 -> x1, t: x1 -> y1
        link(h2, h3); link(h3, t1); link(t1, h); link(h, h2);
        link(t2, t3); link(t3, h1); link(h1, t); link(t, t2);
        he_[t1].face = f1; he_[h1].face = f2;
    } else {
        he_[h].from = to(t2); he_[t].from = to(h2);           // h: y2 -> x2, t: x2 -> y2
        link(h3, t1); link(t1, t2); link(t2, h); link(h, h3);
        link(t3, h1); link(h1, h2); link(h2, t); link(t, t3);
        he_[t1].face = f1; he_[t2].face = f1; he_[h1].face = f2; he_[h2].face = f2;
    }
    faces_[f1].he = h; faces_[f2].he = t;
    if (verts_[u].he == h) verts_[u].he = t1;
    if (verts_[v].he == t) verts_[v].he = h1;
    return true;
}

bool HalfEdgeMesh::isDoublet(int v) const {
    return !verts_[v].dead && verts_[v].he >= 0 && !isBoundaryVertex(v) && valence(v) == 2;
}

bool HalfEdgeMesh::removeDoublet(int v) {
    if (!isDoublet(v)) return false;
    int a1 = verts_[v].he, b1 = he_[a1 ^ 1].next;          // v -> x, v -> y
    int a2 = a1 ^ 1, b2 = b1 ^ 1;
    int F1 = he_[a1].face, F2 = he_[b1].face;
    if (F1 == F2 || he_[b2].face != F1 || he_[a2].face != F2) return false;
    if (faceDegree(F1) + faceDegree(F2) - 4 < 3) return false;
    int x = from(a2), y = from(b2);
    // The merged loop must not repeat a vertex.
    std::vector<int> s1;
    faceVertices(F1, s1);
    bool rep = false;
    forFaceHalfEdges(F2, [&](int g) {
        int w = he_[g].from;
        if (w != v && w != x && w != y && std::find(s1.begin(), s1.end(), w) != s1.end()) rep = true;
    });
    if (rep) return false;
    int pb2 = prev(b2), na1 = next(a1), pa2 = prev(a2), nb1 = next(b1);
    link(pb2, nb1);
    link(pa2, na1);
    for (int g = nb1; g != na1; g = next(g)) he_[g].face = F1;
    faces_[F1].he = na1;
    if (verts_[x].he == a2) verts_[x].he = na1;
    if (verts_[y].he == b2) verts_[y].he = nb1;
    killEdge(a1 >> 1); killEdge(b1 >> 1);
    killFace(F2);
    killVertex(v);
    return true;
}

bool HalfEdgeMesh::isSinglet(int v) const {
    return !verts_[v].dead && verts_[v].he >= 0 && valence(v) == 1;
}

bool HalfEdgeMesh::removeSinglet(int v) {
    if (!isSinglet(v)) return false;
    int h = verts_[v].he, t = h ^ 1;                     // h: v -> x, t: x -> v, same loop
    int L = he_[h].face, x = from(t);
    int deg = 0;
    for (int g = h, n = 0; n == 0 || g != h; g = next(g), ++n) ++deg;
    if (deg < 4) return false;
    int pt = prev(t), nh = next(h);
    link(pt, nh);
    if (L >= 0 && (faces_[L].he == h || faces_[L].he == t)) faces_[L].he = nh;
    if (verts_[x].he == t) verts_[x].he = nh;
    killEdge(h >> 1);
    killVertex(v);
    if (L >= 0 && deg - 2 == 2) {
        // Folded face reduced to two sides x->y->x: replace it by one edge.
        int g1 = nh, g2 = next(g1);
        int o2 = g2 ^ 1, y = from(g2);
        int op = prev(o2), on = next(o2), of = he_[o2].face;
        if (op == g1 || on == g1) return true;            // pathological; leave the 2-gon (validate will flag)
        link(op, g1); link(g1, on);
        he_[g1].face = of;
        if (of >= 0 && faces_[of].he == o2) faces_[of].he = g1;
        verts_[x].he = g1;
        verts_[y].he = g1 ^ 1;
        killEdge(g2 >> 1);
        killFace(L);
        fixVertexHalfEdge(x); fixVertexHalfEdge(y);
    } else {
        fixVertexHalfEdge(x);
    }
    return true;
}

// ---------------------------------------------------------------- whole mesh

void HalfEdgeMesh::garbageCollect() {
    std::vector<int> vmap(verts_.size(), -1), hmap(he_.size(), -1), fmap(faces_.size(), -1);
    std::vector<Vertex> nv; std::vector<HalfEdge> nh; std::vector<Face> nf;
    for (size_t v = 0; v < verts_.size(); ++v) if (!verts_[v].dead) { vmap[v] = (int)nv.size(); nv.push_back(verts_[v]); }
    for (size_t h = 0; h < he_.size(); h += 2)
        if (he_[h].from >= 0) { hmap[h] = (int)nh.size(); hmap[h + 1] = (int)nh.size() + 1; nh.push_back(he_[h]); nh.push_back(he_[h + 1]); }
    for (size_t f = 0; f < faces_.size(); ++f) if (!faces_[f].dead) { fmap[f] = (int)nf.size(); nf.push_back(faces_[f]); }
    for (auto& v : nv) v.he = v.he >= 0 ? hmap[v.he] : -1;
    for (auto& h : nh) { h.from = vmap[h.from]; h.next = hmap[h.next]; h.prev = hmap[h.prev]; h.face = h.face >= 0 ? fmap[h.face] : -1; }
    for (auto& f : nf) f.he = hmap[f.he];
    verts_ = std::move(nv); he_ = std::move(nh); faces_ = std::move(nf);
    liveFaces_ = faces_.size();
}

void HalfEdgeMesh::triangulate() {
    for (int f = 0; f < (int)faces_.size(); ++f) {
        if (faces_[f].dead) continue;
        int guard = 0;
        while (faceDegree(f) > 3 && ++guard < 1000) {
            int h0 = faces_[f].he;
            if (faceDegree(f) == 4) {
                // Shorter diagonal gives the better pair of triangles.
                int h1 = next(h0), h2 = next(h1), h3 = next(h2);
                float d02 = length(position(from(h0)) - position(from(h2)));
                float d13 = length(position(from(h1)) - position(from(h3)));
                if (d02 <= d13 ? splitFace(h0, h2) < 0 && splitFace(h1, h3) < 0
                               : splitFace(h1, h3) < 0 && splitFace(h0, h2) < 0) break;
                continue;
            }
            // Fan: cut one ear (h, next(h)) off, trying each corner until one diagonal is free.
            bool cut = false;
            int h = h0;
            do {
                if (splitFace(h, next(next(h))) >= 0) { cut = true; break; }
                h = next(h);
            } while (h != h0);
            if (!cut) break;
        }
    }
}

size_t fillHoles(HalfEdgeMesh& m) {
    auto loops = m.boundaryLoops();
    if (loops.empty()) return 0;
    std::vector<Vec3> pos;
    std::vector<std::vector<int>> faces;
    // Compacted ids match the order of live vertices, so build the same remap here.
    std::vector<int> remap(m.vertexSlots(), -1);
    int n = 0;
    for (int v = 0; v < m.vertexSlots(); ++v) if (m.vertexAlive(v)) remap[v] = n++;
    m.toPolygons(pos, faces);
    for (const auto& loop : loops) {
        Vec3 c;
        for (int h : loop) c += m.position(m.from(h));
        c = c / (float)loop.size();
        int ci = (int)pos.size();
        pos.push_back(c);
        // A boundary half-edge a -> b has the hole on its left: triangle (a, b, centre).
        for (int h : loop) faces.push_back({remap[m.from(h)], remap[m.to(h)], ci});
    }
    m = HalfEdgeMesh::fromPolygons(pos, faces);
    return loops.size();
}

} // namespace glue
