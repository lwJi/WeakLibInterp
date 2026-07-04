#pragma once

// WeakLibInterp EOS single-point evaluate + evaluate-and-differentiate kernels
// (header-only).
//
// 0-based C++ port of the single-point workhorses
//   weaklib/Distributions/Library/wlInterpolationModule.F90:1640-1707
//   LogInterpolateSingleVariable_3D_Custom_Point( D,T,Y, Ds,Ts,Ys, OS, Table,
//                                                 Interpolant )
//   weaklib/Distributions/Library/wlInterpolationModule.F90:1814-1844
//   LogInterpolateDifferentiateSingleVariable_3D_Custom_Point( D,T,Y, Ds,Ts,Ys,
//                                                 OS, Table, Interpolant,
//                                                 Derivative(1:3) )
// for one EOS dependent variable over the 3D thermodynamic state (rho, T, Ye).
// Interpolation is trilinear in log-stored space with rho and T located in log
// space and Ye in linear space; the physical value is recovered by 10**(...) - OS
// (specs/eos-interpolation.md:66-80). The differentiate kernel additionally
// returns (∂/∂rho, ∂/∂T, ∂/∂Ye) via the per-axis chain-rule scale factors
// aD/aT/aY and the raw TriLinear partials (spec:82-92).
//
// These kernels compose the existing stateless primitives (wli_interp.H bracket/
// delta + TriLinear + partials + recover, wli_index.H flat_index) — they
// re-derive none of the math. They are scalar, allocation-free, have no range
// guards, and let a NaN (from a non-positive rho/T via log10) propagate silently
// (spec:94-99). The value type is wli::Real (= double) throughout, never
// amrex::Real.
//
// Multi-point/array forms are out of scope here; they will reuse this same
// corner gather + wli::TriLinear kernel later.

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

// Value + the three ordered partial derivatives returned together. Order
// matches the Fortran Derivative(1:3) = (∂/∂rho, ∂/∂T, ∂/∂Ye).
struct EosPointDeriv {
  Real value;  // recovered physical value (bit-identical to the evaluate _Point)
  Real dDrho;  // ∂value/∂rho  (Derivative(1))
  Real dDT;    // ∂value/∂T    (Derivative(2))
  Real dDY;    // ∂value/∂Ye   (Derivative(3))
};

// Single-point evaluate-and-differentiate of one EOS dependent variable at raw
// physical query (D, T, Y) = (rho, T, Ye). Same inputs/contract as the evaluate
// kernel above. Shares the identical bracket/delta + 8-corner gather + TriLinear
// + recover sequence so .value is bit-identical to
// EosInterpolateSingleVariable3DPoint, then assembles the three partials from
// the raw TriLinear partials times the per-axis chain-rule scale factors
// (spec:82-92):
//   ∂value/∂X_k = (value + OS) · a_k · (∂/∂d_k of the trilinear-in-log form)
// with a_D = 1/(D·log10(Ds[iD+1]/Ds[iD])), a_T = 1/(T·log10(Ts[iT+1]/Ts[iT]))
// for the LOG axes and a_Y = ln10/(Ys[iY+1]-Ys[iY]) for the LINEAR axis (the
// ln10 factor belongs to the linear axis only). (value + OS) reconstitutes
// 10**(trilinear); it is formed as value+OS (oracle op order), not a second pow.
// No guards: a NaN from a non-positive rho/T propagates through value->recon
// into all three derivatives; a non-positive Ye (linear axis) extrapolates.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
EosPointDeriv EosInterpolateDifferentiateSingleVariable3DPoint(
    Real D, Real T, Real Y,
    Real const* Ds, int nD, Real const* Ts, int nT, Real const* Ys, int nY,
    Real OS, Real const* table) noexcept {
  // Brackets + deltas: rho/T in log space, Ye in linear space.
  auto [iD, dD] = GetIndexAndDeltaLog(D, Ds, nD);
  auto [iT, dT] = GetIndexAndDeltaLog(T, Ts, nT);
  auto [iY, dY] = GetIndexAndDeltaLin(Y, Ys, nY);

  // Gather the 8 corners (identical to the evaluate kernel; dX1=rho least bit).
  amrex::GpuArray<int, 3> const n{nD, nT, nY};
  Real const p000 = table[flat_index<3>({iD,     iT,     iY    }, n)];
  Real const p100 = table[flat_index<3>({iD + 1, iT,     iY    }, n)];
  Real const p010 = table[flat_index<3>({iD,     iT + 1, iY    }, n)];
  Real const p110 = table[flat_index<3>({iD + 1, iT + 1, iY    }, n)];
  Real const p001 = table[flat_index<3>({iD,     iT,     iY + 1}, n)];
  Real const p101 = table[flat_index<3>({iD + 1, iT,     iY + 1}, n)];
  Real const p011 = table[flat_index<3>({iD,     iT + 1, iY + 1}, n)];
  Real const p111 = table[flat_index<3>({iD + 1, iT + 1, iY + 1}, n)];

  // Trilinear-in-log, universal recovery, and the reconstituted 10**(...).
  Real const f =
      TriLinear(p000, p100, p010, p110, p001, p101, p011, p111, dD, dT, dY);
  Real const value = recover(f, OS);
  Real const recon = value + OS;  // == 10**f (oracle op order)

  // Per-axis chain-rule scale factors (spec:87-90). Log axes carry NO ln10;
  // the linear Ye axis carries ln10. aD/aT depend on the query D/T themselves,
  // so they are per-query, never per-cell-cached.
  Real const aD = Real(1) / (D * std::log10(Ds[iD + 1] / Ds[iD]));
  Real const aT = Real(1) / (T * std::log10(Ts[iT + 1] / Ts[iT]));
  Real const aY = ln10 / (Ys[iY + 1] - Ys[iY]);

  // Raw TriLinear partials take the OTHER two deltas (not the differentiated
  // axis's own): dX1 uses (dT,dY); dX2 uses (dD,dY); dX3 uses (dD,dT).
  Real const dDrho =
      recon * aD *
      dTriLineardX1(p000, p100, p010, p110, p001, p101, p011, p111, dT, dY);
  Real const dDT =
      recon * aT *
      dTriLineardX2(p000, p100, p010, p110, p001, p101, p011, p111, dD, dY);
  Real const dDY =
      recon * aY *
      dTriLineardX3(p000, p100, p010, p110, p001, p101, p011, p111, dD, dT);

  return EosPointDeriv{value, dDrho, dDT, dDY};
}

}  // namespace wli
