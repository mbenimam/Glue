# From Navier-Stokes to SPH

This note derives the equations implemented in `apps/sph` (solver: `apps/sph/SphSolver.cpp`,
kernels: `engine/src/physics/SphKernels.h`, neighbour search: `engine/src/spatial/HashGrid.h`).
It starts from the continuum equations and ends with the exact loops that run every time step.

## 1. The continuum equations

A fluid is described by a density field `rho(x, t)`, a velocity field `u(x, t)` and a pressure
field `p(x, t)`. Two conservation laws govern them.

**Mass (continuity).** Mass is neither created nor destroyed. Following a small blob of fluid
along its path, its density changes only if the blob is squeezed or stretched:

```
D rho / Dt = - rho (div u)
```

`D/Dt` is the *material derivative*: the rate of change seen by an observer that moves with the
fluid, `D/Dt = d/dt + u . grad`. For an incompressible fluid such as water, `rho` stays constant
and the equation reduces to `div u = 0`.

**Momentum (Newton's second law per unit volume).** A blob accelerates because of the forces
acting on it: pressure pushes from high to low pressure, viscosity drags it towards the velocity of
its surroundings, gravity pulls it down:

```
D u / Dt = - (1 / rho) grad p  +  nu lap u  +  g
```

`nu = mu / rho` is the kinematic viscosity, `lap` the Laplacian.

The equations are written with `D/Dt` on purpose. On a fixed grid (the Eulerian view) the
`u . grad` part becomes an advection term that is hard to discretise well. SPH takes the
Lagrangian view instead: the unknowns are carried by **particles that move with the fluid**, so
`D/Dt` is just the time derivative of a particle's own quantity, and advection is exact: the
particle simply moves.

## 2. Kernel interpolation

Each particle `j` carries a mass `m_j`, a position `x_j`, a velocity `v_j`, and gets a density
`rho_j` and pressure `p_j`. To evaluate a field anywhere we need a way to go from these scattered
samples back to a smooth function.

Start from an exact identity with the Dirac delta:

```
A(x) = integral A(x') delta(x - x') dx'
```

Replace the delta by a smooth bump `W(r, h)`, the *smoothing kernel*, which is positive, radially
symmetric, zero beyond the *support radius* `h`, and integrates to 1:

```
A(x) ~ integral A(x') W(x - x', h) dx'
```

This is a weighted average of `A` over a ball of radius `h`. Finally replace the integral by a
sum over particles. Each particle stands for a small volume `V_j = m_j / rho_j`:

```
A(x) ~ sum_j (m_j / rho_j) A_j W(x - x_j, h)                         (1)
```

Derivatives only hit the kernel, which we know analytically:

```
grad A(x) ~ sum_j (m_j / rho_j) A_j grad W(x - x_j, h)
lap  A(x) ~ sum_j (m_j / rho_j) A_j lap  W(x - x_j, h)
```

This is the whole trick of SPH: spatial derivatives of the unknown fields become sums of known
kernel derivatives over the neighbours within `h`.

Notation below: `W_ij = W(x_i - x_j, h)`, `r_ij = x_i - x_j`, `r = |r_ij|`, and `grad W_ij` is the
gradient with respect to `x_i`.

## 3. Density: mass conservation for free

Put `A = rho` into (1). The `rho_j` cancel:

```
rho_i = sum_j m_j W_ij                                               (2)
```

(the sum includes `j = i`). This *summation density* is the discrete continuity equation: as long
as particle masses are constant, mass is conserved exactly, and density rises when particles crowd
together and falls when they spread out. No separate equation is integrated.

In the code all particles have the same mass. It is chosen so that a particle in the middle of the
initial lattice (spacing `d`) has exactly the rest density: `m = rho0 / sum_lattice W`. Otherwise
the fluid would start slightly compressed or stretched and jump on the first step.

## 4. Pressure force

Applying the gradient rule naively, `grad p_i ~ sum_j (m_j / rho_j) p_j grad W_ij`, gives a force
that is not symmetric: the push of `j` on `i` differs from the push of `i` on `j`, so momentum is
not conserved and a still fluid can start drifting. The standard fix uses the identity

```
grad(p / rho) = grad p / rho - (p / rho^2) grad rho
=>  grad p / rho = grad(p / rho) + (p / rho^2) grad rho
```

and discretises both terms on the right with the SPH gradient. Collecting terms:

```
a_i^pressure = - sum_j m_j (p_i / rho_i^2 + p_j / rho_j^2) grad W_ij           (3)
```

Since `grad W_ij = - grad W_ji`, the pair `(i, j)` exerts equal and opposite forces on each other,
along the line between them. Momentum is conserved exactly.

## 5. Viscosity force

For `nu lap u` the same naive rule has two problems: the Laplacian of a kernel changes sign (it
can *create* velocity differences instead of damping them) and the result is again not symmetric.
Using velocity *differences* and a kernel whose Laplacian is positive everywhere fixes both:

```
a_i^viscosity = (mu / rho_i) sum_j m_j (v_j - v_i) / rho_j  lap W_ij            (4)
```

The pair term is proportional to `v_j - v_i`, so viscosity pulls the two velocities together and
never adds energy. The app also offers **XSPH**, a cheaper smoothing that blends each particle's
velocity with its neighbours' after the update:

```
v_i <- v_i + eps sum_j (2 m_j / (rho_i + rho_j)) (v_j - v_i) W_ij
```

It removes particle-scale noise while barely affecting the bulk flow (`eps` of 0.01 to 0.1).

## 6. The kernels

Different terms want different kernel properties, so three kernels are used, all with support `h`.
Each constant is chosen so that `integral_0^h 4 pi r^2 W(r) dr = 1`.

**poly6**, for density:

```
W_poly6(r) = 315 / (64 pi h^9) * (h^2 - r^2)^3          0 <= r <= h
```

It depends on `r^2` only, so the density loop needs no square root. Normalisation, with `x = r/h`:
`integral_0^1 x^2 (1 - x^2)^3 dx = 1/3 - 3/5 + 3/7 - 1/9 = 16/315`, so the integral is
`4 pi h^9 * 16/315 = 64 pi h^9 / 315`, which is where the constant comes from.

**spiky**, for pressure:

```
W_spiky(r)   =  15 / (pi h^6) * (h - r)^3
dW_spiky/dr  = -45 / (pi h^6) * (h - r)^2
grad W_spiky = dW/dr * r_ij / r
```

Why not poly6 for pressure too? Its gradient goes to zero as `r -> 0`: two particles that get
very close stop repelling each other and clump. The spiky gradient is largest at `r = 0`.
Normalisation: `integral_0^1 x^2 (1 - x)^3 dx = 2! 3! / 6! = 1/60`, so
`4 pi h^6 / 60 = pi h^6 / 15`.

**viscosity**, for (4):

```
W_visc(r)     = 15 / (2 pi h^3) * ( -r^3 / (2 h^3) + r^2 / h^2 + h / (2 r) - 1 )
lap W_visc(r) = 45 / (pi h^6) * (h - r)
```

For a radial function `lap W = (1 / r^2) d/dr (r^2 dW/dr)`; working it out gives the linear form
above, positive everywhere inside the support, which is exactly what (4) needs.

`tests/test_physics.cpp` checks all three normalisations by numerical radial integration, that
`dW_spiky/dr` is the derivative of `W_spiky` (finite differences) and that the closed-form
Laplacian matches `(1 / r^2) d/dr (r^2 dW/dr)` computed numerically.

## 7. Closing the system: where does pressure come from?

Equations (2) to (4) give accelerations once `p_i` is known. For a liquid, pressure is whatever it
takes to keep the density at `rho0`. There are two families of answers; the app implements one
of each.

### 7a. Weakly compressible SPH (WCSPH)

Treat the liquid as very stiff but slightly compressible, with an equation of state. The Tait
equation is used for water:

```
p = B ( (rho / rho0)^gamma - 1 ),     gamma = 7,     B = rho0 c0^2 / gamma
```

`B` is chosen so that `dp/drho = c0^2` at rest: `c0` is the speed of sound in the model fluid.
Density fluctuations scale like the squared Mach number, `|rho - rho0| / rho0 ~ (v / c0)^2`, so
`c0 = 10 v_max` keeps them near 1 %. The real speed of sound in water (1480 m/s) would force tiny
time steps; a numerical `c0` of a few tens of m/s is enough for a 1 m dam.

Pressure is clamped at zero. Near the free surface the density sum is missing neighbours, which
would give a negative pressure pulling particles together (the *tensile instability*).

The cost is the time step. Information travels at `c0`, and must not cross more than a fraction of
the kernel per step (CFL condition):

```
dt <= lambda h / c0,     lambda ~ 0.4
```

With `h = 4.4 cm` and `c0 = 40 m/s` that is 0.44 ms.

### 7b. Predictive-corrective incompressible SPH (PCISPH)

Instead of an equation of state, PCISPH (Solenthaler and Pajarola, 2009) iterates: guess
pressures, predict where particles would go, measure the resulting density error, correct the
pressures, repeat.

Linearise the density change caused by small displacements `dx`:

```
rho_i(t + dt) = sum_j m W(x_i + dx_i - x_j - dx_j)
             ~ rho_i(t) + sum_j m grad W_ij . (dx_i - dx_j)
```

Assume every particle in the neighbourhood carries the same unknown pressure `p` and that densities
are close to `rho0`. From (3), the pressure force on `i` is `F_i = -m^2 (2 p / rho0^2) sum_j grad W_ij`
and moves it by `dx_i = dt^2 F_i / m`. Each neighbour `j` feels the opposite of `i`'s push,
`dx_j = dt^2 m (2 p / rho0^2) grad W_ij`. Substituting:

```
Delta rho_i = - p * 2 dt^2 m^2 / rho0^2 * ( (sum_j grad W_ij) . (sum_j grad W_ij) + sum_j grad W_ij . grad W_ij )
```

To cancel a predicted density error `rho*_i - rho0`, set `Delta rho_i = -(rho*_i - rho0)`, giving

```
p_i += delta (rho*_i - rho0),     delta = 1 / ( beta ( sum grad W . sum grad W + sum grad W . grad W ) ),
beta = 2 (dt m / rho0)^2
```

`delta` depends only on the kernel, the spacing and `dt`, so it is computed once on a full
prototype neighbourhood (the initial lattice) and reused. Each time step:

```
compute rho_i, non-pressure accelerations a_i^np (gravity, viscosity); p_i = 0, a_i^p = 0
repeat
    v*_i = v_i + dt (a_i^np + a_i^p);   x*_i = x_i + dt v*_i           predict
    rho*_i = sum_j m W(x*_i - x*_j)                                     measure
    p_i = max(p_i + delta (rho*_i - rho0), 0)                           correct
    a_i^p = - m sum_j (p_i + p_j) / rho0^2 grad W_ij                    (3) with rho ~ rho0
until mean compression < eta (0.5 %) and at least 3 iterations
v_i += dt (a_i^np + a_i^p);   x_i += dt v_i
```

Each iteration costs about as much as a WCSPH step without the neighbour search, but the time step
is only bounded by the flow speed (`dt <= lambda h / v_max`), not by an artificial `c0`. On the
dam break this is ~9x larger, for 5 to 10 iterations. `docs/sph-benchmarks.md` has the measured
trade-off.

## 8. Time integration and walls

The update is *symplectic Euler*: velocity first, then position with the new velocity,

```
v_i(t + dt) = v_i(t) + dt a_i(t)
x_i(t + dt) = x_i(t) + dt v_i(t + dt)
```

which, unlike explicit Euler, does not gain energy on oscillating systems.

The container is a box. A particle (radius `d/2`) that leaves it is put back on the wall; its
velocity component into the wall is reflected and scaled by the *restitution* (0 = inelastic,
1 = elastic), and the tangential components lose a small friction fraction. This is the simplest
boundary. Its known weakness: a particle next to a wall is missing the neighbours that would be
on the other side, so its density is under-estimated and the first layer settles a bit closer to
the wall than the rest. Boundary particles that contribute to the density sum fix this; they are
not implemented here.

## 9. Finding neighbours

All the sums run over particles within `h`. Testing every pair is `O(n^2)`. A uniform grid of
cubic cells of side `h` guarantees that every neighbour of a point lies in the 3 x 3 x 3 block of
cells around it. The grid is stored as a hash table (`HashGrid`): integer cell coordinates
`(ix, iy, iz)` are hashed to one of `2^k` buckets, and the table is built by counting sort:

1. hash every particle to a bucket and count the particles per bucket;
2. an exclusive prefix sum of the counts gives where each bucket starts in a sorted array;
3. scatter the particle indices into their buckets.

A query visits the (distinct) buckets of the 27 cells and keeps the candidates within `h`. The
build is `O(n)`, and each query costs about `27 x (particles per cell)` distance tests. The GPU
port runs the same three steps with atomics, and a single work group scans the counts.

## 10. Summary: one WCSPH step as implemented

```
build hash grid, neighbour lists
rho_i = m sum_j W_poly6(r_ij)
p_i   = max(B ((rho_i / rho0)^7 - 1), 0)
a_i   = g + (mu / rho_i) sum_j m (v_j - v_i) / rho_j lap W_visc(r_ij)
          - sum_j m (p_i / rho_i^2 + p_j / rho_j^2) grad W_spiky(r_ij)
v*_i  = v_i + dt a_i
v_i   = v*_i + eps sum_j 2m / (rho_i + rho_j) (v*_j - v*_i) W_poly6(r_ij)      (XSPH)
x_i  += dt v_i;  clamp to the box with restitution
```
