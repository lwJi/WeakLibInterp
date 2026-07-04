// Regression-suite umbrella: the always-on synthetic coverage matrix.
//
// Realizes the closure matrix of specs/regression-suite-design.md: all 8 public
// device entry points (rows) x the 4 input regimes (columns)
//   {in-bounds, on-edge, out-of-range, NaN-input}
// plus the two symmetry-fill closure cells (NES detailed balance, Pair crossing)
// that the NES/Pair rows own. Every cell prints a self-describing tag
//   [row=<name> regime=<regime> tier=<tier>] ok|FAIL
// and asserts exactly one thresholded predicate (spec:81-84): the tolerance-tier
// comparator wli::is_close for interpolated / derivative / round-trip values,
// exact == for inversion integer error codes, and std::isnan for NaN
// propagation. The aggregate exit is EXIT_FAILURE if any cell fails.
//
// Tier assignment (spec:88, brief): interpolated value -> rtol_parity (1e-12);
// derivative / inversion-T / round-trip -> rtol_relaxed (1e-10); closed-form
// node-identity / extrapolation exactness -> rtol_machine (1e-14);
// boundary index / NaN / error-code -> exact == / std::isnan.
//
// All tables are built in memory with closed-form affine-in-log / constant /
// symmetry properties (no HDF5, no external files, no Fortran). The kernels are
// pure host scalar math, so amrex::Initialize is not required (mirrors
// test/test_eos_point.cpp). Hand-rolled harness; no GoogleTest/Catch2.
//
// CRITICAL regime convention (brief, the top hazard): EOS rows 1-3 take RAW
// physical rho/T (kernel logs internally) so the NaN-input cell feeds a
// non-positive rho/T; the opacity rows 4-8 take ALREADY-LOG10'd args and never
// call log10, so their NaN-input cell must pass a literal NaN Log* argument (a
// non-positive raw value would silently extrapolate to a finite result).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "wli_compare.H"
#include "wli_eos.H"
#include "wli_eos_inversion.H"
#include "wli_interp.H"
#include "wli_opacity.H"
#include "wli_real.H"

namespace {

using wli::Real;

int g_failures = 0;

void cell(const char* tag, bool ok) {
  if (ok) {
    std::printf("[%s] ok\n", tag);
  } else {
    std::fprintf(stderr, "[%s] FAIL\n", tag);
    ++g_failures;
  }
}

const Real kQNaN = std::numeric_limits<Real>::quiet_NaN();

// --- EOS affine-in-log model (raw rho/T logged internally; Ye linear). ---
constexpr Real eA = 0.7, eB = 1.3, eC = -0.9, eE = 2.1, eOS = 3.5;
Real affineEOS(Real D, Real T, Real Y) {
  return eA + eB * std::log10(D) + eC * std::log10(T) + eE * Y;
}

// --- Opacity 4D affine model (all coords ALREADY log10 / linear). ---
constexpr Real oA = 0.4, oB = 0.9, oC = 1.1, oD = -0.7, oE = 1.7, oOS = 2.25;
Real affineOp4(Real le, Real ld, Real lt, Real y) {
  return oA + oB * le + oC * ld + oD * lt + oE * y;
}

// --- NES/Pair 2D affine model in (log10 T, log10 eta). ---
constexpr Real sA = 0.6, sB = 1.4, sC = -0.8, sOS = 1.9;
Real affine2(Real lt, Real lx) { return sA + sB * lt + sC * lx; }

// --- Brem 2D affine model in (log10 rho, log10 T). ---
constexpr Real bA = 0.5, bB = 1.2, bD = -0.6, bOS = 1.4;
Real affineB(Real ld, Real lt) { return bA + bB * ld + bD * lt; }

}  // namespace

