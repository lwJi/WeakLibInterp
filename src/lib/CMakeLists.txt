# WeakLibInterp standard library (src/lib). Links AMReX as the single device
# dependency; AMReX::amrex transitively carries includes, compile defs (incl.
# generated config headers), and link libs.

add_library(wli_lib wli.cpp wli_io_eos.cpp wli_io_opacity.cpp)

target_include_directories(wli_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(wli_lib PUBLIC AMReX::amrex)

# Compile-check backends (WLI_GPU_BACKEND=CUDA/HIP): compile the sources with
# the device compiler; no-op for the gating NONE build.
wli_gpu_target(wli_lib)

# The EOS HDF5 reader (wli_io_eos.cpp) needs the C++ HDF5 bindings; expose them
# PUBLIC so test .cpp files including wli_io_eos.H get the HDF5 include path
# transitively. find_package(HDF5 ... COMPONENTS CXX) is called at top level
# before add_subdirectory(src/lib), so the results are already available here.
if(TARGET hdf5::hdf5_cpp)
  target_link_libraries(wli_lib PUBLIC hdf5::hdf5_cpp)
else()
  target_link_libraries(wli_lib PUBLIC ${HDF5_CXX_LIBRARIES})
  target_include_directories(wli_lib PUBLIC ${HDF5_INCLUDE_DIRS})
endif()

# The reader objects call HDF5 C API entry points (H5check_version, ...)
# directly; the C++ bindings' DSO dependency on the C library does not put it
# on the link line (default --no-copy-dt-needed-entries), so link the C
# library explicitly and PUBLIC so every suite executable inherits it.
if(TARGET hdf5::hdf5)
  target_link_libraries(wli_lib PUBLIC hdf5::hdf5)
else()
  target_link_libraries(wli_lib PUBLIC ${HDF5_C_LIBRARIES})
endif()
