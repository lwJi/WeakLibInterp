// Self-contained acceptance probe for the Pair crossing-symmetry fill
// (src/opacity/wli_opacity_nes_pair.H::PairCrossingSymmetryFillPoint).
//
// Enforces spec verification item 5 (specs/opacity-nes-pair.md:158-174, :204),
// whose oracle is the consumer relation
//   weaklib/.../wlOpacityInterpolationModule.f90:196-228 (Bruenn 1985 Eq. C64):
//     Phi0(iEp, iE) = C_i * Kernel(iE, iEp, iJii0) + C_ii * Kernel(iE, iEp, iJi0)
//     Phi1(iEp, iE) = C_i * Kernel(iE, iEp, iJii1) + C_ii * Kernel(iE, iEp, iJi1)
// The upper energy triangle (iEp > iE) is FILLED by TRANSPOSING the two energy
// indices AND EXCHANGING the in-pair / cross-pair kernel components (Ji <-> Jii).
// This is an EXACT RELABELING — no Boltzmann factor, no scaling. The leaf reads
// ONE component at the swapped kernel index (caller supplies it + matching OS);
// C_i/C_ii weighting is consumer assembly data, out of the interpolation
// contract (spec:32,:221), applied here only inside the test's oracle.
//
// Pair kernel components (weaklib wlOpacityFieldsModule.f90:25-28, 1-based):
//   iJi0=1, iJii0=2, iJi1=3, iJii1=4 -> 0-based {0,1,2,3}; swap is k ^ 1
//   ((iJi0<->iJii0), (iJi1<->iJii1)); NO moment-order (0<->1) crossing.
//
// Required checks (default parity tier 1e-12/1e-30, spec:204):
//   1. Upper-triangle Phi0/Phi1 assembled via the leaf (swapped kernels) equals
//      the direct primitive at swapped energies with swapped kernels.
//   2. Distinctness guard: the swapped upper-triangle assembly does NOT equal
//      the un-swapped lower-triangle assembly at the same (iEp, iE).
//   3. Diagonal guard: at iEp==iE the swap changes the component (leaf at
//      kernelSwapped != primitive at un-swapped kernel) — the diagonal must NOT
//      use this fill.
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

// Per-(iEp, iE, kernel) DISTINCT affine-in-log stored function: a wrong energy/
// kernel index is caught, and the transpose+component swap genuinely changes it.
Real affine(int iEp, int iE, int kernel, Real LogT, Real LogX) {
  Real const base = 0.50 + 0.07 * iEp + 0.13 * iE + 0.31 * kernel;
  Real const bT = 0.09 + 0.017 * iEp - 0.006 * kernel;
  Real const bX = -0.05 + 0.011 * iE + 0.008 * kernel;
  return base + bT * LogT + bX * LogX;
}

}  // namespace

