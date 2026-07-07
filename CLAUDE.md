# WeakLibInterp — loop operating manual

## Specifications

**IMPORTANT:** Before implementing any feature, consult the specifications in `specs/README.md`.

- **Check the codebase first.** Before concluding something is or isn't implemented, search the actual code. Specs describe intent; code describes reality.
- **Use specs as guidance.** When implementing a feature, follow the design patterns, types, and architecture defined in the relevant spec.
- **Spec index:** `specs/README.md` lists all specifications organized by category.

## Goal & constraints (the mission — every agent honors this)

**Ultimate goal:** A GPU-friendly C++ reimplementation of weaklib's equation-of-state (EOS) and opacity interpolators, exposed as AMReX-native device functions. Judge the relevance of everything you read against this goal.

**Constraints:**
- Do NOT assume functionality is missing; confirm with code search before concluding absence.
- Treat `src/` as the project's standard library. Prefer consolidated, idiomatic implementations there over ad-hoc copies; flag duplication.
- Single source of truth — no migrations/adapters, no placeholders or stubs. Implement completely (build loop).

## Layout

- `specs/*` — acceptance source of truth (WHAT must hold). `specs/tools/`, `specs/fixtures/`.
- `src/*` — the shared standard library, organized AMReX-style into module dirs `src/{core,eos,opacity,io}/`; every module dir is on the include path, so includes stay flat (`#include "wli_eos.H"`) and the `wli_` filename prefix is the namespace (mirroring `<AMReX_*.H>`). The opacity kernels are one header per opacity leaf spec (`wli_opacity_{emab_iso,nes_pair,brem}.H`) with `wli_opacity.H` as the umbrella.
- `test/*` — the regression suite: one `test_*.cpp` per ctest target, registered in `test/CMakeLists.txt` (hand-rolled assert harness, no GoogleTest/Catch2; `SKIP_RETURN_CODE 77` for guarded cells); `h5ls_snapshot.H` is a test-only fixture parser; `wli_rank_digest.H` is the test-only cross-rank digest header (FNV-1a `wli::test::Hasher` + `digest()` overloads for all six host-table structs) — MPI consistency tests include it rather than re-inlining a hasher.
- `TODO.md` — the durable, priority-sorted plan; the only state carried between loop iterations. Completed items are marked `- [x]` in place by the build loop (never removed); they are pruned only when the plan loop recreates the file.
- Sibling repos (read-only sources of truth): `../amrex` (device interface), `../weaklib` and `../thornado` (Fortran behavior being reimplemented).

## Build & run

Per `specs/build-integration.md` the target is **AMReX CPU-only, double precision, host execution, no Fortran/Matlab**; the correctness-bearing value type is pinned to `double` (`src/core/wli_real.H`, `wli::Real`) regardless of `amrex::Real`. AMReX resolves from `../amrex` via `add_subdirectory` (override with `-DWLI_AMREX_ROOT=<path>`). Build dir is `build/` (gitignored); `.build/` is agent scratch, never the build dir.

- **Validate specs:** `AMREX_ROOT=../amrex bash specs/tools/validate_specs.sh`
- **Configure:** `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- **Build:** `cmake --build build -j4` (cap `-j`; uncapped AMReX builds OOM this host). After adding a new test target to `test/CMakeLists.txt`, re-run configure first — the incremental build won't see it.
- **Test:** `ctest --test-dir build --output-on-failure`. Real-table cells (`production_tables`) SKIP without live tables — before committing changes to them, also run with `WL_TABLES_ROOT=/Users/liwei/Datas/wl_tables/use_for_production` (this host's copy).
- **Thorn check:** `bash specs/tools/validate_thorn.sh` after touching `cactus/thorns/WeakLibInterp/` (structural gate, not in ctest; in-Cactus acceptance stays manual — no Cactus checkout here).
- Host prerequisites (apt, reinstall if the sandbox resets): `cmake`, `g++`, `libhdf5-dev`. Never install Fortran/Matlab. Do not enable `AMReX_HDF5` — HDF5 is found independently.

**Variant builds & CI → `docs/BUILD.md`.** Single-precision, MPI 1/2/4-rank, CUDA/HIP compile-only, tests-off, installed-AMReX mode, installing WLI, and the CI job map live there. Read it before touching any variant tree (`build-single/`, `build-mpi/`, `build-cuda/`, `build-rocm/`, `build-notests/`, `build-installed/`) or `.github/workflows/ci.yml`.

## Self-improvement

If a loop iteration discovers how to compile, run, or test the project — or a correction to anything above — record it by heat: everyday-cycle commands and host quirks go in Build & run above (keep that section under ~15 lines); variant-build and CI detail goes in `docs/BUILD.md`. Commands and how-to-run only, terse; progress belongs in `TODO.md`.
