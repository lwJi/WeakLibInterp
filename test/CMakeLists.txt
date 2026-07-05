# WeakLibInterp regression suite. Links the library + AMReX (CPU/double) + the
# C++ HDF5 library, and runs entirely on host (specs/build-integration.md).

add_executable(wli_tests test_scaffold.cpp)

target_link_libraries(wli_tests PRIVATE wli_lib)

# Link the C++ HDF5 bindings: prefer the imported target, else fall back to
# the FindHDF5 result variables.
if(TARGET hdf5::hdf5_cpp)
  target_link_libraries(wli_tests PRIVATE hdf5::hdf5_cpp)
else()
  target_link_libraries(wli_tests PRIVATE ${HDF5_CXX_LIBRARIES})
  target_include_directories(wli_tests PRIVATE ${HDF5_INCLUDE_DIRS})
endif()

add_test(NAME scaffold COMMAND wli_tests)

# Column-major flat-index round-trip (src/core/wli_index.H). Header-only helper,
# so this links only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(wli_index_tests test_index_roundtrip.cpp)

target_link_libraries(wli_index_tests PRIVATE wli_lib)

add_test(NAME index_roundtrip COMMAND wli_index_tests)

# Scalar multilinear interpolation core + shared comparator (src/core/wli_interp.H,
# src/core/wli_compare.H). Header-only, so links only wli_lib; no HDF5 needed.
add_executable(test_interp_core test_interp_core.cpp)

target_link_libraries(test_interp_core PRIVATE wli_lib)

add_test(NAME interp_core COMMAND test_interp_core)

# Device table-residency / upload helper + metadata convention
# (src/core/wli_table.H). Header-only, so links only wli_lib; no HDF5 needed.
add_executable(test_table_residency test_table_residency.cpp)

target_link_libraries(test_table_residency PRIVATE wli_lib)

add_test(NAME table_residency COMMAND test_table_residency)

# EOS single-point evaluate kernel (src/eos/wli_eos.H). Header-only, so links
# only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_eos_point test_eos_point.cpp)

target_link_libraries(test_eos_point PRIVATE wli_lib)

add_test(NAME eos_point COMMAND test_eos_point)

# EOS single-point evaluate-and-differentiate kernel (src/eos/wli_eos.H).
# Header-only, so links only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_eos_diff_point test_eos_diff_point.cpp)

target_link_libraries(test_eos_diff_point PRIVATE wli_lib)

add_test(NAME eos_diff_point COMMAND test_eos_diff_point)

# EOS temperature-inversion core, DEY family (src/eos/wli_eos_inversion.H).
# Header-only, so links only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_eos_inversion_dey test_eos_inversion_dey.cpp)

target_link_libraries(test_eos_inversion_dey PRIVATE wli_lib)

add_test(NAME eos_inversion_dey COMMAND test_eos_inversion_dey)

# EOS temperature-inversion family wrappers DPY/DSY + _NoError reporting
# (src/eos/wli_eos_inversion.H). Header-only, so links only wli_lib; no HDF5.
add_executable(test_eos_inversion_family test_eos_inversion_family.cpp)

target_link_libraries(test_eos_inversion_family PRIVATE wli_lib)

add_test(NAME eos_inversion_family COMMAND test_eos_inversion_family)

# EmAb single-point 4D evaluate kernel (src/opacity/wli_opacity_emab_iso.H,
# specs/opacity-emab-iso.md). Header-only, synthetic table, so links only
# wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_emab_point test_emab_point.cpp)

target_link_libraries(test_emab_point PRIVATE wli_lib)

add_test(NAME emab_point COMMAND test_emab_point)

# Iso single-point 5D-slice-to-4D evaluate kernel (src/opacity/wli_opacity_emab_iso.H,
# specs/opacity-emab-iso.md). Header-only, synthetic 5D table, so links only
# wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_iso_point test_iso_point.cpp)

target_link_libraries(test_iso_point PRIVATE wli_lib)

add_test(NAME iso_point COMMAND test_iso_point)

# NES/Pair aligned 2D2D bilinear single-point evaluate kernel
# (src/opacity/wli_opacity_nes_pair.H, specs/opacity-nes-pair.md). Header-only, synthetic 5D
# table, so links only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_nes_pair_point test_nes_pair_point.cpp)

target_link_libraries(test_nes_pair_point PRIVATE wli_lib)

