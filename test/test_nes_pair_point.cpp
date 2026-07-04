// Self-contained acceptance probe for the NES/Pair aligned 2D2D bilinear
// single-point evaluate kernel
// (src/opacity/wli_opacity_nes_pair.H::NESPairInterpolateSingleVariable2D2DAlignedPoint).
//
// Enforces the self-contained regression checks of specs/opacity-nes-pair.md
// (Verification :200-206) for the two-energy scattering kernels (NES and Pair),
// whose oracle is the aligned scalar core
//   LinearInterp2D_4DArray_2DAligned_Point (wlInterpolationUtilitiesModule.F90
//   :602-627), driven by LogInterpolateSingleVariable_2D2D_Custom_Aligned_Point.
// A SINGLE channel-neutral kernel serves both NES and Pair; this test exercises
// it directly (the caller supplies table + pre-selected scalar offset).
//
// The 5D table is (nEp, nE, nMom, nT, nEta), column-major with E' fastest. At
// fixed energy indices (iEp, iE) and kernel-component index, only the (T, eta)
// plane is bilinearly interpolated in LOG10 space; the energy and kernel axes
// are pure direct-index slices. Both T and eta arrive PRE-LOG10'd (spec:80-86),
// so queries are fed as log10(T) / log10(eta). The synthetic stored function is
// DISTINCT per (iEp, iE, kernel) triple so an index/slice/cross-talk bug surfaces.
//
// Required checks:
//   1. Affine-in-log exactness (machine tier ~1e-14) at an interior query.
//   2. Node identity (machine tier): interior + top-edge (T, eta) nodes.
//   3. Kernel-slice independence + energy-index independence (machine tier),
//      including at least one iEp > iE pair to prove NO triangle special-casing.
//   4. Boundary extrapolation (exact affine) below/above the T and eta edges.
//   5. NaN propagation on non-positive T or eta (caller's log10(<0) -> NaN).
//
// Hand-rolled harness (no GoogleTest/Catch2), synthetic 5D table only (no HDF5),
// no amrex::Initialize (pure host scalar math). Production-.h5 rtol=1e-12 parity
// is DEFERRED to the regression-suite umbrella (no HDF5 reader exists yet).

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

// Per-(iEp, iE, kernel) DISTINCT affine-in-log model in terms of the ALREADY-
// LOG10'd coordinates (LogT, LogX). A wrong energy index, a wrong kernel slice,
// or a stride bug that blends/ignores an axis is caught because every triple's
// base offset + slopes differ. Coefficients are small so the affine LOG-space
// value stays modest (~0.4..1.7) across interior + extrapolation queries,
// keeping 10**(affine) in a tier where the 1e-14 relative check holds.
Real affine(int iEp, int iE, int kernel, Real LogT, Real LogX) {
  Real const base = 0.50 + 0.07 * iEp + 0.13 * iE + 0.31 * kernel;
  Real const bT = 0.09 + 0.017 * iEp - 0.006 * kernel;
  Real const bX = -0.05 + 0.011 * iE + 0.008 * kernel;
  return base + bT * LogT + bX * LogX;
}

}  // namespace

