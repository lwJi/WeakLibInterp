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

# MPI. OFF (default) is the gating serial build: CPU-only AMReX, suite runs on
# host as a lone process. ON builds AMReX with AMReX_MPI=ON (CPU-only / double
# unchanged) in a separate tree and runs the suite under the MPI launcher — a
# second correctness-gating configuration (specs/build-integration.md §61). The
# default build stays MPI-free and byte-for-byte unchanged (knob OFF forces the
# same AMReX_MPI=OFF value as before).
set(WLI_AMREX_MPI OFF CACHE BOOL
    "Build AMReX with MPI (AMReX_MPI=ON) and run the suite under the MPI launcher")

# Test suite toggle. ON (default) builds and registers the full ctest suite via
# add_subdirectory(test) — byte-for-byte the current default build. OFF is the
# library-only / Cactus consumption path (specs/cactus-integration.md §56,77):
# wli_lib alone is configured, with no test_* targets and no ctest wiring.
set(WLI_BUILD_TESTS ON CACHE BOOL
    "Build and register the ctest suite (OFF = library-only consumption build)")

# ---------------------------------------------------------------------------
# AMReX consumption mode. Empty WLI_AMREX_INSTALL_DIR (default) selects the
# sibling-source add_subdirectory flow (byte-for-byte unchanged default build);
# a non-empty prefix selects installed-AMReX mode via find_package(AMReX) — the
# Cactus delivery path (specs/cactus-integration.md §53-58,62-63,85). Exactly
# one AMReX per executable; the two branches are mutually exclusive.
# ---------------------------------------------------------------------------
set(WLI_AMREX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../amrex"
    CACHE PATH "AMReX source tree (sibling-source consumption mode)")
set(WLI_AMREX_INSTALL_DIR "" CACHE PATH
    "AMReX install prefix; empty = sibling-source add_subdirectory, non-empty = installed-AMReX find_package mode")

if(WLI_AMREX_INSTALL_DIR STREQUAL "")
  # -------------------------------------------------------------------------
  # Sibling-source mode: force AMReX's build configuration, then add_subdirectory.
  # These AMReX_* FORCE sets are build options for the nested AMReX build; they
  # are meaningless (and must not appear) in installed mode.
  # -------------------------------------------------------------------------
  set(AMReX_GPU_BACKEND    ${WLI_GPU_BACKEND} CACHE STRING "" FORCE)
  set(AMReX_PRECISION      ${WLI_AMREX_PRECISION} CACHE STRING "" FORCE)
  set(AMReX_MPI            ${WLI_AMREX_MPI} CACHE BOOL   "" FORCE)
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

  # Resolve AMReX from the sibling checkout (test #4). Explicit binary dir
  # because AMReX forbids in-source builds.
  add_subdirectory(${WLI_AMREX_ROOT} ${CMAKE_BINARY_DIR}/amrex)

  # The CUDA helper (setup_target_for_cuda_compilation) is not on the module
  # path in source mode, so include it by explicit path.
  if(WLI_GPU_BACKEND STREQUAL "CUDA")
    include(${WLI_AMREX_ROOT}/Tools/CMake/AMReXTargetHelpers.cmake)
  endif()
