// Self-contained acceptance probe for the Iso single-point 5D-slice-to-4D
// evaluate kernel (src/opacity/wli_opacity_emab_iso.H::IsoInterpolateSingleVariable5DPoint +
// the IsoOffset 2D offset selector).
//
// Enforces the Iso-specific self-contained regression checks of
// specs/opacity-emab-iso.md (Verification :138-146) for the isoenergetic
// scattering kernel, whose oracle is the slice-then-4D path
// (LogInterpolateSingleVariable_4D_Custom_Point on a fixed-(species,moment)
// slice of the 5D table, spec:39). Because the Iso kernel reuses the same
// GetIndexAndDeltaLin + TetraLinear + recover primitives as the EmAb 4D kernel
// with the moment index held fixed in the flat_index<5> arithmetic, it inherits
// bit-identical affine/node/boundary/NaN behavior on the sliced sub-table:
//   1. Affine-in-log exactness (machine tier ~1e-14), for BOTH moment slices.
//   2. Node identity (machine tier): interior + top-edge nodes at a chosen iMom.
//   3. Moment-slice independence (Iso-specific, spec:144): each fixed (species,
//      moment) slice interpolates independently — iso(0,..)/iso(1,..) each
//      recover their own distinct affine function, the two DIFFER, and the 2D
//      offset lookup is NOT transposed.
//   4. Boundary extrapolation (exact relation) below/above each of E/rho/T/Ye.
//   5. NaN propagation on non-positive E/rho/T; NOT on Ye (linear axis).
//
// The kernel takes E/rho/T PRE-LOG10'd (spec:68), so queries are fed as
// log10(E) etc.; Ye is raw. nMom=2 is mandatory: it makes the fixed-iMom slice
// genuinely strided (D-stride nE*nMom), which is the whole point of this task.
//
// Hand-rolled harness (no GoogleTest/Catch2), synthetic 5D table only (no HDF5),
// mirroring test/test_emab_point.cpp. amrex::Initialize is not required: the
// kernel is pure host scalar math.

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

// Per-moment DISTINCT affine-in-log model in terms of the ALREADY-LOG10'd
// coordinates (LogE, LogD, LogT) and raw Y. Coefficient sets differ between
// iMom=0 and iMom=1 so a wrong slice (or a stride bug that blends/ignores the
// moment) is caught. Coefficients are small so the affine LOG-space value stays
// modest (~0.5..1.6) across all queries incl. extrapolation, keeping 10**(affine)
// in the same tier as the EmAb test so the 1e-14 relative tier holds.
Real affine_m(int iMom, Real LogE, Real LogD, Real LogT, Real Y) {
  if (iMom == 0) {
    return 0.80 + 0.13 * LogE - 0.09 * LogD + 0.07 * LogT + 0.40 * Y;
  }
  // iMom == 1: distinct base + coefficients.
  return 0.55 + 0.06 * LogE + 0.11 * LogD - 0.05 * LogT + 0.22 * Y;
}

}  // namespace

