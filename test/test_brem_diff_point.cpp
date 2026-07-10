// Self-contained acceptance probe for the Brem (nucleon-nucleon bremsstrahlung)
// aligned 2D2D bilinear single-point evaluate-and-differentiate kernels
//   src/opacity/wli_opacity_brem.H::BremInterpolateSingleDensityDifferentiate2DAlignedPoint
//   src/opacity/wli_opacity_brem.H::BremInterpolateSingleVariable2D2DAlignedSummedDifferentiatePoint.
//
// Enforces the derivative-bearing self-contained checks of
// specs/opacity-differentiate.md (Verification :142-148, #1,2,4,5,6,7) for the
// matched value + (∂/∂rho, ∂/∂T) 2D-aligned bilinear Brem kernel, whose oracle is
// the aligned scalar core LinearInterpDeriv2D_4DArray_2DAligned_Point
// (wlInterpolationUtilitiesModule.F90:875-909), plus the exact-linearity summed
// temperature derivative ∂SumInterp/∂T = Σ_l Alpha(l)·∂Interp_l/∂T (spec:104-113):
//   1. Value parity with the evaluate leaf (parity tier 1e-12/1e-30): the .value
//      component equals BremInterpolateSingleDensity2DAlignedPoint on identical
//      inputs — the derivative path must not perturb the value.
//   2. Analytic closed form on an affine-in-log table (relaxed tier 1e-10), with
//      DISTINCT coefficients per (iEp, iE, moment) slice so a wrong slice or a
//      rho/T transpose is caught. The closed forms are
//        ∂value/∂rho = (value+OS)·bD/rho,   ∂value/∂T = (value+OS)·bT/T,
//      asserted at arbitrary interior (non-node) queries.
//   4. Explicit log-axis scale-factor check (relaxed tier): BOTH partials carry
//      1/(X·(node spacing)) with X = 10**(LogX) and NO ln10 (Brem has no linear
//      axis — the ln10-survives half is EmAb's Ye, out of scope). The negative
//      assertion (ln10-added form rejected) is done on BOTH rho and T.
//   5. Brem summed-derivative decomposition (PARITY tier): each per-l plane
//      derivative equals the 2D-aligned core evaluated at its own rho_l, AND
//      ∂SumInterp/∂T = Σ_l Alpha(l)·∂Interp_l/∂T as an exact linear combination.
//   6. Boundary extrapolation of the derivative (exact relation): a query just
//      below/above each edge clamps the index (in a_k) but NOT the delta (in the
//      raw partial), so the affine closed forms still hold on both axes.
//   7. NaN propagation (NaN-equality): non-positive rho or T (modeled as
//      log10(<=0)) makes .value AND every partial NaN.
//
// Both rho and T arrive PRE-LOG10'd (spec:67), so queries are fed as log10(rho) /
// log10(T). Hand-rolled harness (no GoogleTest/Catch2), synthetic 5D table only
// (no HDF5), mirroring test/test_brem_point.cpp and test/test_nes_pair_diff_point.cpp.
// amrex::Initialize is not required.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "wli_compare.H"
#include "wli_interp.H"
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
// terms of the ALREADY-LOG10'd coordinates (LogD, LogT), identical to
// test_brem_point.cpp so the value path and the derivative path agree on the same
// table. Distinct dD/dT slopes break a rho/T transpose; the asymmetric squared
// iEp term breaks an (iEp,iE) swap.
Real affine(int iEp, int iE, int moment, Real LogD, Real LogT) {
  Real const base =
      0.50 + 0.07 * iEp + 0.13 * iE + 0.31 * moment + 0.019 * iEp * iEp;
  Real const bD = 0.09 + 0.017 * iEp - 0.006 * moment;
  Real const bT = -0.05 + 0.011 * iE + 0.008 * moment + 0.004 * iEp;
  return base + bD * LogD + bT * LogT;
}

