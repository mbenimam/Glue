#include "geometry/Differential.h"

namespace prism {

static float cotAngle(const Vec3& a, const Vec3& b) {      // cot of the angle between a and b
    float s = length(cross(a, b));
    return dot(a, b) / std::max(s, 1e-12f);
}
static float angleBetween(const Vec3& a, const Vec3& b) {
    return std::atan2(length(cross(a, b)), dot(a, b));
}

std::vector<Vec3> faceNormals(const HalfEdgeMesh& m) {
    std::vector<Vec3> n(m.faceSlots());
    for (int f = 0; f < m.faceSlots(); ++f)
        if (m.faceAlive(f)) n[f] = normalize(m.faceVectorArea(f));
    return n;
}

std::vector<float> faceAreas(const HalfEdgeMesh& m) {
    std::vector<float> a(m.faceSlots(), 0.0f);
    for (int f = 0; f < m.faceSlots(); ++f)
        if (m.faceAlive(f)) a[f] = length(m.faceVectorArea(f));
    return a;
}

std::vector<Vec3> vertexNormals(const HalfEdgeMesh& m) {
    std::vector<Vec3> n(m.vertexSlots());
    for (int f = 0; f < m.faceSlots(); ++f) {
        if (!m.faceAlive(f)) continue;
        Vec3 fn = normalize(m.faceVectorArea(f));
        m.forFaceHalfEdges(f, [&](int h) {
            int v = m.from(h);
            Vec3 a = m.position(m.to(h)) - m.position(v);
            Vec3 b = m.position(m.from(m.prev(h))) - m.position(v);
            n[v] += fn * angleBetween(a, b);
        });
    }
    for (auto& x : n) x = normalize(x);
    return n;
}

std::vector<float> cotanWeights(const HalfEdgeMesh& m) {
    std::vector<float> w(m.edgeSlots(), 0.0f);
    for (int h = 0; h < m.halfEdgeSlots(); ++h) {
        if (!m.edgeAlive(h >> 1) || m.isBoundary(h)) continue;
        // In triangle (i, j, k) with h = i -> j, the angle opposite the edge sits at k.
        int k = m.from(m.prev(h));
        const Vec3& pk = m.position(k);
        w[h >> 1] += 0.5f * cotAngle(m.position(m.from(h)) - pk, m.position(m.to(h)) - pk);
    }
    return w;
}

std::vector<float> mixedAreas(const HalfEdgeMesh& m) {
    std::vector<float> A(m.vertexSlots(), 0.0f);
    for (int f = 0; f < m.faceSlots(); ++f) {
        if (!m.faceAlive(f)) continue;
        int h0 = m.faceHalfEdge(f), h1 = m.next(h0), h2 = m.next(h1);
        int v[3] = {m.from(h0), m.from(h1), m.from(h2)};
        Vec3 p[3] = {m.position(v[0]), m.position(v[1]), m.position(v[2])};
        float area = 0.5f * length(cross(p[1] - p[0], p[2] - p[0]));
        int obtuse = -1;
        for (int i = 0; i < 3; ++i)
            if (dot(p[(i + 1) % 3] - p[i], p[(i + 2) % 3] - p[i]) < 0) obtuse = i;
        for (int i = 0; i < 3; ++i) {
            if (obtuse < 0) {
                // Voronoi part: (|PR|^2 cot Q + |PQ|^2 cot R) / 8 with P = corner i.
                const Vec3 &P = p[i], &Q = p[(i + 1) % 3], &R = p[(i + 2) % 3];
                float cotQ = cotAngle(P - Q, R - Q), cotR = cotAngle(P - R, Q - R);
                A[v[i]] += (dot(R - P, R - P) * cotQ + dot(Q - P, Q - P) * cotR) / 8.0f;
            } else {
                A[v[i]] += obtuse == i ? area * 0.5f : area * 0.25f;
            }
        }
    }
    return A;
}

std::vector<Vec3> meanCurvatureNormals(const HalfEdgeMesh& m) {
    auto w = cotanWeights(m);
    auto A = mixedAreas(m);
    std::vector<Vec3> K(m.vertexSlots());
    for (int e = 0; e < m.edgeSlots(); ++e) {
        if (!m.edgeAlive(e)) continue;
        int i = m.from(2 * e), j = m.from(2 * e + 1);
        Vec3 d = (m.position(i) - m.position(j)) * (2.0f * w[e]);   // 2 w = cot a + cot b
        K[i] += d; K[j] -= d;
    }
    for (int v = 0; v < m.vertexSlots(); ++v) {
        if (!m.vertexAlive(v) || A[v] <= 0 || m.isBoundaryVertex(v)) { K[v] = {}; continue; }
        K[v] = K[v] / (2.0f * A[v]);
    }
    return K;
}

std::vector<float> angleDefects(const HalfEdgeMesh& m) {
    std::vector<float> d(m.vertexSlots(), 0.0f);
    for (int v = 0; v < m.vertexSlots(); ++v)
        if (m.vertexAlive(v) && m.vertexHalfEdge(v) >= 0) d[v] = m.isBoundaryVertex(v) ? kPi : 2.0f * kPi;
    for (int f = 0; f < m.faceSlots(); ++f) {
        if (!m.faceAlive(f)) continue;
        m.forFaceHalfEdges(f, [&](int h) {
            int v = m.from(h);
            d[v] -= angleBetween(m.position(m.to(h)) - m.position(v), m.position(m.from(m.prev(h))) - m.position(v));
        });
    }
    return d;
}

Curvature computeCurvature(const HalfEdgeMesh& m) {
    Curvature c;
    c.area = mixedAreas(m);
    c.normal = vertexNormals(m);
    auto K = meanCurvatureNormals(m);
    auto defect = angleDefects(m);
    int n = m.vertexSlots();
    c.mean.assign(n, 0); c.gauss.assign(n, 0); c.k1.assign(n, 0); c.k2.assign(n, 0);
    for (int v = 0; v < n; ++v) {
        if (!m.vertexAlive(v) || c.area[v] <= 0) continue;
        // On a convex bump x_v sticks out of its neighbours, so K points along the outward
        // normal there: the sign of H is the sign of K . n.
        float H = 0.5f * length(K[v]);
        if (dot(K[v], c.normal[v]) < 0) H = -H;
        if (m.isBoundaryVertex(v)) H = 0;
        float G = m.isBoundaryVertex(v) ? 0.0f : defect[v] / c.area[v];
        float disc = std::sqrt(std::max(H * H - G, 0.0f));
        c.mean[v] = H; c.gauss[v] = G; c.k1[v] = H + disc; c.k2[v] = H - disc;
    }
    return c;
}

template <class WeightFn>
static void smoothImpl(HalfEdgeMesh& m, int iterations, float lambda, WeightFn weight) {
    std::vector<Vec3> next(m.vertexSlots());
    for (int it = 0; it < iterations; ++it) {
        std::vector<float> w = weight();
        for (int v = 0; v < m.vertexSlots(); ++v) {
            next[v] = m.vertexAlive(v) ? m.position(v) : Vec3{};
            if (!m.vertexAlive(v) || m.vertexHalfEdge(v) < 0 || m.isBoundaryVertex(v)) continue;
            Vec3 sum; float ws = 0;
            m.forOutgoing(v, [&](int h) {
                float wi = w.empty() ? 1.0f : std::max(w[h >> 1], 0.0f);   // clamp: obtuse pairs give negative weights
                sum += m.position(m.to(h)) * wi; ws += wi;
            });
            if (ws > 0) next[v] = m.position(v) + (sum / ws - m.position(v)) * lambda;
        }
        for (int v = 0; v < m.vertexSlots(); ++v) if (m.vertexAlive(v)) m.setPosition(v, next[v]);
    }
}

void smoothUniform(HalfEdgeMesh& m, int iterations, float lambda) {
    smoothImpl(m, iterations, lambda, [] { return std::vector<float>{}; });
}

void smoothCotan(HalfEdgeMesh& m, int iterations, float lambda) {
    smoothImpl(m, iterations, lambda, [&] { return cotanWeights(m); });
}

} // namespace prism
