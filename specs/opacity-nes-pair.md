# Opacity interpolation: NES + Pair (two-energy, on `(E', E, kernel, T, η)`)

> Leaf spec. Self-contained: an agent can implement the NES (neutrino–electron scattering) and Pair (electron–positron pair) kernel interpolators from this file alone. It restates only the conventions it uses and references `fortran-parity-and-tolerances.md` (numeric contract) and `amrex-device-interface.md` (device contract) for the shared cross-cutting details. `README.md` is canonical if any restated convention here conflicts with it.

## Purpose & scope

This spec defines the device-callable interpolation of the two **two-energy** neutrino-opacity scattering channels that depend on temperature `T` and electron degeneracy `η = μₑ / (kT)` (and **not** on ρ or Yₑ):

- **NES** — neutrino–electron scattering kernel, stored as a **5D** table over `(E', E, kernel, T, η)`.
- **Pair** — electron–positron pair (production/annihilation) kernel, stored as a **5D** table over `(E', E, kernel, T, η)`.

Both share the same table geometry: two neutrino-energy axes `(E', E)`, a discrete `kernel` index (a small set of Legendre-moment / coupling-component slices, **not interpolated**), and two thermodynamic axes `(T, η)` interpolated in **log** space. Every value is stored as `log10(value + offset)` and recovered by `10**(...) - offset`.

