# EOS interpolation (single-variable, 3D `(ρ, T, Yₑ)`)

> Leaf spec. Self-contained: an agent can implement the EOS single-point interpolator from this file alone. It restates only the conventions it uses and references `fortran-parity-and-tolerances.md` (numeric contract) and `amrex-device-interface.md` (device contract) for the shared cross-cutting details. `README.md` is canonical if any restated convention here conflicts with it.

## Purpose & scope

This spec defines the device-callable, single-point interpolation of one EOS dependent variable over the 3D thermodynamic state `(ρ, T, Yₑ)`, and the matched routine that additionally returns the three partial derivatives `(∂/∂ρ, ∂/∂T, ∂/∂Yₑ)`. The interpolation is trilinear in log-stored space with ρ and T located in log space, Yₑ in linear space, and the physical value recovered by `10**(...) - offset`.

In scope:
- The single-point evaluate contract: `(ρ, T, Yₑ) → value` for one dependent variable.
- The single-point evaluate-and-differentiate contract: `(ρ, T, Yₑ) → value, (∂value/∂ρ, ∂value/∂T, ∂value/∂Yₑ)`.
- Units, valid ranges, the log/linear axis split, the offset recovery, column-major table indexing.
- Boundary/out-of-range/NaN behavior (bit-for-bit with weaklib).
- The self-contained closed-form checks and the `1e-10` derivative relaxation with rationale.

Out of scope:
- Recovering T from a dependent variable (the inversion direction) — see `eos-inversion.md`.
- Interpolating all 15 dependent variables in one call, the struct-API forms, and the multi-point array forms (the device contract here is the scalar `_Point` form; an array form is the host-level `ParallelFor` wrapper over it, see `amrex-device-interface.md`).
- The on-disk HDF5 layout the table/axes are read from — see `table-format-and-io.md`.
- Opacity channels — see the `opacity-*` specs.

## Source of truth

Pinned weaklib commit: see `weaklib_commit` in `specs/fixtures/tables.provenance`.

- `weaklib/Distributions/Library/wlInterpolationModule.F90` — the named generator-of-record `_Point` routines:
  - `LogInterpolateSingleVariable_3D_Custom_Point` (subroutine at `wlInterpolationModule.F90:1640-1707`) — the single-point evaluate workhorse. Signature `( D, T, Y, Ds, Ts, Ys, OS, Table, Interpolant )`: scalar query `(D,T,Y)`, axis arrays `Ds`/`Ts`/`Ys`, additive offset `OS`, log-stored `Table(iD,iT,iY)`, scalar `Interpolant` out. It clamps the log/log/linear bracket indices, computes unclamped deltas, reads the 8 corners, evaluates the trilinear sum in log space, and returns `10**(...) - OS`.
  - `LogInterpolateDifferentiateSingleVariable_3D_Custom_Point` (subroutine at `wlInterpolationModule.F90:1814-1844`) — the matched evaluate-and-differentiate routine. Signature `( D, T, Y, Ds, Ts, Ys, OS, Table, Interpolant, Derivative )` with `Derivative(1:3)` = `(∂/∂ρ, ∂/∂T, ∂/∂Yₑ)`; it uses `GetIndexAndDelta_Log`/`_Lin` plus the per-axis chain-rule scale factors `aD`/`aT`/`aY` and `LinearInterpDeriv3D_3DArray_Point`.
- `weaklib/Distributions/Library/wlInterpolationUtilitiesModule.F90` — the shared primitives these call: `GetIndexAndDelta_Log` / `GetIndexAndDelta_Lin`, the `TriLinear` basis and its partials `dTriLineardX1/2/3`, and the leaf routines `LinearInterp3D_3DArray_Point` / `LinearInterpDeriv3D_3DArray_Point` that own the `10**(...) - OS` recovery and the `(value+OS)·a·∂/∂d` derivative assembly.

These `_Point` routines are the authoritative oracle that defines "correct" for this leaf (see `fortran-parity-and-tolerances.md`).

## Inputs & outputs

Value type is `double` throughout (weaklib `dp = 8`); see `fortran-parity-and-tolerances.md`. Device contract (qualifiers, `Gpu::DeviceVector<double>` residency, `double const*` table passing, `ParallelFor` launch) is `amrex-device-interface.md`.

### Independent variables (query), order and units

The query is the scalar triple in this fixed order:

| Slot | Symbol | Quantity | Unit | Interp space |
|---|---|---|---|---|
| 1 | ρ | mass density | grams per cm³ | **log** |
| 2 | T | temperature | K (Kelvin) | **log** |
| 3 | Yₑ | electron fraction | dimensionless | **linear** |

