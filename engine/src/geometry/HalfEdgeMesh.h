#pragma once
// Half-edge mesh for polygon surfaces (triangles, quads, mixed).
//
// Every edge owns two half-edges stored next to each other, so twin(h) = h ^ 1 and
// edge(h) = h / 2. A half-edge knows the vertex it starts from, the face on its left and
// the next / previous half-edge around that face. Holes are closed by "boundary loops":
// half-edges with face == -1 linked into cycles just like faces, so walking around a
// boundary vertex never falls off the mesh.
//
// Invariants (checked by validate()):
//   * next(prev(h)) == h, twin(twin(h)) == h, from(next(h)) == to(h)
//   * every face loop has >= 3 half-edges (boundary loops too), and no vertex twice
//   * the half-edges leaving a vertex form ONE fan reachable by rotation (manifold vertex)
//   * at most one edge between any pair of vertices
//   * a boundary vertex stores a boundary half-edge as its `he` (convention used by rotation)
//
// Editing operators (collapse, flip, split, join, rotate, doublet / singlet removal) keep these
// invariants or refuse to act. Removed elements are only flagged; garbageCollect() compacts.
#include <cstdint>
#include <string>
#include <vector>
#include "math/Math.h"

namespace glue {

struct MeshData;

struct BuildReport {
    size_t facesIn = 0, facesKept = 0;
    size_t rejectedDegenerate = 0;     // repeated vertex or fewer than 3 corners
    size_t rejectedNonManifold = 0;    // would give an edge with > 2 faces or a flipped duplicate
    size_t flippedFaces = 0;           // re-oriented to agree with their neighbours
    size_t splitVertices = 0;          // extra copies made for vertices where several fans touch
    bool orientable = true;            // false when some component is a Moebius-like surface
};

struct MeshStats {
    size_t vertices = 0, edges = 0, faces = 0, isolated = 0;
    size_t triangles = 0, quads = 0, polygons = 0;   // polygons = faces with > 4 corners
    size_t boundaryLoops = 0, boundaryEdges = 0, components = 0;
    long long euler = 0;               // V - E + F
    int genus = 0;                     // from chi = 2c - 2g - b (meaningful when valid)
};

class HalfEdgeMesh {
public:
    struct Vertex { Vec3 p; int he = -1; bool dead = false; };
    struct HalfEdge { int from = -1, face = -1, next = -1, prev = -1; };
    struct Face { int he = -1; bool dead = false; };

    // ---- construction ----
    // Faces are vertex index lists of any size >= 3. Inconsistent orientation is repaired
    // by flipping faces (BFS per component); faces that would break manifoldness are skipped.
    static HalfEdgeMesh fromPolygons(const std::vector<Vec3>& positions,
                                     const std::vector<std::vector<int>>& faces,
                                     BuildReport* report = nullptr);
    static HalfEdgeMesh fromMeshData(const MeshData& m, BuildReport* report = nullptr);
    // Live faces as index lists over compacted vertex ids.
    void toPolygons(std::vector<Vec3>& positions, std::vector<std::vector<int>>& faces) const;
    // Fan-triangulated copy for rendering / export (flat-shaded normals are computed by the caller).
    MeshData toMeshData() const;

    // ---- element access ----
    static int twin(int h) { return h ^ 1; }
    static int edgeOf(int h) { return h >> 1; }
    int next(int h) const { return he_[h].next; }
    int prev(int h) const { return he_[h].prev; }
    int from(int h) const { return he_[h].from; }
    int to(int h) const { return he_[h ^ 1].from; }
    int face(int h) const { return he_[h].face; }
    bool isBoundary(int h) const { return he_[h].face < 0; }
    bool isBoundaryEdge(int e) const { return he_[2 * e].face < 0 || he_[2 * e + 1].face < 0; }
    bool isBoundaryVertex(int v) const { return verts_[v].he >= 0 && he_[verts_[v].he].face < 0; }
    bool edgeAlive(int e) const { return he_[2 * e].from >= 0; }
    bool faceAlive(int f) const { return !faces_[f].dead; }
    bool vertexAlive(int v) const { return !verts_[v].dead; }
    const Vec3& position(int v) const { return verts_[v].p; }
    void setPosition(int v, const Vec3& p) { verts_[v].p = p; }
    int vertexHalfEdge(int v) const { return verts_[v].he; }
    int faceHalfEdge(int f) const { return faces_[f].he; }

