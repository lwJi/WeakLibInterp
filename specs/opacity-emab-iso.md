# Opacity interpolation: EmAb (4D) + Iso (5D)

> Leaf spec. Self-contained: an agent can implement the EmAb and isoenergetic-scattering single-point interpolators from this file alone. It restates only the conventions it uses and references `fortran-parity-and-tolerances.md` (numeric contract) and `amrex-device-interface.md` (device contract) for the shared cross-cutting details. `README.md` is canonical if any restated convention here conflicts with it.

## Purpose & scope

This spec defines the device-callable, single-point interpolation of the two neutrino-opacity channels that share the EOS thermodynamic axes `(ρ, T, Yₑ)`:

- **EmAb** — emission/absorption opacity, a **4D** table over `(E, ρ, T, Yₑ)`.
- **Iso** — isoenergetic (elastic) scattering kernel, a **5D** table over `(E, moment, ρ, T, Yₑ)`, where the discrete `moment` index selects a Legendre moment of the scattering kernel and is **not interpolated**.

Both channels store every value as `log10(value + offset)` and recover the physical value by `10**(...) - offset`. Energy `E`, density `ρ`, and temperature `T` are interpolated in **log** space; `Yₑ` in **linear** space; the `moment` index (Iso only) is a direct integer slice.

In scope:
- The single-point evaluate contract for one neutrino species: EmAb `(E, ρ, T, Yₑ) → value`; Iso `(E, moment, ρ, T, Yₑ) → value` at a fixed `moment`.
- Which query coordinates arrive **pre-`LOG10`** (`E, ρ, T`) versus raw (`Yₑ`), and the units/valid ranges of each.
- The per-channel offset dimensionality (EmAb 1D `Offsets[nOpacities]`; Iso 2D `Offsets[nOpacities, nMoments]`) and species/moment index constants.
- Column-major table indexing for the 4D EmAb table and the 5D Iso table (and how the Iso 5D table is sliced to a 4D sub-table at a fixed `(species, moment)`).
- Boundary/out-of-range/NaN behavior (bit-for-bit with weaklib).
- Layer-1 closed-form checks.

Out of scope:
- The two-energy channels NES/Pair (`(E', E, kernel, T, η)`) — see `opacity-nes-pair.md`.
- Brem (`(E', E, moment, ρ, T)` with density decomposition) — see `opacity-brem.md`.
- The physics assembly of opacities/IMFPs from the interpolated kernels (coupling constants, angular integrals) — this spec covers only the table interpolation that produces one kernel/opacity value.
- The on-disk HDF5 layout the tables/axes are read from — see `table-format-and-io.md`.
- The EOS dependent-variable interpolation — see `eos-interpolation.md`.

## Source of truth

Pinned weaklib commit: see `weaklib_commit` in `specs/fixtures/tables.provenance`.

- `weaklib/Distributions/Library/wlInterpolationModule.F90` — the named generator-of-record `_Point` routine for **both** channels:
  - `LogInterpolateSingleVariable_4D_Custom_Point` (subroutine at `wlInterpolationModule.F90:1754-1779`) — the single-point evaluate workhorse. Signature `( LogE, LogD, LogT, Y, LogEs, LogDs, LogTs, Ys, OS, Table, Interpolant )`: the first three query coordinates `LogE, LogD, LogT` are **already `LOG10`'d** by the caller and located via `GetIndexAndDelta_Lin` on the log grids `LogEs, LogDs, LogTs`; the fourth, `Y`, is the **linear** `Yₑ` located on `Ys`; `OS` is the scalar additive offset for the chosen `(species[, moment])`; `Table(iE, iD, iT, iY)` is the log-stored 4D array; `Interpolant` is the scalar out. It computes four bracket indices + deltas, reads the 16 corners, evaluates the tetralinear sum in log space, and returns `10**(...) - OS`.
  - The matched array form `LogInterpolateSingleVariable_4D_Custom` (`wlInterpolationModule.F90:1710-1751`) is the host-level loop over `_Point` (not device-callable); the C++ array form is the `ParallelFor` wrapper over the `_Point` core (see `amrex-device-interface.md`).
