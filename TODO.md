# TODO — WeakLibInterp build plan

Current state: the core mission is substantially complete and green — all 8 device entry-point families, their 15 `_Point` kernels, the 15 host `ParallelFor` array wrappers, the shared math core, all six HDF5 readers, the MPI root-read/broadcast path, and the full regression suite (26 default-tree tests, 73 in `build-mpi/`) are implemented and spec-traced; the prior plan's Tiers 5-6 are fully drained. This cycle's work: `specs/cactus-integration.md` (committed at tip `2e9bd21`, entirely unimplemented — Tier 7), the newly authored `specs/opacity-differentiate.md` (spec-author dispatched this plan run; validator green at 12 registered specs — Tier 8), and residual coverage/parity reconciliation the re-audit surfaced (Tiers 9-10). List order within each tier = priority; Tier 7 items 1-3 are independent, item 4 depends on 1-3, item 5 depends on 1-3.

## Standing facts every item inherits

- **Value type pinned to `double`** on the entire correctness-bearing path (`src/core/wli_real.H`, `wli::Real`), independent of `amrex::Real`. Never let `amrex::Real` leak into the math.
- **Every new CMake knob is `WLI_`-prefixed and defaults to current behavior** — the default `cmake -S . -B build` tree must stay byte-unchanged (26 tests) for every Tier 7 switch, exactly as `WLI_AMREX_MPI`/`WLI_AMREX_PRECISION`/`WLI_GPU_BACKEND` already do.
- **Tolerance tiers** (`src/core/wli_compare.H`): parity `rtol=1e-12, atol=1e-30`; relaxed `1e-10`; machine `~1e-14`; rank-consistency cells bitwise-exact. `specs/opacity-differentiate.md` pins value parity at the parity tier and all derivative checks at relaxed `1e-10`.
- **Spec validation must stay green:** `AMREX_ROOT=../amrex bash specs/tools/validate_specs.sh` — now asserts exactly 12 registered specs (README index + `REGISTERED_SPECS` array both updated this plan run) plus per-spec section/tolerance/source-path gates on `opacity-differentiate.md`.
- **Sandbox limits:** `WL_TABLES_ROOT` unset (all production-table cells SKIP 77 — live-tier claims end in "confirm empirically once available"); `build-mpi/` and the GPU trees are CI-verified, not local; NO Cactus/ET checkout exists (only `../amrex`, `../weaklib`, `../thornado`) — Cactus Verification #5 is explicitly manual/environment-gated (`cactus-integration.md:79`).
- Cap parallel builds at `-j4` (uncapped AMReX builds OOM this host). After adding a new test target to `test/CMakeLists.txt`, re-run the configure step before building.

## Tier 7 — Cactus integration (`specs/cactus-integration.md`)

