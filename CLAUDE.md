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
- `TODO.md` — the durable, priority-sorted plan; the only state carried between loop iterations.
- Sibling repos (read-only sources of truth): `../amrex` (device interface), `../weaklib` and `../thornado` (Fortran behavior being reimplemented).

## Build & run

Greenfield: no C++ build system exists yet. Per `specs/build-integration.md` the target is **AMReX CPU-only, double precision, host execution, no Fortran/Matlab**; the correctness-bearing value type is pinned to `double` regardless of `amrex::Real`. AMReX resolves from `../amrex`.

- **Validate specs:** `AMREX_ROOT=../amrex bash specs/tools/validate_specs.sh`
- **Build the library + suite:** _not yet established — fill in when a build system is stood up._
- **Run the regression suite:** _not yet established — fill in when the suite exists._

When you learn a concrete build/test command, update this section (see loop rule below) so the next iteration inherits it instead of rediscovering it.

## Self-improvement

If a loop iteration discovers how to compile, run, or test the project — or a correction to anything above — the orchestrator records it here (commands and how-to-run only). Keep it operational and terse; progress belongs in `TODO.md`.