int main() {
  // --- Synthetic 5D grid. nMom = 4 to hold all four kernel components
  // {iJi0=0, iJii0=1, iJi1=2, iJii1=3} (0-based). ---
  const int nEp = 4, nE = 4, nMom = 4, nT = 3, nEta = 4;
  const int nOpacities = 1;  // pinned Pair tables have nOpacities = 1
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

  // Synthetic per-species coupling weights (arbitrary, distinct, nonzero; real
  // weak-coupling constants are out of scope, spec:221). C_i = (cv+ca)^2,
  // C_ii = (cv-ca)^2 with cv=0.96, ca=0.50.
  const Real C_i = (0.96 + 0.50) * (0.96 + 0.50);
  const Real C_ii = (0.96 - 0.50) * (0.96 - 0.50);

  // 0-based Pair kernel component indices.
  const int iJi0 = 0, iJii0 = 1, iJi1 = 2, iJii1 = 3;

  Real LogT = 0.35, LogX = 0.1;  // interior query on both thermodynamic axes

  // The primitive at explicit energy indices + kernel (no swap done here).
  auto prim = [&](int a, int b, int kernel) {
    return wli::NESPairInterpolateSingleVariable2D2DAlignedPoint(
        LogT, LogX, LogTs, nT, LogXs, nEta, a, b, nEp, nE, kernel, nMom,
        os_of(kernel), tbl);
  };

  // The Pair crossing-symmetry leaf under test: reads ONE component at the
  // caller-supplied swapped kernel index, energies transposed internally.
  auto leaf = [&](int iEp, int iE, int kernelSwapped) {
    return wli::PairCrossingSymmetryFillPoint(
        LogT, LogX, LogTs, nT, LogXs, nEta, iEp, iE, nEp, nE, kernelSwapped,
        nMom, os_of(kernelSwapped), tbl);
  };

  // --- Check 1: upper-triangle Phi0/Phi1 via the leaf (swapped kernels) equals
  // the direct primitive at swapped energies with swapped kernels. ---
  {
    struct Pair { int iEp, iE; };
    const Pair pairs[] = {{1, 0}, {2, 0}, {3, 1}, {2, 1}, {3, 2}};
    for (const auto& p : pairs) {
      // Phi0(iEp,iE) = C_i*Kernel(iE,iEp,iJii0) + C_ii*Kernel(iE,iEp,iJi0).
      Real const phi0_leaf =
          C_i * leaf(p.iEp, p.iE, iJii0) + C_ii * leaf(p.iEp, p.iE, iJi0);
      Real const phi0_direct = C_i * prim(p.iE, p.iEp, iJii0) +
                               C_ii * prim(p.iE, p.iEp, iJi0);
      check(wli::is_close(phi0_leaf, phi0_direct, wli::rtol_parity,
                          wli::atol_default),
            "upper Phi0 = C_i*K(iE,iEp,iJii0)+C_ii*K(iE,iEp,iJi0) via leaf");

      // Phi1(iEp,iE) = C_i*Kernel(iE,iEp,iJii1) + C_ii*Kernel(iE,iEp,iJi1).
      Real const phi1_leaf =
          C_i * leaf(p.iEp, p.iE, iJii1) + C_ii * leaf(p.iEp, p.iE, iJi1);
      Real const phi1_direct = C_i * prim(p.iE, p.iEp, iJii1) +
                               C_ii * prim(p.iE, p.iEp, iJi1);
      check(wli::is_close(phi1_leaf, phi1_direct, wli::rtol_parity,
                          wli::atol_default),
            "upper Phi1 = C_i*K(iE,iEp,iJii1)+C_ii*K(iE,iEp,iJi1) via leaf");
    }
  }

  // --- Check 2: distinctness guard. The swapped upper-triangle Phi0 must NOT
  // equal the un-swapped lower-triangle assembly at the same (iEp, iE). ---
  {
    struct Pair { int iEp, iE; };
    const Pair pairs[] = {{2, 0}, {3, 1}, {3, 0}};
    for (const auto& p : pairs) {
      Real const phi0_upper =
          C_i * leaf(p.iEp, p.iE, iJii0) + C_ii * leaf(p.iEp, p.iE, iJi0);
      // Un-swapped lower-triangle assembly at the SAME (iEp, iE):
      //   Phi0 = C_i*Kernel(iEp,iE,iJi0) + C_ii*Kernel(iEp,iE,iJii0).
      Real const phi0_lower = C_i * prim(p.iEp, p.iE, iJi0) +
                              C_ii * prim(p.iEp, p.iE, iJii0);
      check(!wli::is_close(phi0_upper, phi0_lower, wli::rtol_parity,
                           wli::atol_default),
            "upper (swapped) Phi0 != un-swapped lower-triangle Phi0");
    }
  }

  // --- Check 3: diagonal guard. At iEp==iE the swap changes the component, so
  // the leaf at the swapped kernel differs from the un-swapped primitive — the
  // diagonal must NOT use this fill (Ji<->Jii swap is not a no-op there). ---
  {
    for (int d = 0; d < nE; ++d) {
      // leaf at kernelSwapped=iJii0 reads Kernel(d,d,iJii0); the un-swapped
      // lower-triangle component is Kernel(d,d,iJi0). affine() makes these
      // distinct per kernel, so the swap genuinely changes the result.
      Real const swapped = leaf(d, d, iJii0);
      Real const unswapped = prim(d, d, iJi0);
      check(!wli::is_close(swapped, unswapped, wli::rtol_parity,
                           wli::atol_default),
            "diagonal iEp==iE: swapped component != un-swapped (no fixed point)");
    }
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "pair_crossing_symmetry: %d check(s) failed\n",
                 g_failures);
    return EXIT_FAILURE;
  }
  std::printf("PASS pair_crossing_symmetry: Pair crossing-symmetry fill\n");
  return EXIT_SUCCESS;
}