    // Slot counts (include removed elements; test the alive flags while iterating).
    int vertexSlots() const { return (int)verts_.size(); }
    int edgeSlots() const { return (int)he_.size() / 2; }
    int faceSlots() const { return (int)faces_.size(); }
    int halfEdgeSlots() const { return (int)he_.size(); }
    size_t liveFaces() const { return liveFaces_; }

    // ---- traversal ----
    // Outgoing half-edges of v in rotation order, starting at its `he` (the boundary one, if any).
    template <class F> void forOutgoing(int v, F&& fn) const {
        int h0 = verts_[v].he;
        if (h0 < 0) return;
        int h = h0, guard = 0;
        do {
            fn(h);
            h = he_[h ^ 1].next;                     // twin comes back into v, its next leaves v again
        } while (h != h0 && ++guard < (1 << 20));
    }
    // Half-edges of a face loop, starting at its `he`.
    template <class F> void forFaceHalfEdges(int f, F&& fn) const {
        int h0 = faces_[f].he, h = h0, guard = 0;
        do { fn(h); h = he_[h].next; } while (h != h0 && ++guard < (1 << 20));
    }
    int valence(int v) const;
    int faceDegree(int f) const;
    void faceVertices(int f, std::vector<int>& out) const;
    int findHalfEdge(int a, int b) const;          // a -> b, or -1
    Vec3 faceCentroid(int f) const;
    Vec3 faceVectorArea(int f) const;              // Newell: normal * area, works for non-planar polygons

    // ---- checks ----
    bool validate(std::string* why = nullptr) const;
    MeshStats stats() const;

    // ---- editing operators ----
    // Flip the diagonal shared by two triangles. Refuses boundary edges, non-triangles and
    // flips that would duplicate an existing edge.
    bool canFlip(int e) const;
    bool flipEdge(int e);

    // Collapse half-edge h (u -> v): u disappears, v moves to `p`. Works on any polygon:
    // a triangle on either side vanishes (its two other edges are glued), larger faces lose a corner.
    // canCollapse() is the generalised link condition; collapseEdge() calls it first.
    bool canCollapse(int h) const;
    bool collapseEdge(int h, const Vec3& p);

    // Insert the diagonal between the origins of two half-edges of the same face. Returns the
    // new half-edge going ha.from -> hb.from, or -1 (adjacent corners, edge already exists).
    int splitFace(int ha, int hb);
    // Remove edge e, merging its two faces. Refuses boundary edges, equal faces, and faces
    // sharing more than this one edge.
    bool canJoin(int e) const;
    int joinFaces(int e);                          // returns surviving face or -1

    // Quad operators (see docs/theory-quadmesh.md).
    // Diagonal collapse: merge the corners from(hc) and from(next(next(hc))) of a quad.
    // The survivor keeps the id of from(hc) and moves to p. Returns the survivor or -1.
    bool canCollapseDiagonal(int hc) const;
    int collapseDiagonal(int hc, const Vec3& p);
    // Edge rotation: e is shared by two quads forming a hexagon; replace e by the hexagon
    // diagonal turned one step clockwise (dir = +1) or counter-clockwise (dir = -1).
    bool canRotate(int e, int dir) const;
    bool rotateEdge(int e, int dir);
    // Doublet: interior vertex of valence 2. The two faces are merged and v removed.
    bool isDoublet(int v) const;
    bool removeDoublet(int v);
    // Singlet: vertex of valence 1 (a folded face). The dangling edge and v are removed and,
    // when the face is left with 2 sides, the face is replaced by a single edge.
    bool isSinglet(int v) const;
    bool removeSinglet(int v);

    // ---- whole-mesh helpers ----
    void garbageCollect();
    // Splits every polygon into triangles (best-shaped diagonal for quads, fans otherwise).
    void triangulate();
    std::vector<std::vector<int>> boundaryLoops() const;   // half-edge ids per loop

private:
    int newVertex(const Vec3& p);
    int newEdge();                                 // returns the first of two half-edges
    int newFace();
    void killEdge(int e);
    void killFace(int f);
    void killVertex(int v);
    void fixVertexHalfEdge(int v);                 // pick a live outgoing half-edge, prefer boundary
    void link(int a, int b) { he_[a].next = b; he_[b].prev = a; }
    bool sharesFaceOtherThan(int u, int v, int f0, int f1) const;

    std::vector<Vertex> verts_;
    std::vector<HalfEdge> he_;
    std::vector<Face> faces_;
    size_t liveFaces_ = 0;
};

// Close each boundary loop with a fan of triangles around a new centroid vertex.
// Returns the number of holes filled.
size_t fillHoles(HalfEdgeMesh& m);

} // namespace glue
