// Self-contained acceptance probe for the EOS temperature-inversion FAMILY
// wrappers DPY (X = pressure) and DSY (X = entropy per baryon), plus the
// bare-Real `_NoError` reporting forms (src/eos/wli_eos_inversion.H).
//
// The three families share one generic _DXY kernel; a sub-table is all the
// algorithm sees, so the synthetic affine/nonaffine models reused here for the P
// and S sub-tables exercise exactly the same code paths the DEY test pins. This
// probe re-runs, for BOTH DPY and DSY, the full DEY check set of
// specs/eos-inversion.md (Verification :144-152), factored into a templated
// run_family() so each family runs it without copy-paste:
//   1. Affine-in-log exactness (machine tier ~1e-14), NoGuess.
//   2. Affine exactness, Guess Step-1 hit (guess in T* cell).
//   3. Affine exactness, Guess Step-2 full-range fallthrough.
//   4. Round-trip invariant (relaxed 1e-10), NoGuess + Guess.
//   5. Error codes 1/2/3/10/11 through both _Error wrappers (== + T==0).
//   6. Code 13 (no root, in-bounds unreachable column; == + T==0).
//   7. Highest-T (NoGuess) vs nearest-to-guess (Guess) root selection on a
//      non-monotone-in-log-T sub-table.
// Plus, beyond the DEY set, the `_NoError` contract (spec:108-111): the bare-Real
// return equals the _Error .T on success, and == 0 on failure (code 2 and the
// code-13 no-root case) — proving T==0 is the sole failure signal.
//
// Hand-rolled harness (no GoogleTest/Catch2), synthetic tables only (no HDF5),
// mirroring test/test_eos_inversion_dey.cpp. No amrex::Initialize: host scalar.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "wli_compare.H"
#include "wli_eos.H"            // forward EosInterpolateSingleVariable3DPoint
#include "wli_eos_inversion.H"  // the routines under test
#include "wli_real.H"

namespace {

using wli::Real;

int g_failures = 0;

void check(bool ok, const char* fam, const char* msg) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: [%s] %s\n", fam, msg);
    ++g_failures;
  } else {
    std::printf("  ok: [%s] %s\n", fam, msg);
  }
}

// Affine-in-log model (kC<0 => strictly monotone-decreasing in log T => unique
// root). Stored (log) value; the physical dependent value the inversion inverts
// is recover(affine, OS) = 10**affine - OS. Reused for BOTH the P and S
// sub-tables: a generic sub-table is all the algorithm sees.
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

