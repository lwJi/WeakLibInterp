# Table format and HDF5 I/O (the on-disk reader contract)

> Leaf spec. Self-contained: an agent can implement the HDF5 reader for the EOS and opacity tables from this file alone. It restates only the conventions it uses and references `fortran-parity-and-tolerances.md` (numeric contract: `double`, log-space+offset, column-major) and `amrex-device-interface.md` (how the read arrays become device-resident `Gpu::DeviceVector<double>`) for the shared cross-cutting details. `README.md` is canonical if any restated convention here conflicts with it. The concrete group/dataset names and shapes documented here are anchored to the committed structural snapshots `specs/fixtures/*.h5ls`, derived once from the pinned production tables.

## Purpose & scope

This spec defines the on-disk HDF5 format of the weaklib production tables and the contract a C++ reader must satisfy to load them into host memory ready for upload to the device. It pins the group/dataset names, the logical (Fortran) shapes and their `h5ls` C-order reversal, the dtypes, the per-channel additive-offset datasets and their dimensionality, the read-order dependence (read `Names`/grid-name datasets before opening the named arrays), and the legacy fallbacks a robust reader handles.

In scope:
- The EOS file layout: groups `/ThermoState`, `/DependentVariables`, `/Metadata`; the named axis and dependent-variable datasets; the 1D `/DependentVariables/Offsets`.
- The opacity file layouts (one file per channel): `/EnergyGrid`, `/ThermoState`, optional `/EtaGrid`, and the channel group `/EmAb`, `/Scat_Iso_Kernels`, `/Scat_NES_Kernels`, `/Scat_Pair_Kernels`, or `/Scat_Brem_Kernels`.
- Exact dataset names, the logical Fortran shapes (first index fastest-varying) and their C-order reversal as `h5ls` reports them, and dtypes (`H5T_NATIVE_DOUBLE`, integers, fixed-width strings; scientific metadata stored as datasets, not attributes).
- Per-channel offset dimensionality: 1D `Offsets[nOpacities]` for EOS dependent variables and EmAb; 2D `Offsets[nOpacities, nMoments]` for every scattering kernel (Iso, NES, Pair, Brem).
- The read-order dependence and the legacy fallbacks (`EmAb_CorrectedAbsorption` group name; the geometric-grid `Zoom`/`Edge`/`Width` datasets present only on a zoomed energy grid; absent `EmAb Parameters` / `EC_table` groups).

Out of scope:
- The interpolation math, derivatives, tolerances, and boundary/NaN policy — see `fortran-parity-and-tolerances.md` and the per-channel `eos-*` / `opacity-*` specs.
- How the read host arrays are uploaded to the device and indexed in kernels — see `amrex-device-interface.md` (this spec stops at "the values are in host memory in a known layout").
- The semantics of the moment/kernel/species indices and the physics symmetries — see the `opacity-*` specs.
- Writing tables; this is a read-only reader contract (the production tables are inputs).

## Source of truth

Pinned weaklib commit: see `weaklib_commit` in `specs/fixtures/tables.provenance`. The committed structural snapshots `specs/fixtures/*.h5ls` (raw `h5ls -r` output of each pinned production table) are the in-repo, CI-reproducible ground truth this spec is diffed against.

