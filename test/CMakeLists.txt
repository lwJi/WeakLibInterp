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

# Column-major flat-index round-trip (src/lib/wli_index.H). Header-only helper,
# so this links only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(wli_index_tests test_index_roundtrip.cpp)

target_link_libraries(wli_index_tests PRIVATE wli_lib)

add_test(NAME index_roundtrip COMMAND wli_index_tests)

# Scalar multilinear interpolation core + shared comparator (src/lib/wli_interp.H,
# src/lib/wli_compare.H). Header-only, so links only wli_lib; no HDF5 needed.
add_executable(test_interp_core test_interp_core.cpp)

target_link_libraries(test_interp_core PRIVATE wli_lib)

add_test(NAME interp_core COMMAND test_interp_core)

# Device table-residency / upload helper + metadata convention
# (src/lib/wli_table.H). Header-only, so links only wli_lib; no HDF5 needed.
add_executable(test_table_residency test_table_residency.cpp)

target_link_libraries(test_table_residency PRIVATE wli_lib)

add_test(NAME table_residency COMMAND test_table_residency)

# EOS single-point evaluate kernel (src/lib/wli_eos.H). Header-only, so links
# only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_eos_point test_eos_point.cpp)

target_link_libraries(test_eos_point PRIVATE wli_lib)

add_test(NAME eos_point COMMAND test_eos_point)

# EOS single-point evaluate-and-differentiate kernel (src/lib/wli_eos.H).
# Header-only, so links only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_eos_diff_point test_eos_diff_point.cpp)

target_link_libraries(test_eos_diff_point PRIVATE wli_lib)

add_test(NAME eos_diff_point COMMAND test_eos_diff_point)

# EOS temperature-inversion core, DEY family (src/lib/wli_eos_inversion.H).
# Header-only, so links only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_eos_inversion_dey test_eos_inversion_dey.cpp)

target_link_libraries(test_eos_inversion_dey PRIVATE wli_lib)

add_test(NAME eos_inversion_dey COMMAND test_eos_inversion_dey)

# EOS temperature-inversion family wrappers DPY/DSY + _NoError reporting
# (src/lib/wli_eos_inversion.H). Header-only, so links only wli_lib; no HDF5.
add_executable(test_eos_inversion_family test_eos_inversion_family.cpp)

target_link_libraries(test_eos_inversion_family PRIVATE wli_lib)

add_test(NAME eos_inversion_family COMMAND test_eos_inversion_family)

# EmAb single-point 4D evaluate kernel (src/lib/wli_opacity.H,
# specs/opacity-emab-iso.md). Header-only, synthetic table, so links only
# wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_emab_point test_emab_point.cpp)

target_link_libraries(test_emab_point PRIVATE wli_lib)

add_test(NAME emab_point COMMAND test_emab_point)

# Iso single-point 5D-slice-to-4D evaluate kernel (src/lib/wli_opacity.H,
# specs/opacity-emab-iso.md). Header-only, synthetic 5D table, so links only
# wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_iso_point test_iso_point.cpp)

target_link_libraries(test_iso_point PRIVATE wli_lib)

add_test(NAME iso_point COMMAND test_iso_point)

# NES/Pair aligned 2D2D bilinear single-point evaluate kernel
# (src/lib/wli_opacity.H, specs/opacity-nes-pair.md). Header-only, synthetic 5D
# table, so links only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_nes_pair_point test_nes_pair_point.cpp)

target_link_libraries(test_nes_pair_point PRIVATE wli_lib)

add_test(NAME nes_pair_point COMMAND test_nes_pair_point)

# NES detailed-balance symmetry fill of the upper energy triangle
# (src/lib/wli_opacity.H, specs/opacity-nes-pair.md:148-156). Header-only,
# synthetic 5D table, so links only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_nes_detailed_balance test_nes_detailed_balance.cpp)

target_link_libraries(test_nes_detailed_balance PRIVATE wli_lib)

add_test(NAME nes_detailed_balance COMMAND test_nes_detailed_balance)

# Pair crossing-symmetry fill of the upper energy triangle
# (src/lib/wli_opacity.H, specs/opacity-nes-pair.md:158-174). Header-only,
# synthetic 5D table, so links only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_pair_crossing_symmetry test_pair_crossing_symmetry.cpp)

target_link_libraries(test_pair_crossing_symmetry PRIVATE wli_lib)

add_test(NAME pair_crossing_symmetry COMMAND test_pair_crossing_symmetry)

# Brem aligned summed 2D2D bilinear single-point evaluate kernels
# (src/lib/wli_opacity.H, specs/opacity-brem.md). Header-only, synthetic 5D
# table, so links only wli_lib (AMReX CPU/double); no HDF5 needed.
add_executable(test_brem_point test_brem_point.cpp)

target_link_libraries(test_brem_point PRIVATE wli_lib)

add_test(NAME brem_point COMMAND test_brem_point)

# EOS HDF5 reader structural-conformance self-test (src/lib/wli_io_eos.H,
# specs/table-format-and-io.md:171-175). Parses the committed h5ls snapshot and
# asserts the reader's expected schema matches it; does NOT open any live .h5,
# so it runs without the multi-GB production table present. HDF5 include path
# comes transitively from wli_lib (PUBLIC).
add_executable(test_eos_reader_schema test_eos_reader_schema.cpp)

target_link_libraries(test_eos_reader_schema PRIVATE wli_lib)

target_compile_definitions(test_eos_reader_schema PRIVATE
  WLI_EOS_H5LS="${CMAKE_SOURCE_DIR}/specs/fixtures/wl-EOS-SFHo-15-25-50.h5ls")

add_test(NAME eos_reader_schema COMMAND test_eos_reader_schema)

# Opacity HDF5 reader structural-conformance self-test (src/lib/wli_io_opacity.H,
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
