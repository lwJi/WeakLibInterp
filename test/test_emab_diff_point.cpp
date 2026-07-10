// Self-contained acceptance probe for the EmAb single-point 4D evaluate-and-
// differentiate kernel
// (src/opacity/wli_opacity_emab_iso.H::EmAbInterpolateDifferentiateSingleVariable4DPoint).
//
// Enforces the derivative-bearing self-contained checks of
// specs/opacity-differentiate.md (Verification :142-148) for the matched
// value + (∂/∂E, ∂/∂rho, ∂/∂T, ∂/∂Ye) tetralinear kernel:
//   1. Value parity with the evaluate leaf (parity tier 1e-12/1e-30): the
//      .value component equals EmAbInterpolateSingleVariable4DPoint on identical
//      inputs — the derivative path must not perturb the value (spec check 1).
//   2. Analytic closed form on an affine-in-log table (relaxed tier 1e-10), with
//      a DISTINCT affine coefficient per axis so a wrong/transposed
//      dTetraLineardX_k is caught. The closed forms are
//        ∂value/∂E   = (value+OS)·kE/E,   ∂value/∂rho = (value+OS)·kD/rho,
//        ∂value/∂T   = (value+OS)·kT/T,    ∂value/∂Ye  = (value+OS)·ln10·kY,
//      asserted at arbitrary interior (non-node) queries (spec check 2).
//   4. Explicit log-axis vs linear-Ye scale-factor check (relaxed tier): the
//      three log-axis partials carry 1/(X·log10(node ratio)) and the linear Ye
//      partial carries ln10/(node spacing) — a swapped or missing ln10 differs
//      by a factor of ln10 and is caught (spec check 4).
//   6. Boundary extrapolation of the derivative (exact relation): a query just
//      below/above each edge clamps the index (in a_k) but NOT the delta (in the
//      raw partial), so the affine closed forms still hold on all four axes; Ye
//      extrapolates rather than NaN-ing (spec check 6).
//   7. NaN propagation (NaN-equality): non-positive E/rho/T (modeled as
//      log10(<=0)) makes .value AND all four partials NaN; a non-positive Ye
//      (linear axis) extrapolates and stays finite (spec check 7).
//
// The kernel takes E/rho/T PRE-LOG10'd (spec:53), so queries are fed as
// log10(E) etc.; Ye is raw. Hand-rolled harness (no GoogleTest/Catch2),
// synthetic table only (no HDF5), mirroring test/test_eos_diff_point.cpp and
// test/test_emab_point.cpp. amrex::Initialize is not required.

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

// Affine-in-log model in the ALREADY-LOG10'd coordinates (LogE, LogD, LogT) and
// raw Y. DISTINCT nonzero coefficient per axis so a transposed derivative axis
// is caught. Small coefficients keep 10**(affine) modest across extrapolation.
constexpr Real kBase = 0.8;
constexpr Real kE = 0.13;
constexpr Real kD = -0.09;
constexpr Real kT = 0.07;
constexpr Real kY = 0.40;
constexpr Real kOS = 0.5;  // nonzero additive offset

Real affine(Real LogE, Real LogD, Real LogT, Real Y) {
  return kBase + kE * LogE + kD * LogD + kT * LogT + kY * Y;
}

}  // namespace

