#pragma once
// Quad mesh simplification by local operators, uniform and adaptive, plus a triangle QEM
// baseline to compare against. Algorithm and invariants: docs/theory-quadmesh.md.
//
//   M  (input triangles) --> ReferenceSurface: grid for closest points, normals, area
//                        --> Fitmaps: per-vertex S (how fast a plane fit degrades = local detail)
//                                     and M (largest radius over which the surface is a height field)
//   M  --> trianglesToQuads (QuadMesh.h) --> M0 quads
//   loop until the target quad count:
//       pop the quad with the smallest priority      uniform: |shortest diagonal|
//                                                    adaptive: |d| * (S(phi(centre)) + b)
//       adaptive only: skip it when its radius exceeds the M-fitmap there
//       diagonal collapse (midpoint, projected on M) and cleaning (doublets, singlets)
//       edge rotations that lower sum |valence - ideal| over the hexagon
//       tangent-space relaxation of the faces around the new vertex, projected back on M
//       re-key the quads whose geometry changed
//
// Invariants kept after every iteration: the half-edge mesh validates (manifold, oriented,
// one edge per vertex pair), the Euler characteristic, component and boundary loop counts
// never change, boundary vertices never move, every vertex lies on M, and no face around a
// collapse turns its normal by more than 90 degrees.
#include <vector>
#include "geometry/HalfEdgeMesh.h"
#include "geometry/SpatialGrid.h"

namespace glue {

// The original surface, used for projection and for measuring errors.
class ReferenceSurface {
public:
    struct Projection {
        Vec3 point, normal;
        int triangle = -1;
        float bary[3] = {0, 0, 0};
        float distance = 0;
    };
    void build(const HalfEdgeMesh& m);            // polygons are fan-triangulated, vertex ids kept
    Projection project(const Vec3& p) const;
    // Linear interpolation of a per-vertex field at a projected point.
    float sample(const std::vector<float>& field, const Projection& pr) const;

    std::vector<Vec3> positions, normals;         // per reference vertex
    std::vector<uint32_t> triangles;
    TriangleGrid grid;
    float area = 0, meanEdge = 0, bboxRadius = 0;
    Vec3 bboxLo, bboxHi;
};

struct Fitmaps {
    std::vector<float> scale;       // S-fitmap: sqrt(a) of the fit RMS(r) ~ a r^2
    std::vector<float> maxRadius;   // M-fitmap: largest radius with < tau inconsistent normals
    std::vector<float> radii;       // the radii r_0 .. r_h that were sampled
    double ms = 0;
};
// levels = h (radii r_0 = mean edge length .. r_h = bounding-box radius, geometric spacing).
// Neighbourhoods are taken from at most `maxNeighbours` vertices (uniform subsample) and the
// work is spread over all hardware threads.
Fitmaps computeFitmaps(const ReferenceSurface& ref, int levels = 8, float tau = 0.05f, int maxNeighbours = 4000);

struct QuadSimplifyOptions {
    int targetQuads = 500;
    bool adaptive = false;          // priority |d| * S, plus the M-fitmap projectability test
    bool rotations = true;
    bool smoothing = true;
    int smoothIterations = 2;       // per local relaxation
    float smoothStrength = 0.5f;    // lambda of each relaxation step
    int finalSmoothing = 3;         // global relaxation passes at the end
    float flatBias = 0.15f;         // adaptive: S + flatBias * mean(S), so flat areas still get sorted by size
    float maxRadiusScale = 1.0f;    // adaptive: collapse allowed when face radius <= scale * M(c)
};

struct QuadSimplifyStats {
    size_t quadsIn = 0, quadsOut = 0, trianglesOut = 0;
    int collapses = 0, refused = 0, blockedByFitmap = 0;
    int gateRelaxations = 0;        // times the M-fitmap test was widened (x1.5) to reach the target
    int rotations = 0, doublets = 0, singlets = 0;
    double ms = 0;
};

// `m` must be a quad-dominant mesh over the same surface as `ref` (normally the output of
// trianglesToQuads). `fit` is required when opt.adaptive is set.
QuadSimplifyStats simplifyQuadMesh(HalfEdgeMesh& m, const ReferenceSurface& ref, const Fitmaps* fit,
                                   const QuadSimplifyOptions& opt);

// ---- baseline: Garland-Heckbert quadric error simplification of a triangle mesh
struct TriQemStats {
    size_t facesIn = 0, facesOut = 0;
    int collapses = 0;
    double ms = 0;
};
TriQemStats simplifyTrianglesQEM(HalfEdgeMesh& m, size_t targetFaces);

// ---- measurements
struct SurfaceDistance {
    double hausdorff = 0;           // max of both one-sided maxima
    double meanAB = 0, meanBA = 0;  // mean distance simplified -> reference and back
    double rms = 0;                 // rms of both directions together
    double diagonal = 0;            // bounding-box diagonal of the reference, for relative numbers
};
// Area-uniform samples on both surfaces (vertices included), nearest points through grids.
SurfaceDistance surfaceDistance(const HalfEdgeMesh& simplified, const ReferenceSurface& ref, int samples = 60000);
// Max distance to the reference per face (face slot indexed), from a few samples per face.
std::vector<float> faceErrors(const HalfEdgeMesh& m, const ReferenceSurface& ref, int samplesPerFace = 6);
// Homeometry: mean over edges of (|e| - mu)^2 and diagonals of (|d| - sqrt2 mu)^2, divided by mu^2,
// with mu = sqrt(area / quads).
double homeometryEnergy(const HalfEdgeMesh& m);

} // namespace glue