// Run the full 7-check DEY set + the _NoError contract for one family, given its
// four wrappers as callables. `noguess`/`guess` are the _Error (struct-returning)
// forms; `noguess_ne`/`guess_ne` the bare-Real _NoError forms.
template <class NoGuessFn, class GuessFn, class NoGuessNE, class GuessNE>
void run_family(const char* fam, NoGuessFn noguess, GuessFn guess,
                NoGuessNE noguess_ne, GuessNE guess_ne) {
  // --- Synthetic grid: nD=4, nT=5, nY=3. Log-spaced Ds/Ts, linear Ys. ---
  const int nD = 4, nT = 5, nY = 3;
  Real Ds[nD] = {1.0e3, 5.0e5, 2.0e8, 6.0e11};
  Real Ts[nT] = {1.0e-1, 3.0e0, 1.0e1, 4.0e1, 1.0e2};
  Real Ys[nY] = {0.05, 0.30, 0.55};

  auto idx = [&](int iD, int iT, int iY) {
    return static_cast<std::size_t>(iD) + nD * (iT + nT * iY);
  };

  // Affine sub-table (log-stored) — used as the family's dependent sub-table.
  std::vector<Real> Xs(static_cast<std::size_t>(nD) * nT * nY);
  for (int iY = 0; iY < nY; ++iY)
    for (int iT = 0; iT < nT; ++iT)
      for (int iD = 0; iD < nD; ++iD)
        Xs[idx(iD, iT, iY)] = affine(Ds[iD], Ts[iT], Ys[iY]);
  const Real* Xsd = Xs.data();

  wli::EosInversionBounds b;
  b.MinD = Ds[0];
  b.MaxD = Ds[nD - 1];
  b.MinY = Ys[0];
  b.MaxY = Ys[nY - 1];
  {
    Real lo = wli::recover(Xs[0], kOS), hi = lo;
    for (std::size_t k = 0; k < Xs.size(); ++k) {
      Real v = wli::recover(Xs[k], kOS);
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
    Real D = 7.3e6, Tstar = 6.0, Y = 0.22;  // between Ts[1] and Ts[2]
    Real X = forward(D, Tstar, Y, Xsd);
    auto r = noguess(D, X, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, b);
    check(r.Error == 0 && wli::is_close(r.T, Tstar, wli::rtol_machine), fam,
          "affine-in-log exactness, NoGuess (~1e-14)");
  }

  // --- Check 2: affine exactness, Guess Step-1 hit (guess in T* cell). ---
  {
    Real D = 7.3e6, Tstar = 6.0, Y = 0.22;
    Real X = forward(D, Tstar, Y, Xsd);
    Real T_Guess = Ts[1] * 1.01;  // same cell (Ts[1], Ts[2]) as Tstar
    auto r = guess(D, X, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, T_Guess, b);
    check(r.Error == 0 && wli::is_close(r.T, Tstar, wli::rtol_machine), fam,
          "affine exactness, Guess Step-1 hit (~1e-14)");
  }

  // --- Check 3: Guess Step-2 fallthrough (guess in a non-bracketing cell). ---
  {
    Real D = 7.3e6, Tstar = 6.0, Y = 0.22;
    Real X = forward(D, Tstar, Y, Xsd);
    Real T_Guess = Ts[3];  // cell (Ts[3], Ts[4]) does not bracket -> Step 2
    auto r = guess(D, X, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, T_Guess, b);
    check(r.Error == 0 && wli::is_close(r.T, Tstar, wli::rtol_machine), fam,
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
      Real X = forward(D, Tstar, Y, Xsd);

      auto rn = noguess(D, X, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, b);
      Real Xn = forward(D, rn.T, Y, Xsd);
      all_ok &= (rn.Error == 0) &&
                wli::is_close(rn.T, Tstar, wli::rtol_relaxed) &&
                wli::is_close(Xn, X, wli::rtol_relaxed);

      auto rg = guess(D, X, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, Tstar, b);
      Real Xg = forward(D, rg.T, Y, Xsd);
      all_ok &= (rg.Error == 0) &&
                wli::is_close(rg.T, Tstar, wli::rtol_relaxed) &&
                wli::is_close(Xg, X, wli::rtol_relaxed);
    }
    check(all_ok, fam, "round-trip invariant, NoGuess + Guess (relaxed 1e-10)");
  }

  // --- Check 5: error codes 1/2/3/10/11 through BOTH wrappers (== + T==0). ---
  {
    Real Dok = 7.3e6, Y = 0.22;
    Real Xok = forward(Dok, 6.0, Y, Xsd);  // in-range X

    struct Case {
      const char* name;
      Real D, X, Y;
      int  code;
      bool use_default_bounds;  // for code 10
    };
    Real nan = std::nan("");
    Case cases[] = {
        {"code 1 (D below MinD)", Ds[0] * 0.1, Xok, Y, 1, false},
        {"code 2 (X above MaxX)", Dok, b.MaxX * 10.0 + 1.0, Y, 2, false},
        {"code 3 (Y above MaxY)", Dok, Xok, Ys[nY - 1] + 0.5, 3, false},
        {"code 10 (uninitialized)", Dok, Xok, Y, 10, true},
        {"code 11 (NaN input)", nan, Xok, Y, 11, false},
    };
    bool all_ok = true;
    for (auto const& c : cases) {
      wli::EosInversionBounds bc =
          c.use_default_bounds ? wli::EosInversionBounds{} : b;
      auto rn = noguess(c.D, c.X, c.Y, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, bc);
      auto rg =
          guess(c.D, c.X, c.Y, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, 6.0, bc);
      bool ok = rn.Error == c.code && rn.T == Real(0) && rg.Error == c.code &&
                rg.T == Real(0);
      if (!ok)
        std::fprintf(stderr, "  (%s: NoGuess {%d,%g} Guess {%d,%g})\n", c.name,
                     rn.Error, rn.T, rg.Error, rg.T);
      all_ok &= ok;
    }
    check(all_ok, fam, "error codes 1/2/3/10/11, both wrappers (== and T==0)");
  }

  // --- Check 6: code 13 (in-bounds X unreachable at the chosen column). ---
  {
    Real D = Ds[0], Y = Ys[0];
    Real colMaxAffine = affine(Ds[0], Ts[0], Ys[0]);  // column max (smallest T)
    Real X = wli::recover(colMaxAffine + 0.5, kOS);    // above column, below MaxX
    bool in_bounds = (X >= b.MinX && X <= b.MaxX);
    auto rn = noguess(D, X, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, b);
    auto rg = guess(D, X, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, 6.0, b);
    check(in_bounds && rn.Error == 13 && rn.T == Real(0) && rg.Error == 13 &&
              rg.T == Real(0),
          fam, "code 13 no-root, both wrappers (== and T==0)");
  }

  // --- Check 7: highest-T vs nearest-to-guess root on a NON-monotone table. ---
  {
    std::vector<Real> X2(static_cast<std::size_t>(nD) * nT * nY);
    for (int iY = 0; iY < nY; ++iY)
      for (int iT = 0; iT < nT; ++iT)
        for (int iD = 0; iD < nD; ++iD)
          X2[idx(iD, iT, iY)] = nonaffine(Ds[iD], Ts[iT], Ys[iY]);
    const Real* X2d = X2.data();

    wli::EosInversionBounds b2;
    b2.MinD = Ds[0];
    b2.MaxD = Ds[nD - 1];
    b2.MinY = Ys[0];
    b2.MaxY = Ys[nY - 1];
    {
      Real lo = wli::recover(X2[0], kOS), hi = lo;
      for (std::size_t k = 0; k < X2.size(); ++k) {
        Real v = wli::recover(X2[k], kOS);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
      }
      b2.MinX = lo;
      b2.MaxX = hi;
    }
    b2.initialized = true;

    // At (Ds[1], Ys[1]) the face rises then falls (peak at node 1); target
    // affine 1.2 => a low-T root in (Ts[0],Ts[1]) and a high-T root in
    // (Ts[1],Ts[2]).
    Real D = Ds[1], Y = Ys[1];
    Real X2q = wli::recover(1.2, kOS);

    auto rn = noguess(D, X2q, Y, Ds, nD, Ts, nT, Ys, nY, kOS, X2d, b2);
    auto rg = guess(D, X2q, Y, Ds, nD, Ts, nT, Ys, nY, kOS, X2d, 0.5, b2);

    bool ok = rn.Error == 0 && rg.Error == 0 && rn.T > Ts[1] && rn.T < Ts[2] &&
              rg.T > Ts[0] && rg.T < Ts[1] && rn.T > rg.T;
    if (!ok)
      std::fprintf(stderr, "  (non-monotone: NoGuess {%d,%g} Guess {%d,%g})\n",
                   rn.Error, rn.T, rg.Error, rg.T);
    check(ok, fam,
          "highest-T (NoGuess) vs nearest-to-guess (Guess) root selection");
  }

  // --- _NoError contract (beyond the DEY set): bare-Real return equals the
  //     _Error .T on success, and == 0 on failure (sole failure signal). ---
  {
    // Success case: NoGuess + Guess bare-Real returns match their _Error .T.
    Real D = 7.3e6, Tstar = 6.0, Y = 0.22;
    Real X = forward(D, Tstar, Y, Xsd);
    auto rn = noguess(D, X, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, b);
    auto rg = guess(D, X, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, Tstar, b);
    Real tn = noguess_ne(D, X, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, b);
    Real tg = guess_ne(D, X, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, Tstar, b);
    bool succ_ok = tn == rn.T && tn != Real(0) && tg == rg.T && tg != Real(0);

    // Failure case (code 2, out-of-bounds X): bare-Real return == 0.
    Real Xhi = b.MaxX * 10.0 + 1.0;
    Real en = noguess_ne(D, Xhi, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, b);
    Real eg = guess_ne(D, Xhi, Y, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, 6.0, b);
    bool err_ok = en == Real(0) && eg == Real(0);

    // No-root case (code 13): bare-Real return == 0.
    Real Dc = Ds[0], Yc = Ys[0];
    Real Xc = wli::recover(affine(Ds[0], Ts[0], Ys[0]) + 0.5, kOS);
    Real cn = noguess_ne(Dc, Xc, Yc, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, b);
    Real cg = guess_ne(Dc, Xc, Yc, Ds, nD, Ts, nT, Ys, nY, kOS, Xsd, 6.0, b);
    bool noroot_ok = cn == Real(0) && cg == Real(0);

    check(succ_ok && err_ok && noroot_ok, fam,
          "_NoError: T matches _Error on success, ==0 on failure/no-root");
  }
}

}  // namespace

