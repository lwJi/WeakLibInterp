// Self-contained acceptance probe for the scalar multilinear interpolation core
// (src/core/wli_interp.H) and the shared tolerance comparator
// (src/core/wli_compare.H).
//
// Enforces the self-contained regression checks of the cross-cutting numeric
// contract (specs/fortran-parity-and-tolerances.md:135-142):
//   1. Node identity: querying exactly at a grid node recovers that node's
//      value to the machine tier; index in [0,n-2] with delta 0 (or 1 at top).
//   2. Affine-in-log exactness: a table whose stored = log10(value+OS) is an
//      exact affine function of the (log/linear) node coordinates is reproduced
//      to the machine tier at any interior query (constant table = degenerate).
//   3. Clamp-index boundary / extrapolation (spec:111-116): an out-of-range
//      query clamps the INDEX to the edge cell (exact ==) but NOT the delta
//      (d<0 below, d>1 above); the kernel linearly extrapolates the edge cell
//      (matches the affine closed form) -- not NaN, not clamped.
//   4. NaN propagation (spec:116): a non-positive log argument yields a NaN
//      result that propagates silently through recovery (no abort).
//   5. Comparator self-check: the mixed rtol/atol formula and the atol floor.
//
// Hand-rolled harness (no GoogleTest/Catch2), synthetic tables only (no HDF5),
// mirroring test/test_index_roundtrip.cpp.

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <AMReX.H>

#include "wli_compare.H"
#include "wli_interp.H"
#include "wli_real.H"

namespace {

using wli::Real;

int g_failures = 0;

void check(bool ok, const char* msg) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  }
}

// ------------------------------------------------------------------
// 1. Node identity
// ------------------------------------------------------------------
void test_node_identity() {
  const Real OS = Real(3.5);          // additive offset
  const int n = 6;

  // Log axis (geometric spacing) and linear axis (uniform spacing).
  Real logXs[n], linXs[n];
  for (int k = 0; k < n; ++k) {
    logXs[k] = Real(1e-2) * std::pow(Real(10), Real(k) * Real(0.7));  // decade-ish
    linXs[k] = Real(0.05) + Real(0.15) * Real(k);
  }

  // Distinct physical node values; stored = log10(value + OS).
  Real val[n], stored[n];
  for (int k = 0; k < n; ++k) {
    val[k] = Real(2.0) + Real(1.3) * Real(k) + Real(0.02) * Real(k) * Real(k);
    stored[k] = std::log10(val[k] + OS);
  }

  // Log axis: query at each node coordinate.
  for (int k = 0; k < n; ++k) {
    auto id = wli::GetIndexAndDeltaLog(logXs[k], logXs, n);
    check(id.i >= 0 && id.i <= n - 2, "node/log: index in [0,n-2]");
    // At a node the bracket lands in either the cell above (d==0) or, when the
    // index rounds down to the cell below, its top edge (d==1) -- both exact.
    check(id.d == Real(0) || id.d == Real(1),
          "node/log: delta is exactly 0 or 1 at a node");
    Real got = wli::recover(wli::Linear(stored[id.i], stored[id.i + 1], id.d), OS);
    check(wli::is_close(got, val[k], wli::rtol_machine),
          "node/log: recovered value == node value");
  }

  // Linear axis.
  for (int k = 0; k < n; ++k) {
    auto id = wli::GetIndexAndDeltaLin(linXs[k], linXs, n);
    check(id.i >= 0 && id.i <= n - 2, "node/lin: index in [0,n-2]");
    check(id.d == Real(0) || id.d == Real(1),
          "node/lin: delta is exactly 0 or 1 at a node");
    Real got = wli::recover(wli::Linear(stored[id.i], stored[id.i + 1], id.d), OS);
    check(wli::is_close(got, val[k], wli::rtol_machine),
          "node/lin: recovered value == node value");
  }
}

