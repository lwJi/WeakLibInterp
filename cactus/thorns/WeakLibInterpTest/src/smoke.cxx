// smoke.cxx — in-Cactus acceptance smoke for WeakLibInterp
// (specs/cactus-integration.md §79, verification item 5).
//
// Compiles the umbrella headers with the consumer's own compiler (spec:67) and
// calls one _Point per kernel family on tiny stack-resident synthetic tables:
//   EOS evaluate + differentiate   (wli_eos.H)
//   EOS inversion                  (wli_eos_inversion.H)
//   EmAb 4D                        (wli_opacity.H -> emab_iso)
//   NES/Pair 2D-aligned + deriv    (wli_opacity.H -> nes_pair)
//   Brem summed + deriv            (wli_opacity.H -> brem)
//
// Every table stores the constant 1.0 with offset OS = 0, so each multilinear
// interpolation must recover exactly 10**1 - 0 = 10 (the machine-precision
// exactness tier, constant-table invariant); the inversion uses a T-monotone
// column and must return error code 0 with a positive T. This is a smoke, not
// the regression suite — the ctest suite remains the sole correctness gate.

#include <cctk.h>

#include <cmath>

#include "wli_eos.H"
#include "wli_eos_inversion.H"
#include "wli_opacity.H"

namespace {

constexpr wli::Real kExpect = 10.0;  // 10**(stored 1.0) - (OS 0)
constexpr wli::Real kTol = 1e-12;    // machine-precision exactness tier

void check(const char* label, wli::Real got, wli::Real expect) {
  if (std::abs(got - expect) <= kTol * std::abs(expect)) {
    CCTK_VInfo(CCTK_THORNSTRING, "%-28s ok (%.15g)", label, got);
  } else {
    CCTK_VError(__LINE__, __FILE__, CCTK_THORNSTRING,
                "%s: got %.17g, expected %.17g", label, got, expect);
  }
}

}  // namespace

extern "C" int WeakLibInterpTest_Smoke(void) {
  using wli::Real;

  // Shared axes: 2 nodes per axis. Log axes carry {1,10} (raw) or {0,1}
  // (pre-LOG10'd, the opacity convention); the linear axis (Ye) carries
  // {0.1,0.2}. Queries sit mid-cell.
  Real const Ds[2] = {1.0, 10.0}, Ts[2] = {1.0, 10.0}, Ys[2] = {0.1, 0.2};
  Real const LogEs[2] = {0.0, 1.0}, LogDs[2] = {0.0, 1.0},
             LogTs[2] = {0.0, 1.0}, LogXs[2] = {0.0, 1.0};
  Real const OS = 0.0;

  Real const t3d[8] = {1, 1, 1, 1, 1, 1, 1, 1};                    // (D,T,Y)
  Real t4d[16], t5d[16];                                           // 4D / 5D
  for (int i = 0; i < 16; ++i) t4d[i] = t5d[i] = 1.0;

  // EOS evaluate + differentiate (family: eos-interpolation).
  check("EOS evaluate",
        wli::EosInterpolateSingleVariable3DPoint(3.0, 3.0, 0.15, Ds, 2, Ts, 2,
                                                 Ys, 2, OS, t3d),
        kExpect);
  check("EOS differentiate (.value)",
        wli::EosInterpolateDifferentiateSingleVariable3DPoint(
            3.0, 3.0, 0.15, Ds, 2, Ts, 2, Ys, 2, OS, t3d)
            .value,
        kExpect);

  // EOS inversion (family: eos-inversion): X stored log10(X) rises 0 -> 1
  // along T at every (D,Y), so X = sqrt(10) must invert with error 0.
  {
    Real const xs[8] = {0, 0, 1, 1, 0, 0, 1, 1};  // (nD,nT,nY) column-major
    Real T = 0.0;
    int const err = wli::ComputeTemperatureWith_DXY_NoGuess(
        3.0, std::sqrt(10.0), 0.15, Ds, 2, Ts, 2, Ys, 2, OS, xs, T);
    if (err == 0 && T > 0.0) {
      CCTK_VInfo(CCTK_THORNSTRING, "%-28s ok (T=%.15g)", "EOS inversion", T);
    } else {
      CCTK_VError(__LINE__, __FILE__, CCTK_THORNSTRING,
                  "EOS inversion: error=%d (%s), T=%.17g", err,
                  wli::DescribeEOSInversionError(err), T);
    }
  }

  // EmAb 4D (family: opacity-emab-iso; the Iso channel shares this kernel).
  check("EmAb 4D evaluate",
        wli::EmAbInterpolateSingleVariable4DPoint(0.5, 0.5, 0.5, 0.15, LogEs, 2,
                                                  LogDs, 2, LogTs, 2, Ys, 2, OS,
                                                  t4d),
        kExpect);

  // NES/Pair 2D-aligned evaluate + differentiate (families: opacity-nes-pair,
  // opacity-differentiate). Table (nEp,nE,nMom,nT,nEta) = (2,2,1,2,2).
  check("NES/Pair 2D-aligned",
        wli::NESPairInterpolateSingleVariable2D2DAlignedPoint(
            0.5, 0.5, LogTs, 2, LogXs, 2, 0, 0, 2, 2, 0, 1, OS, t5d),
        kExpect);
  check("NES/Pair differentiate",
        wli::NESPairInterpolateDifferentiateSingleVariable2D2DAlignedPoint(
            0.5, 0.5, LogTs, 2, LogXs, 2, 0, 0, 2, 2, 0, 1, OS, t5d)
            .value,
        kExpect);

  // Brem summed evaluate + differentiate (families: opacity-brem,
  // opacity-differentiate). Table (nEp,nE,nMom,nD,nT) = (2,2,1,2,2); one
  // effective density with unit weight.
  {
    Real const logd[1] = {0.5}, alpha[1] = {1.0};
    check("Brem summed",
          wli::BremInterpolateSingleVariable2D2DAlignedSummedPoint(
              logd, alpha, 1, 0.5, LogDs, 2, LogTs, 2, 0, 0, 2, 2, 0, 1, OS,
              t5d),
          kExpect);
    check("Brem summed differentiate",
          wli::BremInterpolateSingleVariable2D2DAlignedSummedDifferentiatePoint(
              logd, alpha, 1, 0.5, LogDs, 2, LogTs, 2, 0, 0, 2, 2, 0, 1, OS,
              t5d)
              .value,
          kExpect);
  }

  CCTK_INFO("WeakLibInterp smoke: all kernel families passed");
  return 0;
}