int main() {
  // --- Synthetic grid: non-uniform (uneven-ratio) log axes (already in log10
  //     space); LINEAR Ys. Mirrors test_emab_point.cpp. ---
  const int nE = 3, nD = 4, nT = 3, nY = 3;
  Real LogEs[nE] = {0.0, 0.7, 1.9};
  Real LogDs[nD] = {3.0, 5.4, 8.1, 11.7};
  Real LogTs[nT] = {9.0, 10.3, 11.95};
  Real Ys[nY] = {0.05, 0.30, 0.55};

  std::vector<Real> table(static_cast<std::size_t>(nE) * nD * nT * nY);
  for (int iY = 0; iY < nY; ++iY)
    for (int iT = 0; iT < nT; ++iT)
      for (int iD = 0; iD < nD; ++iD)
        for (int iE = 0; iE < nE; ++iE)
          table[static_cast<std::size_t>(iE) + nE * (iD + nD * (iT + nT * iY))] =
              affine(LogEs[iE], LogDs[iD], LogTs[iT], Ys[iY]);

  const Real* tbl = table.data();

  auto diff = [&](Real LogE, Real LogD, Real LogT, Real Y) {
    return wli::EmAbInterpolateDifferentiateSingleVariable4DPoint(
        LogE, LogD, LogT, Y, LogEs, nE, LogDs, nD, LogTs, nT, Ys, nY, kOS, tbl);
  };
  auto eval = [&](Real LogE, Real LogD, Real LogT, Real Y) {
    return wli::EmAbInterpolateSingleVariable4DPoint(
        LogE, LogD, LogT, Y, LogEs, nE, LogDs, nD, LogTs, nT, Ys, nY, kOS, tbl);
  };

  // --- Check 1: value parity with the evaluate leaf (parity tier) ---
  {
    check(wli::is_close(diff(1.1, 6.7, 10.9, 0.22).value,
                        eval(1.1, 6.7, 10.9, 0.22), wli::rtol_parity,
                        wli::atol_default),
          "value parity vs evaluate leaf, interior A (1e-12/1e-30)");
    check(wli::is_close(diff(0.4, 9.5, 9.6, 0.40).value,
                        eval(0.4, 9.5, 9.6, 0.40), wli::rtol_parity,
                        wli::atol_default),
          "value parity vs evaluate leaf, interior B (1e-12/1e-30)");
  }

  // --- Check 2: analytic closed form (distinct coeff per axis) ---
  auto check_affine_derivs = [&](Real LogE, Real LogD, Real LogT, Real Y,
                                 const char* where) {
    wli::EmAbPointDeriv d = diff(LogE, LogD, LogT, Y);
    Real recon = std::pow(Real(10), affine(LogE, LogD, LogT, Y));  // value+OS
    Real E = std::pow(Real(10), LogE);
    Real rho = std::pow(Real(10), LogD);
    Real T = std::pow(Real(10), LogT);
    Real wDE = recon * kE / E;
    Real wDrho = recon * kD / rho;
    Real wDT = recon * kT / T;
    Real wDY = recon * wli::ln10 * kY;
    check(wli::is_close(d.dDE, wDE, wli::rtol_relaxed, wli::atol_default), where);
    check(wli::is_close(d.dDrho, wDrho, wli::rtol_relaxed, wli::atol_default),
          where);
    check(wli::is_close(d.dDT, wDT, wli::rtol_relaxed, wli::atol_default), where);
    check(wli::is_close(d.dDY, wDY, wli::rtol_relaxed, wli::atol_default), where);
  };
  check_affine_derivs(1.1, 6.7, 10.9, 0.22,
                      "affine closed-form derivs, interior cell A (1e-10)");
  check_affine_derivs(0.4, 9.5, 9.6, 0.40,
                      "affine closed-form derivs, interior cell B (1e-10)");

  // --- Check 4: explicit log-axis vs linear-Ye scale factor ---
  {
    Real LogE = 1.1, LogD = 6.7, LogT = 10.9, Y = 0.22;  // interior
    wli::EmAbPointDeriv d = diff(LogE, LogD, LogT, Y);
    Real recon = std::pow(Real(10), affine(LogE, LogD, LogT, Y));
    // Bracket cells (matches the kernel's clamp-then-delta).
    auto [iE, dE] = wli::GetIndexAndDeltaLin(LogE, LogEs, nE);
    auto [iD, dD] = wli::GetIndexAndDeltaLin(LogD, LogDs, nD);
    auto [iT, dT] = wli::GetIndexAndDeltaLin(LogT, LogTs, nT);
    auto [iY, dY] = wli::GetIndexAndDeltaLin(Y, Ys, nY);
    (void)dE; (void)dD; (void)dT; (void)dY;
    // Raw tetralinear partials on the affine table: coefficient * node spacing.
    Real rawE = kE * (LogEs[iE + 1] - LogEs[iE]);
    Real rawD = kD * (LogDs[iD + 1] - LogDs[iD]);
    Real rawT = kT * (LogTs[iT + 1] - LogTs[iT]);
    Real rawY = kY * (Ys[iY + 1] - Ys[iY]);
    // LOG axes: a = 1/(X · log10(Xs[i+1]/Xs[i])), X = 10**(LogX). NO ln10.
    Real Ehi = std::pow(Real(10), LogEs[iE + 1]),
         Elo = std::pow(Real(10), LogEs[iE]);
    Real Dhi = std::pow(Real(10), LogDs[iD + 1]),
         Dlo = std::pow(Real(10), LogDs[iD]);
    Real Thi = std::pow(Real(10), LogTs[iT + 1]),
         Tlo = std::pow(Real(10), LogTs[iT]);
    Real aE = Real(1) / (std::pow(Real(10), LogE) * std::log10(Ehi / Elo));
    Real aD = Real(1) / (std::pow(Real(10), LogD) * std::log10(Dhi / Dlo));
    Real aT = Real(1) / (std::pow(Real(10), LogT) * std::log10(Thi / Tlo));
    // LINEAR Ye axis: aY = ln10 / (Ys[i+1]-Ys[i]). ln10 survives.
    Real aY = wli::ln10 / (Ys[iY + 1] - Ys[iY]);
    check(wli::is_close(d.dDE, recon * aE * rawE, wli::rtol_relaxed,
                        wli::atol_default),
          "scale factor: E log-axis 1/(X·log10(ratio)), no ln10 (1e-10)");
    check(wli::is_close(d.dDrho, recon * aD * rawD, wli::rtol_relaxed,
                        wli::atol_default),
          "scale factor: rho log-axis 1/(X·log10(ratio)), no ln10 (1e-10)");
    check(wli::is_close(d.dDT, recon * aT * rawT, wli::rtol_relaxed,
                        wli::atol_default),
          "scale factor: T log-axis 1/(X·log10(ratio)), no ln10 (1e-10)");
    check(wli::is_close(d.dDY, recon * aY * rawY, wli::rtol_relaxed,
                        wli::atol_default),
          "scale factor: Ye linear-axis ln10/(spacing) (1e-10)");
    // A swapped ln10 (ln10 on the log E axis) must NOT match the returned deriv.
    check(!wli::is_close(d.dDE, recon * wli::ln10 * aE * rawE, wli::rtol_relaxed,
                         wli::atol_default),
          "log E axis carries NO ln10 (swapped form rejected)");
  }

  // --- Check 6: boundary extrapolation of derivatives (affine closed forms) ---
  {
    // E axis (below/above edge): index clamps, dE unclamped.
    check_affine_derivs(LogEs[0] - 0.8, 6.7, 10.9, 0.22,
                        "boundary derivs below E edge (1e-10)");
    check_affine_derivs(LogEs[nE - 1] + 0.8, 6.7, 10.9, 0.22,
                        "boundary derivs above E edge (1e-10)");
    // rho axis.
    check_affine_derivs(1.1, LogDs[0] - 1.0, 10.9, 0.22,
                        "boundary derivs below rho edge (1e-10)");
    check_affine_derivs(1.1, LogDs[nD - 1] + 1.0, 10.9, 0.22,
                        "boundary derivs above rho edge (1e-10)");
    // T axis.
    check_affine_derivs(1.1, 6.7, LogTs[0] - 0.9, 0.22,
                        "boundary derivs below T edge (1e-10)");
    check_affine_derivs(1.1, 6.7, LogTs[nT - 1] + 0.9, 0.22,
                        "boundary derivs above T edge (1e-10)");
    // Ye axis (LINEAR: extrapolates rather than NaN-ing).
    check_affine_derivs(1.1, 6.7, 10.9, Ys[0] - 0.15,
                        "boundary derivs below Ye edge (1e-10)");
    check_affine_derivs(1.1, 6.7, 10.9, Ys[nY - 1] + 0.15,
                        "boundary derivs above Ye edge (1e-10)");
  }

  // --- Check 7: NaN propagation / linear-axis extrapolation ---
  {
    Real LogE = 1.1, LogD = 6.7, LogT = 10.9, Y = 0.22;
    const Real nanFromNeg = std::log10(Real(-1.0));  // NaN; caller's log10(<0)
    auto all_nan = [](wli::EmAbPointDeriv d) {
      return std::isnan(d.value) && std::isnan(d.dDE) && std::isnan(d.dDrho) &&
             std::isnan(d.dDT) && std::isnan(d.dDY);
    };
    check(all_nan(diff(nanFromNeg, LogD, LogT, Y)),
          "NaN in value + all derivs on non-positive E");
    check(all_nan(diff(LogE, nanFromNeg, LogT, Y)),
          "NaN in value + all derivs on non-positive rho");
    check(all_nan(diff(LogE, LogD, nanFromNeg, Y)),
          "NaN in value + all derivs on non-positive T");
    // Non-positive Ye (linear axis) with valid E,rho,T: finite, extrapolates.
    wli::EmAbPointDeriv dY = diff(LogE, LogD, LogT, Real(-0.1));
    check(!std::isnan(dY.value) && !std::isnan(dY.dDE) &&
              !std::isnan(dY.dDrho) && !std::isnan(dY.dDT) &&
              !std::isnan(dY.dDY),
          "non-positive Ye extrapolates (finite value + all derivs)");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "emab_diff_point: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf(
      "PASS emab_diff_point: EmAb single-point 4D evaluate-and-differentiate "
      "kernel\n");
  return EXIT_SUCCESS;
}