int main() {
  // =========================================================================
  // EOS grids (RAW physical) shared by rows 1-3.
  // =========================================================================
  const int nD = 4, nT = 3, nY = 3;
  Real Ds[nD] = {1.0e3, 5.0e5, 2.0e8, 6.0e11};
  Real Ts[nT] = {1.0e9, 3.0e10, 9.0e11};
  Real Ys[nY] = {0.05, 0.30, 0.55};

  std::vector<Real> eosTbl(static_cast<std::size_t>(nD) * nT * nY);
  auto eIdx = [&](int iD, int iT, int iY) {
    return static_cast<std::size_t>(iD) + nD * (iT + nT * iY);
  };
  for (int iY = 0; iY < nY; ++iY)
    for (int iT = 0; iT < nT; ++iT)
      for (int iD = 0; iD < nD; ++iD)
        eosTbl[eIdx(iD, iT, iY)] = affineEOS(Ds[iD], Ts[iT], Ys[iY]);
  const Real* etbl = eosTbl.data();

  // -------------------------------------------------------------------------
  // Row 1: EOS single-variable EVALUATE (EosInterpolateSingleVariable3DPoint).
  // -------------------------------------------------------------------------
  auto eosEval = [&](Real D, Real T, Real Y) {
    return wli::EosInterpolateSingleVariable3DPoint(D, T, Y, Ds, nD, Ts, nT, Ys,
                                                    nY, eOS, etbl);
  };
  {
    Real D = 7.3e6, T = 1.7e10, Y = 0.22;  // strictly interior
    cell("row=EOS-evaluate regime=in-bounds tier=parity",
         wli::is_close(eosEval(D, T, Y), wli::recover(affineEOS(D, T, Y), eOS),
                       wli::rtol_parity));
  }
  {
    int iD = nD - 1, iT = nT - 1, iY = nY - 1;  // top-edge boundary node
    cell("row=EOS-evaluate regime=on-edge tier=machine",
         wli::is_close(eosEval(Ds[iD], Ts[iT], Ys[iY]),
                       wli::recover(eosTbl[eIdx(iD, iT, iY)], eOS),
                       wli::rtol_machine));
  }
  {
    Real D = 1.0e2, T = 1.7e10, Y = 0.22;  // below rho edge -> extrapolate
    cell("row=EOS-evaluate regime=out-of-range tier=machine",
         wli::is_close(eosEval(D, T, Y), wli::recover(affineEOS(D, T, Y), eOS),
                       wli::rtol_machine));
  }
  {
    Real T = 1.7e10, Y = 0.22;  // non-positive raw rho -> NaN via internal log10
    cell("row=EOS-evaluate regime=NaN-input tier=exact",
         std::isnan(eosEval(Real(0.0), T, Y)) &&
             std::isnan(eosEval(Real(-1.0), T, Y)));
  }

  // -------------------------------------------------------------------------
  // Row 2: EOS EVALUATE-AND-DIFFERENTIATE
  //        (EosInterpolateDifferentiateSingleVariable3DPoint).
  // Analytic derivative of the affine-in-log value:
  //   dValue/dRho = (value+OS) * eB / D
  //   dValue/dT   = (value+OS) * eC / T
  //   dValue/dYe  = (value+OS) * ln10 * eE
  // -------------------------------------------------------------------------
  auto eosDeriv = [&](Real D, Real T, Real Y) {
    return wli::EosInterpolateDifferentiateSingleVariable3DPoint(
        D, T, Y, Ds, nD, Ts, nT, Ys, nY, eOS, etbl);
  };
  auto derivOk = [&](Real D, Real T, Real Y, bool valueMachine) {
    auto r = eosDeriv(D, T, Y);
    Real want = wli::recover(affineEOS(D, T, Y), eOS);
    Real recon = want + eOS;
    bool vok = wli::is_close(r.value, want,
                             valueMachine ? wli::rtol_machine : wli::rtol_parity);
    bool dok = wli::is_close(r.dDrho, recon * eB / D, wli::rtol_relaxed) &&
               wli::is_close(r.dDT, recon * eC / T, wli::rtol_relaxed) &&
               wli::is_close(r.dDY, recon * wli::ln10 * eE, wli::rtol_relaxed);
    return vok && dok;
  };
  cell("row=EOS-differentiate regime=in-bounds tier=relaxed",
       derivOk(7.3e6, 1.7e10, 0.22, /*valueMachine=*/false));
  cell("row=EOS-differentiate regime=on-edge tier=relaxed",
       derivOk(Ds[1], Ts[1], Ys[1], /*valueMachine=*/true));
  cell("row=EOS-differentiate regime=out-of-range tier=relaxed",
       derivOk(1.0e2, 1.7e10, 0.22, /*valueMachine=*/false));
  {
    auto r = eosDeriv(Real(0.0), 1.7e10, 0.22);  // non-positive raw rho
    cell("row=EOS-differentiate regime=NaN-input tier=exact",
         std::isnan(r.value) && std::isnan(r.dDrho) && std::isnan(r.dDT) &&
             std::isnan(r.dDY));
  }

  // -------------------------------------------------------------------------
  // Row 3: EOS INVERSION (ComputeTemperatureWith_DEY_NoGuess + one DPY family).
  // Integer error codes via CheckInputError; round-trip recovers T; T==0 on any
  // failure. Bounds must be initialized or every in-bounds cell returns code 10.
  // -------------------------------------------------------------------------
  {
    // Global E extremes over the box corners bound every interior value.
    Real Emin = std::numeric_limits<Real>::infinity();
    Real Emax = -std::numeric_limits<Real>::infinity();
    for (int iY = 0; iY < nY; iY += nY - 1)
      for (int iT = 0; iT < nT; iT += nT - 1)
        for (int iD = 0; iD < nD; iD += nD - 1) {
          Real v = wli::recover(affineEOS(Ds[iD], Ts[iT], Ys[iY]), eOS);
          Emin = std::min(Emin, v);
          Emax = std::max(Emax, v);
        }
    wli::EosInversionBounds b;
    b.MinD = Ds[0];      b.MaxD = Ds[nD - 1];
    b.MinX = Emin;       b.MaxX = Emax;
    b.MinY = Ys[0];      b.MaxY = Ys[nY - 1];
    b.initialized = true;

    auto invDEY = [&](Real D, Real E, Real Y) {
      return wli::ComputeTemperatureWith_DEY_NoGuess(D, E, Y, Ds, nD, Ts, nT, Ys,
                                                     nY, eOS, etbl, b);
    };
    // in-bounds: interior T round-trip (code 0, relaxed).
    {
      Real D = 7.3e6, Y = 0.22, Ttarget = 1.7e10;
      Real E = eosEval(D, Ttarget, Y);
      auto res = invDEY(D, E, Y);
      cell("row=EOS-inversion-DEY regime=in-bounds tier=relaxed",
           res.Error == 0 && wli::is_close(res.T, Ttarget, wli::rtol_relaxed));
    }
    // on-edge: boundary T node round-trip (code 0, relaxed).
    {
      Real D = 7.3e6, Y = 0.22, Ttarget = Ts[0];
      Real E = eosEval(D, Ttarget, Y);
      auto res = invDEY(D, E, Y);
      cell("row=EOS-inversion-DEY regime=on-edge tier=relaxed",
           res.Error == 0 && wli::is_close(res.T, Ttarget, wli::rtol_relaxed));
    }
    // out-of-range: D/X/Y out of bounds -> codes 1/2/3, T==0 (exact).
    {
      Real D = 7.3e6, Y = 0.22;
      Real Ein = eosEval(D, 1.7e10, Y);
      auto r1 = invDEY(1.0e2, Ein, Y);            // D below MinD -> 1
      auto r2 = invDEY(D, b.MaxX * Real(10), Y);  // X above MaxX -> 2
      auto r3 = invDEY(D, Ein, Real(1.0));        // Y above MaxY -> 3
      cell("row=EOS-inversion-DEY regime=out-of-range tier=exact",
           r1.Error == 1 && r1.T == Real(0) && r2.Error == 2 &&
               r2.T == Real(0) && r3.Error == 3 && r3.T == Real(0));
    }
    // NaN-input: NaN raw input -> code 11, T==0 (exact).
    {
      auto res = invDEY(kQNaN, eosEval(7.3e6, 1.7e10, 0.22), 0.22);
      cell("row=EOS-inversion-DEY regime=NaN-input tier=exact",
           res.Error == 11 && res.T == Real(0));
    }
    // DPY family coverage: same affine sub-table serves as Ps; one round-trip.
    {
      Real D = 7.3e6, Y = 0.22, Ttarget = 4.4e10;
      Real P = eosEval(D, Ttarget, Y);
      auto res = wli::ComputeTemperatureWith_DPY_NoGuess(D, P, Y, Ds, nD, Ts, nT,
                                                         Ys, nY, eOS, etbl, b);
      cell("row=EOS-inversion-DPY regime=in-bounds tier=relaxed",
           res.Error == 0 && wli::is_close(res.T, Ttarget, wli::rtol_relaxed));
    }
  }

  // =========================================================================
  // Opacity 4D grids (ALREADY log10 for E/rho/T, raw for Ye) — rows 4-5.
  // =========================================================================
  const int nE = 3, noD = 4, noT = 3, noY = 3;
  Real LogEs[nE] = {0.0, 0.5, 1.3};
  Real LogDs[noD] = {3.0, 5.0, 8.0, 11.5};
  Real LogTs[noT] = {9.0, 10.5, 11.9};
  Real oYs[noY] = {0.05, 0.30, 0.55};

  // -------------------------------------------------------------------------
  // Row 4: EmAb EVALUATE (EmAbInterpolateSingleVariable4DPoint).
  // -------------------------------------------------------------------------
  {
    std::vector<Real> emab(static_cast<std::size_t>(nE) * noD * noT * noY);
    auto idx4 = [&](int iE, int iD, int iT, int iY) {
      return static_cast<std::size_t>(iE) + nE * (iD + noD * (iT + noT * iY));
    };
    for (int iY = 0; iY < noY; ++iY)
      for (int iT = 0; iT < noT; ++iT)
        for (int iD = 0; iD < noD; ++iD)
          for (int iE = 0; iE < nE; ++iE)
            emab[idx4(iE, iD, iT, iY)] =
                affineOp4(LogEs[iE], LogDs[iD], LogTs[iT], oYs[iY]);
    const Real* tbl = emab.data();
    auto ev = [&](Real le, Real ld, Real lt, Real y) {
      return wli::EmAbInterpolateSingleVariable4DPoint(
          le, ld, lt, y, LogEs, nE, LogDs, noD, LogTs, noT, oYs, noY, oOS, tbl);
    };
    {
      Real le = 0.7, ld = 6.0, lt = 10.0, y = 0.22;
      cell("row=EmAb-evaluate regime=in-bounds tier=parity",
           wli::is_close(ev(le, ld, lt, y),
                         wli::recover(affineOp4(le, ld, lt, y), oOS),
                         wli::rtol_parity));
    }
    {
      int iE = nE - 1, iD = noD - 1, iT = noT - 1, iY = noY - 1;
      cell("row=EmAb-evaluate regime=on-edge tier=machine",
           wli::is_close(ev(LogEs[iE], LogDs[iD], LogTs[iT], oYs[iY]),
                         wli::recover(emab[idx4(iE, iD, iT, iY)], oOS),
                         wli::rtol_machine));
    }
    {
      Real le = -1.0, ld = 6.0, lt = 10.0, y = 0.22;  // below LogE edge
      cell("row=EmAb-evaluate regime=out-of-range tier=machine",
           wli::is_close(ev(le, ld, lt, y),
                         wli::recover(affineOp4(le, ld, lt, y), oOS),
                         wli::rtol_machine));
    }
    {  // literal NaN Log argument (NOT a non-positive raw value)
      cell("row=EmAb-evaluate regime=NaN-input tier=exact",
           std::isnan(ev(kQNaN, 6.0, 10.0, 0.22)));
    }
  }

  // -------------------------------------------------------------------------
  // Row 5: Iso EVALUATE (IsoInterpolateSingleVariable5DPoint + IsoOffset).
  // 5D table (nE, nMom, nD, nT, nY); a fixed moment slice; the OS is selected
  // from a 2D offsets[nOpacities,nMoments] via IsoOffset.
  // -------------------------------------------------------------------------
  {
    const int nMom = 2, nOpac = 2;
    const int iMom = 1, iSpec = 1;
    Real offsets[nOpac * nMom];
    for (int m = 0; m < nMom; ++m)
      for (int s = 0; s < nOpac; ++s)
        offsets[s + nOpac * m] = 1.0 + 0.5 * s + 0.25 * m;  // distinct entries
    Real OS = wli::IsoOffset(offsets, nOpac, nMom, iSpec, iMom);
    cell("row=Iso-evaluate regime=offset-select tier=exact",
         OS == offsets[iSpec + nOpac * iMom]);

    // Slice bias so a wrong-moment gather would differ.
    auto biasMom = [&](int m) { return 0.31 * m; };
    std::vector<Real> iso(static_cast<std::size_t>(nE) * nMom * noD * noT * noY);
    auto idx5 = [&](int iE, int m, int iD, int iT, int iY) {
      return static_cast<std::size_t>(iE) +
             nE * (m + nMom * (iD + noD * (iT + noT * iY)));
    };
    for (int iY = 0; iY < noY; ++iY)
      for (int iT = 0; iT < noT; ++iT)
        for (int iD = 0; iD < noD; ++iD)
          for (int m = 0; m < nMom; ++m)
            for (int iE = 0; iE < nE; ++iE)
              iso[idx5(iE, m, iD, iT, iY)] =
                  affineOp4(LogEs[iE], LogDs[iD], LogTs[iT], oYs[iY]) +
                  biasMom(m);
    const Real* tbl = iso.data();
    auto ev = [&](Real le, Real ld, Real lt, Real y) {
      return wli::IsoInterpolateSingleVariable5DPoint(
          le, ld, lt, y, LogEs, nE, LogDs, noD, LogTs, noT, oYs, noY, iMom, nMom,
          OS, tbl);
    };
    auto want = [&](Real le, Real ld, Real lt, Real y) {
      return wli::recover(affineOp4(le, ld, lt, y) + biasMom(iMom), OS);
    };
    {
      Real le = 0.7, ld = 6.0, lt = 10.0, y = 0.22;
      cell("row=Iso-evaluate regime=in-bounds tier=parity",
           wli::is_close(ev(le, ld, lt, y), want(le, ld, lt, y),
                         wli::rtol_parity));
    }
    {
      int iE = 1, iD = 2, iT = 1, iY = 1;
      cell("row=Iso-evaluate regime=on-edge tier=machine",
           wli::is_close(ev(LogEs[iE], LogDs[iD], LogTs[iT], oYs[iY]),
                         wli::recover(iso[idx5(iE, iMom, iD, iT, iY)], OS),
                         wli::rtol_machine));
    }
    {
      Real le = 0.7, ld = 13.0, lt = 10.0, y = 0.22;  // above LogD edge
      cell("row=Iso-evaluate regime=out-of-range tier=machine",
           wli::is_close(ev(le, ld, lt, y), want(le, ld, lt, y),
                         wli::rtol_machine));
    }
    {
      cell("row=Iso-evaluate regime=NaN-input tier=exact",
           std::isnan(ev(0.7, kQNaN, 10.0, 0.22)));
    }
  }

  // =========================================================================
  // NES/Pair 2D2D aligned grids: (log10 T, log10 eta) bilinear — rows 6-7.
  // 5D table (nEp, nE, nMom, nT, nEta). Energy/kernel indices used directly.
  // =========================================================================
  const int nEp = 3, nEe = 3, nMomK = 4, nTk = 3, nEta = 3;
  Real LogTk[nTk] = {-0.3, 0.4, 1.0};   // log10 T (MeV)
  Real LogXk[nEta] = {-1.0, 0.0, 1.2};  // log10 eta
  auto npBias = [&](int iEp, int iE, int k) {
    return 0.13 * iEp + 0.07 * iE + 0.05 * k;
  };
  std::vector<Real> nptbl(static_cast<std::size_t>(nEp) * nEe * nMomK * nTk *
                          nEta);
  auto idxNP = [&](int iEp, int iE, int k, int iT, int iX) {
    return static_cast<std::size_t>(iEp) +
           nEp * (iE + nEe * (k + nMomK * (iT + nTk * iX)));
  };
  for (int iX = 0; iX < nEta; ++iX)
    for (int iT = 0; iT < nTk; ++iT)
      for (int k = 0; k < nMomK; ++k)
        for (int iE = 0; iE < nEe; ++iE)
          for (int iEp = 0; iEp < nEp; ++iEp)
            nptbl[idxNP(iEp, iE, k, iT, iX)] =
                affine2(LogTk[iT], LogXk[iX]) + npBias(iEp, iE, k);
  const Real* nptr = nptbl.data();
  auto npEval = [&](Real lt, Real lx, int iEp, int iE, int k) {
    return wli::NESPairInterpolateSingleVariable2D2DAlignedPoint(
        lt, lx, LogTk, nTk, LogXk, nEta, iEp, iE, nEp, nEe, k, nMomK, sOS, nptr);
  };

  // Row 6 (NES) and Row 7 (Pair): same primitive, distinct aligned cells.
  struct NPRow { const char* name; int iEp, iE, k; };
  NPRow nprows[2] = {{"NES-evaluate", 1, 2, 1}, {"Pair-evaluate", 0, 1, 2}};
  for (const auto& r : nprows) {
    auto want = [&](Real lt, Real lx) {
      return wli::recover(affine2(lt, lx) + npBias(r.iEp, r.iE, r.k), sOS);
    };
    char tag[96];
    {
      Real lt = 0.1, lx = 0.3;
      std::snprintf(tag, sizeof(tag), "row=%s regime=in-bounds tier=parity",
                    r.name);
      cell(tag, wli::is_close(npEval(lt, lx, r.iEp, r.iE, r.k), want(lt, lx),
                              wli::rtol_parity));
    }
    {
      int iT = 2, iX = 1;
      std::snprintf(tag, sizeof(tag), "row=%s regime=on-edge tier=machine",
                    r.name);
      cell(tag, wli::is_close(
                    npEval(LogTk[iT], LogXk[iX], r.iEp, r.iE, r.k),
                    wli::recover(nptbl[idxNP(r.iEp, r.iE, r.k, iT, iX)], sOS),
                    wli::rtol_machine));
    }
    {
      Real lt = -1.0, lx = 0.3;  // vary CONTINUOUS T below edge (not an index)
      std::snprintf(tag, sizeof(tag),
                    "row=%s regime=out-of-range tier=machine", r.name);
      cell(tag, wli::is_close(npEval(lt, lx, r.iEp, r.iE, r.k), want(lt, lx),
                              wli::rtol_machine));
    }
    {
      std::snprintf(tag, sizeof(tag), "row=%s regime=NaN-input tier=exact",
                    r.name);
      cell(tag, std::isnan(npEval(kQNaN, 0.3, r.iEp, r.iE, r.k)));
    }
  }

  // NES detailed-balance symmetry fill (row 6 closure invariant): the upper
  // triangle value equals the swapped lower-triangle value scaled by the
  // Boltzmann factor exp((E[iE]-E[iEp])/T).
  {
    Real E[nEe] = {2.0, 5.0, 9.0};  // strictly increasing PHYSICAL energies
    Real Tphys = 3.5;               // physical T (MeV)
    Real lt = 0.1, lx = 0.3;
    int iEp = 2, iE = 1, k = 1;  // upper triangle (iEp > iE)
    Real got = wli::NESDetailedBalanceFillPoint(lt, lx, LogTk, nTk, LogXk, nEta,
                                                iEp, iE, nEp, nEe, k, nMomK, sOS,
                                                nptr, E, Tphys);
    Real want = npEval(lt, lx, iE, iEp, k) * std::exp((E[iE] - E[iEp]) / Tphys);
    cell("row=NES-detailed-balance regime=symmetry tier=machine",
         wli::is_close(got, want, wli::rtol_machine));
  }

  // Pair crossing-symmetry fill (row 7 closure invariant): the upper triangle
  // value is the transposed-energy entry at the swapped kernel component (exact
  // relabeling, no Boltzmann factor).
  {
    Real lt = 0.1, lx = 0.3;
    int iEp = 2, iE = 1, kSwapped = 3;  // upper triangle, swapped component
    Real got = wli::PairCrossingSymmetryFillPoint(lt, lx, LogTk, nTk, LogXk,
                                                  nEta, iEp, iE, nEp, nEe,
                                                  kSwapped, nMomK, sOS, nptr);
    Real want = npEval(lt, lx, iE, iEp, kSwapped);
    cell("row=Pair-crossing-symmetry regime=symmetry tier=machine",
         wli::is_close(got, want, wli::rtol_machine));
  }

  // =========================================================================
  // Row 8: Brem SUMMED 2D2D aligned
  //        (BremInterpolateSingleVariable2D2DAlignedSummedPoint).
  // 5D table (nEp, nE, nMom, nD, nT) — CRITICAL: (rho, T) order, rho before T,
  // NOT (T, eta) like NES/Pair. Summed over nSpecies effective densities with
  // the [1,1,28/3] weight decomposition.
  // =========================================================================
  {
    const int bEp = 3, bE = 3, bMom = 1, bnD = 4, bnT = 3;
    const int iEp = 1, iE = 2, moment = 0;
    Real LogDb[bnD] = {3.0, 5.0, 8.0, 11.5};
    Real LogTb[bnT] = {9.0, 10.5, 11.9};
    Real bias = 0.1 * iEp + 0.05 * iE;
    std::vector<Real> brem(static_cast<std::size_t>(bEp) * bE * bMom * bnD *
                           bnT);
    auto idxB = [&](int ep, int e, int m, int iD, int iT) {
      return static_cast<std::size_t>(ep) +
             bEp * (e + bE * (m + bMom * (iD + bnD * iT)));
    };
    for (int iT = 0; iT < bnT; ++iT)
      for (int iD = 0; iD < bnD; ++iD)
        for (int m = 0; m < bMom; ++m)
          for (int e = 0; e < bE; ++e)
            for (int ep = 0; ep < bEp; ++ep)
              brem[idxB(ep, e, m, iD, iT)] =
                  affineB(LogDb[iD], LogTb[iT]) + 0.1 * ep + 0.05 * e;
    const Real* btbl = brem.data();

    const int nSpecies = 3;
    Real Alpha[nSpecies] = {1.0, 1.0, 28.0 / 3.0};
    auto summed = [&](Real ld0, Real ld1, Real ld2, Real lt) {
      Real LogD[nSpecies] = {ld0, ld1, ld2};
      return wli::BremInterpolateSingleVariable2D2DAlignedSummedPoint(
          LogD, Alpha, nSpecies, lt, LogDb, bnD, LogTb, bnT, iEp, iE, bEp, bE,
          moment, bMom, bOS, btbl);
    };
    auto wantSummed = [&](Real ld0, Real ld1, Real ld2, Real lt) {
      Real LogD[nSpecies] = {ld0, ld1, ld2};
      Real s = 0;
      for (int l = 0; l < nSpecies; ++l)
        s += Alpha[l] * wli::recover(affineB(LogD[l], lt) + bias, bOS);
      return s;
    };
    {
      Real a = 4.0, b = 6.0, c = 9.0, lt = 10.0;
      cell("row=Brem-summed regime=in-bounds tier=parity",
           wli::is_close(summed(a, b, c, lt), wantSummed(a, b, c, lt),
                         wli::rtol_parity));
    }
    {
      Real a = LogDb[1], b = LogDb[2], c = LogDb[3], lt = LogTb[1];  // nodes
      cell("row=Brem-summed regime=on-edge tier=machine",
           wli::is_close(summed(a, b, c, lt), wantSummed(a, b, c, lt),
                         wli::rtol_machine));
    }
    {
      Real a = 1.0, b = 6.0, c = 9.0, lt = 10.0;  // ld0 below LogD edge
      cell("row=Brem-summed regime=out-of-range tier=parity",
           wli::is_close(summed(a, b, c, lt), wantSummed(a, b, c, lt),
                         wli::rtol_parity));
    }
    {  // literal NaN Log T -> every term NaN -> sum NaN
      cell("row=Brem-summed regime=NaN-input tier=exact",
           std::isnan(summed(4.0, 6.0, 9.0, kQNaN)));
    }
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "coverage_matrix: %d cell(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf(
      "PASS coverage_matrix: 8 entry points x 4 regimes + 2 symmetry cells\n");
  return EXIT_SUCCESS;
}
