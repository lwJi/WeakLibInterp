# Opacity interpolation: Brem (two-energy, on `(E', E, moment, ρ, T)`)

> Leaf spec. Self-contained: an agent can implement the Brem (nucleon–nucleon bremsstrahlung) kernel interpolator from this file alone. It restates only the conventions it uses and references `fortran-parity-and-tolerances.md` (numeric contract) and `amrex-device-interface.md` (device contract) for the shared cross-cutting details. `README.md` is canonical if any restated convention here conflicts with it.

## Purpose & scope

This spec defines the device-callable interpolation of the **Brem** (nucleon–nucleon bremsstrahlung) neutrino-opacity scattering channel. Brem is the one two-energy scattering channel that depends on density `ρ` and temperature `T` (and **not** on `η` or `Yₑ`), and whose physical value is reconstructed by a fixed-weight **effective-density decomposition** rather than by a symmetry fill.

- **Brem** — bremsstrahlung kernel `S_σ`, stored as a **5D** table over `(E', E, moment, ρ, T)`.

The table geometry: two neutrino-energy axes `(E', E)`, a discrete `moment` index (a small set of Legendre-moment slices, **not interpolated**), and two thermodynamic axes `(ρ, T)` interpolated in **log** space. Every value is stored as `log10(value + offset)` and recovered by `10**(...) - offset`.