The reference consumer (thornado) does **not** interpolate the two energy axes in-kernel. It pre-aligns the kernel tables onto its own discrete neutrino-energy quadrature at initialization time (`NES_AT` / `Pair_AT`), so at evaluation time the two energy indices `(iE', iE)` are used **directly as table indices** and only the `(T, η)` plane is bilinearly interpolated. This spec's device contract is therefore the **aligned** path: `LogInterpolateSingleVariable_2D2D_Custom_Aligned(_Point)` — a 2D `(T, η)` bilinear interpolation at a fixed `(iE', iE, kernel)`.

Correctness is anchored to two physics invariants that hold independently of the interpolation machinery's provenance:
- **NES detailed balance:** `Phi(E', E) = Phi(E, E') · exp((E − E') / T)`.
- **Pair crossing symmetry** (Bruenn 1985, Eq. C64): the upper energy triangle is filled by swapping `(iE, iEp)` **and** the in-pair / cross-pair kernel components (`Ji ↔ Jii`).

In scope:
- The single-point **aligned** evaluate contract: at fixed energy indices `(iE', iE)` and `kernel` index, interpolate one kernel value bilinearly in `(log10 T, log10 η)`.
- Independent-variable order/units; which coordinates arrive **pre-`LOG10`** (`T`, `η`) and which axes are not interpolated (`E'`, `E`, `kernel`).
- The kernel-index constants for NES (`iHi0`, `iHii0`, `iHi1`, `iHii1`) and Pair (`iJi0`, `iJii0`, `iJi1`, `iJii1`).
- The detailed-balance (NES) and crossing-symmetry (Pair) invariants as Layer-1 closure checks: which triangle is computed vs. filled, the exact fill formula, and the tolerance.
- The 2D offset dimensionality `Offsets[nOpacities, nMoments]`.
- Column-major indexing of the 5D `(E', E, kernel, T, η)` table and its 4D `(E', E, T, η)` sub-table at a fixed `kernel`.
- Boundary/out-of-range/NaN behavior (bit-for-bit with weaklib).

Out of scope:
- The EmAb (4D) and Iso (5D, `(E, moment, ρ, T, Yₑ)`) channels — see `opacity-emab-iso.md`.
- Brem (`(E', E, moment, ρ, T)`, density decomposition, no symmetry fill) — see `opacity-brem.md`.
- The full physics assembly of `PhiNES` / `PhiPair` from the interpolated kernels — the weak-coupling constants `cv`, `ca` and the per-species combinations `C_i = (cv ± ca)²`, `C_ii = (cv ∓ ca)²` are **fixture/provenance data** for the consumer's kernel assembly, not interpolation contract. This spec covers only the interpolation that produces one kernel value at one `(iE', iE, kernel, T, η)` point, plus the symmetry that relates the two energy triangles of the assembled kernel.
- Computing `η` from `(ρ, T, Yₑ)` (it is the electron chemical potential over `kT`, supplied by the EOS path — see `eos-interpolation.md`); here `η` is an input query coordinate.
- The non-aligned full-energy-grid weaklib path (`LogInterpolateOpacity_2D1D2D`) — see Open questions / assumptions.
- The on-disk HDF5 layout — see `table-format-and-io.md`.

## Source of truth

Pinned weaklib commit: see `weaklib_commit` in `specs/fixtures/tables.provenance`.

- `weaklib/Distributions/Library/wlInterpolationModule.F90` — the named generator-of-record for the **aligned** consumer path (the contract this spec pins):
  - `LogInterpolateSingleVariable_2D2D_Custom_Aligned_Point` (subroutine at `wlInterpolationModule.F90:1455-1485`) — the scalar, allocation-free, device-callable form. Signature `( LogT, LogX, LogTs, LogXs, OS, Table, Interpolant )`: `LogT` and `LogX` are the **already-`LOG10`'d** scalar query coordinates for `T` and `X = η`, located on the `LOG10`'d grids `LogTs`, `LogXs` via `GetIndexAndDelta_Lin`; `OS` is the scalar additive offset for the chosen `(opacity, kernel)`; `Table(i, j, iT, iX)` is the log-stored 4D `(E', E, T, η)` sub-table at the chosen `kernel`; `Interpolant(i, j)` is filled over the **lower energy triangle `i ≤ j`** only (the upper triangle is left to the caller's symmetry fill). The two energy indices `(i, j)` are used **directly as table indices** — there is no energy-axis interpolation.
  - The matched array form `LogInterpolateSingleVariable_2D2D_Custom_Aligned` (`wlInterpolationModule.F90:1361-1452`) is the host-level loop over query points; the C++ array form is the `ParallelFor` wrapper over the `_Point` core (see `amrex-device-interface.md`).
- `weaklib/Distributions/Library/wlInterpolationUtilitiesModule.F90` — the shared primitives the aligned routine calls:
  - `GetIndexAndDelta_Lin` — bracket-and-delta search applied to the pre-`LOG10`'d `T` and `η` coordinates against their `LOG10`'d grids.
  - `LinearInterp2D_4DArray_2DAligned_Point` (subroutine at `wlInterpolationUtilitiesModule.F90:602-627`) — reads the 4 corners `p00..p11 = Table(iX1, iX2, iY1[+1], iY2[+1])` at fixed energy indices `(iX1, iX2) = (iE', iE)`, evaluates `BiLinear` over the `(T, η)` plane, and owns the `10**(...) - OS` recovery.
- `weaklib/Distributions/OpacitySource/wlOpacityFieldsModule.f90` — the kernel-index constants (cited only for the symmetry physics, not the aligned interpolation kernel):
  - NES kernel components: `iHi0 = 1`, `iHii0 = 2`, `iHi1 = 3`, `iHii1 = 4` (`wlOpacityFieldsModule.f90:18-21`).
  - Pair kernel components: `iJi0 = 1`, `iJii0 = 2`, `iJi1 = 3`, `iJii1 = 4` (`wlOpacityFieldsModule.f90:25-28`).
  - Neutrino-species constants `iNu_e = 1`, `iNu_e_bar = 2`.
- `weaklib/Distributions/OpacitySource/wlOpacityInterpolationModule.f90` — cited **only for the symmetry physics**, not as an interpolation-routine source-of-truth:
  - NES detailed-balance fill of the upper energy triangle: `PhiNES(iEp, iE, l, iS, iX) = PhiNES(iE, iEp, l, iS, iX) · EXP((E(iE) − E(iEp)) / T(iX))` for `iEp > iE` (`wlOpacityInterpolationModule.f90:104-115`).
  - Pair crossing-symmetry fill of the upper triangle, swapping `(iE, iEp)` and `Ji ↔ Jii` (Bruenn 1985, Eq. C64) (`wlOpacityInterpolationModule.f90:196-228`).

The aligned `_Point` routine is the generator-of-record for Layer-2 parity (see `fortran-parity-and-tolerances.md`); the symmetry invariants are the Layer-1 closure checks that gate the loop today.

## Inputs & outputs

Value type is `double` throughout (weaklib `dp = 8`); see `fortran-parity-and-tolerances.md`. Device contract (qualifiers, `Gpu::DeviceVector<double>` residency, `double const*` table passing, `ParallelFor` launch) is `amrex-device-interface.md`.

### Table geometry (both channels)

The 5D kernel table is, in Fortran (column-major, first index fastest-varying) order:

```
Kernels(iE', iE, kernel, iT, iη)   shape (nE', nE, nMom, nT, nEta)
```

with `nE' = nE` (the two energy axes share the energy grid). In the pinned production tables `nE = 40`, `nMom = 4` (the four kernel components), `nT = 81`, `nEta = 120`. (The `h5ls` snapshot lists the reversed dimensions `{120, 81, 4, 40, 40}` = Fortran `(40, 40, 4, 81, 120)`.)

### Query coordinates — order, units, and which arrive pre-`LOG10`

For the **aligned** evaluate, the two energy axes are *not* query points — they are integer table indices `(iE', iE)`, chosen by the consumer's energy quadrature alignment. The only interpolated coordinates are the two thermodynamic ones:

| Slot | Symbol | Quantity | Unit | Interp space | Passed as |
|---|---|---|---|---|---|
| — | iE' | first neutrino-energy index | integer `1..nE'` | **none (direct index)** | integer index |
| — | iE | second neutrino-energy index | integer `1..nE` | **none (direct index)** | integer index |
| — | kernel | kernel-component index | integer `1..nMom` | **none (direct slice)** | integer index |
| 1 | T | temperature | **MeV** | **log** | **pre-`LOG10`** (`LogT = LOG10(T)`) |
| 2 | η | electron degeneracy `μₑ/(kT)` | dimensionless | **log** | **pre-`LOG10`** (`LogX = LOG10(η)`) |

The critical distinctions a fresh agent must observe:
- **`T` is in MeV here, not Kelvin.** The NES/Pair tables and the consumer path carry temperature in MeV (`wlOpacityInterpolationModule.f90:30`, `! --- T and TabT in MeV ---`). This differs from the EOS / EmAb / Iso channels, where `T` is in Kelvin.
- **There is no ρ and no Yₑ.** The two thermodynamic axes are `(T, η)`, not `(ρ, T, Yₑ)`. `η = μₑ/(kT)` is supplied by the consumer.
- **`T` and `η` are located with the linear index/delta formula applied to their already-`LOG10`'d coordinates against already-`LOG10`'d grids** — the caller takes the log once, up front; the routine does not log again (matches `GetIndexAndDelta_Lin` on both axes in the aligned routine).
- **The energy axes and the `kernel` index are never interpolated.** `(iE', iE)` index directly; `kernel` selects a 4D `(E', E, T, η)` sub-table.

### Axis arrays, offsets, and table

- `LogTs(1:nT)`, `LogXs(1:nEta)` — the strictly monotone-ascending **`LOG10`'d** grid-node coordinates for `T` and `η` (i.e. `LOG10` of `/ThermoState/Temperature` and `/EtaGrid/Values`).
- `OS` — the scalar additive offset for the chosen `(opacity, kernel)`, drawn from the **2D** `Offsets[nOpacities, nMoments]`. The caller selects the scalar before the 2D2D call.
- `Table` — the log-stored values.
  - **5D full table:** `log10(value + OS)` indexed `(iE', iE, kernel, iT, iη)`, Fortran column-major (E' fastest-varying). As a flat `double const*` (0-based) the element `Kernels(iEp, iE, k, iT, iEta)` is `table[ iEp + nE'*( iE + nE*( k + nMom*( iT + nT*iEta ) ) ) ]`.
  - **4D aligned sub-table at fixed `kernel = k`:** `Table(iE', iE, iT, iη)` — the energy indices used directly, the `(T, η)` plane bilinearly interpolated. Whether the implementation indexes the 5D buffer with `kernel` fixed in the arithmetic, or first gathers a contiguous 4D slice, is implementation freedom — the observable result is identical.

### Kernel-component index constants

- **NES:** `iHi0 = 1`, `iHii0 = 2`, `iHi1 = 3`, `iHii1 = 4` — the in-pair (`Hi`) and cross-pair (`Hii`) components of the order-0 (`*0`) and order-1 (`*1`) Legendre moments.
- **Pair:** `iJi0 = 1`, `iJii0 = 2`, `iJi1 = 3`, `iJii1 = 4` — analogously the `Ji` / `Jii` components of the order-0 / order-1 moments.

These integers index the `kernel` axis directly; they are never interpolated and never log-transformed.

### Outputs

- The recovered physical kernel value `10**(BiLinear(...)) - OS` at the requested `(iE', iE, kernel, T, η)`, in that channel's stored units (`/Scat_{NES,Pair}_Kernels/Units`).
- For the channel as a whole, the kernel array over the energy plane: the **lower triangle `iE' ≤ iE`** is interpolated directly; the **upper triangle `iE' > iE`** is filled by the channel's symmetry (detailed balance for NES, crossing for Pair) — see Correctness requirements.

### Reference tables (anchor fixtures and Layer-1 checks)

- **NES:** `wl-Op-SFHo-15-25-50-E40-NES.h5`, pinned by path + `sha256` in `specs/fixtures/tables.provenance`; structure committed at `specs/fixtures/wl-Op-SFHo-15-25-50-E40-NES.h5ls`. The kernels live under `/Scat_NES_Kernels/Kernels` (`h5ls` `{120, 81, 4, 40, 40}` = Fortran `(40, 40, 4, 81, 120) = (nE', nE, nMom, nT, nEta)`), the **2D** offsets under `/Scat_NES_Kernels/Offsets` `{4, 1}` = Fortran `(1, 4) = (nOpacities, nMoments)`, the energy axis under `/EnergyGrid/Values` `[40]`, the η axis under `/EtaGrid/Values` `[120]`, and temperature under `/ThermoState/Temperature` `[81]`.
- **Pair:** `wl-Op-SFHo-15-25-50-E40-Pair.h5`, pinned the same way; structure at `specs/fixtures/wl-Op-SFHo-15-25-50-E40-Pair.h5ls`. The kernels live under `/Scat_Pair_Kernels/Kernels` (same `{120, 81, 4, 40, 40}` shape), the **2D** offsets under `/Scat_Pair_Kernels/Offsets` `{4, 1}`, with the same `/EnergyGrid`, `/EtaGrid`, and `/ThermoState/Temperature` axes as the NES file.

Full on-disk contract (group/dataset names, dtypes, legacy fallbacks, the column-major→C-shape reversal): `table-format-and-io.md`.

## Correctness requirements

### The aligned interpolation formula (both channels)

At fixed energy indices `(iE', iE)` and fixed `kernel`, with the two thermodynamic query coordinates `(LogT, LogX)` (where `LogX = LOG10(η)`) located on their `LOG10`'d grids (brackets/deltas as in `fortran-parity-and-tolerances.md`, linear formula on each coordinate):

- `iT` from the linear bracket on `LogTs`, `dT = (LogT − LogTs(iT)) / (LogTs(iT+1) − LogTs(iT))`.
- `iX` from the linear bracket on `LogXs`, `dX = (LogX − LogXs(iX)) / (LogXs(iX+1) − LogXs(iX))`.

Read the 4 corner log-values at the fixed energy indices:

```
p00 = Table(iE', iE, iT  , iX  )
p10 = Table(iE', iE, iT+1, iX  )
p01 = Table(iE', iE, iT  , iX+1)
p11 = Table(iE', iE, iT+1, iX+1)
```

form the bilinear interpolant in log space, then recover:

```
Interpolant = 10**( BiLinear(p00, p10, p01, p11, dT, dX) ) - OS
```

where `BiLinear(p00, p10, p01, p11, d1, d2) = (1−d2)·((1−d1)·p00 + d1·p10) + d2·((1−d1)·p01 + d1·p11)` (linear in `dT`, then `dX`). The result must match `LogInterpolateSingleVariable_2D2D_Custom_Aligned_Point` on identical inputs at the **default parity tier** (`rtol 1e-12`, `atol 1e-30`).

Because both `T` and `η` are interpolated **linearly in their `LOG10`'d coordinate** (the caller pre-`LOG10`'d them), the interpolation is bilinear in `(log10 T, log10 η)`. The energy indices `(iE', iE)` and the `kernel` index select the corner stack; they are not interpolated.

