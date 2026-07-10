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

#include <cmath>  // std::pow (chain-rule scale factors + exponential reconstitution)

#include <AMReX_Array.H>      // AMREX_GPU_HOST_DEVICE / AMREX_FORCE_INLINE / GpuArray
#include <AMReX_GpuLaunch.H>  // amrex::ParallelFor

#include "wli_index.H"
#include "wli_interp.H"
#include "wli_real.H"
#include "wli_table.H"  // wli::TableView<D>

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

// Result of the Brem 2D-aligned evaluate-and-differentiate _Point kernels: the
// recovered value plus its two in-plane partials, in physical-value units per
// unit of each independent variable (spec:67-70). Field naming mirrors
// NESPairPointDeriv but with the Brem axis order (rho BEFORE T): dDrho =
// ∂value/∂rho (DerivativeRho), dDT = ∂value/∂T (DerivativeT). BOTH interpolated
// axes are LOG axes (rho and T arrive pre-LOG10'd), so — like NES/Pair, unlike
// EmAb/Iso's linear Ye — no ln10 survives in either scale factor. For the summed
// wrapper only value/dDT are oracle-matched (populated); dDrho carries the single
// effective-density plane partial ∂Interp_l/∂rho_l for the per-density kernel,
// and is left 0 in the summed struct (the base-density chain-rule assembly
// ∂SumInterp/∂rho = Σ_l Alpha(l)·c_l·dDrho_l needs the caller-supplied c_l and is
// consumer-side, spec:112,:169).
struct BremPointDeriv {
  Real value;  // recovered physical kernel value (bit-identical to the evaluate)
  Real dDrho;  // ∂value/∂rho (DerivativeRho)
  Real dDT;    // ∂value/∂T   (DerivativeT)
};

// Single-effective-density evaluate-and-differentiate of one Brem kernel at the
// fixed (iEp, iE, moment) slice, bilinearly in the (log10 rho, log10 T) plane.
// Same inputs/contract as BremInterpolateSingleDensity2DAlignedPoint above; it
// shares the identical bracket/delta + 4-corner gather + BiLinear + recover
// sequence so .value is bit-identical, then assembles the two partials from the
// raw BiLinear partials times the per-axis chain-rule scale factors
// (spec:79-101; oracle LinearInterpDeriv2D_4DArray_2DAligned_Point,
// wlInterpolationUtilitiesModule.F90:875-909):
//   ∂value/∂X_k = (value + OS) · a_k · (∂/∂d_k of the bilinear-in-log form)
//
// CRITICAL axis order (the Brem transpose vs NES/Pair): axis1 = rho (dX1 = dD),
// axis2 = T (dX2 = dT) — the OPPOSITE of NES/Pair's (T, eta). So the corner
// delta order is BiLinear(...,dD,dT) (identical to the evaluate leaf), and the
// raw partial for each axis takes the OTHER axis's delta: ∂/∂rho (dBiLineardX1)
// uses dT, ∂/∂T (dBiLineardX2) uses dD. (value + OS) reconstitutes 10**(bilinear)
// in the oracle's op order (value+OS, not a second pow).
//
// CRITICAL (differs from EmAb/Iso): BOTH interpolated axes are LOG axes (rho and
// T arrive PRE-LOG10'd, bracketed with the Lin form on the already-log10
// coordinate), so BOTH scale factors use the pure pre-logged form
// a = 1/(X·(LogXs[i+1]-LogXs[i])) with X = 10**(LogX) — NO ln10 survives on
// either axis (there is no linear sibling axis, unlike the EmAb Ye term). aRho
// MUST use THIS effective density's own LogD and its own iD bracket cell (a
// copy-paste from a base rho silently breaks only the scale factor, spec:91,:94).
// No guards: a NaN from a non-positive rho or T propagates through value->recon
// into BOTH derivatives.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
BremPointDeriv BremInterpolateSingleDensityDifferentiate2DAlignedPoint(
    Real LogD, Real LogT, Real const* LogDs, int nD, Real const* LogTs, int nT,
    int iEp, int iE, int nEp, int nE, int moment, int nMom, Real OS,
    Real const* table) noexcept {
  // Brackets + deltas: both rho and T located with the LINEAR formula on their
  // already-LOG10'd coordinates vs already-LOG10'd grids. NO log10 here.
  auto [iD, dD] = GetIndexAndDeltaLin(LogD, LogDs, nD);
  auto [iT, dT] = GetIndexAndDeltaLin(LogT, LogTs, nT);

  // Gather the 4 corners (identical to the evaluate kernel; (iEp,iE,moment)
  // FIXED, only iD/iT toggle; extents (nEp,nE,nMom,nD,nT), rho axis 3, T axis 4).
  amrex::GpuArray<int, 5> const n{nEp, nE, nMom, nD, nT};
  Real const p00 = table[flat_index<5>({iEp, iE, moment, iD,     iT    }, n)];
  Real const p10 = table[flat_index<5>({iEp, iE, moment, iD + 1, iT    }, n)];
  Real const p01 = table[flat_index<5>({iEp, iE, moment, iD,     iT + 1}, n)];
  Real const p11 = table[flat_index<5>({iEp, iE, moment, iD + 1, iT + 1}, n)];

  // Bilinear-in-log (dX1 = dD, dX2 = dT), universal recovery, reconstituted 10**.
  Real const f = BiLinear(p00, p10, p01, p11, dD, dT);
  Real const value = recover(f, OS);
  Real const recon = value + OS;  // == 10**f (oracle op order)

  // Per-axis chain-rule scale factors. BOTH rho and T are PRE-LOG10'd log axes,
  // so both use a = 1/(X·(LogXs[i+1]-LogXs[i])), X = 10**(LogX); no ln10. aRho
  // uses THIS density's own LogD and its own iD cell.
  Real const aRho =
      Real(1) / (std::pow(Real(10), LogD) * (LogDs[iD + 1] - LogDs[iD]));
  Real const aT =
      Real(1) / (std::pow(Real(10), LogT) * (LogTs[iT + 1] - LogTs[iT]));

  // Raw BiLinear partials take the OTHER axis's delta: ∂/∂rho uses dT (dX1),
  // ∂/∂T uses dD (dX2) — the Brem transpose vs the NES/Pair source.
  Real const dDrho = recon * aRho * dBiLineardX1(p00, p10, p01, p11, dT);
  Real const dDT = recon * aT * dBiLineardX2(p00, p10, p01, p11, dD);

  return BremPointDeriv{value, dDrho, dDT};
}

