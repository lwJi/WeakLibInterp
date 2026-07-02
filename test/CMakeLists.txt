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
