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
- Treat `src/lib` as the project's standard library. Prefer consolidated, idiomatic implementations there over ad-hoc copies; flag duplication.
- Single source of truth — no migrations/adapters, no placeholders or stubs. Implement completely (build loop).

## Layout

- `specs/*` — acceptance source of truth (WHAT must hold). `specs/tools/`, `specs/fixtures/`.
- `src/*` — implementation; `src/lib/*` is the shared standard library.
- `TODO.md` — the durable, priority-sorted plan; the only state carried between loop iterations. Completed items are marked `- [x]` in place by the build loop (never removed); they are pruned only when the plan loop recreates the file.
- Sibling repos (read-only sources of truth): `../amrex` (device interface), `../weaklib` and `../thornado` (Fortran behavior being reimplemented).

## Build & run

Per `specs/build-integration.md` the target is **AMReX CPU-only, double precision, host execution, no Fortran/Matlab**; the correctness-bearing value type is pinned to `double` (`src/lib/wli_real.H`, `wli::Real`) regardless of `amrex::Real`. AMReX resolves from `../amrex` via `add_subdirectory` (override with `-DWLI_AMREX_ROOT=<path>`). Build dir is `build/` (gitignored); `.build/` is agent scratch, never the build dir.

- **Validate specs:** `AMREX_ROOT=../amrex bash specs/tools/validate_specs.sh`
- **Configure:** `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- **Build the library + suite:** `cmake --build build -j4` (cap `-j`; AMReX is large and uncapped builds OOM this host). After adding a new test target to `test/CMakeLists.txt`, re-run the configure step first — the incremental build won't see the new target otherwise.
- **Run the regression suite:** `ctest --test-dir build --output-on-failure`
- **Single-precision pin verification** (build-integration.md §63): `cmake -S . -B build-single -DCMAKE_BUILD_TYPE=Release -DWLI_AMREX_PRECISION=SINGLE` → `cmake --build build-single -j4` → `ctest --test-dir build-single --output-on-failure`. Separate tree required — `AMREX_USE_FLOAT` is baked into the AMReX compile, so `build-single/` (gitignored) is a full second AMReX build, not reusable with `build/`. `WLI_AMREX_PRECISION` defaults to `DOUBLE`; the default build is unaffected.
- **GPU compile checks** (build-integration.md, compile-only — never run ctest under these; no GPU hardware needed or used): CUDA: `cmake -S . -B build-cuda -DCMAKE_BUILD_TYPE=Release -DWLI_GPU_BACKEND=CUDA` (needs nvcc; arch defaults to `WLI_CUDA_ARCH=8.0`). HIP: `cmake -S . -B build-rocm -DCMAKE_BUILD_TYPE=Release -DWLI_GPU_BACKEND=HIP -DCMAKE_CXX_COMPILER=/opt/rocm/bin/amdclang++ -DCMAKE_PREFIX_PATH=/opt/rocm` (arch defaults to `WLI_AMD_ARCH=gfx90a`). Separate build trees; this host has neither toolchain — these run in CI containers (`nvidia/cuda:12.6.3-devel-ubuntu24.04`, `rocm/dev-ubuntu-24.04:6.4.1`). `WLI_GPU_BACKEND` defaults to `NONE`; the default build is unaffected. HIP gotchas learned from CI: AMReX's HIP backend needs `rocrand-dev rocprim-dev hiprand-dev` at configure time (the rocm/dev image ships without them); nvcc rejects dynamically-initialized device-visible globals (use constexpr literals); device-linked executables need the HDF5 **C** library linked explicitly (PUBLIC on `wli_lib`), not just `hdf5::hdf5_cpp`.
- Host toolchain prerequisites (apt, reinstall if the sandbox resets): `cmake`, `g++`, `libhdf5-dev` (HDF5 C++ bindings at `/usr/include/hdf5/serial/H5Cpp.h`). Never install Fortran/Matlab. Do not enable `AMReX_HDF5` — HDF5 is found independently.

- **GitHub CI:** `.github/workflows/ci.yml` runs on every push (all branches): spec validation + double-precision build/ctest on ubuntu-latest, plus compile-only CUDA (`build-cuda` job, nvcc/sm_80) and ROCm (`build-rocm` job, amdclang++/gfx90a) builds of the library + suite in pinned vendor containers — GitHub runners have no GPU, so those jobs never run ctest. Sibling repos are cloned at pinned SHAs via the `AMREX_REF`/`WEAKLIB_REF` env vars at the top of the workflow — bump those when the local `../amrex`/`../weaklib` checkouts move. AMReX compiles are ccache-cached keyed on `AMREX_REF` (per-backend keys for the GPU jobs).

When you learn a concrete build/test command, update this section (see loop rule below) so the next iteration inherits it instead of rediscovering it.

## Self-improvement

If a loop iteration discovers how to compile, run, or test the project — or a correction to anything above — the orchestrator records it here (commands and how-to-run only). Keep it operational and terse; progress belongs in `TODO.md`.
