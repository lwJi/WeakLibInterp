// Deliberate-perturbation meta-test (specs/regression-suite-design.md
// Verification #3: "Pass/fail is real").
//
// Proves the suite's assertions are THRESHOLDED, not print-only: takes one
// passing synthetic cell (an affine-in-log EOS node value), injects a 10x error
// into the expected corner, and asserts the suite's own comparator (wli::is_close
// at rtol_parity) FLAGS the injected error. Exit is EXIT_SUCCESS precisely when
// the perturbation IS caught (so a print-only "check" that always passed would
// make this meta-test fail). Self-contained C++; no shell patching, no HDF5, no
// Fortran.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "wli_compare.H"
#include "wli_eos.H"
#include "wli_interp.H"
#include "wli_real.H"

namespace {
using wli::Real;
constexpr Real kA = 0.7, kB = 1.3, kC = -0.9, kE = 2.1, kOS = 3.5;
Real affine(Real D, Real T, Real Y) {
  return kA + kB * std::log10(D) + kC * std::log10(T) + kE * Y;
}
}  // namespace

int main() {
  const int nD = 4, nT = 3, nY = 3;
  Real Ds[nD] = {1.0e3, 5.0e5, 2.0e8, 6.0e11};
  Real Ts[nT] = {1.0e9, 3.0e10, 9.0e11};
  Real Ys[nY] = {0.05, 0.30, 0.55};

  std::vector<Real> table(static_cast<std::size_t>(nD) * nT * nY);
  for (int iY = 0; iY < nY; ++iY)
    for (int iT = 0; iT < nT; ++iT)
      for (int iD = 0; iD < nD; ++iD)
        table[static_cast<std::size_t>(iD) + nD * (iT + nT * iY)] =
            affine(Ds[iD], Ts[iT], Ys[iY]);

  // One passing cell: interior interpolated value vs its affine closed form.
  Real D = 7.3e6, T = 1.7e10, Y = 0.22;
  Real got = wli::EosInterpolateSingleVariable3DPoint(D, T, Y, Ds, nD, Ts, nT,
                                                      Ys, nY, kOS, table.data());
  Real want = wli::recover(affine(D, T, Y), kOS);

  int failures = 0;

  // Positive control: the un-perturbed cell must PASS its assertion (otherwise
  // the meta-test is vacuous — a comparator that rejects everything would also
  // "catch" the perturbation).
  const bool baseline_passes = wli::is_close(got, want, wli::rtol_parity);
  if (!baseline_passes) {
    std::fprintf(stderr,
                 "FAIL: baseline cell did not pass (got=%.17g want=%.17g)\n",
                 got, want);
    ++failures;
  }

  // The perturbation: inject a 10x error into the expected corner. The value is
  // comfortably nonzero, so a 10x shift is far outside every relative tier.
  const Real wrong10x = want * Real(10);
  const bool caught = !wli::is_close(got, wrong10x, wli::rtol_parity);
  if (!caught) {
    std::fprintf(stderr,
                 "FAIL: comparator did NOT flag a 10x-perturbed expected value "
                 "(got=%.17g wrong=%.17g) — assertion is not thresholded\n",
                 got, wrong10x);
    ++failures;
  }

  if (failures != 0) {
    std::fprintf(stderr, "perturbation_meta: %d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf(
      "PASS perturbation_meta: injected 10x error is caught by is_close\n");
  return EXIT_SUCCESS;
}
