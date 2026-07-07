# Variant builds & CI — cold-path reference

The everyday build/test cycle lives in `CLAUDE.md` (Build & run). This file holds everything else: the variant build trees and the CI job map. Read the relevant section before touching a variant tree or `.github/workflows/ci.yml`. Every variant uses its own gitignored build tree — the flags below are baked into the AMReX compile, so trees are not interchangeable.

## Single-precision pin verification (build-integration.md §63)

`cmake -S . -B build-single -DCMAKE_BUILD_TYPE=Release -DWLI_AMREX_PRECISION=SINGLE` → `cmake --build build-single -j4` → `ctest --test-dir build-single --output-on-failure`. Separate tree required — `AMREX_USE_FLOAT` is baked into the AMReX compile, so `build-single/` is a full second AMReX build, not reusable with `build/`. `WLI_AMREX_PRECISION` defaults to `DOUBLE`; the default build is unaffected.

## MPI-enabled build + 1/2/4-rank suite (build-integration.md §61,72)

`cmake -S . -B build-mpi -DCMAKE_BUILD_TYPE=Release -DWLI_AMREX_MPI=ON` → `cmake --build build-mpi -j4` → `ctest --test-dir build-mpi --output-on-failure`.

- ONE configure serves all rank counts — 85 tests total: the 27 bare test names run at 1 rank via the global `CMAKE_TEST_LAUNCHER` = `mpiexec;-n;1` (needs CMake ≥ 3.29 — host has 4.2.3; this is the 1-rank==serial cross-check), plus `<name>_np2`/`<name>_np4` registrations for every target via `wli_add_mpi_ranks()` in `test/CMakeLists.txt` — explicit `mpiexec -n {2,4} --oversubscribe $<TARGET_FILE:...>` COMMANDs with `SKIP_RETURN_CODE 77`, gated `if(WLI_AMREX_MPI)`.
- Key mechanism: an `add_test` whose COMMAND is a plain program path (not an executable target) bypasses the generate-time global launcher — use that, never a reconfigure, to vary rank counts.
- Separate tree required — `AMReX_MPI` is baked into the AMReX compile, so `build-mpi/` is a full second AMReX build, like `build-single/`. `WLI_AMREX_MPI` defaults to `OFF`; the default build is unaffected (`_np*` tests are absent there).
- Host MPI: `sudo apt-get install -y libopenmpi-dev openmpi-bin` (OpenMPI 5.0.10); runs of >1 rank need `mpiexec --oversubscribe` on this host.
- MPI discovery is a guarded top-level `find_package(MPI REQUIRED COMPONENTS CXX)` — CXX only (the project enables no C language); it exists solely for `MPIEXEC_EXECUTABLE`/`MPIEXEC_NUMPROC_FLAG`, link wiring is transitive via `AMReX::amrex`.
- The two `test_mpi_root_bcast` tests (root-read/broadcast) and the three `test_rank_consistency` argv modes (load/result/corrupt, each at `_np2`/`_np4`; cross-rank digest + bitwise result checks) run multi-rank only in `build-mpi/`; they SKIP (77) in the default `build/` (30 tests there).
- Test mains without `amrex::Initialize` run under `mpiexec -n N` as N independent processes — harmless, correctness is exit codes.

## GPU compile checks (build-integration.md; compile-only — never run ctest under these)

No GPU hardware needed or used. CUDA: `cmake -S . -B build-cuda -DCMAKE_BUILD_TYPE=Release -DWLI_GPU_BACKEND=CUDA` (needs nvcc; arch defaults to `WLI_CUDA_ARCH=8.0`). HIP: `cmake -S . -B build-rocm -DCMAKE_BUILD_TYPE=Release -DWLI_GPU_BACKEND=HIP -DCMAKE_CXX_COMPILER=/opt/rocm/bin/amdclang++ -DCMAKE_PREFIX_PATH=/opt/rocm` (arch defaults to `WLI_AMD_ARCH=gfx90a`). Separate build trees; this host has neither toolchain — these run in CI containers (`nvidia/cuda:12.6.3-devel-ubuntu24.04`, `rocm/dev-ubuntu-24.04:6.4.1`). `WLI_GPU_BACKEND` defaults to `NONE`; the default build is unaffected.

HIP gotchas learned from CI: AMReX's HIP backend needs `rocrand-dev rocprim-dev hiprand-dev` at configure time (the rocm/dev image ships without them); nvcc rejects dynamically-initialized device-visible globals (use constexpr literals); device-linked executables need the HDF5 **C** library linked explicitly (PUBLIC on `wli_lib`), not just `hdf5::hdf5_cpp`.

