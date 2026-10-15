#include "scene/MeshData.h"
#include "core/File.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace prism {

void MeshData::computeNormals() {
    normals.assign(positions.size(), Vec3{});
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        uint32_t a = indices[t], b = indices[t + 1], c = indices[t + 2];
        // The cross product's length is twice the triangle area, which gives the area weighting.
        Vec3 n = cross(positions[b] - positions[a], positions[c] - positions[a]);
        normals[a] += n; normals[b] += n; normals[c] += n;
    }
    for (auto& n : normals) n = normalize(n);
}

void MeshData::bounds(Vec3& lo, Vec3& hi) const {
    if (positions.empty()) { lo = hi = Vec3{}; return; }
    lo = hi = positions[0];
    for (const auto& p : positions) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
}

void MeshData::normalizeToUnitBox() {
    Vec3 lo, hi; bounds(lo, hi);
    Vec3 c = (lo + hi) * 0.5f, e = hi - lo;
    float s = 2.0f / std::max({e.x, e.y, e.z, 1e-12f});
    for (auto& p : positions) p = (p - c) * s;
}

float MeshData::signedVolume() const {
    double v = 0;
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const Vec3 &a = positions[indices[t]], &b = positions[indices[t + 1]], &c = positions[indices[t + 2]];
        v += dot(a, cross(b, c));
    }
    return (float)(v / 6.0);
}

void MeshData::flipWinding() {
    for (size_t t = 0; t + 2 < indices.size(); t += 3) std::swap(indices[t + 1], indices[t + 2]);
    for (auto& n : normals) n = -n;
}

size_t weldVertices(MeshData& m, float eps) {
    struct Key {
        long long x, y, z;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return (size_t)(k.x * 73856093LL ^ k.y * 19349663LL ^ k.z * 83492791LL);
        }
    };
    std::unordered_map<Key, uint32_t, KeyHash> seen;
    std::vector<uint32_t> remap(m.positions.size());
    MeshData out;
    for (size_t i = 0; i < m.positions.size(); ++i) {
        const Vec3& p = m.positions[i];
        Key k{(long long)std::llround(p.x / eps), (long long)std::llround(p.y / eps), (long long)std::llround(p.z / eps)};
        auto it = seen.find(k);
        if (it == seen.end()) {
            uint32_t id = (uint32_t)out.positions.size();
            out.positions.push_back(p);
            if (i < m.uvs.size()) out.uvs.push_back(m.uvs[i]);
            seen.emplace(k, id);
            remap[i] = id;
        } else {
            remap[i] = it->second;
        }
    }
    size_t removed = m.positions.size() - out.positions.size();
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        uint32_t a = remap[m.indices[t]], b = remap[m.indices[t + 1]], c = remap[m.indices[t + 2]];
        if (a == b || b == c || a == c) continue;  // collapsed at a pole
        out.indices.insert(out.indices.end(), {a, b, c});
    }
    out.computeNormals();
    m = std::move(out);
    return removed;
}

// ---------------------------------------------------------------- teapot
#include "scene/teapot_data.inc"

static void bernstein(float t, float b[4], float d[4]) {
    float s = 1 - t;
    b[0] = s * s * s; b[1] = 3 * t * s * s; b[2] = 3 * t * t * s; b[3] = t * t * t;
    d[0] = -3 * s * s; d[1] = 3 * s * s - 6 * t * s; d[2] = 6 * t * s - 3 * t * t; d[3] = 3 * t * t;
}

MeshData makeTeapot(int segments) {
    segments = std::max(segments, 1);
    MeshData m;
    const int row = segments + 1;
    for (int patch = 0; patch < 32; ++patch) {
        Vec3 g[4][4];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                int vi = kTeapotPatches[patch * 16 + r * 4 + c];
                Vec3 p{kTeapotVertices[vi * 3], kTeapotVertices[vi * 3 + 1], kTeapotVertices[vi * 3 + 2]};
                if (patch >= 20 && patch < 28) { p.x *= 1.077f; p.y *= 1.077f; }  // lid sits snugly
                g[r][c] = p;
            }
        uint32_t base = (uint32_t)m.positions.size();
        for (int i = 0; i <= segments; ++i)
            for (int j = 0; j <= segments; ++j) {
                float u = (float)i / segments, v = (float)j / segments;
                float bu[4], du[4], bv[4], dv[4];
                bernstein(u, bu, du); bernstein(v, bv, dv);
                Vec3 p;
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) p += g[r][c] * (bu[r] * bv[c]);
                // Newell data is Z-up; rotate by +x -> x, z -> y, y -> -z (a proper rotation).
                m.positions.push_back({p.x, p.z, -p.y});
                m.uvs.push_back({u, v});
            }
        for (int i = 0; i < segments; ++i)
            for (int j = 0; j < segments; ++j) {
                uint32_t a = base + i * row + j, b = a + 1, c = a + row, d = c + 1;
                m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
            }
    }
    weldVertices(m, 1e-4f);
    if (m.signedVolume() < 0) m.flipWinding();
    m.normalizeToUnitBox();
    m.computeNormals();
    return m;
}

