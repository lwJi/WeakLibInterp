# TODO — WeakLibInterp build plan

Current state: the serial CPU-only contract from the previous plan cycle is fully implemented and green (19 ctest targets pass, single-precision pin verified in `build-single/`; the leaf math in `src/{core,eos,opacity,io}/` is line-verified against the Fortran oracles). Branch `mpi` (commit `1f522e2`) extended three specs — `build-integration.md`, `regression-suite-design.md`, `table-format-and-io.md` — with an MPI contract that is entirely unimplemented: zero `ParallelDescriptor`/`Bcast`/`MPI_` references anywhere in `src/` or `test/`, `AMReX_MPI` hardcoded OFF at `CMakeLists.txt:49`, no MPI CI job. This plan carries that MPI work (Tier 5, dependency-ordered — each item blocks the next) plus residual pre-MPI gaps the re-audit surfaced (Tier 6). List order within each tier = priority.

## Standing facts every item inherits

- **Value type pinned to `double`** on the entire correctness-bearing path (`src/core/wli_real.H`, `wli::Real`), independent of `amrex::Real`. Never let `amrex::Real` leak into the math.
- **MPI contract shape:** `AMReX_MPI=ON` is a SECOND correctness-gating configuration alongside the serial default; the default `cmake -S . -B build` must stay MPI-free and byte-unchanged. `AMReX_MPI` is baked into the AMReX compile, so the MPI config needs a separate full tree (`build-mpi/`, gitignored), exactly like `build-single/`. (`build-integration.md:13,45,61,72`)
- **`amrex::ParallelDescriptor` is the MPI source of truth** (communicator, rank identity, `Bcast`, `IOProcessor()`); no GPU-aware-MPI assumptions; production target shape is rank-per-GPU clusters (Frontier/Perlmutter) but this repo's MPI runs are CPU-only. Resolved by spec — do NOT build: no combined MPI×GPU job (`build-integration.md:87`), no parallel HDF5 (`table-format-and-io.md:208`), AMReX's communicator is THE communicator (`:207`), node-level table sharing deferred (`:206`), no >2^31 dataset chunking needed for any pinned table (`:171`).
- **Root-read + broadcast distribution:** only the root rank opens the `.h5`; a fixed-size metadata handshake broadcasts first so non-root ranks can allocate, then arrays; every rank ends with a bit-identical table; failure is collective (same error on every rank, no rank hangs). Mirrors weaklib `BroadcastEquationOfStateTableParallel` (`wlEOSIOModuleHDF.f90:215-354`). Transparent: same six reader entry points, no separate parallel API (`table-format-and-io.md:171`).
- **Tolerance tiers** (`src/core/wli_compare.H`): parity `rtol=1e-12, atol=1e-30`; relaxed `1e-10`; machine `~1e-14`. Rank-consistency cells run at the EXACT tier — bitwise-identical doubles, no tolerance (`regression-suite-design.md:65-72`).
- **Sandbox MPI availability is unverified:** `CLAUDE.md` lists no MPI toolchain among host prerequisites. First MPI increment should try `apt install libopenmpi-dev openmpi-bin`; if the sandbox forbids it, MPI ctest execution becomes CI-verifiable like the CUDA/HIP jobs, but the CMake knob and the broadcast code path must still compile and the serial default must stay green locally.
- Cap parallel builds at `-j4` (uncapped AMReX builds OOM this host). After adding a new test target to `test/CMakeLists.txt`, re-run the configure step before building.

## Tier 5 — MPI-enabled build & table distribution

- [ ] `WLI_AMREX_MPI` CMake cache knob (default OFF) + separate MPI build tree
  - spec: build-integration.md — acceptance source of truth (§:13,45,61,72)
  - tests: MPI tree (`cmake -S . -B build-mpi -DCMAKE_BUILD_TYPE=Release -DWLI_AMREX_MPI=ON`) configures and builds library + suite; default `build/` behavior byte-unchanged; ctest at 1 rank under the launcher equals the serial baseline (19/19 + 1 distinct SKIP) as an explicit assertion, not an assumption.
  - notes: mirror the existing non-FORCE `WLI_AMREX_PRECISION`/`WLI_GPU_BACKEND` knob pattern (`CMakeLists.txt:24-25,33-39`) feeding the forced `AMReX_MPI` value at `CMakeLists.txt:49` — parametrize the VALUE, keep FORCE and the other guard vars. Confirm whether `AMReX::amrex` transitively carries `MPI::MPI_CXX` or whether `wli_lib`/tests need an explicit top-level `find_package(MPI)` — the explicit find is also what provides `MPIEXEC_EXECUTABLE`/`MPIEXEC_NUMPROC_FLAG` for the launcher item, and the two routes can resolve to different MPI installs; pin one. Add `build-mpi/` to `.gitignore`. Record the working recipe in `CLAUDE.md` Build & run (single-precision subsection format). Blocks every other Tier-5 item.

