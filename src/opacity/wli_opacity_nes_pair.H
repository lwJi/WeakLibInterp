#pragma once

// WeakLibInterp NES / Pair single-point aligned opacity kernels (header-only):
// the channel-neutral 2D-aligned bilinear primitive shared by both two-energy
// scattering channels, plus the two upper-triangle symmetry-fill leaves that
// compose it (NES detailed balance, Pair crossing symmetry). Contract:
// specs/opacity-nes-pair.md. Like every opacity leaf, these are scalar,
// allocation-free, guard-free compose-only ports over the wli_interp.H /
// wli_index.H primitives; coordinates arrive ALREADY LOG10'd (no log10 is taken
// here), boundary policy is clamp-index-not-delta (edge-cell extrapolation), and
// NaN propagates silently. Offset selection stays caller-side via the
// channel-agnostic wli::IsoOffset (wli_opacity_emab_iso.H).

#include <cmath>  // std::exp (NES detailed-balance Boltzmann factor)

#include <AMReX_Array.H>      // AMREX_GPU_HOST_DEVICE / AMREX_FORCE_INLINE / GpuArray
#include <AMReX_GpuLaunch.H>  // amrex::ParallelFor

#include "wli_index.H"
#include "wli_interp.H"
#include "wli_real.H"
#include "wli_table.H"  // wli::TableView<D>

namespace wli {

// Single-point evaluate of one NES / Pair two-energy kernel value at the fixed
// energy indices (iEp, iE) and fixed kernel-component index, bilinearly in the
// (log10 T, log10 eta) plane (specs/opacity-nes-pair.md:118-142). This is the
// device-callable "aligned" path: the two neutrino-energy axes (E', E) and the
// kernel-component axis are NOT interpolated — the consumer's energy quadrature
// alignment supplies (iEp, iE) as direct table indices, and kernel selects a
// discrete Legendre/coupling slice — so only the 2D thermodynamic plane is
// interpolated. A SINGLE channel-neutral kernel serves BOTH NES and Pair: the
// two tables are structurally identical, and the caller supplies the table
// pointer + the pre-selected scalar offset OS. It ports the scalar core
//   weaklib/.../wlInterpolationUtilitiesModule.F90:602-627
//   LinearInterp2D_4DArray_2DAligned_Point
// (p00..p11 = Table(iX1,iX2,iY1[+1],iY2[+1]) at fixed (iX1,iX2)=(iEp,iE);
// 10**BiLinear(...)-OS), driven by the aligned _Point routine
//   weaklib/.../wlInterpolationModule.F90:1455-1485.
//
// CRITICAL axis convention (spec:80-86): both LogT and LogX arrive ALREADY
// LOG10'd by the caller (LogT = LOG10(T) with T in MeV for these channels;
// LogX = LOG10(eta)) and are located against the already-LOG10'd grids
// LogTs / LogXs with the LINEAR bracket/delta (wli::GetIndexAndDeltaLin) — this
// kernel NEVER calls log10 itself. Non-positive T or eta means the caller's
// LOG10 already produced NaN before the call, which propagates silently to the
// result (spec:181). Boundary policy is clamp-index-not-delta: an out-of-range
// (T, eta) extrapolates linearly from the edge cell (spec:180).
//
// The 5D table is stored column-major (nEp, nE, nMom, nT, nEta) with E' fastest-
// varying (spec:94). The corner gather holds iEp, iE and kernel FIXED in every
// corner and toggles only iT / iX (this is a BiLinear = 2 toggling axes, not the
// 16-corner tetralinear of the Iso kernel). Corner naming p00/p10/p01/p11 and
// the delta order (dX1 = dT fastest, dX2 = dX(eta)) match wli::BiLinear and the
// Fortran dY1 = dT, dY2 = dX. OS is a plain scalar the caller pre-selects from
// the 2D Offsets[nOpacities, nMoments] (reuse wli::IsoOffset from
// wli_opacity_emab_iso.H, which is channel-agnostic; nOpacities = 1 in the
// pinned tables). No triangle guard on (iEp, iE) and no range guard on
// iEp/iE/kernel are applied: the primitive is a generic corner-fixed bilinear at
// whatever indices it is given, so iEp > iE works; out-of-range integer indices
// are a consumer responsibility (spec:182). The lower/upper-triangle symmetry
// FILLS (NES detailed balance, Pair crossing) are the separate leaves below.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real NESPairInterpolateSingleVariable2D2DAlignedPoint(
    Real LogT, Real LogX, Real const* LogTs, int nT, Real const* LogXs,
    int nEta, int iEp, int iE, int nEp, int nE, int kernel, int nMom, Real OS,
    Real const* table) noexcept {
  // Brackets + deltas: both T and eta are located with the LINEAR formula on
  // their already-LOG10'd coordinates vs already-LOG10'd grids. NO log10 here.
  auto [iT, dT] = GetIndexAndDeltaLin(LogT, LogTs, nT);
  auto [iX, dX] = GetIndexAndDeltaLin(LogX, LogXs, nEta);

  // Gather the 4 corners from the flat column-major 5D table with the energy
  // indices (iEp, iE) and the kernel-component index held FIXED (the aligned
  // slice); only iT / iX toggle their +1 corner offset. Corner order matches
  // wli::BiLinear: p00/p10/p01/p11 with dX1 = dT least-significant.
  amrex::GpuArray<int, 5> const n{nEp, nE, nMom, nT, nEta};
  Real const p00 = table[flat_index<5>({iEp, iE, kernel, iT,     iX    }, n)];
  Real const p10 = table[flat_index<5>({iEp, iE, kernel, iT + 1, iX    }, n)];
  Real const p01 = table[flat_index<5>({iEp, iE, kernel, iT,     iX + 1}, n)];
  Real const p11 = table[flat_index<5>({iEp, iE, kernel, iT + 1, iX + 1}, n)];

  // Bilinear-in-log then universal recovery 10**(...) - OS. dX1 = dT (T),
  // dX2 = dX (eta) — matches Fortran dY1 = dT, dY2 = dX.
  Real const f = BiLinear(p00, p10, p01, p11, dT, dX);
  return recover(f, OS);
}

