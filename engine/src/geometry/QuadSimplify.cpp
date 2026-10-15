#include "geometry/QuadSimplify.h"
#include "geometry/QuadMesh.h"
#include "scene/MeshData.h"
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <queue>
#include <thread>

namespace glue {

using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ---------------------------------------------------------------- reference surface

void ReferenceSurface::build(const HalfEdgeMesh& m) {
    MeshData md = m.toMeshData();
    positions = md.positions;
    normals = md.normals;
    triangles = md.indices;
    grid.build(positions, triangles);
    area = 0; meanEdge = 0;
    for (size_t t = 0; t < triangles.size(); t += 3) {
        const Vec3 &a = positions[triangles[t]], &b = positions[triangles[t + 1]], &c = positions[triangles[t + 2]];
        area += 0.5f * length(cross(b - a, c - a));
        meanEdge += (length(b - a) + length(c - b) + length(a - c)) / 3.0f;
    }
    if (!triangles.empty()) meanEdge /= (float)(triangles.size() / 3);
    md.bounds(bboxLo, bboxHi);
    bboxRadius = 0.5f * length(bboxHi - bboxLo);
}

ReferenceSurface::Projection ReferenceSurface::project(const Vec3& p) const {
    Projection pr;
    TriangleGrid::Hit h = grid.closest(p);
    if (h.triangle < 0) { pr.point = p; return pr; }
    pr.point = h.point;
    pr.triangle = h.triangle;
    for (int k = 0; k < 3; ++k) pr.bary[k] = h.bary[k];
    const uint32_t* t = &triangles[3 * h.triangle];
    pr.normal = normalize(normals[t[0]] * h.bary[0] + normals[t[1]] * h.bary[1] + normals[t[2]] * h.bary[2]);
    pr.distance = std::sqrt(h.dist2);
    return pr;
}

float ReferenceSurface::sample(const std::vector<float>& f, const Projection& pr) const {
    if (pr.triangle < 0 || f.empty()) return 0.0f;
    const uint32_t* t = &triangles[3 * pr.triangle];
    return f[t[0]] * pr.bary[0] + f[t[1]] * pr.bary[1] + f[t[2]] * pr.bary[2];
}

// ---------------------------------------------------------------- fitmaps

// Smallest eigenpair of a symmetric 3x3 matrix by cyclic Jacobi rotations.
static void smallestEigen(double A[3][3], double& lambda, Vec3& vec) {
    double V[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int sweep = 0; sweep < 12; ++sweep) {
        double off = A[0][1] * A[0][1] + A[0][2] * A[0][2] + A[1][2] * A[1][2];
        if (off < 1e-30) break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q) {
                if (std::fabs(A[p][q]) < 1e-300) continue;
                double theta = (A[q][q] - A[p][p]) / (2 * A[p][q]);
                double t = (theta >= 0 ? 1 : -1) / (std::fabs(theta) + std::sqrt(theta * theta + 1));
                double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 3; ++k) {            // A <- A J
                    double akp = A[k][p], akq = A[k][q];
                    A[k][p] = c * akp - s * akq; A[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {            // A <- J^T A
                    double apk = A[p][k], aqk = A[q][k];
                    A[p][k] = c * apk - s * aqk; A[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    double vkp = V[k][p], vkq = V[k][q];
                    V[k][p] = c * vkp - s * vkq; V[k][q] = s * vkp + c * vkq;
                }
            }
    }
    int i = 0;
    for (int k = 1; k < 3; ++k) if (A[k][k] < A[i][i]) i = k;
    lambda = A[i][i];
    vec = {(float)V[0][i], (float)V[1][i], (float)V[2][i]};
}

Fitmaps computeFitmaps(const ReferenceSurface& ref, int levels, float tau, int maxNeighbours) {
    auto t0 = Clock::now();
    Fitmaps fm;
    const int n = (int)ref.positions.size();
    fm.scale.assign(n, 0.0f);
    fm.maxRadius.assign(n, 0.0f);
    if (n == 0) return fm;
    levels = std::max(levels, 1);
    float r0 = std::max(ref.meanEdge, 1e-6f), rh = std::max(ref.bboxRadius, r0 * 1.01f);
    for (int k = 0; k <= levels; ++k) fm.radii.push_back(r0 * std::pow(rh / r0, (float)k / levels));
    std::vector<float> r2;
    for (float r : fm.radii) r2.push_back(r * r);
    // Neighbourhood points: all vertices, or a uniform subsample of them for big meshes.
    std::vector<int> nb;
    int stride = std::max(1, (n + maxNeighbours - 1) / maxNeighbours);
    for (int j = 0; j < n; j += stride) nb.push_back(j);

    auto work = [&](int begin, int end) {
        const int L = levels + 1;
        std::vector<double> mom(L * 13);                 // per level: count, sum p (3), sum ppT (6), sum n (3)
        std::vector<signed char> bucket(nb.size());
        std::vector<Vec3> planeN(L);
        std::vector<int> bad(L), cnt(L);
        std::vector<double> rms(L);
        for (int v = begin; v < end; ++v) {
            std::fill(mom.begin(), mom.end(), 0.0);
            const Vec3 pv = ref.positions[v];
            for (size_t s = 0; s < nb.size(); ++s) {
                const Vec3& p = ref.positions[nb[s]];
                Vec3 d = p - pv;
                float d2 = dot(d, d);
                if (d2 > r2.back()) { bucket[s] = -1; continue; }
                int k = 0;
                while (d2 > r2[k]) ++k;                  // first radius that contains the point
                bucket[s] = (signed char)k;
                // Moments relative to pv keep the sums well conditioned.
                double* m = &mom[k * 13];
                const Vec3& nn = ref.normals[nb[s]];
                m[0] += 1; m[1] += d.x; m[2] += d.y; m[3] += d.z;
                m[4] += d.x * d.x; m[5] += d.x * d.y; m[6] += d.x * d.z;
                m[7] += d.y * d.y; m[8] += d.y * d.z; m[9] += d.z * d.z;
                m[10] += nn.x; m[11] += nn.y; m[12] += nn.z;
            }
            for (int k = 1; k < L; ++k) for (int i = 0; i < 13; ++i) mom[k * 13 + i] += mom[(k - 1) * 13 + i];
            // Least-squares plane per radius: the smallest covariance eigenvalue is the mean
            // squared distance to the best plane.
            for (int k = 0; k < L; ++k) {
                const double* m = &mom[k * 13];
                cnt[k] = (int)m[0];
                rms[k] = 0;
                planeN[k] = ref.normals[v];
                if (m[0] < 4) continue;
                double inv = 1.0 / m[0];
                double cx = m[1] * inv, cy = m[2] * inv, cz = m[3] * inv;
                double C[3][3] = {{m[4] * inv - cx * cx, m[5] * inv - cx * cy, m[6] * inv - cx * cz},
                                  {0, m[7] * inv - cy * cy, m[8] * inv - cy * cz},
                                  {0, 0, m[9] * inv - cz * cz}};
                C[1][0] = C[0][1]; C[2][0] = C[0][2]; C[2][1] = C[1][2];
                double lambda; Vec3 nrm;
                smallestEigen(C, lambda, nrm);
                rms[k] = std::sqrt(std::max(lambda, 0.0));
                Vec3 avgN{(float)m[10], (float)m[11], (float)m[12]};
                planeN[k] = dot(nrm, avgN) < 0 ? -nrm : nrm;
            }
            // M-fitmap: count neighbours whose normal disagrees with the fitted plane.
            std::fill(bad.begin(), bad.end(), 0);
            for (size_t s = 0; s < nb.size(); ++s) {
                int b = bucket[s];
                if (b < 0) continue;
                const Vec3& nn = ref.normals[nb[s]];
                for (int k = b; k < L; ++k) if (dot(nn, planeN[k]) < 0) ++bad[k];
            }
            int kmax = 0;
            for (int k = 0; k < L; ++k) {
                if (cnt[k] > 0 && bad[k] > tau * cnt[k]) break;
                kmax = k;
            }
            fm.maxRadius[v] = fm.radii[kmax];
            // S-fitmap: RMS(r) ~ a r^2. Each radius up to the M-fitmap radius gives an estimate
            // of a; averaging the ratios weighs every scale equally (a plain least-squares fit
            // would be dominated by the largest radius).
            double a = 0; int na = 0;
            for (int k = 0; k <= std::max(kmax, 2) && k < L; ++k) {
                if (cnt[k] < 6) continue;
                a += rms[k] / ((double)fm.radii[k] * fm.radii[k]);
                ++na;
            }
            if (na == 0)            // sparse subsample: fall back to every radius with enough points
                for (int k = 0; k < L; ++k)
                    if (cnt[k] >= 6) { a += rms[k] / ((double)fm.radii[k] * fm.radii[k]); ++na; }
            fm.scale[v] = na ? (float)std::sqrt(a / na) : 0.0f;
        }
    };
    unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    int chunk = (n + (int)threads - 1) / (int)threads;
    for (unsigned t = 0; t < threads; ++t) {
        int b = (int)t * chunk, e = std::min(n, b + chunk);
        if (b < e) pool.emplace_back(work, b, e);
    }
    for (auto& th : pool) th.join();
    fm.ms = msSince(t0);
    return fm;
}

// ---------------------------------------------------------------- quad simplification

namespace {

struct HeapItem {
    float key;
    int face;
    uint32_t stamp;
    bool operator<(const HeapItem& o) const { return key > o.key; }   // min-heap
};

class QuadSimplifier {
public:
    QuadSimplifier(HalfEdgeMesh& m, const ReferenceSurface& ref, const Fitmaps* fit, const QuadSimplifyOptions& opt)
        : m_(m), ref_(ref), fit_(fit), opt_(opt) {}

    QuadSimplifyStats run() {
        auto t0 = Clock::now();
        for (int f = 0; f < m_.faceSlots(); ++f) {
            if (!m_.faceAlive(f)) continue;
            int d = m_.faceDegree(f);
            if (d == 4) ++quads_; else ++nonQuads_;
        }
        st_.quadsIn = quads_;
        if (opt_.adaptive && fit_ && !fit_->scale.empty()) {
            double s = 0;
            for (float x : fit_->scale) s += x;
            meanScale_ = (float)(s / fit_->scale.size());
        }
        if (opt_.adaptive && fit_) {
            bias_ = std::max(opt_.flatBias * meanScale_, 1e-6f);
            // integral of (S + b)^2 over the reference, one third of each triangle per corner
            for (size_t t = 0; t < ref_.triangles.size(); t += 3) {
                const uint32_t* tr = &ref_.triangles[t];
                const Vec3 &a = ref_.positions[tr[0]], &b = ref_.positions[tr[1]], &c = ref_.positions[tr[2]];
                float area = 0.5f * length(cross(b - a, c - a));
                for (int k = 0; k < 3; ++k) { float x = fit_->scale[tr[k]] + bias_; sizeIntegral_ += area / 3.0f * x * x; }
            }
        }
        updateMu();
        // Every vertex lies on the reference surface; cache its normal and fitmap values there.
        info_.resize(m_.vertexSlots());
        for (int v = 0; v < m_.vertexSlots(); ++v)
            if (m_.vertexAlive(v)) setInfo(v, ref_.project(m_.position(v)));
        for (int f = 0; f < m_.faceSlots(); ++f) push(f);
        int guard = 0, lastCollapses = -1;
        gateScale_ = opt_.maxRadiusScale;
        while ((int)quads_ > opt_.targetQuads && ++guard < 50 * (int)st_.quadsIn + 1000) {
            if (heap_.empty()) {
                // Every remaining candidate was refused. Refusals by the M-fitmap are a
                // preference, not a hard limit: widen the radius test and offer every quad again.
                // Stop when a whole round brings no collapse.
                if (st_.collapses == lastCollapses || st_.gateRelaxations >= 24) break;
                lastCollapses = st_.collapses;
                if (opt_.adaptive) { gateScale_ *= 1.5f; ++st_.gateRelaxations; }
                for (int f = 0; f < m_.faceSlots(); ++f) push(f);
                continue;
            }
            HeapItem it = heap_.top();
            heap_.pop();
            if (it.face >= (int)stamp_.size() || stamp_[it.face] != it.stamp || !m_.faceAlive(it.face)) continue;
            stamp_[it.face]++;                                // consumed; re-pushed when it changes
            int w = collapse(it.face);
            if (w < 0) continue;
            region_.clear();
            clean(w);
            if (m_.vertexAlive(w)) {
                if (opt_.rotations) rotateAround(w);
                if (opt_.smoothing) smoothAround(w);
            }
            updateMu();
            rekeyRegion();
        }
        if (opt_.smoothing && opt_.finalSmoothing > 0) {
            std::vector<int> all;
            for (int v = 0; v < m_.vertexSlots(); ++v) if (m_.vertexAlive(v) && m_.vertexHalfEdge(v) >= 0) all.push_back(v);
            for (int k = 0; k < opt_.finalSmoothing; ++k) relax(all);
        }
        for (int f = 0; f < m_.faceSlots(); ++f) {
            if (!m_.faceAlive(f)) continue;
            int d = m_.faceDegree(f);
            if (d == 4) ++st_.quadsOut; else if (d == 3) ++st_.trianglesOut;
        }
        st_.ms = msSince(t0);
        return st_;
    }

private:
    // Ideal quad side. Uniform: mu = sqrt(area / quads). Adaptive: the priority |d| (S + b)
    // levels out when |d| ~ 1 / (S + b), so the ideal side is L(x) = k / (S(x) + b) with k
    // chosen to tile the surface with the current number of quads:
    //   quads = integral dA / L^2 = integral (S + b)^2 dA / k^2.
    void updateMu() {
        float q = (float)std::max<size_t>(quads_, 1);
        mu_ = std::sqrt(ref_.area / q);
        k_ = std::sqrt(sizeIntegral_ / q);
    }
    float idealLength(int v) const {
        if (!opt_.adaptive || !fit_) return mu_;
        return k_ / (info_[v].scale + bias_);
    }

    // Corner positions of a quad, starting at its stored half-edge.
    void corners(int f, int h[4], Vec3 p[4]) const {
        h[0] = m_.faceHalfEdge(f);
        for (int i = 1; i < 4; ++i) h[i] = m_.next(h[i - 1]);
        for (int i = 0; i < 4; ++i) p[i] = m_.position(m_.from(h[i]));
    }

    float priority(int f, int* shortDiag) const {
        int h[4]; Vec3 p[4];
        corners(f, h, p);
        float d0 = length(p[2] - p[0]), d1 = length(p[3] - p[1]);
        *shortDiag = d0 <= d1 ? 0 : 1;
        float key = std::min(d0, d1);
        if (opt_.adaptive && fit_) {
            // S at phi(c), the centre projected on the input surface. The corners already lie on
            // it with their S cached, so S(phi(c)) is taken as the mean of the corner values
            // (linear interpolation over the face). Projecting the centre itself was measured
            // too: no better on the test models and about 1.6x slower.
            float s = 0;
            for (int i = 0; i < 4; ++i) s += 0.25f * info_[m_.from(h[i])].scale;
            key *= s + bias_;
        }
        return key;
    }

    struct VertexInfo { Vec3 normal; float scale = 0, maxRadius = 0; };
    void setInfo(int v, const ReferenceSurface::Projection& pr) {
        VertexInfo& i = info_[v];
        i.normal = pr.normal;
        if (fit_) { i.scale = ref_.sample(fit_->scale, pr); i.maxRadius = ref_.sample(fit_->maxRadius, pr); }
    }

    void push(int f) {
        if ((int)stamp_.size() < m_.faceSlots()) stamp_.resize(m_.faceSlots(), 0);
        if (!m_.faceAlive(f) || m_.faceDegree(f) != 4) return;
        int which;
        float key = priority(f, &which);
        heap_.push({key, f, ++stamp_[f]});
    }

    // Faces around a and c (other than f) must not turn over (normal rotated by more than 90
    // degrees) or vanish when a and c move to p. Concave quads are tolerated here: the collapse
    // of a thin quad in a strip briefly makes its neighbours concave, and the relaxation that
    // follows straightens them. Forbidding it leaves whole strips that can never be coarsened.
    bool geometryOk(int f, int a, int c, const Vec3& p) const {
        bool ok = true;
        Vec3 q[16];
        for (int v : {a, c}) {
            m_.forOutgoing(v, [&](int h) {
                int g = m_.face(h);
                if (!ok || g < 0 || g == f) return;
                int n = 0;
                m_.forFaceHalfEdges(g, [&](int x) {
                    int u = m_.from(x);
                    if (n < 16) q[n++] = (u == a || u == c) ? p : m_.position(u);
                });
                Vec3 before = m_.faceVectorArea(g), after;
                for (int i = 0; i < n; ++i) after += cross(q[i], q[(i + 1) % n]);
                if (dot(after, before) <= 0 || length(after) < 1e-6f * length(before)) ok = false;
            });
        }
        return ok;
    }

    // Diagonal collapse of quad f. Returns the surviving vertex or -1.
    int collapse(int f) {
        if (m_.faceDegree(f) != 4) return -1;
        int h[4]; Vec3 p[4];
        corners(f, h, p);
        int first;
        priority(f, &first);
        Vec3 centre = (p[0] + p[1] + p[2] + p[3]) * 0.25f;
        if (opt_.adaptive && fit_) {
            // Projectability: the face must fit inside the radius over which the input is still a
            // height field around it (smallest M-fitmap value at its centre and corners).
            float radius = 0;
            for (auto& x : p) radius = std::max(radius, length(x - centre));
            float mr = ref_.sample(fit_->maxRadius, ref_.project(centre));
            for (int i = 0; i < 4; ++i) mr = std::min(mr, info_[m_.from(h[i])].maxRadius);
            if (radius > gateScale_ * mr) { ++st_.blockedByFitmap; return -1; }
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            int k = attempt == 0 ? first : 1 - first;
            int hc = h[k];
            int a = m_.from(hc), c = m_.from(h[k + 2]);
            if (!m_.canCollapseDiagonal(hc)) continue;
            bool ba = m_.isBoundaryVertex(a), bc = m_.isBoundaryVertex(c);
            Vec3 target;
            ReferenceSurface::Projection pr;
            if (ba) target = m_.position(a);
            else if (bc) target = m_.position(c);
            else { pr = ref_.project((m_.position(a) + m_.position(c)) * 0.5f); target = pr.point; }
            if (!geometryOk(f, a, c, target)) continue;
            // Keep the boundary vertex id when there is one (collapseDiagonal keeps from(hc)).
            if (bc) hc = h[k + 2];
            int b = m_.from(h[k + 1]), d = m_.from(h[(k + 3) % 4]);
            int w = m_.collapseDiagonal(hc, target);
            if (w < 0) continue;
            if (!ba && !bc) setInfo(w, pr);
            --quads_;
            ++st_.collapses;
            pending_ = {b, d};
            return w;
        }
        ++st_.refused;
        return -1;
    }

    void addRing(int v) {
        if (!m_.vertexAlive(v)) return;
        region_.push_back(v);
        m_.forOutgoing(v, [&](int h) { region_.push_back(m_.to(h)); });
    }

    // Doublets (interior valence 2) and singlets (valence 1) around the collapse.
    void clean(int w) {
        std::vector<int> work = pending_;
        work.push_back(w);
        m_.forOutgoing(w, [&](int h) { work.push_back(m_.to(h)); });
        for (size_t i = 0; i < work.size() && i < 256; ++i) {
            int v = work[i];
            if (!m_.vertexAlive(v)) continue;
            if (m_.isSinglet(v)) {
                int x = m_.to(m_.vertexHalfEdge(v));
                if (m_.removeSinglet(v)) { ++st_.singlets; work.push_back(x); }
                continue;
            }
            if (m_.isDoublet(v)) {
                int h = m_.vertexHalfEdge(v);
                int x = m_.to(h), y = m_.to(m_.next(h ^ 1));
                int fa = m_.face(h), fb = m_.face(h ^ 1);
                bool quadPair = m_.faceDegree(fa) == 4 && m_.faceDegree(fb) == 4;
                bool triQuad = m_.faceDegree(fa) + m_.faceDegree(fb) == 7;
                if (m_.removeDoublet(v)) {
                    ++st_.doublets;
                    if (quadPair || triQuad) --quads_;
                    work.push_back(x); work.push_back(y);
                }
            }
        }
        addRing(w);
        for (int v : pending_) addRing(v);
    }

    int ideal(int v) const { return m_.isBoundaryVertex(v) ? 3 : 4; }
    int irregularity(int v, int delta) const { return std::abs(m_.valence(v) + delta - ideal(v)); }

    // Edge rotations that lower sum |valence - ideal| over the six hexagon vertices.
    void rotateAround(int w) {
        std::vector<int> edges;
        std::vector<int> verts{w};
        m_.forOutgoing(w, [&](int h) { verts.push_back(m_.to(h)); });
        for (int v : verts) m_.forOutgoing(v, [&](int h) { edges.push_back(h >> 1); });
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        for (int e : edges) {
            if (!m_.edgeAlive(e) || m_.isBoundaryEdge(e)) continue;
            int h = 2 * e, t = h + 1;
            if (m_.faceDegree(m_.face(h)) != 4 || m_.faceDegree(m_.face(t)) != 4) continue;
            int u = m_.from(h), v = m_.from(t);
            int x1 = m_.to(m_.next(h)), x2 = m_.to(m_.next(m_.next(h)));
            int y1 = m_.to(m_.next(t)), y2 = m_.to(m_.next(m_.next(t)));
            int base = irregularity(u, 0) + irregularity(v, 0);
            int before[2] = {base + irregularity(x1, 0) + irregularity(y1, 0), base + irregularity(x2, 0) + irregularity(y2, 0)};
            int lose = irregularity(u, -1) + irregularity(v, -1);
            int after[2] = {lose + irregularity(x1, 1) + irregularity(y1, 1), lose + irregularity(x2, 1) + irregularity(y2, 1)};
            // Energy over all six vertices: the two untouched ones cancel in the difference.
            int gain[2] = {before[0] - after[0], before[1] - after[1]};
            int dir = gain[0] >= gain[1] ? 1 : -1;
            int best = std::max(gain[0], gain[1]);
            if (best <= 0) continue;
            if (!rotationGeometryOk(e, dir)) { dir = -dir; if (gain[dir > 0 ? 0 : 1] <= 0 || !rotationGeometryOk(e, dir)) continue; }
            if (m_.rotateEdge(e, dir)) {
                ++st_.rotations;
                region_.push_back(m_.from(h)); region_.push_back(m_.from(t));
            }
        }
    }

    // Both quads after rotation must be convex with respect to their common normal.
    bool rotationGeometryOk(int e, int dir) const {
        int h = 2 * e, t = h + 1;
        int hex[6] = {m_.from(t), m_.to(m_.next(h)), m_.to(m_.next(m_.next(h))),
                      m_.from(h), m_.to(m_.next(t)), m_.to(m_.next(m_.next(t)))};   // v x1 x2 u y1 y2
        int s = dir > 0 ? 1 : 2;               // new diagonal hex[s] - hex[s+3]
        Vec3 n = m_.faceVectorArea(m_.face(h)) + m_.faceVectorArea(m_.face(t));
        for (int part = 0; part < 2; ++part) {
            Vec3 q[4];
            for (int i = 0; i < 4; ++i) q[i] = m_.position(hex[(s + 3 * part + i) % 6]);
            for (int i = 0; i < 4; ++i)
                if (dot(cross(q[(i + 1) % 4] - q[i], q[(i + 3) % 4] - q[i]), n) <= 0) return false;
        }
        return true;
    }

    // Tangent-space relaxation: springs of rest length L along edges and sqrt(2) L along quad
    // diagonals pull v; the move is restricted to the tangent plane and projected back on M.
    void relax(const std::vector<int>& verts) {
        const float sqrt2 = 1.41421356f;
        for (int v : verts) {
            if (!m_.vertexAlive(v) || m_.vertexHalfEdge(v) < 0 || m_.isBoundaryVertex(v)) continue;
            Vec3 x = m_.position(v);
            float Lv = idealLength(v);
            // Each spring's rest length is the mean of the ideal sizes at its two ends.
            auto restLen = [&](int other) { return 0.5f * (Lv + idealLength(other)); };
            Vec3 rest; float wsum = 0;
            m_.forOutgoing(v, [&](int h) {
                int j = m_.to(h);
                Vec3 xj = m_.position(j);
                rest += xj + normalize(x - xj) * restLen(j); wsum += 1;
                int f = m_.face(h);
                if (f >= 0 && m_.faceDegree(f) == 4) {
                    int k = m_.to(m_.next(h));                          // opposite corner of the quad
                    Vec3 xk = m_.position(k);
                    rest += xk + normalize(x - xk) * (sqrt2 * restLen(k)); wsum += 1;
                }
            });
            if (wsum == 0) continue;
            rest = rest / wsum;
            const Vec3& n = info_[v].normal;
            Vec3 move = (rest - x) * opt_.smoothStrength;
            move -= n * dot(move, n);
            auto pr = ref_.project(x + move);
            if (moveOk(v, pr.point)) { m_.setPosition(v, pr.point); setInfo(v, pr); }
        }
    }

    // Worst corner of a polygon, 2 |e1||e2| sin(theta) / (|e1|^2 + |e2|^2) signed by `normal`.
    static float worstCorner(const Vec3* q, int n, const Vec3& normal) {
        float w = 1.0f;
        for (int i = 0; i < n; ++i) {
            Vec3 e1 = q[(i + 1) % n] - q[i], e2 = q[(i + n - 1) % n] - q[i];
            float l = dot(e1, e1) + dot(e2, e2);
            w = std::min(w, l > 0 ? 2.0f * dot(cross(e1, e2), normal) / l : 0.0f);
        }
        return w;
    }

    // A relaxation move is kept when no face around v turns over and the worst corner either
    // stays decent or improves: bad quads must be allowed to get better.
    bool moveOk(int v, const Vec3& p) const {
        bool ok = true;
        float worstBefore = 1.0f, worstAfter = 1.0f;
        m_.forOutgoing(v, [&](int h) {
            int f = m_.face(h);
            if (!ok || f < 0) return;
            Vec3 q[16], q0[16]; int n = 0;
            m_.forFaceHalfEdges(f, [&](int x) {
                if (n >= 16) return;
                q0[n] = m_.position(m_.from(x));
                q[n] = m_.from(x) == v ? p : q0[n];
                ++n;
            });
            Vec3 after;
            for (int i = 0; i < n; ++i) after += cross(q[i], q[(i + 1) % n]);
            Vec3 before = normalize(m_.faceVectorArea(f));
            if (dot(normalize(after), before) < 0.3f) { ok = false; return; }
            worstBefore = std::min(worstBefore, worstCorner(q0, n, before));
            worstAfter = std::min(worstAfter, worstCorner(q, n, before));
        });
        return ok && (worstAfter > 0.1f || worstAfter >= worstBefore);
    }

    void smoothAround(int w) {
        std::vector<int> verts;
        verts.push_back(w);
        m_.forOutgoing(w, [&](int h) {
            int f = m_.face(h);
            if (f >= 0) m_.forFaceHalfEdges(f, [&](int x) { verts.push_back(m_.from(x)); });
        });
        std::sort(verts.begin(), verts.end());
        verts.erase(std::unique(verts.begin(), verts.end()), verts.end());
        for (int k = 0; k < opt_.smoothIterations; ++k) relax(verts);
        region_.insert(region_.end(), verts.begin(), verts.end());
    }

    void rekeyRegion() {
        std::vector<int> faces;
        for (int v : region_) {
            if (!m_.vertexAlive(v)) continue;
            m_.forOutgoing(v, [&](int h) { if (m_.face(h) >= 0) faces.push_back(m_.face(h)); });
        }
        std::sort(faces.begin(), faces.end());
        faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
        for (int f : faces) push(f);
    }

    HalfEdgeMesh& m_;
    const ReferenceSurface& ref_;
    const Fitmaps* fit_;
    QuadSimplifyOptions opt_;
    QuadSimplifyStats st_;
    std::priority_queue<HeapItem> heap_;
    std::vector<uint32_t> stamp_;
    std::vector<int> region_, pending_;
    std::vector<VertexInfo> info_;
    size_t quads_ = 0, nonQuads_ = 0;
    float mu_ = 1.0f, meanScale_ = 0.0f, k_ = 1.0f, bias_ = 1.0f, gateScale_ = 1.0f;
    double sizeIntegral_ = 0;
};

} // namespace

QuadSimplifyStats simplifyQuadMesh(HalfEdgeMesh& m, const ReferenceSurface& ref, const Fitmaps* fit,
                                   const QuadSimplifyOptions& opt) {
    QuadSimplifier s(m, ref, fit, opt);
    return s.run();
}

// ---------------------------------------------------------------- triangle QEM baseline

namespace {

// Symmetric 4x4 quadric (Garland and Heckbert 1997), upper triangle stored row by row.
struct Quadric {
    double a[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    void addPlane(const Vec3& n, double d, double w) {
        double v[4] = {n.x, n.y, n.z, d};
        int k = 0;
        for (int i = 0; i < 4; ++i)
            for (int j = i; j < 4; ++j) a[k++] += w * v[i] * v[j];
    }
    Quadric operator+(const Quadric& o) const { Quadric r; for (int i = 0; i < 10; ++i) r.a[i] = a[i] + o.a[i]; return r; }
    double eval(const Vec3& p) const {
        double x = p.x, y = p.y, z = p.z;
        return a[0] * x * x + 2 * a[1] * x * y + 2 * a[2] * x * z + 2 * a[3] * x + a[4] * y * y + 2 * a[5] * y * z +
               2 * a[6] * y + a[7] * z * z + 2 * a[8] * z + a[9];
    }
    // Minimiser of the quadric: solve A x = -b. Returns false when A is near singular.
    bool optimum(Vec3& out) const {
        double A[3][3] = {{a[0], a[1], a[2]}, {a[1], a[4], a[5]}, {a[2], a[5], a[7]}};
        double b[3] = {-a[3], -a[6], -a[8]};
        double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
                     A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
        double scale = std::fabs(A[0][0]) + std::fabs(A[1][1]) + std::fabs(A[2][2]);
        if (std::fabs(det) < 1e-9 * scale * scale * scale || scale == 0) return false;
        double x[3];
        for (int c = 0; c < 3; ++c) {
            double M[3][3];
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) M[i][j] = j == c ? b[i] : A[i][j];
            x[c] = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                    M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) / det;
        }
        out = {(float)x[0], (float)x[1], (float)x[2]};
        return true;
    }
};

struct EdgeItem {
    double cost;
    int e;
    uint32_t stamp;
    bool operator<(const EdgeItem& o) const { return cost > o.cost; }
};

} // namespace

TriQemStats simplifyTrianglesQEM(HalfEdgeMesh& m, size_t targetFaces) {
    auto t0 = Clock::now();
    TriQemStats st;
    st.facesIn = m.liveFaces();
    std::vector<Quadric> Q(m.vertexSlots());
    for (int f = 0; f < m.faceSlots(); ++f) {
        if (!m.faceAlive(f)) continue;
        Vec3 va = m.faceVectorArea(f);
        float area = length(va);
        if (area <= 0) continue;
        Vec3 n = va / area;
        double d = -dot(n, m.position(m.from(m.faceHalfEdge(f))));
        m.forFaceHalfEdges(f, [&](int h) { Q[m.from(h)].addPlane(n, d, area); });
    }
    // Boundary edges: a plane through the edge, perpendicular to its face, keeps the outline.
    for (int h = 0; h < m.halfEdgeSlots(); ++h) {
        if (!m.edgeAlive(h >> 1) || !m.isBoundary(h)) continue;
        int in = h ^ 1;
        Vec3 a = m.position(m.from(h)), b = m.position(m.to(h));
        Vec3 fn = normalize(m.faceVectorArea(m.face(in)));
        Vec3 n = normalize(cross(b - a, fn));
        double w = 100.0 * dot(b - a, b - a);
        Q[m.from(h)].addPlane(n, -dot(n, a), w);
        Q[m.to(h)].addPlane(n, -dot(n, a), w);
    }
    std::vector<uint32_t> stamp(m.edgeSlots(), 0);
    std::priority_queue<EdgeItem> heap;
    auto place = [&](int e, Vec3& p) {
        int u = m.from(2 * e), v = m.from(2 * e + 1);
        Quadric q = Q[u] + Q[v];
        Vec3 cands[3] = {m.position(u), m.position(v), (m.position(u) + m.position(v)) * 0.5f};
        double best;
        if (q.optimum(p)) best = q.eval(p);
        else { p = cands[2]; best = q.eval(p); }
        for (auto& c : cands) { double x = q.eval(c); if (x < best) { best = x; p = c; } }
        return std::max(best, 0.0);
    };
    auto push = [&](int e) {
        if (!m.edgeAlive(e)) return;
        Vec3 p;
        heap.push({place(e, p), e, ++stamp[e]});
    };
    for (int e = 0; e < m.edgeSlots(); ++e) push(e);
    // Normal-flip test: faces around u and v (except the two that vanish) keep their orientation.
    auto flips = [&](int h, const Vec3& p) {
        int u = m.from(h), v = m.to(h);
        int f0 = m.face(h), f1 = m.face(h ^ 1);
        bool bad = false;
        for (int x : {u, v})
            m.forOutgoing(x, [&](int g) {
                int f = m.face(g);
                if (bad || f < 0 || f == f0 || f == f1) return;
                Vec3 q[3]; int n = 0;
                m.forFaceHalfEdges(f, [&](int k) { int w = m.from(k); if (n < 3) q[n++] = (w == u || w == v) ? p : m.position(w); });
                Vec3 after = cross(q[1] - q[0], q[2] - q[0]);
                Vec3 before = m.faceVectorArea(f);
                if (dot(normalize(after), normalize(before)) < 0.2f) bad = true;
            });
        return bad;
    };
    while (m.liveFaces() > targetFaces && !heap.empty()) {
        EdgeItem it = heap.top();
        heap.pop();
        if (!m.edgeAlive(it.e) || stamp[it.e] != it.stamp) continue;
        Vec3 p;
        place(it.e, p);
        int h = 2 * it.e;
        if (!m.canCollapse(h)) h ^= 1;
        if (!m.canCollapse(h) || flips(h, p)) continue;
        int keep = m.to(h), gone = m.from(h);
        Quadric q = Q[keep] + Q[gone];
        if (!m.collapseEdge(h, p)) continue;
        Q[keep] = q;
        ++st.collapses;
        m.forOutgoing(keep, [&](int g) { push(g >> 1); });
    }
    st.facesOut = m.liveFaces();
    st.ms = msSince(t0);
    return st;
}

// ---------------------------------------------------------------- measurements

namespace {
struct Rng {
    uint32_t s = 12345u;
    float uniform() { s = s * 1664525u + 1013904223u; return (float)(s >> 8) / 16777216.0f; }
};

// Area-uniform points on the triangles of `md` plus all its vertices.
std::vector<Vec3> samplePoints(const MeshData& md, int count) {
    std::vector<Vec3> pts(md.positions.begin(), md.positions.end());
    std::vector<double> cdf;
    double total = 0;
    for (size_t t = 0; t < md.indices.size(); t += 3) {
        const Vec3 &a = md.positions[md.indices[t]], &b = md.positions[md.indices[t + 1]], &c = md.positions[md.indices[t + 2]];
        total += 0.5 * length(cross(b - a, c - a));
        cdf.push_back(total);
    }
    if (total <= 0) return pts;
    Rng rng;
    for (int i = 0; i < count; ++i) {
        double r = rng.uniform() * total;
        size_t t = std::lower_bound(cdf.begin(), cdf.end(), r) - cdf.begin();
        t = std::min(t, cdf.size() - 1);
        float u = rng.uniform(), v = rng.uniform();
        if (u + v > 1) { u = 1 - u; v = 1 - v; }
        const Vec3 &a = md.positions[md.indices[3 * t]], &b = md.positions[md.indices[3 * t + 1]], &c = md.positions[md.indices[3 * t + 2]];
        pts.push_back(a + (b - a) * u + (c - a) * v);
    }
    return pts;
}
} // namespace

SurfaceDistance surfaceDistance(const HalfEdgeMesh& simplified, const ReferenceSurface& ref, int samples) {
    SurfaceDistance d;
    d.diagonal = length(ref.bboxHi - ref.bboxLo);
    MeshData simp = simplified.toMeshData();
    TriangleGrid simpGrid;
    simpGrid.build(simp.positions, simp.indices);
    MeshData refMd;
    refMd.positions = ref.positions;
    refMd.indices = ref.triangles;
    double sum2 = 0;
    size_t n = 0;
    double maxAB = 0, maxBA = 0;
    for (const Vec3& p : samplePoints(simp, samples)) {
        double x = std::sqrt(ref.grid.closest(p).dist2);
        d.meanAB += x; maxAB = std::max(maxAB, x); sum2 += x * x; ++n;
    }
    size_t nAB = n;
    for (const Vec3& p : samplePoints(refMd, samples)) {
        double x = std::sqrt(simpGrid.closest(p).dist2);
        d.meanBA += x; maxBA = std::max(maxBA, x); sum2 += x * x; ++n;
    }
    d.meanAB /= std::max<size_t>(nAB, 1);
    d.meanBA /= std::max<size_t>(n - nAB, 1);
    d.hausdorff = std::max(maxAB, maxBA);
    d.rms = std::sqrt(sum2 / std::max<size_t>(n, 1));
    return d;
}

std::vector<float> faceErrors(const HalfEdgeMesh& m, const ReferenceSurface& ref, int samplesPerFace) {
    std::vector<float> err(m.faceSlots(), 0.0f);
    Rng rng;
    std::vector<Vec3> p;
    for (int f = 0; f < m.faceSlots(); ++f) {
        if (!m.faceAlive(f)) continue;
        p.clear();
        m.forFaceHalfEdges(f, [&](int h) { p.push_back(m.position(m.from(h))); });
        float e = 0;
        // Corners, the centroid and random points of the fan triangles.
        Vec3 c;
        for (auto& x : p) { e = std::max(e, std::sqrt(ref.grid.closest(x).dist2)); c += x; }
        e = std::max(e, std::sqrt(ref.grid.closest(c / (float)p.size()).dist2));
        for (int s = 0; s < samplesPerFace; ++s) {
            size_t t = 1 + (size_t)(rng.uniform() * (p.size() - 2));
            t = std::min(t, p.size() - 2);
            float u = rng.uniform(), v = rng.uniform();
            if (u + v > 1) { u = 1 - u; v = 1 - v; }
            Vec3 q = p[0] + (p[t] - p[0]) * u + (p[t + 1] - p[0]) * v;
            e = std::max(e, std::sqrt(ref.grid.closest(q).dist2));
        }
        err[f] = e;
    }
    return err;
}

double homeometryEnergy(const HalfEdgeMesh& m) {
    double area = 0;
    size_t quads = 0;
    for (int f = 0; f < m.faceSlots(); ++f) {
        if (!m.faceAlive(f)) continue;
        area += length(m.faceVectorArea(f));
        if (m.faceDegree(f) == 4) ++quads;
    }
    if (!quads) return 0;
    double mu = std::sqrt(area / quads), e = 0;
    size_t n = 0;
    for (int ed = 0; ed < m.edgeSlots(); ++ed) {
        if (!m.edgeAlive(ed)) continue;
        double l = length(m.position(m.from(2 * ed)) - m.position(m.from(2 * ed + 1)));
        e += (l - mu) * (l - mu); ++n;
    }
    for (int f = 0; f < m.faceSlots(); ++f) {
        if (!m.faceAlive(f) || m.faceDegree(f) != 4) continue;
        int h = m.faceHalfEdge(f);
        Vec3 p[4];
        for (int i = 0; i < 4; ++i, h = m.next(h)) p[i] = m.position(m.from(h));
        for (int k = 0; k < 2; ++k) {
            double l = length(p[k + 2] - p[k]);
            e += (l - std::sqrt(2.0) * mu) * (l - std::sqrt(2.0) * mu); ++n;
        }
    }
    return e / (n * mu * mu);
}

} // namespace glue
