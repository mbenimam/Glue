#include "spatial/HashGrid.h"

namespace prism {

uint32_t HashGrid::tableSizeFor(size_t n) {
    uint32_t s = 1024;
    while (s < 2 * n) s <<= 1;
    return s;
}

void HashGrid::build(const Vec3* points, size_t n, float cellSize) {
    cell_ = cellSize;
    inv_ = 1.0f / cellSize;
    mask_ = tableSizeFor(n) - 1;
    bucketOf_.resize(n);
    sorted_.resize(n);
    sortedPoints_.resize(n);
    start_.assign(mask_ + 2, 0);

    // 1. Count: start_[b+1] holds the size of bucket b for now.
    for (size_t i = 0; i < n; ++i) {
        const Vec3& p = points[i];
        uint32_t b = hashCell(cellCoord(p.x, inv_), cellCoord(p.y, inv_), cellCoord(p.z, inv_), mask_);
        bucketOf_[i] = b;
        ++start_[b + 1];
    }
    // 2. Prefix sum turns sizes into starts: bucket b is sorted_[start_[b] .. start_[b+1]).
    for (size_t b = 1; b < start_.size(); ++b) start_[b] += start_[b - 1];
    // 3. Scatter, filling each bucket from its start. A cursor copy keeps start_ intact.
    std::vector<uint32_t> cursor(start_.begin(), start_.end() - 1);
    for (size_t i = 0; i < n; ++i) {
        uint32_t k = cursor[bucketOf_[i]]++;
        sorted_[k] = (uint32_t)i;
        sortedPoints_[k] = points[i];
    }
}

int HashGrid::neighborBuckets(const Vec3& x, uint32_t out[27]) const {
    int cx = cellCoord(x.x, inv_), cy = cellCoord(x.y, inv_), cz = cellCoord(x.z, inv_);
    int n = 0;
    uint64_t maybe = 0;   // 64-bit filter on the low bits: the full scan runs only on a filter hit
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                uint32_t b = hashCell(cx + dx, cy + dy, cz + dz, mask_);
                uint64_t bit = 1ull << (b & 63u);
                bool seen = false;   // two cells hashing to one bucket: visit it once
                if (maybe & bit)
                    for (int k = 0; k < n && !seen; ++k) seen = out[k] == b;
                maybe |= bit;
                if (!seen) out[n++] = b;
            }
    return n;
}

void HashGrid::query(const Vec3& x, float radius, std::vector<uint32_t>& out) const {
    out.clear();
    forEachNeighbor(x, radius, [&](uint32_t j) { out.push_back(j); });
}

} // namespace prism
