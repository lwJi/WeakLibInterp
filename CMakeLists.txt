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

# GPU backend. NONE (default) is the gating configuration: CPU-only AMReX,
# suite runs on host. CUDA and HIP are compile-check configurations (CI builds
# the library + suite with nvcc / amdclang++ to catch missing device
# qualifiers and host-only calls in device code, but nothing executes — ctest
# gates only on the NONE build). HIP additionally requires
# CMAKE_CXX_COMPILER=amdclang++ (or clang++/hipcc); AMReX enforces this.
set(WLI_GPU_BACKEND "NONE" CACHE STRING
    "AMReX GPU backend: NONE (gating; runs ctest), CUDA or HIP (compile check)")
set_property(CACHE WLI_GPU_BACKEND PROPERTY STRINGS NONE CUDA HIP)
if(NOT WLI_GPU_BACKEND MATCHES "^(NONE|CUDA|HIP)$")
  message(FATAL_ERROR
      "WLI_GPU_BACKEND must be NONE, CUDA, or HIP (got '${WLI_GPU_BACKEND}')")
endif()

# Compile-check target architectures (nothing executes under CUDA/HIP CI, so
# these only pick which arch's code paths get compile-checked): A100-class and
# MI250X-class by default.
set(WLI_CUDA_ARCH "8.0"    CACHE STRING "AMReX_CUDA_ARCH for WLI_GPU_BACKEND=CUDA")
set(WLI_AMD_ARCH  "gfx90a" CACHE STRING "AMReX_AMD_ARCH for WLI_GPU_BACKEND=HIP")

set(AMReX_GPU_BACKEND    ${WLI_GPU_BACKEND} CACHE STRING "" FORCE)
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

# Backend-specific knobs that must also precede add_subdirectory. CUDA needs
# enable_language at THIS scope: AMReX's own enable_language(CUDA) runs in its
# subdirectory scope and would not cover the wli targets' CUDA-language sources.
if(WLI_GPU_BACKEND STREQUAL "CUDA")
  set(AMReX_CUDA_ARCH ${WLI_CUDA_ARCH} CACHE STRING "" FORCE)
  enable_language(CUDA)
elseif(WLI_GPU_BACKEND STREQUAL "HIP")
  set(AMReX_AMD_ARCH ${WLI_AMD_ARCH} CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Resolve AMReX from the sibling checkout (test #4). Explicit binary dir
# because AMReX forbids in-source builds.
# ---------------------------------------------------------------------------
set(WLI_AMREX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../amrex"
    CACHE PATH "AMReX source tree")
add_subdirectory(${WLI_AMREX_ROOT} ${CMAKE_BINARY_DIR}/amrex)

# ---------------------------------------------------------------------------
# GPU compile setup for wli targets. Under CUDA every target's .cpp sources
# must be compiled as CUDA with AMReX's arch/RDC settings (AMReX's helper does
# both). Under HIP nothing per-target is needed: the -x hip compile options
# propagate transitively via AMReX::amrex -> hip::device. No-op under NONE.
# ---------------------------------------------------------------------------
if(WLI_GPU_BACKEND STREQUAL "CUDA")
  include(${WLI_AMREX_ROOT}/Tools/CMake/AMReXTargetHelpers.cmake)
endif()
function(wli_gpu_target _target)
  if(WLI_GPU_BACKEND STREQUAL "CUDA")
    setup_target_for_cuda_compilation(${_target})
  endif()
endfunction()

# ---------------------------------------------------------------------------
# HDF5 C++ library — found independently of AMReX (AMReX_HDF5 stays OFF to
# avoid its parallel-HDF5 enforcement). Reading needs the C++ API, no Fortran.
# The C component is requested explicitly: the reader objects reference C API
# entry points (H5check_version, ...) directly, and linkers with default
# --no-copy-dt-needed-entries refuse to resolve them through libhdf5_cpp's
# transitive DSO — the C library must be on the link line itself.
# ---------------------------------------------------------------------------
find_package(HDF5 REQUIRED COMPONENTS C CXX)

enable_testing()

add_subdirectory(src/lib)
add_subdirectory(test)
