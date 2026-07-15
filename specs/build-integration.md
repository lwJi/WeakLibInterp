# Build & integration (technical, the AMReX dependency contract)

> Technical spec. It fixes what the library and its regression suite build and link against, and pins the correctness-bearing floating-point type independently of the AMReX build configuration. It references `amrex-device-interface.md` (the device surface those builds expose) and `regression-suite-design.md` (the test target this configuration supports), and restates only the build/link facts a fresh agent needs to stand up a compilable, testable tree. `README.md` is canonical if any restated convention here conflicts with it.

## Purpose & scope

This spec defines the build and integration contract: AMReX is a **required** dependency, consumed **only as an installed prefix** (found, never built in-tree — provisioning prefixes is a separate out-of-band step), configured **CPU-only, double precision**, for host testing with no GPU; the library and the regression suite both link it; the correctness-bearing value type is pinned to `double` regardless of `amrex::Real`; and there is **no Fortran or Matlab dependency** at build or runtime. The **gating configuration is MPI-enabled** (`AMReX_MPI=ON`, still CPU-only / double precision): the full suite runs under the MPI launcher — including the 2/4-rank tiers — as the default correctness gate, the build shape a rank-per-GPU cluster application (the eventual production consumer) links against; a no-MPI (`serial`) configuration is the second correctness-gating build (its 1-rank runs are the serial cross-check). In addition, the library + suite must **compile** (not run) under AMReX's CUDA and HIP GPU backends — a compile-only gate that keeps the device code honest without requiring GPU hardware. It is the single place a fresh agent learns what must be present and configured to compile the device entry points and run the suite.

In scope:
- AMReX as a required build/link dependency of both the library and the regression suite; the find-only consumption mode (an installed AMReX prefix) and where prefixes come from (an out-of-band provisioner building the sibling `amrex` repo).
- The required AMReX build configuration for the test target: CPU-only (no CUDA/HIP/SYCL), double precision, host execution; how the qualifier macros and `ParallelFor` collapse to host-only code under it.
- The **compile-only GPU checks**: the library + suite must also *compile* under AMReX's CUDA and HIP backends (device compiler, concrete target architecture), proving the device qualifiers and launches are real device code — without any GPU execution; the CPU/double configurations (serial and MPI-enabled) remain the only correctness-bearing (ctest-gating) builds.
- The **MPI-enabled configuration**: AMReX built with `AMReX_MPI=ON` (CPU-only, double precision unchanged) is the **default gating build** — the full suite executed under the MPI launcher, with 2- and 4-rank tiers, including the rank-consistency cells of `regression-suite-design.md` and the root-read + broadcast table distribution of `table-format-and-io.md`. The no-MPI (`serial`) configuration is the second correctness-gating (ctest) build in its own tree; its 1-rank suite is the 1-rank ≡ serial cross-check.
- How the library and suite consume AMReX (its headers + the `Gpu::DeviceVector` / `ParallelFor` / qualifier-macro surface from `amrex-device-interface.md`).
- The value-type pin: the correctness-bearing type is `double` (weaklib `dp = 8`) regardless of `amrex::Real`; on-disk reals stay `H5T_NATIVE_DOUBLE`.
- The explicit absence of any Fortran/Matlab build or runtime dependency.

Out of scope:
- The build system itself (CMake vs. GNU Make vs. Meson), target/file layout, and compiler-flag specifics — implementation freedom (see below).
- The HDF5 reader's API choice and on-disk layout (see `table-format-and-io.md`); this spec only states that reading needs the C++ HDF5 library, not Fortran.
- The device interface mechanics (qualifier macros, residency, launch) — see `amrex-device-interface.md`.
- The regression-suite coverage matrix and pass/fail discipline (see `regression-suite-design.md`).
- GPU *execution* validation (numerical results on a physical GPU are not exercised anywhere; the GPU contract here is compile-only).

## Source of truth

The AMReX behavior this contract depends on is the sibling `amrex` repository at the commit recorded alongside the table provenance in `specs/fixtures/tables.provenance`. The header that pins the residency-container and host↔device-copy behavior the build must provide:

- `amrex/Src/Base/AMReX_GpuContainers.H` — `Gpu::DeviceVector<T> = PODVector<T, ArenaAllocator<T>>` and `Gpu::htod_memcpy` / `dtoh_memcpy` / `dtod_memcpy`. Under a **CPU-only** AMReX build these copies collapse to `std::memcpy` and `DeviceVector` allocates ordinary host memory whose `.data()` is a valid host pointer — which is precisely what lets the entire suite run on host with no GPU.
- `amrex/Src/Base/AMReX_ParallelDescriptor.H` — the communicator/rank surface the MPI-aware table loaders use: `MyProc()`, `IOProcessor()` / `IOProcessorNumber()`, and the templated `Bcast(T*, size_t n, int root)`. Under a non-MPI AMReX build the same calls compile to serial no-ops (one rank, `Bcast` does nothing) — which is what lets the loaders be transparently rank-aware through a single code path (see `table-format-and-io.md`).

