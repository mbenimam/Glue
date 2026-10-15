# SPH benchmarks

All numbers on this page were measured on this machine by running the app headless:

```
./bin/Release/sph --hidden --bench                      # 6 OpenMP threads (default)
OMP_NUM_THREADS=1 ./bin/Release/sph --hidden --bench    # single thread
./bin/Release/sph --hidden --check --verify-gpu         # stability + CPU/GPU agreement
```

**Machine:** AMD Ryzen 5 5600 (6 cores, SMT off, 6 OpenMP threads), AMD Radeon RX 580
(Mesa 26.2.2 radeonsi, OpenGL 4.6 core), Linux 7.2, GCC `-O3 -fopenmp` (Release, no `-march`).

**Method.** Dam-break scene (block of 0.5 x 0.7 x 0.6 m in a 1.6 x 1.0 x 0.6 m box), `h = 2 x`
spacing, viscosity 1 Pa s, XSPH 0.05, `dt` from the CFL rule of each solver (WCSPH:
`0.4 h / c0` with `c0 = 40 m/s`; PCISPH: `0.4 h / sqrt(2 g H)`, capped at 5 ms). Per row: reset, 20
warm-up steps, then 100 timed steps; CPU columns are the **median** over the 100 steps, the GPU
column the median over 10 batches of 10 steps each ended by `glFinish` (wall clock, includes
driver submission). "Particles" is the actual count (the lattice fill rounds the 5k/10k/.../160k
targets down). "ms per simulated second" = ms/step / dt, the fair comparison between solvers that
take different time steps.

**Caveat on noise.** The machine was not idle during the runs (a browser and other jobs; load
average between 3 and 6 on 6 cores). The CPU rows vary by up to ~2x between two back-to-back
runs (compare run A and run B, e.g. 18k WCSPH 16.4 ms vs 6.0 ms). The GPU rows reproduce within 1 %.
Treat CPU numbers as "this order of magnitude on a loaded desktop", not as a best case.

## Run A, 6 threads

| particles | solver | dt (ms) | ms/step | neighbours | grid build | neighbour lists | density | pressure | integrate | PCISPH iters | ms per simulated second |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 4760 | WCSPH (CPU) | 0.695 | 3.96 | 25.8 | 0.07 | 2.90 | 0.08 | 0.33 | 0.15 | 1.0 | 5693 |
| 4760 | PCISPH (CPU) | 5.000 | 6.42 | 28.9 | 0.07 | 3.03 | 0.23 | 3.28 | 0.15 | 4.5 | 1283 |
| 4760 | WCSPH (GPU compute) | 0.695 | 1.373 | | | | | | | | 1975 |
| 9450 | WCSPH (CPU) | 0.552 | 4.64 | 26.5 | 0.13 | 3.35 | 0.12 | 0.62 | 0.25 | 1.0 | 8403 |
| 9450 | PCISPH (CPU) | 4.982 | 16.19 | 29.8 | 0.17 | 3.87 | 0.48 | 10.42 | 0.31 | 7.4 | 3249 |
| 9450 | WCSPH (GPU compute) | 0.552 | 1.072 | | | | | | | | 1943 |
| 18414 | WCSPH (CPU) | 0.438 | 16.41 | 27.1 | 0.32 | 6.18 | 2.13 | 3.56 | 2.61 | 1.0 | 37462 |
| 18414 | PCISPH (CPU) | 3.954 | 33.43 | 30.5 | 0.44 | 8.47 | 3.08 | 23.41 | 2.60 | 8.1 | 8455 |
| 18414 | WCSPH (GPU compute) | 0.438 | 1.525 | | | | | | | | 3483 |
| 38080 | WCSPH (CPU) | 0.348 | 13.38 | 27.6 | 0.87 | 7.62 | 0.51 | 2.91 | 1.15 | 1.0 | 38484 |
| 38080 | PCISPH (CPU) | 3.138 | 38.56 | 30.8 | 1.08 | 10.52 | 1.92 | 23.23 | 1.18 | 9.2 | 12286 |
| 38080 | WCSPH (GPU compute) | 0.348 | 3.447 | | | | | | | | 9915 |
| 77400 | WCSPH (CPU) | 0.276 | 27.47 | 28.1 | 2.49 | 15.46 | 1.02 | 5.92 | 2.38 | 1.0 | 99572 |
| 77400 | PCISPH (CPU) | 2.491 | 81.77 | 30.9 | 2.28 | 21.70 | 4.08 | 49.02 | 2.54 | 9.5 | 32827 |
| 77400 | WCSPH (GPU compute) | 0.276 | 7.699 | | | | | | | | 27907 |
| 153090 | WCSPH (CPU) | 0.219 | 73.21 | 28.4 | 4.75 | 35.54 | 5.01 | 16.48 | 7.43 | 1.0 | 334331 |
| 153090 | WCSPH (GPU compute) | 0.219 | 16.801 | | | | | | | | 76724 |

