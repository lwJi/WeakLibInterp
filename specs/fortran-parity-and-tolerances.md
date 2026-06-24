# Fortran parity and tolerances (cross-cutting numeric contract)

> Cross-cutting spec. Leaf specs (`eos-interpolation`, `eos-inversion`, `opacity-emab-iso`, `opacity-nes-pair`, `opacity-brem`) reference this file for the shared numeric contract and restate, in their own sections, only the conventions they actually use. If a leaf spec and this file ever disagree, `README.md` is the canonical arbiter; absent that, this file governs the numeric contract.

## Purpose & scope

This spec defines the numeric contract every interpolation/inversion leaf inherits: the named authoritative oracle, the two-tier tolerance scheme and where each tier relaxes, the universal log-space-plus-offset storage/recovery convention, the column-major memory convention, and the bit-for-bit boundary/NaN replication policy.

In scope:
- The generator-of-record (the named Fortran `_Point` routines) that defines "correct".
- The numeric tolerance tiers (`rtol`, `atol`, the relaxed tier, the machine-precision exactness tier) and the rule for which applies where.
- The universal `value = 10**(stored) - offset` log-space convention and which axes are interpolated in log vs. linear space vs. not interpolated at all.
- Fortran column-major memory layout (first index fastest-varying) and the C-shape reversal it implies.
- The permissive boundary/out-of-range/NaN behavior to replicate bit-for-bit.
- The regression scheme: self-contained closed-form/invariant checks as the regression suite.

Out of scope:
- The per-channel geometry, independent-variable lists, and physics invariants (each leaf spec owns its own).
- The device/AMReX interface (see `amrex-device-interface.md`).
- The on-disk HDF5 group/dataset layout (see `table-format-and-io.md`).
- The regression harness/file-layout (see `regression-suite-design.md`).

## Source of truth

The authoritative behavior is the weaklib Fortran source at the pinned commit recorded in `specs/fixtures/tables.provenance` (`weaklib_commit`). The two files that define the shared interpolation arithmetic, the storage convention, the chain-rule derivative factors, and the boundary/NaN behavior:

- `weaklib/Distributions/Library/wlInterpolationUtilitiesModule.F90` — the stateless scalar primitives: `Index1D` / `Index1D_Lin` / `Index1D_Log` (index search and edge handling), `GetIndexAndDelta_Lin` / `GetIndexAndDelta_Log` (fractional in-cell deltas), the `Linear` / `BiLinear` / `TriLinear` / `TetraLinear` / `PentaLinear` multilinear basis and their analytic partial derivatives, and the `LinearInterp*D_*DArray_Point` / `LinearInterpDeriv*_Point` leaf routines that own the `10**(...) - OS` recovery.
- `weaklib/Distributions/Library/wlInterpolationModule.F90` — the public `LogInterpolate*` / `SumLogInterpolate*` entry points (array and `_Point` forms), the device-annotated private copies of the primitives, and the constant `ln10 = LOG(10)`.

The authoritative oracle that defines "correct" is the matched `_Point` (single-query, scalar, allocation-free) routine for each leaf, named in that leaf's "Source of truth" section.

## Inputs & outputs

This spec does not define a callable surface; it defines conventions the leaf surfaces obey.

### Floating-point type

Every stored value, axis coordinate, offset, delta, interpolant, and derivative is IEEE-754 binary64 (`double`; weaklib `REAL(dp)`, `dp = 8`). The correctness-bearing value type is pinned to `double` regardless of how `amrex::Real` is configured (see `amrex-device-interface.md`). On-disk reals are `H5T_NATIVE_DOUBLE` (see `table-format-and-io.md`).

### Log-space + offset storage and recovery (universal)

Every tabulated quantity — every EOS dependent variable and every opacity kernel — is stored on disk as

```
stored(node) = log10( physical(node) + offset )
```

where `offset` (the per-quantity additive `Offsets` value) is chosen at table-generation time so the argument of the log is strictly positive even for quantities that can be ≤ 0 (e.g. internal energy, chemical potentials). Interpolation is performed multilinearly in this log-stored space, and the physical value is recovered as the final step of every leaf routine:

```
physical = 10**( multilinear_in_log_space(corner_values, deltas) ) - offset
```