MeshData makeUvSphere(int rings, int sectors, float radius) {
    MeshData m;
    for (int r = 0; r <= rings; ++r) {
        float phi = kPi * r / rings;
        for (int s = 0; s <= sectors; ++s) {
            float th = 2 * kPi * s / sectors;
            Vec3 n{std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th)};
            m.positions.push_back(n * radius);
            m.normals.push_back(n);
            m.uvs.push_back({(float)s / sectors, 1.0f - (float)r / rings});
        }
    }
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < sectors; ++s) {
            uint32_t a = r * (sectors + 1) + s, b = a + sectors + 1;
            m.indices.insert(m.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    if (m.signedVolume() < 0) m.flipWinding();
    return m;
}

MeshData makePlane(float h, float y) {
    MeshData m;
    m.positions = {{-h, y, -h}, {h, y, -h}, {h, y, h}, {-h, y, h}};
    m.normals.assign(4, {0, 1, 0});
    m.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    m.indices = {0, 2, 1, 0, 3, 2};
    return m;
}

// ---------------------------------------------------------------- OBJ
bool parseObj(const std::string& text, MeshData& out, std::string* err) {
    std::vector<Vec3> v, vn; std::vector<Vec2> vt;
    std::map<std::tuple<int, int, int>, uint32_t> cache;
    MeshData m;
    bool haveNormals = true;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    auto fail = [&](const std::string& why) {
        if (err) *err = "line " + std::to_string(lineNo) + ": " + why;
        return false;
    };
    auto resolve = [](int i, size_t n) { return i < 0 ? (int)n + i : i - 1; };
    while (std::getline(in, line)) {
        ++lineNo;
        std::istringstream ls(line);
        std::string tag; ls >> tag;
        if (tag == "v") { Vec3 p; ls >> p.x >> p.y >> p.z; v.push_back(p); }
        else if (tag == "vn") { Vec3 p; ls >> p.x >> p.y >> p.z; vn.push_back(p); }
        else if (tag == "vt") { Vec2 p; ls >> p.x >> p.y; vt.push_back(p); }
        else if (tag == "f") {
            std::vector<uint32_t> face;
            std::string tok;
            while (ls >> tok) {
                int a = 0, b = 0, c = 0;
                if (std::sscanf(tok.c_str(), "%d/%d/%d", &a, &b, &c) != 3) {
                    b = c = 0;
                    if (std::sscanf(tok.c_str(), "%d//%d", &a, &c) != 2) {
                        c = 0;
                        if (std::sscanf(tok.c_str(), "%d/%d", &a, &b) != 2) {
                            b = 0;
                            if (std::sscanf(tok.c_str(), "%d", &a) != 1) return fail("bad face token '" + tok + "'");
                        }
                    }
                }
                int vi = resolve(a, v.size());
                if (vi < 0 || vi >= (int)v.size()) return fail("vertex index out of range");
                int ti = b ? resolve(b, vt.size()) : -1, ni = c ? resolve(c, vn.size()) : -1;
                if (ti >= (int)vt.size() || ni >= (int)vn.size()) return fail("attribute index out of range");
                auto key = std::make_tuple(vi, ti, ni);
                auto it = cache.find(key);
                if (it == cache.end()) {
                    uint32_t id = (uint32_t)m.positions.size();
                    m.positions.push_back(v[vi]);
                    m.uvs.push_back(ti >= 0 ? vt[ti] : Vec2{});
                    if (ni >= 0) m.normals.push_back(vn[ni]); else haveNormals = false;
                    it = cache.emplace(key, id).first;
                }
                face.push_back(it->second);
            }
            if (face.size() < 3) return fail("face with fewer than 3 vertices");
            for (size_t i = 1; i + 1 < face.size(); ++i)
                m.indices.insert(m.indices.end(), {face[0], face[i], face[i + 1]});
        }
    }
    if (m.indices.empty()) return fail("no faces");
    if (!haveNormals || m.normals.size() != m.positions.size()) m.computeNormals();
    out = std::move(m);
    return true;
}

bool loadObj(const std::string& path, MeshData& out, std::string* err) {
    std::string text;
    if (!readFile(path, text)) { if (err) *err = "cannot open " + path; return false; }
    return parseObj(text, out, err);
}

} // namespace prism
