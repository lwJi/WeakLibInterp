#pragma once

// WeakLibInterp EmAb single-point 4D opacity evaluate kernel (header-only).
//
// 0-based C++ port of the single-point workhorse
//   weaklib/Distributions/Library/wlInterpolationModule.F90:1754-1779
//   LogInterpolateSingleVariable_4D_Custom_Point( LogE,LogD,LogT,Y,
//                                                 LogEs,LogDs,LogTs,Ys,
//                                                 OS, Table, Interpolant )
// whose corner gather + tetralinear + recovery lives in
//   weaklib/Distributions/Library/wlInterpolationUtilitiesModule.F90:729-768
//   LinearInterp4D_4DArray_Point.
// It evaluates one EmAb (emission/absorption) opacity value over the 4D table
// (E, rho, T, Ye) for one neutrino species (specs/opacity-emab-iso.md:49-56,
// :99-114). The same kernel serves the Iso channel on a fixed-(species,moment)
// 4D slice of its 5D table (spec:39, :77).
//
// CRITICAL axis convention (spec:47-56, :68): all three of E, rho, T arrive
// ALREADY LOG10'd by the caller (LogE = LOG10(E), etc.) and are located against
// the already-LOG10'd grids LogEs/LogDs/LogTs; Ye arrives RAW. So the *linear*
// bracket/delta (wli::GetIndexAndDeltaLin) is applied to ALL FOUR coordinates —
// this kernel NEVER calls log10 itself (contrast wli_eos.H, which takes raw
// rho/T and logs internally via GetIndexAndDeltaLog). The value type is
// wli::Real (= double) throughout, never amrex::Real.
//
// This is a compose-only leaf over the existing stateless primitives
// (wli_interp.H GetIndexAndDeltaLin + TetraLinear + recover, wli_index.H
// flat_index); it re-derives none of the math. It is scalar, allocation-free,
// and has no range guards: an out-of-range (E, rho, T, Ye) clamps the bracket
// index to the edge cell but NOT the delta, so it extrapolates linearly from the
// edge cell (spec:120-124). Non-positive E/rho/T means the caller's LOG10
// already produced NaN before the call, which propagates silently through the
// whole chain to Interpolant (spec:125); non-positive Ye is a LINEAR axis, does
// not log, and simply extrapolates rather than producing NaN.
//
// The Iso (isoenergetic scattering) channel's 5D-slice-to-4D sibling
// (IsoInterpolateSingleVariable5DPoint + the IsoOffset 2D offset selector) lives
// in this same file: it reuses the identical GetIndexAndDeltaLin + TetraLinear +
// recover primitives with the same E=dX1/rho=dX2/T=dX3/Ye=dX4 corner-bit mapping,
// differing only in that the 16-corner gather uses wli::flat_index<5> with the
// moment index held FIXED (an in-place strided slice of the 5D buffer, spec:77,
// :154) instead of flat_index<4> (spec:39, :118).
//
// OS is a plain scalar: the caller pre-selects it from the 1D Offsets[nOpacities]
// (EmAb) or 2D Offsets[nOpacities,nMoments] (Iso) by species[/moment] before the
// call (spec:74). No offset array or species index is threaded into this leaf.
//
// Host-level array (multi-point) forms live at the bottom of this file: free
// wli:: launchers that own one amrex::ParallelFor over the _Point cores above,
// consuming a wli::TableView<4> (EmAb) / wli::TableView<5> (Iso) for the flat
// table + extents. They compose the untouched _Point kernels — never duplicate
// the 16-corner gather / TetraLinear math.

#include <AMReX_Array.H>      // AMREX_GPU_HOST_DEVICE / AMREX_FORCE_INLINE / GpuArray
#include <AMReX_GpuLaunch.H>  // amrex::ParallelFor

#include "wli_index.H"
#include "wli_interp.H"
#include "wli_real.H"
#include "wli_table.H"  // wli::TableView<D>