else()
  # -------------------------------------------------------------------------
  # Installed-AMReX mode: consume a pre-built prefix (the ET AMReX thorn's
  # install). AMReX_* facts are read-only here — never FORCE them.
  # -------------------------------------------------------------------------
  # An MPI-enabled install's AMReXConfig.cmake runs find_dependency(MPI) with
  # the C component (the ET thorn's bundled build enables C), which hard-fails
  # unless this project has the C language enabled too.
  if(WLI_AMREX_MPI)
    enable_language(C)
  endif()
  find_package(AMReX REQUIRED CONFIG PATHS "${WLI_AMREX_INSTALL_DIR}")

  # Configuration-consistency guard (specs/cactus-integration.md §63): a
  # mismatch between the requested backend/MPI and what the install records is a
  # configure-time hard error naming both values. Runs BEFORE any
  # enable_language(CUDA) so a CUDA-vs-NONE mismatch produces THIS diagnostic
  # rather than a "no CUDA compiler" error on a host without nvcc. Precision is
  # exempt: wli::Real stays double regardless of AMReX_PRECISION.
  if(NOT WLI_GPU_BACKEND STREQUAL AMReX_GPU_BACKEND)
    message(FATAL_ERROR
        "AMReX GPU-backend mismatch: requested WLI_GPU_BACKEND='${WLI_GPU_BACKEND}' "
        "but the install at '${WLI_AMREX_INSTALL_DIR}' records "
        "AMReX_GPU_BACKEND='${AMReX_GPU_BACKEND}'. Point WLI_AMREX_INSTALL_DIR at a "
        "matching install or set WLI_GPU_BACKEND to match.")
  endif()

  # Normalize both MPI settings to a canonical bool before comparing.
  if(WLI_AMREX_MPI)
    set(_wli_mpi_requested ON)
  else()
    set(_wli_mpi_requested OFF)
  endif()
  if(AMReX_MPI)
    set(_wli_mpi_recorded ON)
  else()
    set(_wli_mpi_recorded OFF)
  endif()
  if(NOT _wli_mpi_requested STREQUAL _wli_mpi_recorded)
    message(FATAL_ERROR
        "AMReX MPI mismatch: requested WLI_AMREX_MPI='${_wli_mpi_requested}' but the "
        "install at '${WLI_AMREX_INSTALL_DIR}' records AMReX_MPI='${_wli_mpi_recorded}'. "
        "Point WLI_AMREX_INSTALL_DIR at a matching install or set WLI_AMREX_MPI to match.")
  endif()

  # enable_language(CUDA) at THIS scope for the wli targets (as in source mode),
  # only after the guard has confirmed the install is a CUDA build. The helper
  # ships in the install's module dir, which AMReXConfig.cmake prepends to
  # CMAKE_MODULE_PATH, so include it by bare name.
  if(WLI_GPU_BACKEND STREQUAL "CUDA")
    enable_language(CUDA)
    include(AMReXTargetHelpers)
  endif()
endif()

# ---------------------------------------------------------------------------
# GPU compile setup for wli targets. Under CUDA every target's .cpp sources
# must be compiled as CUDA with AMReX's arch/RDC settings (AMReX's helper does
# both). Under HIP nothing per-target is needed: the -x hip compile options
# propagate transitively via AMReX::amrex -> hip::device. No-op under NONE.
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# MPI launcher for the suite. AMReX's own AMReXParallelBackends.cmake already
# find_package(MPI)s inside add_subdirectory(amrex) so wli_lib/tests link MPI
# transitively via AMReX::amrex; the explicit top-level find here is what
# populates MPIEXEC_EXECUTABLE / MPIEXEC_NUMPROC_FLAG. CMAKE_TEST_LAUNCHER
# (CMake >= 3.29) is prepended to every add_test whose COMMAND is an executable
# target, so all 19 test targets run under `mpiexec -n 1` with zero edits to
# test/CMakeLists.txt. Empty when the knob is OFF -> default build unchanged.
# ---------------------------------------------------------------------------
if(WLI_AMREX_MPI)
  # Only the CXX component is requested: this project enables CXX (project(...
  # CXX)), so a C-component request would fail with "language C is not enabled".
  # MPIEXEC_EXECUTABLE / MPIEXEC_NUMPROC_FLAG are populated regardless of the
  # requested language component. Link wiring comes transitively via AMReX.
  find_package(MPI REQUIRED COMPONENTS CXX)
  set(CMAKE_TEST_LAUNCHER
      "${MPIEXEC_EXECUTABLE};${MPIEXEC_NUMPROC_FLAG};1")
endif()

add_subdirectory(src)
if(WLI_BUILD_TESTS)
  add_subdirectory(test)
endif()