// NES detailed-balance symmetry fill of the UPPER energy triangle (iEp > iE)
// for one assembled kernel value (specs/opacity-nes-pair.md:148-156). The
// aligned primitive above computes only the LOWER triangle (iEp <= iE); the
// upper triangle is NOT independently interpolated (those table entries may be
// unpopulated) — it is filled from the symmetric lower-triangle value scaled by
// the Boltzmann factor. The exact relation the consumer applies
//   weaklib/.../wlOpacityInterpolationModule.f90:104-115:
//     Phi(iEp, iE) = Phi(iE, iEp) * exp( ( E(iE) - E(iEp) ) / T )   for iEp > iE
// This leaf COMPOSES the primitive rather than re-deriving it: it reads the
// symmetric lower-triangle value by calling
// NESPairInterpolateSingleVariable2D2DAlignedPoint with the two energy indices
// SWAPPED — the primitive's first two energy args are (iEp, iE), so passing
// (iE, iEp) here reads Phi(iE, iEp) — then multiplies by exp((E[iE]-E[iEp])/T).
//
// CRITICAL units/plumbing (spec:80-86, :227): E[] and T are PHYSICAL (MeV),
// NOT log-space — distinct from LogT/LogTs, which the primitive alone consumes.
// This leaf never touches the log grids beyond forwarding them. Energies must be
// strictly increasing with index, so for iEp > iE we have E[iE] < E[iEp] and the
// factor is < 1 (the upper triangle is Boltzmann-suppressed). Subtracting in the
// wrong order gives the reciprocal exp((E[iEp]-E[iE])/T) > 1 — a physics bug the
// closure test guards against.
//
// No triangle guard is applied inside the leaf: the same formula also yields the
// DIAGONAL fixed point iEp == iE (E[iE] == E[iEp] -> factor exp(0) = 1, returning
// the primitive value unchanged, spec:203). The caller applies this fill only for
// iEp >= iE. E is 0-based here (C++), unlike the 1-based Fortran E(iE). This is
// the NES relation ONLY; the Pair crossing-symmetry fill (index + i/ii component
// swap, no Boltzmann factor, spec:158-174) is a separate leaf.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real NESDetailedBalanceFillPoint(
    Real LogT, Real LogX, Real const* LogTs, int nT, Real const* LogXs,
    int nEta, int iEp, int iE, int nEp, int nE, int kernel, int nMom, Real OS,
    Real const* table, Real const* E, Real T) noexcept {
  // Symmetric lower-triangle value Phi(iE, iEp): SWAP the two energy indices.
  Real const lower = NESPairInterpolateSingleVariable2D2DAlignedPoint(
      LogT, LogX, LogTs, nT, LogXs, nEta, iE, iEp, nEp, nE, kernel, nMom, OS,
      table);
  // Boltzmann factor exp((E(iE) - E(iEp)) / T): physical energies + T in MeV.
  return lower * std::exp((E[iE] - E[iEp]) / T);
}

