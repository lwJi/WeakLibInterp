// Self-contained acceptance probe for the EOS single-point evaluate kernel
// (src/eos/wli_eos.H::EosInterpolateSingleVariable3DPoint).
//
// Enforces the self-contained regression checks of specs/eos-interpolation.md
// (Verification :110-118) for the evaluate-only trilinear-in-log kernel:
//   1. Affine-in-log exactness (machine tier ~1e-14): on a synthetic table whose
//      stored value is exactly affine in (log10 rho, log10 T, Ye), the kernel
//      reproduces 10**(affine) - OS at an interior query, not just at nodes.
//   2. Node identity (machine tier): a query exactly at a grid node returns that
//      node's recovered value; tested at an interior node AND a top-edge node
//      (iD = nD-1) so the index-clamp-to-(n-2) / delta->1 branch is exercised.
//   4. Boundary extrapolation (exact relation): a query outside an edge clamps
//      the index but NOT the delta (d<0 below, d>1 above); on the affine table
//      the extrapolated result still equals 10**(affine) - OS exactly. Tested
//      below/above the rho edge and below/above the T edge.
//   5. NaN propagation (NaN-equality): a query with non-positive rho (or T)
//      makes log10 NaN, which propagates to the result. Asserted via std::isnan,
//      NOT the tolerance comparator.
//
// Hand-rolled harness (no GoogleTest/Catch2), synthetic tables only (no HDF5),
// mirroring test/test_interp_core.cpp. amrex::Initialize is not required: the
// kernel is pure host scalar math (like test/test_index_roundtrip.cpp's helper).

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

// Affine-in-log model shared by checks 1/2/4. Nonzero constants a,b,c,e.
constexpr Real kA = 0.7;
constexpr Real kB = 1.3;
constexpr Real kC = -0.9;
constexpr Real kE = 2.1;
constexpr Real kOS = 3.5;  // nonzero additive offset

Real affine(Real D, Real T, Real Y) {
  return kA + kB * std::log10(D) + kC * std::log10(T) + kE * Y;
}

}  // namespace

int main() {
  // --- Synthetic grid: non-uniform LOG-spaced Ds/Ts (all strictly positive),
  //     LINEAR (uniform) Ys. Small extents nD=4, nT=3, nY=3. ---
  const int nD = 4, nT = 3, nY = 3;
  // Geometric (log-uniform) is affine in log10; use uneven ratios so the axes
  // are non-uniform in raw space and the log-bracket is genuinely exercised.
  Real Ds[nD] = {1.0e3, 5.0e5, 2.0e8, 6.0e11};
  Real Ts[nT] = {1.0e9, 3.0e10, 9.0e11};
  Real Ys[nY] = {0.05, 0.30, 0.55};

  std::vector<Real> table(static_cast<std::size_t>(nD) * nT * nY);
  for (int iY = 0; iY < nY; ++iY)
    for (int iT = 0; iT < nT; ++iT)
      for (int iD = 0; iD < nD; ++iD)
        table[static_cast<std::size_t>(iD) + nD * (iT + nT * iY)] =
            affine(Ds[iD], Ts[iT], Ys[iY]);

  const Real* tbl = table.data();

  auto kernel = [&](Real D, Real T, Real Y) {
    return wli::EosInterpolateSingleVariable3DPoint(D, T, Y, Ds, nD, Ts, nT, Ys,
                                                    nY, kOS, tbl);
  };

  // --- Check 1: affine-in-log exactness at an interior query ---
  {
    Real D = 7.3e6, T = 1.7e10, Y = 0.22;  // strictly inside every axis
    Real got = kernel(D, T, Y);
    Real want = wli::recover(affine(D, T, Y), kOS);
    check(wli::is_close(got, want, wli::rtol_machine, wli::atol_default),
          "affine-in-log exactness at interior query (~1e-14)");
  }

  // --- Check 2: node identity, interior node and top-edge node ---
  {
    // Interior node (iD=1, iT=1, iY=1).
    int iD = 1, iT = 1, iY = 1;
    Real got = kernel(Ds[iD], Ts[iT], Ys[iY]);
    Real want = wli::recover(
        table[static_cast<std::size_t>(iD) + nD * (iT + nT * iY)], kOS);
    check(wli::is_close(got, want, wli::rtol_machine, wli::atol_default),
          "node identity at interior node (~1e-14)");
  }
  {
    // Top-edge node on rho (iD=nD-1): exercises index clamp to n-2 / delta->1.
    int iD = nD - 1, iT = nT - 1, iY = nY - 1;
    Real got = kernel(Ds[iD], Ts[iT], Ys[iY]);
    Real want = wli::recover(
        table[static_cast<std::size_t>(iD) + nD * (iT + nT * iY)], kOS);
    check(wli::is_close(got, want, wli::rtol_machine, wli::atol_default),
          "node identity at top-edge node iD=nD-1 (~1e-14)");
  }

  // --- Check 4: boundary extrapolation (clamp index not delta) ---
  {
    Real T = 1.7e10, Y = 0.22;
    // Below Ds[0] (still strictly positive): index clamps to 0, dD < 0.
    Real Dlo = 1.0e2;
    Real gotLo = kernel(Dlo, T, Y);
    check(wli::is_close(gotLo, wli::recover(affine(Dlo, T, Y), kOS),
                        wli::rtol_machine, wli::atol_default),
          "boundary extrapolation below rho edge (exact under affine-in-log)");
    // Above Ds[nD-1]: index clamps to nD-2, dD > 1.
    Real Dhi = 5.0e13;
    Real gotHi = kernel(Dhi, T, Y);
    check(wli::is_close(gotHi, wli::recover(affine(Dhi, T, Y), kOS),
                        wli::rtol_machine, wli::atol_default),
          "boundary extrapolation above rho edge (exact under affine-in-log)");
  }
  {
    Real D = 7.3e6, Y = 0.22;
    // Below Ts[0]: dT < 0.
    Real Tlo = 1.0e8;
    Real gotLo = kernel(D, Tlo, Y);
    check(wli::is_close(gotLo, wli::recover(affine(D, Tlo, Y), kOS),
                        wli::rtol_machine, wli::atol_default),
          "boundary extrapolation below T edge (exact under affine-in-log)");
    // Above Ts[nT-1]: dT > 1.
    Real Thi = 5.0e12;
    Real gotHi = kernel(D, Thi, Y);
    check(wli::is_close(gotHi, wli::recover(affine(D, Thi, Y), kOS),
                        wli::rtol_machine, wli::atol_default),
          "boundary extrapolation above T edge (exact under affine-in-log)");
  }

  // --- Check 5: NaN propagation on non-positive rho / T ---
  {
    Real T = 1.7e10, Y = 0.22, D = 7.3e6;
    check(std::isnan(kernel(Real(0.0), T, Y)),
          "NaN propagation on rho == 0");
    check(std::isnan(kernel(Real(-1.0), T, Y)),
          "NaN propagation on rho < 0");
    check(std::isnan(kernel(D, Real(0.0), Y)),
          "NaN propagation on T == 0");
    check(std::isnan(kernel(D, Real(-1.0), Y)),
          "NaN propagation on T < 0");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "eos_point: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("PASS eos_point: EOS single-point evaluate kernel\n");
  return EXIT_SUCCESS;
}
