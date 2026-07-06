# Opacity interpolation: evaluate-and-differentiate (EmAb/Iso 4D + NES/Pair/Brem 2D-aligned)

> Leaf spec. Self-contained: an agent can implement the opacity evaluate-and-differentiate `_Point` interpolators from this file alone. It restates only the conventions it uses and references `fortran-parity-and-tolerances.md` (numeric contract) and `amrex-device-interface.md` (device contract) for the shared cross-cutting details, and it defers the value-only recovery, table geometry, offsets, and boundary/NaN policy to the matched evaluate leaves `opacity-emab-iso.md`, `opacity-nes-pair.md`, `opacity-brem.md`. `README.md` is canonical if any restated convention here conflicts with it.

## Purpose & scope

This spec defines the device-callable, single-point **evaluate-and-differentiate** contract for the opacity channels: the routines that return the same recovered physical value as the value-only opacity leaves **plus** the partial derivatives of that value with respect to the interpolated query coordinates. It is the opacity analogue of the EOS evaluate-and-differentiate contract in `eos-interpolation.md`, and it is the caller that turns the C++ port's currently-dead `dBiLineardX1`/`dBiLineardX2` partials (`src/core/wli_interp.H:159-168`) into live code and mandates the still-absent `dTetraLineardX1..4` tetralinear partials.

The weaklib oracle expresses opacity derivatives through two derivative cores, each the difference of two lower-dimensional multilinear evaluations at the two bracketing nodes of the differentiated axis, scaled by a per-axis chain-rule factor and the reconstituted exponential `(value + offset)`:

- **4D core** `LinearInterpDeriv4D_4DArray_Point` — a 16-corner tetralinear evaluate-and-differentiate over a 4D `(Y1, Y2, Y3, Y4)` table, returning the value and four partials via `dTetraLineardX1..4`. This is the **EmAb** `(E, ρ, T, Yₑ)` shape (and **Iso** at a fixed `moment` slice).
- **2D-aligned core** `LinearInterpDeriv2D_4DArray_2DAligned_Point` — a 4-corner bilinear evaluate-and-differentiate over the aligned thermodynamic plane of a 4D array at fixed energy indices `(iX1, iX2)`, returning the value and two in-plane partials via `dBiLineardX1`/`dBiLineardX2`. This is the **NES/Pair** `(T, η)` plane and the **Brem** `(ρ, T)` plane (per effective density).

In scope:
- The 4D EmAb/Iso evaluate-and-differentiate `_Point` contract: `(E, ρ, T, Yₑ) → value, (∂value/∂E, ∂value/∂ρ, ∂value/∂T, ∂value/∂Yₑ)` (Iso at a fixed `moment`, with the 2D offset element).
- The 2D-aligned NES/Pair evaluate-and-differentiate `_Point` contract at fixed `(iE', iE, kernel)`: `(T, η) → value, (∂value/∂T, ∂value/∂η)`.
- The 2D-aligned Brem evaluate-and-differentiate `_Point` contract at fixed `(iE', iE, moment)`, per effective density: `(ρ_l, T) → Interp_l, (∂Interp_l/∂ρ_l, ∂Interp_l/∂T)`, plus the Alpha-weighted summed value/derivative.
- The universal derivative-assembly formula `∂value/∂X_k = (value + offset)·a_k·(∂/∂d_k of the multilinear-in-log form)` and the per-axis chain-rule scale factors `a_k` (log axis vs linear axis).
- The exact raw partials (`dBiLineardX1/2`, `dTetraLineardX1..4`) as the difference-of-lower-basis forms.
- The relaxed `1e-10` derivative tolerance tier (with rationale) versus the default `1e-12` value tier.
- Boundary/out-of-range/NaN behavior of the derivatives (bit-for-bit with weaklib).

