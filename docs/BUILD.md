# Variant presets & CI — cold-path reference

The everyday build/test cycle lives in `CLAUDE.md` (Build & run). This file holds everything else: the preset/prefix-store vocabulary, the per-variant mechanisms, and the CI job map. Read the relevant section before touching `CMakePresets.json`, `tools/*.sh`, a `build-*/` tree, or `.github/workflows/ci.yml`.

One consumption mode covers everything: AMReX is only ever **found** as an installed prefix (`find_package(AMReX CONFIG)`), never built in-tree. `tools/provision-amrex.sh <config>` builds `../amrex` (override: `AMREX_SRC`) once, out-of-band, into the store `${WLI_AMREX_HOME:-$HOME/wli-amrex}/<config>` — it is the single home of the AMReX flag base (FORTRAN/OMP/LINEAR_SOLVERS/AMRLEVEL/PARTICLES/TINY_PROFILE off, INSTALL on, precision/MPI/GPU per config) and of the `-j4` OOM cap, and it is idempotent: it skips when `<dest>/lib/cmake/AMReX/AMReXConfig.cmake` exists (`--force` rebuilds; dest and scratch are wiped before building — never build over remnants). An explicit `WLI_AMREX_INSTALL_DIR` env var always beats the store — how existing prefixes (the ET thorn's `amrex-lib`, CI images) plug in.

Every `-DWLI_*` flag lives only in `CMakePresets.json`. `tools/build.sh [preset]` / `tools/test.sh [preset]` are thin drivers over a sourced `tools/wli-common.sh`; their only `-D` is the `WLI_AMREX_INSTALL_DIR` override passthrough. `build.sh` always reconfigures (cheap in find-only mode; kills the forgot-to-reconfigure footgun) and heals stale caches generically: a configure failure over a pre-existing `CMakeCache.txt` wipes the tree and retries once. `test.sh` refuses `cuda|rocm|notests|guard-trip` loudly (exit 2) and owns `--tables` (below). The drivers route their steps through `tools/q`; `.github/workflows/ci.yml` deliberately stays raw — CI log storage is free and full logs aid post-mortems.

## Preset table

Each configure preset inherits `default`, flips exactly one knob, resolves its prefix as `$env{WLI_AMREX_HOME}/<config>`, and keeps its own gitignored tree — the knob bakes into that tree's configuration, so trees are never interchangeable. Build presets carry `jobs: 4`; test presets carry `--output-on-failure`.

| Preset | Tree | Prefix config | Knob flipped | ctest? |
|---|---|---|---|---|
| `default` | `build/` | `mpi` | — (MPI-ON baseline) | full suite, 1/2/4-rank tiers |
| `serial` | `build-serial/` | `serial` | MPI off | full suite (1-rank ≡ serial cross-check) |
| `single` | `build-single/` | `single` | single precision | full suite |
| `cuda` | `build-cuda/` | `cuda` | GPU backend CUDA | never — compile-only; `test.sh` refuses |
| `rocm` | `build-rocm/` | `hip` | GPU backend HIP | never — compile-only; `test.sh` refuses |
| `notests` | `build-notests/` | `mpi` | tests off | n/a (`ctest -N` lists 0); `test.sh` refuses |
| `guard-trip` | `build-guard/` | `mpi` | GPU CUDA **against the NONE prefix** | never — configure must FAIL (negative check) |

- MPI resolution: `WLI_AMREX_MPI` defaults to `AUTO` — after `find_package(AMReX)` the root CMake adopts the `AMReX_MPI` fact the prefix records (status line `derived MPI=<ON|OFF>`; the cache keeps AUTO so a prefix swap re-derives). Only explicit ON/OFF requests can trip the MPI consistency guard. The `default` preset pins `ON` so the gating build demands an MPI prefix; `serial` pins `OFF`. The Cactus thorn forwards no MPI flag at all — the nested configure derives it from `AMREX_DIR`'s prefix.
- A missing/invalid prefix is a configure-time `FATAL_ERROR` naming the exact `tools/provision-amrex.sh <config>` command plus the `WLI_AMREX_HOME`/`WLI_AMREX_INSTALL_DIR` override knobs. Raw `cmake --preset <name>` with no `WLI_AMREX_HOME` exported resolves the prefix to `/<config>` and hits that same self-explaining error — `tools/build.sh` is the sanctioned entry point.
- Real-table cells: `tools/test.sh [preset] --tables [path]` — an explicit path wins, else a pre-exported `WL_TABLES_ROOT`, else a loud refusal naming the variable (a green `--tables` run can never mean the table cells silently SKIPped).

### MPI (the `default` preset)

- ONE configure serves all rank counts: the bare test names run at 1 rank via the global `CMAKE_TEST_LAUNCHER` = `mpiexec;-n;1` (needs CMake ≥ 3.29), and every target additionally gets `<name>_np2`/`<name>_np4` registrations via `wli_add_mpi_ranks()` in `test/CMakeLists.txt` — explicit `mpiexec -n {2,4} --oversubscribe $<TARGET_FILE:...>` COMMANDs with `SKIP_RETURN_CODE 77`, gated `if(WLI_AMREX_MPI)` (the resolved value, post-AUTO).
- Key mechanism: an `add_test` whose COMMAND is a plain program path (not an executable target) bypasses the generate-time global launcher — use that, never a reconfigure, to vary rank counts.
- Host MPI: `sudo apt-get install -y libopenmpi-dev openmpi-bin`; runs of >1 rank need `mpiexec --oversubscribe` on this host.
- MPI discovery is a guarded top-level `find_package(MPI REQUIRED COMPONENTS CXX)` — it exists solely for `MPIEXEC_EXECUTABLE`/`MPIEXEC_NUMPROC_FLAG`, link wiring is transitive via `AMReX::amrex`. Language C is enabled unconditionally at the top of the root CMake because an MPI-ON prefix's `AMReXConfig.cmake` runs `find_dependency(MPI REQUIRED C CXX)`.
- The `test_mpi_root_bcast` tests (root-read/broadcast) and the three `test_rank_consistency` argv modes (load/result/corrupt) run multi-rank in `build/`; in `build-serial/` they SKIP (77). The serial tree's bare-name 1-rank suite is the 1-rank ≡ serial cross-check (test-name identity across the two trees).
- Test mains without `amrex::Initialize` run under `mpiexec -n N` as N independent processes — harmless, correctness is exit codes.

### GPU compile checks (`cuda`, `rocm`)

- No GPU hardware needed or used; this host has neither toolchain — these compile in CI containers (`nvidia/cuda:12.6.3-devel-ubuntu24.04`, `rocm/dev-ubuntu-24.04:6.4.1`). CUDA needs nvcc, arch defaults to `WLI_CUDA_ARCH=8.0`; HIP needs `CXX=/opt/rocm/bin/amdclang++` + `CMAKE_PREFIX_PATH=/opt/rocm` in the environment (AMReX enforces a HIP-capable compiler), arch defaults to `WLI_AMD_ARCH=gfx90a`. The provisioner bakes the matching `AMReX_CUDA_ARCH`/`AMReX_AMD_ARCH` into the `cuda`/`hip` prefixes.
- HIP/CUDA gotchas learned from CI: AMReX's HIP backend needs `rocrand-dev rocprim-dev hiprand-dev` at configure time (the rocm/dev image ships without them; the root CMake also resolves `hip`/`rocrand`/`rocprim`/`hiprand` packages itself before `find_package(AMReX)` under `WLI_GPU_BACKEND=HIP`); nvcc rejects dynamically-initialized device-visible globals (use constexpr literals); device-linked executables need the HDF5 **C** library linked explicitly (PUBLIC on `wli_lib`), not just `hdf5::hdf5_cpp`.

### Library-only (`notests`)

- The only gate is around `add_subdirectory(test)` — `enable_testing()` and the MPI-launcher block stay unconditional (harmless with no test targets).

### Guard-trip (`guard-trip`)

- Deliberately wrong on purpose: it points at the GPU-NONE `mpi` prefix but requests `WLI_GPU_BACKEND=CUDA`, so `cmake --preset guard-trip` must FAIL with `AMReX GPU-backend mismatch` — the negative check of the consistency guard, exercised in CI with inverted exit status + grep. Never built, never tested.

### Installing WeakLibInterp itself (cactus-integration.md §57, V#3)

- `tools/build.sh notests` → `cmake --install build-notests --prefix <abs wli prefix>`. Yields `lib/libwli_lib.a` + a flat `include/` with the public `wli_*.H` (the two `io/*_detail.H` internals are excluded).
- No CMake package-config is installed by design — a consumer compiles with `-I<prefix>/include` and links the archive plus AMReX and HDF5 explicitly (raw-emission Cactus contract, spec:51). Install rules live in `src/CMakeLists.txt`; the trailing slash on each `install(DIRECTORY core/ eos/ ...)` source dir is what flattens the layout — don't drop it.

### In-Cactus build (`tools/cactus-build.sh`, manual, this host only)

- Requires `$CACTUSX`; option file from `ETKCFG` (sandbox) else `$ETKGUIDE/macos.cfg` (host). Ensures the `arrangements/WeakLibInterp → <repo>/cactus/thorns` symlink, drives `Compile-ETK` against the committed thornlist `cactus/wli.th` into the isolated `configs/wli` (log: `$CACTUSX/last-build-wli.log`; the working `carpetx` config is never touched), then asserts `exe/cactus_wli` exists and `nm` finds `wli_value_type_size` — the `TestWeakLibInterp` consumer thorn's startup registration is what forces the `libwli_lib.a` member into the link. Incremental by default; `--fresh` reconfigures from the option file. Never in ctest or CI (cactus-integration.md V#5).

## GitHub CI

`.github/workflows/ci.yml` runs on every push (all branches); five jobs. Sibling repos are cloned at pinned SHAs via the `AMREX_REF`/`WEAKLIB_REF` env vars at the top of the workflow — bump those when the local `../amrex`/`../weaklib` checkouts move.

Vocabulary identity: every AMReX-consuming job runs the exact commands a developer types locally — restore-cache → `tools/provision-amrex.sh <config>` (unconditional; on a cache hit the skip-if-valid probe makes it a no-op) → `tools/build.sh <preset>` → `tools/test.sh <preset>` (CPU jobs only). No `-DWLI_*` flag appears anywhere in the workflow. The cache holds the finished prefix `$HOME/wli-amrex/<config>` (not per-object compiles; there is no ccache), keyed `amrex-<config>-<AMREX_REF>-hash(tools/provision-amrex.sh)` — exactly the two inputs that can change a prefix; rebuild by bumping `AMREX_REF`, editing the provisioner, or `gh cache delete <key>`. All AMReX-consuming jobs install OpenMPI (every config except `serial` records MPI ON).

- `validate-specs` — `bash specs/tools/validate_specs.sh` in default mode against the pinned amrex/weaklib checkouts (live-table stage prints SKIPPED by design).
- `build-test` (the standing gate) — default preset, full ctest with the 1/2/4-rank tiers on bare ubuntu-latest (non-root runner: the baked-in plain `--oversubscribe` suffices). Also absorbs the two consumption checks: the guard-trip (`cmake --preset guard-trip` must fail, inverted exit + grep `AMReX GPU-backend mismatch` — no CUDA toolchain needed, the guard fires before `enable_language(CUDA)`) and the library-only install (notests preset → `cmake --install` → a heredoc scratch TU compiles, links `libwli_lib.a` from the bare `-I/-L` prefix via a nested `find_package(AMReX CONFIG)` project, and runs).
- `build-test-serial` — same restore → provision → build → test shape with the `serial` preset; test-name identity against `build-test`'s tree is the 1-rank ≡ serial cross-check.
- `build-cuda` / `build-rocm` — the vendor containers above; provision `cuda`/`hip`, `tools/build.sh cuda|rocm`, compile-only (runners have no GPU; `tools/test.sh` refuses these presets anyway).