namespace wli {

// Single-point evaluate of one EmAb opacity at pre-LOG10'd (LogE, LogD, LogT)
// and raw (Y = Ye). LogEs/LogDs/LogTs are the strictly-ascending already-LOG10'd
// grid nodes; Ys are the strictly-ascending raw Ye nodes; OS is the additive
// offset for the chosen species; table is the flat column-major log-stored 4D
// table with E fastest-varying, element (iE,iD,iT,iY) at
// table[iE + nE*(iD + nD*(iT + nT*iY))].
//
// Axis mapping to the tetralinear corner bits (dX1 = least-significant / fastest
// corner index): E -> dX1, rho -> dX2, T -> dX3, Ye -> dX4. Boundary policy: the
// linear bracket clamps the lower index to [0,n-2] and leaves the delta
// unclamped, so an out-of-range query extrapolates linearly from the edge cell.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real EmAbInterpolateSingleVariable4DPoint(
    Real LogE, Real LogD, Real LogT, Real Y,
    Real const* LogEs, int nE, Real const* LogDs, int nD,
    Real const* LogTs, int nT, Real const* Ys, int nY,
    Real OS, Real const* table) noexcept {
  // Brackets + deltas: E/rho/T are located with the LINEAR formula on their
  // already-LOG10'd coordinates vs already-LOG10'd grids; Ye linear on raw. NO
  // log10 is taken here (contrast wli_eos.H's GetIndexAndDeltaLog on raw rho/T).
  auto [iE, dE] = GetIndexAndDeltaLin(LogE, LogEs, nE);
  auto [iD, dD] = GetIndexAndDeltaLin(LogD, LogDs, nD);
  auto [iT, dT] = GetIndexAndDeltaLin(LogT, LogTs, nT);
  auto [iY, dY] = GetIndexAndDeltaLin(Y,    Ys,    nY);

  // Gather the 16 corners from the flat column-major table. Corner slot ordering
  // matches wli::TetraLinear: label b1 b2 b3 b4 -> dX1(E) least-significant bit,
  // so p1000 <-> iE+1 and p0001 <-> iY+1.
  amrex::GpuArray<int, 4> const n{nE, nD, nT, nY};
  Real const p0000 = table[flat_index<4>({iE,     iD,     iT,     iY    }, n)];
  Real const p1000 = table[flat_index<4>({iE + 1, iD,     iT,     iY    }, n)];
  Real const p0100 = table[flat_index<4>({iE,     iD + 1, iT,     iY    }, n)];
  Real const p1100 = table[flat_index<4>({iE + 1, iD + 1, iT,     iY    }, n)];
  Real const p0010 = table[flat_index<4>({iE,     iD,     iT + 1, iY    }, n)];
  Real const p1010 = table[flat_index<4>({iE + 1, iD,     iT + 1, iY    }, n)];
  Real const p0110 = table[flat_index<4>({iE,     iD + 1, iT + 1, iY    }, n)];
  Real const p1110 = table[flat_index<4>({iE + 1, iD + 1, iT + 1, iY    }, n)];
  Real const p0001 = table[flat_index<4>({iE,     iD,     iT,     iY + 1}, n)];
  Real const p1001 = table[flat_index<4>({iE + 1, iD,     iT,     iY + 1}, n)];
  Real const p0101 = table[flat_index<4>({iE,     iD + 1, iT,     iY + 1}, n)];
  Real const p1101 = table[flat_index<4>({iE + 1, iD + 1, iT,     iY + 1}, n)];
  Real const p0011 = table[flat_index<4>({iE,     iD,     iT + 1, iY + 1}, n)];
  Real const p1011 = table[flat_index<4>({iE + 1, iD,     iT + 1, iY + 1}, n)];
  Real const p0111 = table[flat_index<4>({iE,     iD + 1, iT + 1, iY + 1}, n)];
  Real const p1111 = table[flat_index<4>({iE + 1, iD + 1, iT + 1, iY + 1}, n)];

  // Tetralinear-in-log then universal recovery 10**(...) - OS. dX1=dE(E),
  // dX2=dD(rho), dX3=dT(T), dX4=dY(Ye).
  Real const f =
      TetraLinear(p0000, p1000, p0100, p1100, p0010, p1010, p0110, p1110,
                  p0001, p1001, p0101, p1101, p0011, p1011, p0111, p1111,
                  dE, dD, dT, dY);
  return recover(f, OS);
}

