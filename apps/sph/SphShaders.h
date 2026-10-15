#pragma once
// GLSL for the SPH app: the compute port of WCSPH and the sphere-impostor renderer.
// The compute passes mirror SphSolver.cpp line for line so the two can be compared.

namespace sph {

// ---- compute: shared declarations ----
inline const char* kCsCommon = R"(
layout(std430, binding = 0) buffer Pos { vec4 pos[]; };
layout(std430, binding = 1) buffer Vel { vec4 vel[]; };
layout(std430, binding = 2) buffer Rho { float rho[]; };
layout(std430, binding = 3) buffer PosOut { vec4 posOut[]; };
layout(std430, binding = 4) buffer VelTmp { vec4 velTmp[]; };
layout(std430, binding = 5) buffer Press { float press[]; };
layout(std430, binding = 6) buffer BucketOf { uint bucketOf[]; };
layout(std430, binding = 7) buffer Start { uint start[]; };       // tableSize + 1 entries
layout(std430, binding = 8) buffer Cursor { uint cursor[]; };
layout(std430, binding = 9) buffer Sorted { uint sortedIdx[]; };

uniform uint uN, uMask;
uniform float uInvCell, uH, uH2, uMass, uRho0, uTaitB, uDt, uMu, uXsph, uRest, uFriction, uRadius;
uniform float uPoly6C, uSpikyGradC, uViscLapC;
uniform vec3 uGravity, uBox;

// Same hash as glue::HashGrid::hashCell (uint wrap-around arithmetic).
uint hashCell(ivec3 c) { return ((uint(c.x) * 73856093u) ^ (uint(c.y) * 19349663u) ^ (uint(c.z) * 83492791u)) & uMask; }
ivec3 cellOf(vec3 p) { return ivec3(floor(p * uInvCell)); }

// Distinct buckets of the 27 cells around p.
int neighborBuckets(vec3 p, out uint b[27]) {
    ivec3 c = cellOf(p);
    int n = 0;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                uint h = hashCell(c + ivec3(dx, dy, dz));
                bool seen = false;
                for (int k = 0; k < n; ++k) seen = seen || b[k] == h;
                if (!seen) b[n++] = h;
            }
    return n;
}

float poly6(float r2) { if (r2 >= uH2) return 0.0; float d = uH2 - r2; return uPoly6C * d * d * d; }
vec3 spikyGrad(vec3 r, float rl) { if (rl >= uH || rl <= 1e-12) return vec3(0); float d = uH - rl; return r * (uSpikyGradC * d * d / rl); }
float viscLap(float rl) { return rl >= uH ? 0.0 : uViscLapC * (uH - rl); }
)";

// Every particle pass is 128 wide and starts with `uint i = gl_GlobalInvocationID.x; if (i >= uN) return;`.
inline const char* kCsClear = R"(
layout(local_size_x = 128) in;
void main() { uint i = gl_GlobalInvocationID.x; if (i <= uMask + 1u) start[i] = 0u; }
)";

// Counting sort, step 1: bucket of each particle; start[b + 1] counts bucket b.
inline const char* kCsHash = R"(
layout(local_size_x = 128) in;
void main() {
    uint i = gl_GlobalInvocationID.x; if (i >= uN) return;
    uint b = hashCell(cellOf(pos[i].xyz));
    bucketOf[i] = b;
    atomicAdd(start[b + 1u], 1u);
}
)";

// Step 2: inclusive scan of start[0..T] (start[0] = 0) gives start[b] = first slot of bucket b.
// One work group of 1024: each thread scans a contiguous chunk serially, the chunk totals are
// scanned in shared memory (Hillis-Steele), then each chunk adds its offset.
inline const char* kCsScan = R"(
layout(local_size_x = 1024) in;
shared uint partial[1024];
void main() {
    uint t = gl_LocalInvocationID.x;
    uint total = uMask + 2u;
    uint per = (total + 1023u) / 1024u;
    uint b0 = min(t * per, total), b1 = min(b0 + per, total);
    uint s = 0u;
    for (uint b = b0; b < b1; ++b) s += start[b];
    partial[t] = s;
    barrier();
    for (uint off = 1u; off < 1024u; off <<= 1) {
        uint v = t >= off ? partial[t - off] : 0u;
        barrier();
        partial[t] += v;
        barrier();
    }
    uint run = t > 0u ? partial[t - 1u] : 0u;
    for (uint b = b0; b < b1; ++b) {
        run += start[b];
        start[b] = run;
        if (b <= uMask) cursor[b] = run;
    }
}
)";