- [ ] Root-read + broadcast in all six HDF5 reader entry points
  - spec: table-format-and-io.md — acceptance source of truth (§:15,29-31,158,171,184)
  - tests: (a) instrumented open-counter proves non-root ranks never open the `.h5` file; (b) a root-side open failure (nonexistent path) surfaces as the same error on every rank with no rank hanging in a later broadcast (collective failure via a broadcast status flag); (c) the single code path collapses to today's serial behavior when `AMReX_MPI=OFF` — full serial suite stays green.
  - notes: entry points: `read_eos_table` (`src/io/wli_io_eos.cpp:31-33`) and the five opacity readers (`src/io/wli_io_opacity.cpp:91-93,124-126,152-155,184-186`; NES and Pair share `read_scat_nes_pair`). NO separate parallel entry point (spec `:171`). Order is correctness-bearing: broadcast a fixed-size metadata handshake FIRST (extents, `nVariables`/`nOpacities`/`nMoments`, offsets dimensionality, read-status flag) so non-root ranks allocate before any axis/value/offset/name array arrives. Centralize root/broadcast logic in ONE shared helper (extend `src/io/wli_io_hdf5_detail.H` or a new sibling) reused by all six readers, exactly as they already share `read_thermo_state`/`read_double`/`read_strings` — no per-reader duplication. Freedoms to pin once here: `ParallelDescriptor::Bcast` vs raw `MPI_Bcast`; string/name packing; root = `IOProcessorNumber()` (spec calls it the natural choice); packed-int metadata buffer (weaklib style, `wlEOSIOModuleHDF.f90:263`) vs several typed broadcasts. weaklib has no parallel opacity reader — apply the EOS broadcast pattern uniformly.

- [ ] CTest MPI launcher wiring — full matrix re-run at 2 and 4 ranks
  - spec: regression-suite-design.md — acceptance source of truth (§:65-72); build-integration.md §:61,72
  - tests: the unmodified coverage matrix (8 entry-point rows × 4 regimes, `test/test_coverage_matrix.cpp`) and the full suite pass under the MPI launcher at BOTH 2 and 4 ranks in the `WLI_AMREX_MPI=ON` tree.
  - notes: wrap the existing bare `add_test(COMMAND <exe>)` registrations in `test/CMakeLists.txt` with `${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} <N> ${MPIEXEC_PREFLAGS} <exe> ${MPIEXEC_POSTFLAGS}`, parameterized/duplicated for N=2 and N=4; reuse the `SKIP_RETURN_CODE 77` precedent (`test/CMakeLists.txt:211`). Launcher/registration mechanics are implementation freedom (`build-integration.md:82`, `regression-suite-design.md:121`); whether one configuration exercises both rank counts or two ctest invocations do is also free (`build-integration.md:72` silent) — decide once, record in CLAUDE.md. Existing tests print+assert per-process; expect duplicated output per rank, which is fine — correctness is exit codes.

- [ ] Rank-consistency cells + cross-rank-mismatch meta-test
  - spec: regression-suite-design.md — acceptance source of truth (§:65-72,112); table-format-and-io.md §:183
  - tests: (a) table-load consistency — after loading each table (synthetic fixture always; production tables guarded), per-dataset byte digests plus extents/offsets/names compare identical across ranks; (b) result consistency — identical sample queries against each of the 8 public entry points return bitwise-identical `double`s on every rank; both at the EXACT tier; (c) a deliberate one-rank corruption meta-test FAILS the consistency check (positive control proving the comparison is cross-rank, not per-rank-only).
  - notes: new `test/test_rank_consistency.cpp`, MPI-launched, SKIP 77 when not built under MPI. Comparison mechanism (gather-to-root digest vs allreduce min/max vs per-rank dump) is freedom (`regression-suite-design.md:121`) — pin once. Meta-test mirrors the `test/test_perturbation_meta.cpp` pattern.

- [ ] MPI CI job (correctness-gating — builds AND runs ctest)
  - spec: build-integration.md — acceptance source of truth (§:6,61,72)
  - tests: `.github/workflows/ci.yml` gains a `build-test-mpi` job: install an MPI implementation, configure `-DWLI_AMREX_MPI=ON`, build, run ctest under the launcher at 2 and 4 ranks — green on push.
  - notes: distinct from the compile-only CUDA/HIP jobs — this one MUST run ctest (second correctness-gating configuration). OpenMPI on hosted runners typically needs `--oversubscribe` (and `--allow-run-as-root` inside containers) — bake into `MPIEXEC_PREFLAGS` or env. ccache the MPI AMReX compile keyed per-config like the GPU jobs (`AMREX_REF`-based key). Leave the existing jobs untouched.

## Tier 6 — Residual pre-MPI gaps