### Which triangle is computed vs. filled

The aligned routine computes only the **lower energy triangle** `iE' ≤ iE` directly. The **upper triangle** `iE' > iE` is **not** independently interpolated; it is filled from the lower triangle by the channel's physics symmetry below. A correct implementation must (a) interpolate the lower triangle, and (b) fill the upper triangle by the stated symmetry — not by interpolating the upper-triangle table entries (which may be unpopulated / undefined).

### NES detailed-balance invariant (Layer-1 closure check)

For the assembled NES kernel `Phi` at a fixed thermodynamic point with temperature `T` (in MeV) and neutrino energies `E(iE)`, `E(iEp)`:

```
Phi(iEp, iE) = Phi(iE, iEp) · exp( ( E(iE) − E(iEp) ) / T )      for iEp > iE
```

i.e. the upper energy triangle is the lower triangle scaled by the Boltzmann factor `exp((E − E')/T)`. This is the exact relation the consumer applies (`wlOpacityInterpolationModule.f90:104-115`). A correct NES implementation must reproduce it: the upper-triangle value equals the symmetric lower-triangle value times `exp((E(iE) − E(iEp)) / T)`. Verified to the **default parity tier** (`rtol 1e-12`, `atol 1e-30`) where the lower-triangle value is exact, since the only added operations are one `exp` and one divide.