add_test(NAME nes_pair_point COMMAND test_nes_pair_point)

# NES detailed-balance symmetry fill of the upper energy triangle
# (src/opacity/wli_opacity_nes_pair.H, specs/opacity-nes-pair.md:148-156). Header-only,
# synthetic 5D table, so links only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_nes_detailed_balance test_nes_detailed_balance.cpp)

target_link_libraries(test_nes_detailed_balance PRIVATE wli_lib)

add_test(NAME nes_detailed_balance COMMAND test_nes_detailed_balance)

# Pair crossing-symmetry fill of the upper energy triangle
# (src/opacity/wli_opacity_nes_pair.H, specs/opacity-nes-pair.md:158-174). Header-only,
# synthetic 5D table, so links only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_pair_crossing_symmetry test_pair_crossing_symmetry.cpp)

target_link_libraries(test_pair_crossing_symmetry PRIVATE wli_lib)

add_test(NAME pair_crossing_symmetry COMMAND test_pair_crossing_symmetry)

# Brem aligned summed 2D2D bilinear single-point evaluate kernels
# (src/opacity/wli_opacity_brem.H, specs/opacity-brem.md). Header-only, synthetic 5D
# table, so links only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_brem_point test_brem_point.cpp)

target_link_libraries(test_brem_point PRIVATE wli_lib)

add_test(NAME brem_point COMMAND test_brem_point)

# EOS HDF5 reader structural-conformance self-test (src/io/wli_io_eos.H,
# specs/table-format-and-io.md:171-175). Parses the committed h5ls snapshot and
# asserts the reader's expected schema matches it; does NOT open any live .h5,
# so it runs without the multi-GB production table present. HDF5 include path
# comes transitively from wli_lib (PUBLIC).
add_executable(test_eos_reader_schema test_eos_reader_schema.cpp)

target_link_libraries(test_eos_reader_schema PRIVATE wli_lib)

target_compile_definitions(test_eos_reader_schema PRIVATE
  WLI_EOS_H5LS="${CMAKE_SOURCE_DIR}/specs/fixtures/wl-EOS-SFHo-15-25-50.h5ls")

add_test(NAME eos_reader_schema COMMAND test_eos_reader_schema)

# Opacity HDF5 reader structural-conformance self-test (src/io/wli_io_opacity.H,
# specs/table-format-and-io.md:171-175). Parses the five committed h5ls snapshots
# (EmAb/Iso/NES/Pair/Brem) and asserts the reader's expected schema matches them,
# including the load-bearing 1D-vs-2D Offsets dimensionality; does NOT open any
# live .h5. HDF5 include path comes transitively from wli_lib (PUBLIC).
add_executable(test_opacity_reader_schema test_opacity_reader_schema.cpp)

target_link_libraries(test_opacity_reader_schema PRIVATE wli_lib)

target_compile_definitions(test_opacity_reader_schema PRIVATE
  WLI_EMAB_H5LS="${CMAKE_SOURCE_DIR}/specs/fixtures/wl-Op-SFHo-15-25-50-E40-EmAb.h5ls"
  WLI_ISO_H5LS="${CMAKE_SOURCE_DIR}/specs/fixtures/wl-Op-SFHo-15-25-50-E40-Iso.h5ls"
  WLI_NES_H5LS="${CMAKE_SOURCE_DIR}/specs/fixtures/wl-Op-SFHo-15-25-50-E40-NES.h5ls"
  WLI_PAIR_H5LS="${CMAKE_SOURCE_DIR}/specs/fixtures/wl-Op-SFHo-15-25-50-E40-Pair.h5ls"
  WLI_BREM_H5LS="${CMAKE_SOURCE_DIR}/specs/fixtures/wl-Op-SFHo-15-25-50-E40-Brem.h5ls")

add_test(NAME opacity_reader_schema COMMAND test_opacity_reader_schema)

# Regression-suite umbrella: the always-on synthetic coverage matrix
# (specs/regression-suite-design.md). Realizes all 8 public device entry points x
# the 4 input regimes + the NES/Pair symmetry-fill closure cells, each asserting
# one thresholded predicate. Header-only synthetic tables, so links only wli_lib
# (AMReX CPU/double); no HDF5 needed.
add_executable(test_coverage_matrix test_coverage_matrix.cpp)

target_link_libraries(test_coverage_matrix PRIVATE wli_lib)

