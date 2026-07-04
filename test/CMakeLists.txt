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
