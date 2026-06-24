# Regression-suite design (technical, the runnable coverage matrix)

> Technical spec. It binds every leaf spec's "Verification" section into one coherent, runnable matrix and fixes the pass/fail discipline, but defers per-channel correctness arithmetic to the leaf specs and the shared numeric contract. It references `fortran-parity-and-tolerances.md` (tolerance tiers, regression scheme, named oracle) and `amrex-device-interface.md` (the device entry-point surface under test) and restates only what the suite as a whole must guarantee. `README.md` is canonical if any restated convention here conflicts with it.

## Purpose & scope

This spec defines the regression suite as a *contract*: the self-contained verification scheme, the coverage matrix that every public device entry point must satisfy, the tolerance tiers each cell asserts at, and the assert-against-tolerance pass/fail discipline that replaces weaklib's print-only Fortran tests. It is the single place a fresh agent learns **what the Ralph loop gates on**, so no leaf spec's verification section is left as an isolated island.

In scope:
- The regression scheme: self-contained closed-form/invariant checks as the regression suite.
- The coverage matrix: every public device entry point named by the leaf specs × {in-bounds, on-edge, out-of-range, NaN-input} × {synthetic + named production tables}, and which tolerance tier each cell asserts at.
- The pass/fail discipline: every check asserts `|got − expected| ≤ rtol·|expected| + atol` (or exact / NaN-equality for the no-tolerance tier) and **fails the suite** on violation — never prints-and-passes.
- The C++/AMReX-only test-time constraint: the suite links AMReX (CPU-only, double precision) and runs entirely on host with no GPU and no Fortran/Matlab.

Out of scope:
- The per-channel interpolation arithmetic and physics invariants (each leaf spec owns its own; this spec only enumerates which entry points must be covered and how).
- The tolerance numbers' derivation and the log-space/column-major conventions (see `fortran-parity-and-tolerances.md`).
- The device/residency/launch mechanics of the entry points under test (see `amrex-device-interface.md`).
- The on-disk HDF5 reader contract (see `table-format-and-io.md`).
- The build/link configuration of AMReX and the test target (see `build-integration.md`).
- The **test framework, harness, directory layout, and assertion library** — explicitly implementation freedom (see below).

## Source of truth

The suite's authoritative behavior is the same weaklib Fortran generator-of-record the leaf specs name: the matched `_Point` (single-query, scalar, allocation-free) routine for each entry point, at the pinned weaklib commit recorded in `specs/fixtures/tables.provenance` (`weaklib_commit`). The shared definitions of "correct" and the tolerance tiers live in:

- `weaklib/Distributions/Library/wlInterpolationUtilitiesModule.F90` — the stateless scalar primitives (`GetIndexAndDelta_*`, the multilinear basis + partials, the `10**(...) − OS` recovery) every closed-form check exercises by construction.
- `weaklib/Distributions/Library/wlInterpolationModule.F90` — the public `LogInterpolate*` / `SumLogInterpolate*` `_Point` entry points that are the named oracles defining "correct".

This spec does not introduce a new oracle; it organizes the checks that the leaf specs' oracles imply into one matrix. The regression scheme and the named-oracle definition are defined in `fortran-parity-and-tolerances.md`; this spec restates the runnable shape of the scheme and the per-entry-point coverage it applies to.

## Inputs & outputs

This spec does not define a callable surface. Its "inputs" are the public device entry points enumerated by the leaf specs; its "output" is a binary pass/fail per check and an aggregate pass/fail for the suite.

### The public device entry points under test (the coverage rows)

Every public device-callable entry point named across the leaf specs is a row of the coverage matrix. The suite must cover, at minimum, exactly these (each links to the leaf spec that defines its correctness; `_Point` scalar form per `amrex-device-interface.md`):

