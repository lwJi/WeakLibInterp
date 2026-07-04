#pragma once

// WeakLibInterp scalar multilinear interpolation math core (header-only).
//
// 0-based C++ port of the stateless scalar primitives in the weaklib oracle
// weaklib/Distributions/Library/wlInterpolationUtilitiesModule.F90 (line refs
// below). Everything is scalar, allocation-free, and callable from an AMReX
// device kernel. The correctness-bearing value type is wli::Real (= double)
// everywhere, never amrex::Real (specs/fortran-parity-and-tolerances.md:36-38).
//
// Design decision (inherited by every EOS/opacity leaf): the multilinear basis
// is expressed as NAMED, RECURSIVELY-FACTORED functions
//   Linear -> BiLinear -> TriLinear -> TetraLinear -> PentaLinear,
// each built from the one-lower function twice plus a Linear over the top axis.
// This mirrors the Fortran factoring (and its float op-order) and gives simple
// closed-form partials. Corners are passed as unpacked scalars, NOT
// pointer+strides: the leaf gathers the 2^D corners via wli::flat_index (out of
// scope here). Corner->delta bit ordering is fixed here: a corner label
// b1 b2 b3 ... maps to argument slot index (b1 + 2*b2 + 4*b3 + ...), i.e. the
// first axis (dX1) is the least-significant bit / fastest-varying corner index,
// consistent with the column-major table layout and the Fortran arg order.
//
// Bracket/delta boundary policy (spec:111-116): the INDEX is clamped to the
// boundary cell [0, n-2]; the delta d is NEVER clamped. A below-range query
// yields d<0, an above-range query yields d>1, and the multilinear kernel turns
// these into linear extrapolation from the edge cell -- not an error, not a
// result clamp, not a NaN. A non-positive log argument yields NaN that
// propagates silently through the whole chain (no guard).
//
// For _Custom routines whose coordinates arrive pre-LOG10'd, apply the *Lin*
// bracket/delta on the already-logarithmic coordinate (equivalent to the Log
// formula on the raw coordinate); the caller/leaf picks Lin vs Log.

#include <cmath>

#include <AMReX_Array.H>  // AMREX_GPU_HOST_DEVICE / AMREX_FORCE_INLINE

#include "wli_real.H"

namespace wli {

// ln10 = LOG(10), the reconstitution constant (weaklib wlInterpolationModule
// F90; spec:83-86). Used by the per-axis derivative chain-rule scale factors.
// Spelled as the correctly-rounded double literal (== std::log(Real(10)),
// asserted in the suite) because nvcc rejects dynamically-initialized
// device-visible globals; constexpr is usable from both host and device.
inline constexpr Real ln10 = 2.30258509299404568401799145468436421;

// -------------------------------------------------------------------------
// Bracket + delta (oracle :104-209)
// -------------------------------------------------------------------------
// Small integer clamp; std::clamp is not device-guaranteed, so keep it local.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
int clamp_index(int i, int lo, int hi) noexcept {
  return i < lo ? lo : (i > hi ? hi : i);
}

// Result of a bracket/delta: the lower node index i in [0, n-2] (clamped) and
// the fractional in-cell delta d (UNCLAMPED; see boundary policy above).
struct IndexDelta {
  int  i;
  Real d;
};

// Linear bracket/delta (oracle Index1D_Lin + GetIndexAndDelta_Lin, :114,178-192).
// Fortran 1-based [1,n-1] maps to 0-based [0,n-2]; the +1/-1 cancels, only the
// node lookups re-index (Xs(1)->Xs[0], Xs(n)->Xs[n-1]). floor-before-cast is
// faithful (truncation is only accidentally safe). Clamp the index only.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
IndexDelta GetIndexAndDeltaLin(Real X, Real const* Xs, int n) noexcept {
  int i = static_cast<int>(
      std::floor((n - 1) * (X - Xs[0]) / (Xs[n - 1] - Xs[0])));
  i = clamp_index(i, 0, n - 2);
  Real d = (X - Xs[i]) / (Xs[i + 1] - Xs[i]);
  return IndexDelta{i, d};
}

// Log bracket/delta (oracle Index1D_Log + GetIndexAndDelta_Log, :151,195-209).
// A non-positive X (or Xs[i]) makes log10 return NaN, which propagates.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
IndexDelta GetIndexAndDeltaLog(Real X, Real const* Xs, int n) noexcept {
  int i = static_cast<int>(std::floor(
      (n - 1) * std::log10(X / Xs[0]) / std::log10(Xs[n - 1] / Xs[0])));
  i = clamp_index(i, 0, n - 2);
  Real d = std::log10(X / Xs[i]) / std::log10(Xs[i + 1] / Xs[i]);
  return IndexDelta{i, d};
}

// -------------------------------------------------------------------------
// Multilinear basis (oracle :212-517). Deltas are the trailing arguments;
// corners are ordered with dX1 as the least-significant bit (see header note).
// -------------------------------------------------------------------------

// Linear (oracle :212-230): compute ddX = 1-d once, matching the Fortran order.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real Linear(Real p0, Real p1, Real dX1) noexcept {
  Real ddX1 = Real(1) - dX1;
  return ddX1 * p0 + dX1 * p1;
}