add_test(NAME coverage_matrix COMMAND test_coverage_matrix)

# Deliberate-perturbation meta-test (spec Verification #3): proves the suite's
# assertions are thresholded, not print-only, by injecting a 10x error and
# asserting the comparator flags it. Header-only; no HDF5.
add_executable(test_perturbation_meta test_perturbation_meta.cpp)

target_link_libraries(test_perturbation_meta PRIVATE wli_lib)

add_test(NAME perturbation_meta COMMAND test_perturbation_meta)

# Production-table loader/guard (spec:67,113). Resolves the 6 named .h5 tables
# under $WL_TABLES_ROOT and runs node-identity / boundary / NaN cells via the
# public readers when present; returns 77 (CTest SKIP) when WL_TABLES_ROOT is
# unset or no table is found. Links the C++ HDF5 bindings (mirrors wli_tests)
# because it exercises the live readers.
add_executable(test_production_tables test_production_tables.cpp)

target_link_libraries(test_production_tables PRIVATE wli_lib)

# This is the only test that actually CALLS the live readers (wli_io_*.cpp),
# pulling those objects out of the wli_lib archive; they reference the HDF5 C
# API (H5check_version, ...). The C++ bindings target/list does not transitively
# carry the base C library on this host, so link both the C++ bindings and the
# C library explicitly.
if(TARGET hdf5::hdf5_cpp)
  target_link_libraries(test_production_tables PRIVATE hdf5::hdf5_cpp)
else()
  target_link_libraries(test_production_tables PRIVATE ${HDF5_CXX_LIBRARIES})
  target_include_directories(test_production_tables PRIVATE ${HDF5_INCLUDE_DIRS})
endif()

find_library(WLI_HDF5_C_LIB NAMES hdf5_serial hdf5
  HINTS ${HDF5_CXX_LIBRARY_DIRS} /usr/lib/aarch64-linux-gnu
        /usr/lib/aarch64-linux-gnu/hdf5/serial)
if(WLI_HDF5_C_LIB)
  target_link_libraries(test_production_tables PRIVATE ${WLI_HDF5_C_LIB})
endif()

add_test(NAME production_tables COMMAND test_production_tables)

set_tests_properties(production_tables PROPERTIES SKIP_RETURN_CODE 77)

# MPI root-read + broadcast tests (specs/table-format-and-io.md:183-184). One
# executable, two modes: "open" (non-root ranks never open the .h5; loads are
# byte-identical across ranks) and "fail" (root-side open failure surfaces as
# the same error on every rank, no hang). MPI-only: the executable returns 77
# (CTest SKIP) outside an MPI build, so the default build/ suite stays green.
# It writes a synthetic .h5 (test-only) and calls the live readers, so it links
# the HDF5 C++ bindings + C library exactly like test_production_tables.
add_executable(test_mpi_root_bcast test_mpi_root_bcast.cpp)

target_link_libraries(test_mpi_root_bcast PRIVATE wli_lib)

if(TARGET hdf5::hdf5_cpp)
  target_link_libraries(test_mpi_root_bcast PRIVATE hdf5::hdf5_cpp)
else()
  target_link_libraries(test_mpi_root_bcast PRIVATE ${HDF5_CXX_LIBRARIES})
  target_include_directories(test_mpi_root_bcast PRIVATE ${HDF5_INCLUDE_DIRS})
endif()
if(WLI_HDF5_C_LIB)
  target_link_libraries(test_mpi_root_bcast PRIVATE ${WLI_HDF5_C_LIB})
endif()

# These MUST run at >= 2 ranks, so register a custom multi-rank COMMAND that
# bypasses the global 1-rank CMAKE_TEST_LAUNCHER. In a non-MPI build there is no
# MPI launcher and the executable itself returns 77 (SKIP); register it plainly
# so the default build/ suite still lists (and skips) the tests.
if(WLI_AMREX_MPI)
  add_test(NAME mpi_root_open
    COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 --oversubscribe
            $<TARGET_FILE:test_mpi_root_bcast> open)
  add_test(NAME mpi_root_open_failure
    COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 --oversubscribe
            $<TARGET_FILE:test_mpi_root_bcast> fail)
else()
  add_test(NAME mpi_root_open COMMAND test_mpi_root_bcast open)
  add_test(NAME mpi_root_open_failure COMMAND test_mpi_root_bcast fail)
endif()

