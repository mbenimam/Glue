#pragma once
// Triangle mesh -> quad mesh, keeping every original vertex and (almost) every original edge.
//
// Step 1, pairing (Pietroni et al. 2010): an interior edge between two triangles is a candidate
// quad. Score it by squareness, then visit edges from best to worst and flag an edge when neither
// of its triangles already has a flagged edge: at most one flagged edge per triangle, so every
// triangle joins at most one quad. Removing the flagged edges gives a quad-dominant mesh.
//
// Step 2, crawling: the leftover triangles are walked towards each other. A breadth-first search
// over faces finds the nearest other triangle; along that path the triangle swaps places with
// each quad it crosses (triangle + quad = pentagon, re-split so that the triangle sits on the far
// side), until the two triangles touch and merge into a quad. Each step moves one diagonal; the
// face count never grows. A closed component always has an even number of triangles, so it ends
// as a pure quad mesh; with boundaries one triangle per component may remain.
//
// Compare with one Catmull-Clark step: also all quads, but 3x as many faces and every original
// vertex becomes irregular.
#include "geometry/HalfEdgeMesh.h"

namespace glue {

struct QuadConvertOptions {
    bool crawl = true;
    int maxCrawlDepth = 64;          // BFS radius (in faces) when looking for a partner triangle
};

struct QuadConvertStats {
    size_t trianglesIn = 0;
    size_t pairedQuads = 0;          // after step 1
    size_t crawledPairs = 0;         // quads created by step 2
    size_t quads = 0, triangles = 0, others = 0;
    double percentQuads = 0;         // quads / faces * 100
    double ms = 0;
};

// Squareness of a quad in [0, 1]: 1 - mean |cos| of its four corner angles (1 for rectangles).
float quadSquareness(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d);

// Shape quality of a face in [-1, 1], 1 = ideal. Quads: min over corners of
// 2 |e1| |e2| sin(theta) / (|e1|^2 + |e2|^2), signed by the face normal, so it drops with
// skewed angles, with stretched corners, and turns negative for folded (non-convex) quads.
// Triangles: 4 sqrt(3) area / sum of squared edge lengths (1 for equilateral).
float faceQuality(const HalfEdgeMesh& m, int f);

QuadConvertStats trianglesToQuads(HalfEdgeMesh& m, const QuadConvertOptions& opt = {});

struct QuadQualityStats {
    size_t faces = 0, quads = 0, triangles = 0, others = 0;
    double percentQuads = 0;
    double meanQuality = 0, minQuality = 0;      // faceQuality over quads
    double meanAngleDeviation = 0;               // mean |corner angle - 90| in degrees, quads only
    double percentRegular = 0;                   // interior valence 4 / boundary valence 3
};
QuadQualityStats measureQuads(const HalfEdgeMesh& m);

} // namespace glue
