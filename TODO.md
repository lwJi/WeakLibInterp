# TODO — WeakLibInterp build plan

Greenfield: no `src/`, no build system, no C++ yet. All 10 acceptance specs under `specs/` are validator-clean, internally consistent, and line-for-line faithful to the weaklib/thornado Fortran — so there is no spec-authoring work, only implementation. The list below is priority-sorted by the real dependency DAG: build bootstrap and test harness first, then shared `src/lib` interpolation primitives, then the table-I/O data layer, then EOS, then the opacity channels, then full-coverage regression and the double-pin verification. List order = build order = priority.

## Standing facts every item inherits

- **Value type pinned to `double`** for all tables, entry points, and on-disk reads (`H5T_NATIVE_DOUBLE`), regardless of `amrex::Real`.
- **Target: AMReX CPU-only, double precision, host execution.** GPU qualifier macros are no-ops, `ParallelFor` is a sequential loop, `Gpu::DeviceVector<double>` is host memory — but the device-contract *shape* must be authored exactly so a GPU build is a later drop-in. Defaults: `AMReX_GPU_BACKEND=NONE`, `AMReX_PRECISION=DOUBLE` (`amrex/Tools/CMake/AMReXOptions.cmake:112,125`).
- **AMReX resolves from `../amrex`** (sibling repo); mirror `validate_specs.sh`'s env-override-then-sibling-probe pattern (`specs/tools/validate_specs.sh:73-77`).
- **Storage/recovery convention:** every tabulated value is stored `log10(value+offset)`; recover `physical = 10**(stored) - offset`. Offset arrays are **1D for EOS & EmAb, 2D `[nOpacities,nMoments]` for all scattering kernels (Iso/NES/Pair/Brem)** — this split is load-bearing (`specs/table-format-and-io.md:162`).
- **Column-major flat indexing** (Fortran-order, reversed C shape), one formula generalized 1D–5D (`specs/amrex-device-interface.md:64-79`); tables carried as flat `double const*` + extents — never `TableData`/`Table*D`/`Array4` (cannot represent the 5D kernels).
- **Bracket/delta boundary policy:** clamp index to `[1,n-1]`, do NOT clamp the delta → out-of-range = linear extrapolation, bit-for-bit with Fortran; no range checks in the interpolation path; NaN inputs propagate (`specs/fortran-parity-and-tolerances.md:66-77,111-117`). Inversion is the exception: out-of-range/NaN → integer error code + `T=0`, not extrapolation.
- **Tolerance tiers** (all `specs/fortran-parity-and-tolerances.md:96-109`, comparison `|got−expected| ≤ rtol·|expected| + atol`): default parity `rtol=1e-12, atol=1e-30`; relaxed `1e-10` (analytic derivatives, inversion-recovered T & round-trip invariant); machine-precision `~1e-14` (affine-in-log exactness); exact/NaN-equality (boundary, NaN propagation, inversion integer error codes).
- **Fixtures:** production `.h5` tables are NOT in the repo — only committed `*.h5ls` structural snapshots + sha256/commit pins (`specs/fixtures/tables.provenance`, weaklib commit `5836de983…`). Real-table checks need `WL_TABLES_ROOT` (or equiv) pointed at an external table dir; **synthetic-table checks are the always-runnable gate** and depend on no external data. Synthetic checks choose their own offsets/extents and never hard-code production numbers.
- **`src/lib` is the standard library:** the interpolation primitives are restated identically across all 5 leaf specs by design; build each once in `src/lib`, never per-channel (CLAUDE.md "flag duplication").
- Spec validation already works: `AMREX_ROOT=../amrex bash specs/tools/validate_specs.sh` (spec-linter only, builds no C++).

## Tier 0 — Build & test foundation