// Pair crossing-symmetry fill of the UPPER energy triangle (iEp > iE) for one
// kernel component (specs/opacity-nes-pair.md:158-174, Bruenn 1985 Eq. C64).
// The aligned primitive above computes only the LOWER triangle (iEp <= iE); the
// upper triangle is NOT independently interpolated — it is filled by a crossing
// symmetry that TRANSPOSES the two energy indices AND EXCHANGES the in-pair /
// cross-pair kernel components (Ji <-> Jii). The exact relation the consumer
// applies, weaklib/.../wlOpacityInterpolationModule.f90:196-228:
//   Phi0(iEp, iE) = C_i * Kernel(iE, iEp, iJii0) + C_ii * Kernel(iE, iEp, iJi0)
//   Phi1(iEp, iE) = C_i * Kernel(iE, iEp, iJii1) + C_ii * Kernel(iE, iEp, iJi1)
// contrasted with the lower triangle, where the components are NOT swapped.
// The defining, machinery-independent statement is Kernel_i(E',E)=Kernel_ii(E,E')
// (spec:174): an EXACT RELABELING — there is NO Boltzmann factor and NO scaling.
//
// This leaf reads ONE kernel component at the swapped (kernelSwapped) index by
// COMPOSING the primitive with the two energy indices SWAPPED — the primitive's
// first two energy args are (iEp, iE), so passing (iE, iEp) here reads the
// transposed entry Kernel(iE, iEp, kernelSwapped). It performs NO component
// selection and applies NO C_i/C_ii weighting (those are consumer assembly data,
// out of the interpolation contract, spec:32,:221): the caller supplies the
// already-swapped kernel index (e.g. iJii0 in place of iJi0) and the OS matching
// THAT swapped kernel (OS is per-(opacity,kernel), spec:92) — mirroring how
// NESDetailedBalanceFillPoint takes a bare int kernel and its own OS.
//
// CRITICAL guard (differs from NES): the diagonal iEp == iE is NOT a fixed point
// here — the Ji <-> Jii component swap is not a no-op on the diagonal, so the
// diagonal stays on the LOWER-triangle (unswapped) path. The caller applies this
// fill ONLY for iEp > iE (oracle: lower triangle iEp=1..iE inclusive of the
// diagonal, f90:160-192). No Boltzmann factor / no E, T params, unlike NES.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real PairCrossingSymmetryFillPoint(
    Real LogT, Real LogX, Real const* LogTs, int nT, Real const* LogXs,
    int nEta, int iEp, int iE, int nEp, int nE, int kernelSwapped, int nMom,
    Real OS, Real const* table) noexcept {
  // Transposed entry Kernel(iE, iEp, kernelSwapped): SWAP the two energy indices
  // and read the caller-supplied swapped kernel component. Exact relabeling.
  return NESPairInterpolateSingleVariable2D2DAlignedPoint(
      LogT, LogX, LogTs, nT, LogXs, nEta, iE, iEp, nEp, nE, kernelSwapped, nMom,
      OS, table);
}

