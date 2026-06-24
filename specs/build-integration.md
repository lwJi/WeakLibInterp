# Build & integration (technical, the AMReX dependency contract)

> Technical spec. It fixes what the library and its regression suite build and link against, and pins the correctness-bearing floating-point type independently of the AMReX build configuration. It references `amrex-device-interface.md` (the device surface those builds expose) and `regression-suite-design.md` (the test target this configuration supports), and restates only the build/link facts a fresh agent needs to stand up a compilable, testable tree. `README.md` is canonical if any restated convention here conflicts with it.

## Purpose & scope

This spec defines the build and integration contract: AMReX is a **required** dependency, built **CPU-only, double precision**, for host testing with no GPU; the library and the regression suite both link it; the correctness-bearing value type is pinned to `double` regardless of `amrex::Real`; and there is **no Fortran or Matlab dependency** at build or runtime. It is the single place a fresh agent learns what must be present and configured to compile the device entry points and run the suite.

In scope:
- AMReX as a required build/link dependency of both the library and the regression suite, and where its source is available (the sibling `amrex` repo).
- The required AMReX build configuration for the test target: CPU-only (no CUDA/HIP/SYCL), double precision, host execution; how the qualifier macros and `ParallelFor` collapse to host-only code under it.
- How the library and suite consume AMReX (its headers + the `Gpu::DeviceVector` / `ParallelFor` / qualifier-macro surface from `amrex-device-interface.md`).
- The value-type pin: the correctness-bearing type is `double` (weaklib `dp = 8`) regardless of `amrex::Real`; on-disk reals stay `H5T_NATIVE_DOUBLE`.
- The explicit absence of any Fortran/Matlab build or runtime dependency.

Out of scope:
- The build system itself (CMake vs. GNU Make vs. Meson), target/file layout, and compiler-flag specifics — implementation freedom (see below).
- The HDF5 reader's API choice and on-disk layout (see `table-format-and-io.md`); this spec only states that reading needs the C++ HDF5 library, not Fortran.
- The device interface mechanics (qualifier macros, residency, launch) — see `amrex-device-interface.md`.
- The regression-suite coverage matrix and pass/fail discipline (see `regression-suite-design.md`).
- GPU build/run validation (a GPU build is a drop-in, but is not exercised here).

## Source of truth

The AMReX behavior this contract depends on is the sibling `amrex` repository at the commit recorded alongside the table provenance in `specs/fixtures/tables.provenance`. The header that pins the residency-container and host↔device-copy behavior the build must provide:

- `amrex/Src/Base/AMReX_GpuContainers.H` — `Gpu::DeviceVector<T> = PODVector<T, ArenaAllocator<T>>` and `Gpu::htod_memcpy` / `dtoh_memcpy` / `dtod_memcpy`. Under a **CPU-only** AMReX build these copies collapse to `std::memcpy` and `DeviceVector` allocates ordinary host memory whose `.data()` is a valid host pointer — which is precisely what lets the entire suite run on host with no GPU.

The qualifier macros (`AMReX_GpuQualifiers.H`) and the `ParallelFor` launch overloads (`AMReX_GpuLaunchFunctsG.H`) that the same CPU build collapses to no-ops / sequential loops are the source-of-truth for `amrex-device-interface.md`; this spec depends on them via that spec. The relevant AMReX build configuration is its standard double-precision, GPU-backend-disabled mode (`Gpu::DeviceVector` backed by host memory, `ParallelFor` a sequential loop).

## Inputs & outputs

This spec defines no callable surface. Its "inputs" are the dependencies that must be present to build; its "outputs" are the two link targets — the library and its regression suite — and the configuration both inherit.

### Required dependencies

| Dependency | Role | Required configuration |
|---|---|---|
| AMReX (sibling `amrex` repo) | device interface: `Gpu::DeviceVector<double>` residency, `Gpu::htod_memcpy` upload, `AMREX_GPU_HOST_DEVICE`/`AMREX_FORCE_INLINE` qualifiers, `ParallelFor` launch | **CPU-only** (no CUDA/HIP/SYCL backend), **double precision**, host execution |
| C++ HDF5 library | reading the production `.h5` tables (group/dataset names, shapes, dtypes — see `table-format-and-io.md`) | C++ API; reading needs no Fortran |
| C++ toolchain | compiling the library + suite (C++ standard AMReX requires) | host compiler only |

There is **no Fortran compiler and no Matlab** in the build or at runtime. The weaklib Fortran and the Matlab oracles are read-only sources of truth: the library reimplements weaklib's behavior in C++. See `regression-suite-design.md`.

### Link targets

- **The library** — the correctness-bearing device entry points (`eos-*`, `opacity-*` `_Point` functions and their array `ParallelFor` wrappers). Links AMReX. Compiles for host under the CPU-only AMReX build; a GPU build is a drop-in via the same source (the qualifier macros and `ParallelFor` switch to device forms).
- **The regression suite** — links the library + AMReX (same CPU/double configuration) + the C++ HDF5 library. Runs entirely on host. See `regression-suite-design.md`.

