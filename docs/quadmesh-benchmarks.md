# Quad mesh benchmarks

Measured with the headless benchmark of the QuadMesh app:

```
./bin/Release/quadmesh --bench            # all models
./bin/Release/quadmesh --bench box        # one model (substring of teapot / bumpy / box)
```

Machine: AMD Ryzen 5 5600 (6 cores), Release build (`-O3`), Linux. The simplifiers are single
threaded; only the fitmaps use all cores. The machine was shared with other jobs during the run
(load average about 5.5 on 6 cores), so **times are pessimistic and vary by +-30% between
runs**; every other column is deterministic and reproduces exactly.

Algorithms: [theory-quadmesh.md](theory-quadmesh.md). Screenshots:
[uniform vs adaptive on the rounded box](assets/quadmesh.png),
[S-fitmap on the teapot](assets/quadmesh-fitmap.png).

## Models

| model | triangles | vertices | tri->quad ms | fitmaps ms | load + reference + fitmaps + curvature ms |
|---|---|---|---|---|---|
| teapot/8 | 4032 | 2082 | 5.2 | 37.8 | 45.9 |
| teapot/16 | 16256 | 8258 | 14.3 | 121.4 | 144.0 |
| bumpy sphere | 20480 | 10242 | 16.7 | 208.6 | 234.5 |
| rounded box | 27648 | 13826 | 6.2 | 86.0 | 110.7 |

* **teapot/8, teapot/16**: Utah teapot, 8 or 16 segments per patch edge. Open: 6 boundary loops
  (body rim, lid rim, spout tip, handle ends) and 4 components.
* **bumpy sphere**: subdivided icosahedron (5 levels) with three Gaussian bumps of decreasing
  width. Closed.
* **rounded box**: cube grid (48 x 48 per side) pushed onto a box with rounded edges: flat panels,
  cylindrical edges, spherical corners. Closed.

## Columns

* **target**: requested quads; the triangle baseline gets twice as many triangles, so both have
  about the same number of vertices. `0` is the tri -> quad conversion alone.
* **regular %**: vertices of valence 4 (3 on a boundary). **quality**: per quad, the worst
  corner of `2 |e1| |e2| sin(theta) / (|e1|^2 + |e2|^2)` (1 for a square, negative for a
  concave corner); mean and min over quads. **angle dev**: mean `|corner angle - 90|` in
  degrees. **homeometry**: mean of `(|e| - mu)^2` over edges and `(|d| - sqrt2 mu)^2` over
  diagonals, divided by `mu^2`, with `mu = sqrt(area / quads)`. These four only make sense for
  quads and are left blank for the triangle baseline.
* **Hausdorff %**: symmetric Hausdorff distance between result and input, in % of the input's
  bounding-box diagonal. **mean dist %**: mean of the two one-sided mean distances, same unit.
  Both from 40000 area-uniform samples per surface plus all vertices, nearest points through
  uniform grids (exact closest points, not approximations).
* **time ms**: copy of the input + tri -> quad + simplification (measurement excluded).

## Results