This `(ρ, T, Yₑ)` order, these units, and the `LogInterp = [1, 1, 0]` log/log/linear split are the on-disk `ThermoState` convention (verified in the reference table: `Names = [Density, Temperature, Electron Fraction]`, `Units = [Grams per cm^3, K, (none)]`, `LogInterp = [1,1,0]`).

### Axis arrays and table

- `Ds(1:nD)`, `Ts(1:nT)`, `Ys(1:nY)` — the strictly monotone-ascending grid-node coordinates for ρ, T, Yₑ (raw physical values, not pre-`LOG10`'d; this routine takes the log internally).
- `OS` — the scalar additive offset for the chosen dependent variable.
- `Table` — the log-stored values `log10(physical + OS)` over the 3D grid, indexed `(iD, iT, iY)` in Fortran column-major order: as a flat `double const*` the element `Table(iD,iT,iY)` (0-based) is `table[ iD + nD*( iT + nT*iY ) ]` (see `amrex-device-interface.md`).

### Outputs

- Evaluate: `Interpolant` — the recovered physical value of the dependent variable in its own units.
- Differentiate: additionally `Derivative(1:3) = (∂value/∂ρ, ∂value/∂T, ∂value/∂Yₑ)` in physical units per unit of each independent variable.

### Reference table (anchors the regression-suite checks)

`wl-EOS-SFHo-15-25-50.h5`, pinned by path + `sha256` in `specs/fixtures/tables.provenance`; its structure is committed at `specs/fixtures/wl-EOS-SFHo-15-25-50.h5ls`. The thermodynamic axes are stored under group `/ThermoState` (`/ThermoState/Density` `[185]`, `/ThermoState/Temperature` `[81]`, `/ThermoState/Electron Fraction` `[30]`), and each dependent variable under group `/DependentVariables` (e.g. `/DependentVariables/Pressure`, shape `{30, 81, 185}` in `h5ls` C-order = Fortran `(185, 81, 30) = (nρ, nT, nYe)`), with per-variable additive offsets in `/DependentVariables/Offsets` `[15]`. Real grid extents in this table: ρ ∈ [1.66054e3, 3.16409e15] g/cm³, T ∈ [1.16045e9, 1.83919e12] K, Yₑ ∈ [0.01, 0.6]. Full on-disk contract: `table-format-and-io.md`.

## Correctness requirements

### The interpolation formula

For a query `(D, T, Y)` strictly inside the grid, with brackets and deltas (see `fortran-parity-and-tolerances.md`):

- `iD` from the log bracket on `Ds`, `dD = log10(D/Ds(iD)) / log10(Ds(iD+1)/Ds(iD))`.
- `iT` from the log bracket on `Ts`, `dT = log10(T/Ts(iT)) / log10(Ts(iT+1)/Ts(iT))`.
- `iY` from the linear bracket on `Ys`, `dY = (Y - Ys(iY)) / (Ys(iY+1) - Ys(iY))`.

Read the 8 corner log-values `p_{abc} = Table(iD+a, iT+b, iY+c)` for `a,b,c ∈ {0,1}` and form the trilinear interpolant in log space, then recover:

```
Interpolant = 10**( trilinear(p000..p111, dD, dT, dY) ) - OS
```

where `trilinear` is the standard 8-corner form (linear in `dD`, then `dT`, then `dY`). The result must match `LogInterpolateSingleVariable_3D_Custom_Point` on identical inputs at the default parity tier (`rtol 1e-12`, `atol 1e-30`).

### Derivatives

The matched differentiate routine returns:

```
∂Interpolant/∂X_k = (Interpolant + OS) · a_k · (∂/∂d_k of the trilinear-in-log form)
```

with per-axis scale factors `a_D = 1/( D · log10(Ds(iD+1)/Ds(iD)) )`, `a_T = 1/( T · log10(Ts(iT+1)/Ts(iT)) )` (log axes) and `a_Y = ln10 / ( Ys(iY+1) - Ys(iY) )` (linear axis), `ln10 = LOG(10)`. The factor `(Interpolant + OS)` reconstitutes `10**(trilinear)`. Derivatives must match `LogInterpolateDifferentiateSingleVariable_3D_Custom_Point` at the **relaxed tier `1e-10`**.

**Rationale for the `1e-10` derivative relaxation:** the derivative is a product of the reconstituted exponential `(value+OS)`, a transcendental scale factor (`log10` of a node ratio), and a finite-difference of the trilinear form — an interpolation-of-an-interpolation whose order-of-operations and transcendental rounding accumulate beyond the `1e-12` single-value tier. `1e-10` bounds that accumulation while still catching real regressions; the single recovered value itself stays at the default `1e-12` tier.

### Boundary / out-of-range / NaN (bit-for-bit with weaklib)

Replicate the permissive behavior exactly (see `fortran-parity-and-tolerances.md`):

- Out-of-range `(ρ, T, Yₑ)`: clamp the bracket index to the edge cell `[1, n-1]`, do **not** clamp the delta. A below-range query gives `d < 0`, above-range gives `d > 1`, producing **linear extrapolation from the edge cell** — not an error, not a result clamp, not a NaN. No range check; range enforcement is a consumer responsibility.
- Non-positive `ρ` or `T` (or any non-positive node): `log10` of a non-positive argument yields NaN, which propagates silently to `Interpolant`. The C++ result must be NaN in the same circumstances. Yₑ (linear axis) does not log, so a non-positive Yₑ extrapolates rather than NaNs.

### Conventions restated (the subset this leaf uses)

- ρ, T interpolated in log space; Yₑ in linear space.
- Universal recovery `value = 10**(stored) - offset`.
- Fortran column-major table: first index (ρ) fastest-varying; flat offset `iD + nD*(iT + nT*iY)`.
- `double` (`dp = 8`) throughout.

## Verification

### Self-contained checks (the regression suite)

Run against both synthetic in-suite tables and the real reference table `wl-EOS-SFHo-15-25-50.h5`:

1. **Affine-in-log exactness (machine-precision tier `~1e-14`).** Build a synthetic 3D table whose stored value is an exact affine function of `(log10 ρ, log10 T, Yₑ)`, i.e. `stored(iD,iT,iY) = c0 + cρ·log10(Ds(iD)) + cT·log10(Ts(iT)) + cY·Ys(iY)`. Trilinear-in-log interpolation must reproduce `10**(affine) - OS` exactly at *any* interior query, not just at nodes. The constant table (`cρ=cT=cY=0`) is the degenerate case.
2. **Node identity (machine-precision tier).** Querying exactly at a grid node `(Ds(i), Ts(j), Ys(k))` returns that node's recovered value `10**(Table(i,j,k)) - OS`. Verify on the real table too.
3. **Derivative chain-rule scale factors (relaxed tier `1e-10`).** On the affine-in-log table the analytic derivatives have a closed form; the returned `Derivative(1:3)` must match it. Cross-check against a tight central finite-difference at the relaxed tier on the real table.
4. **Boundary extrapolation (no tolerance / exact relation).** A query just outside an edge must equal the edge cell's linear extrapolation (compare against the same trilinear formula evaluated with the unclamped delta) — confirming clamp-index-but-not-delta.
5. **NaN propagation (NaN-equality).** A query with non-positive ρ or T must produce a NaN `Interpolant`.

### Mechanical (validator)

`bash specs/tools/validate_specs.sh` (default mode) asserts: the 7 mandated sections in order; both `_Point` routine names present; the `1e-10` relaxation present; the cited weaklib source-of-truth paths resolve; and the documented `/ThermoState/Density` and `/DependentVariables/Pressure` structures appear in the committed `wl-EOS-SFHo-15-25-50.h5ls` snapshot with the table named in this spec.

## Implementation freedom

- The internal multilinear evaluation order and whether the 8 corners are read into locals or indexed in place.
- Whether evaluate and evaluate-and-differentiate share code or are separate.
- The exact `_Point` signature argument grouping (subject to the scalar/allocation-free/`double const*`-plus-extents contract in `amrex-device-interface.md`).
- Whether the multi-point array form is a hand-written loop or a `ParallelFor` over the `_Point` core.
- Any caching/precomputation of node ratios, provided results meet the stated tolerances.

## Open questions / assumptions

- **Concrete per-variable offsets (assumption, non-blocking).** The 15 `Offsets` values for this table live only in `/DependentVariables/Offsets` of the `.h5` file. This spec pins the recovery contract; the table supplies the numbers. The self-contained exactness checks use synthetic tables whose offsets the suite chooses, so they do not depend on the unknown production offsets.
- **Dependent-variable index assignment (assumption, non-blocking).** The mapping of variable slot → physical quantity (e.g. Pressure, Entropy, …) is authoritative via the `/DependentVariables/i*` datasets and the `Names` ordering in the file, not hard-coded here. This spec's contract is per-variable (one `OS` + one log-stored sub-table); which slot is which is read from the table per `table-format-and-io.md`.
