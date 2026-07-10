// Self-contained acceptance probe for the Brem (nucleon-nucleon bremsstrahlung)
// aligned, summed two-energy single-point evaluate kernels
//   src/opacity/wli_opacity_brem.H::BremInterpolateSingleDensity2DAlignedPoint (inner)
//   src/opacity/wli_opacity_brem.H::BremInterpolateSingleVariable2D2DAlignedSummedPoint.
//
// Enforces the self-contained regression checks of specs/opacity-brem.md
// (Verification :200-211), whose oracle is the aligned scalar core
//   LinearInterp2D_4DArray_2DAligned_Point (wlInterpolationUtilitiesModule.F90
//   :602-627), driven by the summed aligned routine
//   SumLogInterpolateSingleVariable_2D2D_Custom_Aligned
//   (wlInterpolationModule.F90:1488-1595).
//
// The 5D table is (nEp, nE, nMom, nD, nT), column-major with E' fastest and the
// last two axes (rho, T) — density BEFORE temperature (spec:71,:104), the
// explicit contrast with the NES/Pair (T, eta) layout. At fixed energy indices
// (iEp, iE) and moment index, only the (rho, T) plane is bilinearly interpolated
// in LOG10 space; the energy and moment axes are pure direct-index slices. Both
// rho and T arrive PRE-LOG10'd (spec:88), so queries are fed as log10(rho) /
// log10(T). The physical value is the fixed-weight effective-density
// decomposition SumInterp = Sum_l Alpha(l)*K(rho_l), Alpha_Brem = [1, 1, 28/3].
// The synthetic stored function is DISTINCT per (iEp, iE, moment) triple and
// ASYMMETRIC in (iEp, iE) so an index/slice/transpose/cross-talk bug surfaces.
//
// Required checks (spec:200-211):
//   1. Affine-in-log exactness (machine tier ~1e-14) at an interior (rho, T).
//   2. Node identity (machine tier): LogD/LogT equal to a LOG10'd grid node.
//   3. Moment-slice independence (machine tier): each moment recovers its own
//      function with the matching 2D offset element.
//   4. Effective-density decomposition closure (parity tier): summed routine
//      equals 1*K1 + 1*K2 + (28/3)*K3 over rho*xp, rho*xn, rho*sqrt(xp*xn).
//   5. Weight-order sensitivity (parity tier): permuting Alpha changes the
//      result; only [1,1,28/3] matches the closed form.
//   6. Both-triangles-computed (machine tier): (iEp,iE) and (iE,iEp) each equal
//      their own decomposition on the asymmetric table (no symmetry fill).
//   7. Boundary extrapolation (exact relation): one effective density just
//      outside a rho edge equals the same BiLinear with the unclamped delta.
//   8. NaN propagation (NaN-equality): non-positive effective rho or T -> NaN.
//
// Hand-rolled harness (no GoogleTest/Catch2), synthetic 5D table only (no HDF5),
// no amrex::Initialize (pure host scalar math). Real-table rtol=1e-12 parity
// (vs wl-Op-SFHo-15-25-50-E40-Brem.h5) is covered by run_brem in
// test/test_production_tables.cpp.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "wli_compare.H"
#include "wli_opacity.H"
#include "wli_real.H"

namespace {

using wli::Real;

int g_failures = 0;

void check(bool ok, const char* msg) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  } else {
    std::printf("  ok: %s\n", msg);
  }
}

// Per-(iEp, iE, moment) DISTINCT and (iEp,iE)-ASYMMETRIC affine-in-log model in
// terms of the ALREADY-LOG10'd coordinates (LogD, LogT). A wrong energy index, a
// wrong moment slice, a rho/T transpose, or a stride bug that blends/ignores an
// axis is caught because every triple's base offset + slopes differ and
// affine(i,j,...) != affine(j,i,...). Coefficients are small so the affine LOG-
// space value stays modest across interior + extrapolation queries, keeping
// 10**(affine) in a tier where the 1e-14 relative check holds. The asymmetric
// term 0.019*iEp*iEp (only iEp, squared) breaks (iEp,iE) <-> (iE,iEp) symmetry
// and the distinct dD/dT slopes break a rho/T transpose.
Real affine(int iEp, int iE, int moment, Real LogD, Real LogT) {
  Real const base =
      0.50 + 0.07 * iEp + 0.13 * iE + 0.31 * moment + 0.019 * iEp * iEp;
  Real const bD = 0.09 + 0.017 * iEp - 0.006 * moment;
  Real const bT = -0.05 + 0.011 * iE + 0.008 * moment + 0.004 * iEp;
  return base + bD * LogD + bT * LogT;
}

}  // namespace

