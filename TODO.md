# TODO — WeakLibInterp build plan

Greenfield: no `src/`, no `test/`, no build system exists in the tracked tree yet. Goal is a GPU-friendly C++ reimplementation of weaklib's EOS and opacity interpolators exposed as AMReX-native device functions. All 10 acceptance specs are committed and internally consistent; the synthesis (`.research/synthesis.md`) found no missing spec. This plan decomposes the whole effort into dependency-ordered increments — foundations first (`src/lib` shared math + build scaffold), then EOS, then opacity kernels, then HDF5 readers, then the regression suite. Every increment is status **missing/new**; none is partial. List order within each group = build priority.

## Standing facts every item inherits

- **Value type pinned to `double`** on the entire correctness-bearing path, independent of `amrex::Real`. Entry points take `double const*` tables and `double` scalars; residency is `Gpu::DeviceVector<double>`. Never let `amrex::Real` leak into the math. (`build-integration.md:53`, `amrex-device-interface.md:90-96`)
- **Build target:** AMReX **CPU-only** (`AMReX_GPU_BACKEND=NONE`), **double precision** (`AMReX_PRECISION=DOUBLE`), **host execution, no Fortran/Matlab** at build or runtime. AMReX is a required dependency resolved from sibling `../amrex` (env override else `$PARENT/amrex`); probe header `amrex/Src/Base/AMReX_GpuContainers.H`. AMReX forces C++20; `AMReX_MPI` defaults ON and must be forced OFF via `CACHE … FORCE` before `add_subdirectory`. Library + regression suite both link AMReX + C++ HDF5. On RAM-constrained hosts cap parallel build (`-j4`). (`build-integration.md:39,45-48,52-55`)
- **Log-space + offset storage:** tables store `log10(value + offset)`; recovery is `physical = 10**(stored) - offset`. Axis kinds: **log** (ρ, T, E, η), **linear** (Yₑ, pre-`LOG10`'d opacity coords), **not-interpolated** (moment/kernel/species integer axes). NaN propagates naturally when a log argument ≤ 0; no range checks / aborts / guards anywhere on the interpolation path.
- **Bracket/delta:** `GetIndexAndDelta_{Log,Lin}` = `Index1D_*` bracket then `delta = (x - x[i])/(x[i+1]-x[i])`. **Clamp the INDEX** to `[lo, hi-1]` (`MAX(lo,MIN(hi-1,·))`); **do NOT clamp the delta** — that is exactly how edge extrapolation happens. Oracle: `weaklib/Distributions/Library/wlInterpolationUtilitiesModule.F90`, `wlInterpolationModule.F90`.
- **Tolerance tiers** (`fortran-parity-and-tolerances.md:40-88`): default parity `rtol=1e-12, atol=1e-30` (single interpolated value vs named `_Point` oracle); **relaxed `1e-10`** for analytic derivatives and inversion-recovered T; **machine-precision `~1e-14`** for closed-form self-contained invariants. Pass/fail: `|got-expected| <= rtol*|expected| + atol`; assert-and-fail (never print-only). The shared comparator lives in `src/lib` (built in the interpolation-core increment) and is reused by every leaf test.
- **Column-major flat indexing** (`amrex-device-interface.md:64-78`): h5ls prints C-order → reverse to Fortran order. One formula `i0 + n0*(i1 + n1*(i2 + n2*(...)))` for 1D–5D, used identically by kernels and reader. EOS 3D `t[iD + nD*(iT + nT*iY)]`; EmAb 4D `(nE,nρ,nT,nYe)`; opacity 5D e.g. `iEp + nE'*(iE + nE*(k + nMom*(iT + nT*iEta)))`.
- **Device conventions** (`amrex-device-interface.md:36-88`): every `_Point` is `AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE`, scalar, allocation-free, table passed as `double const*` + int extents. `TableData`/`Table1D–4D` **cap at 4D** (no `Table5D`), so 5D opacity channels must use flat `double const*`+extents — use the flat-pointer convention **uniformly, even for 3D EOS**. Residency: host-filled `Gpu::DeviceVector<double>` uploaded once via `Gpu::htod_memcpy`; per-axis metadata via `GpuArray`. Array form = `ParallelFor` wrapper over the `_Point` core (host launch).
- **HDF5 layout** (`table-format-and-io.md`; 6 committed `specs/fixtures/*.h5ls` match every name/shape exactly): read `/DependentVariables/Names` before opening named value arrays; index by name/slot, not fixed integer. EOS `/DependentVariables/Offsets` 1D `[15]`; EmAb `/EmAb/Offsets` 1D `[nOpacities]`; scattering kernels (Iso/NES/Pair/Brem) `Offsets` 2D `[nOpacities,nMoments]`. Shared `/EnergyGrid` (`Values{40}`), `/EtaGrid` (`Values{120}`, NES/Pair only). Fixture shapes (Fortran order): EOS DVs `(30,81,185)=(Yₑ,T,ρ)`; EmAb `(40,185,81,30)`; Iso `(40,2,185,81,30)`; NES/Pair Kernels `(40,40,4,81,120)`; Brem `S_sigma (40,40,1,185,81)`.

## Tier 0 — Foundations (`src/lib` + build scaffold; blocks everything)

- [x] Build scaffold + value-type pin — stand up the CPU-only AMReX+HDF5 build with `src/lib/` and `test/` layout
  - spec: build-integration.md — acceptance source of truth
  - tests: (#1) CPU-only build of library + suite targets succeeds against `../amrex`; (#2) no Fortran/Matlab in toolchain (C++/HDF5-C++ only); (#4) AMReX source resolves from the sibling checkout; (#5) `AMREX_ROOT=../amrex bash specs/tools/validate_specs.sh` passes; `static_assert` that the pinned value type is `double` (not `amrex::Real`).
  - notes: DONE — all five checks pass. Landed: top-level `CMakeLists.txt` (C++20, `project(... CXX)`, force-block `AMReX_GPU_BACKEND=NONE`/`AMReX_PRECISION=DOUBLE`/`AMReX_MPI=OFF`/`AMReX_FORTRAN=OFF`/`AMReX_OMP=OFF`/`AMReX_INSTALL=OFF` + solvers/amrlevel/particles/tiny-profile OFF, all `CACHE … FORCE` before `add_subdirectory(../amrex)`); `src/lib/wli_real.H` pins `wli::Real = double` with `static_assert` (never references `amrex::Real`); `wli_lib` static lib PUBLIC-links `AMReX::amrex`; `wli_tests` links `hdf5::hdf5_cpp` (fallback `${HDF5_CXX_LIBRARIES}`); ctest `scaffold` test. Build dir is `build/` (gitignored); `.build/` stays agent scratch. Do NOT enable `AMReX_HDF5` — find HDF5 independently to avoid AMReX's parallel-HDF5 enforcement. Host toolchain was installed via apt (`cmake`, `g++`, `libhdf5-dev` with C++ bindings at `/usr/include/hdf5/serial/H5Cpp.h`); reinstall if the sandbox is reset. Commands recorded in `CLAUDE.md` Build & run.

- [ ] Column-major flat-index helper (1D–5D) in `src/lib`
  - spec: amrex-device-interface.md — acceptance source of truth (§:64-78)
  - tests: index round-trip — write sentinels via the flat-index formula into a `DeviceVector<double>`, read back through the helper for 3D/4D/5D shapes; exact equality.
  - notes: One templated/variadic `AMREX_GPU_HOST_DEVICE` helper; single source of truth reused by every kernel and the reader. Do not use `TableData` (caps at 4D).

- [ ] Interpolation math core (`src/lib`, header-only, `AMREX_GPU_HOST_DEVICE`) + shared comparator
  - spec: fortran-parity-and-tolerances.md — acceptance source of truth (§:66-88)
  - tests: node identity (query at a node ⇒ index/delta reproduce the stored node exactly); affine-in-log exactness `~1e-14` (interpolate a synthetic affine-in-log table, recover input); clamp-index boundary behavior (out-of-range ⇒ index clamped, delta unclamped/extrapolating); NaN propagation on non-positive log argument.
  - notes: Implements `GetIndexAndDelta_{Log,Lin}` (`:178-209`, `Index1D_{Lin,Log}1/2` `:104-175`), the `Linear/BiLinear/TriLinear/TetraLinear/PentaLinear` basis (`:212-310`) + partials `dTriLineardX1/2/3` (`:313-367`), and the `10**(...)-OS` recovery. Add the shared `rtol/atol` comparator (tier constants `1e-12/1e-30`, `1e-10`, `~1e-14`) here for reuse by every leaf test. **Design decision to make once:** single N-D templated kernel vs named Tri/Tetra/Penta functions (freedom, `fortran-parity-and-tolerances.md:148-152`). Blocks all interpolation kernels.

- [ ] Device table-residency / upload helper + qualifier convention
  - spec: amrex-device-interface.md — acceptance source of truth (§:36-44,80-88)
  - tests: device/host equivalence — `_Point` called directly on host equals the same call inside `ParallelFor` (machine-precision/exact); upload round-trip (host buffer → `htod_memcpy` → read back).
  - notes: Thin dimensionality-agnostic wrapper over `Gpu::DeviceVector<double>` + `Gpu::htod_memcpy`, carrying per-axis metadata (extents, log/lin/not-interp flags, offsets) via `GpuArray` or a small POD. **Pick the metadata-carrying convention once** so all leaf kernels stay consistent. Reused by 3D EOS, 4D EmAb, 5D Iso/NES/Pair/Brem.

## Tier 1 — EOS interpolation & inversion

- [ ] EOS single-point evaluate `_Point` (trilinear-in-log)
  - spec: eos-interpolation.md — acceptance source of truth
  - tests: affine-in-log exactness `~1e-14`; node identity; boundary extrapolation (clamp index not delta); NaN propagation; production-table parity vs oracle `rtol=1e-12,atol=1e-30` (deferred to the regression-suite production cells).
  - notes: Trilinear-in-log with **ρ,T log axes, Yₑ linear**, one dependent variable at a time; table `t[iD + nD*(iT + nT*iY)]`. Oracle `LogInterpolateSingleVariable_3D_Custom_Point` (`wlInterpolationModule.F90:1640-1707`) → `LinearInterp3D_3DArray_Point` (`wlInterpolationUtilitiesModule.F90:630-660`). Reuses the flat-index helper + math core. First full end-to-end path.

- [ ] EOS evaluate-and-differentiate `_Point`
  - spec: eos-interpolation.md — acceptance source of truth
  - tests: derivative chain-rule vs finite-difference, **relaxed `1e-10`** (`∂/∂ρ,∂/∂T,∂/∂Yₑ`); value component identical to the evaluate `_Point`; boundary/NaN as evaluate.
  - notes: Assembles `∂value/∂X_k = (value+OS)·a_k·(∂trilinear/∂d_k)` using partials from the math core and per-axis scale factors (`wlInterpolationModule.F90:1801-1803,1836-1838`). Oracle `LogInterpolateDifferentiateSingleVariable_3D_Custom_Point` (`:1814-1844`) → `LinearInterpDeriv3D_3DArray_Point` (`wlInterpolationUtilitiesModule.F90:912-964`). Shares the evaluate trilinear kernel (evaluate/differentiate code-sharing is freedom, `eos-interpolation.md:127`).

- [ ] EOS inversion core — DEY family (NoGuess + Guess)
  - spec: eos-inversion.md — acceptance source of truth
  - tests: round-trip invariant on recovered T and re-evaluated X, **relaxed `1e-10`** (`:113-128`); affine-in-log inverse `~1e-14`; error-code exact-equality for all codes `{0,01,02,03,10,11,13}` in the mandated cascade order; `T=0` sole failure signal (`_NoError`); highest-T-root (NoGuess) vs nearest-to-guess (Guess) root selection on non-monotone/no-bracket.
  - notes: Algorithm is **table-node interval bisection** (convergence `i_b==i_a+1`, no float tolerance) + one closed-form `InverseLogInterp` (`wlEOSInversionModule.F90:253-267`). Pieces: bounds struct + "initialized" flag (`:169`); `CheckInputError` cascade uninitialized(10)→NaN(11)→D(01)→X(02)→Y(03)→0 (`:188-227`); **2D fixed-T-node bilinear-in-log face eval over `(ρ,Yₑ)`** via `LogInterpolateSingleVariable_2D_Custom_Point` (`wlInterpolationModule.F90:1115-1165`, reuses the math-core BiLinear); NoGuess (`:443-573`, highest-T-root descending scan, `Error=13`); Guess (`:270-440`, guess-cell-first then full-range then nearest-to-guess, `Error=13`). No extrapolation (unlike forward); input-check runs first. weaklib is sole algorithm source (thornado is only a consumer).

- [ ] EOS inversion family generalization (DPY, DSY) + `_Error`/`_NoError` reporting wrappers
  - spec: eos-inversion.md — acceptance source of truth
  - tests: DPY and DSY families reproduce the same round-trip/error-code/`T=0` checks as the DEY core; `_Error` returns the code, `_NoError` signals only via `T=0`.
  - notes: Generalize the DEY core over the three sub-tables E/P/S (+ their offsets) — single templated core vs separate entry points is freedom (`:162`). Add `_Error`/`_NoError` wrappers and the `DEY/DPY/DSY × {Guess,NoGuess} × {Error,NoError}` dispatch matrix (`wlEOSInversionModule.F90:576-1005`, thin per-point). `_Many` array wrapper is out of scope (belongs to the residency/`ParallelFor` layer).

## Tier 2 — Opacity kernels (mutually independent after the math core)

- [ ] EmAb 4D `_Point` (tetralinear-in-log)
  - spec: opacity-emab-iso.md — acceptance source of truth
  - tests: affine-in-log exactness `~1e-14`; node identity; boundary extrapolation; NaN propagation; production parity `rtol=1e-12,atol=1e-30` (via regression suite).
  - notes: Tetralinear-in-log 4D over `(LogE,LogD,LogT,Y)` (caller pre-`LOG10`s E,ρ,T; Yₑ raw). Oracle `LogInterpolateSingleVariable_4D_Custom_Point` (`wlInterpolationModule.F90:1754-1779`) → `LinearInterp4D_4DArray_Point` (`wlInterpolationUtilitiesModule.F90:729-768`). 1D `Offsets[nOpacities]`. Species `iNu_e=1,iNu_e_bar=2` (`wlOpacityFieldsModule.f90:10-13`); pinned tables have `nOpacities=2`. Reuses flat-index helper + math core.

- [ ] Iso 5D-slice-to-4D `_Point`
  - spec: opacity-emab-iso.md — acceptance source of truth
  - tests: moment-slice independence (each fixed `(species,moment)` slice interpolates independently); reuse of the EmAb kernel gives identical affine/node/boundary/NaN behavior.
  - notes: Slice the 5D table `(40,2,185,81,30)` at fixed integer `(species,moment)` (moment **not interpolated**) into a 4D sub-table and call the EmAb kernel; 2D `Offsets[nOpacities,nMoments]` lookup `Offsets(species,moment)`. thornado slice-then-4D-call pattern `NeutrinoOpacitiesComputationModule.F90:1499-1508`.

- [ ] NES/Pair aligned 2D2D bilinear `_Point` (one shared primitive)
  - spec: opacity-nes-pair.md — acceptance source of truth
  - tests: affine-in-log exactness `~1e-14`; node identity; kernel-slice independence; boundary extrapolation; NaN propagation; production parity `rtol=1e-12`.
  - notes: 5D table `(E',E,kernel,T,η)`; bilinear in **`(log10 T, log10 η)`** at fixed integer `(iE',iE,kernel)`; **lower-triangle only `iE'≤iE`**. Oracle `LogInterpolateSingleVariable_2D2D_Custom_Aligned_Point` (`wlInterpolationModule.F90:1455-1485`) → `LinearInterp2D_4DArray_2DAligned_Point` (`wlInterpolationUtilitiesModule.F90:602-627`). 2D `Offsets[4,1]`. Kernel components `iHi0=1,iHii0=2,iHi1=3,iHii1=4`. **NES and Pair are structurally identical here — consolidate one shared primitive**; they differ only in the symmetry-fill increments below. Aligned path (not `LogInterpolateOpacity_2D1D2D`) is the thornado critical path.

- [ ] NES detailed-balance symmetry fill
  - spec: opacity-nes-pair.md — acceptance source of truth (§:150-156)
  - tests: closure check — upper triangle satisfies `Phi(iEp,iE) = Phi(iE,iEp)·exp((E(iE)-E(iEp))/T)` for `iEp>iE`.
  - notes: Post-processing fill of the upper triangle from the shared 2D2D primitive's lower-triangle result. Oracle `wlOpacityInterpolationModule.f90:104-115`. Placement (shared `src/lib` util vs caller) is freedom (`:216`).

- [ ] Pair crossing-symmetry fill
  - spec: opacity-nes-pair.md — acceptance source of truth (§:160-172)
  - tests: closure check — upper triangle transposes energy indices and swaps `Ji↔Jii` kernel components (lower triangle unswapped).
  - notes: Oracle `wlOpacityInterpolationModule.f90:196-228`. Analogous to the NES fill; per-species `C_i/C_ii` weighting is out of the interpolation contract.

- [ ] Brem 5D aligned summed `_Point`
  - spec: opacity-brem.md — acceptance source of truth
  - tests: affine-in-log exactness `~1e-14`; node identity; moment-slice independence; **effective-density decomposition closure** (sum of 3 weighted interps); weight-order sensitivity; **both energy triangles computed independently** (no symmetry fill / no transpose derivation); boundary extrapolation; NaN propagation; production parity `rtol=1e-12`.
  - notes: 5D table `(nE',nE,nMom,nρ,nT)=(40,40,1,185,81)`; bilinear in **`(log10 ρ, log10 T)`** at fixed `(iE',iE,moment)`, evaluated at **three effective densities** and summed with fixed weights `Alpha_Brem=[1, 1, 28/3]`. Oracle `SumLogInterpolateSingleVariable_2D2D_Custom_Aligned` (`wlInterpolationModule.F90:1488-1595`) calling `LinearInterp2D_4DArray_2DAligned_Point` once per density. Effective densities: `log10(D·Xp)`, `log10(D·Xn)`, `log10(D·sqrt(|Xp·Xn|))` (thornado `:2463-2465`). 2D `Offsets[1,1]`; no `/EtaGrid`. Per-point C++ must compute every `(iE',iE)` entry independently (the Fortran triangular loop is an array-form perf detail).

## Tier 3 — HDF5 readers (parallelizable after the build scaffold; feed production-table cells)

- [ ] HDF5 reader: EOS table + shared in-memory table struct
  - spec: table-format-and-io.md — acceptance source of truth
  - tests: structural-conformance self-test — discovered group/dataset names+shapes match `specs/fixtures/wl-EOS-SFHo-15-25-50.h5ls` exactly (runs without the multi-GB table present).
  - notes: Read `/ThermoState` (Density{185},Temperature{81},Electron Fraction{30},LogInterp{3}), `/DependentVariables` (**Names before arrays**, 1D `Offsets[15]`, per-var min/max), `/Metadata`; produce host axis arrays + flat log-stored `double[]` per named DV ready for device upload, keyed by name/slot. Oracle `wlEOSIOModuleHDF.f90:183-213,489-496`, `wlIOModuleHDF.F90:882-1036`. **Decide the shared in-memory table struct (SoA vs flat+extents) once here** (freedom, `:187`) so all channel readers reuse it. Independent of the math/kernel increments; only gates production-table regression cells.

- [ ] HDF5 reader: EnergyGrid/EtaGrid + opacity channels
  - spec: table-format-and-io.md — acceptance source of truth
  - tests: structural conformance against `wl-Op-*-{EmAb,Iso,NES,Pair,Brem}.h5ls` (names, shapes, 1D-vs-2D `Offsets` dimensionality) exactly.
  - notes: Shared `/EnergyGrid` (optional geometric `Zoom/Edge/Width`, keyed off `Zoom` presence) + `/EtaGrid`; per-channel `/EmAb` (1D Offsets, `/EmAb_CorrectedAbsorption` legacy fallback, optional `/EmAb Parameters`,`/EC_table`), `/Scat_Iso_Kernels`, `/Scat_NES_Kernels`, `/Scat_Pair_Kernels`, `/Scat_Brem_Kernels` (2D Offsets). Reuses the EOS-reader in-memory struct. EmAb legacy fallback is implementation-freedom and moot for all pinned fixtures (all have `/EmAb`).

## Tier 4 — Regression suite & precision verification

- [ ] Regression-suite umbrella — coverage matrix + production loader/guard + meta-test
  - spec: regression-suite-design.md — acceptance source of truth
  - tests: coverage-matrix closure — **8 entry points × 4 regimes** (in-bounds / on-edge / out-of-range / NaN-input), each cell at its assigned tier; production-table cells run when the 6 named `.h5` tables are present and **skip distinctly (not fail)** when absent; a **deliberate-perturbation meta-test** proving an injected wrong value fails an assertion; no Fortran/Matlab at test time; `validate_specs.sh` closure gate passes.
  - notes: 8 rows = EOS evaluate, EOS evaluate+differentiate, EOS inversion, EmAb, Iso, NES, Pair, Brem (`regression-suite-design.md:40-51`). Builds synthetic in-memory generators (affine-in-log, constant, symmetry triangles — always-on primary fixtures) and a production-table loader/guard keyed off `specs/fixtures/tables.provenance` (path+sha256; snapshot_sha256 for CI, table_sha256 for refresh). Reuses the `src/lib` comparator. Downstream: assumes all 8 entry points + both readers exist. Framework/layout free (GoogleTest/Catch2/hand-rolled + CTest/custom).

- [ ] Value-type pin verification under single-precision AMReX (follow-on)
  - spec: build-integration.md — acceptance source of truth (§:63)
  - tests: machine-precision self-contained checks still pass with `amrex::Real=float`, proving the `double` pin holds (`~1e-14`).
  - notes: Explicitly deferred by all prior work. May need a second AMReX build config (`AMReX_PRECISION=SINGLE`) or may reduce to a `static_assert`-level structural guarantee — decide scope when reached. Small; trails the suite (needs the machine-precision checks to exist to re-run under single precision).

## Decisions & non-blocking spec items

These are decisions/notes, NOT build increments — the responsible increment resolves each once and reuses it.

- **Spec coverage is complete** — the synthesis confirmed all 10 committed specs cover the surface with verified Fortran-oracle citations; **no new spec is warranted**. Do not author specs on a "missing" basis.
- **In-memory table struct** (SoA vs flat+extents) — freedom (`table-format-and-io.md:187`); decide once in the EOS-reader increment, reuse everywhere.
- **N-D multilinear kernel** — single templated N-D vs named Tri/Tetra/Penta functions; freedom (`fortran-parity-and-tolerances.md:148-152`, `amrex-device-interface.md:107-113`). Decide once in the interpolation-math-core increment.
- **Per-axis metadata carrier** (`GpuArray` vs POD vs separate args) and the `ParallelFor` overload — freedom; pin one convention in the device-residency increment.
- **Inversion dispatch** — single templated core vs separate `DEY/DPY/DSY` entry points; freedom (`eos-inversion.md:162`).
- **EmAb legacy fallback mechanism** — spec allows link-exists probe vs try-open-catch on `/EmAb` else `/EmAb_CorrectedAbsorption`; Fortran actually branches on a parsed `nuclei_EC_table==-1` sentinel (`wlOpacityTableIOModuleHDF.f90:1157`). Moot for all pinned fixtures (all have `/EmAb`). Non-blocking; note for the opacity-channels reader.
- **`/EC_table` group** in the EmAb fixture is not among the 8 coverage rows — undecided whether it is a 9th cell or out of scope (`regression-suite-design.md` is silent; evidence: `wl-Op-…-EmAb.h5ls` carries `/EC_table`, no spec entry-point references it). Decide whether the regression-suite umbrella needs a 9th row when that increment is reached.
- **Build system + target names** — CMake and names like `weaklibinterp`/`weaklibinterp_suite` come only from a stale/orphaned `.build/` cache (a hint, not a spec fact; specs are silent on names). Its design (env-override-then-sibling AMReX probe, `CACHE…FORCE` pinning, C++20, `AMReX_MPI=OFF`, `-j4` OOM cap) is reusable as a starting point for the build scaffold, but the files are gone — re-derive/re-verify from spec. Choose freely.
- **Test framework** (GoogleTest/Catch2/hand-rolled) and CTest-vs-custom harness — freedom (`regression-suite-design.md:104-109`).