// BiLinear (oracle :233-251).
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real BiLinear(Real p00, Real p10, Real p01, Real p11, Real dX1,
              Real dX2) noexcept {
  return Linear(Linear(p00, p10, dX1), Linear(p01, p11, dX1), dX2);
}

// TriLinear (oracle :292-310).
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real TriLinear(Real p000, Real p100, Real p010, Real p110, Real p001,
               Real p101, Real p011, Real p111, Real dX1, Real dX2,
               Real dX3) noexcept {
  return Linear(BiLinear(p000, p100, p010, p110, dX1, dX2),
                BiLinear(p001, p101, p011, p111, dX1, dX2), dX3);
}

// TetraLinear (oracle :370-392).
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real TetraLinear(Real p0000, Real p1000, Real p0100, Real p1100, Real p0010,
                 Real p1010, Real p0110, Real p1110, Real p0001, Real p1001,
                 Real p0101, Real p1101, Real p0011, Real p1011, Real p0111,
                 Real p1111, Real dX1, Real dX2, Real dX3, Real dX4) noexcept {
  return Linear(TriLinear(p0000, p1000, p0100, p1100, p0010, p1010, p0110,
                          p1110, dX1, dX2, dX3),
                TriLinear(p0001, p1001, p0101, p1101, p0011, p1011, p0111,
                          p1111, dX1, dX2, dX3),
                dX4);
}

// PentaLinear (oracle :487-517). 32 corners; dX1 least-significant.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real PentaLinear(Real p00000, Real p10000, Real p01000, Real p11000,
                 Real p00100, Real p10100, Real p01100, Real p11100,
                 Real p00010, Real p10010, Real p01010, Real p11010,
                 Real p00110, Real p10110, Real p01110, Real p11110,
                 Real p00001, Real p10001, Real p01001, Real p11001,
                 Real p00101, Real p10101, Real p01101, Real p11101,
                 Real p00011, Real p10011, Real p01011, Real p11011,
                 Real p00111, Real p10111, Real p01111, Real p11111, Real dX1,
                 Real dX2, Real dX3, Real dX4, Real dX5) noexcept {
  return Linear(TetraLinear(p00000, p10000, p01000, p11000, p00100, p10100,
                            p01100, p11100, p00010, p10010, p01010, p11010,
                            p00110, p10110, p01110, p11110, dX1, dX2, dX3, dX4),
                TetraLinear(p00001, p10001, p01001, p11001, p00101, p10101,
                            p01101, p11101, p00011, p10011, p01011, p11011,
                            p00111, p10111, p01111, p11111, dX1, dX2, dX3, dX4),
                dX5);
}

// -------------------------------------------------------------------------
// Analytic partials (raw d/d(delta_k); oracle :254-367). Each is the difference
// of two lower-dim basis evals at the two bracketing values of the
// differentiated axis. The per-axis chain-rule scale factors and the
// (physical+offset) reconstitution are a LATER task (EOS evaluate-and-diff);
// this exposes only the raw partials. Tetra/Penta partials are out of scope.
// -------------------------------------------------------------------------

// d(BiLinear)/d(dX1) (oracle :254-270).
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real dBiLineardX1(Real p00, Real p10, Real p01, Real p11, Real dX2) noexcept {
  return Linear(p10, p11, dX2) - Linear(p00, p01, dX2);
}

// d(BiLinear)/d(dX2) (oracle :273-289).
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real dBiLineardX2(Real p00, Real p10, Real p01, Real p11, Real dX1) noexcept {
  return Linear(p01, p11, dX1) - Linear(p00, p10, dX1);
}

// d(TriLinear)/d(dX1) (oracle :313-329).
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real dTriLineardX1(Real p000, Real p100, Real p010, Real p110, Real p001,
                   Real p101, Real p011, Real p111, Real dX2,
                   Real dX3) noexcept {
  return BiLinear(p100, p110, p101, p111, dX2, dX3) -
         BiLinear(p000, p010, p001, p011, dX2, dX3);
}

// d(TriLinear)/d(dX2) (oracle :332-348).
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real dTriLineardX2(Real p000, Real p100, Real p010, Real p110, Real p001,
                   Real p101, Real p011, Real p111, Real dX1,
                   Real dX3) noexcept {
  return BiLinear(p010, p110, p011, p111, dX1, dX3) -
         BiLinear(p000, p100, p001, p101, dX1, dX3);
}

// d(TriLinear)/d(dX3) (oracle :351-367).
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real dTriLineardX3(Real p000, Real p100, Real p010, Real p110, Real p001,
                   Real p101, Real p011, Real p111, Real dX1,
                   Real dX2) noexcept {
  return BiLinear(p001, p101, p011, p111, dX1, dX2) -
         BiLinear(p000, p100, p010, p110, dX1, dX2);
}

// -------------------------------------------------------------------------
// Recovery (oracle :520-543 etc.): physical = 10**(multilinear_in_log) - OS.
// A NaN interpolant (from a non-positive log argument upstream) propagates
// naturally through pow -- no guard (spec:116).
// -------------------------------------------------------------------------
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real recover(Real f, Real OS) noexcept {
  return std::pow(Real(10), f) - OS;
}

}  // namespace wli
