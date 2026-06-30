# TODO — WeakLibInterp implementation plan

Durable, priority-ordered plan that drives the Ralph loop. A *fresh* agent reads this top-to-bottom every iteration. **Plan only lives here; implementation lives in `src/` and `tests/`.**

## Where we are (state snapshot)

- **Greenfield.** All 10 specs (`specs/*.md`) and the spec validator (`specs/tools/validate_specs.sh`) are committed and pass. The pinned weaklib commit `5836de983d2ea1305af31bf971099702759b1889` matches the live `../weaklib` checkout, so every oracle `file:line` citation in the specs resolves.
- **No C++ exists.** There is no `src/`, no build system, no tests, no `TODO.md` history before this one. **Every task below is `new`** (nothing is partial or complete yet).
- **The goal:** GPU-friendly C++ reimplementation of weaklib's EOS + opacity interpolators as AMReX-native device functions; `double`-pinned; bit-for-bit parity with the weaklib Fortran `_Point` oracles within the spec's tolerance tiers.
- **The dominant design constraint:** the same handful of device primitives (bracket+delta, multilinear kernels, column-major index, `10^x - OS` recovery, the 2D2D-aligned inner kernel) are required by *every* channel. They MUST be written once in `src/lib` and reused — see the consolidation map below. Duplicating them per channel is a defect.

## Decisions (sensible defaults; flag in review if you want to override)

These resolve the open questions the research surfaced. The specs grant implementation freedom here; these are the chosen defaults so a fresh agent doesn't re-litigate them each loop.