PCISPH was not run at 160k (minutes per row).

## Run B, 6 threads (immediately after run A)

| particles | solver | dt (ms) | ms/step | neighbours | grid build | neighbour lists | density | pressure | integrate | PCISPH iters | ms per simulated second |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 4760 | WCSPH (CPU) | 0.695 | 3.13 | 25.8 | 0.06 | 1.05 | 0.06 | 0.31 | 0.13 | 1.0 | 4501 |
| 4760 | PCISPH (CPU) | 5.000 | 4.90 | 28.9 | 0.06 | 1.30 | 0.23 | 1.62 | 0.15 | 4.5 | 980 |
| 4760 | WCSPH (GPU compute) | 0.695 | 1.362 | | | | | | | | 1960 |
| 9450 | WCSPH (CPU) | 0.552 | 2.84 | 26.5 | 0.13 | 1.71 | 0.12 | 0.60 | 0.24 | 1.0 | 5151 |
| 9450 | PCISPH (CPU) | 4.982 | 7.27 | 29.8 | 0.13 | 2.42 | 0.45 | 4.12 | 0.29 | 7.4 | 1459 |
| 9450 | WCSPH (GPU compute) | 0.552 | 1.082 | | | | | | | | 1961 |
| 18414 | WCSPH (CPU) | 0.438 | 6.04 | 27.1 | 0.29 | 3.43 | 0.24 | 1.41 | 0.56 | 1.0 | 13797 |
| 18414 | PCISPH (CPU) | 3.954 | 16.74 | 30.5 | 0.37 | 4.84 | 0.96 | 9.76 | 0.63 | 8.1 | 4233 |
| 18414 | WCSPH (GPU compute) | 0.438 | 1.527 | | | | | | | | 3486 |
| 38080 | WCSPH (CPU) | 0.348 | 13.51 | 27.6 | 0.87 | 7.55 | 0.51 | 2.91 | 1.12 | 1.0 | 38878 |
| 38080 | PCISPH (CPU) | 3.138 | 37.21 | 30.8 | 0.86 | 10.19 | 1.90 | 22.47 | 1.19 | 9.2 | 11858 |
| 38080 | WCSPH (GPU compute) | 0.348 | 3.433 | | | | | | | | 9875 |
| 77400 | WCSPH (CPU) | 0.276 | 26.51 | 28.1 | 1.77 | 15.89 | 1.00 | 5.76 | 2.33 | 1.0 | 96094 |
| 77400 | PCISPH (CPU) | 2.491 | 87.24 | 30.9 | 2.36 | 21.74 | 4.29 | 54.48 | 2.83 | 9.5 | 35022 |
| 77400 | WCSPH (GPU compute) | 0.276 | 7.707 | | | | | | | | 27936 |
| 153090 | WCSPH (CPU) | 0.219 | 89.58 | 28.4 | 6.18 | 39.31 | 8.59 | 23.36 | 12.69 | 1.0 | 409084 |
| 153090 | WCSPH (GPU compute) | 0.219 | 16.816 | | | | | | | | 76792 |

## Single thread (`OMP_NUM_THREADS=1`), CPU rows only

| particles | solver | ms/step | grid build | neighbour lists | density | pressure | integrate | ms per simulated second |
|---|---|---|---|---|---|---|---|---|
| 4760 | WCSPH | 3.68 | 0.04 | 1.86 | 0.17 | 1.10 | 0.43 | 5294 |
| 4760 | PCISPH | 8.97 | 0.06 | 3.05 | 0.77 | 4.46 | 0.50 | 1793 |
| 9450 | WCSPH | 7.10 | 0.11 | 3.41 | 0.35 | 2.26 | 0.87 | 12861 |
| 9450 | PCISPH | 24.49 | 0.15 | 6.64 | 1.58 | 14.12 | 1.05 | 4915 |
| 18414 | WCSPH | 14.21 | 0.26 | 6.87 | 0.69 | 4.46 | 1.75 | 32458 |
| 18414 | PCISPH | 52.17 | 0.29 | 13.90 | 3.25 | 32.65 | 2.08 | 13194 |
| 38080 | WCSPH | 30.19 | 0.65 | 14.66 | 1.49 | 9.52 | 3.68 | 86839 |
| 38080 | PCISPH | 128.56 | 0.86 | 29.57 | 7.02 | 85.08 | 4.65 | 40965 |
| 77400 | WCSPH | 71.67 | 1.51 | 35.72 | 3.24 | 21.12 | 8.15 | 259763 |
| 77400 | PCISPH | 262.66 | 1.95 | 63.53 | 13.69 | 174.72 | 9.22 | 105451 |
| 153090 | WCSPH | 158.38 | 3.83 | 85.39 | 6.69 | 44.81 | 16.96 | 723288 |

## What the numbers say

