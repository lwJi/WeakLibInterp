# Variant builds & CI — cold-path reference

The everyday build/test cycle lives in `CLAUDE.md` (Build & run). This file holds everything else: the variant build trees and the CI job map. Read the relevant section before touching a variant tree or `.github/workflows/ci.yml`.

Two invariants cover every variant: each `WLI_*` variant flag defaults to its inert value (`WLI_AMREX_PRECISION=DOUBLE`, `WLI_AMREX_MPI=OFF`, `WLI_GPU_BACKEND=NONE`, `WLI_BUILD_TESTS=ON`, `WLI_AMREX_INSTALL_DIR` empty), so the default `build/` is never affected; and each variant needs its own gitignored tree — the flag is baked into that tree's AMReX compile, so trees are never interchangeable.

Commands are `tools/q`-prefixed (the quiet runner from `CLAUDE.md` Build & run). `.github/workflows/ci.yml` deliberately stays raw — CI log storage is free and full logs aid post-mortems.

## Variant trees

Configure is `tools/q cmake -S . -B <tree> -DCMAKE_BUILD_TYPE=Release <flags>`, build is `tools/q cmake --build <tree> -j4`, test (where the row says ctest) is `tools/q ctest --test-dir <tree> --output-on-failure`.

| Tree | Flags | After build | Spec |
|---|---|---|---|
| `build-single` | `-DWLI_AMREX_PRECISION=SINGLE` | full ctest | build-integration.md §63 |
| `build-mpi` | `-DWLI_AMREX_MPI=ON` | full ctest (1/2/4-rank tiers) | build-integration.md §61,72 |
| `build-cuda` | `-DWLI_GPU_BACKEND=CUDA` | compile-only — **never** run ctest | build-integration.md |
| `build-rocm` | `-DWLI_GPU_BACKEND=HIP -DCMAKE_CXX_COMPILER=/opt/rocm/bin/amdclang++ -DCMAKE_PREFIX_PATH=/opt/rocm` | compile-only — **never** run ctest | build-integration.md |
| `build-notests` | `-DWLI_BUILD_TESTS=OFF` | build `--target wli_lib`; `ctest -N` lists 0 | cactus-integration.md §56,77 |
| `build-installed` | `-DWLI_AMREX_INSTALL_DIR=<abs prefix>` | full ctest | cactus-integration.md §53-58, V#1-#2 |

### MPI (`build-mpi`)

- ONE configure serves all rank counts: the bare test names run at 1 rank via the global `CMAKE_TEST_LAUNCHER` = `mpiexec;-n;1` (needs CMake ≥ 3.29; this is the 1-rank==serial cross-check), and every target additionally gets `<name>_np2`/`<name>_np4` registrations via `wli_add_mpi_ranks()` in `test/CMakeLists.txt` — explicit `mpiexec -n {2,4} --oversubscribe $<TARGET_FILE:...>` COMMANDs with `SKIP_RETURN_CODE 77`, gated `if(WLI_AMREX_MPI)`.
- Key mechanism: an `add_test` whose COMMAND is a plain program path (not an executable target) bypasses the generate-time global launcher — use that, never a reconfigure, to vary rank counts.
- Host MPI: `sudo apt-get install -y libopenmpi-dev openmpi-bin`; runs of >1 rank need `mpiexec --oversubscribe` on this host.
- MPI discovery is a guarded top-level `find_package(MPI REQUIRED COMPONENTS CXX)` — CXX only (the project enables no C language); it exists solely for `MPIEXEC_EXECUTABLE`/`MPIEXEC_NUMPROC_FLAG`, link wiring is transitive via `AMReX::amrex`.
- The `test_mpi_root_bcast` tests (root-read/broadcast) and the three `test_rank_consistency` argv modes (load/result/corrupt; cross-rank digest + bitwise result checks) run multi-rank only in `build-mpi/`; they SKIP (77) in the default `build/`.
- Test mains without `amrex::Initialize` run under `mpiexec -n N` as N independent processes — harmless, correctness is exit codes.

### GPU compile checks (`build-cuda`, `build-rocm`)

