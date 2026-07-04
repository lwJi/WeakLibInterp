#pragma once

// WeakLibInterp Brem (nucleon-nucleon bremsstrahlung) single-point aligned
// opacity kernels (header-only): the single-effective-density (rho, T) bilinear
// inner kernel and the summed effective-density-decomposition wrapper over it.
// Contract: specs/opacity-brem.md. Like every opacity leaf, these are scalar,
// allocation-free, guard-free compose-only ports over the wli_interp.H /
// wli_index.H primitives; coordinates arrive ALREADY LOG10'd (no log10 is taken
// here), boundary policy is clamp-index-not-delta (edge-cell extrapolation), and
// NaN propagates silently. Offset selection stays caller-side via the
// channel-agnostic wli::IsoOffset (wli_opacity_emab_iso.H). Unlike NES/Pair
// (wli_opacity_nes_pair.H) there is NO symmetry fill: every (iE', iE) entry is
// computed independently, in both energy triangles.

#include <AMReX_Array.H>  // AMREX_GPU_HOST_DEVICE / AMREX_FORCE_INLINE / GpuArray

#include "wli_index.H"
#include "wli_interp.H"
#include "wli_real.H"

namespace wli {

// Single-effective-density inner kernel for the Brem (nucleon-nucleon
// bremsstrahlung) two-energy scattering channel: one bilinear-in-(log10 rho,
// log10 T) interpolation of the kernel sub-table at the fixed energy indices
// (iEp, iE) and fixed moment slice, recovered to one physical value
// (specs/opacity-brem.md:126-153). It ports the scalar core
//   weaklib/.../wlInterpolationUtilitiesModule.F90:602-627
//   LinearInterp2D_4DArray_2DAligned_Point
// (p00..p11 = Table(iX1,iX2,iY1[+1],iY2[+1]) at fixed (iX1,iX2)=(iEp,iE);
// 10**BiLinear(...)-OS), which is the once-per-effective-density body the summed
// aligned routine
//   weaklib/.../wlInterpolationModule.F90:1488-1595
//   SumLogInterpolateSingleVariable_2D2D_Custom_Aligned
// invokes. This is the device-callable "aligned" path: the two neutrino-energy
// axes (E', E) and the moment index are NOT interpolated — the consumer's energy
// quadrature alignment supplies (iEp, iE) as direct table indices and moment
// selects a discrete Legendre slice — so only the 2D thermodynamic plane is
// interpolated.
//
// CRITICAL axis order (the Brem-specific copy-paste hazard vs NES/Pair): the
// interpolated thermodynamic axes are (rho, T) — density BEFORE temperature —
// NOT (T, eta). The 5D table is stored column-major (nEp, nE, nMom, nD, nT) with
// E' fastest-varying and the last two axes (rho, T) (spec:71,:104). The corner
// gather holds iEp, iE and moment FIXED and toggles iD then iT, and the BiLinear
// delta order is (dD first, dT second) — dX1 = dD(density), dX2 = dT(temperature)
// (spec:145,:149; oracle nests Linear(Linear(p00,p10,dD),Linear(p01,p11,dD),dT)).
// Getting the extents order or the BiLinear delta order wrong silently transposes
// rho/T.
//
// CRITICAL units (spec:88-95,:189): rho is in g cm^-3 and T is in K (the
// EOS/EmAb/Iso thermodynamic convention), NOT the NES/Pair MeV convention. Both
// LogD and LogT arrive ALREADY LOG10'd by the caller and are located against the
// already-LOG10'd grids LogDs / LogTs with the LINEAR bracket/delta
// (wli::GetIndexAndDeltaLin) — this kernel NEVER calls log10 itself. Non-positive
// rho or T means the caller's LOG10 already produced NaN before the call, which
// propagates silently through BiLinear/recover to the result (spec:183).
// Boundary policy is clamp-index-not-delta: an out-of-range (rho, T)
// extrapolates linearly from the edge cell (spec:182). No range guard on
// iEp/iE/moment (spec:184). OS is a plain scalar the caller pre-selects from the
// 2D Offsets[nOpacities, nMoments] (reuse wli::IsoOffset from
// wli_opacity_emab_iso.H, channel-agnostic; nOpacities = 1 in the pinned table);
// the SAME OS applies to all three effective densities (spec:102).
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real BremInterpolateSingleDensity2DAlignedPoint(
    Real LogD, Real LogT, Real const* LogDs, int nD, Real const* LogTs, int nT,
    int iEp, int iE, int nEp, int nE, int moment, int nMom, Real OS,
    Real const* table) noexcept {
  // Brackets + deltas: both rho and T are located with the LINEAR formula on
  // their already-LOG10'd coordinates vs already-LOG10'd grids. NO log10 here.
  auto [iD, dD] = GetIndexAndDeltaLin(LogD, LogDs, nD);
  auto [iT, dT] = GetIndexAndDeltaLin(LogT, LogTs, nT);

  // Gather the 4 corners from the flat column-major 5D table with the energy
  // indices (iEp, iE) and the moment index held FIXED (the aligned slice); only
  // iD / iT toggle their +1 corner offset. Extents are (nEp, nE, nMom, nD, nT):
  // rho is axis 3, T is axis 4. Corner order matches wli::BiLinear: p00/p10/
  // p01/p11 with dX1 = dD(density) least-significant.
  amrex::GpuArray<int, 5> const n{nEp, nE, nMom, nD, nT};
  Real const p00 = table[flat_index<5>({iEp, iE, moment, iD,     iT    }, n)];
  Real const p10 = table[flat_index<5>({iEp, iE, moment, iD + 1, iT    }, n)];
  Real const p01 = table[flat_index<5>({iEp, iE, moment, iD,     iT + 1}, n)];
  Real const p11 = table[flat_index<5>({iEp, iE, moment, iD + 1, iT + 1}, n)];

  // Bilinear-in-log then universal recovery 10**(...) - OS. dX1 = dD (density),
  // dX2 = dT (temperature) — matches spec:145,:149 and the oracle nesting.
  Real const f = BiLinear(p00, p10, p01, p11, dD, dT);
  return recover(f, OS);
}

// Summed aligned _Point evaluate of one Brem kernel value at the fixed energy
// indices (iEp, iE) and fixed moment slice: the fixed-weight effective-density
// decomposition over nSpecies effective densities
// (specs/opacity-brem.md:155-170). It ports the summed aligned routine
//   weaklib/.../wlInterpolationModule.F90:1488-1595
//   SumLogInterpolateSingleVariable_2D2D_Custom_Aligned
// which invokes the single-effective-density inner kernel above once per
// species and accumulates SumInterp = Sum_l Alpha(l) * Interp_l. For Brem
// nSpecies = 3 and Alpha_Brem = [1, 1, 28/3] over the three effective densities
// rho*xp, rho*xn, rho*sqrt(xp*xn) (spec:18,:58); those densities and weights are
// CALLER-supplied (LogD is a length-nSpecies array of the already-LOG10'd
// effective densities), keeping this primitive channel-neutral in the number of
// species and the weight values.
//
// CRITICAL order of operations (spec:153,:192; oracle :1559-1564): the offset
// recovery 10**(...) - OS happens INSIDE the inner call — per effective density,
// BEFORE weighting — and the weights Alpha multiply the RECOVERED physical
// values, never the log-space values. The SAME moment/OS/(iEp,iE) sub-table and
// the same LogDs/LogTs grids are used for every effective density; only iD(l) /
// dD(l) vary (LogT / iT / dT are effectively shared — the inner call re-brackets
// T on the same grid each iteration, which is exact). NO symmetry fill /
// transpose: every (iEp, iE) entry is computed independently by this same
// decomposition, in both energy triangles (spec:172-176) — the explicit contrast
// with NES detailed balance / Pair crossing. Boundary/NaN behavior is inherited
// per effective density from the inner kernel (spec:182-183).
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real BremInterpolateSingleVariable2D2DAlignedSummedPoint(
    Real const* LogD, Real const* Alpha, int nSpecies, Real LogT,
    Real const* LogDs, int nD, Real const* LogTs, int nT, int iEp, int iE,
    int nEp, int nE, int moment, int nMom, Real OS,
    Real const* table) noexcept {
  Real sum = Real(0);
  for (int l = 0; l < nSpecies; ++l) {
    // Recovery (- OS) is INSIDE this call: Interp_l is a recovered physical
    // value; Alpha[l] weights the recovered value, not the log-space value.
    sum += Alpha[l] * BremInterpolateSingleDensity2DAlignedPoint(
                          LogD[l], LogT, LogDs, nD, LogTs, nT, iEp, iE, nEp, nE,
                          moment, nMom, OS, table);
  }
  return sum;
}

}  // namespace wli