- **Thread scaling is modest:** 6 threads vs 1 at 77k particles: WCSPH 71.7 -> 26.5-27.5 ms
  (2.6-2.7x), PCISPH 262.7 -> 81.8-87.2 ms (3.0-3.2x), on a machine that was not idle.
- **Neighbour lists are the largest part of every CPU WCSPH step** (34-73 % of the median step). With cells of side `h`,
  each query tests 27 cells x ~8 particles = ~216 candidates to keep ~28 neighbours. The hash grid
  build itself is cheap (serial, 1.8-2.5 ms at 77k).
- **PCISPH vs WCSPH:** PCISPH costs 2-4x more per step (4.5-9.5 correction iterations) but takes
  steps ~9x larger, so per simulated second it was 2.6-4.6x cheaper in every CPU row of both runs.
- **GPU WCSPH vs CPU WCSPH (6 threads):** at 77k particles 7.7 ms vs 26.5-27.5 ms (~3.5x), at 153k
  16.8 ms vs 73-90 ms (4.4-5.3x). Below ~10k particles the GPU step is flat at ~1.1-1.4 ms: fixed
  cost of 7 dispatches, uniform updates and the single-work-group prefix scan, not particle work.
- At 18k particles, GPU WCSPH (3.5 s of wall time per simulated second) and CPU PCISPH
  (4.2-8.5 s) are the fastest options; neither is real time at this resolution on this machine.

## Stability and CPU/GPU agreement (`--check --verify-gpu`, 20k target)

2000 steps of every solver on every scene. A run fails on any NaN/inf, any particle outside the
box, or a maximum speed above 3x the free-fall speed from the box height (13.3 m/s).

| scene | solver | particles | dt (ms) | simulated (s) | max \|v\| seen (m/s) | mean compression at end | result |
|---|---|---|---|---|---|---|---|
| Dam break | WCSPH (CPU) | 18414 | 0.438 | 0.876 | 3.345 | 0.06 % | ok |
| Drop | WCSPH (CPU) | 18141 | 0.401 | 0.802 | 3.378 | 0.04 % | ok |
| Double dam break | WCSPH (CPU) | 18304 | 0.533 | 1.065 | 6.353 | 0.10 % | ok |
| Dam break | PCISPH (CPU) | 18414 | 3.954 | 7.908 | 4.024 | 0.47 % | ok |
| Drop | PCISPH (CPU) | 18141 | 3.620 | 7.240 | 2.547 | 0.45 % | ok |
| Double dam break | PCISPH (CPU) | 18304 | 4.810 | 9.620 | 4.497 | 0.49 % | ok |
| Dam break | WCSPH (GPU) | 18414 | 0.438 | 0.876 | 3.331 | 0.06 % | ok |
| Drop | WCSPH (GPU) | 18141 | 0.401 | 0.802 | 3.378 | 0.04 % | ok |
| Double dam break | WCSPH (GPU) | 18304 | 0.533 | 1.065 | 6.385 | 0.12 % | ok |

The same check also passed at a 10k target (9450 / 8300 / 8840 particles).

Mean compression is the mean of `max(rho / rho0 - 1, 0)`. PCISPH stops iterating at 0.5 %, which
is why it sits just below that.

**CPU vs GPU WCSPH**, dam break, 18414 particles, same initial state. After one step the per-particle
densities differ by at most 3.7e-7 (relative) and positions by at most 6e-8 m: float rounding
from a different summation order (GPU buckets are filled by atomics). After that the two runs
diverge chaotically like any two fluid simulations, so bulk statistics are compared:

| t (s) | | com.x | com.y | mean \|v\| | max \|v\| | mean rho | front x |
|---|---|---|---|---|---|---|---|
| 0.109 | CPU | 0.2511 | 0.3036 | 0.6759 | 1.5840 | 963.62 | 0.5488 |
| | GPU | 0.2511 | 0.3036 | 0.6759 | 1.5840 | 963.61 | 0.5488 |
| 0.219 | CPU | 0.2939 | 0.2364 | 0.9279 | 3.1378 | 979.90 | 0.7658 |
| | GPU | 0.2939 | 0.2364 | 0.9281 | 3.1342 | 979.97 | 0.7658 |
| 0.438 | CPU | 0.4518 | 0.1485 | 0.8676 | 3.0684 | 980.29 | 1.3100 |
| | GPU | 0.4518 | 0.1485 | 0.8682 | 3.0929 | 980.21 | 1.3094 |
| 0.876 | CPU | 0.7316 | 0.0957 | 0.5013 | 1.6704 | 984.97 | 1.5891 |
| | GPU | 0.7317 | 0.0957 | 0.5005 | 1.6672 | 984.85 | 1.5891 |

Pass criteria (checked in code): after 1 step, density < 1e-4 relative and position < 1e-5 m;
at each checkpoint, centre of mass within 1 cm, mean speed within 5 %, mean density within 0.5 %.
Mean density stays below 1000 because surface and wall particles lack neighbours (see
`docs/theory-sph.md`, section 8).