// ---------------------------------------------------------------------------
// Host-level array (multi-point) launchers. Each owns ONE amrex::ParallelFor
// over the matching _Point core; the flat 5D table + extents (nEp,nE,nMom,nT,
// nEta) are carried by a wli::TableView<5>, the already-LOG10'd LogTs/LogXs
// grids, OS and the discrete selectors (iEp,iE,kernel) — plus (NES) the physical
// energies E[] and temperature T — are batch-shared, and only the query
// coordinates LogT[p]/LogX[p] vary per point. Captured `[=]` by value,
// `noexcept`, no allocation in the lambda (specs/amrex-device-interface.md:80-88).
// The wrapper batches over query points at a FIXED energy selector; the
// triangular energy loop / applicability domain stays a caller concern.
// ---------------------------------------------------------------------------

// Array aligned NES/Pair evaluate over NESPairInterpolateSingleVariable2D2DAlignedPoint.
inline void NESPairInterpolateSingleVariable2D2DAligned(
    int npoints, Real const* LogT, Real const* LogX, Real const* LogTs,
    Real const* LogXs, int iEp, int iE, int kernel, Real OS,
    TableView<5> const& view, Real* out) noexcept {
  int const nEp = view.n[0], nE = view.n[1], nMom = view.n[2], nT = view.n[3],
            nEta = view.n[4];
  double const* table = view.data;
  amrex::ParallelFor(npoints, [=] AMREX_GPU_DEVICE (int p) noexcept {
    out[p] = NESPairInterpolateSingleVariable2D2DAlignedPoint(
        LogT[p], LogX[p], LogTs, nT, LogXs, nEta, iEp, iE, nEp, nE, kernel, nMom,
        OS, table);
  });
}

// Array NES detailed-balance fill over NESDetailedBalanceFillPoint. E[] and T are
// PHYSICAL (MeV), batch-shared; only LogT[p]/LogX[p] vary per point.
inline void NESDetailedBalanceFill(
    int npoints, Real const* LogT, Real const* LogX, Real const* LogTs,
    Real const* LogXs, int iEp, int iE, int kernel, Real OS,
    TableView<5> const& view, Real const* E, Real T, Real* out) noexcept {
  int const nEp = view.n[0], nE = view.n[1], nMom = view.n[2], nT = view.n[3],
            nEta = view.n[4];
  double const* table = view.data;
  amrex::ParallelFor(npoints, [=] AMREX_GPU_DEVICE (int p) noexcept {
    out[p] = NESDetailedBalanceFillPoint(
        LogT[p], LogX[p], LogTs, nT, LogXs, nEta, iEp, iE, nEp, nE, kernel, nMom,
        OS, table, E, T);
  });
}

// Array Pair crossing-symmetry fill over PairCrossingSymmetryFillPoint (no E/T).
inline void PairCrossingSymmetryFill(
    int npoints, Real const* LogT, Real const* LogX, Real const* LogTs,
    Real const* LogXs, int iEp, int iE, int kernelSwapped, Real OS,
    TableView<5> const& view, Real* out) noexcept {
  int const nEp = view.n[0], nE = view.n[1], nMom = view.n[2], nT = view.n[3],
            nEta = view.n[4];
  double const* table = view.data;
  amrex::ParallelFor(npoints, [=] AMREX_GPU_DEVICE (int p) noexcept {
    out[p] = PairCrossingSymmetryFillPoint(
        LogT[p], LogX[p], LogTs, nT, LogXs, nEta, iEp, iE, nEp, nE, kernelSwapped,
        nMom, OS, table);
  });
}

}  // namespace wli
