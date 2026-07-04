cmake_minimum_required(VERSION 3.20)

project(WeakLibInterp CXX)

# ---------------------------------------------------------------------------
# Language standard (AMReX device headers require C++17+; we use C++20).
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Honor normal variables set before find_package/add_subdirectory for options.
cmake_policy(SET CMP0077 NEW)

# ---------------------------------------------------------------------------
# AMReX build configuration (force-block) — MUST precede add_subdirectory.
# AMReX defaults MPI ON and INSTALL ON; both must be overridden here or the
# MPI find_package fails on this (MPI-less) host. CPU-only + double precision
# is the gating test configuration (specs/build-integration.md).
# ---------------------------------------------------------------------------
# Value precision of amrex::Real. Default DOUBLE is the gating build; SINGLE
# is only for pin-verification (proving wli::Real stays double independently of
# amrex::Real). AMReX FATAL_ERRORs on any value other than SINGLE/DOUBLE.
set(WLI_AMREX_PRECISION "DOUBLE" CACHE STRING
    "AMReX real precision: DOUBLE (gating) or SINGLE (pin-verification)")

set(AMReX_GPU_BACKEND    NONE   CACHE STRING "" FORCE)
set(AMReX_PRECISION      ${WLI_AMREX_PRECISION} CACHE STRING "" FORCE)
set(AMReX_MPI            OFF    CACHE BOOL   "" FORCE)
set(AMReX_FORTRAN        OFF    CACHE BOOL   "" FORCE)
set(AMReX_OMP            OFF    CACHE BOOL   "" FORCE)
set(AMReX_INSTALL        OFF    CACHE BOOL   "" FORCE)

# Trim optional subsystems to cut build time / OOM risk on the RAM-limited host.
set(AMReX_LINEAR_SOLVERS OFF    CACHE BOOL   "" FORCE)
set(AMReX_AMRLEVEL       OFF    CACHE BOOL   "" FORCE)
set(AMReX_PARTICLES      OFF    CACHE BOOL   "" FORCE)
set(AMReX_TINY_PROFILE   OFF    CACHE BOOL   "" FORCE)

# ---------------------------------------------------------------------------
# Resolve AMReX from the sibling checkout (test #4). Explicit binary dir
# because AMReX forbids in-source builds.
# ---------------------------------------------------------------------------
set(WLI_AMREX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../amrex"
    CACHE PATH "AMReX source tree")
add_subdirectory(${WLI_AMREX_ROOT} ${CMAKE_BINARY_DIR}/amrex)

# ---------------------------------------------------------------------------
# HDF5 C++ library — found independently of AMReX (AMReX_HDF5 stays OFF to
# avoid its parallel-HDF5 enforcement). Reading needs the C++ API, no Fortran.
# ---------------------------------------------------------------------------
find_package(HDF5 REQUIRED COMPONENTS CXX)

enable_testing()

add_subdirectory(src/lib)
add_subdirectory(test)
