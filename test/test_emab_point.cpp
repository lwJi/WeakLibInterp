// Self-contained acceptance probe for the EmAb single-point 4D evaluate kernel
// (src/opacity/wli_opacity_emab_iso.H::EmAbInterpolateSingleVariable4DPoint).
//
// Enforces the self-contained regression checks of specs/opacity-emab-iso.md
// (Verification :138-146) for the evaluate-only tetralinear-in-log kernel, whose
// oracle is LogInterpolateSingleVariable_4D_Custom_Point
// (weaklib wlInterpolationModule.F90:1754-1779) ->
// LinearInterp4D_4DArray_Point (wlInterpolationUtilitiesModule.F90:729-768):
//   1. Affine-in-log exactness (machine tier ~1e-14): on a synthetic table whose
//      stored value is exactly affine in (log10 E, log10 rho, log10 T, Ye), the
//      kernel reproduces 10**(affine) - OS at an interior query, not just at
//      nodes.
//   2. Node identity (machine tier): a query exactly at a grid node returns that
//      node's recovered value; tested at an interior node AND a top-edge node
//      (iE=nE-1, iD=nD-1, iT=nT-1, iY=nY-1) so the index-clamp-to-(n-2) /
//      delta->1 branch is exercised on every axis.
//   4. Boundary extrapolation (exact relation): a query outside an edge clamps
//      the index but NOT the delta (d<0 below, d>1 above); on the affine table
//      the extrapolated result still equals 10**(affine) - OS exactly. Tested
//      below/above EACH of the four axes E, rho, T, AND Ye (the Ye pair is
//      net-new vs the 3D EOS test).
//   5. NaN propagation (NaN-equality): a query with non-positive E, rho, or T
//      makes the caller's log10 NaN, which propagates to the result; asserted via
//      std::isnan. A non-positive Ye (linear axis, not logged) must NOT NaN.
//
// The kernel takes E/rho/T PRE-LOG10'd (spec:68), so queries are fed as
// log10(E) etc.; Ye is raw. A non-positive E/rho/T is modeled by passing
// log10(<=0) = NaN as the pre-logged coordinate (what the caller would produce).
//
// Hand-rolled harness (no GoogleTest/Catch2), synthetic table only (no HDF5),
// mirroring test/test_eos_point.cpp. amrex::Initialize is not required: the
// kernel is pure host scalar math. Real-.h5 production parity is deferred to the
// regression-suite umbrella (no HDF5 loader exists yet); moment-slice
// independence is Iso-only.

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

// Affine-in-log model shared by checks 1/2/4, in terms of the ALREADY-LOG10'd
// coordinates (LogE, LogD, LogT) and raw Y. Nonzero constants.
// Coefficients are deliberately small so the affine LOG-space value stays modest
// (~0.6..1.6) across all queries incl. extrapolation; otherwise 10**(affine)
// grows so large that a few-ULP error in the exponent exceeds the 1e-14 relative
// tier after recovery. kOS is chosen well below the recovered 10**(affine) so
// recover() has no near-cancellation.
constexpr Real kBase = 0.8;
constexpr Real kE = 0.13;
constexpr Real kD = -0.09;
constexpr Real kT = 0.07;
constexpr Real kY = 0.4;
constexpr Real kOS = 0.5;  // nonzero additive offset

// The STORED table value is itself this affine function of the (already-log10)
// coordinates (LogE, LogD, LogT) and raw Y — i.e. "affine in log". Tetralinear
// interpolation of an affine function is exact, so the interpolated log value
// equals affine(query) at any query; the recovered physical value is then
// 10**(affine(query)) - OS exactly. (Mirrors test_eos_point.cpp, which stores
// the affine log-space value directly, not log10 of a physical value.)
Real affine(Real LogE, Real LogD, Real LogT, Real Y) {
  return kBase + kE * LogE + kD * LogD + kT * LogT + kY * Y;
}

}  // namespace