// Select the scalar Iso additive offset from the 2D Offsets[nOpacities,nMoments]
// (Fortran OS_Iso(iC, iMoment), wlOpacityFieldsModule.f90:148,356). The array is
// species-MAJOR / moment-MINOR, column-major, so the flat 0-based element is
// iSpecies + nOpacities*iMom (spec:74, :118). MUST NOT be transposed: species is
// the fast index. The nMoments extent is unused by the fold but kept as a clean
// shape carrier. The caller passes the returned scalar as OS to
// IsoInterpolateSingleVariable5DPoint (mirroring the EmAb 1D-offset pre-select).
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real IsoOffset(Real const* offsets, int nOpacities, int nMoments,
               int iSpecies, int iMom) noexcept {
  return offsets[flat_index<2>({iSpecies, iMom}, {nOpacities, nMoments})];
}

// Single-point evaluate of one Iso (isoenergetic scattering) kernel moment at
// pre-LOG10'd (LogE, LogD, LogT) and raw (Y = Ye), for a FIXED moment index iMom
// (spec:58-66). This is the 5D-slice-to-4D path (spec:39): the 5D table is stored
// column-major (nE, nMom, nD, nT, nY) with E fastest-varying and moment at axis 1
// (element (iE,iMom,iD,iT,iY) at table[iE + nE*(iMom + nMom*(iD + nD*(iT + nT*iY)))],
// spec:77), so a fixed-iMom slice is STRIDED (D-stride nE*nMom, not nE). Rather
// than gather a contiguous 4D slice, this holds iMom fixed in the flat_index<5>
// index arithmetic (spec:154 implementation freedom) — bit-identical to the EmAb
// 4D kernel on the sliced sub-table because it composes the exact same
// GetIndexAndDeltaLin + TetraLinear + recover primitives with the same
// E=dX1/rho=dX2/T=dX3/Ye=dX4 corner-bit mapping. Only E/rho/T/Ye toggle their +1
// corner offset; iMom is fixed in every corner. OS is the scalar pre-selected by
// the caller from the 2D offsets via IsoOffset (spec:74). Boundary / NaN behavior
// is identical to the EmAb kernel above.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real IsoInterpolateSingleVariable5DPoint(
    Real LogE, Real LogD, Real LogT, Real Y,
    Real const* LogEs, int nE, Real const* LogDs, int nD,
    Real const* LogTs, int nT, Real const* Ys, int nY,
    int iMom, int nMom,
    Real OS, Real const* table) noexcept {
  // Brackets + deltas: identical to the EmAb kernel — E/rho/T linear on their
  // already-LOG10'd coordinates, Ye linear on raw. NO log10 is taken here.
  auto [iE, dE] = GetIndexAndDeltaLin(LogE, LogEs, nE);
  auto [iD, dD] = GetIndexAndDeltaLin(LogD, LogDs, nD);
  auto [iT, dT] = GetIndexAndDeltaLin(LogT, LogTs, nT);
  auto [iY, dY] = GetIndexAndDeltaLin(Y,    Ys,    nY);

  // Gather the 16 corners from the flat column-major 5D table with iMom FIXED
  // (the strided slice). Corner slot ordering matches wli::TetraLinear:
  // dX1(E) least-significant, so p1000 <-> iE+1 and p0001 <-> iY+1.
  amrex::GpuArray<int, 5> const n{nE, nMom, nD, nT, nY};
  Real const p0000 = table[flat_index<5>({iE,     iMom, iD,     iT,     iY    }, n)];
  Real const p1000 = table[flat_index<5>({iE + 1, iMom, iD,     iT,     iY    }, n)];
  Real const p0100 = table[flat_index<5>({iE,     iMom, iD + 1, iT,     iY    }, n)];
  Real const p1100 = table[flat_index<5>({iE + 1, iMom, iD + 1, iT,     iY    }, n)];
  Real const p0010 = table[flat_index<5>({iE,     iMom, iD,     iT + 1, iY    }, n)];
  Real const p1010 = table[flat_index<5>({iE + 1, iMom, iD,     iT + 1, iY    }, n)];
  Real const p0110 = table[flat_index<5>({iE,     iMom, iD + 1, iT + 1, iY    }, n)];
  Real const p1110 = table[flat_index<5>({iE + 1, iMom, iD + 1, iT + 1, iY    }, n)];
  Real const p0001 = table[flat_index<5>({iE,     iMom, iD,     iT,     iY + 1}, n)];
  Real const p1001 = table[flat_index<5>({iE + 1, iMom, iD,     iT,     iY + 1}, n)];
  Real const p0101 = table[flat_index<5>({iE,     iMom, iD + 1, iT,     iY + 1}, n)];
  Real const p1101 = table[flat_index<5>({iE + 1, iMom, iD + 1, iT,     iY + 1}, n)];
  Real const p0011 = table[flat_index<5>({iE,     iMom, iD,     iT + 1, iY + 1}, n)];
  Real const p1011 = table[flat_index<5>({iE + 1, iMom, iD,     iT + 1, iY + 1}, n)];
  Real const p0111 = table[flat_index<5>({iE,     iMom, iD + 1, iT + 1, iY + 1}, n)];
  Real const p1111 = table[flat_index<5>({iE + 1, iMom, iD + 1, iT + 1, iY + 1}, n)];

  // Tetralinear-in-log then universal recovery 10**(...) - OS. dX1=dE(E),
  // dX2=dD(rho), dX3=dT(T), dX4=dY(Ye) — same mapping as EmAb.
  Real const f =
      TetraLinear(p0000, p1000, p0100, p1100, p0010, p1010, p0110, p1110,
                  p0001, p1001, p0101, p1101, p0011, p1011, p0111, p1111,
                  dE, dD, dT, dY);
  return recover(f, OS);
}

