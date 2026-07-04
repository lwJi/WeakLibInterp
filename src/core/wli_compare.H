#pragma once

// WeakLibInterp shared tolerance-tier comparator.
//
// Single source of truth for the mixed relative/absolute pass-fail predicate
// every interpolation/inversion leaf test reuses. The four named tiers and the
// comparison form are fixed by the cross-cutting numeric contract
// (specs/fortran-parity-and-tolerances.md:94-109):
//
//   | got - expected | <= rtol * |expected| + atol
//
// Tiers (spec:96-101):
//   - rtol_parity  = 1e-12  : a single interpolated value vs. the _Point oracle
//                             (absolute floor atol_default = 1e-30).
//   - rtol_relaxed = 1e-10  : analytic derivatives, inversion-recovered T,
//                             round-trip inversion.
//   - rtol_machine = 1e-14  : self-contained closed-form exactness checks
//                             (affine-in-log, constant, node-identity tables).
//   - (no tolerance)        : boundary index equality and NaN propagation use
//                             exact == / std::isnan, NOT this comparator.
//
// This header only returns the bool; asserting-and-failing is the test's job.

#include <cmath>

#include <AMReX_Array.H>  // AMREX_GPU_HOST_DEVICE / AMREX_FORCE_INLINE

#include "wli_real.H"

namespace wli {

// Default-parity relative tolerance: the definition of correctness for a single
// interpolated value against the named Fortran _Point oracle (spec:98).
AMREX_GPU_HOST_DEVICE inline constexpr Real rtol_parity = Real(1e-12);

// Absolute floor paired with every relative tier (spec:98,109).
AMREX_GPU_HOST_DEVICE inline constexpr Real atol_default = Real(1e-30);

// Relaxed tier for derivatives / inversion (spec:99).
AMREX_GPU_HOST_DEVICE inline constexpr Real rtol_relaxed = Real(1e-10);

// Machine-precision exactness tier (a few ULP) for closed-form checks (spec:100).
AMREX_GPU_HOST_DEVICE inline constexpr Real rtol_machine = Real(1e-14);

// Mixed relative/absolute closeness predicate (spec:103-109):
//   | got - expected | <= rtol * |expected| + atol.
// The absolute floor atol makes comparisons near zero well-defined.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
bool is_close(Real got, Real expected, Real rtol,
              Real atol = atol_default) noexcept {
  return std::abs(got - expected) <= rtol * std::abs(expected) + atol;
}

}  // namespace wli