// ------------------------------------------------------------------
// 2. Affine-in-log exactness (3D: two log axes + one linear axis)
// ------------------------------------------------------------------
// stored[i,j,k] = a0 + a1*log10(X1) + a2*log10(X2) + a3*X3, with X1,X2 on log
// axes and X3 on a linear axis. Multilinear interpolation of an affine function
// of the interpolation coordinates is exact, so recover(TriLinear(...)) equals
// 10**(a0+a1*log10(X1q)+a2*log10(X2q)+a3*X3q) - OS at any interior query.
void affine3d_case(Real a0, Real a1, Real a2, Real a3, const char* tag) {
  const Real OS = Real(1.25);
  const int n1 = 5, n2 = 4, n3 = 6;

  Real X1s[n1], X2s[n2], X3s[n3];
  for (int i = 0; i < n1; ++i) X1s[i] = Real(1e-1) * std::pow(Real(10), Real(i) * Real(0.5));
  for (int j = 0; j < n2; ++j) X2s[j] = Real(2e0) * std::pow(Real(10), Real(j) * Real(0.6));
  for (int k = 0; k < n3; ++k) X3s[k] = Real(0.02) + Real(0.11) * Real(k);

  auto storedAt = [&](int i, int j, int k) {
    return a0 + a1 * std::log10(X1s[i]) + a2 * std::log10(X2s[j]) + a3 * X3s[k];
  };

  // Interior query.
  Real X1q = Real(0.9), X2q = Real(11.0), X3q = Real(0.37);

  auto id1 = wli::GetIndexAndDeltaLog(X1q, X1s, n1);
  auto id2 = wli::GetIndexAndDeltaLog(X2q, X2s, n2);
  auto id3 = wli::GetIndexAndDeltaLin(X3q, X3s, n3);
  const int i = id1.i, j = id2.i, k = id3.i;

  // Corner gather: p[b1 b2 b3] with b1 (axis 1) least-significant.
  Real p000 = storedAt(i, j, k),         p100 = storedAt(i + 1, j, k);
  Real p010 = storedAt(i, j + 1, k),     p110 = storedAt(i + 1, j + 1, k);
  Real p001 = storedAt(i, j, k + 1),     p101 = storedAt(i + 1, j, k + 1);
  Real p011 = storedAt(i, j + 1, k + 1), p111 = storedAt(i + 1, j + 1, k + 1);

  Real f = wli::TriLinear(p000, p100, p010, p110, p001, p101, p011, p111,
                          id1.d, id2.d, id3.d);
  Real got = wli::recover(f, OS);
  Real want = std::pow(Real(10),
                       a0 + a1 * std::log10(X1q) + a2 * std::log10(X2q) + a3 * X3q) - OS;
  check(wli::is_close(got, want, wli::rtol_machine), tag);
}

void test_affine_in_log() {
  affine3d_case(Real(0.7), Real(1.3), Real(-0.4), Real(2.1),
                "affine3d: interior query reproduces affine-in-log to machine tier");
  // Degenerate constant table.
  affine3d_case(Real(0.55), Real(0), Real(0), Real(0),
                "affine3d(const): constant table reproduced to machine tier");
}

// ------------------------------------------------------------------
// 3. Clamp-index boundary / extrapolation (1D log axis, affine-in-log)
// ------------------------------------------------------------------
void test_boundary_extrapolation() {
  const Real OS = Real(0.75);
  const int n = 5;
  const Real a0 = Real(0.4), a1 = Real(1.7);  // stored = a0 + a1*log10(X)

  Real Xs[n], stored[n];
  for (int k = 0; k < n; ++k) {
    Xs[k] = Real(1.0) * std::pow(Real(10), Real(k) * Real(0.4));
    stored[k] = a0 + a1 * std::log10(Xs[k]);
  }

  // Below range: X < Xs[0].
  {
    Real Xq = Xs[0] * Real(0.3);
    auto id = wli::GetIndexAndDeltaLog(Xq, Xs, n);
    check(id.i == 0, "boundary(below): index exactly clamped to 0");
    check(id.d < Real(0), "boundary(below): delta < 0 (unclamped)");
    Real got = wli::recover(wli::Linear(stored[id.i], stored[id.i + 1], id.d), OS);
    Real want = std::pow(Real(10), a0 + a1 * std::log10(Xq)) - OS;
    check(!std::isnan(got), "boundary(below): result is not NaN");
    check(wli::is_close(got, want, wli::rtol_machine),
          "boundary(below): linear extrapolation of edge cell");
  }

  // Above range: X > Xs[n-1].
  {
    Real Xq = Xs[n - 1] * Real(4.0);
    auto id = wli::GetIndexAndDeltaLog(Xq, Xs, n);
    check(id.i == n - 2, "boundary(above): index exactly clamped to n-2");
    check(id.d > Real(1), "boundary(above): delta > 1 (unclamped)");
    Real got = wli::recover(wli::Linear(stored[id.i], stored[id.i + 1], id.d), OS);
    Real want = std::pow(Real(10), a0 + a1 * std::log10(Xq)) - OS;
    check(!std::isnan(got), "boundary(above): result is not NaN");
    check(wli::is_close(got, want, wli::rtol_machine),
          "boundary(above): linear extrapolation of edge cell");
  }
}