- [ ] `DescribeEOSInversionError` code→string mapping
  - spec: eos-inversion.md — acceptance source of truth (§:12,34,94-106,110)
  - tests: each of the 7 error codes {0,01,02,03,10,11,13} maps to its expected description string.
  - notes: the only named source-of-truth routine with zero C++ counterpart; `src/eos/wli_eos_inversion.H:35` currently marks it out of scope but the spec text lists it as in-scope — implement (host-side is fine; it is diagnostic, not device math). Oracle `wlEOSInversionModule.F90:230-250`.

- [ ] Production-table kernel parity: Iso/NES/Pair/Brem live cells + EOS inversion round-trip
  - spec: opacity-emab-iso.md, opacity-nes-pair.md (§:140), opacity-brem.md, eos-inversion.md (§:146) — production parity `rtol=1e-12, atol=1e-30`
  - tests: guarded live-`.h5` node-identity + boundary + NaN cells for Iso, NES, Pair, and Brem that actually call `IsoInterpolate...`/`NESPairInterpolate...`/`BremInterpolate...` on real data, mirroring the existing `run_eos`/`run_emab` cells; an inversion round-trip via `ComputeTemperatureWith_{DEY,DPY,DSY}_{NoGuess,Guess}` against `wl-EOS-SFHo-15-25-50.h5`; distinct SKIP (77) when `$WL_TABLES_ROOT` is absent.
  - notes: `test/test_production_tables.cpp` is schema-only for Iso (`:150-158`), NES/Pair (`:160-174`), Brem (`:176-184`) — counts/extents checked, kernels never invoked on real data; inversion has zero live coverage. The live branch is unexercisable in-sandbox (host-only tables per `tables.provenance`) — implement fully, verify the skip branch locally; live verification happens user-side. Remember the linker lesson: binaries calling `wli::io::read_*` need the HDF5 C library explicitly.

- [ ] Host-level `ParallelFor` array-form wrappers over the `_Point` kernels
  - spec: amrex-device-interface.md — acceptance source of truth (§:80-88,100-102)
  - tests: array form produces per-element results bit-identical to a serial loop of the `_Point` core; callable inside a `ParallelFor` lambda (device/host equivalence on this CPU-only target).
  - notes: no production `ParallelFor` wrapper exists in `src/` for any kernel — only test-infra self-checks; `src/opacity/wli_opacity_emab_iso.H:48` and `src/eos/wli_eos_inversion.H:34` carry "out of scope" comments that this item retires. Wrappers should consume `ResidentTable`/`TableView` (`src/core/wli_table.H`), which are currently unit-tested but wired into no real call site — readers hand raw host `std::vector` data straight to `_Point` kernels. Surface: EOS evaluate/differentiate/inversion, EmAb, Iso, NES/Pair (+ both symmetry fills), Brem. Keep the `_Point` cores untouched; wrappers compose, never duplicate.

- [ ] Minor coverage fills: literal flat-in-T code-13 case + DSY row in the coverage matrix
  - spec: eos-inversion.md (§:149); regression-suite-design.md — acceptance source of truth
  - tests: a constant-in-T (flat sub-table) inversion returns code 13 with `T==0`; `test/test_coverage_matrix.cpp` gains a DSY row passing the same regime cells as the existing DEY/DPY rows.
  - notes: the current code-13 check uses a monotone-column-unreachable-value shape, not the spec-named degenerate flat shape; DSY is covered by the dedicated inversion binaries but absent from the umbrella matrix.

## Decisions & non-blocking spec items

These are decisions/notes, NOT build increments — the responsible increment resolves each once and reuses it.

- **Opacity evaluate-and-differentiate spec candidate** — the oracle defines derivative cores for 4D and 2D2D (`LinearInterpDeriv4D_4DArray_Point`, `LinearInterpDeriv2D_4DArray_2DAligned_Point`) and the port carries dead `dBiLineardX1/2` partials with no caller, but NO committed spec scopes opacity differentiation. Do not build; decide first whether it is in-goal, and if yes author a spec before any implementation (single source of truth). Correction to a stale prior note: the oracle DOES define `dTetraLineardX1..4`; only Penta has no derivative counterpart.
- **`/EC_table` group** in the EmAb fixture is still not among the 8 coverage rows — undecided 9th cell vs out of scope (`regression-suite-design.md` is silent). Carried from the prior plan.
- **MPI freedoms** are listed inside their owning Tier-5 items (broadcast mechanics, shared-helper placement, rank-consistency comparison, MPI discovery route, one-config-vs-two for 2/4 ranks) — resolve each ONCE in its increment and record the choice in that item's notes and, where operational, in `CLAUDE.md` Build & run.
- **Resolved by spec — do NOT build:** no combined MPI×GPU CI job; no parallel HDF5; AMReX's communicator is the communicator; node-level table sharing deferred; no dataset chunking for >2^31 elements.

## Discovered since last plan

(empty — the build loop appends new issues here; the next plan run drains it)
