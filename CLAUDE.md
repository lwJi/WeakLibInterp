# WeakLibInterp — loop operating manual

This file auto-loads in every session (orchestrator and every subagent). It carries the standing goal, the constraints every agent judges relevance against, and how to build/run the project. Status and progress live in `TODO.md`, never here.

## Goal & constraints (the mission — every agent honors this)

**Ultimate goal:** A GPU-friendly C++ reimplementation of weaklib's equation-of-state (EOS) and opacity interpolators, exposed as AMReX-native device functions. Judge the relevance of everything you read against this goal.

**Constraints:**
- Do NOT assume functionality is missing; confirm with code search before concluding absence.
- Treat `src/lib` as the project's standard library. Prefer consolidated, idiomatic implementations there over ad-hoc copies; flag duplication.
- Single source of truth — no migrations/adapters, no placeholders or stubs. Implement completely (build loop).

## Layout

- `specs/*` — acceptance source of truth (WHAT must hold). `specs/tools/`, `specs/fixtures/`.
- `src/*` — implementation; `src/lib/*` is the shared standard library. *(Not created yet — greenfield.)*
- `TODO.md` — the durable, priority-sorted plan; the only state carried between loop iterations. Completed items are marked `- [x]` in place by the build loop (never removed); they are pruned only when the plan loop recreates the file.
- Sibling repos (read-only sources of truth): `../amrex` (device interface), `../weaklib` and `../thornado` (Fortran behavior being reimplemented).

## Build & run

Per `specs/build-integration.md` the target is **AMReX CPU-only, double precision, host execution, no Fortran/Matlab**; the correctness-bearing value type is pinned to `double` (`src/lib/wli_real.H`, `wli::Real`) regardless of `amrex::Real`. AMReX resolves from `../amrex` via `add_subdirectory` (override with `-DWLI_AMREX_ROOT=<path>`). Build dir is `build/` (gitignored); `.build/` is agent scratch, never the build dir.

- **Validate specs:** `AMREX_ROOT=../amrex bash specs/tools/validate_specs.sh`
- **Configure:** `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- **Build the library + suite:** `cmake --build build -j4` (cap `-j`; AMReX is large and uncapped builds OOM this host)
- **Run the regression suite:** `ctest --test-dir build --output-on-failure`
- Host toolchain prerequisites (apt, reinstall if the sandbox resets): `cmake`, `g++`, `libhdf5-dev` (HDF5 C++ bindings at `/usr/include/hdf5/serial/H5Cpp.h`). Never install Fortran/Matlab. Do not enable `AMReX_HDF5` — HDF5 is found independently.

When you learn a concrete build/test command, update this section (see loop rule below) so the next iteration inherits it instead of rediscovering it.

## Self-improvement

If a loop iteration discovers how to compile, run, or test the project — or a correction to anything above — the orchestrator records it here (commands and how-to-run only). Keep it operational and terse; progress belongs in `TODO.md`.

- SSH to GitHub is blocked in this sandbox; `origin` has been switched to HTTPS (`https://github.com/lwJi/WeakLibInterp.git`, proxy injects credentials), so plain `git push`/`git fetch` work. If a remote ever reappears as `git@github.com:…`, switch it back with `git remote set-url origin https://github.com/lwJi/WeakLibInterp.git`.