- `weaklib/Distributions/EOSSource/wlEOSIOModuleHDF.f90` — the EOS reader/writer. `ReadEquationOfStateTableHDF` (subroutine at `wlEOSIOModuleHDF.f90:183-213`) opens `DependentVariables`, reads `Dimensions` + `nVariables`, allocates, then calls `ReadThermoStateHDF` and `ReadDependentVariablesHDF`. The per-variable offset recovery `physical = 10**(stored) - Offsets(j)` is applied at `wlEOSIOModuleHDF.f90:490-494`.
- `weaklib/Distributions/Library/wlIOModuleHDF.F90` — the shared `WriteThermoStateHDF` / `ReadThermoStateHDF`, `WriteDependentVariablesHDF` / `ReadDependentVariablesHDF`, and the primitive `WriteHDF`/`ReadHDF` that fix the dtypes and the dataset-per-name layout.
- `weaklib/Distributions/OpacitySource/wlOpacityTableIOModuleHDF.f90` — the opacity reader/writer. `WriteGridHDF`/`ReadGridHDF` (`wlOpacityTableIOModuleHDF.f90:424-475`, `:1882-1971`) own the `/EnergyGrid` and `/EtaGrid` group layout including the geometric-grid `Zoom`/`Edge`/`Width` fields; `WriteOpacityTableHDF_EmAb` / `_parameters` / `_EC_table` / `_Scat` (`:477-844`) and the readers (`:1010-1641`) own the channel-group layouts, the 1D-vs-2D `Offsets` dimensionality, and the `EmAb` → `EmAb_CorrectedAbsorption` legacy group-name fallback (`:1157-1164`, `:1357`).

This spec mirrors the *on-disk* contract these writers/readers establish; it is the reader half of the weaklib I/O surface, reimplemented in C++ against the same byte layout.

## Inputs & outputs

Value type for every real dataset is `double` (IEEE-754 binary64; weaklib `dp = 8`, on-disk `H5T_NATIVE_DOUBLE`); see `fortran-parity-and-tolerances.md`. Integers are `H5T_NATIVE_INTEGER` (32-bit), strings are fixed-width `H5T_NATIVE_CHARACTER` (`LEN = 32` for names/units, `LEN = 120` for metadata provenance strings). Scientific metadata (`LogInterp`, `Offsets`, `Dimensions`, `nPoints`, the `i*` index slots) is stored as ordinary **datasets**, never as HDF5 attributes; the only HDF5 *attributes* in these files are human-readable provenance strings no reader reads back.

### Shape and the column-major ↔ C-order reversal (the load-bearing rule)

Multidimensional datasets are written by the Fortran HDF5 bindings in **column-major** order: the Fortran **first index is fastest-varying** (most contiguous in the byte stream). Consequently:

- A Fortran logical shape `(n0, n1, ..., n_{D-1})` is reported by `h5ls -r` in the **reverse** order `{n_{D-1}, ..., n1, n0}` (C / slowest-varying-first convention).
- A reader that wants to address an element by its Fortran indices `(i0, i1, ..., i_{D-1})` (0-based) computes the flat offset
  `offset = i0 + n0*( i1 + n1*( i2 + ... + n_{D-2}*( i_{D-1} ) ... ) )`,
  reading the raw bytes contiguously and *not* permuting them. Equivalently, to index naturally in C as `data[i_{D-1}]...[i0]` the C shape is the reverse of the Fortran shape.

This is the same convention `fortran-parity-and-tolerances.md` states globally and that `amrex-device-interface.md` extends to in-kernel indexing of the flat `double const*` table. **Every shape written below is given in both forms**: the `h5ls` C-order shape (as it appears in the committed `*.h5ls` snapshots) and the Fortran logical `(…)` shape.

### EOS file (`wl-EOS-SFHo-15-25-50.h5`)

Pinned by path + `sha256` in `specs/fixtures/tables.provenance`; structure committed at `specs/fixtures/wl-EOS-SFHo-15-25-50.h5ls`. Three top-level groups:

**`/ThermoState`** — the three independent EOS axes.

| Dataset | dtype | `h5ls` shape | Fortran shape | Meaning |
|---|---|---|---|---|
| `/ThermoState/Names` | string[32] | `{3}` | `(3)` | `[Density, Temperature, Electron Fraction]` |
| `/ThermoState/Units` | string[32] | `{3}` | `(3)` | `[Grams per cm^3, K, (none)]` |
| `/ThermoState/Dimensions` | int | `{3}` | `(3)` | `[nρ, nT, nYe]` |
| `/ThermoState/LogInterp` | int | `{3}` | `(3)` | `[1, 1, 0]` (ρ,T log; Yₑ linear) |
| `/ThermoState/Density` | double | `{185}` | `(nρ)` | ρ node coordinates, g/cm³ |
| `/ThermoState/Temperature` | double | `{81}` | `(nT)` | T node coordinates, K |
| `/ThermoState/Electron Fraction` | double | `{30}` | `(nYe)` | Yₑ node coordinates |
| `/ThermoState/iRho`, `/iT`, `/iYe` | int | `{1}` | `(1)` | physical-role → slot index |
| `/ThermoState/minValues`, `/maxValues` | double | `{3}` | `(3)` | per-axis extents |

