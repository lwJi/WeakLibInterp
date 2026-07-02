// Scaffold acceptance probe.
//
// Proves, at build+run time, the build-integration contract for this increment:
//  * the value-type header pins `double` (its static_assert compiles here);
//  * the AMReX CPU-only device probe header compiles and collapses to host
//    memory (DeviceVector<double> allocates ordinary host storage);
//  * the C++ HDF5 binding links (H5Cpp is included and a version query runs).

#include <cstdio>
#include <cstdlib>

#include <AMReX_GpuContainers.H>
#include <H5Cpp.h>

#include "wli_real.H"

int main() {
  // Value-type pin: literal double, 8 bytes, independent of amrex::Real.
  static_assert(sizeof(wli::Real) == 8, "wli::Real must be 8-byte double");
  if (sizeof(wli::Real) != 8) {
    std::fprintf(stderr, "FAIL: sizeof(wli::Real) != 8\n");
    return EXIT_FAILURE;
  }

  // CPU-only AMReX: DeviceVector<double> collapses to host memory whose
  // .data() is a valid host pointer.
  amrex::Gpu::DeviceVector<double> v(4);
  double* p = v.data();
  if (p == nullptr) {
    std::fprintf(stderr, "FAIL: DeviceVector<double>.data() is null\n");
    return EXIT_FAILURE;
  }
  for (int i = 0; i < 4; ++i) {
    p[i] = static_cast<double>(i) * 0.5;
  }
  if (p[3] != 1.5) {
    std::fprintf(stderr, "FAIL: host-memory write/read mismatch\n");
    return EXIT_FAILURE;
  }

  // C++ HDF5 binding links: query the library version.
  unsigned maj = 0, min = 0, rel = 0;
  H5::H5Library::getLibVersion(maj, min, rel);

  std::printf(
      "PASS scaffold: wli::Real=double (%zu bytes), "
      "AMReX DeviceVector host ptr ok, HDF5 %u.%u.%u\n",
      sizeof(wli::Real), maj, min, rel);
  return EXIT_SUCCESS;
}