Out of scope:
- The value-only evaluate contracts, table geometry, column-major indexing, offset dimensionality, species/moment/kernel index constants, energy-triangle symmetry fills (NES detailed balance, Pair crossing), and the Brem effective-density decomposition weights — all pinned by the matched evaluate leaves `opacity-emab-iso.md`, `opacity-nes-pair.md`, `opacity-brem.md`. This spec adds only the derivatives on top of those.
- The EOS 3D `(ρ, T, Yₑ)` evaluate-and-differentiate — see `eos-interpolation.md` (its derivative section is the direct precedent this spec generalizes).
- Recovering T from a dependent variable (the inversion direction) — see `eos-inversion.md`.
- The non-aligned full-energy-grid 2D2D path (`LogInterpolateDifferentiateSingleVariable_2D2D_Custom_Point`, energy axes interpolated) — off the aligned consumer's critical path; recorded in Open questions / assumptions.
- The physics assembly of opacities/IMFPs from the interpolated kernels and their derivatives.
- The on-disk HDF5 layout — see `table-format-and-io.md`.

## Source of truth

Pinned weaklib commit: see `weaklib_commit` in `specs/fixtures/tables.provenance`.

- `weaklib/Distributions/Library/wlInterpolationUtilitiesModule.F90` — the two derivative cores and the raw partials that define "correct" for this leaf:
  - `LinearInterpDeriv4D_4DArray_Point` (subroutine at `wlInterpolationUtilitiesModule.F90:967-1035`) — the 4D evaluate-and-differentiate workhorse. Signature `( iY1, iY2, iY3, iY4, dY1, dY2, dY3, dY4, aY1, aY2, aY3, aY4, OS, Table, Interpolant, dIdY1, dIdY2, dIdY3, dIdY4 )`: reads the 16 corners `Table(iY1[+1], iY2[+1], iY3[+1], iY4[+1])`, forms `Interpolant = 10**( TetraLinear(...) ) - OS`, and assembles `dIdY_k = (Interpolant + OS)·aY_k·dTetraLineardX_k(16 corners, the other three deltas)`.
  - `dTetraLineardX1..4` (functions at `wlInterpolationUtilitiesModule.F90:395-484`) — the four raw tetralinear partials, each the difference of two `TriLinear` evaluations at the two bracketing nodes of the differentiated axis (e.g. `dTetraLineardX1 = TriLinear(the iY1+1 corner group, dY2, dY3, dY4) − TriLinear(the iY1 corner group, dY2, dY3, dY4)`).
  - `LinearInterpDeriv2D_4DArray_2DAligned_Point` (subroutine at `wlInterpolationUtilitiesModule.F90:875-909`) — the 2D-aligned evaluate-and-differentiate workhorse. Signature `( iX1, iX2, iY1, iY2, dY1, dY2, aY1, aY2, OS, Table, Interpolant, dIdY1, dIdY2 )`: reads the 4 corners `Table(iX1, iX2, iY1[+1], iY2[+1])` at the fixed energy indices `(iX1, iX2)`, forms `Interpolant = 10**( BiLinear(...) ) - OS`, and assembles `dIdY1 = (Interpolant + OS)·aY1·dBiLineardX1( p00, p10, p01, p11, dY2 )`, `dIdY2 = (Interpolant + OS)·aY2·dBiLineardX2( p00, p10, p01, p11, dY1 )`.
  - `dBiLineardX1` / `dBiLineardX2` (functions at `wlInterpolationUtilitiesModule.F90:254-289`) — the two raw bilinear partials: `dBiLineardX1(p00,p10,p01,p11,dX2) = Linear(p10,p11,dX2) − Linear(p00,p01,dX2)`, `dBiLineardX2(p00,p10,p01,p11,dX1) = Linear(p01,p11,dX1) − Linear(p00,p10,dX1)`. These are the ported-but-dead `wli::dBiLineardX1`/`dBiLineardX2` at `src/core/wli_interp.H:159-168`.