int main() {
  // --- Synthetic 5D grid: non-uniform (uneven-ratio) LOG10'd rho and T axes. ---
  const int nEp = 3, nE = 3, nMom = 3, nD = 5, nT = 4;
  const int nOpacities = 1;  // pinned Brem table has nOpacities = 1
  Real LogDs[nD] = {-1.0, -0.4, 0.15, 0.9, 1.7};  // log10 rho (rho in g/cm^3)
  Real LogTs[nT] = {-0.3, 0.15, 0.7, 1.35};       // log10 T (T in K)

  // 5D table, column-major (nEp, nE, nMom, nD, nT): E' fastest, (rho, T) last.
  std::vector<Real> table(static_cast<std::size_t>(nEp) * nE * nMom * nD * nT);
  for (int iT = 0; iT < nT; ++iT)
    for (int iD = 0; iD < nD; ++iD)
      for (int m = 0; m < nMom; ++m)
        for (int j = 0; j < nE; ++j)
          for (int i = 0; i < nEp; ++i)
            table[wli::flat_index<5>({i, j, m, iD, iT},
                                     {nEp, nE, nMom, nD, nT})] =
                affine(i, j, m, LogDs[iD], LogTs[iT]);

  const Real* tbl = table.data();

  // 2D offsets Offsets[nOpacities, nMoments], column-major (species-major /
  // moment-minor). Distinct per moment so a wrong offset element is caught.
  std::vector<Real> offsets(static_cast<std::size_t>(nOpacities) * nMom);
  for (int m = 0; m < nMom; ++m)
    for (int s = 0; s < nOpacities; ++s)
      offsets[wli::flat_index<2>({s, m}, {nOpacities, nMom})] =
          0.5 + 0.11 * s + 0.29 * m;

  const int iSpecies = 0;  // nOpacities = 1 -> only species index available
  auto os_of = [&](int moment) {
    return wli::IsoOffset(offsets.data(), nOpacities, nMom, iSpecies, moment);
  };

  // Inner single-effective-density kernel wrapper: fixed (iEp, iE, moment).
  auto K = [&](int iEp, int iE, int moment, Real LogD, Real LogT) {
    return wli::BremInterpolateSingleDensity2DAlignedPoint(
        LogD, LogT, LogDs, nD, LogTs, nT, iEp, iE, nEp, nE, moment, nMom,
        os_of(moment), tbl);
  };

  // Summed aligned wrapper: sum_l Alpha[l] * K(iEp, iE, moment, LogD[l], LogT).
  auto brem = [&](int iEp, int iE, int moment, const Real* LogD,
                  const Real* Alpha, int nSpecies, Real LogT) {
    return wli::BremInterpolateSingleVariable2D2DAlignedSummedPoint(
        LogD, Alpha, nSpecies, LogT, LogDs, nD, LogTs, nT, iEp, iE, nEp, nE,
        moment, nMom, os_of(moment), tbl);
  };

  // Expected single-density recovered value: 10**(affine) - OS.
  auto want = [&](int iEp, int iE, int moment, Real LogD, Real LogT) {
    return wli::recover(affine(iEp, iE, moment, LogD, LogT), os_of(moment));
  };

  // --- Check 1: affine-in-log exactness at an interior query (via K) ---
  {
    int iEp = 1, iE = 2, m = 1;
    Real LogD = 0.42, LogT = 0.35;  // interior on both axes
    check(wli::is_close(K(iEp, iE, m, LogD, LogT), want(iEp, iE, m, LogD, LogT),
                        wli::rtol_machine, wli::atol_default),
          "affine-in-log exactness at interior (rho, T) query (~1e-14)");
  }

  // --- Check 2: node identity at a (rho, T) grid node ---
  {
    int iEp = 2, iE = 0, m = 2, iD = 2, iT = 1;
    Real got = K(iEp, iE, m, LogDs[iD], LogTs[iT]);
    Real w = wli::recover(
        table[wli::flat_index<5>({iEp, iE, m, iD, iT}, {nEp, nE, nMom, nD, nT})],
        os_of(m));
    check(wli::is_close(got, w, wli::rtol_machine, wli::atol_default),
          "node identity at interior (rho, T) node (~1e-14)");
    // Top-edge node: both indices clamp to n-2, deltas -> 1.
    int iDe = nD - 1, iTe = nT - 1;
    Real gotE = K(iEp, iE, m, LogDs[iDe], LogTs[iTe]);
    Real wE = wli::recover(
        table[wli::flat_index<5>({iEp, iE, m, iDe, iTe},
                                 {nEp, nE, nMom, nD, nT})],
        os_of(m));
    check(wli::is_close(gotE, wE, wli::rtol_machine, wli::atol_default),
          "node identity at top-edge (rho, T) node (~1e-14)");
  }

  // --- Check 3: moment-slice independence ---
  {
    int iEp = 1, iE = 2;
    Real LogD = 0.31, LogT = 0.22;  // shared interior point
    Real g0 = K(iEp, iE, 0, LogD, LogT);
    Real g1 = K(iEp, iE, 1, LogD, LogT);
    Real g2 = K(iEp, iE, 2, LogD, LogT);
    check(wli::is_close(g0, want(iEp, iE, 0, LogD, LogT), wli::rtol_machine,
                        wli::atol_default),
          "moment-slice independence: moment=0 recovers its own function");
    check(wli::is_close(g1, want(iEp, iE, 1, LogD, LogT), wli::rtol_machine,
                        wli::atol_default),
          "moment-slice independence: moment=1 recovers its own function");
    check(wli::is_close(g2, want(iEp, iE, 2, LogD, LogT), wli::rtol_machine,
                        wli::atol_default),
          "moment-slice independence: moment=2 recovers its own function");
    // Slices must DIFFER (guards a stride bug blending/ignoring moment, or a
    // wrong 2D offset element).
    check(!wli::is_close(g0, g1, wli::rtol_machine, wli::atol_default) &&
              !wli::is_close(g1, g2, wli::rtol_machine, wli::atol_default),
          "moment slices differ (moment is a genuine strided slice)");
  }

  // --- Check 4: effective-density decomposition closure ---
  // Choose rho, xp, xn so the three effective densities differ and all stay
  // in-bounds; feed the three LOG10'd effective densities as LogD[0..2].
  const Real Alpha_Brem[3] = {Real(1), Real(1), Real(28.0 / 3.0)};
  {
    int iEp = 0, iE = 1, m = 1;
    Real rho = 4.0, xp = 0.35, xn = 0.55, T = 2.7;
    Real LogT = std::log10(T);
    Real LogD[3] = {std::log10(rho * xp), std::log10(rho * xn),
                    std::log10(rho * std::sqrt(std::fabs(xp * xn)))};
    Real K1 = K(iEp, iE, m, LogD[0], LogT);
    Real K2 = K(iEp, iE, m, LogD[1], LogT);
    Real K3 = K(iEp, iE, m, LogD[2], LogT);
    Real closed = Real(1) * K1 + Real(1) * K2 + Real(28.0 / 3.0) * K3;
    Real summed = brem(iEp, iE, m, LogD, Alpha_Brem, 3, LogT);
    check(wli::is_close(summed, closed, wli::rtol_parity, wli::atol_default),
          "effective-density decomposition: summed == 1*K1 + 1*K2 + (28/3)*K3");
    // The three terms must be genuinely distinct so the 28/3 weight is exercised.
    check(!wli::is_close(K1, K2, wli::rtol_machine, wli::atol_default) &&
              !wli::is_close(K2, K3, wli::rtol_machine, wli::atol_default) &&
              !wli::is_close(K1, K3, wli::rtol_machine, wli::atol_default),
          "three effective-density terms are distinct (weight 28/3 exercised)");
  }

  // --- Check 5: weight-order sensitivity ---
  {
    int iEp = 2, iE = 0, m = 0;
    Real rho = 3.2, xp = 0.28, xn = 0.6, T = 1.9;
    Real LogT = std::log10(T);
    Real LogD[3] = {std::log10(rho * xp), std::log10(rho * xn),
                    std::log10(rho * std::sqrt(std::fabs(xp * xn)))};
    Real K1 = K(iEp, iE, m, LogD[0], LogT);
    Real K2 = K(iEp, iE, m, LogD[1], LogT);
    Real K3 = K(iEp, iE, m, LogD[2], LogT);
    Real closed = K1 + K2 + Real(28.0 / 3.0) * K3;
    Real correct = brem(iEp, iE, m, LogD, Alpha_Brem, 3, LogT);
    check(wli::is_close(correct, closed, wli::rtol_parity, wli::atol_default),
          "weight-order: correct Alpha=[1,1,28/3] matches closed form");
    // Permuted weights must give a DIFFERENT result (28/3 lands on a different
    // effective density), proving the weight applies to the third (cross) term.
    const Real Alpha_perm[3] = {Real(28.0 / 3.0), Real(1), Real(1)};
    Real permd = brem(iEp, iE, m, LogD, Alpha_perm, 3, LogT);
    check(!wli::is_close(permd, closed, wli::rtol_parity, wli::atol_default),
          "weight-order: permuted Alpha=[28/3,1,1] changes the result");
  }

  // --- Check 6: both-triangles-computed (no symmetry fill) ---
  // The table is asymmetric under transposing (iEp, iE); each of (iEp,iE) and
  // (iE,iEp) must equal its OWN decomposition, and the two must differ.
  {
    int a = 0, b = 2, m = 1;  // a != b so transpose is a genuine other entry
    Real rho = 5.0, xp = 0.4, xn = 0.5, T = 3.1;
    Real LogT = std::log10(T);
    Real LogD[3] = {std::log10(rho * xp), std::log10(rho * xn),
                    std::log10(rho * std::sqrt(std::fabs(xp * xn)))};
    // (a, b) equals its own decomposition.
    Real closed_ab = K(a, b, m, LogD[0], LogT) + K(a, b, m, LogD[1], LogT) +
                     Real(28.0 / 3.0) * K(a, b, m, LogD[2], LogT);
    Real summed_ab = brem(a, b, m, LogD, Alpha_Brem, 3, LogT);
    check(wli::is_close(summed_ab, closed_ab, wli::rtol_machine,
                        wli::atol_default),
          "both-triangles: (iEp=0,iE=2) equals its own decomposition");
    // (b, a) equals its own decomposition.
    Real closed_ba = K(b, a, m, LogD[0], LogT) + K(b, a, m, LogD[1], LogT) +
                     Real(28.0 / 3.0) * K(b, a, m, LogD[2], LogT);
    Real summed_ba = brem(b, a, m, LogD, Alpha_Brem, 3, LogT);
    check(wli::is_close(summed_ba, closed_ba, wli::rtol_machine,
                        wli::atol_default),
          "both-triangles: (iEp=2,iE=0) equals its own decomposition");
    // The two transposed outputs must DIFFER: no entry is derived from its
    // transpose, no detailed-balance / crossing fill is applied.
    check(!wli::is_close(summed_ab, summed_ba, wli::rtol_machine,
                         wli::atol_default),
          "both-triangles: transposed outputs differ (no symmetry fill)");
  }

  // --- Check 7: boundary extrapolation (clamp-index-but-not-delta) ---
  // Query one effective density just BELOW the rho[0] edge; its term must equal
  // the edge cell's BiLinear evaluated with the (negative) unclamped delta.
  {
    int iEp = 1, iE = 1, m = 2;
    Real LogT = 0.3;
    Real LogDlo = LogDs[0] - 0.5;  // below the density edge -> dD < 0
    Real got = K(iEp, iE, m, LogDlo, LogT);
    // Reconstruct the expected edge-cell linear extrapolation directly. Since
    // the stored value is exactly affine, its recovered value is 10**(affine)-OS
    // at the unclamped delta, i.e. want(...) evaluates the affine at LogDlo.
    Real w = want(iEp, iE, m, LogDlo, LogT);
    check(wli::is_close(got, w, wli::rtol_machine, wli::atol_default),
          "boundary extrapolation below rho edge (unclamped delta, ~1e-14)");
    // Above the T edge as well.
    Real LogThi = LogTs[nT - 1] + 0.6;
    check(wli::is_close(K(iEp, iE, m, 0.3, LogThi),
                        want(iEp, iE, m, 0.3, LogThi), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation above T edge (unclamped delta, ~1e-14)");
  }

  // --- Check 8: NaN propagation on non-positive effective rho or T ---
  {
    int iEp = 1, iE = 1, m = 0;
    Real LogT = 0.3;
    const Real nanFromNeg = std::log10(Real(-1.0));  // NaN; caller's log10(<0)
    // Non-positive effective density for one species.
    Real LogD_bad[3] = {nanFromNeg, std::log10(Real(2.0)),
                        std::log10(Real(3.0))};
    check(std::isnan(brem(iEp, iE, m, LogD_bad, Alpha_Brem, 3, LogT)),
          "NaN propagation on non-positive effective rho (log10(<0))");
    // Non-positive temperature.
    Real LogD_ok[3] = {std::log10(Real(1.0)), std::log10(Real(2.0)),
                       std::log10(Real(3.0))};
    check(std::isnan(brem(iEp, iE, m, LogD_ok, Alpha_Brem, 3, nanFromNeg)),
          "NaN propagation on non-positive T (log10(<0))");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "brem_point: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf(
      "PASS brem_point: Brem aligned summed 2D2D bilinear evaluate kernel\n");
  return EXIT_SUCCESS;
}
