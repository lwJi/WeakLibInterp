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

Prerequisites: `cmake`, `g++`, `libhdf5-dev`. AMReX resolves from a sibling
checkout by default (override with `-DWLI_AMREX_ROOT=<path>`).

```sh
# Configure
tools/q cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
# Build
tools/q cmake --build build -j4
# Test
tools/q ctest --test-dir build --output-on-failure
```

`tools/q` is the quiet-runner wrapper: it stashes output under `.build/logs/`
and prints one line on success, dumping the full log on failure. See the
`CLAUDE.md` "Build & run" section for details.

## Variants & CI

`docs/BUILD.md` covers the variant build trees (single-precision, MPI,
CUDA/HIP compile-only, installed-AMReX) and the CI job map.

## Cactus thorn

`cactus/thorns/WeakLibInterp/` is an ExternalLibraries-style thorn that
finds-or-builds the installed library; see `specs/cactus-integration.md`.

## License

GNU LGPL-2.1 — see `LICENSE`.