- [x] Installed-AMReX consumption mode + configuration-consistency guard
  - spec: specs/cactus-integration.md — acceptance source of truth (§:53-58,62-63,70,76)
  - tests: (a) configure this repo in installed-AMReX mode against a CPU-only/double AMReX install prefix succeeds and the library + suite build (Verification #1's "installed-AMReX rehearsal … standing CI proxy", spec:75); (b) guard-trips negative test (Verification #2, spec:76): configure against a CPU-backend install prefix while requesting `WLI_GPU_BACKEND=CUDA` fails at configure time naming both values. Both are configure/build-level checks, not ctest cells.
  - notes: DONE (CMake-only, root `CMakeLists.txt`). Knob is `WLI_AMREX_INSTALL_DIR` (CACHE PATH, empty default = sibling-source mode unchanged; `WLI_AMREX_ROOT` still the source path — not overloaded). Installed branch: `find_package(AMReX REQUIRED CONFIG PATHS ...)`, then the consistency guard, then `enable_language(CUDA)` + bare `include(AMReXTargetHelpers)` (the install prepends its module dir to `CMAKE_MODULE_PATH`; guard-before-enable_language ordering is load-bearing so V#2 trips with the guard's diagnostic, not "no CUDA compiler"). Guard covers GPU backend + MPI only — precision is spec-exempt (spec:63; `wli::Real` pinned to double regardless), narrower than this item's original paraphrase. All three checks passed locally: installed-mode ctest 26 green, CUDA-vs-NONE guard trips naming both values, default `build/` tree unchanged (26 green). Scratch install left at `.build/amrex-installed/prefix` for this session only — .build/ is wiped each iteration; the CI-job item is the standing harness and must rebuild the prefix itself. Caveat learned: a stale `build/` cache from another OS (macOS `mpicxx` path) fails configure — `rm -rf build` first when that happens.

- [ ] Library-only / tests-off CMake switch
  - spec: specs/cactus-integration.md (§:56,77)
  - tests: configure with the switch ON produces no `test_*` ctest targets and still builds `wli_lib`; configure with it OFF/absent is byte-for-byte the current behavior (all existing targets present). Configure-level check.
  - notes: `CMakeLists.txt:134` unconditionally runs `add_subdirectory(test)`; no guard exists. Smallest, fully independent increment.

- [ ] `install()` rules for `wli_lib` + all public headers, preserving the flat include convention
  - spec: specs/cactus-integration.md (§:50,57,77)
  - tests: Verification #3 (spec:77) — a scratch translation unit including a public `wli_*.H` and calling a `_Point` kernel compiles and links against the installed prefix alone (no source tree). Build/link-level check, naturally a CI step.
  - notes: no `install(TARGETS|FILES|DIRECTORY)` call exists anywhere (root/src/test CMakeLists all checked). Install every public `wli_*.H` under `src/{core,eos,opacity,io}/` such that `#include "wli_eos.H"` still resolves flat post-install. Independent of the two items above (can install against the current sibling-source build).

- [ ] CI job: installed-AMReX rehearsal (the standing thorn-build-mode proxy)
  - spec: specs/cactus-integration.md (§:75-77, Verification #1-#3)
  - tests: `.github/workflows/ci.yml` gains a 6th job: build+install AMReX CPU-only/double into a prefix, configure this repo in installed-AMReX mode, build library + full suite, run ctest with unchanged tolerance tiers, plus the config-guard-trips negative check and the installed-prefix scratch-TU compile — green on push.
  - notes: existing 5 jobs are `validate-specs`, `build-test`, `build-test-mpi`, `build-cuda`, `build-rocm`; mirror the GPU jobs' install-first pattern and ccache keying (`AMREX_REF`-keyed, distinct config key). Depends on the three items above. On-push green is the final signal; actionlint unavailable in-sandbox.

- [ ] Author the `WeakLibInterp` ExternalLibraries-style thorn
  - spec: specs/cactus-integration.md (§:9-13,25,39-51,84)
  - tests: structural verification only — `configuration.ccl` (`PROVIDES WeakLibInterp`, emitting `INCLUDE_DIRECTORY`/`LIBRARY_DIRECTORY`/`LIBRARY`), `src/detect.sh` (find-or-build keyed on `WEAKLIBINTERP_DIR`), `src/build.sh` (drives this repo's own CMake into `${SCRATCH_BUILD}/external/...` or `WEAKLIBINTERP_INSTALL_DIR`), `src/make.code.deps`, following the `ExternalLibraries-AMReX` pattern. Verification #5 (in-Cactus acceptance) is explicitly manual/environment-gated (spec:79) and cannot run in this repo's ctest.
  - notes: no `cactus/`/`thorns/` directory exists. Path is implementation freedom; spec example is `cactus/thorns/WeakLibInterp/`. Lowest Tier-7 priority because it is structurally-verified only until a real Cactus build exercises it; depends on the CMake surface from items 1-3. Once built, the next plan run adds a `CLAUDE.md` Layout bullet (orchestrator-owned — do not edit CLAUDE.md from the build loop).

## Tier 8 — Opacity evaluate-and-differentiate (`specs/opacity-differentiate.md`, authored this plan run)

- [ ] EmAb/Iso 4D evaluate-and-differentiate `_Point` kernels
  - spec: specs/opacity-differentiate.md — acceptance source of truth (§:51-57,77-134; Verification #1,2,4,6,7)
  - tests: value parity with the evaluate leaf at the parity tier (`1e-12`/`1e-30` — the derivative path must not perturb the value); affine-in-log closed-form derivative check at relaxed `1e-10` on a synthetic table with a distinct affine offset per slice (wrong slice / transposed offset caught), at arbitrary interior queries, not just nodes; log-axis vs linear-`Ye` scale-factor check (`1/(X·log10(node ratio))` vs `ln10/(node spacing)`); boundary cell — derivative of the edge cell's linear extrapolation, clamp-index-but-not-delta (exact relation); NaN propagation into `Interpolant` and every partial on non-positive `E`/`rho`/`T`.
  - notes: no named 4D Fortran differentiate wrapper exists — assemble by pairing the value leaf's bracket/delta (`LogInterpolateSingleVariable_4D_Custom_Point`) with the `LinearInterpDeriv4D_4DArray_Point` core + `dTetraLineardX1..4` scale factors, the identical construction weaklib uses for the 3D EOS differentiate (spec §Open questions pins this as non-blocking). `dTetraLineardX1..4` do not exist in `src/core/wli_interp.H` yet (only `dTriLineardX1/2/3` are live). Whether Iso reuses the EmAb 4D routine on its `(species, moment)` slice is implementation freedom (spec:158).

- [ ] NES/Pair 2D-aligned evaluate-and-differentiate `_Point` kernel
  - spec: specs/opacity-differentiate.md (§:58-64; oracle `LogInterpolateDifferentiateSingleVariable_2D2D_Custom_Aligned`, `LinearInterpDeriv2D_4DArray_2DAligned_Point`)
  - tests: same check families as the EmAb item — value parity (parity tier), affine-in-log closed form per `(iEp,iE,kernel)` slice (relaxed), scale factors, boundary extrapolation (exact), NaN propagation into value + `(∂/∂T, ∂/∂eta)` — per the spec's Verification #1,2,4,6,7.
  - notes: this increment finally wires the dead `dBiLineardX1`/`dBiLineardX2` (`src/core/wli_interp.H:159-168`) into a live consumer — resolves the dBiLineardX half of the orphaned-constructs hygiene item in Tier 10. Channel-neutral like the value leaf; `iEp/iE/kernel` are discrete indices.

- [ ] Brem per-effective-density + summed evaluate-and-differentiate
  - spec: specs/opacity-differentiate.md (§:65-72,104-113; Verification #5,6,7)
  - tests: per-plane derivatives equal the 2D-aligned core evaluated at each `rho_l`; `∂SumInterp/∂T = Σ_l Alpha(l)·∂Interp_l/∂T` asserted at the parity tier (exact linear combination) on a synthetic Brem kernel; boundary + NaN cells.
  - notes: no Fortran `SumLogInterpolateDifferentiate` wrapper exists — the summed relations are exact by linearity (spec §Open questions, non-blocking); `∂SumInterp/∂rho` at fixed composition needs caller-supplied `c_l = xp, xn, sqrt(xp·xn)`, consistent with the value-only Brem contract. Axis order is rho-then-T (opposite of NES/Pair — known silent transpose hazard).

- [ ] Array-form wrappers + guarded real-table FD cross-check cells for the new derivative kernels
  - spec: specs/opacity-differentiate.md (Verification #3; §:163 — array form as hand loop or ParallelFor over the `_Point` core); amrex-device-interface.md (§:80-88,100-102)
  - tests: array form produces per-element results bit-identical to a serial loop of the `_Point` core (extend `test/test_parallelfor_wrappers.cpp`); guarded `test/test_production_tables.cpp` cells comparing each returned partial against a tight central finite difference of the value-only leaf at relaxed `1e-10` on the real EmAb/Iso/NES/Pair/Brem tables (SKIP 77 when `WL_TABLES_ROOT` unset).
  - notes: mirror the 15 existing wrappers' pinned shape — `wli::TableView<D>` by value, extents from `view.n[i]`, axis grids/OS/discrete selectors as batch-shared scalars, one allocation-free `amrex::ParallelFor`. Host-only-capture bugs remain guarded only by the compile-only CUDA/HIP CI jobs.

## Tier 9 — Coverage & parity reconciliation

- [ ] Reconcile production-table node-identity tolerance tier (spec vs test divergence)
  - spec: fortran-parity-and-tolerances.md (§:109) + leaf specs eos-interpolation.md:115, opacity-emab-iso.md:143, opacity-nes-pair.md:201, opacity-brem.md:205 — all state node identity = machine-precision tier, verified on the real table too
  - tests: the reconciled `test_production_tables.cpp` node-identity cells pass at the chosen tier against a real `WL_TABLES_ROOT` table (currently unset in this sandbox → cells SKIP 77; confirm empirically before committing the tier choice). Test-only OR spec-only edit; no `src/` change.
  - notes: `test/test_production_tables.cpp` node-identity assertions use `wli::rtol_relaxed` (1e-10) at 6 call sites: `:103` (EOS), `:214` (EmAb), `:258` (Iso), `:319` (NES/Pair), `:368` (Brem). `TODO.md`'s prior cycle recorded the relaxed choice as a build-time decision, but that is plan state, not a spec — `fortran-parity-and-tolerances.md:109` requires a relaxed tier be invoked only with a stated rationale in the leaf spec itself. Fix is EITHER (a) tighten the 6 calls to `wli::rtol_machine`, OR (b) add a rationale-bearing carve-out to the 4 leaf specs. Preferred sequencing: once `WL_TABLES_ROOT` is available, empirically test whether `rtol_machine` actually holds on real data first — if it does, tighten (a); only pursue the spec carve-out (b) if real transcendental error genuinely exceeds the 1e-14 band, and route that spec edit through the plan loop (record in Discovered), not the build loop.

- [ ] EOS differentiate kernel real-table FD cross-check
  - spec: eos-interpolation.md (§:116) — "Cross-check against a tight central finite-difference at the relaxed tier on the real table"
  - tests: central-FD-vs-analytic-derivative agreement at `rtol_relaxed` on `wl-EOS-SFHo-15-25-50.h5`, under the same `WL_TABLES_ROOT` SKIP-77 guard. Test-only, no `src/` change.
  - notes: `run_eos` (`test/test_production_tables.cpp:77-115`) only calls `EosInterpolateSingleVariable3DPoint` (evaluate); it never calls `EosInterpolateDifferentiateSingleVariable3DPoint`, and no real-table FD cross-check exists anywhere (only the synthetic non-affine FD check at `test/test_eos_diff_point.cpp:124-146`). Add a cell inside `run_eos()` mirroring the synthetic pattern. Natural pairing: same file as the Tier 8 FD item — coordinate if built in the same iteration.

- [ ] Exercise the `/EmAb_CorrectedAbsorption` legacy fallback
  - spec: table-format-and-io.md (§:182, Verification #4) — explicitly calls for a synthetic/legacy fixture lacking `/EmAb`
  - tests: a synthetic EmAb fixture written under `/EmAb_CorrectedAbsorption` only (no `/EmAb`) reads successfully and asserts `t.usedLegacyGroup == true`. Test-only.
  - notes: the fallback is implemented (`src/io/wli_io_opacity.cpp:152-157`, sets `t.usedLegacyGroup`) and even digested (`test/wli_rank_digest.H:116`), but no test constructs a fixture lacking `/EmAb` — `test/test_rank_consistency.cpp:210-213` (`write_synthetic_emab`) always writes `/EmAb`; grep for `EmAb_CorrectedAbsorption` across `test/` = 0 hits. The branch is dead from the suite's perspective.

- [ ] Extend the EOS rank digest to all 15 `HostDVIndices`
  - spec: table-format-and-io.md (§:183) — digest must fold "the extents, offsets, and names" of every loaded array
  - tests: the existing `test_rank_consistency.cpp` load/corrupt modes now catch a corruption injected into any of the previously-unfolded 12 indices (extend the corrupt positive-control to cover one). Test-only (`test/wli_rank_digest.H` edit).
  - notes: `digest(HostEosTable)` (`test/wli_rank_digest.H:92-111`) folds only 3 of 15 `HostDVIndices` members (`iPressure`, `iEntropyPerBaryon`, `iGamma1` at `:106-108`), though `bcast_eos_table` (`src/io/wli_io_eos.cpp:166-198`) broadcasts all 15 as a fixed `std::array<int,15>` — a divergence in the other 12 role→slot indices would go uncaught. Multi-rank verification is CI-side (`build-mpi/` not present locally); the serial tree's plain registrations SKIP 77 by design.

## Tier 10 — Hygiene

- [ ] Fix stale "no HDF5 reader / DEFERRED" comments in four test headers
  - spec: regression-suite-design.md (comment hygiene; no behavior change)
  - tests: none — comment-only; existing suite stays green.
  - notes: `test/test_nes_pair_point.cpp:29-30`, `test/test_nes_detailed_balance.cpp:21-22`, `test/test_pair_crossing_symmetry.cpp:30-31`, `test/test_brem_point.cpp:39-42` claim the NES/Pair/Brem HDF5 readers don't exist yet — now false (readers live in `src/io/wli_io_opacity.{H,cpp}`, exercised by `test_production_tables.cpp`). Update to forward-reference `run_nespair`/`run_brem`.

- [ ] Resolve the `TableMeta<D>`/`AxisKind` orphaned-metadata decision
  - spec: amrex-device-interface.md + CLAUDE.md no-placeholder discipline
  - tests: if wired — the new consumer's own test exercises the metadata path; if removed — existing suite still green (removal is invisible).
  - notes: `TableMeta<D>`/`AxisKind` (`src/core/wli_table.H:36,48-61`) are only self-tested by `test_table_residency.cpp`; every live kernel threads raw `Real const*` + int extents instead (`TableView<D>` IS used by the array wrappers, so only the metadata half is orphaned). The `dBiLineardX1/2` half of the original hygiene finding is resolved by the Tier 8 NES/Pair item — do that first. For TableMeta: either give it a real consumer (the Tier 8 wrappers or the Cactus-facing surface are candidates) or remove/document as intentional convention.

## Decisions & non-blocking spec items

These are decisions/notes, NOT build increments — the responsible increment resolves each once and reuses it.

- **Coverage-matrix registration of the new derivative entry points** — `opacity-differentiate.md` §Mechanical explicitly defers referencing its cores in `regression-suite-design.md`'s coverage matrix (a spec edit). Once the Tier 8 kernels exist, the next plan run decides whether they become coverage rows and dispatches a spec-author on `regression-suite-design.md` — the build loop never hand-edits specs.
- **`/EC_table` group** in the EmAb fixture is still not among the 8 coverage rows — undecided 9th cell vs out of scope (`regression-suite-design.md` is silent; `HostEmAbTable::hasECTable` is read and digested but has no correctness cell). Carried from the prior plan.
- **Node-identity carve-out** (option (b) of the Tier 9 reconcile item) is a spec repair — if the build loop determines the machine tier cannot hold on real data, it records that in Discovered with the measured error and the plan loop dispatches the spec-author; it does not edit the four leaf specs itself.
- **Small polish, flag-only:** `wli::wli_value_type_size()` (`src/core/wli.cpp:12`) is an unused link-anchor (`test_scaffold.cpp` re-derives `sizeof(wli::Real)==8` locally); no shared helper derives `EosInversionBounds` from a loaded table — spec leaves bounds representation free (`eos-inversion.md:163,169`), every caller hand-builds by scanning sub-table extents. Neither is mandated; touch only if an increment is already in the area (anti-duplication discipline).
- **Resolved by spec — do NOT build:** chunked broadcast for datasets > INT_MAX elements (`table-format-and-io.md:171`, all pinned tables far below the limit); no external Fortran numeric fixture (self-contained scheme is by design, `fortran-parity-and-tolerances.md:135-142`); Cactus Verification #5 / official ET packaging / HDF5 C-API port / consumer thorn / ctest-inside-Cactus (`cactus-integration.md:15-19,91-98`); no combined MPI×GPU CI job; no parallel HDF5.

## Discovered since last plan

(empty — the build loop appends new issues here; the next plan run drains it)
