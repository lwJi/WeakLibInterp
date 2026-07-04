#pragma once

// WeakLibInterp correctness-bearing value type.
//
// This type is pinned to the literal C++ `double` (weaklib `dp = 8`),
// INDEPENDENTLY of how `amrex::Real` is configured. A single-precision AMReX
// build (`amrex::Real = float`) must never be able to silently degrade the
// interpolator's bit-level parity: its tables and entry points still use
// `double`, and the on-disk reader contract stays `H5T_NATIVE_DOUBLE`.
// See specs/build-integration.md (value-type pin).

#include <type_traits>

namespace wli {

using Real = double;

}  // namespace wli

static_assert(std::is_same_v<wli::Real, double>,
              "WeakLibInterp value type must be double, not amrex::Real");