// Summed aligned _Point evaluate-and-differentiate of one Brem kernel over the
// fixed-weight effective-density decomposition (spec:65-70,:104-113). Same
// signature as BremInterpolateSingleVariable2D2DAlignedSummedPoint: the caller
// supplies the length-nSpecies already-LOG10'd effective densities LogD[l] and
// weights Alpha[l], a shared LogT / grids / (iEp,iE,moment) slice / OS. It loops
// l over nSpecies calling the per-density differentiate kernel and accumulates
//   SumInterp     = Σ_l Alpha(l)·Interp_l        (bit-identical to the value leaf)
//   ∂SumInterp/∂T = Σ_l Alpha(l)·∂Interp_l/∂T    (exact by linearity, spec:109)
// Recovery (- OS) happens INSIDE each per-l call BEFORE weighting; Alpha weights
// the RECOVERED physical values and partials, never the log-space form — matching
// the value-path order exactly.
//
// SCOPE (spec:112,:160,:169): ∂SumInterp/∂rho is NOT assembled here. The base-
// density chain rule ∂SumInterp/∂rho = Σ_l Alpha(l)·c_l·∂Interp_l/∂rho_l needs
// the caller-supplied composition factors c_l = {xp, xn, √(xp·xn)} (mass
// fractions held fixed), so it is a consumer-side natural extension. The per-l
// plane partial ∂Interp_l/∂rho_l is exposed by the per-density kernel above; the
// summed struct's dDrho field is left 0.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
BremPointDeriv BremInterpolateSingleVariable2D2DAlignedSummedDifferentiatePoint(
    Real const* LogD, Real const* Alpha, int nSpecies, Real LogT,
    Real const* LogDs, int nD, Real const* LogTs, int nT, int iEp, int iE,
    int nEp, int nE, int moment, int nMom, Real OS,
    Real const* table) noexcept {
  Real value = Real(0);
  Real dDT = Real(0);
  for (int l = 0; l < nSpecies; ++l) {
    // Per effective density: recover + differentiate INSIDE, then Alpha-weight
    // the recovered value and its T-partial (linearity is exact).
    BremPointDeriv const d = BremInterpolateSingleDensityDifferentiate2DAlignedPoint(
        LogD[l], LogT, LogDs, nD, LogTs, nT, iEp, iE, nEp, nE, moment, nMom, OS,
        table);
    value += Alpha[l] * d.value;
    dDT += Alpha[l] * d.dDT;
  }
  // dDrho left 0: the base-density chain-rule assembly is consumer-side.
  return BremPointDeriv{value, Real(0), dDT};
}

// ---------------------------------------------------------------------------
// Host-level array (multi-point) launchers. Each owns ONE amrex::ParallelFor
// over the matching _Point core; the flat 5D table + extents (nEp,nE,nMom,nD,nT
// — rho BEFORE T, the Brem-specific axis order) are carried by a
// wli::TableView<5>, the already-LOG10'd LogDs/LogTs grids, OS and the discrete
// selectors (iEp,iE,moment) are batch-shared. For the single-density form only
// LogD[p]/LogT[p] vary; for the summed form the per-species LogD[nSpecies] /
// Alpha[nSpecies] / nSpecies stay batch-shared (the inner core loops species
// internally) and only LogT[p] varies. Captured `[=]` by value, `noexcept`, no
// allocation in the lambda (specs/amrex-device-interface.md:80-88).
// ---------------------------------------------------------------------------