## Correctness requirements

- **AMReX is a required dependency, built CPU-only / double precision.** Both the library and the suite link AMReX in its CPU-only (no GPU backend), double-precision configuration. Under it the device interface (`amrex-device-interface.md`) runs on host: the qualifier macros expand to nothing, `ParallelFor` is a sequential loop, and `Gpu::DeviceVector` allocates host memory. No GPU is required to build or run the suite.
- **Value type pinned to `double`, independent of `amrex::Real`.** The correctness-bearing tables are `Gpu::DeviceVector<double>` and the entry points take `double const*`; the value type is fixed to `double` (= weaklib `dp = 8`) **regardless of how `amrex::Real` is configured**. A single-precision AMReX build (`amrex::Real = float`) must not be able to silently degrade bit-level parity — the interpolator's tables and entry points still use `double`. The on-disk reader contract remains `H5T_NATIVE_DOUBLE` (see `table-format-and-io.md`). This is a requirement the build must enforce, not a property AMReX provides.
- **No Fortran/Matlab build or runtime dependency.** The build pipeline invokes no Fortran compiler and no Matlab; nothing in the library or the suite calls a Fortran/Matlab routine at build time or test time (see `fortran-parity-and-tolerances.md` / `regression-suite-design.md`).
- **The sibling `amrex` repo is the available AMReX source.** The build resolves AMReX from the sibling `amrex` checkout (or an equivalent AMReX install/source the build is pointed at); the headers under `amrex/Src/Base/` named above must be present and compilable in the CPU/double configuration.

## Verification

A fresh agent confirms the build/integration contract is met by these self-contained checks:

1. **CPU-only build succeeds with no GPU.** The library and the regression suite both build and link against AMReX in its CPU-only, double-precision configuration on a host with no GPU toolchain, and the suite runs to completion on host. (This is the same property `amrex-device-interface.md`'s "host/CPU parity needs no GPU" relies on.)
2. **No Fortran/Matlab in the toolchain.** The build completes with only a C++ toolchain + AMReX + C++ HDF5 present; no Fortran compiler or Matlab is invoked at build or test time. Verified by building/running in an environment that has no Fortran/Matlab available.
3. **`double` is preserved under a single-precision AMReX build.** With AMReX configured `amrex::Real = float`, the library's tables and entry points still use `double`, and the machine-precision exactness checks (affine-in-log, node identity) still pass at the `~1e-14` tier — proving the value type is pinned independently of `amrex::Real`.
4. **AMReX source resolves.** The cited `amrex/Src/Base/AMReX_GpuContainers.H` header is present in the sibling `amrex` repo (or wherever the build is pointed) and compiles in the CPU/double configuration.
5. **Mechanical (validator).** `bash specs/tools/validate_specs.sh` (default mode) asserts this file carries the 7 mandated sections in order, names a concrete numeric tolerance, and that its cited `amrex/...` source-of-truth path resolves under `$AMREX_ROOT`.

## Implementation freedom

- The build system (CMake, GNU Make, Meson, …), the target/file layout, and the compiler-flag specifics, provided the required dependencies and configuration above are met.
- How AMReX is located/consumed (a submodule, a sibling-repo path, an installed `amrex` package, `find_package`/`add_subdirectory`), provided the CPU-only / double-precision configuration is used for the test target.
- Which C++ HDF5 binding/API the reader uses (see `table-format-and-io.md`).
- Whether the library is built static or shared, and how the suite's test targets are organized (see `regression-suite-design.md`).
- Whether a GPU build is also offered (it is a drop-in via the same source), provided the CPU/double test target remains the gating build.

## Open questions / assumptions

- **CPU-only build is the test target (assumption, non-blocking).** No GPU is required or assumed in this environment; AMReX is built CPU-only / double precision and the suite runs on host. A GPU build is a drop-in (the qualifier macros and `ParallelFor` overloads switch to device forms), but GPU execution is not verified here. See `amrex-device-interface.md`.
- **`amrex::Real` configuration (assumption, non-blocking).** The contract pins the value type to `double` independently of `amrex::Real`; if an AMReX build sets `amrex::Real = float`, the interpolator's tables and entry points still use `double`. This is a stated requirement the build must enforce, not a property AMReX guarantees.
- **AMReX source location (assumption, non-blocking).** The sibling `amrex` repo is the available source in this environment; a build elsewhere may point at an installed AMReX or a different checkout. The contract is the CPU-only / double-precision configuration and the header surface above, not a specific path.
- **No Fortran/Matlab toolchain (assumption, non-blocking).** This environment has no Fortran/Matlab build capability, and the contract requires none: the library is pure C++/AMReX. See `regression-suite-design.md`.