int main() {
  // DPY family (X = pressure sub-table).
  run_family(
      "DPY",
      [](Real D, Real X, Real Y, const Real* Ds, int nD, const Real* Ts, int nT,
         const Real* Ys, int nY, Real OS, const Real* Xs,
         const wli::EosInversionBounds& b) {
        return wli::ComputeTemperatureWith_DPY_NoGuess(D, X, Y, Ds, nD, Ts, nT,
                                                       Ys, nY, OS, Xs, b);
      },
      [](Real D, Real X, Real Y, const Real* Ds, int nD, const Real* Ts, int nT,
         const Real* Ys, int nY, Real OS, const Real* Xs, Real T_Guess,
         const wli::EosInversionBounds& b) {
        return wli::ComputeTemperatureWith_DPY_Guess(
            D, X, Y, Ds, nD, Ts, nT, Ys, nY, OS, Xs, T_Guess, b);
      },
      [](Real D, Real X, Real Y, const Real* Ds, int nD, const Real* Ts, int nT,
         const Real* Ys, int nY, Real OS, const Real* Xs,
         const wli::EosInversionBounds& b) {
        return wli::ComputeTemperatureWith_DPY_NoGuess_NoError(
            D, X, Y, Ds, nD, Ts, nT, Ys, nY, OS, Xs, b);
      },
      [](Real D, Real X, Real Y, const Real* Ds, int nD, const Real* Ts, int nT,
         const Real* Ys, int nY, Real OS, const Real* Xs, Real T_Guess,
         const wli::EosInversionBounds& b) {
        return wli::ComputeTemperatureWith_DPY_Guess_NoError(
            D, X, Y, Ds, nD, Ts, nT, Ys, nY, OS, Xs, T_Guess, b);
      });

  // DSY family (X = entropy-per-baryon sub-table).
  run_family(
      "DSY",
      [](Real D, Real X, Real Y, const Real* Ds, int nD, const Real* Ts, int nT,
         const Real* Ys, int nY, Real OS, const Real* Xs,
         const wli::EosInversionBounds& b) {
        return wli::ComputeTemperatureWith_DSY_NoGuess(D, X, Y, Ds, nD, Ts, nT,
                                                       Ys, nY, OS, Xs, b);
      },
      [](Real D, Real X, Real Y, const Real* Ds, int nD, const Real* Ts, int nT,
         const Real* Ys, int nY, Real OS, const Real* Xs, Real T_Guess,
         const wli::EosInversionBounds& b) {
        return wli::ComputeTemperatureWith_DSY_Guess(
            D, X, Y, Ds, nD, Ts, nT, Ys, nY, OS, Xs, T_Guess, b);
      },
      [](Real D, Real X, Real Y, const Real* Ds, int nD, const Real* Ts, int nT,
         const Real* Ys, int nY, Real OS, const Real* Xs,
         const wli::EosInversionBounds& b) {
        return wli::ComputeTemperatureWith_DSY_NoGuess_NoError(
            D, X, Y, Ds, nD, Ts, nT, Ys, nY, OS, Xs, b);
      },
      [](Real D, Real X, Real Y, const Real* Ds, int nD, const Real* Ts, int nT,
         const Real* Ys, int nY, Real OS, const Real* Xs, Real T_Guess,
         const wli::EosInversionBounds& b) {
        return wli::ComputeTemperatureWith_DSY_Guess_NoError(
            D, X, Y, Ds, nD, Ts, nT, Ys, nY, OS, Xs, T_Guess, b);
      });

  if (g_failures != 0) {
    std::fprintf(stderr, "eos_inversion_family: %d check(s) failed\n",
                 g_failures);
    return EXIT_FAILURE;
  }
  std::printf(
      "PASS eos_inversion_family: DPY/DSY wrappers + _NoError reporting\n");
  return EXIT_SUCCESS;
}
