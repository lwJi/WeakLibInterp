# WeakLibInterp standard library (src/lib). Links AMReX as the single device
# dependency; AMReX::amrex transitively carries includes, compile defs (incl.
# generated config headers), and link libs.

add_library(wli_lib wli.cpp)

target_include_directories(wli_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(wli_lib PUBLIC AMReX::amrex)