// Array single-effective-density evaluate over BremInterpolateSingleDensity2DAlignedPoint.
inline void BremInterpolateSingleDensity2DAligned(
    int npoints, Real const* LogD, Real const* LogT, Real const* LogDs,
    Real const* LogTs, int iEp, int iE, int moment, Real OS,
    TableView<5> const& view, Real* out) noexcept {
  int const nEp = view.n[0], nE = view.n[1], nMom = view.n[2], nD = view.n[3],
            nT = view.n[4];
  double const* table = view.data;
  amrex::ParallelFor(npoints, [=] AMREX_GPU_DEVICE (int p) noexcept {
    out[p] = BremInterpolateSingleDensity2DAlignedPoint(
        LogD[p], LogT[p], LogDs, nD, LogTs, nT, iEp, iE, nEp, nE, moment, nMom,
        OS, table);
  });
}

// Array summed evaluate over BremInterpolateSingleVariable2D2DAlignedSummedPoint.
// LogD[nSpecies]/Alpha[nSpecies]/nSpecies are batch-shared; only LogT[p] varies.
inline void BremInterpolateSingleVariable2D2DAlignedSummed(
    int npoints, Real const* LogD, Real const* Alpha, int nSpecies,
    Real const* LogT, Real const* LogDs, Real const* LogTs, int iEp, int iE,
    int moment, Real OS, TableView<5> const& view, Real* out) noexcept {
  int const nEp = view.n[0], nE = view.n[1], nMom = view.n[2], nD = view.n[3],
            nT = view.n[4];
  double const* table = view.data;
  amrex::ParallelFor(npoints, [=] AMREX_GPU_DEVICE (int p) noexcept {
    out[p] = BremInterpolateSingleVariable2D2DAlignedSummedPoint(
        LogD, Alpha, nSpecies, LogT[p], LogDs, nD, LogTs, nT, iEp, iE, nEp, nE,
        moment, nMom, OS, table);
  });
}

// Array single-effective-density evaluate-and-differentiate over
// BremInterpolateSingleDensityDifferentiate2DAlignedPoint. out[p] = the
// {value,dDrho,dDT} struct; only LogD[p]/LogT[p] vary per point.
inline void BremInterpolateSingleDensityDifferentiate2DAligned(
    int npoints, Real const* LogD, Real const* LogT, Real const* LogDs,
    Real const* LogTs, int iEp, int iE, int moment, Real OS,
    TableView<5> const& view, BremPointDeriv* out) noexcept {
  int const nEp = view.n[0], nE = view.n[1], nMom = view.n[2], nD = view.n[3],
            nT = view.n[4];
  double const* table = view.data;
  amrex::ParallelFor(npoints, [=] AMREX_GPU_DEVICE (int p) noexcept {
    out[p] = BremInterpolateSingleDensityDifferentiate2DAlignedPoint(
        LogD[p], LogT[p], LogDs, nD, LogTs, nT, iEp, iE, nEp, nE, moment, nMom,
        OS, table);
  });
}

// Array summed evaluate-and-differentiate over
// BremInterpolateSingleVariable2D2DAlignedSummedDifferentiatePoint. The per-
// species LogD[nSpecies]/Alpha[nSpecies]/nSpecies are batch-shared (the inner
// core loops species internally); only LogT[p] varies. out[p] = the
// {value,dDrho(==0),dDT} struct — dDrho stays 0 by design (base-density chain
// rule is consumer-side, spec:104-113).
inline void BremInterpolateSingleVariable2D2DAlignedSummedDifferentiate(
    int npoints, Real const* LogD, Real const* Alpha, int nSpecies,
    Real const* LogT, Real const* LogDs, Real const* LogTs, int iEp, int iE,
    int moment, Real OS, TableView<5> const& view, BremPointDeriv* out) noexcept {
  int const nEp = view.n[0], nE = view.n[1], nMom = view.n[2], nD = view.n[3],
            nT = view.n[4];
  double const* table = view.data;
  amrex::ParallelFor(npoints, [=] AMREX_GPU_DEVICE (int p) noexcept {
    out[p] = BremInterpolateSingleVariable2D2DAlignedSummedDifferentiatePoint(
        LogD, Alpha, nSpecies, LogT[p], LogDs, nD, LogTs, nT, iEp, iE, nEp, nE,
        moment, nMom, OS, table);
  });
}

}  // namespace wli
