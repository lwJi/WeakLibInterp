// Self-contained acceptance probe for the Iso single-point 5D-slice-to-4D
// evaluate-and-differentiate kernel
// (src/opacity/wli_opacity_emab_iso.H::IsoInterpolateDifferentiateSingleVariable5DPoint
//  + the IsoOffset 2D offset selector).
//
// Enforces the derivative-bearing self-contained checks of
// specs/opacity-differentiate.md (Verification :142-148) for the Iso channel,
// whose value path is a fixed-(species,moment) 5D slice interpolated as 4D. The
// derivative assembly is identical to the EmAb 4D kernel one moment-slice down.
//   1. Value parity with the evaluate leaf (parity tier 1e-12/1e-30), BOTH
//      moment slices — the derivative path must not perturb the value.
//   2. Analytic closed form on a per-moment DISTINCT affine-in-log table
//      (relaxed tier 1e-10), with a distinct affine coefficient per axis so a
//      transposed dTetraLineardX_k (or a blended moment slice) is caught:
//        ∂value/∂E = (value+OS)·bE/E,  ∂value/∂rho = (value+OS)·bD/rho,
//        ∂value/∂T = (value+OS)·bT/T,  ∂value/∂Ye  = (value+OS)·ln10·bY,
//      at arbitrary interior (non-node) queries.
//   4. Explicit log-axis vs linear-Ye scale-factor check (relaxed tier).
//   6. Boundary extrapolation of the derivative (exact relation) on all four
//      axes; Ye extrapolates rather than NaN-ing.
//   7. NaN propagation on non-positive E/rho/T; NOT on Ye.
//
// nMom=2 is mandatory: it makes the fixed-iMom slice genuinely strided. The
// kernel takes E/rho/T PRE-LOG10'd; Ye is raw. Hand-rolled harness, synthetic 5D
// table only, mirroring test/test_iso_point.cpp and test/test_emab_diff_point.cpp.

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

// Per-moment coefficient sets. Distinct per axis AND per moment so a transposed
// derivative axis or a blended/wrong slice is caught.
struct Coeffs {
  Real base, bE, bD, bT, bY;
};
constexpr Coeffs kC0{0.80, 0.13, -0.09, 0.07, 0.40};
constexpr Coeffs kC1{0.55, 0.06, 0.11, -0.05, 0.22};

Coeffs coeffs(int iMom) { return iMom == 0 ? kC0 : kC1; }

Real affine_m(int iMom, Real LogE, Real LogD, Real LogT, Real Y) {
  Coeffs c = coeffs(iMom);
  return c.base + c.bE * LogE + c.bD * LogD + c.bT * LogT + c.bY * Y;
}

}  // namespace

