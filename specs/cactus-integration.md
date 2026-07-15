# Cactus / CarpetX integration (technical, the Einstein Toolkit delivery contract)

> Technical spec. It fixes how the library is delivered into a Cactus / Einstein Toolkit (ET) configuration driven by the CarpetX driver (which is itself built on AMReX): an ExternalLibraries-style wrapper thorn hosted in this repository that builds the library with the repo's own CMake against the AMReX installation the ET `AMReX` thorn provides, plus the upstream CMake capabilities that thorn requires. It references `build-integration.md` (the AMReX dependency contract this extends), `amrex-device-interface.md` (the header-inline device surface consumers compile), and `table-format-and-io.md` (the loader behavior that must survive inside a Cactus run). `README.md` is canonical if any restated convention here conflicts with it.

## Purpose & scope

This spec defines the Cactus delivery contract: a thorn named `WeakLibInterp`, hosted inside this repository, following the ET **ExternalLibraries** pattern — a `configuration.ccl` `PROVIDES WeakLibInterp` block with a detect script, a find-or-build protocol keyed on `WEAKLIBINTERP_DIR`, a build script that drives **this repo's own CMake** (never a second build description), and the standard Cactus `INCLUDE_DIRECTORY` / `LIBRARY_DIRECTORY` / `LIBRARY` emissions so a consumer thorn needs only `REQUIRES WeakLibInterp`. Inside a Cactus configuration the library is built against the AMReX **installation** provided by the ET `AMReX` thorn — the same AMReX CarpetX links — via the installed-AMReX consumption (`find_package(AMReX)`) that is this repo's sole CMake mode (`build-integration.md`).