- No GPU hardware needed or used; this host has neither toolchain — these run in CI containers (`nvidia/cuda:12.6.3-devel-ubuntu24.04`, `rocm/dev-ubuntu-24.04:6.4.1`). CUDA needs nvcc, arch defaults to `WLI_CUDA_ARCH=8.0`; HIP arch defaults to `WLI_AMD_ARCH=gfx90a`.
- HIP/CUDA gotchas learned from CI: AMReX's HIP backend needs `rocrand-dev rocprim-dev hiprand-dev` at configure time (the rocm/dev image ships without them); nvcc rejects dynamically-initialized device-visible globals (use constexpr literals); device-linked executables need the HDF5 **C** library linked explicitly (PUBLIC on `wli_lib`), not just `hdf5::hdf5_cpp`.

### Library-only (`build-notests`)

- The only gate is around `add_subdirectory(test)` — `enable_testing()` and the MPI-launcher block stay unconditional (harmless with no test targets).

### Installed-AMReX mode (`build-installed`)

- `-DWLI_AMREX_INSTALL_DIR=<abs prefix>` switches root CMake from sibling-source `add_subdirectory` to `find_package(AMReX REQUIRED CONFIG)`; empty/absent default keeps the sibling-source path byte-unchanged.
- Configure hard-errors when the install's recorded `AMReX_GPU_BACKEND`/`AMReX_MPI` mismatch the requested `WLI_GPU_BACKEND`/`WLI_AMREX_MPI`, naming both values (precision exempt per spec:63).
- Produce a scratch CPU-only/double prefix: `tools/q cmake -S ../amrex -B <scratch>/amrex-build -DCMAKE_BUILD_TYPE=Release -DAMReX_MPI=OFF -DAMReX_LINEAR_SOLVERS=OFF -DAMReX_AMRLEVEL=OFF -DAMReX_PARTICLES=OFF -DAMReX_TINY_PROFILE=OFF -DCMAKE_INSTALL_PREFIX=<abs prefix>` → `tools/q cmake --build <scratch>/amrex-build -j4 --target install`. Use the same host cmake for both (the installed `AMReXConfig.cmake` bakes in its `cmake_minimum_required`); if a tree carries a stale cache from another OS, `rm -rf` it and reconfigure.

### Installing WeakLibInterp itself (cactus-integration.md §57, V#3)

- Add `-DCMAKE_INSTALL_PREFIX=<abs wli prefix>` to any configure (typically the tests-off + installed-AMReX combination) → `tools/q cmake --build <tree> --target install`. Yields `lib/libwli_lib.a` + a flat `include/` with the public `wli_*.H` (the two `io/*_detail.H` internals are excluded).
- No CMake package-config is installed by design — a consumer compiles with `-I<prefix>/include` and links the archive plus AMReX and HDF5 explicitly (raw-emission Cactus contract, spec:51). Install rules live in `src/CMakeLists.txt`; the trailing slash on each `install(DIRECTORY core/ eos/ ...)` source dir is what flattens the layout — don't drop it.

## GitHub CI

`.github/workflows/ci.yml` runs on every push (all branches). Sibling repos are cloned at pinned SHAs via the `AMREX_REF`/`WEAKLIB_REF` env vars at the top of the workflow — bump those when the local `../amrex`/`../weaklib` checkouts move. AMReX compiles are ccache-cached keyed on `AMREX_REF` (per-config keys: `amrex-mpi-`, per-backend for the GPU jobs, `amrex-installed-`).

- Spec validation + default double-precision build/ctest on ubuntu-latest.
- `build-test-mpi` (correctness-gating) — the `build-mpi` row on bare ubuntu-latest (no container; apt `libopenmpi-dev openmpi-bin`); ONE unfiltered ctest covers the 1/2/4-rank tiers; non-root runner means the baked-in `--oversubscribe` suffices, no `--allow-run-as-root`.
- `build-cuda` / `build-rocm` — those rows compile-only in the pinned vendor containers above; GitHub runners have no GPU, so these jobs never run ctest.
- `build-test-installed-amrex` ("Installed-AMReX rehearsal", cactus-integration.md V#1-#3) — installs a scratch AMReX prefix, runs the full suite via the `build-installed` row, asserts the config guard trips (CUDA-vs-NONE, grep `AMReX GPU-backend mismatch` — no CUDA toolchain needed, the guard fires before `enable_language(CUDA)`), and compiles/links/runs a heredoc scratch TU against the installed WLI prefix alone via a nested `find_package(AMReX CONFIG)` project.
