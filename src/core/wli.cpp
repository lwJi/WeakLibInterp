// Compiled translation unit for the WeakLibInterp standard library target.
// The value-type pin lives in the header; this TU exists so `wli_lib` has an
// object to build and link. The interpolator entry points are header-only by
// design (AMREX_GPU_HOST_DEVICE, in src/eos/wli_eos.H and
// src/opacity/wli_opacity_*.H), so they never compile into this TU.

#include "wli_real.H"

namespace wli {

// Placeholder-free anchor: a real, exported symbol proving the library links.
// Returns the byte size of the pinned value type (8 for double).
int wli_value_type_size() { return static_cast<int>(sizeof(Real)); }

}  // namespace wli
