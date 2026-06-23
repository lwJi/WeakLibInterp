# WeakLibInterp specifications

This `specs/` set is the durable contract for a GPU-friendly C++ reimplementation of weaklib's equation-of-state (EOS) and opacity interpolators, exposed as AMReX-native device functions. It is written to drive a **Ralph loop**: a long-running loop in which a *fresh* agent re-reads the specs on every iteration with no memory of prior runs. Each spec pins down *what "correct" means and how to verify it* while leaving *how to implement it* open.

Open any single spec file in isolation: it is self-contained. The two cross-cutting specs (`fortran-parity-and-tolerances`, `amrex-device-interface`) carry the shared numeric and device contracts; every leaf spec restates only the conventions it uses and references those two for the rest. **This README is the canonical source if a leaf spec ever conflicts with it.**

## Global correctness contract (inherited by every spec)

- **Authoritative oracle / generator-of-record.** "Correct" for a single query is, by definition, the value the matched weaklib Fortran `_Point` (single-query, scalar) routine produces on identical inputs at the pinned weaklib commit (recorded in `fixtures/tables.provenance`). Each leaf spec names its `_Point` routine by file and line.
- **Two-tier tolerances** (mixed relative/absolute: `|got - expected| <= rtol·|expected| + atol`):
  - **Default parity** — `rtol = 1e-12`, `atol = 1e-30` — for a single interpolated value vs. the Fortran oracle.
  - **Relaxed** — `1e-10` — for analytic derivatives, inversion-recovered T, and the round-trip inversion invariant (interpolation-of-an-interpolation accumulates more rounding; each leaf states the rationale where it relaxes).
  - **Machine-precision exactness** — `~1e-14` (a few ULP) — for Layer-1 checks with closed-form exact answers: affine-in-log tables, constant tables, node identity.
  - **Exact / NaN-equality** — for boundary index behavior, NaN propagation, and inversion integer error codes.
- **Universal log-space + offset storage.** Every tabulated quantity is stored as `log10(value + offset)`, interpolated multilinearly in that space, and recovered with `value = 10**(stored) - offset`. ρ, T, E, and η = μₑ/kT are interpolated in **log** space; Yₑ in **linear** space; discrete moment/kernel/species indices are **not interpolated** (used directly as integer indices).
- **Floating-point type.** `double` (IEEE-754 binary64; weaklib `dp = 8`), pinned regardless of `amrex::Real`; on-disk reals are `H5T_NATIVE_DOUBLE`.
- **Fortran column-major layout.** The Fortran first index is fastest-varying. A logical shape `(n0, n1, ..., n_{D-1})` indexed `(i0, ..., i_{D-1})` is the flat offset `i0 + n0*(i1 + n1*(i2 + ... + n_{D-2}*i_{D-1} ...))`; the C shape is the reverse, and `h5ls` dimension lists are the reverse of the Fortran shape.
- **Permissive boundary/NaN behavior, replicated bit-for-bit.** Clamp the bracket index to the edge cell but do **not** clamp the fractional delta (out-of-range queries linearly extrapolate from the edge); `log10` of a non-positive argument yields a silently propagating NaN. Range enforcement is a *consumer* responsibility. The EOS inversion path is the one exception, with an integer error-code protocol and `T = 0` on failure.
- **Two-layer regression scheme.** **Layer 1** (self-contained closed-form/invariant checks, run against synthetic *and* real production tables) is the **sole active gate**. **Layer 2** (Fortran-parity golden fixtures, generated offline) is fully specified but **pending** until fixtures are supplied; its tests report pending, never passing. The suite runs **only C++/AMReX** (AMReX built CPU-only, double precision; no Fortran/Matlab at test time).
- **Reference tables.** Synthetic in-suite tables are the always-on Layer-1 primary; the named production tables (pinned by path + `sha256` in `fixtures/tables.provenance`, structure committed in `fixtures/*.h5ls`) anchor the reader contract and real-table Layer-1 invariants now, and Layer-2 parity later.

## Spec index