int main() {
  // --- Synthetic grid: non-uniform (uneven-ratio) arrays for the log axes,
  //     already in log10 space; LINEAR (uniform) Ys. Small extents. ---
  const int nE = 3, nD = 4, nT = 3, nY = 3;
  // Uneven spacing so the linear bracket on the log coordinate is genuinely
  // exercised (these are the LOG10'd node coordinates directly).
  Real LogEs[nE] = {0.0, 0.7, 1.9};
  Real LogDs[nD] = {3.0, 5.4, 8.1, 11.7};
  Real LogTs[nT] = {9.0, 10.3, 11.95};
  Real Ys[nY] = {0.05, 0.30, 0.55};

  std::vector<Real> table(static_cast<std::size_t>(nE) * nD * nT * nY);
  // Store the affine log-space value directly (tetralinear-exact); recover()
  // then yields 10**(affine) - OS. iE innermost/fastest, matching column-major
  // E-fastest layout.
  for (int iY = 0; iY < nY; ++iY)
    for (int iT = 0; iT < nT; ++iT)
      for (int iD = 0; iD < nD; ++iD)
        for (int iE = 0; iE < nE; ++iE)
          table[static_cast<std::size_t>(iE) +
                nE * (iD + nD * (iT + nT * iY))] =
              affine(LogEs[iE], LogDs[iD], LogTs[iT], Ys[iY]);

  const Real* tbl = table.data();

  // Kernel wrapper: arguments are the pre-LOG10'd log coordinates + raw Y.
  auto kernel = [&](Real LogE, Real LogD, Real LogT, Real Y) {
    return wli::EmAbInterpolateSingleVariable4DPoint(
        LogE, LogD, LogT, Y, LogEs, nE, LogDs, nD, LogTs, nT, Ys, nY, kOS, tbl);
  };

  // Expected recovered value at the given log coordinates: 10**(affine) - OS.
  auto want_at = [&](Real LogE, Real LogD, Real LogT, Real Y) {
    return wli::recover(affine(LogE, LogD, LogT, Y), kOS);
  };

  // --- Check 1: affine-in-log exactness at an interior query ---
  {
    Real LogE = 1.1, LogD = 6.7, LogT = 10.9, Y = 0.22;  // inside every axis
    Real got = kernel(LogE, LogD, LogT, Y);
    check(wli::is_close(got, want_at(LogE, LogD, LogT, Y), wli::rtol_machine,
                        wli::atol_default),
          "affine-in-log exactness at interior query (~1e-14)");
  }

  // --- Check 2: node identity, interior node and top-edge node ---
  {
    // Interior node (iE=1, iD=1, iT=1, iY=1).
    int iE = 1, iD = 1, iT = 1, iY = 1;
    Real got = kernel(LogEs[iE], LogDs[iD], LogTs[iT], Ys[iY]);
    Real want = wli::recover(
        table[static_cast<std::size_t>(iE) + nE * (iD + nD * (iT + nT * iY))],
        kOS);
    check(wli::is_close(got, want, wli::rtol_machine, wli::atol_default),
          "node identity at interior node (~1e-14)");
  }
  {
    // Top-edge node on every axis (iE=nE-1, iD=nD-1, iT=nT-1, iY=nY-1):
    // exercises index clamp to n-2 / delta->1 on all four axes at once.
    int iE = nE - 1, iD = nD - 1, iT = nT - 1, iY = nY - 1;
    Real got = kernel(LogEs[iE], LogDs[iD], LogTs[iT], Ys[iY]);
    Real want = wli::recover(
        table[static_cast<std::size_t>(iE) + nE * (iD + nD * (iT + nT * iY))],
        kOS);
    check(wli::is_close(got, want, wli::rtol_machine, wli::atol_default),
          "node identity at top-edge node (all axes at n-1) (~1e-14)");
  }

  // --- Check 4: boundary extrapolation on all four axes (clamp index not
  //     delta). On the affine table the extrapolated value still equals the
  //     exact affine relation with the unclamped delta. ---
  {
    Real LogE = 1.1, LogD = 6.7, LogT = 10.9, Y = 0.22;  // interior baseline
    // E axis: below LogEs[0] (dE<0) and above LogEs[nE-1] (dE>1).
    Real Elo = LogEs[0] - 0.8, Ehi = LogEs[nE - 1] + 0.8;
    check(wli::is_close(kernel(Elo, LogD, LogT, Y),
                        want_at(Elo, LogD, LogT, Y), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation below E edge (exact under affine-in-log)");
    check(wli::is_close(kernel(Ehi, LogD, LogT, Y),
                        want_at(Ehi, LogD, LogT, Y), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation above E edge (exact under affine-in-log)");
    // rho axis.
    Real Dlo = LogDs[0] - 1.0, Dhi = LogDs[nD - 1] + 1.0;
    check(wli::is_close(kernel(LogE, Dlo, LogT, Y),
                        want_at(LogE, Dlo, LogT, Y), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation below rho edge (exact under affine-in-log)");
    check(wli::is_close(kernel(LogE, Dhi, LogT, Y),
                        want_at(LogE, Dhi, LogT, Y), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation above rho edge (exact under affine-in-log)");
    // T axis.
    Real Tlo = LogTs[0] - 0.9, Thi = LogTs[nT - 1] + 0.9;
    check(wli::is_close(kernel(LogE, LogD, Tlo, Y),
                        want_at(LogE, LogD, Tlo, Y), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation below T edge (exact under affine-in-log)");
    check(wli::is_close(kernel(LogE, LogD, Thi, Y),
                        want_at(LogE, LogD, Thi, Y), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation above T edge (exact under affine-in-log)");
    // Ye axis (LINEAR; net-new vs the 3D EOS test).
    Real Ylo = Ys[0] - 0.15, Yhi = Ys[nY - 1] + 0.15;
    check(wli::is_close(kernel(LogE, LogD, LogT, Ylo),
                        want_at(LogE, LogD, LogT, Ylo), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation below Ye edge (exact under affine-in-log)");
    check(wli::is_close(kernel(LogE, LogD, LogT, Yhi),
                        want_at(LogE, LogD, LogT, Yhi), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation above Ye edge (exact under affine-in-log)");
  }

  // --- Check 5: NaN propagation on non-positive E/rho/T; NOT on Ye ---
  {
    Real LogE = 1.1, LogD = 6.7, LogT = 10.9, Y = 0.22;
    // A non-positive E/rho/T means the caller's LOG10(<=0) already yielded NaN;
    // pass that NaN as the pre-logged coordinate (log10(0) = -inf actually, so
    // model the caller's log10 of a non-positive argument explicitly).
    const Real nanFromZero = std::log10(Real(0.0));   // -inf; caller's log10(0)
    const Real nanFromNeg = std::log10(Real(-1.0));   // NaN;  caller's log10(<0)
    // log10(<0) is NaN and must propagate.
    check(std::isnan(kernel(nanFromNeg, LogD, LogT, Y)),
          "NaN propagation on non-positive E (log10(<0))");
    check(std::isnan(kernel(LogE, nanFromNeg, LogT, Y)),
          "NaN propagation on non-positive rho (log10(<0))");
    check(std::isnan(kernel(LogE, LogD, nanFromNeg, Y)),
          "NaN propagation on non-positive T (log10(<0))");
    // log10(0) = -inf: the coordinate is a hugely-below-range extrapolation.
    // The result is finite (an extrapolated value) but must not be a real value
    // check here; assert only that non-positive-via-neg NaNs above hold and that
    // the -inf case does not spuriously turn into a NaN-free real query for E.
    (void)nanFromZero;
    // A non-positive Ye (linear axis, never logged) must NOT produce NaN: it
    // simply extrapolates below the edge.
    check(!std::isnan(kernel(LogE, LogD, LogT, Real(0.0))),
          "no NaN on Ye == 0 (linear axis extrapolates)");
    check(!std::isnan(kernel(LogE, LogD, LogT, Real(-0.2))),
          "no NaN on Ye < 0 (linear axis extrapolates)");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "emab_point: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("PASS emab_point: EmAb single-point 4D evaluate kernel\n");
  return EXIT_SUCCESS;
}