1. **Build system: CMake.** Greenfield C++; AMReX ships first-class CMake. Wire AMReX via `add_subdirectory(../amrex)` (fallback `find_package(AMReX)`) with `AMReX_GPU_BACKEND=NONE`, `AMReX_PRECISION=DOUBLE`. (`build-integration.md:67`)
2. **Value type: `double`, never `amrex::Real`.** Enforce with a `static_assert` in a core header; do not template entry points on `amrex::Real`. (`build-integration.md:52-53`)
3. **Test framework: hand-rolled minimal harness** (a `main()` aggregating pass/fail, non-zero exit on any failure) in `src/lib/test/`. Avoids an external dep, keeps device/host equivalence cells trivial, satisfies the two-state pass/fail model. GoogleTest/Catch2 acceptable if preferred. (`regression-suite-design.md:106`)
4. **HDF5 binding: HDF5 C API.** Most portable, mirrors weaklib's `h5*_f` calls 1:1, no header-only dependency risk. (`table-format-and-io.md:184`)
5. **Pre-log convention is per-channel and load-bearing.** EOS 3D entry points take **raw** ρ,T and `log10` internally; EmAb/Iso/NES/Pair/Brem entry points take coords **already `log10`'d** by the caller (the Fortran `_Custom` convention). A shared kernel must NOT auto-log. (`opacity-emab-iso.md:50-67`, `eos-interpolation.md:51`)
6. **Reader hands downstream the raw log-stored bytes** (`log10(value+offset)`); the interpolation layer applies `10^x - OS`. Reader does NOT eagerly undo log-storage. (`table-format-and-io.md:188`)
7. **Iso 5D access: in-place moment-stride indexing** (no gather copy) using flat offset `iE + nE*(iMom + nMom*(iD + nρ*(iT + nT*iY)))`. (`opacity-emab-iso.md:133`)
8. **Symmetry fills live in an `src/opacity` assembly layer**, separate from the `_Point` kernel (NES detailed-balance, Pair crossing-symmetry). (`opacity-nes-pair.md:216`)
9. **Max multilinear order needed is 4 (`TetraLinear`).** No pentalinear: Iso slices the moment index to a 4D problem; NES/Pair/Brem reduce to 2D bilinear on aligned sub-tables. (corrects the cross-cutting agent's "PentaLinear" suggestion)

## Shared `src/lib` consolidation map (build these once)

| Primitive | Purpose | Oracle | Consumers |
|---|---|---|---|
| `interp/IndexAndDelta.H` | `GetIndexAndDelta_Lin` / `_Log`: O(1) bracket, **clamp index, do NOT clamp delta** | `wlInterpolationUtilitiesModule.F90:178-209` | every channel |
| `interp/Multilinear.H` | `BiLinear`, `TriLinear`(+`dTriLineardX1/2/3`), `TetraLinear` from unpacked corners+deltas | `wlInterpolationUtilitiesModule.F90:292-392` | EOS, EmAb, Iso |
| `interp/ColMajorIndex.H` | constexpr column-major flat index 1D–5D: `i0+n0*(i1+n1*(…))` | `fortran-parity-and-tolerances.md:122-127` | every channel |
| `interp/LogRecovery.H` | `recover = 10^stored - OS`; `store = log10(v+OS)`; `ln10` constant | `wlInterpolationUtilitiesModule.F90:541` | every channel |
| `interp/Interp3D.H` | trilinear recovery + 3-derivative assembly (chain-rule scale factors) | `…Utilities…:630-660, 912-964` | EOS interp/diff |
| `interp/Interp4D.H` | `LinearInterp4D_4DArray_Point`: 16-corner tetralinear + recovery | `…Utilities…:729-768` | EmAb, Iso |
| `interp/Bilinear2DAligned.H` | `LinearInterp2D_4DArray_2DAligned_Point`: 4-corner bilinear at fixed `(iX1,iX2)` + recovery | `…Utilities…:602-627` | NES, Pair, Brem |
| `table/DeviceTable.H` | `Gpu::DeviceVector<double>` + `htod_memcpy` upload; `.data()→double const*` | `amrex-device-interface.md:42-44` | all readers |
| `table/TableStructs.H` | axis-grid / EOS / opacity / scatter-kernel POD structs (host-side reader outputs) | `table-format-and-io.md:153-155` | all readers |
| `io/Hdf5Reader.H` | C-API read helpers (1D–5D double, int, fixed-width string), `H5Lexists` probe, dtype asserts | `wlIOModuleHDF.F90:38-58` | all readers |
| `test/ToleranceAssert.H` | `\|got-expected\| ≤ rtol·\|expected\|+atol`; NaN-equality; exact-equality; 4 tiers | `fortran-parity-and-tolerances.md:97-109` | all tests |
| `test/SyntheticTable.H` | affine-in-log / constant generators for 3D/4D/5D shapes | `regression-suite-design.md:66` | all tests |
| `test/Harness.H` | register/run cells, aggregate non-zero exit; host+`ParallelFor` device-equivalence helper | `regression-suite-design.md:79,85` | all tests |

**Tolerance tiers** (apply per acceptance criterion): default parity `rtol=1e-12, atol=1e-30`; relaxed `1e-10` (derivatives, inversion-recovered T, round-trip); machine-precision `~1e-14` (affine/constant/node-identity closed forms); exact/NaN-equality (boundary indices, error codes, NaN propagation).

---

## Plan (priority-ordered; later tiers depend on earlier ones)

### P0 — Foundation / critical path (nothing compiles or is testable until these land)

- [ ] **P0.1 Project skeleton + CMake build.** Root `CMakeLists.txt` + `src/` + `tests/`; AMReX wired CPU-only/double (decision 1); `weaklibinterp` library target + `regression_suite` executable + CTest. Discover HDF5 C API. *(build-integration.md:46-48)*
  - **Tests:** clean configure+build succeeds; a one-line smoke `main()` links AMReX and returns 0; CTest registers and runs it.
- [ ] **P0.2 `double` pin guard.** `static_assert` in a core header that entry-point table type is `double const*` and `sizeof(double)==8`, independent of `amrex::Real`. *(build-integration.md:52-53)*
  - **Tests:** build still passes; (manual) configuring AMReX single-precision does not change entry-point signatures.
- [ ] **P0.3 Core device interp primitives** — `src/lib/interp/{IndexAndDelta,Multilinear,ColMajorIndex,LogRecovery}.H`, all `AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE`. *(consolidation map rows 1–4)*
  - **Tests (machine-precision ~1e-14 / exact):** bracket returns clamped index but unclamped delta at and beyond edges; `BiLinear/TriLinear/TetraLinear` reproduce hand-computed corners; `recover(store(v))≈v`; column-major index matches a reference flat-offset computation for 3D/4D/5D.
- [ ] **P0.4 Test harness** — `src/lib/test/{ToleranceAssert,SyntheticTable,Harness}.H`. *(regression-suite-design.md:81-85, 66, 79)*
  - **Tests:** ToleranceAssert passes within-tier / fails out-of-tier and on unexpected NaN; SyntheticTable affine generator reproduces its closed form at sampled nodes; harness returns non-zero when any cell fails; device/host-equivalence helper agrees bitwise on a trivial cell.

### P1 — EOS (first full vertical slice; proves the whole stack end-to-end)

- [ ] **P1.1 `interp/Interp3D.H`** — trilinear recovery + 3-derivative chain-rule assembly. *(map row 5; oracle …Utilities…:630-660, 912-964)*
- [ ] **P1.2 `src/eos/EosInterpolate.H`** — `EosInterpolate_Point(ρ,T,Yₑ,…)` and `EosDifferentiate_Point(…,Deriv[3])`; raw ρ,T logged internally (decision 5). Oracles `wlInterpolationModule.F90:1640` / `:1814`. *(eos-interpolation.md:10-11)*
  - **Tests:** affine-in-log exactness `~1e-14`; node identity on synthetic table; value parity `1e-12/1e-30` and derivative parity `1e-10` vs hand/oracle; boundary extrapolation (clamp index, not delta); NaN on non-positive ρ or T; host==device. *(eos-interpolation.md:114-119)*
- [ ] **P1.3 `src/eos/` inversion** — `InverseLogInterp`, `CheckInputError` (ordered codes 10→11→01→02→03), `ComputeTemperatureWith_{DEY,DPY,DSY}_{Guess,NoGuess}_{Error,NoError}` (template over sub-table), a `EosInversionBounds` POD replacing Fortran module globals, host-only `DescribeEosInversionError` (no device `STOP`). Oracles `wlEOSInversionModule.F90:139-1005`. *(eos-inversion.md:10-12,77-90)*
  - **Tests:** round-trip recover-T at `1e-10` for all three families × Guess/NoGuess; affine-in-log exactness machine-precision; all error codes unit-tested with `T==0` asserted on failure; `_NoError` callers see `T==0` as the sole failure signal; non-monotone/no-root handled. *(eos-inversion.md:148-152)*
  - Inversion needs a 2D `(ρ,Yₑ)`-face bilinear-in-log evaluator at fixed T nodes — implement as a **slice of the 3D kernel**, not a separate copy (flag duplication). *(eos.md open-q 1)*

### P2 — Opacity EmAb + Iso (reuse EOS thermo axes + a shared 4D kernel)

- [ ] **P2.1 `interp/Interp4D.H`** — 16-corner tetralinear recovery. *(map row 6; oracle …Utilities…:729-768)*
- [ ] **P2.2 `src/opacity/EmAb.H`** — `emab_interp_point(LogE,LogD,LogT,Y,…)`; pre-logged E,ρ,T + raw Yₑ (decision 5); 1D `Offsets[nOpacities]`. Oracle `wlInterpolationModule.F90:1754-1779`. *(opacity-emab-iso.md:15-17)*
- [ ] **P2.3 `src/opacity/Iso.H`** — `iso_interp_point(…,iMoment,iSpecies,…)`; select `OS = Offsets[iSpecies + nOpacities*iMoment]` (col-major) then 4D kernel via in-place moment stride (decision 7); 2D `Offsets[nOpacities,nMoments]`; moment index not interpolated. Oracle path `NeutrinoOpacitiesComputationModule.F90:1499-1508`. *(opacity-emab-iso.md:39)*
  - **Tests (both):** affine-in-log exactness `~1e-14`; node identity; **Iso moment-slice independence** (distinct affine per moment, no cross-talk); boundary extrapolation; NaN propagation; host==device; parity `1e-12/1e-30`. *(opacity-emab-iso.md:142-146)*

### P3 — Opacity scattering/production kernels (NES, Pair, Brem; shared 2D2D-aligned inner kernel)

- [ ] **P3.1 `interp/Bilinear2DAligned.H`** — the single shared inner kernel for all three channels; generic `(iX1,iX2)` fixed + `(iY1,iY2,dY1,dY2)` interpolated. *(map row 7; oracle …Utilities…:602-627; axis-order note opacity-kernels.md open-q 5)*
- [ ] **P3.2 `src/opacity/Nes.H`** — `nes_interp_point` bilinear on `(T,η)` (η pre-`log10`, T in **MeV**); lower-triangle `iE'≤iE` loop + `ParallelFor`; detailed-balance upper-triangle fill `Phi(iEp,iE)=Phi(iE,iEp)·exp((E(iE)-E(iEp))/T)` in the assembly layer. Oracles `wlInterpolationModule.F90:1455-1485`, `wlOpacityInterpolationModule.f90:104-115`. *(opacity-nes-pair.md:42-43,150-156)*
- [ ] **P3.3 `src/opacity/Pair.H`** — same `_Point`; crossing-symmetry fill (swap energies AND Ji↔Jii components). Oracle `wlOpacityInterpolationModule.f90:196-228`. *(opacity-nes-pair.md:160-174)*
  - **Tests (NES+Pair):** affine-in-log + node identity + kernel-slice independence `~1e-14`; NES detailed balance and Pair crossing symmetry at `1e-12/1e-30`; boundary extrapolation; NaN; host==device. *(opacity-nes-pair.md, opacity-kernels.md task 11)*
- [ ] **P3.4 `src/opacity/Brem.H`** — `brem_interp_point` summed over 3 effective densities with `Alpha_Brem={1,1,28/3}`, T in **K**; full-plane loop (no symmetry fill, both triangles). Oracles `wlInterpolationModule.F90:1488-1595`, `wlOpacityInterpolationModule.f90:266-272`. *(opacity-brem.md:51,116-117)*
  - **Tests:** affine-in-log + node identity + moment-slice independence; **effective-density decomposition** and weight-order sensitivity at `1e-12/1e-30`; both-triangles-computed; boundary; NaN; host==device. *(opacity-kernels.md task 12)*
  - Enforce the **T-unit split** (NES/Pair MeV vs Brem K) at the API boundary — a wrong-unit call silently corrupts the Boltzmann factor. *(opacity-kernels.md open-q 4)*

### P4 — HDF5 readers + production-table integration (synthetic tests above do NOT need these)

- [ ] **P4.1 `src/lib/io/Hdf5Reader.H` + `src/lib/table/TableStructs.H` + `table/DeviceTable.H`.** C-API read helpers with `H5Lexists` optional-probe and dtype asserts; host structs; DeviceVector upload. *(map rows 8–10; table-io.md tasks 1–2)*
- [ ] **P4.2 Channel readers** — EOS (`/DependentVariables`+`/ThermoState`, **Names-first** ordering, 1D `Offsets{15}`, `i<Name>` slot datasets), EmAb (1D `Offsets{2}`, legacy `/EmAb`→`/EmAb_CorrectedAbsorption` fallback, optional `/EC_table`→sentinel `-1`), Iso (2D `Offsets{2,2}`, geometric `/EnergyGrid` `Zoom`-probe), NES/Pair (shared; `/EtaGrid{120}`, `Kernels{120,81,4,40,40}`, 2D `Offsets{4,1}`), Brem (`S_sigma` not `Kernels`, `{81,185,1,40,40}`, 2D `Offsets{1,1}`, no `/EtaGrid`). Always probe geometric extras, never assume absent. *(table-io.md tasks 3–7; table-format-and-io.md:159-167)*
  - **Tests:** structural conformance — one cell per committed `fixtures/*.h5ls` asserting documented names/shapes/offset-rank (CI-runnable, no real tables); with real tables present: axis lengths (Density 185, T 81, Yₑ 30), flat-offset addressing vs `h5dump` C-order, offset rank per channel, legacy-fallback path. *(table-io.md tasks 8–10)*
- [ ] **P4.3 Production-table guard.** Test driver checks for the 6 `.h5` files from `fixtures/tables.provenance`; absent ⇒ print **`SKIPPED`** for real-table cells (never a silent pass); synthetic cells always run. *(regression-suite-design.md:112-113)*

### P5 — Coverage closure & housekeeping

- [ ] **P5.1 Realize the full coverage matrix** — every public entry point × {in-bounds, on-edge, out-of-range, NaN-input} × {synthetic + named tables}, including a host/device-equivalence cell per entry point. Keep `regression-suite-design.md`'s matrix and the validator's `COVERAGE_ENTRY_POINTS` (`specs/tools/validate_specs.sh:410-417`) in sync by **registry append** if any entry point is added. *(regression-suite-design.md:38-51)*
- [ ] **P5.2 (low) Spec citation fix** — `amrex-device-interface.md:28` cites `AMReX_GpuQualifiers.H` for `AMREX_FORCE_INLINE`; the macro actually lives in `AMReX_Extension.H:86-113`. Correct the citation (validator still passes either way). *(cross-cutting.md open-q 4)*

## Cross-cutting notes for every iteration

- **Reuse before you write.** Before adding any interp/index/recovery code to a channel, check the consolidation map — the primitive almost certainly belongs in `src/lib` and may already exist. Duplication is the main failure mode this plan guards against.
- **Boundary policy is uniform:** clamp the bracket index to `[0,n-2]`, never clamp the fractional delta (out-of-range linearly extrapolates); `log10` of non-positive silently yields NaN; no aborts except the EOS-inversion integer-error-code path (`T=0` on failure).
- **No new specs are required** — the 10-spec set is complete and the validator enforces closure; all gaps are *implementation*, not *specification*. (If a future gap proves otherwise, author `specs/<name>.md` first, then add a task here.)
- **Research artifacts** backing this plan are under `.research/*.md` (one file per slice); regenerate via the planning prompt if specs change.