## Library-only / tests-off build (cactus-integration.md §56,77)

`cmake -S . -B build-notests -DCMAKE_BUILD_TYPE=Release -DWLI_BUILD_TESTS=OFF` → `cmake --build build-notests -j4 --target wli_lib` builds `wli_lib` with zero ctest targets (`ctest -N` lists 0). `WLI_BUILD_TESTS` defaults to `ON`; the default build is unaffected. The only gate is around `add_subdirectory(test)` — `enable_testing()` and the MPI-launcher block stay unconditional (harmless with no test targets).

## Installed-AMReX mode (cactus-integration.md §53-58, Verification #1-#2)

`-DWLI_AMREX_INSTALL_DIR=<abs prefix>` switches root CMake from sibling-source `add_subdirectory` to `find_package(AMReX REQUIRED CONFIG)`; empty/absent default keeps the sibling-source path byte-unchanged. Installed mode hard-errors at configure when the install's recorded `AMReX_GPU_BACKEND`/`AMReX_MPI` mismatch the requested `WLI_GPU_BACKEND`/`WLI_AMREX_MPI`, naming both values (precision exempt per spec:63).

Produce a scratch CPU-only/double prefix: `cmake -S ../amrex -B <scratch>/amrex-build -DCMAKE_BUILD_TYPE=Release -DAMReX_MPI=OFF -DAMReX_LINEAR_SOLVERS=OFF -DAMReX_AMRLEVEL=OFF -DAMReX_PARTICLES=OFF -DAMReX_TINY_PROFILE=OFF -DCMAKE_INSTALL_PREFIX=<abs prefix>` → `cmake --build <scratch>/amrex-build -j4 --target install`; use the same host cmake for both (the installed `AMReXConfig.cmake` bakes in its `cmake_minimum_required`). If `build/` carries a stale cache from another OS (e.g. macOS compiler paths), `rm -rf build` and reconfigure.

## Installing WeakLibInterp itself (cactus-integration.md §57, Verification #3)

Add `-DCMAKE_INSTALL_PREFIX=<abs wli prefix>` to any configure (typically the tests-off + installed-AMReX combination above) → `cmake --build <tree> --target install`. Yields `lib/libwli_lib.a` + a flat `include/` with the 13 public `wli_*.H` (the two `io/*_detail.H` internals are excluded); no CMake package-config is installed by design — a consumer compiles with `-I<prefix>/include` and links the archive plus AMReX and HDF5 explicitly (raw-emission Cactus contract, spec:51). Rules live in `src/CMakeLists.txt:43-70`; the trailing slash on each `install(DIRECTORY core/ eos/ ...)` source dir is what flattens the layout — don't drop it.

## GitHub CI

`.github/workflows/ci.yml` runs on every push (all branches):

- Spec validation + double-precision build/ctest on ubuntu-latest.
- A correctness-gating `build-test-mpi` job (bare ubuntu-latest, no container — apt `libopenmpi-dev openmpi-bin`, `-DWLI_AMREX_MPI=ON` into `build-mpi/`, ONE unfiltered ctest covering the 1/2/4-rank tiers; non-root runner means the baked-in `--oversubscribe` suffices, no `--allow-run-as-root`).
- Compile-only CUDA (`build-cuda` job, nvcc/sm_80) and ROCm (`build-rocm` job, amdclang++/gfx90a) builds of the library + suite in pinned vendor containers — GitHub runners have no GPU, so those jobs never run ctest.
- `build-test-installed-amrex` ("Installed-AMReX rehearsal", cactus-integration.md V#1-#3): installs a CPU-only/double AMReX prefix (`--target install`), runs the full suite in installed-AMReX mode (`build-installed`), asserts the config guard trips (CUDA-vs-NONE, grep `AMReX GPU-backend mismatch` — no CUDA toolchain needed, guard fires before `enable_language(CUDA)`), and compiles/links/runs a heredoc scratch TU against the installed WLI prefix alone via a nested `find_package(AMReX CONFIG)` project (ccache key `amrex-installed-`).
- Sibling repos are cloned at pinned SHAs via the `AMREX_REF`/`WEAKLIB_REF` env vars at the top of the workflow — bump those when the local `../amrex`/`../weaklib` checkouts move. AMReX compiles are ccache-cached keyed on `AMREX_REF` (per-config keys: `amrex-mpi-` for MPI, per-backend for the GPU jobs).
