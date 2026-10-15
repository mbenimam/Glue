#pragma once
// Tiny test framework: TEST(name) { CHECK(...); } in any tests/test_*.cpp registers itself.
#include <cmath>
#include <cstdio>
#include <vector>

namespace testfw {
struct Case { const char* name; void (*fn)(); };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }
inline int& checks() { static int c = 0; return c; }
struct Reg { Reg(const char* n, void (*f)()) { registry().push_back({n, f}); } };
}

#define TEST(name) static void name(); static testfw::Reg reg_##name(#name, name); static void name()
#define CHECK(cond) do { ++testfw::checks(); if (!(cond)) { ++testfw::failures(); std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((double)(a) - (double)(b)) <= (eps))
