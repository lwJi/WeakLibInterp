// Self-contained acceptance probe for the EOS temperature-inversion core, DEY
// family (src/lib/wli_eos_inversion.H).
//
// Enforces the self-contained regression checks of specs/eos-inversion.md
// (Verification :144-152) for the DEY NoGuess + Guess entry points:
//   1. Affine-in-log exactness (machine tier ~1e-14), NoGuess: on a synthetic
//      sub-table exactly affine in (log10 rho, log10 T, Ye) the log-linear
//      inverse recovers an interior T* to machine precision.
//   2. Affine exactness, Guess Step-1 hit: a T_Guess in the same cell as T*.
//   3. Guess Step-2 fallthrough: a T_Guess in a different, non-bracketing cell
//      forces full-range bisection; still exact.
//   4. Round-trip invariant (relaxed 1e-10): invert several interior queries and
//      re-evaluate the dependent variable; T and re-evaluated X both close.
//   5. Error codes (exact ==, plus T==0): codes 1/2/3/10/11 through BOTH the
//      NoGuess and Guess wrappers (identical CheckInputError cascade).
//   6. Code 13 (no root, exact ==, T==0): an in-bounds E unreachable at the
//      chosen (rho,Ye) column.
//   7. Highest-T vs nearest-to-guess root selection on a NON-monotone-in-log-T
//      sub-table: NoGuess picks the higher-T root, Guess (guess near the low-T
//      root) picks the low-T root.
//
// Hand-rolled harness (no GoogleTest/Catch2), synthetic tables only (no HDF5),
// mirroring test/test_eos_point.cpp. No amrex::Initialize: pure host scalar math.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "wli_compare.H"
#include "wli_eos.H"            // forward EosInterpolateSingleVariable3DPoint
#include "wli_eos_inversion.H"  // the routines under test
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

// Affine-in-log model (kC<0 => strictly monotone-decreasing in log T => unique
// root). Stored (log) value; the physical dependent value the inversion inverts
// is recover(affine, OS) = 10**affine - OS.
constexpr Real kA = 0.7;
constexpr Real kB = 1.3;
constexpr Real kC = -0.9;
constexpr Real kE = 2.1;
constexpr Real kOS = 3.5;  // nonzero additive offset

Real affine(Real D, Real T, Real Y) {
  return kA + kB * std::log10(D) + kC * std::log10(T) + kE * Y;
}

// Non-monotone-in-log-T model (parabolic, downward): two roots for a suitable
// target at a fixed (rho, Ye).
Real nonaffine(Real D, Real T, Real Y) {
  Real lT = std::log10(T);
  return 0.4 + 0.6 * std::log10(D) * Y - 0.3 * lT * lT;
}

}  // namespace