- [ ] Build bootstrap — CMake tree + AMReX linkage + two link targets
  - spec: specs/build-integration.md
  - tests: configure+build of both targets ("the library" and "the regression suite") succeeds on host; no `enable_language(Fortran)`/Matlab and HDF5 Fortran bindings stay off (verification #2); AMReX source resolves from `../amrex` (verification #4,#5); `validate_specs.sh` still passes (verification #1).
  - notes: Hard prerequisite for every other task — nothing else compiles/tests without it. Pin `AMReX_GPU_BACKEND=NONE` + `AMReX_PRECISION=DOUBLE`; "the regression suite" links library + AMReX + C++ HDF5 via `find_package(HDF5 COMPONENTS CXX)` — verify HDF5 succeeds locally before assuming it. Confirm AMReX's minimum C++ standard (~C++17) from `amrex/CMakeLists.txt`. Verification #3 (double survives `amrex::Real=float`) is deferred to the double-pin task. Decisions: AMReX consumption `add_subdirectory(../amrex)` (lower-friction) vs `find_package(AMReX)`.

- [ ] Regression harness skeleton + tolerance/exact/NaN comparators + pass/fail-is-real self-test
  - spec: specs/regression-suite-design.md, specs/fortran-parity-and-tolerances.md
  - tests: comparator passes/fails correctly at each tier (incl. atol floor and NaN-equality); the "pass/fail is real" meta-check — a deliberately perturbed expected value makes its cell fail the suite (regression-suite-design.md:100).
  - notes: Assert-and-fail (never print-only), aggregate exit status; framework free (hand-rolled/GoogleTest/Catch2). The mixed `rtol/atol` `expect_near` + exact-equality + NaN-equality comparators (parameterized by the four named tiers) are the single shared test-support primitive every later task's checks call — build once here. Decisions: comparator home (`src/lib` vs `src/test-support`); "pass/fail is real" as one-time proof vs standing meta-test.

## Tier 1 — Shared interpolation primitives (`src/lib`)

- [ ] Column-major flat-index helper (1D–5D) + indexing round-trip check
  - spec: specs/amrex-device-interface.md:64-79, specs/fortran-parity-and-tolerances.md:119-127
  - tests: indexing round-trip — write sentinels into a flat buffer via the formula, read back through the helper for 3D/4D/5D (amrex-device-interface.md verification #2).
  - notes: Header-only `AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE`. Consumed by every `_Point` kernel and the reader/upload path; establishes the flat-buffer layout convention project-wide.

- [ ] Bracket/delta + node-search primitives
  - spec: specs/fortran-parity-and-tolerances.md:66-77
  - tests: bracket index correct incl. clamp at both ends; delta unclamped (linear-extrapolation regime) → boundary/node-identity behavior; log-space vs linear-space bracketing match Fortran formulas.
  - notes: `GetIndexAndDelta_Lin`/`GetIndexAndDelta_Log` (clamp index `[1,n-1]`, unclamped delta) as device-inline free fns over `double const* Xs, int n`; plus the distinct `Index1D`-style integer node-search (no delta) that EOS-inversion bisection needs.

- [ ] N-linear-in-log basis + `10**(stored)-offset` recovery
  - spec: specs/fortran-parity-and-tolerances.md:40-54
  - tests: affine-in-log exactness at `~1e-14` (machine tier); node identity (delta=0 returns exact corner value after recovery).
  - notes: `Linear`/`BiLinear`/`TriLinear`/`TetraLinear` corner-weighted combination + recovery; mirrors `wlInterpolationUtilitiesModule.F90:212-310`. The correctness-bearing core reused by EOS (tri), EmAb/Iso (tetra), NES/Pair/Brem (bi). Derivative chain-rule scale factors are EOS-only → built in the EOS-differentiate task, not here. Decisions: one dimension-parameterized template vs four named fns; **and the project-wide device-signature convention** (pointer+strides vs corner-scalar `_Point`, arg order/grouping, `ParallelFor` overload, metadata packing) must be settled here + the two neighboring primitives since every leaf depends on it.

- [ ] Device-resident table wrapper + `ParallelFor` launch convention
  - spec: specs/amrex-device-interface.md:40-44,80-88
  - tests: device/host equivalence — same `_Point` fn called directly vs inside `ParallelFor` gives identical results (verification #1); no-allocation/kernel-callability — compiles and runs inside `ParallelFor` (verification #3).
  - notes: Dimensionality-agnostic wrapper: host-fill flat `double` buffer → `amrex::Gpu::DeviceVector<double>` → `Gpu::htod_memcpy`; small `GpuArray`/POD metadata carriers (extents, log/linear axis flags, per-moment offsets) captured by value; one host-level `ParallelFor` array-launch pattern wrapping `_Point` calls (pointer+extents by value, `noexcept` lambda). Consumes the reader's flat buffer (dependency edge) but exercisable with a synthetic buffer first.

## Tier 2 — Table format & I/O

- [ ] HDF5 shared reader primitives + EOS table reader
  - spec: specs/table-format-and-io.md (EOS: :47-83,159)
  - tests: structural conformance against committed `wl-EOS-SFHo-15-25-50.h5ls` (always-runnable); real-table round-trip + offset-dimensionality (gated on `WL_TABLES_ROOT`, table-format-and-io.md:176-178).
  - notes: Shared `src/lib` HDF5 helpers (open-by-name, Names-before-arrays read order, read-dataset, 1D-vs-2D `Offsets` dispatch, optional-dataset/`Zoom`-keyed probe, `EmAb`→`EmAb_CorrectedAbsorption` fallback) + EOS reader producing host axis arrays (`/ThermoState/{Density,Temperature,Electron Fraction}`), 1D `/DependentVariables/Offsets{15}`, flat log-stored value buffers. Source `wlEOSIOModuleHDF.f90:183-213`. Foundational data layer — must precede EOS/opacity real-table cells. Decisions: in-memory representation + offset-application timing (eager `physical` recovery vs keep log-stored) chosen once here, applied identically across all readers.

- [ ] HDF5 opacity-channel readers (EmAb / Iso / NES / Pair / Brem)
  - spec: specs/table-format-and-io.md:104-166
  - tests: structural conformance against the 5 committed `wl-Op-…-{EmAb,Iso,NES,Pair,Brem}.h5ls` snapshots; 1D-vs-2D offset dispatch correctness; legacy-fallback branch selection (gated real-table tier where applicable).
  - notes: Reuse the shared reader primitives for the 5 channel files; 2D `Offsets[nOpacities,nMoments]`; `/EnergyGrid` (+ `/EtaGrid` for NES/Pair); EmAb legacy fallback + optional `/EmAb Parameters`,`/EC_table` → sentinel `-1`; skip/read Iso correction-flag scalars. Source `wlOpacityTableIOModuleHDF.f90:995-1066,1157-1164,1355-1463`.

## Tier 3 — EOS interpolation & inversion

- [ ] EOS forward 3D evaluate — `LogInterpolateSingleVariable_3D_Custom_Point`
  - spec: specs/eos-interpolation.md:66-99,114-122
  - tests: affine-in-log exactness (machine tier); node identity; boundary extrapolation (bit-for-bit); NaN propagation; against synthetic table (self-contained) and, for node-identity/boundary/NaN, real `wl-EOS-SFHo-15-25-50.h5` (gated); result parity tier `rtol=1e-12/atol=1e-30`.
  - notes: Scalar device fn over `(ρ,T,Yₑ)`: log/log/linear bracket+delta, 8-corner trilinear-in-log, `10**(...)-OS` recovery. Oracle `wlInterpolationModule.F90:1640-1707`.

- [ ] EOS evaluate+differentiate 3D — `LogInterpolateDifferentiateSingleVariable_3D_Custom_Point`
  - spec: specs/eos-interpolation.md:82-92, specs/fortran-parity-and-tolerances.md:79-88
  - tests: derivative chain-rule vs finite-difference at relaxed `1e-10`; value matches the forward-evaluate output; node identity / boundary / NaN as the forward task.
  - notes: Adds `(value, dval/dρ, dval/dT, dval/dYₑ)` via chain-rule scale factors (log-axis `a=1/(X·log10(Xs[i+1]/Xs[i]))`, linear-axis `a=ln10/(Xs[i+1]-Xs[i])`) × `dTriLinear`. Coverage-matrix row 2; builds directly on the forward kernel. Oracle `wlInterpolationModule.F90:1814-1844`.

- [ ] EOS inversion — input validation + `InverseLogInterp` + 2D fixed-T-node face evaluator + error mapping
  - spec: specs/eos-inversion.md:77-111,166-170
  - tests: error-code exactness across the 6 defined codes `{0,01,02,03,10,11,13}`; `InverseLogInterp` affine-in-log exactness; input-validation ordering.
  - notes: `InverseLogInterp` scalar formula; `CheckInputError` exact code order `10→11→01→02→03→0`; bounds/init-state representation (yields code `10` when absent); the 2D bilinear-in-log `(ρ,Yₑ)` fixed-T-node evaluator (built on shared `BiLinear` as `src/lib` code — NOT covered by eos-interpolation.md which is 3D-only, so build here, not a duplicate); `DescribeEOSInversionError` mapping + abort for codes >13. Source `wlEOSInversionModule.F90:188-267`, `wlInterpolationModule.F90:1115-1165`.

- [ ] EOS inversion — bisection kernels + family dispatch + Error/NoError variants
  - spec: specs/eos-inversion.md:83,108-128,144-152
  - tests: round-trip invariant at relaxed `1e-10` (atol `1e-30`); `T=0`-on-failure signaling; no-root/non-monotone highest-temperature-root and nearest-to-guess root selection (needs a synthetic non-monotone-in-T table); against synthetic + gated real table.
  - notes: `ComputeTemperatureWith_DXY_Guess` (guess-cell short-circuit → full-range node bisection → nearest-to-guess linear-scan fallback, `Error=13`) and `_NoGuess` (full-range bisection → highest-temperature-root scan, `Error=13`); convergence = `i_b==i_a+1` (no Newton, no float tol); thin dispatch over families DEY/DPY/DSY; `_Error`/`_NoError` wrappers (`T=0` sole failure signal for `_NoError`). Depends on the prior inversion task. Source `wlEOSInversionModule.F90:270-573`.

## Tier 4 — Opacity channels

- [ ] Opacity EmAb + Iso — shared 4D tetralinear kernel + two thin wrappers
  - spec: specs/opacity-emab-iso.md:33-114
  - tests: affine-in-log exactness (machine tier); node identity; Iso moment-slice independence; boundary extrapolation; NaN propagation; against synthetic + gated real `…-EmAb.h5`/`…-Iso.h5`.
  - notes: ONE `src/lib` 4D tetralinear-in-log `_Point` kernel (16-corner) reused by both: `emab_point` (full 4D table, 1D `Offsets[species]`) and `iso_point` (5D→4D fixed-`(species,moment)` slice, 2D `Offsets[species][moment]`). Coverage rows 4 & 5 — do not duplicate the kernel for Iso. Oracle `wlInterpolationModule.F90:1754-1779`, `wlInterpolationUtilitiesModule.F90:729-768`.

- [ ] Opacity NES + Pair — aligned bilinear-in-log kernel + symmetry-fill closure checks
  - spec: specs/opacity-nes-pair.md:118-174
  - tests: the 7 checks — affine-in-log exactness, node identity, kernel-slice independence, NES detailed balance, Pair crossing symmetry, boundary extrapolation, NaN propagation; against synthetic + gated real `…-NES.h5`/`…-Pair.h5`.
  - notes: Shared `src/lib` "aligned" 2D-bilinear adapter (fixed leading `(iE',iE,kernel)` indices, interpolate `(log10 T, log10 η)`; T in **MeV** here) reused by both channels + Brem; plus NES detailed-balance fill `Phi(iEp,iE)=Phi(iE,iEp)·exp((E(iE)-E(iEp))/T)` and Pair crossing-symmetry fill (energy-transpose + `Ji↔Jii` swap). Build the aligned adapter once here. Decisions: symmetry fills as product code vs test-only helper. Oracle `wlInterpolationModule.F90:1455-1485`, `wlOpacityInterpolationModule.f90:104-115,196-228`.

- [ ] Opacity Brem — aligned summed 3-effective-density kernel
  - spec: specs/opacity-brem.md:126-153,172-176,200-211
  - tests: the 8 checks — affine-in-log exactness, node identity, moment-slice independence, decomposition closure, weight-order sensitivity, both-triangles-computed-independently, boundary extrapolation, NaN propagation; against synthetic + gated real `…-Brem.h5`.
  - notes: Reuse the aligned bilinear adapter, invoked 3× against the same 4D `(E',E,ρ,T)` sub-table for effective densities `ρ·xₚ, ρ·xₙ, ρ·√|xₚ·xₙ|` (supplied pre-`LOG10`'d) summed with fixed weights `Alpha_Brem=[1,1,28/3]`; both energy triangles computed independently (no symmetry fill). `xₚ,xₙ` are EOS-supplied inputs, not computed here (dependency edge, out of scope). Oracle `wlInterpolationModule.F90:1488-1595`, `wlInterpolationUtilitiesModule.F90:602-627`.

## Tier 5 — Full-coverage regression & double-pin

- [ ] Full coverage-matrix regression runner (real-table cells + closure)
  - spec: specs/regression-suite-design.md:36-102
  - tests: closure — every leaf entry point realized as a matrix row (enforced by `validate_specs.sh`); each row×regime realized with the tier-appropriate assertion; synthetic cells always run, production cells run when tables present.
  - notes: Assemble the 8-entry-point × 4-regime (in-bounds/on-edge/out-of-range/NaN-input) × 2-fixture-source coverage matrix into one runner with aggregate exit status; wire real-production-table cells (node-identity/boundary/NaN) for all entry points through the readers. Depends on all Tier 2–4 tasks. Decisions: production-table cells when tables absent (skip vs fail) — synthetic cells must always run as the real gate.

- [ ] Double-pin verification under single-precision AMReX
  - spec: specs/build-integration.md:63
  - tests: `~1e-14`-tier checks pass identically under both the double and single AMReX builds (verification #3).
  - notes: A second build configuration/preset building AMReX with `AMReX_PRECISION=SINGLE` (`amrex::Real=float`), re-running the machine-precision checks to prove the library's tables/entry points stay `double`. Lowest priority; needs the full coverage-matrix machine-precision checks to exist. Could be a CI leg / CMake preset.

## Decisions & non-blocking spec items

No genuinely-missing spec was found — all 10 specs are validator-clean and faithful to Fortran; `src/lib`'s shape is inferred from cross-spec repetition (which the specs intend), not a gap. All items below are implementation-freedom choices or ambiguities *within* existing specs, resolved by the build loop when it reaches the noted task — none is itself a build increment and none needs a new spec.

- **Device-signature convention** (biggest up-front decision): pointer+strides vs corner-scalar `_Point` signatures, arg order/grouping, `ParallelFor` overload, metadata packing — all implementation freedom (`amrex-device-interface.md:107-113`). Pick ONE convention project-wide (settle with the Tier-1 primitive tasks); divergence would violate the no-duplication constraint. *Ambiguity within spec.*
- **`src/lib` N-linear kernel form:** one dimension-parameterized template vs four named `Linear/BiLinear/TriLinear/TetraLinear` fns (`fortran-parity-and-tolerances`/leaf implementation-freedom sections). Decide once with the N-linear-basis task. *Ambiguity within spec.*
- **Comparator / test-support home:** `src/lib` vs a sibling `src/test-support` target — `regression-suite-design.md:104-109` leaves harness layout free. Build-loop decision at the harness task. *Genuinely unspecified, non-gap.*
- **In-memory table representation + offset-application timing** (eager `physical` recovery vs keep log-stored) — free (`table-format-and-io.md:184-190`); choose once at the EOS-reader task, apply identically across EOS + all 5 opacity readers. *Ambiguity within spec.*
- **AMReX consumption:** `add_subdirectory(../amrex)` vs installed `find_package(AMReX)` — free (`build-integration.md:70`); `add_subdirectory` is lower-friction for the loop. *Ambiguity within spec.*
- **2D fixed-T-node evaluator placement:** shared `src/lib` code vs inlined in inversion — free (`eos-inversion.md:161`); recommended shared to avoid a duplicate bilinear-in-log. *Ambiguity within spec.*
- **Symmetry fills (NES/Pair):** product-code primitive vs test-only helper — free (`opacity-nes-pair.md:216`). *Ambiguity within spec.*
- **Production-table cells when tables absent:** skip vs fail — free (`regression-suite-design.md:108,113`); synthetic cells must always run as the real gate. *Ambiguity within spec.*
- **"Pass/fail is real" self-test:** one-time proof vs standing meta-test — ambiguous wording (`regression-suite-design.md:100,55`); pick one at the harness task. *Ambiguity within spec.*
- **Build hygiene (not tasks):** keep HDF5 Fortran bindings off (`HDF5_ENABLE_FORTRAN`); confirm AMReX's minimum C++ standard from `amrex/CMakeLists.txt`; verify a C++ HDF5 install is present locally before assuming it.
- **Environment carry-forward:** production `.h5` tables are absent (only `.h5ls` + provenance). Every real-table regression cell is gated on `WL_TABLES_ROOT`; synthetic-table checks are the always-runnable gate and must never hard-code production offsets/extents/slot mappings (those live only in the `.h5` files).