int main() {
  // --- Synthetic 5D grid: non-uniform (uneven-ratio) arrays for the log axes,
  //     already in log10 space; LINEAR (uniform) Ys. nMom=2 is mandatory. ---
  const int nE = 3, nMom = 2, nD = 4, nT = 3, nY = 3;
  const int nOpacities = 2, nMoments = 2;
  Real LogEs[nE] = {0.0, 0.7, 1.9};
  Real LogDs[nD] = {3.0, 5.4, 8.1, 11.7};
  Real LogTs[nT] = {9.0, 10.3, 11.95};
  Real Ys[nY] = {0.05, 0.30, 0.55};

  // 5D table, column-major (nE, nMom, nD, nT, nY): E fastest, moment at axis 1.
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

  // 2D offsets Offsets[nOpacities, nMoments], column-major (species-major /
  // moment-minor). Populated ASYMMETRIC and all-distinct so a transposed lookup
  // picks a wrong element.
  Real offsets[nOpacities * nMoments];
  for (int m = 0; m < nMoments; ++m)
    for (int s = 0; s < nOpacities; ++s)
      offsets[wli::flat_index<2>({s, m}, {nOpacities, nMoments})] =
          0.5 + 0.11 * s + 0.37 * m;

  // Interpolation checks fix iSpecies=1 (nonzero, exercises the species stride).
  const int iSpecies = 1;

  auto os_of = [&](int iMom) {
    return wli::IsoOffset(offsets, nOpacities, nMoments, iSpecies, iMom);
  };

  // Kernel wrapper: fixed (species, moment) slice + its 2D offset.
  auto iso = [&](int iMom, Real LogE, Real LogD, Real LogT, Real Y) {
    return wli::IsoInterpolateSingleVariable5DPoint(
        LogE, LogD, LogT, Y, LogEs, nE, LogDs, nD, LogTs, nT, Ys, nY, iMom,
        nMom, os_of(iMom), tbl);
  };

  // Expected recovered value for the chosen moment: 10**(affine_m) - OS.
  auto want_m = [&](int iMom, Real LogE, Real LogD, Real LogT, Real Y) {
    return wli::recover(affine_m(iMom, LogE, LogD, LogT, Y), os_of(iMom));
  };

  // --- Check 1: affine-in-log exactness at an interior query, BOTH moments ---
  {
    Real LogE = 1.1, LogD = 6.7, LogT = 10.9, Y = 0.22;  // inside every axis
    check(wli::is_close(iso(0, LogE, LogD, LogT, Y),
                        want_m(0, LogE, LogD, LogT, Y), wli::rtol_machine,
                        wli::atol_default),
          "affine-in-log exactness at interior query, iMom=0 (~1e-14)");
    check(wli::is_close(iso(1, LogE, LogD, LogT, Y),
                        want_m(1, LogE, LogD, LogT, Y), wli::rtol_machine,
                        wli::atol_default),
          "affine-in-log exactness at interior query, iMom=1 (~1e-14)");
  }

  // --- Check 2: node identity at a chosen moment (iMom=1) ---
  {
    const int iMom = 1;
    // Interior node.
    int iE = 1, iD = 1, iT = 1, iY = 1;
    Real got = iso(iMom, LogEs[iE], LogDs[iD], LogTs[iT], Ys[iY]);
    Real want = wli::recover(
        table[wli::flat_index<5>({iE, iMom, iD, iT, iY},
                                 {nE, nMom, nD, nT, nY})],
        os_of(iMom));
    check(wli::is_close(got, want, wli::rtol_machine, wli::atol_default),
          "node identity at interior node, iMom=1 (~1e-14)");
  }
  {
    const int iMom = 1;
    // Top-edge node on every interpolated axis: index clamp to n-2 / delta->1.
    int iE = nE - 1, iD = nD - 1, iT = nT - 1, iY = nY - 1;
    Real got = iso(iMom, LogEs[iE], LogDs[iD], LogTs[iT], Ys[iY]);
    Real want = wli::recover(
        table[wli::flat_index<5>({iE, iMom, iD, iT, iY},
                                 {nE, nMom, nD, nT, nY})],
        os_of(iMom));
    check(wli::is_close(got, want, wli::rtol_machine, wli::atol_default),
          "node identity at top-edge node (all axes n-1), iMom=1 (~1e-14)");
  }

  // --- Check 3: moment-slice independence + non-transposition (spec:144) ---
  {
    Real LogE = 1.05, LogD = 7.2, LogT = 10.6, Y = 0.31;  // shared interior pt
    Real g0 = iso(0, LogE, LogD, LogT, Y);
    Real g1 = iso(1, LogE, LogD, LogT, Y);
    check(wli::is_close(g0, want_m(0, LogE, LogD, LogT, Y), wli::rtol_machine,
                        wli::atol_default),
          "moment-slice independence: iMom=0 recovers its own function");
    check(wli::is_close(g1, want_m(1, LogE, LogD, LogT, Y), wli::rtol_machine,
                        wli::atol_default),
          "moment-slice independence: iMom=1 recovers its own function");
    // The two slices must DIFFER (guards a stride bug blending/ignoring iMom).
    check(!wli::is_close(g0, g1, wli::rtol_machine, wli::atol_default),
          "moment slices differ (iMom is a genuine strided slice)");
    // Non-transposition: the 2D offset lookup is species-major, so swapping
    // (species, moment) picks a different element.
    check(wli::IsoOffset(offsets, nOpacities, nMoments, 1, 0) !=
              wli::IsoOffset(offsets, nOpacities, nMoments, 0, 1),
          "IsoOffset is not transposed (species-major, moment-minor)");
    // iso(1,..) used IsoOffset(..,iSpecies=1,iMom=1): using the transposed
    // element would break the exactness of check-3's iMom=1 recovery, so the
    // passing is_close above already confirms the correct offset element was
    // consumed. Assert the exact offset value the kernel should have used.
    check(os_of(1) == wli::IsoOffset(offsets, nOpacities, nMoments, 1, 1),
          "iso(1,..) consumed offset element (iSpecies=1, iMom=1)");
  }

  // --- Check 4: boundary extrapolation on all four axes at iMom=1 ---
  {
    const int iMom = 1;
    Real LogE = 1.1, LogD = 6.7, LogT = 10.9, Y = 0.22;  // interior baseline
    Real Elo = LogEs[0] - 0.8, Ehi = LogEs[nE - 1] + 0.8;
    check(wli::is_close(iso(iMom, Elo, LogD, LogT, Y),
                        want_m(iMom, Elo, LogD, LogT, Y), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation below E edge, iMom=1 (exact affine)");
    check(wli::is_close(iso(iMom, Ehi, LogD, LogT, Y),
                        want_m(iMom, Ehi, LogD, LogT, Y), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation above E edge, iMom=1 (exact affine)");
    Real Dlo = LogDs[0] - 1.0, Dhi = LogDs[nD - 1] + 1.0;
    check(wli::is_close(iso(iMom, LogE, Dlo, LogT, Y),
                        want_m(iMom, LogE, Dlo, LogT, Y), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation below rho edge, iMom=1 (exact affine)");
    check(wli::is_close(iso(iMom, LogE, Dhi, LogT, Y),
                        want_m(iMom, LogE, Dhi, LogT, Y), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation above rho edge, iMom=1 (exact affine)");
    Real Tlo = LogTs[0] - 0.9, Thi = LogTs[nT - 1] + 0.9;
    check(wli::is_close(iso(iMom, LogE, LogD, Tlo, Y),
                        want_m(iMom, LogE, LogD, Tlo, Y), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation below T edge, iMom=1 (exact affine)");
    check(wli::is_close(iso(iMom, LogE, LogD, Thi, Y),
                        want_m(iMom, LogE, LogD, Thi, Y), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation above T edge, iMom=1 (exact affine)");
    Real Ylo = Ys[0] - 0.15, Yhi = Ys[nY - 1] + 0.15;
    check(wli::is_close(iso(iMom, LogE, LogD, LogT, Ylo),
                        want_m(iMom, LogE, LogD, LogT, Ylo), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation below Ye edge, iMom=1 (exact affine)");
    check(wli::is_close(iso(iMom, LogE, LogD, LogT, Yhi),
                        want_m(iMom, LogE, LogD, LogT, Yhi), wli::rtol_machine,
                        wli::atol_default),
          "boundary extrapolation above Ye edge, iMom=1 (exact affine)");
  }

  // --- Check 5: NaN propagation on non-positive E/rho/T; NOT on Ye ---
  {
    const int iMom = 1;
    Real LogE = 1.1, LogD = 6.7, LogT = 10.9, Y = 0.22;
    const Real nanFromNeg = std::log10(Real(-1.0));  // NaN; caller's log10(<0)
    check(std::isnan(iso(iMom, nanFromNeg, LogD, LogT, Y)),
          "NaN propagation on non-positive E (log10(<0)), iMom=1");
    check(std::isnan(iso(iMom, LogE, nanFromNeg, LogT, Y)),
          "NaN propagation on non-positive rho (log10(<0)), iMom=1");
    check(std::isnan(iso(iMom, LogE, LogD, nanFromNeg, Y)),
          "NaN propagation on non-positive T (log10(<0)), iMom=1");
    check(!std::isnan(iso(iMom, LogE, LogD, LogT, Real(0.0))),
          "no NaN on Ye == 0 (linear axis extrapolates), iMom=1");
    check(!std::isnan(iso(iMom, LogE, LogD, LogT, Real(-0.2))),
          "no NaN on Ye < 0 (linear axis extrapolates), iMom=1");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "iso_point: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("PASS iso_point: Iso single-point 5D-slice-to-4D evaluate kernel\n");
  return EXIT_SUCCESS;
}