// Per-axis affine slopes (must match affine() above).
Real slopeD(int iEp, int /*iE*/, int moment) {
  return 0.09 + 0.017 * iEp - 0.006 * moment;
}
Real slopeT(int iEp, int iE, int moment) {
  return -0.05 + 0.011 * iE + 0.008 * moment + 0.004 * iEp;
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

  // 2D offsets Offsets[nOpacities, nMoments], column-major. NONZERO and distinct
  // per moment so recon = value+OS is genuinely exercised.
  std::vector<Real> offsets(static_cast<std::size_t>(nOpacities) * nMom);
  for (int m = 0; m < nMom; ++m)
    for (int s = 0; s < nOpacities; ++s)
      offsets[wli::flat_index<2>({s, m}, {nOpacities, nMom})] =
          0.5 + 0.11 * s + 0.29 * m;

  const int iSpecies = 0;  // nOpacities = 1 -> only species index available
  auto os_of = [&](int moment) {
    return wli::IsoOffset(offsets.data(), nOpacities, nMom, iSpecies, moment);
  };

  // Per-effective-density evaluate-and-differentiate + value-only wrappers.
  auto diff = [&](int iEp, int iE, int moment, Real LogD, Real LogT) {
    return wli::BremInterpolateSingleDensityDifferentiate2DAlignedPoint(
        LogD, LogT, LogDs, nD, LogTs, nT, iEp, iE, nEp, nE, moment, nMom,
        os_of(moment), tbl);
  };
  auto eval = [&](int iEp, int iE, int moment, Real LogD, Real LogT) {
    return wli::BremInterpolateSingleDensity2DAlignedPoint(
        LogD, LogT, LogDs, nD, LogTs, nT, iEp, iE, nEp, nE, moment, nMom,
        os_of(moment), tbl);
  };
  // Summed evaluate-and-differentiate wrapper.
  auto sdiff = [&](int iEp, int iE, int moment, const Real* LogD,
                   const Real* Alpha, int nSpecies, Real LogT) {
    return wli::BremInterpolateSingleVariable2D2DAlignedSummedDifferentiatePoint(
        LogD, Alpha, nSpecies, LogT, LogDs, nD, LogTs, nT, iEp, iE, nEp, nE,
        moment, nMom, os_of(moment), tbl);
  };

  // --- Check 1: value parity with the evaluate leaf (parity tier) ---
  {
    check(wli::is_close(diff(1, 2, 1, 0.42, 0.35).value,
                        eval(1, 2, 1, 0.42, 0.35), wli::rtol_parity,
                        wli::atol_default),
          "value parity vs evaluate leaf, interior A (1e-12/1e-30)");
    // Asymmetric transposed slice (iEp>iE), no triangle guard.
    check(wli::is_close(diff(2, 0, 2, -0.05, 0.6).value,
                        eval(2, 0, 2, -0.05, 0.6), wli::rtol_parity,
                        wli::atol_default),
          "value parity vs evaluate leaf, interior B iEp>iE (1e-12/1e-30)");
  }

  // --- Check 2: analytic closed form (distinct coeff per slice/axis) ---
  auto check_affine_derivs = [&](int iEp, int iE, int m, Real LogD, Real LogT,
                                 const char* where) {
    wli::BremPointDeriv d = diff(iEp, iE, m, LogD, LogT);
    Real recon = std::pow(Real(10), affine(iEp, iE, m, LogD, LogT));  // value+OS
    Real rho = std::pow(Real(10), LogD);
    Real T = std::pow(Real(10), LogT);
    Real wDrho = recon * slopeD(iEp, iE, m) / rho;
    Real wDT = recon * slopeT(iEp, iE, m) / T;
    check(wli::is_close(d.dDrho, wDrho, wli::rtol_relaxed, wli::atol_default),
          where);
    check(wli::is_close(d.dDT, wDT, wli::rtol_relaxed, wli::atol_default), where);
  };
  check_affine_derivs(1, 2, 1, 0.42, 0.35,
                      "affine closed-form derivs, interior A (1e-10)");
  check_affine_derivs(2, 0, 2, -0.1, 0.6,
                      "affine closed-form derivs, interior B iEp>iE (1e-10)");
  check_affine_derivs(0, 1, 0, 0.2, -0.15,
                      "affine closed-form derivs, interior C (1e-10)");

  // --- Check 4: explicit log-axis scale factors + negative assertions ---
  {
    int iEp = 1, iE = 2, m = 1;
    Real LogD = 0.42, LogT = 0.35;  // interior
    wli::BremPointDeriv d = diff(iEp, iE, m, LogD, LogT);
    Real recon = std::pow(Real(10), affine(iEp, iE, m, LogD, LogT));
    // Bracket cells (matches the kernel's clamp-then-delta).
    auto [iD, dD] = wli::GetIndexAndDeltaLin(LogD, LogDs, nD);
    auto [iT, dT] = wli::GetIndexAndDeltaLin(LogT, LogTs, nT);
    (void)dD; (void)dT;
    // Raw bilinear partials on the affine table: coefficient * node spacing.
    Real rawD = slopeD(iEp, iE, m) * (LogDs[iD + 1] - LogDs[iD]);
    Real rawT = slopeT(iEp, iE, m) * (LogTs[iT + 1] - LogTs[iT]);
    // BOTH LOG axes: a = 1/(X·(node spacing)), X = 10**(LogX). NO ln10.
    Real aRho =
        Real(1) / (std::pow(Real(10), LogD) * (LogDs[iD + 1] - LogDs[iD]));
    Real aT = Real(1) / (std::pow(Real(10), LogT) * (LogTs[iT + 1] - LogTs[iT]));
    check(wli::is_close(d.dDrho, recon * aRho * rawD, wli::rtol_relaxed,
                        wli::atol_default),
          "scale factor: rho log-axis 1/(X·spacing), no ln10 (1e-10)");
    check(wli::is_close(d.dDT, recon * aT * rawT, wli::rtol_relaxed,
                        wli::atol_default),
          "scale factor: T log-axis 1/(X·spacing), no ln10 (1e-10)");
    // REQUIRED negative assertions on BOTH axes (Brem has no linear sibling):
    // an ln10-added form must NOT match the returned derivative.
    check(!wli::is_close(d.dDrho, recon * wli::ln10 * aRho * rawD,
                         wli::rtol_relaxed, wli::atol_default),
          "log rho axis carries NO ln10 (ln10-added form rejected)");
    check(!wli::is_close(d.dDT, recon * wli::ln10 * aT * rawT, wli::rtol_relaxed,
                         wli::atol_default),
          "log T axis carries NO ln10 (ln10-added form rejected)");
  }

  // --- Check 5: Brem summed-derivative decomposition (parity tier) ---
  // Per-l plane derivative equals the 2D-aligned core at each rho_l, AND
  // ∂SumInterp/∂T = Σ_l Alpha(l)·∂Interp_l/∂T (exact linear combination).
  const Real Alpha_Brem[3] = {Real(1), Real(1), Real(28.0 / 3.0)};
  {
    int iEp = 0, iE = 1, m = 1;
    Real rho = 4.0, xp = 0.35, xn = 0.55, T = 2.7;
    Real LogT = std::log10(T);
    Real LogD[3] = {std::log10(rho * xp), std::log10(rho * xn),
                    std::log10(rho * std::sqrt(std::fabs(xp * xn)))};
    // Per-effective-density plane derivatives (the 2D-aligned core at rho_l).
    wli::BremPointDeriv d0 = diff(iEp, iE, m, LogD[0], LogT);
    wli::BremPointDeriv d1 = diff(iEp, iE, m, LogD[1], LogT);
    wli::BremPointDeriv d2 = diff(iEp, iE, m, LogD[2], LogT);
    // Each per-l plane derivative equals its own affine closed form at rho_l.
    for (int l = 0; l < 3; ++l) {
      wli::BremPointDeriv dl = diff(iEp, iE, m, LogD[l], LogT);
      Real recon = std::pow(Real(10), affine(iEp, iE, m, LogD[l], LogT));
      Real rho_l = std::pow(Real(10), LogD[l]);
      Real wDrho = recon * slopeD(iEp, iE, m) / rho_l;
      check(wli::is_close(dl.dDrho, wDrho, wli::rtol_relaxed, wli::atol_default),
            "per-l plane derivative equals core evaluated at its own rho_l");
    }
    // Summed evaluate-and-differentiate.
    wli::BremPointDeriv s = sdiff(iEp, iE, m, LogD, Alpha_Brem, 3, LogT);
    Real sumDT = Real(1) * d0.dDT + Real(1) * d1.dDT + Real(28.0 / 3.0) * d2.dDT;
    Real sumVal =
        Real(1) * d0.value + Real(1) * d1.value + Real(28.0 / 3.0) * d2.value;
    check(wli::is_close(s.dDT, sumDT, wli::rtol_parity, wli::atol_default),
          "summed ∂/∂T == Σ_l Alpha(l)·∂Interp_l/∂T (exact linear combination)");
    check(wli::is_close(s.value, sumVal, wli::rtol_parity, wli::atol_default),
          "summed value == Σ_l Alpha(l)·Interp_l (parity with the value leaf)");
    // dDrho is deliberately consumer-side (left 0 in the summed struct).
    check(s.dDrho == Real(0),
          "summed dDrho left 0 (base-density chain rule is consumer-side)");
  }

  // --- Check 6: boundary extrapolation of derivatives (affine closed forms) ---
  {
    int iEp = 2, iE = 1, m = 1;
    // rho axis (below/above edge): index clamps, dD unclamped.
    check_affine_derivs(iEp, iE, m, LogDs[0] - 0.7, 0.35,
                        "boundary derivs below rho edge (1e-10)");
    check_affine_derivs(iEp, iE, m, LogDs[nD - 1] + 0.7, 0.35,
                        "boundary derivs above rho edge (1e-10)");
    // T axis.
    check_affine_derivs(iEp, iE, m, 0.3, LogTs[0] - 0.7,
                        "boundary derivs below T edge (1e-10)");
    check_affine_derivs(iEp, iE, m, 0.3, LogTs[nT - 1] + 0.7,
                        "boundary derivs above T edge (1e-10)");
  }

  // --- Check 7: NaN propagation on non-positive rho or T ---
  {
    int iEp = 1, iE = 1, m = 0;
    Real LogD = 0.2, LogT = 0.3;
    const Real nanFromNeg = std::log10(Real(-1.0));  // NaN; caller's log10(<0)
    auto all_nan = [](wli::BremPointDeriv d) {
      return std::isnan(d.value) && std::isnan(d.dDrho) && std::isnan(d.dDT);
    };
    // Per-density kernel: value AND every partial NaN.
    check(all_nan(diff(iEp, iE, m, nanFromNeg, LogT)),
          "NaN in value + dDrho + dDT on non-positive rho");
    check(all_nan(diff(iEp, iE, m, LogD, nanFromNeg)),
          "NaN in value + dDrho + dDT on non-positive T");
    // Summed kernel: one non-positive effective rho NaNs value + dDT (dDrho is 0).
    Real LogD_bad[3] = {nanFromNeg, std::log10(Real(2.0)), std::log10(Real(3.0))};
    wli::BremPointDeriv sb = sdiff(iEp, iE, m, LogD_bad, Alpha_Brem, 3, LogT);
    check(std::isnan(sb.value) && std::isnan(sb.dDT),
          "summed value + dDT NaN on non-positive effective rho");
    Real LogD_ok[3] = {std::log10(Real(1.0)), std::log10(Real(2.0)),
                       std::log10(Real(3.0))};
    wli::BremPointDeriv st = sdiff(iEp, iE, m, LogD_ok, Alpha_Brem, 3, nanFromNeg);
    check(std::isnan(st.value) && std::isnan(st.dDT),
          "summed value + dDT NaN on non-positive T");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "brem_diff_point: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf(
      "PASS brem_diff_point: Brem aligned 2D2D bilinear "
      "evaluate-and-differentiate kernels\n");
  return EXIT_SUCCESS;
}