| # | Entry point (device-callable contract) | Defining leaf spec | Geometry |
|---|---|---|---|
| 1 | EOS single-variable **evaluate** — `LogInterpolateSingleVariable_3D_Custom_Point` | [eos-interpolation](./eos-interpolation.md) | 3D `(ρ, T, Yₑ)` |
| 2 | EOS single-variable **evaluate-and-differentiate** — `LogInterpolateDifferentiateSingleVariable_3D_Custom_Point` | [eos-interpolation](./eos-interpolation.md) | 3D `(ρ, T, Yₑ)` → value + `(∂/∂ρ, ∂/∂T, ∂/∂Yₑ)` |
| 3 | EOS **inversion** — recover `T` from `(ρ, X∈{E,P,S}, Yₑ)`, the `ComputeTemperatureWith_{DEY,DPY,DSY}_*` families over the `LogInterpolateSingleVariable_2D_Custom_Point` fixed-`T`-node evaluation | [eos-inversion](./eos-inversion.md) | 3D inverse; integer error codes, `T=0`-on-failure |
| 4 | Opacity **EmAb** evaluate — `LogInterpolateSingleVariable_4D_Custom_Point` | [opacity-emab-iso](./opacity-emab-iso.md) | 4D `(E, ρ, T, Yₑ)` |
| 5 | Opacity **Iso** evaluate — the 5D `(E, moment, ρ, T, Yₑ)` channel evaluated via the 4D `_Point` kernel at a fixed integer moment | [opacity-emab-iso](./opacity-emab-iso.md) | 5D, moment index not interpolated |
| 6 | Opacity **NES** evaluate — `LogInterpolateSingleVariable_2D2D_Custom_Aligned(_Point)` | [opacity-nes-pair](./opacity-nes-pair.md) | 5D `(E′, E, kernel, T, η)`, energy indices used directly, `(T, η)` bilinear |
| 7 | Opacity **Pair** evaluate — `LogInterpolateSingleVariable_2D2D_Custom_Aligned(_Point)` | [opacity-nes-pair](./opacity-nes-pair.md) | 5D `(E′, E, kernel, T, η)`, energy indices used directly, `(T, η)` bilinear |
| 8 | Opacity **Brem** evaluate — `SumLogInterpolateSingleVariable_2D2D_Custom_Aligned` | [opacity-brem](./opacity-brem.md) | 5D `(E′, E, moment, ρ, T)`, `[1,1,28/3]` density decomposition |

This table is the **closure list**: no channel or variant may be silently uncovered. The validator (`tools/validate_specs.sh`) asserts that every public entry-point routine named in a leaf spec appears as a row here, so adding a leaf entry point without adding its coverage row fails the gate.

### The coverage columns (input regimes)

Each entry point is exercised across these four input regimes, drawn from both synthetic in-suite tables and the named production tables:

| Regime | Definition | Expected behavior (per `fortran-parity-and-tolerances.md`) |
|---|---|---|
| in-bounds | query strictly inside the grid on every axis | normal multilinear interpolation |
| on-edge | query exactly at a grid node (and exactly on a boundary node) | node identity; the boundary cell with delta `0` or `1` |
| out-of-range | query below `Xs(1)` or above `Xs(n)` on ≥1 axis | clamp bracket index, **unclamped delta** → linear extrapolation from the edge cell (no error, no result clamp); for inversion, the appropriate integer error code and `T=0` |
| NaN-input | non-positive value on a log axis (so `log10` is NaN) | NaN propagates silently to the result; for inversion, the `T=0`/error-code path |

### The reference tables (the two fixture sources)

- **Synthetic in-suite tables** — the always-on primary. The suite builds small tables in memory with closed-form properties (affine-in-log, constant, known symmetry triangles), so the suite runs with no external files. Sizes/ranges are implementation freedom provided the closed-form property holds by construction.
- **Named production tables** — pinned by path + `sha256` in `specs/fixtures/tables.provenance`, structure committed in `specs/fixtures/*.h5ls`: `wl-EOS-SFHo-15-25-50.h5` (entry points 1–3) and `wl-Op-SFHo-15-25-50-E40-{EmAb,Iso,NES,Pair,Brem}.h5` (entry points 4–8). These anchor the reader contract and the real-table invariants.

## Correctness requirements

### The regression scheme — self-contained checks

- **Self-contained checks (the regression suite).** Computed entirely within the C++/AMReX build with **no external oracle**, run against synthetic *and* real production tables:
  - *Affine-in-log exactness* (machine-precision tier `~1e-14`): a table whose `stored = log10(value+offset)` is exactly affine in the (log/linear) axis coordinates must be reproduced at *any* interior query, not just at nodes. The constant table is the degenerate case.
  - *Node identity* (machine-precision tier): querying at a grid node returns `10**(stored) − offset`.
  - *Derivative checks* (relaxed tier `1e-10`): the chain-rule factors against the affine-in-log closed form and/or a tight finite difference (entry point 2).
  - *Symmetry / closure invariants* (the tier each leaf states): detailed balance for NES, crossing symmetry for Pair, the `[1,1,28/3]` density decomposition for Brem, round-trip inversion for EOS inversion (relaxed tier `1e-10`) — **each invariant is owned and stated by its leaf spec**; this suite only requires that it be *run* as a coverage cell.
  - *Boundary / NaN behavior* (no-tolerance / NaN-equality): clamp-index-but-not-delta extrapolation; NaN propagation; inversion integer error codes and `T=0`-on-failure.
  - *Device/host equivalence and indexing round-trip* (per `amrex-device-interface.md`): a `_Point` function gives identical results called directly on host and from inside a `ParallelFor` lambda; the column-major index→offset arithmetic round-trips sentinels exactly for 3D/4D/5D shapes.