- `weaklib/Distributions/Library/wlInterpolationModule.F90` — the host-level wrappers that supply the chain-rule scale factors and drive the cores:
  - `LogInterpolateDifferentiateSingleVariable_2D2D_Custom_Aligned_P` (subroutine at `wlInterpolationModule.F90:2050-2086`) — the scalar, allocation-free, device-callable aligned form for NES/Pair. Signature `( LogT, LogX, LogTs, LogXs, OS, Table, Interpolant, DerivativeT, DerivativeX )`: locates `(LogT, LogX)` on the LOG10'd grids via `GetIndexAndDelta_Lin`, computes the scale factors `aT`/`aX`, and calls `LinearInterpDeriv2D_4DArray_2DAligned_Point` over the lower energy triangle `i ≤ j`. The matched host array form `LogInterpolateDifferentiateSingleVariable_2D2D_Custom_Aligned` (`wlInterpolationModule.F90:1966-2047`) loops over query points; the C++ array form is the `ParallelFor` wrapper over the `_Point` core (see `amrex-device-interface.md`).
  - `LogInterpolateDifferentiateSingleVariable_3D_Custom_Point` (subroutine at `wlInterpolationModule.F90:1814-1844`) — the 3D EOS differentiate wrapper (already pinned by `eos-interpolation.md`); cited here as the construction template the EmAb 4D differentiate generalizes: pair the value-leaf's bracket/delta with the matching `LinearInterpDeriv*_Point` core and the per-axis `a_k`. There is no named `LogInterpolateDifferentiateSingleVariable_4D_Custom_Point` in weaklib; the EmAb 4D differentiate is assembled by pairing the `LogInterpolateSingleVariable_4D_Custom_Point` bracket/delta (see `opacity-emab-iso.md`) with the `LinearInterpDeriv4D_4DArray_Point` core (see Open questions / assumptions).
- The matched value-only evaluate leaves for each channel — `opacity-emab-iso.md` (EmAb/Iso, `LogInterpolateSingleVariable_4D_Custom_Point`), `opacity-nes-pair.md` (NES/Pair, `LogInterpolateSingleVariable_2D2D_Custom_Aligned_Point`), `opacity-brem.md` (Brem, `SumLogInterpolateSingleVariable_2D2D_Custom_Aligned`) — pin the value output this spec's routines must reproduce; the derivatives are the added output.

These `_Point` cores (and the scale-factor wrappers) are the authoritative oracle that defines "correct" for this leaf (see `fortran-parity-and-tolerances.md`).

## Inputs & outputs

Value type is `double` throughout (weaklib `dp = 8`); see `fortran-parity-and-tolerances.md`. Device contract (qualifiers, `Gpu::DeviceVector<double>` residency, `double const*` table passing, `ParallelFor` launch) is `amrex-device-interface.md`. The query coordinates, axis arrays, offsets, and tables are exactly those of the matched value-only leaf for each channel; only the derivative outputs are added.

### EmAb (4D) and Iso (4D slice) evaluate-and-differentiate

The query is the same 4-tuple as `opacity-emab-iso.md` — `E`, `ρ`, `T` arrive **pre-`LOG10`'d** (`LogE, LogD, LogT`) on the LOG10'd grids `LogEs, LogDs, LogTs`; `Yₑ` is **raw** on the linear grid `Ys`; Iso additionally selects a `moment` slice and its 2D offset element. Outputs:

- `Interpolant` — the recovered physical value (identical to the value-only leaf).
- `Derivative(1:4) = (∂value/∂E, ∂value/∂ρ, ∂value/∂T, ∂value/∂Yₑ)`, in physical value-units per unit of each independent variable (per MeV, per g/cm³, per K, per unit Yₑ).

### NES/Pair (2D-aligned) evaluate-and-differentiate

At fixed energy indices `(iE', iE)` and fixed `kernel`, the two interpolated coordinates are the thermodynamic pair `(T, η)`, both **pre-`LOG10`'d** (`LogT`, `LogX = LOG10(η)`) on the LOG10'd grids `LogTs, LogXs`, with `T` in **MeV** (per `opacity-nes-pair.md`). Outputs:

- `Interpolant` — the recovered physical kernel value (identical to the value-only aligned leaf, on the lower energy triangle `iE' ≤ iE`).
- `DerivativeT = ∂value/∂T`, `DerivativeX = ∂value/∂η`.

### Brem (2D-aligned, summed) evaluate-and-differentiate

