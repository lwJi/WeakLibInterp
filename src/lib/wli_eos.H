#pragma once

// WeakLibInterp EOS single-point evaluate kernel (header-only).
//
// 0-based C++ port of the single-point evaluate workhorse
//   weaklib/Distributions/Library/wlInterpolationModule.F90:1640-1707
//   LogInterpolateSingleVariable_3D_Custom_Point( D,T,Y, Ds,Ts,Ys, OS, Table,
//                                                 Interpolant )
// for one EOS dependent variable over the 3D thermodynamic state (rho, T, Ye).
// Interpolation is trilinear in log-stored space with rho and T located in log
// space and Ye in linear space; the physical value is recovered by 10**(...) - OS
// (specs/eos-interpolation.md:66-80).
//
// This kernel composes the existing stateless primitives (wli_interp.H bracket/
// delta + TriLinear + recover, wli_index.H flat_index) — it re-derives none of
// the math. It is scalar, allocation-free, has no range guards, and lets a NaN
// (from a non-positive rho/T via log10) propagate silently (spec:94-99). The
// value type is wli::Real (= double) throughout, never amrex::Real.
//
// Differentiate and multi-point/array forms are out of scope here; they will
// reuse this same corner gather + wli::TriLinear kernel later.

#include <AMReX_Array.H>  // AMREX_GPU_HOST_DEVICE / AMREX_FORCE_INLINE / GpuArray

#include "wli_index.H"
#include "wli_interp.H"
#include "wli_real.H"

namespace wli {

// Single-point evaluate of one EOS dependent variable at raw physical query
// (D, T, Y) = (rho, T, Ye). Ds/Ts/Ys are the strictly-ascending raw physical
// grid nodes (NOT pre-log10'd; the log axes are logged internally); OS is the
// additive offset; table is the flat column-major log-stored sub-table with rho
// fastest-varying, element (iD,iT,iY) at table[iD + nD*(iT + nT*iY)].
//
// Axis mapping (spec:41-47): rho -> log, dX1 (fastest / least-significant corner
// bit); T -> log, dX2; Ye -> linear, dX3. Boundary policy: the bracket clamps
// the lower index to [0,n-2] and leaves the delta unclamped, so an out-of-range
// query extrapolates linearly from the edge cell (spec:94-98).
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real EosInterpolateSingleVariable3DPoint(
    Real D, Real T, Real Y,
    Real const* Ds, int nD, Real const* Ts, int nT, Real const* Ys, int nY,
    Real OS, Real const* table) noexcept {
  // Brackets + deltas: rho/T in log space, Ye in linear space.
  auto [iD, dD] = GetIndexAndDeltaLog(D, Ds, nD);
  auto [iT, dT] = GetIndexAndDeltaLog(T, Ts, nT);
  auto [iY, dY] = GetIndexAndDeltaLin(Y, Ys, nY);

  // Gather the 8 corners from the flat column-major table. Corner slot ordering
  // matches wli::TriLinear: label b1 b2 b3 -> dX1(rho) least-significant bit.
  amrex::GpuArray<int, 3> const n{nD, nT, nY};
  Real const p000 = table[flat_index<3>({iD,     iT,     iY    }, n)];
  Real const p100 = table[flat_index<3>({iD + 1, iT,     iY    }, n)];
  Real const p010 = table[flat_index<3>({iD,     iT + 1, iY    }, n)];
  Real const p110 = table[flat_index<3>({iD + 1, iT + 1, iY    }, n)];
  Real const p001 = table[flat_index<3>({iD,     iT,     iY + 1}, n)];
  Real const p101 = table[flat_index<3>({iD + 1, iT,     iY + 1}, n)];
  Real const p011 = table[flat_index<3>({iD,     iT + 1, iY + 1}, n)];
  Real const p111 = table[flat_index<3>({iD + 1, iT + 1, iY + 1}, n)];

  // Trilinear-in-log then universal recovery 10**(...) - OS. dX1=dD(rho),
  // dX2=dT(T), dX3=dY(Ye).
  Real const f =
      TriLinear(p000, p100, p010, p110, p001, p101, p011, p111, dD, dT, dY);
  return recover(f, OS);
}

}  // namespace wli