int main() {
  // --- Synthetic 5D grid: non-uniform (uneven-ratio) LOG10'd T and eta axes. ---
  const int nEp = 4, nE = 4, nMom = 3, nT = 3, nEta = 4;
  const int nOpacities = 1;  // pinned NES/Pair tables have nOpacities = 1
  Real LogTs[nT] = {-0.4, 0.15, 0.9};        // log10 T (T in MeV)
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

  // 2D offsets Offsets[nOpacities, nMoments], column-major (species-major /
  // moment-minor). Distinct per kernel so a wrong offset element is caught.
  Real offsets[nOpacities * nMom];
  for (int k = 0; k < nMom; ++k)
    for (int s = 0; s < nOpacities; ++s)
      offsets[wli::flat_index<2>({s, k}, {nOpacities, nMom})] =
          0.5 + 0.11 * s + 0.29 * k;

  const int iSpecies = 0;  // nOpacities = 1 -> only species index available
  auto os_of = [&](int kernel) {
    return wli::IsoOffset(offsets, nOpacities, nMom, iSpecies, kernel);
  };

  // Kernel wrapper: fixed (iEp, iE, kernel) slice + its 2D offset.
  auto nespair = [&](int iEp, int iE, int kernel, Real LogT, Real LogX) {
    return wli::NESPairInterpolateSingleVariable2D2DAlignedPoint(
        LogT, LogX, LogTs, nT, LogXs, nEta, iEp, iE, nEp, nE, kernel, nMom,
        os_of(kernel), tbl);
  };

  // Expected recovered value: 10**(affine) - OS.
  auto want = [&](int iEp, int iE, int kernel, Real LogT, Real LogX) {
    return wli::recover(affine(iEp, iE, kernel, LogT, LogX), os_of(kernel));
  };

  // --- Check 1: affine-in-log exactness at an interior query ---
  {
    int iEp = 1, iE = 2, k = 1;
    Real LogT = 0.35, LogX = 0.1;  // interior on both axes
    check(wli::is_close(nespair(iEp, iE, k, LogT, LogX),
                        want(iEp, iE, k, LogT, LogX), wli::rtol_machine,
                        wli::atol_default),
          "affine-in-log exactness at interior query (~1e-14)");
  }

  // --- Check 2: node identity at interior + top-edge (T, eta) nodes ---
  {
    int iEp = 2, iE = 3, k = 2;
    // Interior node.
    int iT = 1, iX = 1;
    Real got = nespair(iEp, iE, k, LogTs[iT], LogXs[iX]);
    Real w = wli::recover(
        table[wli::flat_index<5>({iEp, iE, k, iT, iX},
                                 {nEp, nE, nMom, nT, nEta})],
        os_of(k));
    check(wli::is_close(got, w, wli::rtol_machine, wli::atol_default),
          "node identity at interior (T, eta) node (~1e-14)");
    // Top-edge node: index clamps to n-2, delta -> 1.
    int iTe = nT - 1, iXe = nEta - 1;
    Real gotE = nespair(iEp, iE, k, LogTs[iTe], LogXs[iXe]);
    Real wE = wli::recover(
        table[wli::flat_index<5>({iEp, iE, k, iTe, iXe},
                                 {nEp, nE, nMom, nT, nEta})],
        os_of(k));
    check(wli::is_close(gotE, wE, wli::rtol_machine, wli::atol_default),
          "node identity at top-edge (T, eta) node (~1e-14)");
  }

  // --- Check 3: kernel-slice + energy-index independence (incl. iEp > iE) ---
  {
    Real LogT = 0.2, LogX = -0.1;  // shared interior point
    // Each kernel slice recovers its own function at fixed (iEp, iE).
    int iEp = 1, iE = 2;
    Real g0 = nespair(iEp, iE, 0, LogT, LogX);
    Real g1 = nespair(iEp, iE, 1, LogT, LogX);
    Real g2 = nespair(iEp, iE, 2, LogT, LogX);
    check(wli::is_close(g0, want(iEp, iE, 0, LogT, LogX), wli::rtol_machine,
                        wli::atol_default),
          "kernel-slice independence: kernel=0 recovers its own function");
    check(wli::is_close(g1, want(iEp, iE, 1, LogT, LogX), wli::rtol_machine,
                        wli::atol_default),
          "kernel-slice independence: kernel=1 recovers its own function");
    check(wli::is_close(g2, want(iEp, iE, 2, LogT, LogX), wli::rtol_machine,
                        wli::atol_default),
          "kernel-slice independence: kernel=2 recovers its own function");
    // Slices must DIFFER (guards a stride bug blending/ignoring kernel).
    check(!wli::is_close(g0, g1, wli::rtol_machine, wli::atol_default) &&
              !wli::is_close(g1, g2, wli::rtol_machine, wli::atol_default),
          "kernel slices differ (kernel is a genuine strided slice)");
    // Energy-index independence: a different (iEp, iE) at the same kernel
    // recovers a DIFFERENT function.
    Real h = nespair(3, 0, 1, LogT, LogX);  // iEp=3 > iE=0 -> upper triangle
    check(wli::is_close(h, want(3, 0, 1, LogT, LogX), wli::rtol_machine,
                        wli::atol_default),
          "energy-index independence: (iEp=3, iE=0) recovers its own function");
    check(!wli::is_close(h, g1, wli::rtol_machine, wli::atol_default),
          "distinct (iEp, iE) give distinct results (no cross-talk)");
    // iEp > iE must work with NO triangle special-casing: exact affine recovery.
    int iEpU = 3, iEU = 1;  // upper triangle
    check(wli::is_close(nespair(iEpU, iEU, 2, LogT, LogX),
                        want(iEpU, iEU, 2, LogT, LogX), wli::rtol_machine,
                        wli::atol_default),
          "iEp > iE handled (no triangle guard): exact affine recovery (~1e-14)");
  }

  // --- Check 4: boundary extrapolation below/above T and eta edges ---
  {
    int iEp = 2, iE = 2, k = 1;
    Real LogT = 0.3, LogX = 0.0;  // interior baseline
    Real Tlo = LogTs[0] - 0.7, Thi = LogTs[nT - 1] + 0.7;
    check(wli::is_close(nespair(iEp, iE, k, Tlo, LogX),
                        want(iEp, iE, k, Tlo, LogX), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation below T edge (exact affine)");
    check(wli::is_close(nespair(iEp, iE, k, Thi, LogX),
                        want(iEp, iE, k, Thi, LogX), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation above T edge (exact affine)");
    Real Xlo = LogXs[0] - 0.9, Xhi = LogXs[nEta - 1] + 0.9;
    check(wli::is_close(nespair(iEp, iE, k, LogT, Xlo),
                        want(iEp, iE, k, LogT, Xlo), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation below eta edge (exact affine)");
    check(wli::is_close(nespair(iEp, iE, k, LogT, Xhi),
                        want(iEp, iE, k, LogT, Xhi), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation above eta edge (exact affine)");
  }

  // --- Check 5: NaN propagation on non-positive T or eta ---
  {
    int iEp = 1, iE = 1, k = 0;
    Real LogT = 0.3, LogX = 0.0;
    const Real nanFromNeg = std::log10(Real(-1.0));  // NaN; caller's log10(<0)
    check(std::isnan(nespair(iEp, iE, k, nanFromNeg, LogX)),
          "NaN propagation on non-positive T (log10(<0))");
    check(std::isnan(nespair(iEp, iE, k, LogT, nanFromNeg)),
          "NaN propagation on non-positive eta (log10(<0))");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "nes_pair_point: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf(
      "PASS nes_pair_point: NES/Pair aligned 2D2D bilinear evaluate kernel\n");
  return EXIT_SUCCESS;
}
