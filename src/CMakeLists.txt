# WeakLibInterp standard library (src/), organized AMReX-style: module
# directories on disk (core/ eos/ opacity/ io/), every module dir on the PUBLIC
# include path, includes stay flat (#include "wli_eos.H") with the wli_ filename
# prefix as the namespace — mirroring AMReX's Src/Base + <AMReX_*.H> convention.
# Links AMReX as the single device dependency; AMReX::amrex transitively carries
# includes, compile defs (incl. generated config headers), and link libs.

add_library(wli_lib core/wli.cpp io/wli_io_eos.cpp io/wli_io_opacity.cpp)

target_include_directories(wli_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/core
  ${CMAKE_CURRENT_SOURCE_DIR}/eos
  ${CMAKE_CURRENT_SOURCE_DIR}/opacity
  ${CMAKE_CURRENT_SOURCE_DIR}/io)

target_link_libraries(wli_lib PUBLIC AMReX::amrex)

# Compile-check backends (WLI_GPU_BACKEND=CUDA/HIP): compile the sources with
# the device compiler; no-op for the gating NONE build.
wli_gpu_target(wli_lib)

# The EOS HDF5 reader (io/wli_io_eos.cpp) needs the C++ HDF5 bindings; expose
# them PUBLIC so test .cpp files including wli_io_eos.H get the HDF5 include
# path transitively. find_package(HDF5 ... COMPONENTS CXX) is called at top
# level before add_subdirectory(src), so the results are already available here.
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

# ---------------------------------------------------------------------------
# Install rules (cactus-integration.md §57, Verification #3): ship wli_lib and
# every public wli_*.H header into a prefix so a consumer thorn builds against
# the installed prefix alone (no source tree). Plain install, no EXPORT set /
# no package-config — the Cactus contract emits raw INCLUDE_DIRECTORY /
# LIBRARY_DIRECTORY / LIBRARY (spec:51), not a find_package(WeakLibInterp)
# path, and the HDF5 raw-path fallback above cannot be re-expressed as a
# find_dependency(). Hardcoded lib/ + include/ match the sibling-AMReX layout.
install(TARGETS wli_lib
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)

# Flatten each module dir into a single include/ (trailing slash on each source
# dir drops the module-name component), preserving the #include "wli_eos.H"
# convention. FILES_MATCHING keeps only public headers; the *_detail.H EXCLUDE
# drops the two internal-only headers (io/wli_io_hdf5_detail.H,
# io/wli_io_bcast_detail.H) that pull <H5Cpp.h>/AMReX internals and are used
# only from the .cpp TUs, never from a public header. Result: 13 public headers.
install(DIRECTORY core/ eos/ opacity/ io/
  DESTINATION include
  FILES_MATCHING
    PATTERN "wli_*.H"
    PATTERN "*_detail.H" EXCLUDE)