The reference consumer (thornado) does **not** interpolate the two energy axes in-kernel. It pre-aligns the kernel table onto its own discrete neutrino-energy quadrature at initialization time (`Brem_AT`), so at evaluation time the two energy indices `(iE', iE)` are used **directly as table indices** and only the `(ρ, T)` plane is bilinearly interpolated. This spec's device contract is therefore the **aligned, summed** path: `SumLogInterpolateSingleVariable_2D2D_Custom_Aligned(_Point)` — a 2D `(ρ, T)` bilinear interpolation at a fixed `(iE', iE, moment)`, evaluated once per **effective density** and accumulated with fixed weights.

Correctness is anchored to the closure invariant that holds independently of the interpolation machinery's provenance — the **effective-density decomposition**:

```
SumInterp = Σ_l  Alpha(l) · Interp_l ,   Alpha_Brem = [ 1, 1, 28/3 ]
```

over the three effective densities `ρ·xₚ`, `ρ·xₙ`, `ρ·√(xₚ·xₙ)` (proton, neutron, and the proton–neutron cross term, where `xₚ`, `xₙ` are the proton/neutron mass fractions). Each `Interp_l` is the bilinear-in-log interpolation of the *same* kernel sub-table at the *same* fixed energy/moment indices, evaluated at that effective density's `(ρ_l, T)`.

In scope:
- The single-point **aligned, summed** evaluate contract: at fixed energy indices `(iE', iE)` and `moment` index, interpolate one kernel value bilinearly in `(log10 ρ, log10 T)` for each of the three effective densities, then accumulate `Σ_l Alpha(l)·Interp_l`.
- Independent-variable order/units; which coordinates arrive **pre-`LOG10`** (`ρ`, `T`) and which axes are not interpolated (`E'`, `E`, `moment`).
- The effective-density decomposition `Alpha_Brem = [1, 1, 28/3]` over `ρ·xₚ`, `ρ·xₙ`, `ρ·√(xₚ·xₙ)` as a closure check.
- The 2D offset dimensionality `Offsets[nOpacities, nMoments]`.
- Column-major indexing of the 5D `(E', E, moment, ρ, T)` table and its 4D `(E', E, ρ, T)` sub-table at a fixed `moment`.
- That **both energy triangles are computed** (no symmetry fill) — the explicit distinction from NES/Pair.
- Boundary/out-of-range/NaN behavior (bit-for-bit with weaklib).

Out of scope:
- The EmAb (4D) and Iso (5D, `(E, moment, ρ, T, Yₑ)`) channels — see `opacity-emab-iso.md`.
- The NES + Pair channels (`(E', E, kernel, T, η)`, detailed-balance / crossing symmetry fills) — see `opacity-nes-pair.md`.
- Computing the proton/neutron mass fractions `xₚ`, `xₙ` and thus the effective densities — these come from the EOS path (`ProtonMassFraction` / `NeutronMassFraction` dependent variables; see `eos-interpolation.md`). Here the three effective densities (or equivalently `ρ`, `xₚ`, `xₙ`) are supplied as input.
- The non-aligned full-energy-grid weaklib path (`wlInterpolateOpacity_Brem` over `LinearInterp_Array_Point`) — see Open questions / assumptions.
- The on-disk HDF5 layout — see `table-format-and-io.md`.

## Source of truth

Pinned weaklib commit: see `weaklib_commit` in `specs/fixtures/tables.provenance`.

- `weaklib/Distributions/Library/wlInterpolationModule.F90` — the named generator-of-record for the **aligned, summed** consumer path (the contract this spec pins):
  - `SumLogInterpolateSingleVariable_2D2D_Custom_Aligned` (subroutine at `wlInterpolationModule.F90:1488-1595`) — the routine the reference consumer drives. Signature `( LogD, LogT, LogDs, LogTs, Alpha, OS, Table, Interpolant, ASYNC_Option )`:
    - `LogD(1:nSpecies, 1:nPoints)` are the **already-`LOG10`'d** effective densities — one row per species/effective-density `l` (for Brem, `nSpecies = 3`: `ρ·xₚ`, `ρ·xₙ`, `ρ·√(xₚ·xₙ)`), located on the `LOG10`'d density grid `LogDs` via `Index1D_Lin` + the linear delta.
    - `LogT(1:nPoints)` is the **already-`LOG10`'d** temperature query, located on the `LOG10`'d grid `LogTs`.
    - `Alpha(1:nSpecies)` are the fixed decomposition weights — for Brem, `Alpha_Brem = [1.0d0, 1.0d0, 28.0d0/3.0d0]`.
    - `OS` is the scalar additive offset for the chosen `(opacity, moment)`.
    - `Table(i, j, iD, iT)` is the log-stored 4D `(E', E, ρ, T)` sub-table at the chosen `moment`.
    - `Interpolant(i, j, k)` accumulates `SumInterp = Σ_l Alpha(l) · Interp_l`, where each `Interp_l = 10**(BiLinear over (ρ, T) at the corner stack (i, j, iD(l), iT)) - OS`. The two energy indices `(i, j)` are used **directly as table indices** — there is no energy-axis interpolation.
  - The scalar `_Point` device contract this spec adopts is the per-`(iE', iE)` body of that routine: one bilinear-in-`(log10 ρ, log10 T)` interpolation per effective density `l`, accumulated with `Alpha(l)`. (The Fortran array form owns the host-level loop over query points and the triangular energy loop; the C++ array form is the `ParallelFor` wrapper over the scalar core — see `amrex-device-interface.md`.)
- `weaklib/Distributions/Library/wlInterpolationUtilitiesModule.F90` — the shared primitives the aligned routine calls:
  - `Index1D_Lin` — the O(1) bracket search applied to the pre-`LOG10`'d `ρ` and `T` coordinates against their `LOG10`'d grids; the linear in-cell delta `d = (x − xs(i))/(xs(i+1) − xs(i))` is formed directly after.
  - `LinearInterp2D_4DArray_2DAligned_Point` (subroutine at `wlInterpolationUtilitiesModule.F90:602-627`) — reads the 4 corners `p00..p11 = Table(iX1, iX2, iY1[+1], iY2[+1])` at fixed energy indices `(iX1, iX2) = (iE', iE)`, evaluates `BiLinear` over the `(ρ, T)` plane, and owns the `10**(...) - OS` recovery. This is the single-effective-density inner kernel that the summed routine calls once per species.
- `weaklib/Distributions/OpacitySource/wlOpacityInterpolationModule.f90` — cited **only for the decomposition physics** (the effective densities and the no-symmetry-fill structure), not as the aligned-interpolation source-of-truth:
  - `wlInterpolateOpacity_Brem` (subroutine at `wlOpacityInterpolationModule.f90:236-277`) — the standalone full-energy-grid Brem wrapper. It computes **both** energy triangles in a full 5-level nested loop (`DO jj …; DO ii …` over the whole `nE'×nE` block, `wlOpacityInterpolationModule.f90:266-272`) with `Phi0a_Brem` `INTENT(inout)` (caller zero-initializes) and **no symmetry fill** — confirming the Brem-specific "both triangles computed" property. (Its `GetIndexAndDelta` / `LinearInterp_Array_Point` overload resolution is recorded as a non-blocking provenance note in Open questions / assumptions.)

The effective densities and `Alpha_Brem` weights are pinned by the reference consumer (thornado `Modules/Opacities/NeutrinoOpacitiesComputationModule.F90`): `Alpha_Brem(3) = [1.0d0, 1.0d0, 28.d0/3.d0]`, and the three effective densities `LogDX_P(1..3) = LOG10(ρ·xₚ)`, `LOG10(ρ·xₙ)`, `LOG10(ρ·√|xₚ·xₙ|)` (each divided by the density unit), passed as the rows of `LogD` to `SumLogInterpolateSingleVariable_2D2D_Custom_Aligned`. These are cited as consumer/provenance data; this spec pins the interpolation-plus-decomposition contract, fixtures supply any table-specific numbers.

The aligned summed `_Point` body is the authoritative oracle that defines "correct" for this leaf (see `fortran-parity-and-tolerances.md`); the effective-density decomposition is the closure check of the regression suite.

## Inputs & outputs

Value type is `double` throughout (weaklib `dp = 8`); see `fortran-parity-and-tolerances.md`. Device contract (qualifiers, `Gpu::DeviceVector<double>` residency, `double const*` table passing, `ParallelFor` launch) is `amrex-device-interface.md`.

### Table geometry

The 5D kernel table is, in Fortran (column-major, first index fastest-varying) order:

```
S_sigma(iE', iE, moment, iρ, iT)   shape (nE', nE, nMom, nρ, nT)
```

with `nE' = nE` (the two energy axes share the energy grid). In the pinned production table `nE = 40`, `nMom = 1`, `nρ = 185`, `nT = 81`. (The `h5ls` snapshot lists the reversed dimensions `{81, 185, 1, 40, 40}` = Fortran `(40, 40, 1, 185, 81) = (nE', nE, nMom, nρ, nT)`.)

Note the axes ordering versus NES/Pair: in Brem the last two table dimensions are `(ρ, T)`, not `(T, η)`. There is no η axis and no Yₑ axis.

### Query coordinates — order, units, and which arrive pre-`LOG10`

For the **aligned** evaluate, the two energy axes are *not* query points — they are integer table indices `(iE', iE)`, chosen by the consumer's energy quadrature alignment. The only interpolated coordinate axes are the two thermodynamic ones `(ρ, T)`, and the density axis is queried at **three** effective-density values per point:

| Slot | Symbol | Quantity | Unit | Interp space | Passed as |
|---|---|---|---|---|---|
| — | iE' | first neutrino-energy index | integer `1..nE'` | **none (direct index)** | integer index |
| — | iE | second neutrino-energy index | integer `1..nE` | **none (direct index)** | integer index |
| — | moment | Legendre-moment index | integer `1..nMom` | **none (direct slice)** | integer index |
| 1 | ρ (×3 effective) | effective mass density | **g cm⁻³** | **log** | **pre-`LOG10`** (`LogD(l) = LOG10(ρ_l)`) |
| 2 | T | temperature | **K** | **log** | **pre-`LOG10`** (`LogT = LOG10(T)`) |

The critical distinctions a fresh agent must observe:
- **There is no η and no Yₑ.** The two thermodynamic axes are `(ρ, T)`, exactly the EmAb/Iso ρ/T sub-axes — but with `ρ` evaluated at **three effective densities** instead of one.
- **The density axis is queried three times per output point**, once per effective density `ρ_l ∈ {ρ·xₚ, ρ·xₙ, ρ·√(xₚ·xₙ)}`, against the *same* density grid `LogDs`. Each effective density gets its own bracket index `iD(l)` and delta `dD(l)`.
- **`ρ` and `T` are located with the linear index/delta formula applied to their already-`LOG10`'d coordinates against already-`LOG10`'d grids** — the caller takes the log once, up front; the routine does not log again (matches `Index1D_Lin` + the linear delta on both axes in the aligned routine).
- **The energy axes and the `moment` index are never interpolated.** `(iE', iE)` index directly; `moment` selects a 4D `(E', E, ρ, T)` sub-table.
- **`T` units.** Brem shares the EOS/EmAb/Iso thermodynamic axes (`ρ` in g cm⁻³, `T` in K), unlike NES/Pair which carry `T` in MeV. The on-disk `/ThermoState/Units` dataset is authoritative (see `table-format-and-io.md`); the consumer must pass `ρ`, `T` in the same units the `/ThermoState/Density` and `/ThermoState/Temperature` grids were tabulated in.

### Axis arrays, offsets, weights, and table

- `LogDs(1:nρ)`, `LogTs(1:nT)` — the strictly monotone-ascending **`LOG10`'d** grid-node coordinates for `ρ` and `T` (i.e. `LOG10` of `/ThermoState/Density` and `/ThermoState/Temperature`).
- `LogD(1:nSpecies)` — the **`LOG10`'d** effective densities; for Brem `nSpecies = 3` and `LogD = [LOG10(ρ·xₚ), LOG10(ρ·xₙ), LOG10(ρ·√(xₚ·xₙ))]`.
- `Alpha(1:nSpecies)` — the fixed decomposition weights; for Brem `Alpha = Alpha_Brem = [1, 1, 28/3]`.
- `OS` — the scalar additive offset for the chosen `(opacity, moment)`, drawn from the **2D** `Offsets[nOpacities, nMoments]`. The caller selects the scalar before the summed 2D2D call. The **same** `OS` applies to all three effective-density interpolations (they share the one `(opacity, moment)` sub-table).
- `Table` — the log-stored values.
  - **5D full table:** `log10(value + OS)` indexed `(iE', iE, moment, iρ, iT)`, Fortran column-major (E' fastest-varying). As a flat `double const*` (0-based) the element `S_sigma(iEp, iE, m, iD, iT)` is `table[ iEp + nE'*( iE + nE*( m + nMom*( iD + nρ*iT ) ) ) ]`.
  - **4D aligned sub-table at fixed `moment = m`:** `Table(iE', iE, iρ, iT)` — the energy indices used directly, the `(ρ, T)` plane bilinearly interpolated, the same sub-table reused for all three effective densities. Whether the implementation indexes the 5D buffer with `moment` fixed in the arithmetic, or first gathers a contiguous 4D slice, is implementation freedom — the observable result is identical.

### Outputs

- The recovered, decomposed physical kernel value at the requested `(iE', iE, moment, ρ, T)`:

  ```
  SumInterp(iE', iE) = Σ_{l=1..3} Alpha_Brem(l) · ( 10**(BiLinear_l(...)) - OS )
  ```

  where `BiLinear_l` is the `(log10 ρ_l, log10 T)` bilinear interpolation at the fixed corner stack `(iE', iE, iD(l), iT)`. Units are the channel's stored units (`/Scat_Brem_Kernels/Units`).
- For the channel as a whole, the kernel array over the **entire** energy plane: **both** the lower triangle `iE' ≤ iE` and the upper triangle `iE' > iE` are computed by the same decomposition formula. There is **no symmetry fill** — unlike NES (detailed balance) and Pair (crossing symmetry), every `(iE', iE)` entry is interpolated independently. (The Fortran aligned array form iterates only `i ≤ j` for performance, but the standalone Brem wrapper computes the full block; this spec's contract is that every requested `(iE', iE)` entry is a direct, independent decomposition with no implied relation to its transpose.)

### Reference table (anchor the regression-suite checks)

- **Brem:** `wl-Op-SFHo-15-25-50-E40-Brem.h5`, pinned by path + `sha256` in `specs/fixtures/tables.provenance`; structure committed at `specs/fixtures/wl-Op-SFHo-15-25-50-E40-Brem.h5ls`. The kernels live under `/Scat_Brem_Kernels/S_sigma` (`h5ls` `{81, 185, 1, 40, 40}` = Fortran `(40, 40, 1, 185, 81) = (nE', nE, nMom, nρ, nT)`), the **2D** offsets under `/Scat_Brem_Kernels/Offsets` `{1, 1}` = Fortran `(1, 1) = (nOpacities, nMoments)`, the energy axis under `/EnergyGrid/Values` `[40]`, the density axis under `/ThermoState/Density` `[185]`, and temperature under `/ThermoState/Temperature` `[81]`. Note there is **no `/EtaGrid` group** (Brem has no η axis), distinguishing the Brem file from the NES/Pair files.

Full on-disk contract (group/dataset names, dtypes, legacy fallbacks, the column-major→C-shape reversal): `table-format-and-io.md`.

## Correctness requirements

### The aligned, summed interpolation formula

At fixed energy indices `(iE', iE)` and fixed `moment`, with the two thermodynamic query coordinates pre-`LOG10`'d (`LogT = LOG10(T)`; and per effective density `LogD(l) = LOG10(ρ_l)`), located on their `LOG10`'d grids (brackets/deltas as in `fortran-parity-and-tolerances.md`, linear formula on each coordinate):

- `iT` from the linear bracket on `LogTs`, `dT = (LogT − LogTs(iT)) / (LogTs(iT+1) − LogTs(iT))`.
- For each effective density `l = 1..3`: `iD(l)` from the linear bracket on `LogDs`, `dD(l) = (LogD(l) − LogDs(iD(l))) / (LogDs(iD(l)+1) − LogDs(iD(l)))`.

For each effective density `l`, read the 4 corner log-values at the fixed energy indices:

```
p00 = Table(iE', iE, iD(l)  , iT  )
p10 = Table(iE', iE, iD(l)+1, iT  )
p01 = Table(iE', iE, iD(l)  , iT+1)
p11 = Table(iE', iE, iD(l)+1, iT+1)
```

form the bilinear interpolant in log space, recover, and accumulate:

```
Interp_l    = 10**( BiLinear(p00, p10, p01, p11, dD(l), dT) ) - OS
SumInterp   = Σ_{l=1..3}  Alpha_Brem(l) · Interp_l
```

where `BiLinear(p00, p10, p01, p11, d1, d2) = (1−d2)·((1−d1)·p00 + d1·p10) + d2·((1−d1)·p01 + d1·p11)` (linear in `dD(l)`, then `dT`), and `Alpha_Brem = [1, 1, 28/3]`. The result must match `SumLogInterpolateSingleVariable_2D2D_Custom_Aligned` on identical inputs at the **default parity tier** (`rtol 1e-12`, `atol 1e-30`).

Because both `ρ` and `T` are interpolated **linearly in their `LOG10`'d coordinate** (the caller pre-`LOG10`'d them), each `Interp_l` is bilinear in `(log10 ρ_l, log10 T)`. The energy indices `(iE', iE)` and the `moment` index select the corner stack; they are not interpolated.

Order of operations: the offset recovery `- OS` is applied **per effective density, inside the sum** (each `Interp_l` is a recovered physical value), and the weights `Alpha_Brem` multiply the recovered values — not the log-space values. This matches `wlInterpolationModule.F90:1559-1564` (the `10**(...) - OS` is inside the `DO l` loop and `SumInterp = SumInterp + Alpha(l) * Interp`).

### The effective-density decomposition (closure check)

The defining, machinery-independent statement of Brem correctness is the fixed-weight decomposition over three effective densities:

```
SumInterp = Alpha(1)·K(ρ·xₚ) + Alpha(2)·K(ρ·xₙ) + Alpha(3)·K(ρ·√(xₚ·xₙ)) ,
            Alpha_Brem = [ 1, 1, 28/3 ]
```

where `K(ρ_l)` denotes the bilinear-in-`(log10 ρ_l, log10 T)` interpolation of the *same* `(iE', iE, moment)` kernel sub-table at effective density `ρ_l`. A correct implementation must:
- use exactly the three effective densities `ρ·xₚ`, `ρ·xₙ`, `ρ·√(xₚ·xₙ)` (the proton/neutron cross term uses the geometric mean of the two mass fractions; the consumer forms it as `ρ·√|xₚ·xₙ|`, taking the absolute value before the square root);
- use exactly the weights `Alpha_Brem = [1, 1, 28/3]` in that order;
- interpolate the same kernel sub-table (same `(iE', iE, moment)`, same `OS`) once per effective density, against the same `LogDs`/`LogTs` grids;
- sum the three recovered values with their weights.

This is verifiable in closed form: for any fixed `(iE', iE, moment, T)`, evaluating the single-effective-density interpolation `K` at each `ρ_l` and combining as `1·K₁ + 1·K₂ + (28/3)·K₃` must equal the summed routine's output to the **default parity tier** (`rtol 1e-12`, `atol 1e-30`), since the summed routine is exactly that linear combination of the same interpolations.

### Both energy triangles computed (no symmetry fill) — distinct from NES/Pair

Brem has **no detailed-balance and no crossing symmetry**. Every `(iE', iE)` entry — in both the lower triangle `iE' ≤ iE` and the upper triangle `iE' > iE` — is computed independently by the decomposition formula above; no entry is derived from its transpose. A correct implementation must produce each requested `(iE', iE)` entry by interpolating that entry's own corner stack, not by copying or scaling the `(iE, iE')` entry.

This is the explicit contrast with `opacity-nes-pair.md`, where the aligned routine computes only the lower triangle and the consumer fills the upper triangle by physics symmetry (NES: `Phi(E', E) = Phi(E, E')·exp((E − E')/T)`; Pair: crossing-symmetry index/component swap). For Brem there is no such relation and no fill step.

### Boundary / out-of-range / NaN (bit-for-bit with weaklib)

Replicate the permissive behavior exactly (see `fortran-parity-and-tolerances.md`):

- Out-of-range `ρ` (any effective density) or `T`: clamp each bracket index to the edge cell `[1, n-1]`, do **not** clamp the delta. A below-range query gives `d < 0`, above-range `d > 1`, producing **linear extrapolation from the edge cell** in the `LOG10` coordinate — not an error, not a result clamp, not a NaN. No range check; range enforcement is a consumer responsibility. Each effective density is clamped independently.
- Non-positive `ρ_l` or `T`: the caller forms `LOG10` of a non-positive argument, yielding NaN that propagates silently to that effective density's `Interp_l` and hence to `SumInterp`. The C++ result must be NaN in the same circumstances.
- The energy indices `(iE', iE)` and the `moment` index are assumed valid (`1..nE'`, `1..nE`, `1..nMom`); range-checking them is a consumer responsibility (out-of-range integer indices are undefined, mirroring the Fortran which indexes without a guard).

### Conventions restated (the subset this channel uses)

- `ρ` and `T` interpolated in log space (caller passes `LOG10`); the energy axes `(E', E)` and the `moment` index are not interpolated.
- `ρ` in g cm⁻³, `T` in K (the EOS/EmAb/Iso thermodynamic units; **not** the NES/Pair MeV convention).
- No η, no Yₑ; the two thermodynamic axes are `(ρ, T)`.
- The density axis is queried at three effective densities `ρ·xₚ`, `ρ·xₙ`, `ρ·√(xₚ·xₙ)`.
- Universal recovery `value = 10**(stored) - offset`, applied per effective density before weighting.
- Decomposition weights `Alpha_Brem = [1, 1, 28/3]`.
- Offset dimensionality: **2D `Offsets[nOpacities, nMoments]`**; one scalar `OS` per `(opacity, moment)`, shared across the three effective densities.
- Fortran column-major tables: first index (E') fastest-varying. 5D flat offset `iEp + nE'*(iE + nE*(m + nMom*(iD + nρ*iT)))`.
- `double` (`dp = 8`) throughout.

## Verification

### Self-contained checks (the regression suite)

Run against both synthetic in-suite tables and the real reference table `wl-Op-SFHo-15-25-50-E40-Brem.h5`:

1. **Affine-in-log exactness (machine-precision tier `~1e-14`).** Build a synthetic 5D `(E', E, moment, ρ, T)` table whose stored value, at fixed `(iE', iE, moment)`, is an exact affine function of `(log10 ρ, log10 T)` — with a distinct affine offset per `(iE', iE, moment)` triple so a wrong energy/moment index is caught. The single-effective-density bilinear-in-log interpolation `K` at fixed `(iE', iE, moment)` must reproduce `10**(affine) - OS` exactly at *any* interior `(ρ, T)`, not just at nodes. The constant table is the degenerate case.
2. **Node identity (machine-precision tier).** Querying a single effective density exactly at a `(ρ, T)` grid node (with `LogD`/`LogT` equal to a `LOG10`'d node) at fixed `(iE', iE, moment)` returns that node's recovered value `10**(Table(iE', iE, moment, iD, iT)) - OS`. Verify on the real table too.
3. **Moment-slice independence (machine-precision tier).** On a synthetic table with a different known affine function per `moment` slice, interpolating at each `moment` must recover that slice's function and be unaffected by the others — confirming the `moment` index (and the energy indices) are pure slices with the matching 2D offset element `Offsets(opacity, moment)`.
4. **Effective-density decomposition (default parity tier `1e-12`/`1e-30`).** Pick a fixed `(iE', iE, moment, T)` and three densities `ρ·xₚ`, `ρ·xₙ`, `ρ·√(xₚ·xₙ)` (choose `ρ`, `xₚ`, `xₙ` in-bounds). Compute the three single-effective-density interpolations `K₁ = K(ρ·xₚ)`, `K₂ = K(ρ·xₙ)`, `K₃ = K(ρ·√(xₚ·xₙ))` independently, and assert the summed routine's output equals `1·K₁ + 1·K₂ + (28/3)·K₃`. This is the exact linear combination the routine computes, so it holds to the default parity tier. Use distinct synthetic affine functions for the three corner stacks (or a non-affine table) so the three terms are genuinely different and the weight `28/3` is exercised (a degenerate equal-`K` table cannot distinguish the weights).
5. **Weight-order sensitivity (default parity tier).** On a synthetic table where the three effective densities yield distinct `K_l`, confirm that permuting the weights or the effective-density assignment changes the result — i.e. the implementation applies `28/3` to the **third** (cross-term) effective density `ρ·√(xₚ·xₙ)`, not to `ρ·xₚ` or `ρ·xₙ`. (Assert the correct assignment matches the closed-form `1·K₁ + 1·K₂ + (28/3)·K₃`.)
6. **Both-triangles-computed (machine-precision tier).** On a synthetic table that is *not* symmetric under transposing the two energy indices (`Table(i, j, …) ≠ Table(j, i, …)`), confirm that the `(iE', iE)` output and the `(iE, iE')` output each independently equal their own decomposition — i.e. no entry is derived from its transpose and no detailed-balance/crossing fill is applied.
7. **Boundary extrapolation (no tolerance / exact relation).** A query just outside an edge of `ρ` (for one effective density) or `T` must make that effective density's term equal the edge cell's linear extrapolation (the same bilinear formula evaluated with the unclamped delta) — confirming clamp-index-but-not-delta.
8. **NaN propagation (NaN-equality).** A query with a non-positive effective density `ρ_l` or non-positive `T` must produce a NaN `SumInterp`.

### Mechanical (validator)

`bash specs/tools/validate_specs.sh` (default mode) asserts: the 7 mandated sections in order; the `SumLogInterpolateSingleVariable_2D2D_Custom_Aligned` routine name present; the decomposition weight `28/3` present; the cited weaklib source-of-truth paths resolve; and the documented `/Scat_Brem_Kernels/S_sigma` structure is confirmed against the committed snapshot `wl-Op-SFHo-15-25-50-E40-Brem.h5ls`, with the table cited by name in this spec.

## Implementation freedom

- Whether the aligned interpolation indexes the 5D buffer with the energy/`moment` indices fixed in the arithmetic, or first gathers a contiguous 4D `(E', E, ρ, T)` slice (results must be identical).
- The internal bilinear evaluation order and whether the 4 corners are read into locals or indexed in place.
- Whether the three effective-density interpolations are computed in a loop over `l`, fully unrolled, or vectorized — provided the accumulation `Σ_l Alpha(l)·Interp_l` is in the stated order (offset recovery inside the sum, weights on recovered values).
- Whether the consumer forms the three effective densities (`ρ·xₚ`, `ρ·xₙ`, `ρ·√(xₚ·xₙ)`) or the entry point receives `ρ`, `xₚ`, `xₙ` and forms them — provided the observable result (including NaN-on-non-positive) matches; the `√` cross term uses `√|xₚ·xₙ|`.
- Whether the caller pre-`LOG10`'s `ρ`/`T` or the entry point does it internally, provided the observable result matches the `_Point` contract.
- The exact `_Point` signature argument grouping (subject to the scalar/allocation-free/`double const*`-plus-extents contract in `amrex-device-interface.md`).
- Whether the multi-point array form is a hand-written loop or a `ParallelFor` over the `_Point` core.
- Whether all `nE'×nE` energy entries are computed in a full double loop or a triangular loop is used as a performance optimization — provided the full-plane result has every entry computed by its own decomposition (no symmetry fill), so the choice is invisible to the caller.
- How the three effective densities are tabulated/passed (a `nSpecies × nPoints` array as the Fortran does, or three separate scalars) — the contract is the values and weights, not the container.

## Open questions / assumptions

- **Brem `GetIndexAndDelta` / `LinearInterp_Array_Point` overload provenance (research OQ#2 — assumption, explicitly non-blocking).** weaklib's standalone full-energy-grid Brem wrapper `wlInterpolateOpacity_Brem` (`wlOpacityInterpolationModule.f90:236-277`) calls an unsuffixed generic `GetIndexAndDelta(…)` and a generic `LinearInterp_Array_Point(ii, jj, iD, iT, iY, dD, dT, dY, OS, Table)`. Both are resolved by Fortran generic-interface overload at `wlInterpolationUtilitiesModule.f90:43-59`: `GetIndexAndDelta` is the `_Lin`/`_Log` interface and `LinearInterp_Array_Point` dispatches by argument shape to `LinearInterp{1..5}D_*DArray_Point`. From the call's argument signature (five integer indices `ii, jj, iD, iT, iY` + three deltas `dD, dT, dY` + `OS` + a rank-5 `Table`), the selected overload is the **5D leaf** `LinearInterp5D_5DArray_Point` — a full 5D interpolation over `(E', E, ρ, T, Yₑ)`-style axes, *not* the 2D-aligned leaf. The exact pinned definition of that 5D leaf for this revision was not traced end-to-end (the standalone wrapper path is not what the reference consumer uses). **This is off the reference consumer's critical path:** `wlOpacityInterpolationModule.f90` is never `USE`d by thornado, whose production Brem path (`NeutrinoOpacitiesComputationModule.F90:2471-2473`) drives the **aligned, summed** routine `SumLogInterpolateSingleVariable_2D2D_Custom_Aligned` on the pre-aligned `Brem_AT` table (research §7). This spec therefore pins the **aligned** path — whose inner kernel `LinearInterp2D_4DArray_2DAligned_Point` (`wlInterpolationUtilitiesModule.F90:602-627`) *is* fully traced — as the source-of-truth interpolation contract, and the standalone-wrapper overload is recorded here purely as a provenance note for weaklib's full-energy-grid path, which the C++ port does not need to reproduce. The Ralph loop is never blocked on this overload: Brem correctness is anchored to the aligned bilinear interpolation plus the effective-density decomposition invariant, both fully specified above.
- **Concrete offsets and ρ/T grid extents (assumption, non-blocking).** The 2D `Offsets[nOpacities, nMoments]` value(s) and the density/temperature node coordinates live only in the production `.h5` file (research OQ#3). This spec pins the recovery contract, offset dimensionality, the effective-density set, and the decomposition weights; the fixture/table supplies the numbers. The exactness and decomposition checks use synthetic tables whose offsets the suite chooses, so they do not depend on the unknown production values.
- **`Alpha_Brem` and the effective densities are pinned by the reference consumer (assumption).** `Alpha_Brem = [1, 1, 28/3]` and the three effective densities `ρ·xₚ`, `ρ·xₙ`, `ρ·√(xₚ·xₙ)` are taken from thornado `NeutrinoOpacitiesComputationModule.F90` (`Alpha_Brem(3) = [1.0d0, 1.0d0, 28.d0/3.d0]`; `LogDX_P(1..3)`), which is the reference consumer of the aligned Brem table. These are treated as fixed physics constants of the decomposition, not table data. If a future table is generated with a different decomposition convention, the weights/effective densities must be re-read from the consumer-of-record at that table's pinned commit; the interpolation-plus-decomposition *structure* in this spec is unchanged.
- **`nMoments` is read from the table (assumption, non-blocking).** In the pinned Brem table `nMoments = 1` (the `S_sigma {81, 185, 1, 40, 40}` and `Offsets {1, 1}` snapshot) and `nOpacities = 1`. The interpolation contract is per-`(opacity, moment)` (one `OS` + one log-stored 4D `(E', E, ρ, T)` sub-table); the counts are read per `table-format-and-io.md`. A table with `nMoments > 1` interpolates each moment slice independently with its own `Offsets(opacity, moment)`.
- **Temperature/density units (assumption).** This spec assumes the Brem `(ρ, T)` axes use the EOS/EmAb/Iso convention (`ρ` in g cm⁻³, `T` in K), in contrast to the MeV temperature of NES/Pair. The on-disk `/ThermoState/Units` dataset is authoritative; the consumer must pass `ρ`, `T` in the same units the `/ThermoState/Density` / `/ThermoState/Temperature` grids were tabulated in.