| Spec | Description | Status |
|---|---|---|
| [fortran-parity-and-tolerances](./fortran-parity-and-tolerances.md) | Cross-cutting numeric contract: named oracle, tolerance tiers, log-space/offset + column-major conventions, bit-for-bit boundary/NaN policy, two-layer scheme. | committed |
| [amrex-device-interface](./amrex-device-interface.md) | Cross-cutting device contract: qualifier macros, `Gpu::DeviceVector<double>` residency + `htod_memcpy` upload, raw `double const*` + extents column-major indexing, scalar `_Point` contract, `ParallelFor` launch. | committed |
| [eos-interpolation](./eos-interpolation.md) | EOS single-variable 3D `(ρ,T,Yₑ)` trilinear-in-log evaluate + differentiate `_Point` contract. | committed |
| [eos-inversion](./eos-inversion.md) | Recover T from `(ρ, X∈{E,P,S}, Yₑ)` via node bisection + log-linear inverse; integer error codes, `T=0`-on-failure, round-trip invariant. | committed |
| [table-format-and-io](./table-format-and-io.md) | On-disk HDF5 reader contract: EOS/opacity group + dataset names, shapes, dtypes, per-channel offsets, column-major reversal, legacy fallbacks. | committed |
| [opacity-emab-iso](./opacity-emab-iso.md) | EmAb 4D `(E,ρ,T,Yₑ)` and Iso 5D `(E,moment,ρ,T,Yₑ)` opacity channels sharing the EOS thermodynamic axes; shared 4D `_Point` kernel, EmAb 1D vs Iso 2D offsets, moment index not interpolated. | committed |
| [opacity-nes-pair](./opacity-nes-pair.md) | NES + Pair 5D `(E',E,kernel,T,η)` channels; detailed-balance and crossing-symmetry invariants; `_2D2D_Custom_Aligned` consumer path. | committed |
| [opacity-brem](./opacity-brem.md) | Brem 5D `(E',E,moment,ρ,T)` channel; `[1,1,28/3]` effective-density decomposition; no symmetry fill; `_2D2D_Custom_Aligned` summed consumer path. | committed |
| [regression-suite-design](./regression-suite-design.md) | The two-layer scheme as a runnable coverage matrix over every public device entry point × {in-bounds, on-edge, out-of-range, NaN-input} × {synthetic + named tables}; tolerance tiers; assert-against-tolerance pass/fail; C++/AMReX-only. | committed |
| [build-integration](./build-integration.md) | AMReX as a required CPU-only / double-precision dependency linked by the library + suite; value type pinned to `double` regardless of `amrex::Real`; no Fortran/Matlab build or runtime dependency. | committed |

All 10 specs are committed and linked above. The validator (`tools/validate_specs.sh`) enforces that the README links **exactly** these 10 spec files — no orphan spec files on disk, no missing links, no broken links — and that the `regression-suite-design` coverage matrix references every leaf spec's public entry points (the closure check). Coverage grows by registry append in `tools/validate_specs.sh`.

## Validating the spec set

`bash tools/validate_specs.sh` runs the **default, CI-reproducible** mode (no production tables needed): it checks every registered spec has the 7 mandated sections in order, names a concrete numeric tolerance, cites source-of-truth paths that resolve in the sibling `weaklib`/`amrex` repos, and matches its documented table structure against the committed `fixtures/*.h5ls` snapshots; it checks the README index links every registered spec with no orphans/broken links; and it checksums the committed snapshots against `fixtures/tables.provenance`.

`WL_TABLES_ROOT=/path/to/live/tables bash tools/validate_specs.sh` additionally runs the **provenance/refresh** mode: it re-derives each `fixtures/*.h5ls` from the live multi-GB `.h5` tables and confirms zero drift plus matching `sha256`. With `WL_TABLES_ROOT` unset, that stage prints `SKIPPED` — never a silent pass.

The 7 mandated sections, in order, that every leaf spec must contain:

```text
## Purpose & scope → ## Source of truth → ## Inputs & outputs →
## Correctness requirements → ## Verification → ## Implementation freedom →
## Open questions / assumptions
```