- `weaklib/Distributions/Library/wlInterpolationUtilitiesModule.F90` — the shared primitives the `_Point` routine calls: `GetIndexAndDelta_Lin` (the bracket-and-delta search used on the pre-`LOG10`'d energy/ρ/T grids and on the linear `Yₑ` grid), the `TetraLinear` 16-corner basis, and `LinearInterp4D_4DArray_Point` (subroutine at `wlInterpolationUtilitiesModule.F90:729-768`) which reads the 16 corners, calls `TetraLinear`, and owns the `10**(...) - OS` recovery.
- `weaklib/Distributions/OpacitySource/wlOpacityFieldsModule.f90` — channel geometry and index constants: neutrino-species constants `iNu_e = 1`, `iNu_e_bar = 2`, `iNu_x = 3`, `iNu_x_bar = 4` (`wlOpacityFieldsModule.f90:10-13`); the `OpacityTypeEmAb` (4D, 1D `Offsets`) and `OpacityTypeScatIso` (5D, 2D `Offsets(nOpacities, nMoments)`) field-type definitions.

**The 5D Iso table is interpolated via the 4D `_Point` routine on a fixed-`(species, moment)` slice.** The reference consumer path (`thornado/Modules/Opacities/NeutrinoOpacitiesComputationModule.F90:1505-1508`) slices the 5D Iso table at the requested integer `moment` and species, `Iso_T(:,:,:,:, iMoment, iC)`, then calls `LogInterpolateSingleVariable_4D_Custom_Point` on that 4D `(E, ρ, T, Yₑ)` sub-table with the 2D offset `OS_Iso(iC, iMoment)`. So both channels share one interpolation kernel; the only Iso-specific step is selecting the `(species, moment)` slice and the 2D offset before the 4D call.

These `_Point` routines (and the slice-then-4D path) are the generator-of-record for Layer-2 parity (see `fortran-parity-and-tolerances.md`).

## Inputs & outputs

Value type is `double` throughout (weaklib `dp = 8`); see `fortran-parity-and-tolerances.md`. Device contract (qualifiers, `Gpu::DeviceVector<double>` residency, `double const*` table passing, `ParallelFor` launch) is `amrex-device-interface.md`.

### Query coordinates — order, units, and which arrive pre-`LOG10`

**EmAb (4D):** the query is the 4-tuple, in this fixed order:

| Slot | Symbol | Quantity | Unit | Interp space | Passed as |
|---|---|---|---|---|---|
| 1 | E | neutrino energy | MeV | **log** | **pre-`LOG10`** (`LogE = LOG10(E)`) |
| 2 | ρ | mass density | grams per cm³ | **log** | **pre-`LOG10`** (`LogD = LOG10(ρ)`) |
| 3 | T | temperature | K (Kelvin) | **log** | **pre-`LOG10`** (`LogT = LOG10(T)`) |
| 4 | Yₑ | electron fraction | dimensionless | **linear** | **raw** (`Y = Yₑ`) |

**Iso (5D):** the same four interpolated coordinates plus a discrete **moment index** that selects a 5D-table slice and is **not interpolated**:

| Slot | Symbol | Quantity | Unit | Interp space | Passed as |
|---|---|---|---|---|---|
| 1 | E | neutrino energy | MeV | **log** | **pre-`LOG10`** |
| 2 | moment | Legendre-moment index | integer `1..nMoments` | **none (direct slice)** | integer index |
| 3 | ρ | mass density | grams per cm³ | **log** | **pre-`LOG10`** |
| 4 | T | temperature | K | **log** | **pre-`LOG10`** |
| 5 | Yₑ | electron fraction | dimensionless | **linear** | **raw** |

The critical distinction a fresh agent must observe: **`E`, `ρ`, `T` are located with the *linear* index/delta formula applied to their already-`LOG10`'d coordinates against already-`LOG10`'d grids** (the caller has taken the log once, up front; the routine does not log again), while **`Yₑ` is located with the linear formula on its raw values**. This matches `GetIndexAndDelta_Lin` being called for all four coordinates in the `_Point` routine — the log-ness of E/ρ/T lives in the fact that both the query and the grid are pre-`LOG10`'d, not in the index formula. (Contrast `eos-interpolation.md`, where the 3D EOS routine takes raw ρ/T and logs them internally.)

### Axis arrays, offsets, and table

- `LogEs(1:nE)`, `LogDs(1:nρ)`, `LogTs(1:nT)` — the strictly monotone-ascending **`LOG10`'d** grid-node coordinates for E, ρ, T (i.e. `LOG10` of `/EnergyGrid/Values` and of `/ThermoState/Density`, `/Temperature`).
- `Ys(1:nYe)` — the strictly monotone `Yₑ` node coordinates (raw, linear).
- `OS` — the scalar additive offset. EmAb: a single value per species from the **1D** `Offsets[nOpacities]`. Iso: a single value per `(species, moment)` from the **2D** `Offsets[nOpacities, nMoments]`. The caller selects the scalar before the 4D call.
- `Table` — the log-stored values.
  - **EmAb (4D):** `log10(physical + OS)` indexed `(iE, iD, iT, iY)`, Fortran column-major (E fastest-varying). As a flat `double const*` the element `Table(iE,iD,iT,iY)` (0-based) is `table[ iE + nE*( iD + nρ*( iT + nT*iY ) ) ]`.
  - **Iso (5D):** `log10(physical + OS)` indexed `(iE, iMom, iD, iT, iY)`, Fortran column-major (E fastest-varying). The flat 0-based offset is `iE + nE*( iMom + nMom*( iD + nρ*( iT + nT*iY ) ) )`. Selecting a fixed `iMom` yields the 4D `(E, ρ, T, Yₑ)` sub-table the 4D kernel interpolates; that sub-table is **strided** in the 5D buffer (E contiguous, then a stride of `nE·nMom` between successive ρ planes). Whether the implementation interpolates the 5D buffer with the moment fixed in the index arithmetic, or first gathers a contiguous 4D slice, is implementation freedom — the observable result is identical.

### Species / moment index constants

- Neutrino species: `iNu_e = 1`, `iNu_e_bar = 2`, `iNu_x = 3`, `iNu_x_bar = 4` (`wlOpacityFieldsModule.f90:10-13`). The production EmAb/Iso tables carry the two electron-flavor species as the named datasets `Electron Neutrino` and `Electron Antineutrino` (`nOpacities = 2`).
- Moment index (Iso only): an integer in `1..nMoments`; `nMoments = 2` in the pinned Iso table. It selects which moment slice of the 5D kernel is interpolated and is used **directly as an integer index** — never interpolated, never log-transformed.

### Outputs

- A single recovered physical value: the EmAb opacity for the chosen species, or the Iso scattering-kernel moment for the chosen `(species, moment)`, in that channel's stored units (per-species `/…/Units`).

### Reference tables (anchor fixtures and Layer-1 checks)

- **EmAb:** `wl-Op-SFHo-15-25-50-E40-EmAb.h5`, pinned by path + `sha256` in `specs/fixtures/tables.provenance`; structure committed at `specs/fixtures/wl-Op-SFHo-15-25-50-E40-EmAb.h5ls`. The opacity arrays live under `/EmAb` (`/EmAb/Electron Neutrino`, `/EmAb/Electron Antineutrino`, each `h5ls` `{30, 81, 185, 40}` = Fortran `(40, 185, 81, 30) = (nE, nρ, nT, nYe)`), the energy axis under `/EnergyGrid/Values` `[40]`, the thermodynamic axes under `/ThermoState` (`/ThermoState/Density` `[185]`, `/ThermoState/Temperature` `[81]`, `/ThermoState/Electron Fraction` `[30]`), and the **1D** offsets under `/EmAb/Offsets` `[2]`.
- **Iso:** `wl-Op-SFHo-15-25-50-E40-Iso.h5`, pinned the same way; structure at `specs/fixtures/wl-Op-SFHo-15-25-50-E40-Iso.h5ls`. The kernels live under `/Scat_Iso_Kernels` (`/Scat_Iso_Kernels/Electron Neutrino`, `…/Electron Antineutrino`, each `h5ls` `{30, 81, 185, 2, 40}` = Fortran `(40, 2, 185, 81, 30) = (nE, nMom, nρ, nT, nYe)`), and the **2D** offsets under `/Scat_Iso_Kernels/Offsets` `{2, 2}` = `(nOpacities, nMoments)`. The energy and thermodynamic axes are the same `/EnergyGrid` and `/ThermoState` groups as the EmAb file.

Full on-disk contract (group/dataset names, dtypes, legacy fallbacks, the column-major→C-shape reversal): `table-format-and-io.md`.

## Correctness requirements

### The interpolation formula (both channels)

For a query whose interpolated coordinates `(LogE, LogD, LogT, Y)` are strictly inside the grid (brackets/deltas as in `fortran-parity-and-tolerances.md`, located with the linear formula on each coordinate):

- `iE` from the linear bracket on `LogEs`, `dE = (LogE − LogEs(iE)) / (LogEs(iE+1) − LogEs(iE))`.
- `iD` from the linear bracket on `LogDs`, `dD = (LogD − LogDs(iD)) / (LogDs(iD+1) − LogDs(iD))`.
- `iT` from the linear bracket on `LogTs`, `dT = (LogT − LogTs(iT)) / (LogTs(iT+1) − LogTs(iT))`.
- `iY` from the linear bracket on `Ys`, `dY = (Y − Ys(iY)) / (Ys(iY+1) − Ys(iY))`.

Read the 16 corner log-values `p_{abcd} = Table(iE+a, iD+b, iT+c, iY+d)` for `a,b,c,d ∈ {0,1}` (for Iso, at the fixed `iMom`) and form the tetralinear interpolant in log space, then recover:

```
Interpolant = 10**( tetralinear(p0000..p1111, dE, dD, dT, dY) ) - OS
```

where `tetralinear` is the standard 16-corner form (linear in `dE`, then `dD`, then `dT`, then `dY`). The result must match `LogInterpolateSingleVariable_4D_Custom_Point` on identical inputs (EmAb: the full 4D table; Iso: the 4D slice at the chosen `(species, moment)`) at the **default parity tier** (`rtol 1e-12`, `atol 1e-30`).

Because both `E`/`ρ`/`T` are interpolated **linearly in their `LOG10`'d coordinate** (the caller pre-`LOG10`'d them) and `Yₑ` is interpolated linearly in its raw coordinate, the interpolation is multilinear in `(log10 E, log10 ρ, log10 T, Yₑ)`.

