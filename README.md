# WeakLibInterp

[![CI](https://github.com/lwJi/WeakLibInterp/actions/workflows/ci.yml/badge.svg)](https://github.com/lwJi/WeakLibInterp/actions/workflows/ci.yml)

A GPU-friendly C++ reimplementation of weaklib's equation-of-state (EOS) and
opacity interpolators, exposed as AMReX-native device functions.

The code follows an AMReX-style convention: the `wli::` namespace mirrors the
`wli_` filename prefix, and the correctness-bearing value type `wli::Real`
(`src/core/wli_real.H`) is pinned to `double` independently of `amrex::Real`.
EOS interpolation is trilinear in `(rho, T, Ye)` with evaluate,
evaluate-and-differentiate, and temperature-inversion (from energy) variants;
opacity covers EmAb+Iso, NES/Pair, and Brem under the `wli_opacity.H` umbrella.
Tables are uploaded once as flat device vectors accessed through a lightweight
`TableView`.

## Specifications

`specs/README.md` is the acceptance source of truth — it indexes what must hold
for every module. Consult it before implementing or changing behavior.

## Build & test

Prerequisites: `cmake`, `g++`, `libhdf5-dev`, and OpenMPI for the MPI-ON
default build. AMReX is consumed as an installed prefix; provision one from a
sibling `../amrex` checkout first.

```sh
# Provision an AMReX prefix (once; installs to ~/wli-amrex/mpi)
tools/provision-amrex.sh mpi
# Build (default preset)
tools/build.sh
# Test (full suite, incl. the 1/2/4-rank MPI tiers)
tools/test.sh
```

Every build flag lives in `CMakePresets.json`; the scripts route output
through `tools/q`, the quiet-runner wrapper (one line on success, full log
dump on failure, logs under `.scratch/logs/`). See the `CLAUDE.md` "Build & run"
section for details.

## Variants & CI

`docs/BUILD.md` covers the preset vocabulary (serial, single-precision,
CUDA/HIP compile-only, library-only), the AMReX prefix store, and the CI job
map.

## Cactus thorn

`cactus/thorns/WeakLibInterp/` is an ExternalLibraries-style thorn that
finds-or-builds the installed library; see `specs/cactus-integration.md`.

## License

GNU LGPL-2.1 — see `LICENSE`.