| model | method | target | faces | quads % | regular % | quality (mean / min) | angle dev | homeometry | Hausdorff % | mean dist % | time ms |
|---|---|---|---|---|---|---|---|---|---|---|---|
| teapot/8 | tri -> quad | 0 | 2016 | 100.0 | 93.6 | 0.602 / -0.908 | 7.9 | 0.471 | 0.437 | 0.0006 | 5.3 |
| teapot/8 | uniform quads | 1000 | 1000 | 100.0 | 90.7 | 0.668 / -0.226 | 7.7 | 0.188 | 2.995 | 0.1097 | 49.9 |
| teapot/8 | adaptive quads | 1000 | 1000 | 100.0 | 81.4 | 0.668 / -0.636 | 14.8 | 0.262 | 2.970 | 0.0702 | 62.3 |
| teapot/8 | triangle QEM | 1000 | 1999 | 0.0 | - | - | - | - | 0.196 | 0.0115 | 4.4 |
| teapot/8 | uniform quads | 500 | 499 | 100.0 | 83.5 | 0.732 / -0.356 | 11.5 | 0.147 | 3.692 | 0.2684 | 34.7 |
| teapot/8 | adaptive quads | 500 | 500 | 100.0 | 61.0 | 0.591 / -0.451 | 24.8 | 0.291 | 2.970 | 0.2356 | 40.9 |
| teapot/8 | triangle QEM | 500 | 1000 | 0.0 | - | - | - | - | 0.358 | 0.0475 | 3.8 |
| teapot/8 | uniform quads | 250 | 250 | 100.0 | 68.7 | 0.665 / -0.302 | 19.6 | 0.211 | 5.936 | 0.5192 | 65.7 |
| teapot/8 | adaptive quads | 250 | 250 | 100.0 | 56.6 | 0.428 / -0.392 | 30.9 | 0.496 | 9.800 | 0.9289 | 87.8 |
| teapot/8 | triangle QEM | 250 | 499 | 0.0 | - | - | - | - | 0.781 | 0.1250 | 3.3 |
| teapot/16 | tri -> quad | 0 | 8128 | 100.0 | 96.8 | 0.623 / -0.834 | 5.7 | 0.480 | 0.069 | 0.0001 | 14.6 |
| teapot/16 | uniform quads | 2000 | 2000 | 100.0 | 88.9 | 0.803 / -0.210 | 7.8 | 0.122 | 2.990 | 0.1037 | 197.4 |
| teapot/16 | adaptive quads | 2000 | 2000 | 100.0 | 73.9 | 0.670 / -0.328 | 18.8 | 0.315 | 2.990 | 0.0656 | 251.1 |
| teapot/16 | triangle QEM | 2000 | 4000 | 0.0 | - | - | - | - | 0.128 | 0.0127 | 16.9 |
| teapot/16 | uniform quads | 1000 | 1000 | 100.0 | 78.6 | 0.749 / -0.137 | 15.6 | 0.130 | 3.236 | 0.1841 | 189.3 |
| teapot/16 | adaptive quads | 1000 | 1000 | 100.0 | 60.0 | 0.500 / -0.463 | 30.3 | 0.445 | 2.990 | 0.1878 | 324.4 |
| teapot/16 | triangle QEM | 1000 | 2000 | 0.0 | - | - | - | - | 0.251 | 0.0320 | 27.8 |
| teapot/16 | uniform quads | 500 | 500 | 100.0 | 73.5 | 0.558 / -0.185 | 23.8 | 0.594 | 6.455 | 0.4213 | 262.9 |
| teapot/16 | adaptive quads | 500 | 500 | 100.0 | 55.1 | 0.330 / -0.815 | 38.4 | 0.749 | 9.615 | 1.0322 | 250.0 |
| teapot/16 | triangle QEM | 500 | 1000 | 0.0 | - | - | - | - | 0.487 | 0.0604 | 43.2 |
| bumpy sphere | tri -> quad | 0 | 10240 | 100.0 | 85.3 | 0.824 / -0.917 | 24.6 | 0.080 | 0.233 | 0.0005 | 16.9 |
| bumpy sphere | uniform quads | 2000 | 2000 | 100.0 | 75.3 | 0.861 / 0.097 | 12.2 | 0.034 | 1.391 | 0.0365 | 264.0 |
| bumpy sphere | adaptive quads | 2000 | 2000 | 100.0 | 78.1 | 0.865 / 0.084 | 12.0 | 0.056 | 0.337 | 0.0358 | 245.2 |
| bumpy sphere | triangle QEM | 2000 | 4000 | 0.0 | - | - | - | - | 0.062 | 0.0129 | 18.4 |
| bumpy sphere | uniform quads | 800 | 800 | 100.0 | 74.8 | 0.860 / 0.312 | 12.6 | 0.033 | 1.898 | 0.0895 | 268.3 |
| bumpy sphere | adaptive quads | 800 | 800 | 100.0 | 76.8 | 0.851 / 0.124 | 12.8 | 0.057 | 2.465 | 0.0926 | 271.9 |
| bumpy sphere | triangle QEM | 800 | 1600 | 0.0 | - | - | - | - | 0.178 | 0.0337 | 17.5 |
| bumpy sphere | uniform quads | 300 | 299 | 100.0 | 76.7 | 0.865 / 0.105 | 11.5 | 0.031 | 3.645 | 0.2360 | 299.5 |
| bumpy sphere | adaptive quads | 300 | 300 | 100.0 | 77.5 | 0.846 / 0.322 | 12.4 | 0.056 | 2.059 | 0.2390 | 304.4 |
| bumpy sphere | triangle QEM | 300 | 600 | 0.0 | - | - | - | - | 0.573 | 0.0886 | 25.1 |
| rounded box | tri -> quad | 0 | 13824 | 100.0 | 99.9 | 0.974 / 0.833 | 0.9 | 0.023 | 0.000 | 0.0000 | 6.4 |
| rounded box | uniform quads | 2000 | 2000 | 100.0 | 84.2 | 0.901 / -0.072 | 10.0 | 0.025 | 0.584 | 0.0459 | 322.9 |
| rounded box | adaptive quads | 2000 | 2000 | 100.0 | 76.3 | 0.859 / 0.121 | 12.4 | 0.169 | 0.311 | 0.0292 | 359.0 |
| rounded box | triangle QEM | 2000 | 4000 | 0.0 | - | - | - | - | 0.006 | 0.0000 | 78.5 |
| rounded box | uniform quads | 800 | 800 | 100.0 | 78.4 | 0.873 / 0.121 | 11.6 | 0.029 | 1.007 | 0.1115 | 405.7 |
| rounded box | adaptive quads | 800 | 800 | 100.0 | 76.2 | 0.846 / 0.198 | 13.0 | 0.157 | 0.791 | 0.0743 | 426.9 |
| rounded box | triangle QEM | 800 | 1600 | 0.0 | - | - | - | - | 0.042 | 0.0018 | 75.5 |
| rounded box | uniform quads | 300 | 300 | 100.0 | 78.5 | 0.872 / 0.434 | 11.3 | 0.029 | 2.643 | 0.3191 | 348.3 |
| rounded box | adaptive quads | 300 | 300 | 100.0 | 71.9 | 0.814 / 0.282 | 14.1 | 0.140 | 1.643 | 0.2051 | 384.6 |
| rounded box | triangle QEM | 300 | 600 | 0.0 | - | - | - | - | 0.129 | 0.0074 | 88.8 |