### Pass/fail discipline (the hard departure from weaklib's Fortran tests)

- Every check **asserts** and **fails the suite** on violation. The comparison is `|got − expected| ≤ rtol·|expected| + atol` for the parity/relaxed tiers, exact equality for index round-trips and inversion error codes, and "result is NaN" for NaN-propagation checks. There is no print-only or eyeball check: weaklib's Fortran unit tests mostly print numbers with no threshold (only `wlInterpolateBrem.f90` has an in-code NaN assertion) — this suite replaces that with a thresholded assertion for every cell.
- The suite's aggregate exit status is failure if any check fails.

### Tolerance tiers (referenced, not redefined)

The four tiers — default parity `rtol 1e-12` / `atol 1e-30`, relaxed `1e-10`, machine-precision exactness `~1e-14`, and exact / NaN-equality — and the rule for which applies where are defined in `fortran-parity-and-tolerances.md`. Each coverage cell asserts at the tier its row's leaf spec and the regime column dictate (interpolated value: default; derivative / inversion-T / round-trip: relaxed; closed-form Layer-1 exactness: machine-precision; boundary/NaN/error-code: exact/NaN-equality).

### C++/AMReX-only at test time

The suite links AMReX (default double-precision **CPU** configuration; see `build-integration.md`) and runs entirely on host with no GPU: the qualifier macros expand to nothing, `ParallelFor` becomes a sequential loop, and `Gpu::DeviceVector` allocates host memory. It **never builds or runs Fortran or Matlab** at test time; the named Fortran `_Point` routines are read-only sources of truth that define "correct". The correctness-bearing value type is `double` regardless of `amrex::Real`.

## Verification

A fresh agent confirms the suite itself is correctly designed by these self-contained checks:

1. **Closure (mechanical).** Every public device entry-point routine named in a leaf spec appears as a row of the coverage matrix above; running `bash specs/tools/validate_specs.sh` (default mode) asserts this and fails if a leaf names an entry point with no coverage row. This is the matrix-closure gate.
2. **Every row × every regime is realized.** For each of the 8 entry points, a check exists for each of {in-bounds, on-edge, out-of-range, NaN-input} against the synthetic table, and at least the node-identity / boundary / NaN cells additionally against the named production table.
3. **Pass/fail is real.** Deliberately perturbing an expected value (e.g. injecting a 10× error into one corner) makes the corresponding cell **fail** the suite (proving the assertion is thresholded, not print-only).
4. **No-Fortran-at-test-time.** The full suite builds and runs with only a C++ toolchain + AMReX (CPU/double); no Fortran or Matlab toolchain is invoked. Verified by building/running in this environment.
5. **Mechanical (validator).** `bash specs/tools/validate_specs.sh` (default mode) asserts this file carries the 7 mandated sections in order, names a concrete numeric tolerance, that its cited weaklib source-of-truth paths resolve, and that the coverage matrix references every leaf entry point (closure check).

## Implementation freedom

- **The test framework, assertion library, harness, and directory layout** — GoogleTest, Catch2, a hand-rolled `assert`-based driver, CMake/CTest targets, fixture directory structure: all free.
- The synthetic-table construction (sizes, axis ranges, how the affine-in-log/constant/symmetry properties are built in) — provided the closed-form property holds by construction.
- Whether checks are organized per-entry-point, per-regime, or per-table, and whether the production-table cells are gated behind a "tables present" guard — provided the closure matrix is fully realized when the tables are available.
- The reporting format for `pass` / `fail`, provided the two states are distinct.

## Open questions / assumptions

- **Production tables present, Fortran absent (assumption, non-blocking).** The named `.h5` tables are readable here (C++ HDF5 only), so the real-table cells run now; the Fortran/Matlab oracles are read-only sources of truth and are not invoked at test time. If a production table is absent on a given runner, its real-table cells may be skipped (reported distinctly from pass), but the synthetic cells — the actual gate — always run.
- **CPU-only test target (assumption, non-blocking).** No GPU is required; AMReX is built CPU-only / double precision and the suite runs on host. A GPU build is a drop-in (the device/host-equivalence cell guards against host-only divergence), but GPU execution is not verified here. See `build-integration.md`.