In scope:
- The wrapper thorn: layout-independent behavior (provides/requires, option variables, find-or-build semantics, emissions), hosted in this repo so the thorn is versioned with the library and builds from the checkout with zero source drift.
- The upstream CMake surface the thorn needs: installed-AMReX consumption via `find_package(AMReX)`, a library-only (tests-off) mode, install rules for the library + all public headers, and a configure-time configuration-consistency guard.
- The dependency story inside Cactus: exactly one AMReX per executable (the ET AMReX thorn's install), HDF5 from the ET HDF5 thorn (C and C++ APIs), MPI from the MPI thorn.
- What consumer thorns may rely on: flat `#include "wli_eos.H"`-style includes, header-inline device entry points they compile with their own (possibly CUDA/HIP) compiler, and the host-side loader/`ParallelFor`-wrapper objects from the installed library.

Out of scope:
- Official ET distribution packaging (a `dist/` tarball, licensing/documentation review) — deferred; this contract is the group-use, build-from-checkout mode (see Open questions).
- The consumer thorn itself (scheduling, physics, where table loads are scheduled) — only the ordering constraint it must honor is stated here.
- Running the regression suite inside Cactus: this repo's ctest configurations (`build-integration.md`, `regression-suite-design.md`) remain the sole correctness gates; the Cactus build is a delivery vehicle, not a test target.
- GPU execution validation (unchanged from `build-integration.md`: GPU backends are compile-checked, never executed, in this project's gates).

## Source of truth

Pattern exemplars (read-only sibling checkouts; cited by mechanism, not vendored):

- `einsteintoolkit/ExternalLibraries-AMReX` — the canonical ExternalLibraries mechanism this thorn replicates: `configuration.ccl` declaring `PROVIDES AMReX { SCRIPT src/detect.sh; OPTIONS AMREX_DIR ... }`; `src/detect.sh` deciding find-vs-build from `AMREX_DIR` (unset or `BUILD` → build bundled source, a path → detect a pre-installed tree via `find_lib`) and emitting `INCLUDE_DIRECTORY` / `LIBRARY_DIRECTORY` / `LIBRARY`; `src/build.sh` driving the library's native CMake into `${SCRATCH_BUILD}/external/<Thorn>`; `src/make.code.deps` gating a `done/<Thorn>` stamp on the build script; the GPU option pass-through (`AMREX_ENABLE_CUDA`, `AMREX_ENABLE_HIP`, `AMREX_CMAKE_CUDA_ARCHITECTURES`, `AMREX_AMD_ARCH`) and the consumer-side compiler switch — under `AMREX_ENABLE_CUDA=yes` its detect script forces every other AMReX-consuming thorn to compile with `CXX = $(CUCC)`.
- `RePrimAnd/ET_interface/thorns/RePrimAnd` — the in-library-repo thorn-hosting model this spec adopts (thorn versioned with the library, same repo checked out by GetComponents), and the staleness failure mode this contract eliminates: RePrimAnd builds from a manually re-tarred `dist/RePrimAnd.tar` snapshot its own README admits can lag; here the thorn builds from the live checkout instead.
- `einsteintoolkit/ExternalLibraries-HDF5` — the HDF5 provider: its bundled build enables the C++ API by default (`HDF5_ENABLE_CXX:=yes` → `--enable-cxx`), and its detection searches for and exposes `hdf5_cpp` / `hdf5_hl_cpp` in `HDF5_LIBS`, so the reader's C+C++ HDF5 requirement (`table-format-and-io.md`) is satisfiable by that thorn.

AMReX installed-package surface (sibling `amrex` repo, pinned per `fixtures/tables.provenance`):

- `amrex/Tools/CMake/AMReXConfig.cmake.in` — the installed `AMReXConfig.cmake` records the build's configuration as package variables (`AMReX_GPU_BACKEND`, `AMReX_PRECISION`, `AMReX_MPI`), exports the `AMReX::` targets, and prepends the installed CMake-module directory to `CMAKE_MODULE_PATH`.
- `amrex/Tools/CMake/AMReXInstallHelpers.cmake` — `make install` ships the entire `Tools/CMake/` module directory (including `AMReXTargetHelpers.cmake`, i.e. `setup_target_for_cuda_compilation`) into the prefix and creates the legacy `libamrex` symlink the ET thorn's `LIBRARY amrex` emission relies on. Everything a source-tree `add_subdirectory` consumer would obtain is therefore present in an install prefix.
- `amrex/Src/Base/AMReX_GpuContainers.H` and `amrex/Src/Base/AMReX_ParallelDescriptor.H` — the device-residency and rank/broadcast surfaces (`build-integration.md`) consumed unchanged inside a Cactus run; `amrex::Initialize` is the CarpetX driver's job, and the loaders' root-read + broadcast path (`table-format-and-io.md`) requires it to have run first.

## Inputs & outputs

This spec defines no callable surface. Its inputs are a Cactus configuration; its outputs are the thorn's emissions plus the upstream CMake modes the thorn consumes.

### Thorn inputs (thorn list + option-list variables)

| Input | Meaning |
|---|---|
| thorn list | `WeakLibInterp` alongside the ET `AMReX`, `HDF5`, and `MPI` thorns (a CarpetX-style configuration) |
| `WEAKLIBINTERP_DIR` | unset or `BUILD` → build the library from this repo's checkout; a path → use that pre-installed prefix (detect-only, no build) |
| `WEAKLIBINTERP_INSTALL_DIR` | optional override of the install prefix (default `${SCRATCH_BUILD}/external/WeakLibInterp`) |
| AMReX thorn options | `AMREX_DIR`, `AMREX_ENABLE_CUDA`, `AMREX_ENABLE_HIP`, and the architecture variables — **read, never redefined**, by this thorn; they select the one AMReX install everything links |

### Thorn outputs

- An installed prefix: `include/` carrying every public `wli_*.H` header such that the repo's flat include convention (`#include "wli_eos.H"`) compiles for consumers, and `lib/` carrying the library.
- The Cactus emissions `INCLUDE_DIRECTORY` / `LIBRARY_DIRECTORY` / `LIBRARY`, so a consumer thorn's whole obligation is `REQUIRES WeakLibInterp` in its `configuration.ccl`.

### Required upstream CMake surface (this repo)

- **Installed-AMReX consumption:** configure against an AMReX install prefix via `find_package(AMReX)` — the repo's only consumption mode; the thorn hands it the ET AMReX thorn's install directly.
- **Library-only mode:** a switch that skips configuring the test suite, so the thorn builds just the library.
- **Install rules** for the library and all public headers.
- **The configuration-consistency guard** (Correctness requirements).

## Correctness requirements

- **Exactly one AMReX per executable.** The thorn never bundles, vendors, or builds its own AMReX. It consumes the installation the ET AMReX thorn provides — whether that thorn built its bundled source or was pointed at an external install via `AMREX_DIR` — the same AMReX CarpetX links. Two AMReX copies in one Cactus executable is a build error, not a degraded mode.
- **Configuration-consistency guard.** A mismatch between the requested backend/MPI configuration and what the install's `AMReXConfig.cmake` records (`AMReX_GPU_BACKEND`, `AMReX_MPI`) is a **configure-time hard error** naming both values — never a silent build. The thorn forwards no MPI setting at all: the nested configure derives MPI from what the prefix records, so only an explicit request can mismatch. The value type stays pinned: `wli::Real` is `double` regardless of the install's `AMReX_PRECISION` (the `build-integration.md` pin, unchanged here).
- **Single build description.** The thorn carries no second source list or build description of `src/`; it drives this repo's CMake. Adding a source or header under `src/` must require no thorn edit.
- **Build-from-checkout, zero drift.** In build mode the thorn builds directly from the repository checkout it is part of (GetComponents checks out the whole repo; the arrangement entry is a path into it). No committed tarball and no copied/generated second source tree may exist in this mode.
- **Installed-mode behavior holds.** The library built via `find_package(AMReX)` against an install prefix passes the full regression suite with unchanged tolerance tiers (see Verification) — the same gating builds `build-integration.md` defines.
- **Consumers compile the device headers themselves.** All correctness-bearing `_Point` entry points are header-inline device functions (`amrex-device-interface.md`); a consumer thorn compiles them with its own compiler — under `AMREX_ENABLE_CUDA=yes` the AMReX thorn already switches consumer thorns to the CUDA compiler. The installed library contributes only host-side objects (the HDF5 loaders and host-level `ParallelFor` wrappers); no cross-thorn device-object linking may be required of consumers.
- **HDF5 comes from the ET HDF5 thorn.** The reader's C and C++ HDF5 API requirement is satisfied by that thorn (`HDF5_ENABLE_CXX=yes`, its bundled-build default); the thorn must not bundle or separately detect its own HDF5.
- **Find-or-build is never silent.** If `WEAKLIBINTERP_DIR` points at a prefix that does not contain the library + headers, or a build-mode build fails, configuration fails loudly (the ExternalLibraries "could neither find nor build" discipline).
- **The standalone gating builds are untouched.** The thorn drives the same root CMake in the same find-only mode; the default (MPI-ON), serial, single-precision-pin, and GPU compile-check gating builds of `build-integration.md` are byte-for-byte unaffected by the thorn's existence.
- **Loader semantics survive inside Cactus.** Root-read + broadcast table distribution (`table-format-and-io.md`) is unchanged; `amrex::Initialize` (run by the CarpetX driver) must precede any table load. Honoring that ordering is the consumer thorn's scheduling responsibility, restated here as the one integration-visible constraint.

## Verification

1. **Installed-AMReX rehearsal (CI-able, no Cactus required).** Build and install AMReX (CPU-only / double, per `build-integration.md`) into a prefix; configure this repo in installed-AMReX mode against it; build the library **and the full suite**; ctest passes with unchanged tiers — default parity `rtol = 1e-12` / `atol = 1e-30`, relaxed `1e-10`, machine-precision exactness `~1e-14`. This is the standing CI proxy for the thorn's build mode.
2. **The guard trips.** Against a CPU-backend (`NONE`) install prefix, requesting a CUDA-backend library configure fails at configure time with a diagnostic naming the requested backend and the install's recorded `AMReX_GPU_BACKEND`.
3. **Library-only install is self-sufficient.** A tests-off configure builds and installs only the library; a scratch translation unit that does `#include "wli_eos.H"` (and odr-uses one entry point per family) compiles and links against the installed prefix alone — proving the header set and library are complete without the source tree.
4. **No drift in the gating builds.** The default `build/` configure+ctest flow is unchanged by the thorn's existence — the gating configurations are defined by `build-integration.md`'s presets, never adjusted for Cactus.
5. **In-Cactus acceptance (environment-gated, manual — like the GPU compile checks, run where a Cactus checkout exists, never in this repo's ctest).** A CarpetX configuration whose thorn list adds `WeakLibInterp` and a scratch consumer thorn (`REQUIRES WeakLibInterp`, includes the umbrella headers, calls one `_Point` per family) builds to completion in CPU mode; the same configuration with `AMREX_ENABLE_CUDA=yes` (or HIP) compiles. *Status: the CPU-mode leg is exercised on the host — `tools/cactus-build.sh` builds the isolated `configs/wli` from the committed thornlist `cactus/wli.th` (consumer thorn `cactus/thorns/TestWeakLibInterp/`) and asserts at symbol level that `exe/cactus_wli` pulled `libwli_lib.a` members; the CUDA/HIP leg remains unexercised.*
6. **Mechanical (validator).** `bash specs/tools/validate_specs.sh` (default mode) asserts this file carries the 7 mandated sections in order, names a concrete numeric tolerance, and that its cited `amrex/...` source-of-truth paths resolve under `$AMREX_ROOT`.

## Implementation freedom

- The thorn's path inside this repo (e.g. `cactus/thorns/WeakLibInterp/`), its arrangement name in a Cactus checkout, and the split/style of `detect.sh` / `build.sh` / `make.code.deps`.
- The names of the CMake knobs (prefix path, tests-off switch) and how the nested configure derives its MPI value from the prefix, provided it never guesses.
- Static vs shared library, the installed library's file name (the thorn's `LIBRARY` emission must simply match), and the install layout beyond the flat-include guarantee.
- How `build.sh` locates the repo root from the thorn directory (relative path, symlink resolution, an exported variable).
- How the AMReX thorn's GPU options are forwarded into the nested CMake configure (e.g. setting `CUDACXX` / arch variables in `build.sh`), provided the consistency guard holds.
- Whether a helper consumer thorn (scheduled table loading, global table handles) is ever added — a separate future contract, not this one.

## Open questions / assumptions

- **Official ET distribution is deferred (assumption, non-blocking).** Group-use build-from-checkout is the contract; shipping through the official ET would add a `dist/` tarball (regenerated mechanically, not hand-maintained — learn from RePrimAnd's staleness), ET licensing/doc conventions, and a thornlist entry. Nothing here forecloses that.
- **HDF5 C-API port (open).** Porting the reader from the HDF5 C++ bindings to the C API would drop the `hdf5_cpp` requirement and widen cluster compatibility (some system HDF5 modules ship without C++ bindings, currently forcing the ET HDF5 thorn's bundled build). Owned by `table-format-and-io.md` if taken; this contract works either way via `HDF5_ENABLE_CXX=yes`.
- **CUDA consumer-compiler interplay (assumption, non-blocking).** Under `AMREX_ENABLE_CUDA=yes` the AMReX thorn rewires consumer thorns' `CXX` to the CUDA compiler; the nested CMake build instead receives explicit compiler/arch variables from `build.sh`. The first real CUDA Cactus build may surface flag-forwarding details; the guard ensures misconfiguration fails at configure time rather than at link.
- **CarpetX initializes AMReX before any scheduled table load (assumption, non-blocking).** The loaders assume `amrex::Initialize` has run; in a CarpetX run the driver guarantees this before scheduled thorn routines execute. Revisit only if a consumer needs tables at parameter-check time.
- **First consumer is undecided (assumption, non-blocking).** The contract exposes the library generically (`REQUIRES WeakLibInterp`); a nuX-style or purpose-built consumer thorn, and any helper thorn for shared table handles, are future work scoped only when a concrete consumer exists.
- **Cluster option lists (simfactory) are unexercised (assumption, non-blocking).** The thorn's option variables are designed to slot into machine option lists like the AMReX thorn's; no cluster build has validated them yet.
