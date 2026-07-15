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
- `cactus/` — the Cactus delivery: the ExternalLibraries-style provider thorn `thorns/WeakLibInterp/` (ccl files + `src/{detect.sh,build.sh,make.code.deps,make.code.defn}`) that finds-or-builds this repo's installed library, the `thorns/TestWeakLibInterp/` scratch consumer, and the compile thornlist `wli.th`; structural gate is `specs/tools/validate_thorn.sh` (not in ctest), in-Cactus build is `tools/cactus-build.sh` (manual, host-gated).
- `docs/BUILD.md` — preset/variant & CI detail; `tools/` — the workflow scripts (`q`, `provision-amrex.sh`, `build.sh`, `test.sh`, `cactus-build.sh`, shared `wli-common.sh`).
- `TODO.md` — the durable, priority-sorted plan; the only state carried between loop iterations. Completed items are marked `- [x]` in place by the build loop (never removed); they are pruned only when the plan loop recreates the file.
- Sibling repos (read-only sources of truth): `../amrex` (device interface), `../weaklib` and `../thornado` (Fortran behavior being reimplemented).

## Build & run

Per `specs/build-integration.md` the target is **AMReX CPU-only, double precision, host execution, no Fortran/Matlab**; the gating configuration is MPI-ON (the default suite includes the 1/2/4-rank tiers); the correctness-bearing value type is pinned to `double` (`src/core/wli_real.H`, `wli::Real`) regardless of `amrex::Real`. AMReX is only ever **found** as an installed prefix — presets resolve `$WLI_AMREX_HOME/<config>` (default `$HOME/wli-amrex`); an explicit `WLI_AMREX_INSTALL_DIR` env var always wins. Build trees are `build/` (+ `build-<preset>/`, gitignored); `.build/` is agent scratch, never a build dir.

- **Quiet runner:** prefix noisy commands with `tools/q` — output is stashed to `.build/logs/<cmd>.log` (kept after success, greppable); exit 0 prints one `✓` line, nonzero dumps the full log and passes the exit code through. The `tools/*.sh` drivers below already route through it. Contract: `specs/build-integration.md` "Quiet-runner workflow".
- **Provision AMReX (once per config):** `tools/q bash tools/provision-amrex.sh mpi` — the single home of the AMReX flag base and the `-j4` OOM cap; idempotent (`--force` rebuilds). A missing prefix fails configure with this exact command in the error.
- **Build:** `tools/build.sh [preset]` — always reconfigures and self-heals stale caches (wipe + one retry). Every `-DWLI_*` flag lives in `CMakePresets.json`, never in scripts or docs.
- **Test:** `tools/test.sh [preset]`. Real-table cells (`production_tables`) SKIP without live tables — before committing changes to them, also run `tools/test.sh --tables /Users/liwei/Datas/wl_tables/use_for_production` (this host's copy).
- **Validate specs:** `AMREX_ROOT=../amrex tools/q bash specs/tools/validate_specs.sh`
- **Thorn check:** `tools/q bash specs/tools/validate_thorn.sh` after touching `cactus/thorns/` (structural gate, not in ctest). In-Cactus acceptance: `tools/cactus-build.sh [--fresh]` — manual, needs `$CACTUSX` on this host, never in ctest/CI.
- Host prerequisites (apt, reinstall if the sandbox resets): `cmake`, `g++`, `libhdf5-dev`, plus OpenMPI (`libopenmpi-dev openmpi-bin`) for the MPI-ON default. Never install Fortran/Matlab. Do not enable `AMReX_HDF5` — HDF5 is found independently.

**Variant presets & CI → `docs/BUILD.md`.** The preset table (`default|serial|single|cuda|rocm|notests|guard-trip`), prefix-store/provisioner mechanics, per-variant mechanisms, and the CI job map live there. Read it before touching `CMakePresets.json`, `tools/*.sh`, any `build-*/` tree, or `.github/workflows/ci.yml`.

## Self-improvement

If a loop iteration discovers how to compile, run, or test the project — or a correction to anything above — record it by heat: everyday-cycle commands and host quirks go in Build & run above (keep that section under ~15 lines); variant-build and CI detail goes in `docs/BUILD.md`. In `docs/BUILD.md`, record mechanisms, not inventories: append to the matching variant's bullet group or CI bullet, never a new top-level section; no test/header counts or other now-values that drift. Commands and how-to-run only, terse; progress belongs in `TODO.md`.