## Reading the numbers

**Conversion.** Squareness pairing plus crawling gives 100% quads on every model, without
adding vertices: 96.8% regular vertices on teapot/16, 99.9% on the box (whose triangles came
from a grid). On the bumpy sphere (icosphere triangles, valence 5 and 6) only 85% can be
regular, and pairing has to accept some poor quads (min quality -0.92: a few concave ones).

**Uniform quads** keep the mesh regular (69 to 91% regular vertices) and square (angle deviation
8 to 24 degrees). Error grows smoothly as the budget shrinks.

**Adaptive vs uniform.** Same quad count, different placement:

* Rounded box: lower error at every budget, Hausdorff -47% / -21% / -38% and mean distance
  -36% / -33% / -36% at 2000 / 800 / 300 quads. The flat panels get big quads, the rounded
  edges and corners small ones ([screenshot](assets/quadmesh.png)).
* Bumpy sphere at 2000 quads: Hausdorff 1.39% -> 0.34%; the bumps keep their shape. At 800
  quads the sharpest bump is narrower than any face either version can afford there and the
  Hausdorff error is worse (2.47% vs 1.90%); at 300 the mean distances are equal and the
  Hausdorff error is lower again (2.06% vs 3.65%).
* Teapot: better at moderate reduction (mean distance -36% at 1000 quads on teapot/8, -37% at
  2000 on teapot/16), worse at strong reduction. The teapot's six boundary loops keep all their
  vertices (no boundary operator), which pins a ring of small faces along each border, and its
  flat bottom has `F_s = 0`, so faces there grow first. With few quads left, both effects
  starve the body.
* Grading sizes costs regularity: up to 19 points fewer regular vertices on the teapot, 2 to 8
  on the box (the same on the bumpy sphere), and a larger homeometry energy (by design: it
  measures the distance to one uniform size).

**Triangle QEM** is much more accurate for the same number of vertices (mean distance 2.7x to
over 50x smaller), and much faster (it does one heap operation per collapse and no smoothing).
It minimises distance and nothing else: its output is a field of slivers and arbitrary
valences, not a quad layout. The quad methods should be compared with it as the price of
structure, not as competitors on error.

**Speed.** Simplifying teapot/16 (8128 quads) to 2000 / 1000 / 500 quads takes 0.19 to 0.33 s
on the loaded machine, teapot/8 0.03 to 0.09 s; the 14k-quad box takes 0.3
to 0.43 s. Model preparation (reference grid, fitmaps, curvature) is paid once per model:
0.05 to 0.23 s.