### Pair crossing-symmetry invariant (Bruenn 1985, Eq. C64) (Layer-1 closure check)

For the assembled Pair kernel, the upper energy triangle is filled by a **crossing symmetry** that swaps **both** the energy indices `(iE, iEp)` **and** the in-pair / cross-pair kernel components (`Ji ↔ Jii`). Concretely, for a species with per-species coupling weights `C_i`, `C_ii`, the upper-triangle (`iEp > iE`) entries are assembled from the *transposed* energy entry with the kernel components exchanged:

```
Phi0(iEp, iE) = C_i · Kernel(iE, iEp, iJii0) + C_ii · Kernel(iE, iEp, iJi0)
Phi1(iEp, iE) = C_i · Kernel(iE, iEp, iJii1) + C_ii · Kernel(iE, iEp, iJi1)
```

— contrast the lower triangle, where the same components are *not* swapped:

```
Phi0(iEp, iE) = C_i · Kernel(iEp, iE, iJi0) + C_ii · Kernel(iEp, iE, iJii0)      for iEp ≤ iE
Phi1(iEp, iE) = C_i · Kernel(iEp, iE, iJi1) + C_ii · Kernel(iEp, iE, iJii1)
```

This matches `wlOpacityInterpolationModule.f90:196-228`. The defining, machinery-independent statement of the symmetry is: **the Pair kernel is invariant under simultaneously transposing the two energy indices and exchanging the `i`/`ii` (in-pair/cross-pair) kernel components** — `Kernel_i(E', E) = Kernel_ii(E, E')`. A correct implementation must reproduce this index-and-component swap exactly (an exact relabeling — no Boltzmann factor), verified to the default parity tier on the lower-triangle interpolated values.