The per-axis arrays are stored under their `Names` values (`Density`, `Temperature`, `Electron Fraction`), so a reader must read `/ThermoState/Names` **before** opening them. `nρ = 185`, `nT = 81`, `nYe = 30` in this table.

**`/DependentVariables`** — the EOS outputs.

| Dataset | dtype | `h5ls` shape | Fortran shape | Meaning |
|---|---|---|---|---|
| `/DependentVariables/Names` | string[32] | `{15}` | `(nV)` | per-variable names |
| `/DependentVariables/Units` | string[32] | `{15}` | `(nV)` | per-variable units |
| `/DependentVariables/Dimensions` | int | `{3}` | `(3)` | `[nρ, nT, nYe]` |
| `/DependentVariables/nVariables` | int | `{1}` | `(1)` | `nV` (= 15 here) |
| `/DependentVariables/Offsets` | double | `{15}` | `(nV)` | **1D** per-variable additive offset |
| `/DependentVariables/Repaired` | int | `{30, 81, 185}` | `(nρ, nT, nYe)` | repair flags |
| `/DependentVariables/<Name>` | double | `{30, 81, 185}` | `(nρ, nT, nYe)` | log-stored sub-table per variable |
| `/DependentVariables/i<Name>` × 15 | int | `{1}` | `(1)` | role → slot (`iPressure`, `iEntropyPerBaryon`, …, `iGamma1`) |
| `/DependentVariables/minValues`, `/maxValues` | double | `{15}` | `(nV)` | per-variable extents |

Each dependent-variable sub-table is stored under its `Names` value (e.g. `/DependentVariables/Pressure`, `/DependentVariables/Entropy Per Baryon`, `/DependentVariables/Internal Energy Density`, `/DependentVariables/Electron Chemical Potential`), so a reader must read `/DependentVariables/Names` **before** opening the named arrays. Each sub-table's `h5ls` shape `{30, 81, 185}` is the Fortran `(185, 81, 30) = (nρ, nT, nYe)` with ρ fastest-varying. The physical value is recovered per variable `j` as `physical = 10**(stored) - Offsets(j)`.

**`/Metadata`** — seven `string[120]`, each `{1}`: `/Metadata/IDTag`, `/TableResolution`, `/NucEOSLink`, `/LeptonEOSLink`, `/SourceLink`, `/WLRevision`, `/TableLink`. Human-readable provenance only; not consumed by the interpolation path.

### Opacity files (one per channel)