At fixed energy indices `(iE', iE)` and fixed `moment`, the interpolated plane is `(ρ, T)`, both **pre-`LOG10`'d** on `LogDs, LogTs`, evaluated once per effective density `l` (`ρ·xₚ`, `ρ·xₙ`, `ρ·√(xₚ·xₙ)`; see `opacity-brem.md`). Per effective density the 2D-aligned core returns `Interp_l` and `(∂Interp_l/∂ρ_l, ∂Interp_l/∂T)`. The channel outputs:

- `SumInterp = Σ_l Alpha(l)·Interp_l` (identical to the value-only summed leaf, `Alpha_Brem = [1, 1, 28/3]`).
- `∂SumInterp/∂T = Σ_l Alpha(l)·∂Interp_l/∂T`.
- The base-density derivative `∂SumInterp/∂ρ` via the effective-density chain rule (see Correctness requirements and Open questions / assumptions).

### Reference tables (anchor the regression-suite checks)

The same reference tables as the matched value-only leaves, pinned by path + `sha256` in `specs/fixtures/tables.provenance` with committed `specs/fixtures/*.h5ls` snapshots: EmAb `wl-Op-SFHo-15-25-50-E40-EmAb.h5`, Iso `wl-Op-SFHo-15-25-50-E40-Iso.h5`, NES `wl-Op-SFHo-15-25-50-E40-NES.h5`, Pair `wl-Op-SFHo-15-25-50-E40-Pair.h5`, Brem `wl-Op-SFHo-15-25-50-E40-Brem.h5`. The table geometry, axes, and offsets are documented in the matched value-only leaf and `table-format-and-io.md`; the derivative checks reuse them and additionally exercise synthetic affine-in-log tables whose analytic derivatives are known in closed form.

## Correctness requirements

### The universal derivative-assembly formula

For every channel, each partial derivative is

```
∂Interpolant/∂X_k = (Interpolant + OS) · a_k · (∂/∂d_k of the multilinear-in-log form)
```

where `(Interpolant + OS) = 10**(multilinear)` reconstitutes the exponential (the additive offset `OS` is undone by the `+ OS`), `a_k` is the per-axis chain-rule scale factor below, and the trailing factor is the exact raw partial of the multilinear basis with respect to the in-cell delta `d_k` of axis `k`. The `Interpolant` value itself is identical to the matched value-only leaf.

### Per-axis chain-rule scale factors

- **Pre-`LOG10`'d (log) axis** located on a LOG10'd grid `LogXs`, with physical coordinate value `X = 10**(LogX)`: `a = 1 / ( ( LogXs(i+1) − LogXs(i) ) · X )`, equivalently `1 / ( X · log10( Xs(i+1)/Xs(i) ) )`. This is the EmAb `E`/`ρ`/`T`, the NES/Pair `T`/`η`, and the Brem `ρ`/`T` factor. The `ln10` from differentiating `10**(...)` cancels the `ln10` from differentiating `log10` of the coordinate, so **no `ln10` survives on a log axis** (matching `aT = One / (LogTs(iT+1) − LogTs(iT)) / 10.0d0**LogT` in the aligned wrapper).
- **Linear axis** located on a raw grid `Ys`: `a = ln10 / ( Ys(iY+1) − Ys(iY) )`, `ln10 = LOG(10)` (`wli::ln10`, `src/core/wli_interp.H:47`). This is the EmAb `Yₑ` factor. Here `ln10` **does** survive because the coordinate is not log-transformed (matching `aY = ln10 / (Ys(iY+1) − Ys(iY))` in the 3D EOS wrapper).

These are the identical log/linear scale-factor forms `eos-interpolation.md` pins for `(ρ, T, Yₑ)`; opacity differs only in that the log axes arrive pre-`LOG10`'d (so `X = 10**(LogX)` and `LogXs(i+1) − LogXs(i) = log10(Xs(i+1)/Xs(i))`), exactly as the value-only opacity leaves already note for the bracket/delta.

### The raw partials