### Boundary / out-of-range / NaN (bit-for-bit with weaklib)

Replicate the permissive behavior exactly (see `fortran-parity-and-tolerances.md`):

- Out-of-range `(T, η)`: clamp each bracket index to the edge cell `[1, n-1]`, do **not** clamp the delta. A below-range query gives `d < 0`, above-range `d > 1`, producing **linear extrapolation from the edge cell** in the `LOG10` coordinate — not an error, not a result clamp, not a NaN. No range check; range enforcement is a consumer responsibility.
- Non-positive `T` or `η`: the caller forms `LOG10` of a non-positive argument, yielding NaN that propagates silently to `Interpolant`. The C++ result must be NaN in the same circumstances.
- The energy indices `(iE', iE)` and the `kernel` index are assumed valid (`1..nE'`, `1..nE`, `1..nMom`); range-checking them is a consumer responsibility (out-of-range integer indices are undefined, mirroring the Fortran which indexes without a guard).

### Conventions restated (the subset these channels use)

- `T` and `η` interpolated in log space (caller passes `LOG10`); the energy axes `(E', E)` and the `kernel` index are not interpolated.
- `T` is in **MeV** for these channels (not Kelvin).
- No ρ, no Yₑ; the two thermodynamic axes are `(T, η)`.
- Universal recovery `value = 10**(stored) - offset`.
- Offset dimensionality: **2D `Offsets[nOpacities, nMoments]`** for both NES and Pair.
- Fortran column-major tables: first index (E') fastest-varying. 5D flat offset `iEp + nE'*(iE + nE*(k + nMom*(iT + nT*iEta)))`.
- `double` (`dp = 8`) throughout.

## Verification

### Layer 1 — self-contained checks (the active gate)

Run against both synthetic in-suite tables and the real reference tables `wl-Op-SFHo-15-25-50-E40-NES.h5` / `…-Pair.h5`:

1. **Affine-in-log exactness (machine-precision tier `~1e-14`).** Build a synthetic 5D `(E', E, kernel, T, η)` table whose stored value, at fixed `(iE', iE, kernel)`, is an exact affine function of `(log10 T, log10 η)` — with a distinct affine offset per `(iE', iE, kernel)` triple so a wrong energy/kernel index is caught. The aligned bilinear-in-log interpolation at fixed `(iE', iE, kernel)` must reproduce `10**(affine) - OS` exactly at *any* interior `(T, η)`, not just at nodes. The constant table is the degenerate case.
2. **Node identity (machine-precision tier).** Querying exactly at a `(T, η)` grid node (with `LogT`/`LogX` equal to a `LOG10`'d node) at fixed `(iE', iE, kernel)` returns that node's recovered value `10**(Table(iE', iE, kernel, iT, iX)) - OS`. Verify on the real tables too.
3. **Kernel-slice independence (machine-precision tier).** On a synthetic table with a different known affine function per `kernel` slice, interpolating at each `kernel` must recover that slice's function and be unaffected by the others — confirming the `kernel` index (and the energy indices) are pure slices with the matching 2D offset element `Offsets(opacity, kernel)`.
4. **NES detailed balance (default parity tier `1e-12`/`1e-30`).** Build a synthetic NES kernel whose lower triangle `iEp ≤ iE` is known, fill the upper triangle by `Phi(iEp, iE) = Phi(iE, iEp) · exp((E(iE) − E(iEp)) / T)`, and assert the implementation's upper-triangle output equals the lower-triangle value times the Boltzmann factor `exp((E − E')/T)`. Use a synthetic energy grid `E` and temperature `T` (MeV) so the factor is exactly computable; the diagonal `iEp = iE` is the fixed point (factor `= 1`).
5. **Pair crossing symmetry (default parity tier).** Build a synthetic Pair kernel and assert the implementation's upper-triangle output (`iEp > iE`) equals the transposed-energy entry with the `i`/`ii` kernel components exchanged: `Phi0(iEp, iE) = C_i·Kernel(iE, iEp, iJii0) + C_ii·Kernel(iE, iEp, iJi0)` (and the order-1 analogue), distinct from the un-swapped lower-triangle assembly. This is an exact relabeling — no tolerance beyond default parity is needed.
6. **Boundary extrapolation (no tolerance / exact relation).** A query just outside an edge of `T` or `η` must equal the edge cell's linear extrapolation (the same bilinear formula evaluated with the unclamped delta) — confirming clamp-index-but-not-delta.
7. **NaN propagation (NaN-equality).** A query with non-positive `T` or `η` must produce a NaN `Interpolant`.

### Layer 2 — Fortran parity (specified, PENDING)

Compare `Interpolant` (default tier `1e-12`/`1e-30`) against committed golden fixtures generated offline by `LogInterpolateSingleVariable_2D2D_Custom_Aligned_Point` at the pinned commit — over `(iE', iE, kernel, T, η)` query points drawn from `wl-Op-SFHo-15-25-50-E40-NES.h5` and `wl-Op-SFHo-15-25-50-E40-Pair.h5`. Until fixtures exist these tests report **pending**, not passing (see `fortran-parity-and-tolerances.md`).

### Mechanical (validator)

`bash specs/tools/validate_specs.sh` (default mode) asserts: the 7 mandated sections in order; the `LogInterpolateSingleVariable_2D2D_Custom_Aligned` routine name present; the detailed-balance expression `exp((E − E') / T)` and a crossing-symmetry statement present; the cited weaklib source-of-truth paths resolve; and the documented `/Scat_NES_Kernels/Kernels` and `/Scat_Pair_Kernels/Kernels` structure is confirmed against the committed snapshots `wl-Op-SFHo-15-25-50-E40-NES.h5ls` / `…-Pair.h5ls`, with each table cited by name in this spec.

## Implementation freedom

- Whether the aligned interpolation indexes the 5D buffer with the energy/`kernel` indices fixed in the arithmetic, or first gathers a contiguous 4D `(E', E, T, η)` slice (results must be identical).
- The internal bilinear evaluation order and whether the 4 corners are read into locals or indexed in place.
- Whether the symmetry fill (detailed balance / crossing) is done in the interpolation kernel, in the consumer's kernel-assembly step, or fused — provided the observable upper-triangle values match the stated invariant.
- Whether the lower-triangle iteration is the collapsed triangular loop the Fortran uses or a plain doubly-nested `i ≤ j` loop.
- Whether the caller pre-`LOG10`'s `T`/`η` or the entry point does it internally, provided the observable result — including NaN-on-non-positive — matches the `_Point` contract.
- The exact `_Point` signature argument grouping (subject to the scalar/allocation-free/`double const*`-plus-extents contract in `amrex-device-interface.md`).
- Whether the multi-point array form is a hand-written loop or a `ParallelFor` over the `_Point` core.
- How the per-species coupling combination (`C_i`, `C_ii` from `cv`, `ca`) is applied — these are consumer assembly data, not interpolation contract; only the symmetry relation between triangles is required.

## Open questions / assumptions

- **`LogInterpolateOpacity_2D1D2D` provenance gap (research OQ#1 — assumption, explicitly non-blocking).** weaklib's standalone NES/Pair wrappers `wlInterpolateOpacity_NES` / `_Pair` (`wlOpacityInterpolationModule.f90:51-52, 149-150`) interpolate the full `(E', E)` energy grid via a routine `LogInterpolateOpacity_2D1D2D` that is **imported but not present** in the read `wlInterpolationModule.F90` source (a different revision, a generated file, or a build-time include). Its exact signature and per-axis log/linear convention are therefore unresolved. **This is off the reference consumer's critical path:** `wlOpacityInterpolationModule.f90` is never `USE`d by thornado, whose production NES/Pair path drives the **aligned** routine `LogInterpolateSingleVariable_2D2D_Custom_Aligned(_Point)` on pre-aligned `NES_AT` / `Pair_AT` tables (research §7). This spec therefore pins the **aligned** path as the source-of-truth interpolation contract, and the unresolved `2D1D2D` routine is recorded here purely as a provenance note for weaklib's standalone full-energy-grid path, which the C++ port does not need to reproduce. The Ralph loop is never blocked on this routine name: NES/Pair correctness is anchored to the aligned bilinear interpolation plus the detailed-balance / crossing-symmetry invariants, all of which are fully specified above.
- **Layer-2 golden fixtures are future work (assumption, non-blocking).** No Fortran-generated golden NES/Pair interpolation outputs exist or can be generated in this environment; Layer-2 tests ship **pending**, the named aligned `_Point` routine remains the generator-of-record, and the Ralph loop gates on Layer 1. See `fortran-parity-and-tolerances.md`.
- **Concrete offsets, energy/η/T grid extents, and coupling constants (assumption, non-blocking).** The 2D `Offsets[nOpacities, nMoments]` values, the energy / η / temperature node coordinates, and the weak-coupling constants `cv`, `ca` (and thus `C_i = (cv ± ca)²`, `C_ii = (cv ∓ ca)²`) live only in the production `.h5` files and the consumer's physics assembly (research OQ#3). This spec pins the recovery contract, offset dimensionality, and the symmetry invariants; the fixture/table supplies the numbers. Layer-1 exactness and symmetry checks use synthetic tables whose offsets and coupling weights the suite chooses, so they do not depend on the unknown production values.
- **Temperature units (assumption).** This spec assumes `T` is in MeV for the NES/Pair channels (per `wlOpacityInterpolationModule.f90:30` and the η/Boltzmann-factor physics), in contrast to the Kelvin convention of the EOS / EmAb / Iso channels. The on-disk `/ThermoState/Units` dataset is authoritative; the consumer must pass `T` in the same units the `/ThermoState/Temperature` grid was tabulated in, and `η`, `(E − E')/T` must be consistent (energies and `T` in the same energy unit so the Boltzmann factor is dimensionless).
- **`nOpacities` / `nMoments` are read from the table (assumption, non-blocking).** In the pinned NES/Pair tables `nMoments = 4` (the four kernel components `iHi0..iHii1` / `iJi0..iJii1`) and `nOpacities = 1` (the `Offsets {4,1}` = Fortran `(1, 4)` snapshot). The interpolation contract is per-`(opacity, kernel)` (one `OS` + one log-stored 4D `(E', E, T, η)` sub-table), with the counts read per `table-format-and-io.md`.