// ---------------------------------------------------------------------------
// Host-level array (multi-point) launchers. Each owns ONE amrex::ParallelFor
// over the matching _Point core; the flat table + extents are carried by a
// wli::TableView<D> (EmAb: 4D (nE,nD,nT,nY); Iso: 5D (nE,nMom,nD,nT,nY)), the
// already-LOG10'd axis grids, OS and (Iso) the discrete moment selector iMom are
// batch-shared, and only the query coordinates LogE[p]/LogD[p]/LogT[p]/Y[p] vary
// per point. Captured `[=]` by value, `noexcept`, no allocation in the lambda
// (specs/amrex-device-interface.md:80-88). npoints results written to out[].
// ---------------------------------------------------------------------------

// Array EmAb evaluate over EmAbInterpolateSingleVariable4DPoint.
inline void EmAbInterpolateSingleVariable4D(
    int npoints, Real const* LogE, Real const* LogD, Real const* LogT,
    Real const* Y, Real const* LogEs, Real const* LogDs, Real const* LogTs,
    Real const* Ys, Real OS, TableView<4> const& view, Real* out) noexcept {
  int const nE = view.n[0], nD = view.n[1], nT = view.n[2], nY = view.n[3];
  double const* table = view.data;
  amrex::ParallelFor(npoints, [=] AMREX_GPU_DEVICE (int p) noexcept {
    out[p] = EmAbInterpolateSingleVariable4DPoint(
        LogE[p], LogD[p], LogT[p], Y[p], LogEs, nE, LogDs, nD, LogTs, nT, Ys, nY,
        OS, table);
  });
}

// Array Iso evaluate over IsoInterpolateSingleVariable5DPoint at fixed iMom.
inline void IsoInterpolateSingleVariable5D(
    int npoints, Real const* LogE, Real const* LogD, Real const* LogT,
    Real const* Y, Real const* LogEs, Real const* LogDs, Real const* LogTs,
    Real const* Ys, int iMom, Real OS, TableView<5> const& view,
    Real* out) noexcept {
  int const nE = view.n[0], nMom = view.n[1], nD = view.n[2], nT = view.n[3],
            nY = view.n[4];
  double const* table = view.data;
  amrex::ParallelFor(npoints, [=] AMREX_GPU_DEVICE (int p) noexcept {
    out[p] = IsoInterpolateSingleVariable5DPoint(
        LogE[p], LogD[p], LogT[p], Y[p], LogEs, nE, LogDs, nD, LogTs, nT, Ys, nY,
        iMom, nMom, OS, table);
  });
}

}  // namespace wli
