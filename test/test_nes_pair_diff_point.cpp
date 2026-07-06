// Self-contained acceptance probe for the NES/Pair aligned 2D2D bilinear
// single-point evaluate-and-differentiate kernel
// (src/opacity/wli_opacity_nes_pair.H::NESPairInterpolateDifferentiateSingleVariable2D2DAlignedPoint).
//
// Enforces the derivative-bearing self-contained checks of
// specs/opacity-differentiate.md (Verification :142-148, #1,2,4,6,7) for the
// matched value + (∂/∂T, ∂/∂eta) 2D-aligned bilinear kernel, whose oracle is the
// aligned scalar core LinearInterpDeriv2D_4DArray_2DAligned_Point
// (wlInterpolationUtilitiesModule.F90:875-909), driven with the log-axis scale
// factors from LogInterpolateDifferentiateSingleVariable_2D2D_Custom_Aligned_P:
//   1. Value parity with the evaluate leaf (parity tier 1e-12/1e-30): the .value
//      component equals NESPairInterpolateSingleVariable2D2DAlignedPoint on
//      identical inputs — the derivative path must not perturb the value.
//   2. Analytic closed form on an affine-in-log table (relaxed tier 1e-10), with
//      DISTINCT coefficients per (iEp, iE, kernel) slice so a wrong slice or a
//      transposed dBiLineardX_k is caught. The closed forms are
//        ∂value/∂T   = (value+OS)·bT/T,   ∂value/∂eta = (value+OS)·bX/eta,
//      asserted at arbitrary interior (non-node) queries, including iEp > iE.
//   4. Explicit log-axis scale-factor check (relaxed tier): BOTH partials carry
//      1/(X·(node spacing)) with X = 10**(LogX) and NO ln10. Since NES/Pair has
//      no linear sibling axis, the required negative assertion is done on BOTH
//      axes: a swapped/ln10-added form must NOT match the returned derivative.
//   6. Boundary extrapolation of the derivative (exact relation): a query just
//      below/above each edge clamps the index (in a_k) but NOT the delta (in the
//      raw partial), so the affine closed forms still hold on both axes.
//   7. NaN propagation (NaN-equality): non-positive T or eta (modeled as
//      log10(<=0)) makes .value AND both partials NaN. NES/Pair has no axis that
//      stays finite on non-positive input, so ALL-NaN is asserted on each.
//
// Both T and eta arrive PRE-LOG10'd (spec:60), so queries are fed as log10(T) /
// log10(eta). Hand-rolled harness (no GoogleTest/Catch2), synthetic 5D table
// only (no HDF5), mirroring test/test_emab_diff_point.cpp and
// test/test_nes_pair_point.cpp. amrex::Initialize is not required.

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

// Per-(iEp, iE, kernel) DISTINCT affine-in-log model in terms of the ALREADY-
// LOG10'd coordinates (LogT, LogX). A wrong energy index, a wrong kernel slice,
// or a transposed derivative axis is caught because every triple's base offset
// and per-axis slopes differ. Coefficients are small so the affine LOG-space
// value stays modest across interior + extrapolation queries, keeping
// 10**(affine) in a tier where the relaxed derivative check holds.
Real affine(int iEp, int iE, int kernel, Real LogT, Real LogX) {
  Real const base = 0.50 + 0.07 * iEp + 0.13 * iE + 0.31 * kernel;
  Real const bT = 0.09 + 0.017 * iEp - 0.006 * kernel;
  Real const bX = -0.05 + 0.011 * iE + 0.008 * kernel;
  return base + bT * LogT + bX * LogX;
}

// Per-axis affine slopes (must match affine() above).
Real slopeT(int iEp, int /*iE*/, int kernel) { return 0.09 + 0.017 * iEp - 0.006 * kernel; }
Real slopeX(int /*iEp*/, int iE, int kernel) { return -0.05 + 0.011 * iE + 0.008 * kernel; }

}  // namespace