int main() {
  // --- Synthetic grid: nD=4, nT=5, nY=3. Log-spaced positive Ds/Ts, linear Ys.
  const int nD = 4, nT = 5, nY = 3;
  Real Ds[nD] = {1.0e3, 5.0e5, 2.0e8, 6.0e11};
  Real Ts[nT] = {1.0e-1, 3.0e0, 1.0e1, 4.0e1, 1.0e2};
  Real Ys[nY] = {0.05, 0.30, 0.55};

  auto idx = [&](int iD, int iT, int iY) {
    return static_cast<std::size_t>(iD) + nD * (iT + nT * iY);
  };

  // Affine sub-table (log-stored).
  std::vector<Real> Es(static_cast<std::size_t>(nD) * nT * nY);
  for (int iY = 0; iY < nY; ++iY)
    for (int iT = 0; iT < nT; ++iT)
      for (int iD = 0; iD < nD; ++iD)
        Es[idx(iD, iT, iY)] = affine(Ds[iD], Ts[iT], Ys[iY]);
  const Real* Esd = Es.data();

  // Bounds from the affine table: rho/Ye from the axes, X from the recovered
  // physical value extents.
  wli::EosInversionBounds b;
  b.MinD = Ds[0];
  b.MaxD = Ds[nD - 1];
  b.MinY = Ys[0];
  b.MaxY = Ys[nY - 1];
  {
    Real lo = wli::recover(Es[0], kOS), hi = lo;
    for (std::size_t k = 0; k < Es.size(); ++k) {
      Real v = wli::recover(Es[k], kOS);
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    b.MinX = lo;
    b.MaxX = hi;
  }
  b.initialized = true;

  auto forward = [&](Real D, Real T, Real Y, const Real* tbl) {
    return wli::EosInterpolateSingleVariable3DPoint(D, T, Y, Ds, nD, Ts, nT, Ys,
                                                    nY, kOS, tbl);
  };

  // --- Check 1: affine-in-log exactness (~1e-14), NoGuess. Interior, off-node.
  {
    Real D = 7.3e6, Tstar = 6.0, Y = 0.22;  // Tstar between Ts[1] and Ts[2]
    Real E = forward(D, Tstar, Y, Esd);
    auto r = wli::ComputeTemperatureWith_DEY_NoGuess(D, E, Y, Ds, nD, Ts, nT, Ys,
                                                     nY, kOS, Esd, b);
    check(r.Error == 0 && wli::is_close(r.T, Tstar, wli::rtol_machine),
          "affine-in-log exactness, NoGuess (~1e-14)");
  }

  // --- Check 2: affine exactness, Guess Step-1 hit (guess in T* cell). ---
  {
    Real D = 7.3e6, Tstar = 6.0, Y = 0.22;
    Real E = forward(D, Tstar, Y, Esd);
    Real T_Guess = Ts[1] * 1.01;  // same cell (Ts[1], Ts[2]) as Tstar
    auto r = wli::ComputeTemperatureWith_DEY_Guess(
        D, E, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Esd, T_Guess, b);
    check(r.Error == 0 && wli::is_close(r.T, Tstar, wli::rtol_machine),
          "affine exactness, Guess Step-1 hit (~1e-14)");
  }

  // --- Check 3: Guess Step-2 fallthrough (guess in a non-bracketing cell). ---
  {
    Real D = 7.3e6, Tstar = 6.0, Y = 0.22;
    Real E = forward(D, Tstar, Y, Esd);
    Real T_Guess = Ts[3];  // cell (Ts[3], Ts[4]) does not bracket -> Step 2
    auto r = wli::ComputeTemperatureWith_DEY_Guess(
        D, E, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Esd, T_Guess, b);
    check(r.Error == 0 && wli::is_close(r.T, Tstar, wli::rtol_machine),
          "affine exactness, Guess Step-2 full-range bisection (~1e-14)");
  }

  // --- Check 4: round-trip invariant (relaxed 1e-10), NoGuess + Guess. ---
  {
    Real Dq[3] = {7.3e6, 2.0e4, 1.0e10};
    Real Tq[3] = {6.0, 20.0, 50.0};
    Real Yq[3] = {0.22, 0.10, 0.45};
    bool all_ok = true;
    for (int q = 0; q < 3; ++q) {
      Real D = Dq[q], Tstar = Tq[q], Y = Yq[q];
      Real E = forward(D, Tstar, Y, Esd);

      auto rn = wli::ComputeTemperatureWith_DEY_NoGuess(
          D, E, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Esd, b);
      Real Xn = forward(D, rn.T, Y, Esd);
      all_ok &= (rn.Error == 0) &&
                wli::is_close(rn.T, Tstar, wli::rtol_relaxed) &&
                wli::is_close(Xn, E, wli::rtol_relaxed);

      auto rg = wli::ComputeTemperatureWith_DEY_Guess(
          D, E, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Esd, Tstar, b);
      Real Xg = forward(D, rg.T, Y, Esd);
      all_ok &= (rg.Error == 0) &&
                wli::is_close(rg.T, Tstar, wli::rtol_relaxed) &&
                wli::is_close(Xg, E, wli::rtol_relaxed);
    }
    check(all_ok, "round-trip invariant, NoGuess + Guess (relaxed 1e-10)");
  }

  // --- Check 5: error codes 1/2/3/10/11 through BOTH wrappers (== + T==0). ---
  {
    Real Dok = 7.3e6, Y = 0.22;
    Real Eok = forward(Dok, 6.0, Y, Esd);  // in-range E

    struct Case {
      const char* name;
      Real D, E, Y;
      int  code;
      bool use_default_bounds;  // for code 10
    };
    Real nan = std::nan("");
    Case cases[] = {
        {"code 1 (D below MinD)", Ds[0] * 0.1, Eok, Y, 1, false},
        {"code 2 (E above MaxX)", Dok, b.MaxX * 10.0 + 1.0, Y, 2, false},
        {"code 3 (Y above MaxY)", Dok, Eok, Ys[nY - 1] + 0.5, 3, false},
        {"code 10 (uninitialized)", Dok, Eok, Y, 10, true},
        {"code 11 (NaN input)", nan, Eok, Y, 11, false},
    };
    bool all_ok = true;
    for (auto const& c : cases) {
      wli::EosInversionBounds bc = c.use_default_bounds
                                       ? wli::EosInversionBounds{}
                                       : b;
      auto rn = wli::ComputeTemperatureWith_DEY_NoGuess(
          c.D, c.E, c.Y, Ds, nD, Ts, nT, Ys, nY, kOS, Esd, bc);
      auto rg = wli::ComputeTemperatureWith_DEY_Guess(
          c.D, c.E, c.Y, Ds, nD, Ts, nT, Ys, nY, kOS, Esd, 6.0, bc);
      bool ok = rn.Error == c.code && rn.T == Real(0) &&
                rg.Error == c.code && rg.T == Real(0);
      if (!ok) std::fprintf(stderr, "  (%s: NoGuess {%d,%g} Guess {%d,%g})\n",
                            c.name, rn.Error, rn.T, rg.Error, rg.T);
      all_ok &= ok;
    }
    check(all_ok, "error codes 1/2/3/10/11, both wrappers (== and T==0)");
  }

  // --- Check 6: code 13 (in-bounds E unreachable at the chosen column). ---
  {
    // Column (Ds[0], Ys[0]) has its largest E at the smallest T (kC<0). Target
    // an E above that column's max but still <= global MaxX (reachable only at
    // higher rho/Ye) => in-bounds, no sign-change bracket => code 13.
    Real D = Ds[0], Y = Ys[0];
    Real colMaxAffine = affine(Ds[0], Ts[0], Ys[0]);  // column max (smallest T)
    Real E = wli::recover(colMaxAffine + 0.5, kOS);    // above column, below MaxX
    // Sanity: E is strictly in-bounds so it passes CheckInputError.
    bool in_bounds = (E >= b.MinX && E <= b.MaxX);
    auto rn = wli::ComputeTemperatureWith_DEY_NoGuess(D, E, Y, Ds, nD, Ts, nT, Ys,
                                                      nY, kOS, Esd, b);
    auto rg = wli::ComputeTemperatureWith_DEY_Guess(
        D, E, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Esd, 6.0, b);
    check(in_bounds && rn.Error == 13 && rn.T == Real(0) && rg.Error == 13 &&
              rg.T == Real(0),
          "code 13 no-root, both wrappers (== and T==0)");
  }

  // --- Check 7: highest-T vs nearest-to-guess root on a NON-monotone table. ---
  {
    std::vector<Real> Es2(static_cast<std::size_t>(nD) * nT * nY);
    for (int iY = 0; iY < nY; ++iY)
      for (int iT = 0; iT < nT; ++iT)
        for (int iD = 0; iD < nD; ++iD)
          Es2[idx(iD, iT, iY)] = nonaffine(Ds[iD], Ts[iT], Ys[iY]);
    const Real* Es2d = Es2.data();

    wli::EosInversionBounds b2;
    b2.MinD = Ds[0];
    b2.MaxD = Ds[nD - 1];
    b2.MinY = Ys[0];
    b2.MaxY = Ys[nY - 1];
    {
      Real lo = wli::recover(Es2[0], kOS), hi = lo;
      for (std::size_t k = 0; k < Es2.size(); ++k) {
        Real v = wli::recover(Es2[k], kOS);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
      }
      b2.MinX = lo;
      b2.MaxX = hi;
    }
    b2.initialized = true;

    // At (Ds[1], Ys[1]) the (log-stored) face values over Ts are
    // {1.126, 1.358, 1.126, 0.656, 0.226}: rise then fall (peak at node 1).
    // Target affine 1.2 => one root in (Ts[0],Ts[1]) [low-T] and one in
    // (Ts[1],Ts[2]) [high-T].
    Real D = Ds[1], Y = Ys[1];
    Real E2 = wli::recover(1.2, kOS);

    auto rn = wli::ComputeTemperatureWith_DEY_NoGuess(
        D, E2, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Es2d, b2);
    // T_Guess=0.5 lands (via the log index) in cell (Ts[0],Ts[1]) which brackets
    // the low-T root => Guess Step 1 returns the low-T root.
    auto rg = wli::ComputeTemperatureWith_DEY_Guess(
        D, E2, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Es2d, 0.5, b2);

    bool ok = rn.Error == 0 && rg.Error == 0 &&
              rn.T > Ts[1] && rn.T < Ts[2] &&   // NoGuess: high-T root in (3,10)
              rg.T > Ts[0] && rg.T < Ts[1] &&   // Guess:   low-T root in (0.1,3)
              rn.T > rg.T;                       // highest-T > nearest-to-guess
    if (!ok)
      std::fprintf(stderr, "  (non-monotone: NoGuess {%d,%g} Guess {%d,%g})\n",
                   rn.Error, rn.T, rg.Error, rg.T);
    check(ok, "highest-T (NoGuess) vs nearest-to-guess (Guess) root selection");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "eos_inversion_dey: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("PASS eos_inversion_dey: EOS temperature-inversion core (DEY)\n");
  return EXIT_SUCCESS;
}
