// Minimal test runner: no framework, no GL context needed. Run: bin/Release/tests
#include "gfx/ShaderToy.h"
#include "math/Math.h"
#include "scene/MeshData.h"
#include <map>
#include <set>
#include <string>

#include "TestFramework.h"

using namespace prism;

TEST(core_testMath) {
    Mat4 I;
    Mat4 t = translate({1, 2, 3});
    Mat4 p = I * t;
    CHECK_NEAR(p.at(0, 3), 1, 1e-6); CHECK_NEAR(p.at(2, 3), 3, 1e-6);
    Vec3 c = cross({1, 0, 0}, {0, 1, 0});
    CHECK_NEAR(c.z, 1, 1e-6);
    Vec3 n = normalize({3, 0, 4});
    CHECK_NEAR(length(n), 1, 1e-6);
    // lookAt maps the eye to the origin and the target onto the -z axis.
    Mat4 v = lookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    CHECK_NEAR(v.at(2, 3), -5, 1e-5);
    // perspective: a point on the near plane maps to NDC z = -1.
    Mat4 P = perspective(radians(60), 1.0f, 0.1f, 100.0f);
    float z = -0.1f, w = 1.0f;
    float clipZ = P.at(2, 2) * z + P.at(2, 3) * w, clipW = P.at(3, 2) * z;
    CHECK_NEAR(clipZ / clipW, -1, 1e-4);
}

TEST(core_testTeapot) {
    MeshData t = makeTeapot(8);
    CHECK(t.triangleCount() > 1000);
    CHECK(t.signedVolume() > 0);                     // outward facing
    Vec3 lo, hi; t.bounds(lo, hi);
    CHECK_NEAR(hi.x - lo.x, 2.0, 1e-4);                    // spout to handle is the longest extent, scaled to 2
    CHECK(hi.y - lo.y > 0.9f && hi.y - lo.y < 1.1f);       // about half as tall as it is wide
    // Vertex welding closed the seams: far fewer vertices than 32 patches * 9*9.
    CHECK(t.positions.size() < 32u * 81u);
    // Every edge of a closed surface is shared by exactly two triangles... except the open rim
    // and lid hole. Count edges used once: must be a small boundary, not seams everywhere.
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (size_t i = 0; i < t.indices.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            uint32_t a = t.indices[i + k], b = t.indices[i + (k + 1) % 3];
            edges[{std::min(a, b), std::max(a, b)}]++;
        }
    size_t boundary = 0;
    for (auto& e : edges) if (e.second == 1) ++boundary;
    CHECK(boundary < edges.size() / 20);
    for (auto& n : t.normals) CHECK(std::fabs(length(n) - 1.0f) < 1e-3f || length(n) == 0.0f);
}

TEST(core_testObj) {
    MeshData m; std::string err;
    CHECK(parseObj("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n", m, &err));   // quad -> 2 triangles
    CHECK(m.triangleCount() == 2);
    CHECK(parseObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf -3 -2 -1\n", m, &err));            // negative indices
    CHECK(m.triangleCount() == 1);
    CHECK(parseObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\n", m, &err));
    CHECK_NEAR(m.normals[0].z, 1, 1e-6);
    CHECK(!parseObj("v 0 0 0\nf 1 2 3\n", m, &err));                                // index out of range
    CHECK(!err.empty());
}

TEST(core_testWeld) {
    MeshData m;
    m.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    m.indices = {0, 1, 2, 3, 4, 5};
    size_t removed = weldVertices(m);
    CHECK(removed == 2);
    CHECK(m.positions.size() == 4);
    CHECK(m.triangleCount() == 2);
}

TEST(core_testShaderToy) {
    auto ps = parseUniformParams(
        "uniform float uA = 0.25; // @range 0 2\n"
        "uniform int uN = 6; // @range 2 16\n"
        "uniform vec3 uC = vec3(0.1, 0.2, 0.3); // @color\n"
        "uniform vec3 uPlain;\n"
        "uniform float iTime;\n"
        "uniform bool uOn = true;\n");
    CHECK(ps.size() == 4);
    CHECK(ps[0].name == "uA" && ps[0].kind == UniformParam::Float);
    CHECK_NEAR(ps[0].value[0], 0.25, 1e-6); CHECK_NEAR(ps[0].hi, 2.0, 1e-6);
    CHECK(ps[1].kind == UniformParam::Int && ps[1].lo == 2 && ps[1].hi == 16);
    CHECK(ps[2].kind == UniformParam::Color); CHECK_NEAR(ps[2].value[2], 0.3, 1e-6);
    CHECK(ps[3].kind == UniformParam::Bool && ps[3].value[0] == 1.0f);
    std::string w = wrapShadertoy("void mainImage(out vec4 c, in vec2 p){c=vec4(1);}");
    CHECK(w.find("#version 450 core") == 0);
    CHECK(w.find("#line 1") != std::string::npos);
    CHECK(w.find("mainImage(c, vUv * iResolution.xy)") != std::string::npos);
}