### Iso moment index

The `moment` index is a direct integer slice into axis 2 of the 5D table; it is never interpolated. Selecting `iMom` and the corresponding `OS = Offsets(species, iMom)` reduces Iso exactly to the 4D EmAb formula above on the 4D sub-table. A wrong moment slice or a wrong offset element (e.g. transposing `(species, moment)`) is a correctness failure.

### Boundary / out-of-range / NaN (bit-for-bit with weaklib)

Replicate the permissive behavior exactly (see `fortran-parity-and-tolerances.md`):

- Out-of-range `(E, ρ, T, Yₑ)`: clamp the bracket index to the edge cell `[1, n-1]`, do **not** clamp the delta. A below-range query gives `d < 0`, above-range gives `d > 1`, producing **linear extrapolation from the edge cell** (in the `LOG10` coordinate for E/ρ/T, in the raw coordinate for Yₑ) — not an error, not a result clamp, not a NaN. No range check; range enforcement is a consumer responsibility.
- Non-positive `E`, `ρ`, or `T`: the caller forms `LOG10` of a non-positive argument, yielding NaN that propagates silently to `Interpolant`. The C++ result must be NaN in the same circumstances (whether the `LOG10` is taken by the caller or inside the entry point, the observable NaN-on-non-positive behavior must match). `Yₑ` (linear axis) does not log, so a non-positive `Yₑ` extrapolates rather than NaNs.
- The `moment` index is assumed valid (`1..nMoments`); range-checking it is a consumer responsibility (out-of-range integer indices are undefined, mirroring the Fortran which indexes without a guard).