The concrete `offset` numbers live only in the production `.h5` files' `Offsets` datasets; specs name the tables, fixtures carry the numbers.

### Which axes are log, linear, or not interpolated

| Axis kind | Located / interpolated as | Examples |
|---|---|---|
| log | bracket + delta in `log10` space | density ρ, temperature T, neutrino energy E, degeneracy η = μₑ/kT |
| linear | bracket + delta in linear space | electron fraction Yₑ |
| not interpolated | used directly as an integer table index | discrete moment index, kernel-component index, neutrino-species index, and (for `_Aligned` energy axes) the pre-collapsed energy indices |

Each leaf spec restates the subset it uses.

### Bracket / delta computation

For an ascending axis array `Xs(1:n)` and query `X`, the O(1) bracket-and-delta is:

- linear (`GetIndexAndDelta_Lin`):
  `i = clamp( 1 + floor( (n-1)·(X - Xs(1)) / (Xs(n) - Xs(1)) ), 1, n-1 )`,
  `d = (X - Xs(i)) / (Xs(i+1) - Xs(i))`.
- log (`GetIndexAndDelta_Log`):
  `i = clamp( 1 + floor( (n-1)·log10(X/Xs(1)) / log10(Xs(n)/Xs(1)) ), 1, n-1 )`,
  `d = log10(X/Xs(i)) / log10(Xs(i+1)/Xs(i))`.

The bracket index `i` is clamped to `[1, n-1]`; the delta `d` is **not** clamped (see boundary policy below). For `_Custom` routines whose coordinates arrive pre-`LOG10`'d, the *linear* bracket formula is applied to the already-logarithmic coordinate (this is equivalent to the log formula on the raw coordinate); each leaf spec states which of its coordinates arrive pre-`LOG10`.

### Derivative chain-rule factors

A leaf that returns derivatives differentiates the multilinear-in-log form and multiplies by:

- the reconstitution factor `(physical + offset) = 10**(multilinear_in_log_space)` — the `d(10^f)/df` factor with `ln10` folded into the per-axis scale below; and
- a per-axis scale factor:
  - log axis: `a = 1 / ( X · log10( Xs(i+1)/Xs(i) ) )`
  - linear axis: `a = ln10 / ( Xs(i+1) - Xs(i) )`, with `ln10 = LOG(10)`.

So `∂(physical)/∂X_k = (physical + offset) · a_k · (∂/∂d_k of the multilinear-in-log form)`.

## Correctness requirements

### Tolerance tiers

The C++ port performs the same IEEE-754 double arithmetic, but order-of-operations and transcendental (`10**`, `log10`) implementation differences are unavoidable; derivatives and inversion (interpolation-of-an-interpolation) accumulate more. Four tiers, each with a stated home:

| Tier | Bound | Applies to |
|---|---|---|
| Default parity | relative `rtol = 1e-12` with absolute floor `atol = 1e-30` | the definition of correctness for a single interpolated value against the named `_Point` oracle |
| Relaxed | `1e-10` | analytic derivatives and inversion-recovered T; the round-trip inversion invariant |
| Machine-precision exactness | `~1e-14` (a few ULP) | self-contained exactness checks that have closed-form exact answers: affine-in-log tables, constant tables, node identity |
| (no tolerance) | exact / NaN-equality | boundary index behavior, NaN propagation, inversion integer error codes |

The pass/fail comparison for the parity and relaxed tiers is the mixed relative/absolute form:

```
| got - expected | <= rtol · |expected| + atol
```

with `(rtol, atol)` = `(1e-12, 1e-30)` for the default tier and `(1e-10, 1e-30)` for the relaxed tier. A leaf spec may invoke a relaxed tier only with a stated rationale; it may not tighten below the default tier.

### Boundary / out-of-range / NaN — replicate bit-for-bit

The reference interpolators are permissive, not defensive, and the C++ port must reproduce this exactly:

- **Out-of-range query:** the bracket index is clamped to the boundary cell `[1, n-1]`, but the fractional delta is **not** clamped. A below-range query yields `d < 0`, an above-range query yields `d > 1`, and the multilinear kernel turns these into **linear extrapolation from the edge cell** — not an error, not a clamp of the result, not a NaN. There is no range check in the interpolation path; range enforcement is a *consumer* responsibility (mirroring thornado's `Eos_MinD` / `QueryOpacity_*` density guards).
- **Non-positive log argument:** `log10(X)` for `X ≤ 0` (or any `Xs(i) ≤ 0`) produces NaN, which propagates silently through the entire chain to the result. There is no guard. The C++ port must produce a NaN result in the same circumstances (verified by `NaN`-propagation checks, asserted via "result is NaN", not by value equality).
- **No result clamping, no abort, no error injection** anywhere in the interpolation path. The only input validation in the weaklib surface is the array wrappers' size-consistency check (which the scalar `_Point` contract does not have) and the EOS inversion module's integer error-code protocol (see `eos-inversion.md`).

### Memory layout — Fortran column-major

The reference tables are written by the Fortran HDF5 bindings in column-major order: the Fortran **first index is fastest-varying** (most contiguous). A Fortran array declared `Table(n0, n1, ..., n_{D-1})` and indexed `Table(i0, i1, ..., i_{D-1})` maps to the flat offset

```
offset = i0 + n0*( i1 + n1*( i2 + ... + n_{D-2}*( i_{D-1} ) ... ) )
```

(0-based indices). Equivalently, to index the same data naturally in C as `data[i_{D-1}]...[i1][i0]`, the C shape is the reverse of the Fortran shape. `h5ls -r` reports dataset dimensions in C order (slowest-varying first), so an `h5ls` dimension list is the **reverse** of the Fortran logical shape; the committed `specs/fixtures/*.h5ls` snapshots are read with that reversal in mind. This same convention governs in-kernel indexing of the flat `double const*` table (see `amrex-device-interface.md`).

### The named oracle

For every leaf, "correct" for a single query is, by definition, the value the matched Fortran `_Point` routine produces on identical inputs (same axis arrays, same offset, same table values) at the pinned weaklib commit. The leaf spec names that routine by file and line. The self-contained checks (below) enforce this; the `_Point` routine satisfies them by construction.

## Verification

### The regression scheme — self-contained checks

- **Self-contained checks (the regression suite).** Computed entirely within the C++/AMReX test build, with no external oracle, against both synthetic in-suite tables and the real production tables named by each leaf:
  - *Affine-in-log exactness:* build a table whose `stored = log10(value+offset)` is an exact affine function of the (log/linear) axis coordinates; multilinear interpolation must reproduce it to the machine-precision tier at *any* interior query, not just at nodes. The constant table is the degenerate case.
  - *Node identity:* querying exactly at a grid node returns that node's recovered value (`10**(stored) - offset`) to the machine-precision tier.
  - *Derivative checks:* the analytic-derivative chain-rule factors above, verified against the affine-in-log closed form and/or a tight finite-difference at the relaxed tier.
  - *Symmetry / closure invariants:* detailed balance (NES), crossing symmetry (Pair), the `[1,1,28/3]` density decomposition (Brem), round-trip inversion (`eos-inversion`) — each owned by its leaf spec.
  - *Boundary / NaN behavior:* clamp-index-but-not-delta extrapolation past the edge; NaN propagation on non-positive log argument; inversion error codes and `T=0`-on-failure.

### How this spec is mechanically checked

`bash specs/tools/validate_specs.sh` (default mode) asserts this file carries the 7 mandated sections in order, names the concrete tolerances `1e-12` and `1e-30`, and that its cited weaklib source-of-truth paths resolve.

## Implementation freedom

- The internal data structures, the order of floating-point operations within the multilinear kernel, and whether the innermost helpers take a pointer+strides or unpacked scalars — provided the result meets the stated tolerance tiers.
- The synthetic-table construction (sizes, axis ranges) — provided the affine-in-log / constant / node-identity properties hold by construction.
- The test framework, harness, and file layout (see `regression-suite-design.md`).

## Open questions / assumptions

- **Concrete offsets and grid extents (assumption, non-blocking).** The per-quantity `Offsets` values and the energy/η grid extents live only in the production `.h5` files. This spec pins the *contract* (`10**(stored) - offset`, which axes are log/linear); the live tables carry the numbers. Self-contained checks that need exact answers use synthetic tables whose offsets are chosen by the suite, so they do not depend on the unknown production offsets.