// ------------------------------------------------------------------
// 4. NaN propagation on non-positive log argument
// ------------------------------------------------------------------
void test_nan_propagation() {
  const Real OS = Real(1.0);
  const int n = 4;
  Real Xs[n], stored[n];
  for (int k = 0; k < n; ++k) {
    Xs[k] = Real(1.0) * std::pow(Real(10), Real(k) * Real(0.5));
    stored[k] = std::log10(Real(2.0) + Real(k) + OS);
  }

  Real Xq = Real(-5.0);  // non-positive -> log10 NaN
  auto id = wli::GetIndexAndDeltaLog(Xq, Xs, n);
  Real got = wli::recover(wli::Linear(stored[id.i], stored[id.i + 1], id.d), OS);
  check(std::isnan(got), "nan: non-positive log argument propagates to NaN result");
}

// ------------------------------------------------------------------
// 5. Comparator self-check
// ------------------------------------------------------------------
void test_comparator() {
  // Exact equality passes at every tier.
  check(wli::is_close(Real(1.0), Real(1.0), wli::rtol_parity), "cmp: exact equal");

  // Just inside / just outside the relative band at the machine tier.
  Real e = Real(1.0);
  check(wli::is_close(e + Real(0.5e-14), e, wli::rtol_machine),
        "cmp: within rtol_machine band");
  check(!wli::is_close(e + Real(2e-14), e, wli::rtol_machine),
        "cmp: outside rtol_machine band");

  // Relative scaling: at |expected|=1e6 the machine band is 1e-14*1e6 = 1e-8,
  // so a 0.5e-8 absolute deviation is admitted (it would fail at |expected|=1).
  check(wli::is_close(Real(1e6) + Real(0.5e-8), Real(1e6), wli::rtol_machine),
        "cmp: relative band scales with magnitude");
  check(!wli::is_close(Real(1.0) + Real(0.5e-8), Real(1.0), wli::rtol_machine),
        "cmp: same deviation fails at unit magnitude");

  // atol floor governs near zero (relative term vanishes).
  check(wli::is_close(Real(0.5e-30), Real(0.0), wli::rtol_parity),
        "cmp: atol floor passes tiny value vs zero");
  check(!wli::is_close(Real(1e-20), Real(0.0), wli::rtol_parity),
        "cmp: value above atol floor fails vs zero");

  // Parity tier is looser than machine tier.
  check(wli::is_close(e + Real(1e-13), e, wli::rtol_parity),
        "cmp: parity tier admits 1e-13");
  check(!wli::is_close(e + Real(1e-13), e, wli::rtol_machine),
        "cmp: machine tier rejects 1e-13");
}

// ------------------------------------------------------------------
// 6. ln10 literal (wli_interp.H): the constexpr literal (nvcc cannot
//    dynamically initialize device-visible globals) must be bit-identical
//    to the runtime std::log the Fortran oracle's LOG(10) corresponds to.
// ------------------------------------------------------------------
void test_ln10_literal() {
  check(wli::ln10 == std::log(Real(10)), "ln10: literal == std::log(10)");
}

}  // namespace

int main(int argc, char* argv[]) {
  amrex::Initialize(argc, argv);

  test_node_identity();
  test_affine_in_log();
  test_boundary_extrapolation();
  test_nan_propagation();
  test_comparator();
  test_ln10_literal();

  amrex::Finalize();

  if (g_failures != 0) {
    std::fprintf(stderr, "interp_core: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("PASS interp_core: node identity, affine-in-log, boundary "
              "extrapolation, NaN propagation, comparator\n");
  return EXIT_SUCCESS;
}