### Conventions restated (the subset these channels use)

- E, ρ, T interpolated in log space (caller passes `LOG10`); Yₑ in linear space; `moment` not interpolated.
- Universal recovery `value = 10**(stored) - offset`.
- Offset dimensionality: **EmAb 1D `Offsets[nOpacities]`**, **Iso 2D `Offsets[nOpacities, nMoments]`**.
- Fortran column-major tables: first index (E) fastest-varying. EmAb flat offset `iE + nE*(iD + nρ*(iT + nT*iY))`; Iso flat offset `iE + nE*(iMom + nMom*(iD + nρ*(iT + nT*iY)))`.
- `double` (`dp = 8`) throughout.

## Verification

### Layer 1 — self-contained checks (the active gate)

Run against both synthetic in-suite tables and the real reference tables `wl-Op-SFHo-15-25-50-E40-EmAb.h5` / `…-Iso.h5`:

1. **Affine-in-log exactness (machine-precision tier `~1e-14`).** Build a synthetic 4D (EmAb) and 5D (Iso) table whose stored value is an exact affine function of `(log10 E, log10 ρ, log10 T, Yₑ)` — for Iso, with a distinct affine offset per `moment` slice so a wrong slice is caught. Tetralinear-in-log interpolation must reproduce `10**(affine) - OS` exactly at *any* interior query, not just at nodes. The constant table is the degenerate case.
2. **Node identity (machine-precision tier).** Querying exactly at a grid node (with `LogE/LogD/LogT` equal to a `LOG10`'d node and `Y` equal to a `Yₑ` node) returns that node's recovered value `10**(Table(iE,iD,iT,iY)) - OS` (Iso: at the chosen `iMom`). Verify on the real tables too.
3. **Moment-slice independence (Iso, machine-precision tier).** On a synthetic Iso table with a different known affine function per `moment`, interpolating at each `iMom` must recover that moment's function and be unaffected by the others — confirming the moment index is a pure slice with the matching 2D offset element.
4. **Boundary extrapolation (no tolerance / exact relation).** A query just outside an edge of E, ρ, T, or Yₑ must equal the edge cell's linear extrapolation (the same tetralinear formula evaluated with the unclamped delta) — confirming clamp-index-but-not-delta.
5. **NaN propagation (NaN-equality).** A query with non-positive E, ρ, or T must produce a NaN `Interpolant`.

### Layer 2 — Fortran parity (specified, PENDING)

Compare `Interpolant` (default tier `1e-12`/`1e-30`) against committed golden fixtures generated offline by `LogInterpolateSingleVariable_4D_Custom_Point` at the pinned commit — for EmAb over query points drawn from `wl-Op-SFHo-15-25-50-E40-EmAb.h5`, and for Iso by slicing `wl-Op-SFHo-15-25-50-E40-Iso.h5` at each `(species, moment)` and applying the same 4D routine. Until fixtures exist these tests report **pending**, not passing (see `fortran-parity-and-tolerances.md`).

### Mechanical (validator)

`bash specs/tools/validate_specs.sh` (default mode) asserts: the 7 mandated sections in order; the `LogInterpolateSingleVariable_4D_Custom_Point` routine name present; the cited weaklib source-of-truth paths resolve; and the offset-dimensionality claims are confirmed against the committed snapshots — the **1D** `/EmAb/Offsets` in `wl-Op-SFHo-15-25-50-E40-EmAb.h5ls` and the **2D** `/Scat_Iso_Kernels/Offsets` in `wl-Op-SFHo-15-25-50-E40-Iso.h5ls`, with each table cited by name in this spec.

## Implementation freedom

- Whether the Iso interpolation gathers a contiguous 4D slice at the chosen `moment` or interpolates the 5D buffer in place with the moment fixed in the index arithmetic (results must be identical).
- The internal tetralinear evaluation order and whether the 16 corners are read into locals or indexed in place.
- Whether EmAb and the Iso-slice path share one 4D kernel (the weaklib source does) or are separate code.
- Whether the caller pre-`LOG10`'s `E/ρ/T` or the entry point does it internally, provided the observable result — including NaN-on-non-positive — matches the `_Point` contract.
- The exact `_Point` signature argument grouping (subject to the scalar/allocation-free/`double const*`-plus-extents contract in `amrex-device-interface.md`).
- Whether the multi-point array form is a hand-written loop or a `ParallelFor` over the `_Point` core.
- Any caching/precomputation of node coordinates or `LOG10`'d grids, provided results meet the stated tolerances.

## Open questions / assumptions

- **Layer-2 golden fixtures are future work (assumption, non-blocking).** No Fortran-generated golden opacity interpolation outputs exist or can be generated in this environment; Layer-2 tests ship **pending**, the named `_Point` routine remains the generator-of-record, and the Ralph loop gates on Layer 1. See `fortran-parity-and-tolerances.md`.
- **Concrete per-channel offsets and energy-grid extents (assumption, non-blocking).** The `Offsets` values (1D for EmAb, 2D for Iso) and the energy/η node coordinates live only in the production `.h5` files (research OQ#3). This spec pins the recovery contract and offset dimensionality; the fixture/table supplies the numbers. Layer-1 exactness checks use synthetic tables whose offsets the suite chooses, so they do not depend on the unknown production offsets.
- **Species/moment counts are read from the table (assumption, non-blocking).** `nOpacities` (2 here) and `nMoments` (2 here for Iso) are authoritative via the `/…/nOpacities`, `/Scat_Iso_Kernels/nMoments` datasets and the `Names` ordering; this spec's contract is per-`(species[, moment])` (one `OS` + one log-stored sub-table), with the counts read per `table-format-and-io.md`. The `iNu_x`/`iNu_x_bar` heavy-lepton species exist as constants but are not carried as named datasets in the pinned 2-species production tables.
