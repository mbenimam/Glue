#pragma once
// Uniform spatial hash grid for fixed-radius neighbour queries (SPH, boids, collision broad phase).
//
// Space is cut into cubes of side `cellSize` (= the kernel radius h for SPH, so every neighbour
// within h lies in the 3x3x3 block of cells around a point). Cells are not stored densely: the
// integer cell (ix,iy,iz) is hashed into a table of 2^k buckets, so unbounded domains cost memory
// proportional to the particle count only. Build is a counting sort by bucket:
//   1. bucket of every point, count points per bucket
//   2. exclusive prefix sum of the counts -> start of each bucket in the sorted array
//   3. scatter point indices into their bucket's range
// Two different cells can share a bucket (hash collision); queries filter by distance anyway,
// and visit each distinct bucket once so no point is reported twice.
#include "math/Math.h"
#include <cstdint>
#include <vector>

namespace glue {

class HashGrid {
public:
    // Same hash as the GPU port (uint arithmetic, wraps on overflow): keep the two in sync.
    static uint32_t hashCell(int ix, int iy, int iz, uint32_t mask) {
        return (((uint32_t)ix * 73856093u) ^ ((uint32_t)iy * 19349663u) ^ ((uint32_t)iz * 83492791u)) & mask;
    }
    static int cellCoord(float x, float invCell) { return (int)std::floor(x * invCell); }
    // Table size used for n points: next power of two >= 2n (load factor <= 0.5), at least 1024.
    static uint32_t tableSizeFor(size_t n);

    // Rebuilds the grid over `n` points. O(n + table size).
    void build(const Vec3* points, size_t n, float cellSize);

    // Calls f(j) for every point j with |points[j] - x| <= radius (radius <= cellSize), including
    // the point itself if x is one of the indexed points. Uses the positions as they were at build().
    template <class F>
    void forEachNeighbor(const Vec3& x, float radius, F&& f) const {
        uint32_t buckets[27];
        int nb = neighborBuckets(x, buckets);
        float r2 = radius * radius;
        for (int b = 0; b < nb; ++b) {
            uint32_t s = start_[buckets[b]], e = start_[buckets[b] + 1];
            for (uint32_t k = s; k < e; ++k) {
                Vec3 d = sortedPoints_[k] - x;     // contiguous within a bucket: cache friendly
                if (dot(d, d) <= r2) f(sorted_[k]);
            }
        }
    }
    // Convenience wrapper that collects the indices (unordered).
    void query(const Vec3& x, float radius, std::vector<uint32_t>& out) const;

    // Raw arrays, e.g. to upload to the GPU or to iterate points bucket by bucket.
    const std::vector<uint32_t>& bucketStart() const { return start_; }   // size tableSize()+1
    const std::vector<uint32_t>& sortedIndices() const { return sorted_; }
    uint32_t tableSize() const { return mask_ + 1; }
    float cellSize() const { return cell_; }

private:
    // Distinct buckets of the 27 cells around x; returns how many (<= 27).
    int neighborBuckets(const Vec3& x, uint32_t out[27]) const;

    float cell_ = 1.0f, inv_ = 1.0f;
    uint32_t mask_ = 0;
    std::vector<uint32_t> bucketOf_, start_, sorted_;
    std::vector<Vec3> sortedPoints_;   // points in bucket order
};

} // namespace glue