- **2D-aligned plane** (`LinearInterpDeriv2D_4DArray_2DAligned_Point`), with the 4 corners `p00, p10, p01, p11 = Table(iX1, iX2, iY1[+1], iY2[+1])`:
  - `∂/∂d1 = dBiLineardX1(p00,p10,p01,p11, d2) = Linear(p10,p11,d2) − Linear(p00,p01,d2)`.
  - `∂/∂d2 = dBiLineardX2(p00,p10,p01,p11, d1) = Linear(p01,p11,d1) − Linear(p00,p10,d1)`.
  where `Linear(a,b,d) = (1−d)·a + d·b`. Axis 1 is `T`, axis 2 is `η` for NES/Pair; axis 1 is `ρ`, axis 2 is `T` for Brem (the corner delta ordering follows the value-only leaf).
- **4D tetralinear** (`LinearInterpDeriv4D_4DArray_Point`): each `dTetraLineardX_k` is the difference of two `TriLinear` evaluations, one over the eight corners with the differentiated axis at its upper node and one at its lower node, using the other three deltas — the exact forms of `dTetraLineardX1..4` (`wlInterpolationUtilitiesModule.F90:395-484`). The C++ port must add these four functions (currently declared out of scope at `src/core/wli_interp.H:154-156`); they mirror the already-present `dTriLineardX1/2/3` one dimension up.

### Brem summed derivative (effective-density decomposition)

The Brem value is `SumInterp = Σ_l Alpha(l)·Interp_l` over the three effective densities `ρ_l = ρ·c_l` with `c_l ∈ {xₚ, xₙ, √(xₚ·xₙ)}` and `Alpha_Brem = [1, 1, 28/3]` (see `opacity-brem.md`). Because `Σ` and `∂` commute and `Alpha` is constant per query, the **temperature** derivative is the exact Alpha-weighted sum of the per-density plane derivatives:

```
∂SumInterp/∂T = Σ_l Alpha(l)·∂Interp_l/∂T
```

The **base-density** derivative uses the effective-density chain rule `∂ρ_l/∂ρ = c_l` (holding the mass fractions fixed), so `∂SumInterp/∂ρ = Σ_l Alpha(l)·c_l·(∂Interp_l/∂ρ_l)`, where `∂Interp_l/∂ρ_l` is the plane derivative the 2D-aligned core returns at effective density `ρ_l`. The oracle-matched quantity is the per-effective-density plane derivative from the core; the base-`ρ` chain-rule assembly is the natural extension recorded in Open questions / assumptions (weaklib ships no summed-derivative wrapper).

### Tolerances

- The `Interpolant` value output matches the matched value-only leaf at the **default parity tier** (`rtol 1e-12`, `atol 1e-30`).
- The derivative outputs match the weaklib core (`LinearInterpDeriv4D_4DArray_Point` / `LinearInterpDeriv2D_4DArray_2DAligned_Point`, driven with the scale factors above) at the **relaxed tier `1e-10`**.

**Rationale for the `1e-10` derivative relaxation** (identical to `eos-interpolation.md`): the derivative is a product of the reconstituted exponential `(value + OS)`, a transcendental scale factor (a `log10` node-ratio, or `ln10` over a node spacing), and a finite-difference of the multilinear form — an interpolation-of-an-interpolation whose order-of-operations and transcendental rounding accumulate beyond the `1e-12` single-value tier. `1e-10` bounds that accumulation while still catching real regressions; the recovered value itself stays at the default `1e-12` tier.

### Boundary / out-of-range / NaN (bit-for-bit with weaklib)

Replicate the permissive behavior exactly (see `fortran-parity-and-tolerances.md`), consistent with the value-only leaves:

- Out-of-range query on any interpolated axis: clamp the bracket index to the edge cell `[1, n-1]`, do **not** clamp the delta. The scale factor `a_k` uses the clamped edge cell's node spacing (and, for a log axis, the physical coordinate `X = 10**(LogX)`); the raw partial uses the unclamped delta — so the derivative is the derivative of the edge-cell linear extrapolation, not an error, not a clamp, not a NaN. No range check.
- Non-positive `E`, `ρ`, `T`, or `η` (any pre-`LOG10`'d coordinate): the caller forms `LOG10` of a non-positive argument, yielding NaN that propagates silently to both `Interpolant` and every derivative. The C++ result must be NaN in the same circumstances. `Yₑ` (linear axis) does not log, so a non-positive `Yₑ` extrapolates rather than NaNs.
- The energy indices, `kernel`/`moment` indices, and (Iso) the `moment` slice are assumed valid; range-checking them is a consumer responsibility, mirroring the unguarded Fortran indexing.

### Conventions restated (the subset this leaf uses)

- The derivative is `(value + offset)·a_k·(∂ multilinear/∂d_k)`; on a log axis the `ln10` cancels, on a linear axis it survives.
- The query coordinates, log/linear axis split, units (EmAb/Iso `T` in K; NES/Pair/Brem `T` in MeV, NES/Pair `η` dimensionless), offsets, and column-major table indexing are exactly those of the matched value-only leaf.
- Universal recovery `value = 10**(stored) − offset`; `(value + offset)` reconstitutes the exponential in the derivative.
- `double` (`dp = 8`) throughout.

## Verification

### Self-contained checks (the regression suite)

Run against both synthetic in-suite tables and the real reference tables named above:

1. **Value parity with the evaluate leaf (default parity tier `1e-12`/`1e-30`).** The `Interpolant` returned by each evaluate-and-differentiate routine equals the value-only leaf's output on identical inputs — the derivative path must not perturb the value.
2. **Derivative closed form on an affine-in-log table (relaxed tier `1e-10`).** Build a synthetic table whose stored value, at each fixed slice, is an exact affine function of the log-coordinates (and of `Yₑ` for EmAb) with a distinct affine offset per slice (`moment`/`kernel`/energy pair) so a wrong slice or a transposed offset is caught. The analytic derivative has a closed form (`∂value/∂X = value·(affine slope in log)·(chain factor)`, or `value·slope·ln10` for the linear `Yₑ`); the returned derivatives must match it at *any* interior query, not just at nodes.
3. **Finite-difference cross-check on the real tables (relaxed tier `1e-10`).** Compare each returned partial against a tight central finite difference of the value-only leaf in that coordinate; agreement to the relaxed tier confirms the scale factor and raw-partial assembly on production data.
4. **Log-axis vs linear-axis scale factor (relaxed tier).** On the affine table, confirm the log-axis partials carry `1/(X·log10(node ratio))` and the linear `Yₑ` partial carries `ln10/(node spacing)` — a swapped or missing `ln10` is caught.
5. **Brem summed-derivative decomposition (default parity tier).** With a synthetic Brem kernel, assert `∂SumInterp/∂T = Σ_l Alpha(l)·∂Interp_l/∂T` (an exact linear combination), and that the per-effective-density plane derivatives equal the 2D-aligned core evaluated at each `ρ_l`.
6. **Boundary extrapolation of the derivative (no tolerance / exact relation).** A query just outside an edge must return the derivative of the edge cell's linear extrapolation — the same assembly formula evaluated with the clamped index (in `a_k`) and the unclamped delta (in the raw partial) — confirming clamp-index-but-not-delta on the derivative path.
7. **NaN propagation (NaN-equality).** A query with non-positive `E`, `ρ`, `T`, or `η` must produce NaN in `Interpolant` and in every derivative output.

### Mechanical (validator)

`bash specs/tools/validate_specs.sh` (default mode) asserts: the 7 mandated sections in order; a concrete numeric tolerance is named (`1e-10`, `1e-12`, `1e-30`); the `LinearInterpDeriv4D_4DArray_Point` and `LinearInterpDeriv2D_4DArray_2DAligned_Point` core names and the `LogInterpolateDifferentiateSingleVariable_2D2D_Custom_Aligned` wrapper name are present; the cited `weaklib/…` source-of-truth paths resolve; and the reference tables are cited by name against their committed `*.h5ls` snapshots. (Registering this spec in the validator's `REGISTERED_SPECS`, linking it from `README.md`, and referencing its cores in the `regression-suite-design.md` coverage matrix are follow-up integration steps outside this single-file spec.)

## Implementation freedom

- Whether the evaluate-and-differentiate routine shares code with the value-only leaf (recomputing the value inline) or calls it; the value must be bit-identical either way.
- Whether the 16/4 corners are read into locals or indexed in place, and the internal multilinear/partial evaluation order.
- Whether the EmAb 4D differentiate is a distinct routine or the 3D EOS differentiate pattern generalized one dimension up; whether Iso reuses the EmAb 4D routine on its `(species, moment)` slice.
- Whether `dTetraLineardX1..4` are added as standalone `wli::` functions (mirroring `dTriLineardX1/2/3`) or fused into the 4D core; whether the dead `dBiLineardX1/2` are called directly or inlined.
- Whether the Brem summed derivative is assembled in the interpolation kernel or in the consumer's decomposition step, provided the observable `∂SumInterp/∂T` (and, if provided, `∂SumInterp/∂ρ`) match the stated relations.
- Whether the caller pre-`LOG10`'s the log coordinates or the entry point does it internally, provided the observable result — including NaN-on-non-positive — matches the `_Point` contract.
- The exact `_Point` signature argument grouping (subject to the scalar/allocation-free/`double const*`-plus-extents contract in `amrex-device-interface.md`).
- Whether the multi-point array form is a hand-written loop or a `ParallelFor` over the `_Point` core.
- Any caching/precomputation of node ratios or LOG10'd grids, provided results meet the stated tolerances.

## Open questions / assumptions

- **No named 4D EmAb differentiate wrapper (assumption, non-blocking).** weaklib exposes `LogInterpolateDifferentiate…` wrappers only for the 3D EOS (`_3D_Custom`), the non-aligned 2D2D (`_2D2D_Custom`), and the aligned 2D2D (`_2D2D_Custom_Aligned`) shapes — there is no `LogInterpolateDifferentiateSingleVariable_4D_Custom_Point`. The EmAb (and Iso) 4D evaluate-and-differentiate is therefore assembled by pairing the value-leaf's bracket/delta (`LogInterpolateSingleVariable_4D_Custom_Point`, `opacity-emab-iso.md`) with the `LinearInterpDeriv4D_4DArray_Point` core and the four per-axis scale factors — the identical construction weaklib uses for the 3D EOS differentiate and the non-aligned 2D2D differentiate. The core defines the four partials unambiguously, so this is non-blocking; the affine-in-log and finite-difference checks anchor correctness without a dedicated Fortran wrapper.
- **No named Brem summed-derivative wrapper (assumption, non-blocking).** weaklib ships no `SumLogInterpolateDifferentiate…` routine. The oracle-matched quantity for Brem is the per-effective-density 2D-aligned plane derivative from `LinearInterpDeriv2D_4DArray_2DAligned_Point`. The summed value derivative `∂SumInterp/∂T = Σ_l Alpha(l)·∂Interp_l/∂T` is exact (linearity), and the base-density derivative `∂SumInterp/∂ρ = Σ_l Alpha(l)·c_l·(∂Interp_l/∂ρ_l)` follows the effective-density chain rule with the mass fractions held fixed; if a consumer needs `∂/∂ρ` at fixed composition it must supply the `c_l = xₚ, xₙ, √(xₚ·xₙ)`, consistent with the value-only Brem contract.
- **Non-aligned full-energy-grid 2D2D differentiate (provenance note).** `LogInterpolateDifferentiateSingleVariable_2D2D_Custom_Point` (`wlInterpolationModule.F90:1924-1963`) interpolates both energy axes and drives `LinearInterpDeriv4D_4DArray_Point` with the two energy scale factors set to `One` and the energy partials discarded (only `DerivativeT`/`DerivativeX` are returned). This is the non-aligned path the value-only NES/Pair leaf marks out of scope (thornado drives the aligned path on pre-aligned tables); it is recorded here only as provenance and the C++ port does not need to reproduce it.
- **Concrete offsets, grid extents, and coupling/mass-fraction data (assumption, non-blocking).** As in the value-only leaves, the production `Offsets`, energy/η/T/ρ node coordinates, and the Brem mass fractions live only in the `.h5` files and the consumer's physics assembly. This spec pins the derivative-assembly contract and scale factors; the self-contained affine-in-log checks use synthetic tables whose offsets and slopes the suite chooses, so they do not depend on the unknown production numbers.
