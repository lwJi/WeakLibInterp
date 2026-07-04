// Self-contained acceptance probe for the EOS single-point evaluate-and-
// differentiate kernel
// (src/eos/wli_eos.H::EosInterpolateDifferentiateSingleVariable3DPoint).
//
// Enforces the derivative-bearing self-contained checks of
// specs/eos-interpolation.md (Verification :110-118) for the matched
// value + (∂/∂rho, ∂/∂T, ∂/∂Ye) kernel:
//   1. Analytic chain-rule closed form (relaxed tier 1e-10): on a synthetic
//      affine-in-log table the interpolant reproduces the affine function
//      exactly, so the chain rule collapses to
//        ∂value/∂rho = (value+OS)·kB/D,  ∂value/∂T = (value+OS)·kC/T,
//        ∂value/∂Ye  = (value+OS)·ln10·kE.
//      Asserted at two interior queries in different cells (spec check 3).
//   2. Finite-difference cross-check (relaxed tier 1e-10): on a NON-affine
//      synthetic table, a 4th-order central difference of the kernel's own
//      .value component (stencil confined to one interior cell) matches the
//      returned analytic derivatives for all three axes (spec check 3).
//   3. Value identity: the .value component is bit-identical (exact ==) to the
//      evaluate _Point kernel at interior, node, and out-of-edge queries.
//   4. Boundary extrapolation of derivatives (relaxed tier): just below/above
//      the rho and T edges the index clamps but the delta does not, so the
//      affine closed forms still hold exactly (spec check 4).
//   5. NaN propagation (NaN-equality): non-positive rho or T makes .value AND
//      all three derivatives NaN; a non-positive Ye (linear axis, valid D,T)
//      stays finite / extrapolates (spec check 5).
//
// Hand-rolled harness (no GoogleTest/Catch2), synthetic tables only (no HDF5),
// mirroring test/test_eos_point.cpp. amrex::Initialize is not required: the
// kernel is pure host scalar math.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "wli_compare.H"
#include "wli_eos.H"
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

// Affine-in-log model shared by checks 1/3/4. Nonzero constants.
constexpr Real kA = 0.7;
constexpr Real kB = 1.3;
constexpr Real kC = -0.9;
constexpr Real kE = 2.1;
constexpr Real kOS = 3.5;  // nonzero additive offset

Real affine(Real D, Real T, Real Y) {
  return kA + kB * std::log10(D) + kC * std::log10(T) + kE * Y;
}

// Non-affine stored model for the finite-difference cross-check. Smooth in
// (log10 rho, log10 T, Ye); genuinely curved so FD exercises the chain rule.
Real nonaffine(Real D, Real T, Real Y) {
  Real lD = std::log10(D), lT = std::log10(T);
  return 0.4 + 0.6 * lD * Y - 0.3 * lT * lT;
}

}  // namespace