The qualifier macros (`AMReX_GpuQualifiers.H`) and the `ParallelFor` launch overloads (`AMReX_GpuLaunchFunctsG.H`) that the same CPU build collapses to no-ops / sequential loops are the source-of-truth for `amrex-device-interface.md`; this spec depends on them via that spec. The relevant AMReX build configuration is its standard double-precision, GPU-backend-disabled mode (`Gpu::DeviceVector` backed by host memory, `ParallelFor` a sequential loop).

## Inputs & outputs

This spec defines no callable surface. Its "inputs" are the dependencies that must be present to build; its "outputs" are the two link targets — the library and its regression suite — and the configuration both inherit.

### Required dependencies

| Dependency | Role | Required configuration |
|---|---|---|
| AMReX (installed prefix, provisioned from the sibling `amrex` repo) | device interface: `Gpu::DeviceVector<double>` residency, `Gpu::htod_memcpy` upload, `AMREX_GPU_HOST_DEVICE`/`AMREX_FORCE_INLINE` qualifiers, `ParallelFor` launch | **CPU-only** (no CUDA/HIP/SYCL backend), **double precision**, host execution |
| C++ HDF5 library | reading the production `.h5` tables (group/dataset names, shapes, dtypes — see `table-format-and-io.md`) | C++ API; reading needs no Fortran |
| C++ toolchain | compiling the library + suite (C++ standard AMReX requires) | host compiler only |
| MPI implementation (OpenMPI / MPICH / Cray MPICH, …) | rank launch + host-side table broadcast via the `ParallelDescriptor` surface (see `table-format-and-io.md`) | required for every `AMReX_MPI=ON` configuration — including the default gating build; only the no-MPI `serial` configuration builds without it |

There is **no Fortran compiler and no Matlab** in the build or at runtime. The weaklib Fortran and the Matlab oracles are read-only sources of truth: the library reimplements weaklib's behavior in C++. See `regression-suite-design.md`.

### Link targets

- **The library** — the correctness-bearing device entry points (`eos-*`, `opacity-*` `_Point` functions and their array `ParallelFor` wrappers). Links AMReX. Compiles for host under the CPU-only AMReX build; a GPU build is a drop-in via the same source (the qualifier macros and `ParallelFor` switch to device forms).
- **The regression suite** — links the library + AMReX (same CPU/double configuration) + the C++ HDF5 library. Runs entirely on host. See `regression-suite-design.md`.

## Correctness requirements

- **AMReX is a required dependency, built CPU-only / double precision.** Both the library and the suite link AMReX in its CPU-only (no GPU backend), double-precision configuration. Under it the device interface (`amrex-device-interface.md`) runs on host: the qualifier macros expand to nothing, `ParallelFor` is a sequential loop, and `Gpu::DeviceVector` allocates host memory. No GPU is required to build or run the suite.
- **Value type pinned to `double`, independent of `amrex::Real`.** The correctness-bearing tables are `Gpu::DeviceVector<double>` and the entry points take `double const*`; the value type is fixed to `double` (= weaklib `dp = 8`) **regardless of how `amrex::Real` is configured**. A single-precision AMReX build (`amrex::Real = float`) must not be able to silently degrade bit-level parity — the interpolator's tables and entry points still use `double`. The on-disk reader contract remains `H5T_NATIVE_DOUBLE` (see `table-format-and-io.md`). This is a requirement the build must enforce, not a property AMReX provides.
- **No Fortran/Matlab build or runtime dependency.** The build pipeline invokes no Fortran compiler and no Matlab; nothing in the library or the suite calls a Fortran/Matlab routine at build time or test time (see `fortran-parity-and-tolerances.md` / `regression-suite-design.md`).
- **AMReX is found, never built in-tree.** The build consumes AMReX exclusively as an installed prefix (`find_package(AMReX CONFIG)`); a missing or invalid prefix is a configure-time hard error naming the exact provisioning command. Provisioning is out-of-band: `tools/provision-amrex.sh <config>` is the single home of the AMReX build recipe, building the sibling `amrex` checkout into the shared store `${WLI_AMREX_HOME:-$HOME/wli-amrex}/<config>`; an existing install plugs in via the `WLI_AMREX_INSTALL_DIR` override, which always wins. The headers under `amrex/Src/Base/` named above must be present in the prefix and compilable in the CPU/double configuration.
- **The library + suite compile under the CUDA and HIP backends (compile-only).** With AMReX configured for the CUDA backend (nvcc, a concrete `AMReX_CUDA_ARCH`) and for the HIP backend (an AMD HIP compiler, a concrete `AMReX_AMD_ARCH`), every library and suite translation unit compiles and links — proving the `AMREX_GPU_HOST_DEVICE`-qualified entry points and the `ParallelFor` launches are valid device code (no host-only calls, no missing qualifiers). Nothing executes under these builds: no GPU hardware is required or assumed, ctest is not run, and the CPU-only/double configurations (serial and MPI-enabled) remain the sole correctness-bearing test targets.
- **The MPI-enabled build is the default and is correctness-gating.** With AMReX configured `AMReX_MPI=ON` (CPU-only, double precision unchanged) — the default gating configuration — the library and suite build against a standard MPI implementation and the **full regression suite passes under the MPI launcher at ≥ 2 ranks**, including the rank-consistency cells (`regression-suite-design.md`) and the root-read + broadcast table distribution (`table-format-and-io.md`). The no-MPI `serial` build is the second correctness-gating configuration in its own tree. The MPI request never guesses: left unset it derives from what the installed prefix records (`AMReX_MPI`), and an explicit request that mismatches the prefix is a configure-time hard error — never a silent degrade. GPU-aware MPI is **not** required: the library performs no communication inside kernels and broadcasts only host memory — each rank uploads its own device copy. A 1-rank MPI run and the serial build produce identical suite results.

