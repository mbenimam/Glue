#include "geometry/SpatialGrid.h"
#include <cfloat>

namespace prism {

Vec3 closestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c, float w[3]) {
    // Classify p against the Voronoi regions of the vertices, edges and face of the triangle.
    Vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) { w[0] = 1; w[1] = w[2] = 0; return a; }
    Vec3 bp = p - b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) { w[1] = 1; w[0] = w[2] = 0; return b; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        float t = d1 / (d1 - d3);
        w[0] = 1 - t; w[1] = t; w[2] = 0;
        return a + ab * t;
    }
    Vec3 cp = p - c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) { w[2] = 1; w[0] = w[1] = 0; return c; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        float t = d2 / (d2 - d6);
        w[0] = 1 - t; w[2] = t; w[1] = 0;
        return a + ac * t;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        float t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        w[1] = 1 - t; w[2] = t; w[0] = 0;
        return b + (c - b) * t;
    }
    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom, u = vc * denom;
    w[0] = 1 - v - u; w[1] = v; w[2] = u;
    return a + ab * v + ac * u;
}

void TriangleGrid::build(const std::vector<Vec3>& pos, const std::vector<uint32_t>& tris, float perCell) {
    pos_ = pos;
    tris_ = tris;
    size_t nt = tris.size() / 3;
    stamp_.assign(nt, 0);
    query_ = 0;
    if (pos.empty() || nt == 0) return;
    lo_ = hi_ = pos[0];
    for (const auto& p : pos) {
        lo_ = {std::min(lo_.x, p.x), std::min(lo_.y, p.y), std::min(lo_.z, p.z)};
        hi_ = {std::max(hi_.x, p.x), std::max(hi_.y, p.y), std::max(hi_.z, p.z)};
    }
    Vec3 ext = hi_ - lo_;
    float pad = std::max({ext.x, ext.y, ext.z, 1e-6f}) * 1e-3f;
    lo_ -= Vec3{pad, pad, pad}; hi_ += Vec3{pad, pad, pad};
    ext = hi_ - lo_;
    // About nt / perCell cells in the box. A surface only crosses a fraction of them, so the
    // occupied cells hold a handful of triangles each. At most 128 cells per axis.
    float target = std::max(1.0f, (float)nt / std::max(perCell, 0.1f));
    float volume = std::max(ext.x * ext.y * ext.z, 1e-12f);
    cell_ = std::cbrt(volume / target);
    cell_ = std::max(cell_, std::max({ext.x, ext.y, ext.z}) / 128.0f);
    nx_ = std::max(1, (int)std::ceil(ext.x / cell_));
    ny_ = std::max(1, (int)std::ceil(ext.y / cell_));
    nz_ = std::max(1, (int)std::ceil(ext.z / cell_));
    std::vector<uint32_t> count((size_t)nx_ * ny_ * nz_ + 1, 0);
    auto cellRange = [&](size_t t, int lo[3], int hi[3]) {
        Vec3 a = pos[tris[3 * t]], b = pos[tris[3 * t + 1]], c = pos[tris[3 * t + 2]];
        Vec3 mn{std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}), std::min({a.z, b.z, c.z})};
        Vec3 mx{std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y}), std::max({a.z, b.z, c.z})};
        int n[3] = {nx_, ny_, nz_};
        float l[3] = {mn.x - lo_.x, mn.y - lo_.y, mn.z - lo_.z}, h[3] = {mx.x - lo_.x, mx.y - lo_.y, mx.z - lo_.z};
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::clamp((int)(l[k] / cell_), 0, n[k] - 1);
            hi[k] = std::clamp((int)(h[k] / cell_), 0, n[k] - 1);
        }
    };
    int lo[3], hi[3];
    for (size_t t = 0; t < nt; ++t) {
        cellRange(t, lo, hi);
        for (int z = lo[2]; z <= hi[2]; ++z)
            for (int y = lo[1]; y <= hi[1]; ++y)
                for (int x = lo[0]; x <= hi[0]; ++x) ++count[cellIndex(x, y, z) + 1];
    }
    for (size_t i = 1; i < count.size(); ++i) count[i] += count[i - 1];
    cellStart_ = count;
    cellItems_.assign(count.back(), 0);
    std::vector<uint32_t> fill(count.begin(), count.end() - 1);
    for (size_t t = 0; t < nt; ++t) {
        cellRange(t, lo, hi);
        for (int z = lo[2]; z <= hi[2]; ++z)
            for (int y = lo[1]; y <= hi[1]; ++y)
                for (int x = lo[0]; x <= hi[0]; ++x) cellItems_[fill[cellIndex(x, y, z)]++] = (uint32_t)t;
    }
}

TriangleGrid::Hit TriangleGrid::closest(const Vec3& q) const {
    Hit best;
    best.dist2 = FLT_MAX;
    if (tris_.empty()) return best;
    if (++query_ == 0) { std::fill(stamp_.begin(), stamp_.end(), 0); query_ = 1; }
    int c[3] = {std::clamp((int)((q.x - lo_.x) / cell_), 0, nx_ - 1),
                std::clamp((int)((q.y - lo_.y) / cell_), 0, ny_ - 1),
                std::clamp((int)((q.z - lo_.z) / cell_), 0, nz_ - 1)};
    int maxR = std::max({nx_, ny_, nz_});
    float w[3];
    for (int r = 0; r <= maxR; ++r) {
        for (int z = c[2] - r; z <= c[2] + r; ++z) {
            if (z < 0 || z >= nz_) continue;
            for (int y = c[1] - r; y <= c[1] + r; ++y) {
                if (y < 0 || y >= ny_) continue;
                for (int x = c[0] - r; x <= c[0] + r; ++x) {
                    if (x < 0 || x >= nx_) continue;
                    // Only the shell of the block: the inside was visited at smaller r.
                    if (std::max({std::abs(x - c[0]), std::abs(y - c[1]), std::abs(z - c[2])}) != r) continue;
                    int ci = cellIndex(x, y, z);
                    for (uint32_t k = cellStart_[ci]; k < cellStart_[ci + 1]; ++k) {
                        uint32_t t = cellItems_[k];
                        if (stamp_[t] == query_) continue;
                        stamp_[t] = query_;
                        Vec3 p = closestPointOnTriangle(q, pos_[tris_[3 * t]], pos_[tris_[3 * t + 1]], pos_[tris_[3 * t + 2]], w);
                        Vec3 d = p - q;
                        float d2 = dot(d, d);
                        if (d2 < best.dist2) {
                            best.dist2 = d2; best.point = p; best.triangle = (int)t;
                            best.bary[0] = w[0]; best.bary[1] = w[1]; best.bary[2] = w[2];
                        }
                    }
                }
            }
        }
        // Distance from q to the outside of the visited block of cells.
        float exitDist = FLT_MAX;
        float qc[3] = {q.x - lo_.x, q.y - lo_.y, q.z - lo_.z};
        int n[3] = {nx_, ny_, nz_};
        bool coversAll = true;
        for (int k = 0; k < 3; ++k) {
            int blo = c[k] - r, bhi = c[k] + r;
            if (blo > 0) { exitDist = std::min(exitDist, qc[k] - blo * cell_); coversAll = false; }
            if (bhi < n[k] - 1) { exitDist = std::min(exitDist, (bhi + 1) * cell_ - qc[k]); coversAll = false; }
        }
        if (coversAll) break;
        if (best.triangle >= 0 && exitDist > 0 && best.dist2 <= exitDist * exitDist) break;
    }
    return best;
}

} // namespace prism