int main() {
  // --- Synthetic grid: non-uniform LOG-spaced Ds/Ts, LINEAR Ys. ---
  const int nD = 4, nT = 3, nY = 3;
  Real Ds[nD] = {1.0e3, 5.0e5, 2.0e8, 6.0e11};
  Real Ts[nT] = {1.0e9, 3.0e10, 9.0e11};
  Real Ys[nY] = {0.05, 0.30, 0.55};

  std::vector<Real> atable(static_cast<std::size_t>(nD) * nT * nY);
  std::vector<Real> ntable(static_cast<std::size_t>(nD) * nT * nY);
  for (int iY = 0; iY < nY; ++iY)
    for (int iT = 0; iT < nT; ++iT)
      for (int iD = 0; iD < nD; ++iD) {
        std::size_t idx = static_cast<std::size_t>(iD) + nD * (iT + nT * iY);
        atable[idx] = affine(Ds[iD], Ts[iT], Ys[iY]);
        ntable[idx] = nonaffine(Ds[iD], Ts[iT], Ys[iY]);
      }

  const Real* atbl = atable.data();
  const Real* ntbl = ntable.data();

  auto adiff = [&](Real D, Real T, Real Y) {
    return wli::EosInterpolateDifferentiateSingleVariable3DPoint(
        D, T, Y, Ds, nD, Ts, nT, Ys, nY, kOS, atbl);
  };
  auto ndiff = [&](Real D, Real T, Real Y) {
    return wli::EosInterpolateDifferentiateSingleVariable3DPoint(
        D, T, Y, Ds, nD, Ts, nT, Ys, nY, kOS, ntbl);
  };
  auto nval = [&](Real D, Real T, Real Y) { return ndiff(D, T, Y).value; };

  // --- Check 1: analytic chain-rule closed form on the affine table ---
  auto check_affine_derivs = [&](Real D, Real T, Real Y, const char* where) {
    wli::EosPointDeriv d = adiff(D, T, Y);
    Real recon = std::pow(Real(10), affine(D, T, Y));  // == value + OS
    Real wDrho = recon * kB / D;
    Real wDT = recon * kC / T;
    Real wDY = recon * wli::ln10 * kE;
    check(wli::is_close(d.dDrho, wDrho, wli::rtol_relaxed, wli::atol_default),
          where);
    check(wli::is_close(d.dDT, wDT, wli::rtol_relaxed, wli::atol_default),
          where);
    check(wli::is_close(d.dDY, wDY, wli::rtol_relaxed, wli::atol_default),
          where);
  };
  check_affine_derivs(7.3e6, 1.7e10, 0.22,
                      "affine closed-form derivs, interior cell A (1e-10)");
  check_affine_derivs(3.1e9, 4.4e11, 0.40,
                      "affine closed-form derivs, interior cell B (1e-10)");

  // --- Check 2: 4th-order central-difference cross-check on non-affine table.
  // Query at the center of a single interior cell; relative step h = 1e-3*x
  // keeps the 4-sample stencil well inside the cell (cell widths >= 30x). ---
  {
    Real D = 1.0e7, T = 1.6e11, Y = 0.30;  // interior, comfortably in one cell
    wli::EosPointDeriv d = ndiff(D, T, Y);
    Real hD = 1.0e-3 * D, hT = 1.0e-3 * T, hY = 1.0e-3 * 0.25;
    auto fd = [](Real vm2, Real vm1, Real vp1, Real vp2, Real h) {
      return (-vp2 + 8.0 * vp1 - 8.0 * vm1 + vm2) / (12.0 * h);
    };
    Real fdD = fd(nval(D - 2 * hD, T, Y), nval(D - hD, T, Y),
                  nval(D + hD, T, Y), nval(D + 2 * hD, T, Y), hD);
    Real fdT = fd(nval(D, T - 2 * hT, Y), nval(D, T - hT, Y),
                  nval(D, T + hT, Y), nval(D, T + 2 * hT, Y), hT);
    Real fdY = fd(nval(D, T, Y - 2 * hY), nval(D, T, Y - hY),
                  nval(D, T, Y + hY), nval(D, T, Y + 2 * hY), hY);
    check(wli::is_close(d.dDrho, fdD, wli::rtol_relaxed, wli::atol_default),
          "FD cross-check ∂/∂rho on non-affine table (1e-10)");
    check(wli::is_close(d.dDT, fdT, wli::rtol_relaxed, wli::atol_default),
          "FD cross-check ∂/∂T on non-affine table (1e-10)");
    check(wli::is_close(d.dDY, fdY, wli::rtol_relaxed, wli::atol_default),
          "FD cross-check ∂/∂Ye on non-affine table (1e-10)");
  }

  // --- Check 3: .value bit-identical to the evaluate kernel (exact ==) ---
  {
    auto eval = [&](Real D, Real T, Real Y) {
      return wli::EosInterpolateSingleVariable3DPoint(D, T, Y, Ds, nD, Ts, nT,
                                                      Ys, nY, kOS, ntbl);
    };
    // Interior.
    check(ndiff(7.3e6, 1.7e10, 0.22).value == eval(7.3e6, 1.7e10, 0.22),
          "value identity vs evaluate at interior query");
    // Exactly at a node.
    check(ndiff(Ds[1], Ts[1], Ys[1]).value == eval(Ds[1], Ts[1], Ys[1]),
          "value identity vs evaluate at grid node");
    // Out-of-edge (below rho, above T): index clamps, delta extrapolates.
    check(ndiff(1.0e2, 5.0e12, 0.22).value == eval(1.0e2, 5.0e12, 0.22),
          "value identity vs evaluate at out-of-edge query");
  }

  // --- Check 4: boundary extrapolation of derivatives (affine closed forms) ---
  {
    // Below/above rho edge (index clamps, dD unclamped) at valid T,Y.
    check_affine_derivs(1.0e2, 1.7e10, 0.22,
                        "boundary derivs below rho edge (1e-10)");
    check_affine_derivs(5.0e13, 1.7e10, 0.22,
                        "boundary derivs above rho edge (1e-10)");
    // Below/above T edge.
    check_affine_derivs(7.3e6, 1.0e8, 0.22,
                        "boundary derivs below T edge (1e-10)");
    check_affine_derivs(7.3e6, 5.0e12, 0.22,
                        "boundary derivs above T edge (1e-10)");
  }

  // --- Check 5: NaN propagation / linear-axis extrapolation ---
  {
    Real D = 7.3e6, T = 1.7e10, Y = 0.22;
    auto all_nan = [](wli::EosPointDeriv d) {
      return std::isnan(d.value) && std::isnan(d.dDrho) &&
             std::isnan(d.dDT) && std::isnan(d.dDY);
    };
    check(all_nan(adiff(Real(0.0), T, Y)),
          "NaN in value + all derivs on rho == 0");
    check(all_nan(adiff(Real(-1.0), T, Y)),
          "NaN in value + all derivs on rho < 0");
    check(all_nan(adiff(D, Real(0.0), Y)),
          "NaN in value + all derivs on T == 0");
    check(all_nan(adiff(D, Real(-1.0), Y)),
          "NaN in value + all derivs on T < 0");
    // Non-positive Ye (linear axis) with valid D,T: finite, extrapolates.
    wli::EosPointDeriv dY = adiff(D, T, Real(-0.1));
    check(!std::isnan(dY.value) && !std::isnan(dY.dDrho) &&
              !std::isnan(dY.dDT) && !std::isnan(dY.dDY),
          "non-positive Ye extrapolates (finite value + derivs)");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "eos_diff_point: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf(
      "PASS eos_diff_point: EOS single-point evaluate-and-differentiate kernel\n");
  return EXIT_SUCCESS;
}