## Verification

A fresh agent confirms the build/integration contract is met by these self-contained checks:

1. **CPU-only build succeeds with no GPU.** The library and the regression suite both build and link against AMReX in its CPU-only, double-precision configuration on a host with no GPU toolchain, and the suite runs to completion on host. (This is the same property `amrex-device-interface.md`'s "host/CPU parity needs no GPU" relies on.)
2. **No Fortran/Matlab in the toolchain.** The build completes with only a C++ toolchain + AMReX + C++ HDF5 present; no Fortran compiler or Matlab is invoked at build or test time. Verified by building/running in an environment that has no Fortran/Matlab available.
3. **`double` is preserved under a single-precision AMReX build.** With AMReX configured `amrex::Real = float`, the library's tables and entry points still use `double`, and the machine-precision exactness checks (affine-in-log, node identity) still pass at the `~1e-14` tier — proving the value type is pinned independently of `amrex::Real`.
4. **AMReX source resolves.** The cited `amrex/Src/Base/AMReX_GpuContainers.H` header is present in the sibling `amrex` repo (the provisioner's source input — or whatever install prefix the build is pointed at) and compiles in the CPU/double configuration.
5. **CUDA and HIP compile checks pass.** On a GPU-toolchain host (no GPU hardware needed — e.g. a `nvidia/cuda:*-devel` or `rocm/dev-ubuntu-*` container), the library + full suite build to completion with the CUDA backend (nvcc, concrete arch) and with the HIP backend (AMD HIP compiler, concrete arch). ctest is **not** run under either; pass = clean compile + link.
6. **MPI build + mpirun suite passes (the default gate).** With `AMReX_MPI=ON` — the default gating configuration — on a host with an MPI implementation, the library + suite build and the full ctest suite passes executed under the MPI launcher at 2 and at 4 ranks (launcher flags such as `--oversubscribe` on core-scarce runners are implementation freedom). A failure on any rank fails the suite. Additionally, a 1-rank MPI run agrees with the serial build's results.
7. **Mechanical (validator).** `bash specs/tools/validate_specs.sh` (default mode) asserts this file carries the 7 mandated sections in order, names a concrete numeric tolerance, and that its cited `amrex/...` source-of-truth path resolves under `$AMREX_ROOT`.

## Implementation freedom

- The build system (CMake, GNU Make, Meson, …), the target/file layout, and the compiler-flag specifics, provided the required dependencies and configuration above are met.
- Where installed AMReX prefixes live and how they are provisioned (the store layout, `WLI_AMREX_HOME`, pointing `WLI_AMREX_INSTALL_DIR` at an existing install), provided consumption stays find-only and the CPU-only / double-precision configuration is used for the test target.
- Which C++ HDF5 binding/API the reader uses (see `table-format-and-io.md`).
- Whether the library is built static or shared, and how the suite's test targets are organized (see `regression-suite-design.md`).
- How the CUDA/HIP compile checks are provisioned (which container images, toolkit versions, and target architectures), provided each backend compiles the library + suite with a concrete architecture and the CPU/double test targets remain the gating builds.
- Which MPI implementation and launcher are used (OpenMPI, MPICH, Cray MPICH; `mpiexec`/`mpirun` flags such as `--oversubscribe` or `--allow-run-as-root` in containers) and how the test targets register the launcher (CMake `MPIEXEC_EXECUTABLE`, a wrapper script, …), provided the MPI configuration executes the full suite at ≥ 2 ranks.

## Open questions / assumptions

- **CPU-only builds are the test targets; GPU builds are compile-checked only (assumption, non-blocking).** No GPU hardware is required or assumed in this environment; AMReX is built CPU-only / double precision and the suite runs on host — under the MPI launcher in the default build and serially in the `serial` configuration. The CUDA/HIP backends are verified as compile checks (the qualifier macros and `ParallelFor` overloads switch to device forms and must compile), but GPU *execution* is not verified anywhere. See `amrex-device-interface.md`.
- **Cluster targets are Frontier/Perlmutter-class (assumption, non-blocking).** The eventual production environments are MPI+HIP (OLCF Frontier-class, `gfx90a`) and MPI+CUDA (NERSC Perlmutter-class, `sm_80`) machines — matching the GPU compile-check architecture defaults. Combined MPI×GPU compile checks are **not** part of this contract: the MPI gate is the CPU mpirun suite and the GPU gates stay serial compile-only. Revisit if a real cluster build surfaces an MPI×GPU build interaction.
- **GPU-aware MPI is not required (assumption, non-blocking).** Every rank holds a full host copy of each table and uploads its own device copy; no device pointer is ever passed to an MPI call. Node-level host-memory sharing is a deferred optimization owned by `table-format-and-io.md`'s open questions.
- **`amrex::Real` configuration (assumption, non-blocking).** The contract pins the value type to `double` independently of `amrex::Real`; if an AMReX build sets `amrex::Real = float`, the interpolator's tables and entry points still use `double`. This is a stated requirement the build must enforce, not a property AMReX guarantees.
- **AMReX source location (assumption, non-blocking).** The sibling `amrex` repo is the provisioner's default source in this environment (`AMREX_SRC` overrides); a build elsewhere may point at any existing AMReX install prefix instead. The contract is the CPU-only / double-precision configuration and the header surface above, not a specific path.
- **No Fortran/Matlab toolchain (assumption, non-blocking).** This environment has no Fortran/Matlab build capability, and the contract requires none: the library is pure C++/AMReX. See `regression-suite-design.md`.

## Quiet-runner workflow (`tools/q`)

A workflow contract (not a correctness requirement on the library): every noisy build/test/validate command in the agent loop runs through the committed wrapper `tools/q <command> [args...]`, which keeps command output out of the agent's context unless it is needed.

- **Exit-code passthrough.** The wrapper exits with the child's exit code unchanged — ctest semantics (including `SKIP_RETURN_CODE 77` cells), CI step gating, and `&&`-chaining are unaffected. No arguments is usage + exit 2.
- **Silence on success.** Child exit 0 → the wrapper prints exactly one line, `✓ <command as invoked> (<duration>)`, and nothing else. No warnings, skip counts, or log excerpts are surfaced.
- **Full dump on failure.** Child exit ≠ 0 → the wrapper prints `✗ <command> (exit N, <duration>)` followed by the complete stashed output (stdout+stderr, merged, in order) between delimiters that name the log path.
- **Stash location.** `.scratch/logs/<basename-of-command>.log` (repo-relative; a leading `bash`/`sh` interpreter is skipped when naming), overwritten per invocation and **kept after success** so the last run stays inspectable (e.g. grepping the last passing build for warnings — the accepted blind spot of silence-on-success). `.scratch/` is agent scratch and gitignored; the wrapper must never stash under `.build/`, which is reserved for the Ralph loop's per-iteration state and is wiped at every iteration start.
- **Transparency.** The wrapper does nothing but redirect: no `cd`, no environment manipulation — `WL_TABLES_ROOT=… tools/q ctest …` behaves identically to the unwrapped command.
- **Scope.** The thin drivers (`tools/build.sh`, `tools/test.sh`, `tools/cactus-build.sh`) route their noisy steps through `tools/q` internally; the remaining documented commands (`tools/provision-amrex.sh`, the spec/thorn validators) are `tools/q`-prefixed in `CLAUDE.md` / `docs/BUILD.md`; `.github/workflows/ci.yml` deliberately stays unwrapped (CI log storage is free and full logs aid post-mortems).