Each opacity channel ships as its own `.h5` file that carries a shared `/EnergyGrid`, a shared `/ThermoState` (identical layout to the EOS file's `/ThermoState`), the NES/Pair files additionally carry `/EtaGrid`, and exactly one channel group. The pinned production tables, one per channel, are `wl-Op-SFHo-15-25-50-E40-EmAb.h5`, `wl-Op-SFHo-15-25-50-E40-Iso.h5`, `wl-Op-SFHo-15-25-50-E40-NES.h5`, `wl-Op-SFHo-15-25-50-E40-Pair.h5`, and `wl-Op-SFHo-15-25-50-E40-Brem.h5`. All pinned by path + `sha256` in `specs/fixtures/tables.provenance`; structures committed at the matching `specs/fixtures/wl-Op-SFHo-15-25-50-E40-{EmAb,Iso,NES,Pair,Brem}.h5ls`.

**`/EnergyGrid`** (and, identically structured, `/EtaGrid` on NES/Pair) — a single-axis grid descriptor written by `WriteGridHDF`:

| Dataset | dtype | `h5ls` shape | Meaning |
|---|---|---|---|
| `/EnergyGrid/Name`, `/Unit` | string[32] | `{1}` | axis name/unit |
| `/EnergyGrid/nPoints` | int | `{1}` | number of energy points (40 here) |
| `/EnergyGrid/LogInterp` | int | `{1}` | 1 = log-spaced |
| `/EnergyGrid/Values` | double | `{40}` | energy node coordinates |
| `/EnergyGrid/Edge` | double | `{nPoints+1}` | **geometric-grid only** (present iff `Zoom > 0`) |
| `/EnergyGrid/Width` | double | `{nPoints}` | **geometric-grid only** |
| `/EnergyGrid/Zoom` | double | `{1}` | **geometric-grid only** |
| `/EnergyGrid/min*`, `/max*` | double | `{1}` | extents |

`/EtaGrid/Values` is `{120}` in the NES/Pair tables (η = μₑ/kT degeneracy grid). The geometric-grid extras (`Zoom`, `Edge[41]`, `Width[40]`, `minEdge`, `maxEdge`, `minWidth`) are present in the committed Iso snapshot's `/EnergyGrid` but **absent** when the grid was built non-geometrically; a reader must treat them as optional and key their presence off `Zoom` (read `Zoom` first; if absent or `≤ 0`, do not require `Edge`/`Width`).

**`/EmAb`** (EmAb file) — emission/absorption opacity, 4D `(E, ρ, T, Yₑ)`:

| Dataset | dtype | `h5ls` shape | Fortran shape | Meaning |
|---|---|---|---|---|
| `/EmAb/nOpacities` | int | `{1}` | `(1)` | number of neutrino species (2: νₑ, ν̄ₑ) |
| `/EmAb/Units` | string[32] | `{2}` | `(nOp)` | per-species units |
| `/EmAb/Offsets` | double | `{2}` | `(nOp)` | **1D** per-species additive offset |
| `/EmAb/Electron Neutrino` | double | `{30, 81, 185, 40}` | `(nE, nρ, nT, nYe)` | log-stored νₑ opacity |
| `/EmAb/Electron Antineutrino` | double | `{30, 81, 185, 40}` | `(nE, nρ, nT, nYe)` | log-stored ν̄ₑ opacity |

The per-species opacity arrays are stored under their `Names` values (`Electron Neutrino`, `Electron Antineutrino`); read the names before opening. The EmAb file also carries an `/EmAb Parameters` group (seven integer physics flags) and, when `nuclei_EC_table > 0`, an `/EC_table` group (an electron-capture sub-table with its own grids and `spec_Offsets`/`rate_Offsets`). Both are present in the committed EmAb snapshot but a robust reader treats them as optional. **Legacy fallback:** older tables wrote the channel under the group name `/EmAb_CorrectedAbsorption` instead of `/EmAb`; a reader must open `/EmAb` if present and fall back to `/EmAb_CorrectedAbsorption` otherwise.

**`/Scat_Iso_Kernels`** (Iso file) — isoenergetic scattering, 5D `(E, moment, ρ, T, Yₑ)`:

| Dataset | dtype | `h5ls` shape | Fortran shape | Meaning |
|---|---|---|---|---|
| `/Scat_Iso_Kernels/nOpacities` | int | `{1}` | `(1)` | species count (2) |
| `/Scat_Iso_Kernels/nMoments` | int | `{1}` | `(1)` | moment count (2) |
| `/Scat_Iso_Kernels/Units` | string[32] | `{2}` | `(nOp)` | per-species units |
| `/Scat_Iso_Kernels/Offsets` | double | `{2, 2}` | `(nOp, nMom)` | **2D** per-(species,moment) additive offset |
| `/Scat_Iso_Kernels/Electron Neutrino` | double | `{30, 81, 185, 2, 40}` | `(nE, nMom, nρ, nT, nYe)` | log-stored νₑ kernel |
| `/Scat_Iso_Kernels/Electron Antineutrino` | double | `{30, 81, 185, 2, 40}` | `(nE, nMom, nρ, nT, nYe)` | log-stored ν̄ₑ kernel |

Correction flags `weak_magnetism_corr`, `ion_ion_corr`, `many_body_corr` (int) and `ga_strange` (double) accompany the group.

**`/Scat_NES_Kernels`** (NES file) and **`/Scat_Pair_Kernels`** (Pair file) — 5D `(E', E, kernel-index, T, η)`:

| Dataset | dtype | `h5ls` shape | Fortran shape | Meaning |
|---|---|---|---|---|
| `/Scat_{NES,Pair}_Kernels/nOpacities` | int | `{1}` | `(1)` | (1) |
| `/Scat_{NES,Pair}_Kernels/nMoments` | int | `{1}` | `(1)` | kernel-component count (4) |
| `/Scat_{NES,Pair}_Kernels/Units` | string[32] | `{1}` | `(nOp)` | units |
| `/Scat_{NES,Pair}_Kernels/Offsets` | double | `{4, 1}` | `(nOp, nMom)` | **2D** additive offset |
| `/Scat_{NES,Pair}_Kernels/Kernels` | double | `{120, 81, 4, 40, 40}` | `(nE', nE, nMom, nT, nEta)` | log-stored kernel |

(NES additionally carries an integer `NPS` flag.) Note the `h5ls` shape `{120, 81, 4, 40, 40}` reverses to Fortran `(40, 40, 4, 81, 120) = (nE', nE, nMom, nT, nEta)`: the two energy axes are 40 each, the moment/kernel index is 4, T is 81, and η is 120 (matching `/EtaGrid/Values {120}`).

**`/Scat_Brem_Kernels`** (Brem file) — 5D `(E', E, moment, ρ, T)`:

| Dataset | dtype | `h5ls` shape | Fortran shape | Meaning |
|---|---|---|---|---|
| `/Scat_Brem_Kernels/nOpacities` | int | `{1}` | `(1)` | (1) |
| `/Scat_Brem_Kernels/nMoments` | int | `{1}` | `(1)` | (1) |
| `/Scat_Brem_Kernels/Units` | string[32] | `{1}` | `(nOp)` | units |
| `/Scat_Brem_Kernels/Offsets` | double | `{1, 1}` | `(nOp, nMom)` | **2D** additive offset |
| `/Scat_Brem_Kernels/S_sigma` | double | `{81, 185, 1, 40, 40}` | `(nE', nE, nMom, nρ, nT)` | log-stored structure function |

The Brem dataset is named `S_sigma` (not `Kernels`), and its `h5ls` shape `{81, 185, 1, 40, 40}` reverses to Fortran `(40, 40, 1, 185, 81) = (nE', nE, nMom, nρ, nT)` — its trailing two axes are `(ρ, T)`, **not** `(T, η)`; Brem has no η/Yₑ dependence and no `/EtaGrid`.

### Outputs (what a reader produces)

For each table the reader produces, in host memory: the axis coordinate arrays (`double[]`), the per-quantity additive offset(s) (1D or 2D as documented above), and the log-stored value array(s) as a flat `double[]` in the on-disk (Fortran column-major) byte order plus their integer extents. These are exactly the inputs the per-channel interpolation specs consume and `amrex-device-interface.md` uploads to the device.

## Correctness requirements

1. **Names-before-arrays read order.** A reader must read the name datasets (`/ThermoState/Names`, `/DependentVariables/Names`, the grid `Name` datasets) before opening the value arrays, because the value arrays are stored under their name strings. Reading the EOS file follows `ReadEquationOfStateTableHDF`'s order: open `/DependentVariables`, read `Dimensions` + `nVariables`, allocate, then read `/ThermoState`, then `/DependentVariables`.
2. **Shapes match the snapshot exactly, with the column-major reversal applied.** Every documented dataset name and `h5ls` shape must match the committed `specs/fixtures/*.h5ls` snapshot for that table. A reader that addresses elements by Fortran indices must use the flat-offset formula above (first index fastest-varying); it must not transpose the raw bytes.
3. **Dtypes are fixed.** Reals are `H5T_NATIVE_DOUBLE`, integers `H5T_NATIVE_INTEGER`, names/units fixed-width `string[32]`, metadata `string[120]`. Scientific metadata is read from *datasets*, never attributes.
4. **Offset dimensionality is per-channel and load-bearing.** `/DependentVariables/Offsets` and `/EmAb/Offsets` are **1D** `[nOpacities]`; every scattering `Offsets` (`/Scat_Iso_Kernels`, `/Scat_NES_Kernels`, `/Scat_Pair_Kernels`, `/Scat_Brem_Kernels`) is **2D** `[nOpacities, nMoments]`. The recovery is `physical = 10**(stored) - offset` using the 1D `offset[op]` for EOS/EmAb and the 2D `offset[op][mom]` for scattering. A reader that reads a scattering `Offsets` as 1D, or an EmAb/EOS `Offsets` as 2D, is incorrect.
5. **Legacy fallbacks are handled, not assumed-away.**
   - EmAb channel group: open `/EmAb`; if absent, open `/EmAb_CorrectedAbsorption`.
   - Geometric-grid extras: `/EnergyGrid/Zoom`, `/Edge`, `/Width` (and `minEdge`/`maxEdge`/`minWidth`) exist only on a zoomed grid; key their presence off `Zoom` and do not require them otherwise.
   - Optional groups: `/EmAb Parameters` and `/EC_table` may be absent (legacy tables); their absence maps the corresponding flags to a sentinel (weaklib uses `-1`) and must not fail the read.
6. **`double` throughout, parity-pinned.** Real values are read as `double` (IEEE-754 binary64) and remain `double` regardless of `amrex::Real` (see `amrex-device-interface.md`); a single-precision read would silently break the downstream bit-level parity tiers (default `rtol 1e-12` / `atol 1e-30`; see `fortran-parity-and-tolerances.md`) and is incorrect — the reader contributes no interpolation tolerance of its own but must preserve full `double` precision so those tiers remain achievable.

## Verification

### Layer 1 — self-contained checks (the active gate)

Run against the committed snapshots and (where present) the real production tables:

1. **Structural conformance against the committed snapshots (always runnable, CI-reproducible).** For each of the six tables, assert that the documented group/dataset names and `h5ls` shapes in this spec are exactly those in the committed `specs/fixtures/*.h5ls` snapshot — in particular: the EOS `/ThermoState` and `/DependentVariables` groups with `/DependentVariables/Offsets {15}` (1D); the EmAb `/EmAb/Offsets {2}` (1D) and `/EmAb/Electron Neutrino {30,81,185,40}`; the Iso `/Scat_Iso_Kernels/Offsets {2,2}` (2D) and `…/Electron Neutrino {30,81,185,2,40}`; the NES/Pair `/Scat_{NES,Pair}_Kernels/Offsets {4,1}` (2D) and `…/Kernels {120,81,4,40,40}`; the Brem `/Scat_Brem_Kernels/Offsets {1,1}` (2D) and `…/S_sigma {81,185,1,40,40}`.
2. **Round-trip layout (when a real table is present).** Read a known dataset (e.g. EOS `/ThermoState/Density`) and confirm its length equals the documented extent (185) and that addressing element `(iρ, iT, iYe)` of a 3D sub-table by the flat-offset formula reproduces the value `h5dump` reports at the matching C-order index — confirming the column-major reversal is applied correctly.
3. **Offset-dimensionality check (when a real table is present).** Confirm `/DependentVariables/Offsets` reads as a rank-1 dataset of length `nVariables`, and that any scattering `Offsets` reads as a rank-2 dataset of shape `(nOpacities, nMoments)`.
4. **Legacy-fallback check.** Confirm the reader opens `/EmAb` on the production table, and that the documented fallback to `/EmAb_CorrectedAbsorption` is implemented for a table lacking `/EmAb` (a synthetic or legacy fixture).

### Layer 2 — Fortran parity (specified, PENDING)

This is a reader contract, not an interpolation; "parity" here means the host arrays a C++ reader produces are byte-for-byte identical (after the documented column-major interpretation) to what the weaklib Fortran reader produces from the same `.h5` file. A pending golden fixture would capture, for the pinned tables, the expected axis arrays + offsets + a checksum of each value array as read by the weaklib reader at the pinned commit. Until that exists, the structural-conformance check (Layer 1) against the committed snapshots is the active gate. See `fortran-parity-and-tolerances.md`.

### Mechanical (validator)

`bash specs/tools/validate_specs.sh` (default mode) asserts: the 7 mandated sections in order; the cited weaklib I/O source-of-truth paths resolve; and that the group/dataset names and shapes this spec documents are present in the committed `specs/fixtures/*.h5ls` snapshots — specifically `/ThermoState/Density` and `/DependentVariables/Offsets` in the EOS snapshot, `/EmAb/Offsets` in the EmAb snapshot, `/Scat_Iso_Kernels/Offsets` in the Iso snapshot, `/Scat_NES_Kernels/Kernels` in the NES snapshot, `/Scat_Pair_Kernels/Kernels` in the Pair snapshot, and `/Scat_Brem_Kernels/S_sigma` in the Brem snapshot — with each spec citing the corresponding table by name. In refresh mode (`WL_TABLES_ROOT=…`) each snapshot is re-derived from its live `.h5` table to confirm zero drift.

## Implementation freedom

- The HDF5 access library/bindings used (the official HDF5 C/C++ API, a wrapper, etc.), provided the documented names/shapes/dtypes are read correctly.
- The in-memory representation of the read arrays (a struct of `std::vector<double>`, a flat buffer, etc.) before upload — provided the column-major byte order is preserved and the extents recorded.
- Whether the offset is applied at read time (eagerly recovering `physical`) or kept log-stored and applied during interpolation — the interpolation specs assume the table arrives **log-stored** (`10**(stored) - offset` applied during/after interpolation), so a reader that recovers eagerly must also expose the log-stored form; the contract is that the values handed to the interpolators are the log-stored `log10(value + offset)`.
- How optional/legacy groups are detected (link-exists probe vs. try-open-and-catch).
- Caching, lazy loading, and which datasets are loaded (a reader may skip `minValues`/`maxValues`/`Metadata` if unused, provided the axis/value/offset datasets are read).

## Open questions / assumptions

- **Concrete offsets and grid extents live only in the `.h5` files (assumption, non-blocking).** The numeric `Offsets` values and the energy/η grid node coordinates are written at table-generation time and exist only inside the production tables (research OQ#3). This spec pins the *layout* (names, shapes, dimensionality, dtype, recovery formula); the tables carry the numbers. The committed `*.h5ls` snapshots pin the structure but not the values.
- **Legacy-table surface beyond the named fallbacks (assumption, non-blocking).** This spec pins the fallbacks weaklib's current reader implements (`EmAb_CorrectedAbsorption`; optional `Zoom`/`Edge`/`Width`; optional `EmAb Parameters`/`EC_table`). Older or hand-edited tables may differ in ways not exercised by the six pinned production tables; a reader targeting only those tables (the pinned set) need not handle unseen variants, but the named fallbacks above are required because they appear in the weaklib reader path.
- **Dependent-variable slot assignment is read, not hard-coded (assumption, non-blocking).** Which `/DependentVariables` slot is Pressure vs. Entropy etc. is authoritative via the `/DependentVariables/i<Name>` datasets and the `Names` ordering, not assumed by position; `SwapDependentVariables` in weaklib can reorder them in place. A reader keys variables by name/`i*` slot, not by a fixed index.
- **Layer-2 reader-parity fixtures are future work (assumption, non-blocking).** No weaklib-reader-produced golden arrays exist in this environment; the structural-conformance check against the committed snapshots is the active gate, and the weaklib reader remains the source of truth for byte-level interpretation. See `fortran-parity-and-tolerances.md`.
