// test_wli.cxx — the scratch consumer that makes the in-Cactus link assertion
// real (specs/cactus-integration.md V#5).
//
// CI's scratch-TU pattern, thorn-ified: the flat installed umbrella headers
// must compile with this thorn's own compiler (spec:67 — consumers compile
// the device headers themselves), one address-taken _Point per family behind
// a never-true guard forces that compile without any table data (the entry
// points are AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE), and
// wli::wli_value_type_size() — the one .cpp-defined symbol — is what forces
// the linker to actually pull libwli_lib.a members into cactus_wli, so
// tools/cactus-build.sh can assert the link at symbol level with nm.

#include <cctk.h>

#include "wli_eos.H"
#include "wli_eos_inversion.H"
#include "wli_opacity.H"

// The only symbol libwli_lib.a defines out-of-line; deliberately not declared
// in any public header (same declaration CI's scratch TU carries).
namespace wli {
int wli_value_type_size();
}

extern "C" int TestWeakLibInterp_Startup(void) {
  // Never-true guard: odr-use one _Point entry point per family so the
  // header-inline device surface compiles, without ever executing (no tables
  // exist at startup, and none are needed).
  static volatile int never = 0;
  if (never) {
    (void)&wli::EosInterpolateSingleVariable3DPoint;
    (void)&wli::ComputeTemperatureWith_DEY_NoGuess;
    (void)&wli::EmAbInterpolateSingleVariable4DPoint;
    (void)&wli::IsoInterpolateSingleVariable5DPoint;
    (void)&wli::NESPairInterpolateSingleVariable2D2DAlignedPoint;
    (void)&wli::BremInterpolateSingleDensity2DAlignedPoint;
  }

  if (wli::wli_value_type_size() == 8) {
    CCTK_INFO("WeakLibInterp linked: wli::Real is double-sized (8 bytes)");
  } else {
    CCTK_ERROR("WeakLibInterp value-type size is not 8 bytes — wli::Real is "
               "not double");
  }
  return 0;
}