int main() {
  // --- Synthetic 5D grid: non-uniform (uneven-ratio) LOG10'd T and eta axes. ---
  const int nEp = 4, nE = 4, nMom = 3, nT = 3, nEta = 4;
  const int nOpacities = 1;  // pinned NES/Pair tables have nOpacities = 1
  Real LogTs[nT] = {-0.4, 0.15, 0.9};         // log10 T (T in MeV)
  Real LogXs[nEta] = {-1.0, -0.3, 0.5, 1.4};  // log10 eta

  // 5D table, column-major (nEp, nE, nMom, nT, nEta): E' fastest.
  std::vector<Real> table(static_cast<std::size_t>(nEp) * nE * nMom * nT * nEta);
  for (int iX = 0; iX < nEta; ++iX)
    for (int iT = 0; iT < nT; ++iT)
      for (int k = 0; k < nMom; ++k)
        for (int j = 0; j < nE; ++j)
          for (int i = 0; i < nEp; ++i)
            table[wli::flat_index<5>({i, j, k, iT, iX},
                                     {nEp, nE, nMom, nT, nEta})] =
                affine(i, j, k, LogTs[iT], LogXs[iX]);

  const Real* tbl = table.data();

  // 2D offsets Offsets[nOpacities, nMoments], column-major. NONZERO and distinct
  // per kernel so recon = value+OS is genuinely exercised and a wrong offset
  // element is caught.
  Real offsets[nOpacities * nMom];
  for (int k = 0; k < nMom; ++k)
    for (int s = 0; s < nOpacities; ++s)
      offsets[wli::flat_index<2>({s, k}, {nOpacities, nMom})] =
          0.5 + 0.11 * s + 0.29 * k;

  const int iSpecies = 0;  // nOpacities = 1 -> only species index available
  auto os_of = [&](int kernel) {
    return wli::IsoOffset(offsets, nOpacities, nMom, iSpecies, kernel);
  };

  // Evaluate-and-differentiate + value-only wrappers at a fixed slice + offset.
  auto diff = [&](int iEp, int iE, int kernel, Real LogT, Real LogX) {
    return wli::NESPairInterpolateDifferentiateSingleVariable2D2DAlignedPoint(
        LogT, LogX, LogTs, nT, LogXs, nEta, iEp, iE, nEp, nE, kernel, nMom,
        os_of(kernel), tbl);
  };
  auto eval = [&](int iEp, int iE, int kernel, Real LogT, Real LogX) {
    return wli::NESPairInterpolateSingleVariable2D2DAlignedPoint(
        LogT, LogX, LogTs, nT, LogXs, nEta, iEp, iE, nEp, nE, kernel, nMom,
        os_of(kernel), tbl);
  };

  // --- Check 1: value parity with the evaluate leaf (parity tier) ---
  {
    check(wli::is_close(diff(1, 2, 1, 0.35, 0.1).value, eval(1, 2, 1, 0.35, 0.1),
                        wli::rtol_parity, wli::atol_default),
          "value parity vs evaluate leaf, interior A (1e-12/1e-30)");
    // iEp > iE slice (no triangle guard).
    check(wli::is_close(diff(3, 1, 2, -0.05, 0.6).value,
                        eval(3, 1, 2, -0.05, 0.6), wli::rtol_parity,
                        wli::atol_default),
          "value parity vs evaluate leaf, interior B iEp>iE (1e-12/1e-30)");
  }

  // --- Check 2: analytic closed form (distinct coeff per slice/axis) ---
  auto check_affine_derivs = [&](int iEp, int iE, int k, Real LogT, Real LogX,
                                 const char* where) {
    wli::NESPairPointDeriv d = diff(iEp, iE, k, LogT, LogX);
    Real recon = std::pow(Real(10), affine(iEp, iE, k, LogT, LogX));  // value+OS
    Real T = std::pow(Real(10), LogT);
    Real eta = std::pow(Real(10), LogX);
    Real wDT = recon * slopeT(iEp, iE, k) / T;
    Real wDX = recon * slopeX(iEp, iE, k) / eta;
    check(wli::is_close(d.dDT, wDT, wli::rtol_relaxed, wli::atol_default), where);
    check(wli::is_close(d.dDX, wDX, wli::rtol_relaxed, wli::atol_default), where);
  };
  check_affine_derivs(1, 2, 1, 0.35, 0.1,
                      "affine closed-form derivs, interior A (1e-10)");
  check_affine_derivs(2, 0, 2, -0.1, 0.6,
                      "affine closed-form derivs, interior B (1e-10)");
  check_affine_derivs(3, 1, 0, 0.2, -0.2,
                      "affine closed-form derivs, interior C iEp>iE (1e-10)");

  // --- Check 4: explicit log-axis scale factors + negative assertions ---
  {
    int iEp = 1, iE = 2, k = 1;
    Real LogT = 0.35, LogX = 0.1;  // interior
    wli::NESPairPointDeriv d = diff(iEp, iE, k, LogT, LogX);
    Real recon = std::pow(Real(10), affine(iEp, iE, k, LogT, LogX));
    // Bracket cells (matches the kernel's clamp-then-delta).
    auto [iT, dT] = wli::GetIndexAndDeltaLin(LogT, LogTs, nT);
    auto [iX, dX] = wli::GetIndexAndDeltaLin(LogX, LogXs, nEta);
    (void)dT; (void)dX;
    // Raw bilinear partials on the affine table: coefficient * node spacing.
    Real rawT = slopeT(iEp, iE, k) * (LogTs[iT + 1] - LogTs[iT]);
    Real rawX = slopeX(iEp, iE, k) * (LogXs[iX + 1] - LogXs[iX]);
    // BOTH LOG axes: a = 1/(X·(LogXs[i+1]-LogXs[i])), X = 10**(LogX). NO ln10.
    Real aT = Real(1) / (std::pow(Real(10), LogT) * (LogTs[iT + 1] - LogTs[iT]));
    Real aX = Real(1) / (std::pow(Real(10), LogX) * (LogXs[iX + 1] - LogXs[iX]));
    check(wli::is_close(d.dDT, recon * aT * rawT, wli::rtol_relaxed,
                        wli::atol_default),
          "scale factor: T log-axis 1/(X·spacing), no ln10 (1e-10)");
    check(wli::is_close(d.dDX, recon * aX * rawX, wli::rtol_relaxed,
                        wli::atol_default),
          "scale factor: eta log-axis 1/(X·spacing), no ln10 (1e-10)");
    // REQUIRED negative assertions on BOTH axes (no linear sibling to contrast):
    // an ln10-added form must NOT match the returned derivative.
    check(!wli::is_close(d.dDT, recon * wli::ln10 * aT * rawT, wli::rtol_relaxed,
                         wli::atol_default),
          "log T axis carries NO ln10 (ln10-added form rejected)");
    check(!wli::is_close(d.dDX, recon * wli::ln10 * aX * rawX, wli::rtol_relaxed,
                         wli::atol_default),
          "log eta axis carries NO ln10 (ln10-added form rejected)");
  }

  // --- Check 6: boundary extrapolation of derivatives (affine closed forms) ---
  {
    int iEp = 2, iE = 2, k = 1;
    // T axis (below/above edge): index clamps, dT unclamped.
    check_affine_derivs(iEp, iE, k, LogTs[0] - 0.8, 0.1,
                        "boundary derivs below T edge (1e-10)");
    check_affine_derivs(iEp, iE, k, LogTs[nT - 1] + 0.8, 0.1,
                        "boundary derivs above T edge (1e-10)");
    // eta axis.
    check_affine_derivs(iEp, iE, k, 0.3, LogXs[0] - 0.8,
                        "boundary derivs below eta edge (1e-10)");
    check_affine_derivs(iEp, iE, k, 0.3, LogXs[nEta - 1] + 0.8,
                        "boundary derivs above eta edge (1e-10)");
  }

  // --- Check 7: NaN propagation on non-positive T or eta ---
  {
    int iEp = 1, iE = 1, k = 0;
    Real LogT = 0.3, LogX = 0.0;
    const Real nanFromNeg = std::log10(Real(-1.0));  // NaN; caller's log10(<0)
    auto all_nan = [](wli::NESPairPointDeriv d) {
      return std::isnan(d.value) && std::isnan(d.dDT) && std::isnan(d.dDX);
    };
    check(all_nan(diff(iEp, iE, k, nanFromNeg, LogX)),
          "NaN in value + dDT + dDX on non-positive T");
    check(all_nan(diff(iEp, iE, k, LogT, nanFromNeg)),
          "NaN in value + dDT + dDX on non-positive eta");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "nes_pair_diff_point: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf(
      "PASS nes_pair_diff_point: NES/Pair aligned 2D2D bilinear "
      "evaluate-and-differentiate kernel\n");
  return EXIT_SUCCESS;
}