// Step 3: scatter. Order inside a bucket depends on atomics, so it differs from the CPU's,
// which only changes the order of float sums (rounding-level differences).
inline const char* kCsScatter = R"(
layout(local_size_x = 128) in;
void main() {
    uint i = gl_GlobalInvocationID.x; if (i >= uN) return;
    uint k = atomicAdd(cursor[bucketOf[i]], 1u);
    sortedIdx[k] = i;
}
)";

// Density and Tait pressure.
inline const char* kCsDensity = R"(
layout(local_size_x = 128) in;
void main() {
    uint i = gl_GlobalInvocationID.x; if (i >= uN) return;
    vec3 xi = pos[i].xyz;
    uint bk[27]; int nb = neighborBuckets(xi, bk);
    float s = 0.0;
    for (int b = 0; b < nb; ++b)
        for (uint k = start[bk[b]]; k < start[bk[b] + 1u]; ++k) {
            vec3 r = xi - pos[sortedIdx[k]].xyz;
            s += poly6(dot(r, r));
        }
    float d = uMass * s;
    rho[i] = d;
    float q = d / uRho0, q2 = q * q, q7 = q2 * q2 * q2 * q;
    press[i] = max(uTaitB * (q7 - 1.0), 0.0);
}
)";

// Gravity + viscosity + symmetric pressure force, then v* = v + dt a.
inline const char* kCsForce = R"(
layout(local_size_x = 128) in;
void main() {
    uint i = gl_GlobalInvocationID.x; if (i >= uN) return;
    vec3 xi = pos[i].xyz, vi = vel[i].xyz;
    float ri = rho[i], pi = press[i] / (ri * ri);
    vec3 av = vec3(0), ap = vec3(0);
    uint bk[27]; int nb = neighborBuckets(xi, bk);
    for (int b = 0; b < nb; ++b)
        for (uint k = start[bk[b]]; k < start[bk[b] + 1u]; ++k) {
            uint j = sortedIdx[k];
            if (j == i) continue;
            vec3 r = xi - pos[j].xyz;
            float r2 = dot(r, r);
            if (r2 > uH2) continue;
            float rl = sqrt(r2), rj = rho[j];
            av += (vel[j].xyz - vi) * (uMass / rj * viscLap(rl));
            ap -= spikyGrad(r, rl) * (uMass * (pi + press[j] / (rj * rj)));
        }
    vec3 a = uGravity + ap + (uMu > 0.0 ? av * (uMu / ri) : vec3(0));
    velTmp[i] = vec4(vi + a * uDt, 0.0);
}
)";

// XSPH smoothing, position update, box walls. Writes posOut (ping-pong) and the final velocity.
inline const char* kCsIntegrate = R"(
layout(local_size_x = 128) in;
void main() {
    uint i = gl_GlobalInvocationID.x; if (i >= uN) return;
    vec3 xi = pos[i].xyz, vs = velTmp[i].xyz;
    float ri = rho[i];
    vec3 dv = vec3(0);
    if (uXsph > 0.0) {
        uint bk[27]; int nb = neighborBuckets(xi, bk);
        for (int b = 0; b < nb; ++b)
            for (uint k = start[bk[b]]; k < start[bk[b] + 1u]; ++k) {
                uint j = sortedIdx[k];
                vec3 r = xi - pos[j].xyz;
                dv += (velTmp[j].xyz - vs) * (2.0 * uMass / (ri + rho[j]) * poly6(dot(r, r)));
            }
    }
    vec3 v = vs + dv * uXsph;
    vec3 x = xi + v * uDt;
    for (int a = 0; a < 3; ++a) {
        bool lo = x[a] < uRadius, up = x[a] > uBox[a] - uRadius;
        if (!lo && !up) continue;
        x[a] = lo ? uRadius : uBox[a] - uRadius;
        if ((lo && v[a] < 0.0) || (up && v[a] > 0.0)) v[a] = -uRest * v[a];
        for (int b = 0; b < 3; ++b) if (b != a) v[b] *= 1.0 - uFriction;
    }
    posOut[i] = vec4(x, 1.0);
    vel[i] = vec4(v, 0.0);
}
)";

// ---- rendering ----