int main() {
  // --- Synthetic 5D grid, column-major (nE, nMom, nD, nT, nY). ---
  const int nE = 3, nMom = 2, nD = 4, nT = 3, nY = 3;
  const int nOpacities = 2, nMoments = 2;
  Real LogEs[nE] = {0.0, 0.7, 1.9};
  Real LogDs[nD] = {3.0, 5.4, 8.1, 11.7};
  Real LogTs[nT] = {9.0, 10.3, 11.95};
  Real Ys[nY] = {0.05, 0.30, 0.55};

  std::vector<Real> table(static_cast<std::size_t>(nE) * nMom * nD * nT * nY);
  for (int iY = 0; iY < nY; ++iY)
    for (int iT = 0; iT < nT; ++iT)
      for (int iD = 0; iD < nD; ++iD)
        for (int iMom = 0; iMom < nMom; ++iMom)
          for (int iE = 0; iE < nE; ++iE)
            table[wli::flat_index<5>({iE, iMom, iD, iT, iY},
                                     {nE, nMom, nD, nT, nY})] =
                affine_m(iMom, LogEs[iE], LogDs[iD], LogTs[iT], Ys[iY]);

  const Real* tbl = table.data();

  // 2D offsets Offsets[nOpacities, nMoments], species-major / moment-minor,
  // all-distinct so a transposed lookup picks a wrong element.
  Real offsets[nOpacities * nMoments];
  for (int m = 0; m < nMoments; ++m)
    for (int s = 0; s < nOpacities; ++s)
      offsets[wli::flat_index<2>({s, m}, {nOpacities, nMoments})] =
          0.5 + 0.11 * s + 0.37 * m;

  const int iSpecies = 1;  // nonzero: exercises the species stride
  auto os_of = [&](int iMom) {
    return wli::IsoOffset(offsets, nOpacities, nMoments, iSpecies, iMom);
  };

  auto diff = [&](int iMom, Real LogE, Real LogD, Real LogT, Real Y) {
    return wli::IsoInterpolateDifferentiateSingleVariable5DPoint(
        LogE, LogD, LogT, Y, LogEs, nE, LogDs, nD, LogTs, nT, Ys, nY, iMom, nMom,
        os_of(iMom), tbl);
  };
  auto eval = [&](int iMom, Real LogE, Real LogD, Real LogT, Real Y) {
    return wli::IsoInterpolateSingleVariable5DPoint(
        LogE, LogD, LogT, Y, LogEs, nE, LogDs, nD, LogTs, nT, Ys, nY, iMom, nMom,
        os_of(iMom), tbl);
  };

  // --- Check 1: value parity with the evaluate leaf, both moments ---
  {
    check(wli::is_close(diff(0, 1.1, 6.7, 10.9, 0.22).value,
                        eval(0, 1.1, 6.7, 10.9, 0.22), wli::rtol_parity,
                        wli::atol_default),
          "value parity vs evaluate leaf, iMom=0 (1e-12/1e-30)");
    check(wli::is_close(diff(1, 1.1, 6.7, 10.9, 0.22).value,
                        eval(1, 1.1, 6.7, 10.9, 0.22), wli::rtol_parity,
                        wli::atol_default),
          "value parity vs evaluate leaf, iMom=1 (1e-12/1e-30)");
  }

  // --- Check 2: analytic closed form (distinct coeff per axis & per moment) ---
  auto check_affine_derivs = [&](int iMom, Real LogE, Real LogD, Real LogT,
                                 Real Y, const char* where) {
    wli::IsoPointDeriv d = diff(iMom, LogE, LogD, LogT, Y);
    Coeffs c = coeffs(iMom);
    Real recon = std::pow(Real(10), affine_m(iMom, LogE, LogD, LogT, Y));
    Real E = std::pow(Real(10), LogE);
    Real rho = std::pow(Real(10), LogD);
    Real T = std::pow(Real(10), LogT);
    check(wli::is_close(d.dDE, recon * c.bE / E, wli::rtol_relaxed,
                        wli::atol_default),
          where);
    check(wli::is_close(d.dDrho, recon * c.bD / rho, wli::rtol_relaxed,
                        wli::atol_default),
          where);
    check(wli::is_close(d.dDT, recon * c.bT / T, wli::rtol_relaxed,
                        wli::atol_default),
          where);
    check(wli::is_close(d.dDY, recon * wli::ln10 * c.bY, wli::rtol_relaxed,
                        wli::atol_default),
          where);
  };
  check_affine_derivs(0, 1.1, 6.7, 10.9, 0.22,
                      "affine closed-form derivs, iMom=0 interior (1e-10)");
  check_affine_derivs(1, 0.4, 9.5, 9.6, 0.40,
                      "affine closed-form derivs, iMom=1 interior (1e-10)");

  // --- Check 4: explicit log-axis vs linear-Ye scale factor (iMom=1) ---
  {
    const int iMom = 1;
    Coeffs c = coeffs(iMom);
    Real LogE = 1.05, LogD = 7.2, LogT = 10.6, Y = 0.31;  // interior
    wli::IsoPointDeriv d = diff(iMom, LogE, LogD, LogT, Y);
    Real recon = std::pow(Real(10), affine_m(iMom, LogE, LogD, LogT, Y));
    auto [iE, dE] = wli::GetIndexAndDeltaLin(LogE, LogEs, nE);
    auto [iD, dD] = wli::GetIndexAndDeltaLin(LogD, LogDs, nD);
    auto [iT, dT] = wli::GetIndexAndDeltaLin(LogT, LogTs, nT);
    auto [iY, dY] = wli::GetIndexAndDeltaLin(Y, Ys, nY);
    (void)dE; (void)dD; (void)dT; (void)dY;
    Real rawE = c.bE * (LogEs[iE + 1] - LogEs[iE]);
    Real rawD = c.bD * (LogDs[iD + 1] - LogDs[iD]);
    Real rawT = c.bT * (LogTs[iT + 1] - LogTs[iT]);
    Real rawY = c.bY * (Ys[iY + 1] - Ys[iY]);
    Real aE = Real(1) / (std::pow(Real(10), LogE) *
                         std::log10(std::pow(Real(10), LogEs[iE + 1]) /
                                    std::pow(Real(10), LogEs[iE])));
    Real aD = Real(1) / (std::pow(Real(10), LogD) *
                         std::log10(std::pow(Real(10), LogDs[iD + 1]) /
                                    std::pow(Real(10), LogDs[iD])));
    Real aT = Real(1) / (std::pow(Real(10), LogT) *
                         std::log10(std::pow(Real(10), LogTs[iT + 1]) /
                                    std::pow(Real(10), LogTs[iT])));
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
    check(!wli::is_close(d.dDE, recon * wli::ln10 * aE * rawE, wli::rtol_relaxed,
                         wli::atol_default),
          "log E axis carries NO ln10 (swapped form rejected)");
  }

  // --- Check 6: boundary extrapolation of derivatives (iMom=1) ---
  {
    const int iMom = 1;
    check_affine_derivs(iMom, LogEs[0] - 0.8, 6.7, 10.9, 0.22,
                        "boundary derivs below E edge, iMom=1 (1e-10)");
    check_affine_derivs(iMom, LogEs[nE - 1] + 0.8, 6.7, 10.9, 0.22,
                        "boundary derivs above E edge, iMom=1 (1e-10)");
    check_affine_derivs(iMom, 1.1, LogDs[0] - 1.0, 10.9, 0.22,
                        "boundary derivs below rho edge, iMom=1 (1e-10)");
    check_affine_derivs(iMom, 1.1, LogDs[nD - 1] + 1.0, 10.9, 0.22,
                        "boundary derivs above rho edge, iMom=1 (1e-10)");
    check_affine_derivs(iMom, 1.1, 6.7, LogTs[0] - 0.9, 0.22,
                        "boundary derivs below T edge, iMom=1 (1e-10)");
    check_affine_derivs(iMom, 1.1, 6.7, LogTs[nT - 1] + 0.9, 0.22,
                        "boundary derivs above T edge, iMom=1 (1e-10)");
    check_affine_derivs(iMom, 1.1, 6.7, 10.9, Ys[0] - 0.15,
                        "boundary derivs below Ye edge, iMom=1 (1e-10)");
    check_affine_derivs(iMom, 1.1, 6.7, 10.9, Ys[nY - 1] + 0.15,
                        "boundary derivs above Ye edge, iMom=1 (1e-10)");
  }

  // --- Check 7: NaN propagation / linear-axis extrapolation (iMom=1) ---
  {
    const int iMom = 1;
    Real LogE = 1.1, LogD = 6.7, LogT = 10.9, Y = 0.22;
    const Real nanFromNeg = std::log10(Real(-1.0));  // NaN; caller's log10(<0)
    auto all_nan = [](wli::IsoPointDeriv d) {
      return std::isnan(d.value) && std::isnan(d.dDE) && std::isnan(d.dDrho) &&
             std::isnan(d.dDT) && std::isnan(d.dDY);
    };
    check(all_nan(diff(iMom, nanFromNeg, LogD, LogT, Y)),
          "NaN in value + all derivs on non-positive E, iMom=1");
    check(all_nan(diff(iMom, LogE, nanFromNeg, LogT, Y)),
          "NaN in value + all derivs on non-positive rho, iMom=1");
    check(all_nan(diff(iMom, LogE, LogD, nanFromNeg, Y)),
          "NaN in value + all derivs on non-positive T, iMom=1");
    wli::IsoPointDeriv dY = diff(iMom, LogE, LogD, LogT, Real(-0.1));
    check(!std::isnan(dY.value) && !std::isnan(dY.dDE) &&
              !std::isnan(dY.dDrho) && !std::isnan(dY.dDT) &&
              !std::isnan(dY.dDY),
          "non-positive Ye extrapolates (finite value + all derivs), iMom=1");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "iso_diff_point: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf(
      "PASS iso_diff_point: Iso single-point 5D-slice-to-4D "
      "evaluate-and-differentiate kernel\n");
  return EXIT_SUCCESS;
}
