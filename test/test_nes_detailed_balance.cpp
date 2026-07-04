// Self-contained acceptance probe for the NES detailed-balance symmetry fill
// (src/lib/wli_opacity.H::NESDetailedBalanceFillPoint).
//
// Enforces spec verification item 4 (specs/opacity-nes-pair.md:148-156, :203),
// whose oracle is the consumer relation
//   weaklib/.../wlOpacityInterpolationModule.f90:104-115:
//     Phi(iEp, iE) = Phi(iE, iEp) * exp( ( E(iE) - E(iEp) ) / T )   for iEp > iE
// The upper energy triangle (iEp > iE) is FILLED from the symmetric lower-
// triangle value (read by the aligned primitive at SWAPPED energy indices) times
// the Boltzmann factor exp((E(iE)-E(iEp))/T). E and T are PHYSICAL (MeV), NOT
// log-space — distinct plumbing from the primitive's LogT/LogTs.
//
// Required checks (default parity tier 1e-12/1e-30, spec:156, :203):
//   1. Fill equals lower-triangle value * exp((E[iE]-E[iEp])/T) for iEp > iE.
//   2. Sign/index-order guard: fill does NOT equal the reciprocal
//      lower * exp((E[iEp]-E[iE])/T) (catches a swapped subtraction).
//   3. Diagonal iEp == iE is the fixed point (factor = 1): fill equals the
//      primitive value at the same indices.
//
// Hand-rolled harness (no GoogleTest/Catch2), synthetic 5D table only (no HDF5),
// no amrex::Initialize (pure host scalar math). Production-.h5 parity is DEFERRED
// (no HDF5 reader exists yet).

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

// Per-(iEp, iE, kernel) DISTINCT affine-in-log stored function (mirrors the
// sibling test_nes_pair_point.cpp): a wrong energy/kernel index is caught.
Real affine(int iEp, int iE, int kernel, Real LogT, Real LogX) {
  Real const base = 0.50 + 0.07 * iEp + 0.13 * iE + 0.31 * kernel;
  Real const bT = 0.09 + 0.017 * iEp - 0.006 * kernel;
  Real const bX = -0.05 + 0.011 * iE + 0.008 * kernel;
  return base + bT * LogT + bX * LogX;
}

}  // namespace

int main() {
  // --- Synthetic 5D grid (same geometry as the sibling primitive test). ---
  const int nEp = 4, nE = 4, nMom = 3, nT = 3, nEta = 4;
  const int nOpacities = 1;  // pinned NES tables have nOpacities = 1
  Real LogTs[nT] = {-0.4, 0.15, 0.9};          // log10 T (T in MeV)
  Real LogXs[nEta] = {-1.0, -0.3, 0.5, 1.4};   // log10 eta

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

  // 2D offsets Offsets[nOpacities, nMoments], column-major (species-major).
  Real offsets[nOpacities * nMom];
  for (int k = 0; k < nMom; ++k)
    for (int s = 0; s < nOpacities; ++s)
      offsets[wli::flat_index<2>({s, k}, {nOpacities, nMom})] =
          0.5 + 0.11 * s + 0.29 * k;

  const int iSpecies = 0;
  auto os_of = [&](int kernel) {
    return wli::IsoOffset(offsets, nOpacities, nMom, iSpecies, kernel);
  };

  // Physical energy grid (MeV), STRICTLY increasing, and physical T (MeV),
  // chosen so factors differ noticeably across pairs. E is 0-based (C++).
  Real E[nE] = {2.0, 8.0, 20.0, 45.0};
  const Real T = 5.0;

  // The detailed-balance leaf under test: fills the upper triangle at (iEp, iE).
  auto fill = [&](int iEp, int iE, int kernel, Real LogT, Real LogX) {
    return wli::NESDetailedBalanceFillPoint(
        LogT, LogX, LogTs, nT, LogXs, nEta, iEp, iE, nEp, nE, kernel, nMom,
        os_of(kernel), tbl, E, T);
  };

  // The symmetric lower-triangle value Phi(iE, iEp): primitive at SWAPPED args.
  auto lower_swapped = [&](int iEp, int iE, int kernel, Real LogT, Real LogX) {
    return wli::NESPairInterpolateSingleVariable2D2DAlignedPoint(
        LogT, LogX, LogTs, nT, LogXs, nEta, iE, iEp, nEp, nE, kernel, nMom,
        os_of(kernel), tbl);
  };

  Real LogT = 0.35, LogX = 0.1;  // interior query on both thermodynamic axes

  // --- Check 1: fill = lower * exp((E[iE]-E[iEp])/T) for several iEp > iE ---
  {
    struct Pair { int iEp, iE, k; };
    const Pair pairs[] = {{1, 0, 0}, {2, 0, 1}, {3, 1, 2}, {2, 1, 0}, {3, 2, 1}};
    for (const auto& p : pairs) {
      Real const lower = lower_swapped(p.iEp, p.iE, p.k, LogT, LogX);
      Real const expected = lower * std::exp((E[p.iE] - E[p.iEp]) / T);
      Real const got = fill(p.iEp, p.iE, p.k, LogT, LogX);
      check(wli::is_close(got, expected, wli::rtol_parity, wli::atol_default),
            "upper-triangle fill = lower * exp((E[iE]-E[iEp])/T) for iEp>iE");
      // Boltzmann factor must be < 1 (upper triangle Boltzmann-suppressed).
      check(std::exp((E[p.iE] - E[p.iEp]) / T) < 1.0,
            "Boltzmann factor < 1 for iEp > iE (E increasing with index)");
    }
  }

  // --- Check 2: sign/index-order guard against the reciprocal ---
  {
    int iEp = 3, iE = 0, k = 1;  // large energy gap -> factor far from 1
    Real const lower = lower_swapped(iEp, iE, k, LogT, LogX);
    Real const reciprocal = lower * std::exp((E[iEp] - E[iE]) / T);
    Real const got = fill(iEp, iE, k, LogT, LogX);
    // The correct fill and the reciprocal differ by far more than rtol_parity.
    check(!wli::is_close(got, reciprocal, wli::rtol_parity, wli::atol_default),
          "fill != reciprocal lower*exp((E[iEp]-E[iE])/T) (swapped-subtraction guard)");
  }

  // --- Check 3: diagonal iEp == iE is the fixed point (factor = 1) ---
  {
    for (int d = 0; d < nE; ++d) {
      int k = d % nMom;
      Real const prim = wli::NESPairInterpolateSingleVariable2D2DAlignedPoint(
          LogT, LogX, LogTs, nT, LogXs, nEta, d, d, nEp, nE, k, nMom, os_of(k),
          tbl);
      Real const got = fill(d, d, k, LogT, LogX);
      check(wli::is_close(got, prim, wli::rtol_parity, wli::atol_default),
            "diagonal iEp==iE fixed point: fill = primitive value (factor 1)");
    }
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "nes_detailed_balance: %d check(s) failed\n",
                 g_failures);
    return EXIT_FAILURE;
  }
  std::printf("PASS nes_detailed_balance: NES detailed-balance symmetry fill\n");
  return EXIT_SUCCESS;
}