// Sphere impostors: one camera-facing quad per particle (instanced, corners from gl_VertexID),
// the fragment shader turns the quad into a lit sphere and writes its true depth.
inline const char* kParticleVs = R"(#version 450
layout(std430, binding = 0) readonly buffer Pos { vec4 pos[]; };
layout(std430, binding = 1) readonly buffer Vel { vec4 vel[]; };
layout(std430, binding = 2) readonly buffer Rho { float rho[]; };
uniform mat4 uView, uProj;
uniform float uRadius, uRho0, uMaxSpeed, uDensRange;
uniform int uColorBy;   // 0 speed, 1 density
out vec2 vUV;
out vec3 vCenter;       // view space
out float vValue;       // 0..1 along the colour map
void main() {
    const vec2 corners[4] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(1, 1));
    vec2 c = corners[gl_VertexID];
    vec4 vc = uView * vec4(pos[gl_InstanceID].xyz, 1.0);
    vCenter = vc.xyz;
    vUV = c;
    vValue = uColorBy == 0 ? length(vel[gl_InstanceID].xyz) / uMaxSpeed
                           : 0.5 + (rho[gl_InstanceID] / uRho0 - 1.0) / (2.0 * uDensRange);
    vc.xy += c * uRadius;
    gl_Position = uProj * vc;
}
)";

inline const char* kParticleFs = R"(#version 450
in vec2 vUV;
in vec3 vCenter;
in float vValue;
uniform mat4 uProj;
uniform float uRadius;
out vec4 fragColor;
// Deep blue -> cyan -> white -> warm: calm water reads blue, fast or compressed spray reads hot.
vec3 ramp(float t) {
    t = clamp(t, 0.0, 1.0);
    vec3 a = vec3(0.05, 0.18, 0.55), b = vec3(0.10, 0.60, 0.95), c = vec3(0.90, 0.97, 1.00), d = vec3(1.00, 0.55, 0.20);
    return t < 0.4 ? mix(a, b, t / 0.4) : t < 0.75 ? mix(b, c, (t - 0.4) / 0.35) : mix(c, d, (t - 0.75) / 0.25);
}
void main() {
    float r2 = dot(vUV, vUV);
    if (r2 > 1.0) discard;
    vec3 n = vec3(vUV, sqrt(1.0 - r2));
    vec3 p = vCenter + n * uRadius;
    vec4 clip = uProj * vec4(p, 1.0);
    gl_FragDepth = clip.z / clip.w * 0.5 + 0.5;
    vec3 L = normalize(vec3(0.4, 0.8, 0.5)), V = normalize(-p), H = normalize(L + V);
    vec3 base = ramp(vValue);
    float diff = max(dot(n, L), 0.0), spec = pow(max(dot(n, H), 0.0), 48.0);
    vec3 col = base * (0.25 + 0.75 * diff) + vec3(0.35) * spec;
    fragColor = vec4(col, 1.0);
}
)";

// Container: 12 box edges (line list, 24 vertices) and a floor quad (triangle strip), all from gl_VertexID.
inline const char* kBoxVs = R"(#version 450
uniform mat4 uViewProj;
uniform vec3 uBox;
uniform int uFloor;
out vec3 vWorld;
void main() {
    const ivec2 e[12] = ivec2[](ivec2(0,1), ivec2(1,3), ivec2(3,2), ivec2(2,0), ivec2(4,5), ivec2(5,7),
                                ivec2(7,6), ivec2(6,4), ivec2(0,4), ivec2(1,5), ivec2(2,6), ivec2(3,7));
    vec3 p;
    if (uFloor == 1) {
        const vec2 q[4] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 1));
        p = vec3(q[gl_VertexID].x * uBox.x, 0.0, q[gl_VertexID].y * uBox.z);
    } else {
        int c = (gl_VertexID & 1) == 0 ? e[gl_VertexID / 2].x : e[gl_VertexID / 2].y;
        p = vec3(c & 1, (c >> 1) & 1, (c >> 2) & 1) * uBox;
    }
    vWorld = p;
    gl_Position = uViewProj * vec4(p, 1.0);
}
)";

inline const char* kBoxFs = R"(#version 450
in vec3 vWorld;
uniform int uFloor;
out vec4 fragColor;
void main() {
    if (uFloor == 0) { fragColor = vec4(0.55, 0.60, 0.70, 1.0); return; }
    vec2 c = floor(vWorld.xz * 10.0);
    float k = mod(c.x + c.y, 2.0);
    fragColor = vec4(mix(vec3(0.11, 0.12, 0.15), vec3(0.15, 0.16, 0.20), k), 1.0);
}
)";

} // namespace sph
