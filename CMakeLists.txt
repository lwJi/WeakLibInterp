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
# WLI configuration knobs. AMReX is only ever consumed as an installed prefix
# (find_package below); these knobs are requests that the consistency guard
# reconciles against what the prefix records. CPU-only + double precision +
# MPI is the gating test configuration (specs/build-integration.md).
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

# MPI. ON (default) is the gating configuration: an AMReX_MPI=ON prefix
# (CPU-only / double unchanged), the suite running under the MPI launcher with
# the 1/2/4-rank tiers as the standing default suite — matching every real
# consumer (the ET AMReX prefix records MPI ON). OFF is the serial variant in
# its own tree; its bare-name 1-rank runs are the 1-rank ≡ serial cross-check
# (specs/build-integration.md §61).
set(WLI_AMREX_MPI ON CACHE BOOL
    "Consume an AMReX_MPI=ON prefix and run the suite under the MPI launcher")

# Test suite toggle. ON (default) builds and registers the full ctest suite via
# add_subdirectory(test) — byte-for-byte the current default build. OFF is the
# library-only / Cactus consumption path (specs/cactus-integration.md §56,77):
# wli_lib alone is configured, with no test_* targets and no ctest wiring.
set(WLI_BUILD_TESTS ON CACHE BOOL
    "Build and register the ctest suite (OFF = library-only consumption build)")

# ---------------------------------------------------------------------------
# AMReX consumption: installed-prefix only — AMReX is found, never built here
# (specs/cactus-integration.md §53-58,62-63,85; exactly one AMReX per
# executable). Provisioning prefixes is out-of-band: tools/provision-amrex.sh
# builds ../amrex into the shared store ${WLI_AMREX_HOME:-$HOME/wli-amrex}/
# <config>; existing prefixes (the ET thorn's install, CI images) plug in via
# WLI_AMREX_INSTALL_DIR directly.
# ---------------------------------------------------------------------------
set(WLI_AMREX_INSTALL_DIR "" CACHE PATH
    "AMReX install prefix (required); provision one with tools/provision-amrex.sh")

# Preflight: fail with the fix in hand before find_package can produce a less
# helpful error. The validity probe (lib/cmake/AMReX/AMReXConfig.cmake exists)
# is the same check tools/provision-amrex.sh uses to decide a config is
# provisioned, so both ends of the contract agree.
if(WLI_AMREX_INSTALL_DIR STREQUAL ""
   OR NOT EXISTS "${WLI_AMREX_INSTALL_DIR}/lib/cmake/AMReX/AMReXConfig.cmake")
  # Recover the store config name from the prefix path's basename (the presets
  # resolve to $WLI_AMREX_HOME/<config>, so the basename survives even when
  # WLI_AMREX_HOME was never exported); fall back to the default config.
  get_filename_component(_wli_amrex_cfg "${WLI_AMREX_INSTALL_DIR}" NAME)
  if(_wli_amrex_cfg STREQUAL "")
    set(_wli_amrex_cfg "mpi")
  endif()
  message(FATAL_ERROR
      "AMReX prefix missing or invalid at '${WLI_AMREX_INSTALL_DIR}' "
      "(no lib/cmake/AMReX/AMReXConfig.cmake there).\n"
      "Provision it once:\n"
      "    tools/provision-amrex.sh ${_wli_amrex_cfg}\n"
      "which installs into \$WLI_AMREX_HOME/${_wli_amrex_cfg} (WLI_AMREX_HOME "
      "defaults to \$HOME/wli-amrex).\n"
      "Or point WLI_AMREX_HOME at your prefix store, or set "
      "WLI_AMREX_INSTALL_DIR to an existing AMReX install prefix directly.")
endif()

# An MPI-ON prefix's AMReXConfig.cmake runs find_dependency(MPI REQUIRED C
# CXX), and FindMPI can only satisfy the C component when language C is
# enabled — in this CXX-only project it would otherwise fail inside
# find_package(AMReX) with "language C is not enabled", masking the
# consistency guard below as the diagnostic for MPI mismatches. Enable C
# unconditionally (cheap: compiler detection only; nothing compiles as C).
enable_language(C)

# A HIP prefix's AMReXTargets.cmake names hip::device, hip::hiprand,
# roc::rocrand and roc::rocprim in its PUBLIC link interface, but unlike the
# CUDA arm (find_dependency(CUDAToolkit)) AMReXConfig.cmake resolves none of
# them — the consumer must define those imported targets before the include,
# or find_package(AMReX) dies with "hip::device ... not found". Resolve them
# here, gated on the requested backend so NONE/CUDA hosts never need ROCm;
# CMAKE_PREFIX_PATH=/opt/rocm (or a hip toolchain) makes these findable.
if(WLI_GPU_BACKEND STREQUAL "HIP")
  find_package(hip CONFIG REQUIRED)
  find_package(rocrand CONFIG REQUIRED)
  find_package(rocprim CONFIG REQUIRED)
  find_package(hiprand CONFIG REQUIRED)
endif()

# AMReX_* facts are read-only here — never FORCE them.
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

# enable_language(CUDA) at THIS scope for the wli targets, only after the
# guard has confirmed the install is a CUDA build. The helper ships in the
# install's module dir, which AMReXConfig.cmake prepends to
# CMAKE_MODULE_PATH, so include it by bare name.
if(WLI_GPU_BACKEND STREQUAL "CUDA")
  enable_language(CUDA)
  include(AMReXTargetHelpers)
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
# MPI launcher for the suite. The installed prefix's AMReXConfig.cmake already
# find_dependency(MPI)s so wli_lib/tests link MPI transitively via
# AMReX::amrex; the explicit top-level find here is what
# populates MPIEXEC_EXECUTABLE / MPIEXEC_NUMPROC_FLAG. CMAKE_TEST_LAUNCHER
# (CMake >= 3.29) is prepended to every add_test whose COMMAND is an executable
# target, so all 19 test targets run under `mpiexec -n 1` with zero edits to
# test/CMakeLists.txt. Empty when the knob is OFF -> default build unchanged.
# ---------------------------------------------------------------------------
if(WLI_AMREX_MPI)
  # Only the CXX component is requested: this find exists solely for the
  # launcher variables, and MPIEXEC_EXECUTABLE / MPIEXEC_NUMPROC_FLAG are
  # populated regardless of the requested language component. Link wiring
  # comes transitively via AMReX.
  find_package(MPI REQUIRED COMPONENTS CXX)
  set(CMAKE_TEST_LAUNCHER
      "${MPIEXEC_EXECUTABLE};${MPIEXEC_NUMPROC_FLAG};1")
endif()

add_subdirectory(src)
if(WLI_BUILD_TESTS)
  add_subdirectory(test)
endif()