set_tests_properties(mpi_root_open mpi_root_open_failure
  PROPERTIES SKIP_RETURN_CODE 77)

# ---------------------------------------------------------------------------
# Full-suite multi-rank launcher wiring (specs/build-integration.md:61,72,82;
# specs/regression-suite-design.md:65-72). In the MPI tree the whole suite must
# pass under the launcher at BOTH 2 and 4 ranks. CMAKE_TEST_LAUNCHER is baked at
# generate time (a single value, `-n 1`) so it cannot vary per ctest run;
# instead we register per-test explicit mpiexec COMMANDs at N=2 and N=4. An
# add_test whose COMMAND is a plain program path (${MPIEXEC_EXECUTABLE}) is NOT
# wrapped by the global CMAKE_TEST_LAUNCHER (only executable *targets* are) —
# exactly why the mpi_root_* tests above bypass the 1-rank launcher. The bare
# 1-rank names (registered above/at file scope) stay untouched as the
# "1-rank MPI == serial" cross-check (build-integration.md:61,72). Absent in
# build/ (knob OFF) -> the serial test list is byte-identical to before.
# --oversubscribe is a literal (MPIEXEC_PREFLAGS/POSTFLAGS are empty in the
# cache) and is required for >1 rank on this core-scarce host.
if(WLI_AMREX_MPI)
  function(wli_add_mpi_ranks _name _exe)
    foreach(_np 2 4)
      add_test(NAME ${_name}_np${_np}
        COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} ${_np} --oversubscribe
                $<TARGET_FILE:${_exe}>)
      # Any target may legitimately SKIP (exit 77): production_tables does when
      # WL_TABLES_ROOT is unset (same env on every forked rank, so the 77 is
      # consistent across ranks); harmless on the pure-host-math targets that
      # never return 77.
      set_tests_properties(${_name}_np${_np} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
  endfunction()

  wli_add_mpi_ranks(scaffold               wli_tests)
  wli_add_mpi_ranks(index_roundtrip        wli_index_tests)
  wli_add_mpi_ranks(interp_core            test_interp_core)
  wli_add_mpi_ranks(table_residency        test_table_residency)
  wli_add_mpi_ranks(eos_point              test_eos_point)
  wli_add_mpi_ranks(eos_diff_point         test_eos_diff_point)
  wli_add_mpi_ranks(eos_inversion_dey      test_eos_inversion_dey)
  wli_add_mpi_ranks(eos_inversion_family   test_eos_inversion_family)
  wli_add_mpi_ranks(emab_point             test_emab_point)
  wli_add_mpi_ranks(iso_point              test_iso_point)
  wli_add_mpi_ranks(nes_pair_point         test_nes_pair_point)
  wli_add_mpi_ranks(nes_detailed_balance   test_nes_detailed_balance)
  wli_add_mpi_ranks(pair_crossing_symmetry test_pair_crossing_symmetry)
  wli_add_mpi_ranks(brem_point             test_brem_point)
  wli_add_mpi_ranks(eos_reader_schema      test_eos_reader_schema)
  wli_add_mpi_ranks(opacity_reader_schema  test_opacity_reader_schema)
  wli_add_mpi_ranks(coverage_matrix        test_coverage_matrix)
  wli_add_mpi_ranks(perturbation_meta      test_perturbation_meta)
  wli_add_mpi_ranks(production_tables      test_production_tables)

  # The root-read/broadcast tests already run at 2 ranks (mpi_root_open*, above);
  # add their 4-rank vehicle so the whole suite genuinely runs at both counts.
  add_test(NAME mpi_root_open_np4
    COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 4 --oversubscribe
            $<TARGET_FILE:test_mpi_root_bcast> open)
  add_test(NAME mpi_root_open_failure_np4
    COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 4 --oversubscribe
            $<TARGET_FILE:test_mpi_root_bcast> fail)
  set_tests_properties(mpi_root_open_np4 mpi_root_open_failure_np4
    PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Compile-check backends (WLI_GPU_BACKEND=CUDA/HIP): compile every suite
# executable with the device compiler so the ParallelFor launches and device
# kernels in the tests are compile-checked too; no-op for the gating NONE
# build (and CI never runs ctest under CUDA/HIP — no GPU on the runners).
get_property(_wli_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_wli_t IN LISTS _wli_test_targets)
  wli_gpu_target(${_wli_t})
endforeach()
