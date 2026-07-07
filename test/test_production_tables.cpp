// Production-table loader/guard cell (specs/regression-suite-design.md:67,113;
// brief). Resolves the 6 named production tables from
// specs/fixtures/tables.provenance under $WL_TABLES_ROOT, and — for each table
// that is actually present — runs node-identity / boundary / NaN cells through
// the PUBLIC readers + the same _Point kernels the synthetic suite exercises.
//
// SKIP discipline (CTest SKIP_RETURN_CODE 77): if $WL_TABLES_ROOT is unset or
// ZERO of the 6 tables are found, this test returns 77 so CTest reports it as
// Skipped (distinct from Failed) rather than passing vacuously. Absent tables in
// an otherwise-present set print a distinct "SKIPPED <basename>" line. No live
// .h5 exists in this sandbox, so only the skip branch is exercised here — that is
// expected, not a failure.
//
// Presence is probed with std::ifstream BEFORE any reader call, because the
// readers throw H5::FileIException / std::runtime_error on an absent/malformed
// file rather than returning a status; the real read is additionally wrapped in
// try/catch as a backstop. Path join mirrors the validator
// (specs/tools/validate_specs.sh): $WL_TABLES_ROOT/use_for_production/<base>
// first, then $WL_TABLES_ROOT/<base>. No Fortran/Matlab at test time.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <AMReX.H>

#include "wli_compare.H"
#include "wli_eos.H"
#include "wli_eos_inversion.H"
#include "wli_io_eos.H"
#include "wli_io_opacity.H"
#include "wli_opacity.H"
#include "wli_real.H"

namespace {

using wli::Real;

int g_failures = 0;

void check(bool ok, const std::string& msg) {
  if (ok) {
    std::printf("  ok: %s\n", msg.c_str());
  } else {
    std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    ++g_failures;
  }
}

// The 6 pinned production tables (basenames from tables.provenance).
const char* kEosBase = "wl-EOS-SFHo-15-25-50.h5";
const char* kEmAbBase = "wl-Op-SFHo-15-25-50-E40-EmAb.h5";
const char* kIsoBase = "wl-Op-SFHo-15-25-50-E40-Iso.h5";
const char* kNesBase = "wl-Op-SFHo-15-25-50-E40-NES.h5";
const char* kPairBase = "wl-Op-SFHo-15-25-50-E40-Pair.h5";
const char* kBremBase = "wl-Op-SFHo-15-25-50-E40-Brem.h5";

bool file_exists(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return f.good();
}

// Resolve <root>/use_for_production/<base> then <root>/<base>; "" if neither.
std::string resolve(const std::string& root, const char* base) {
  std::string a = root + "/use_for_production/" + base;
  if (file_exists(a)) return a;
  std::string b = root + "/" + base;
  if (file_exists(b)) return b;
  return std::string();
}

// -------- Per-channel cell runners (only invoked when the file is present). ---

void run_eos(const std::string& path) {
  wli::io::HostEosTable t = wli::io::read_eos_table(path);
  // Schema conformance against the reader's own expectations.
  check(t.nVariables == wli::io::schema::kNVariables,
        "EOS nVariables == schema::kNVariables");
  check(t.nPoints[0] == wli::io::schema::kAxisExtent[0] &&
            t.nPoints[1] == wli::io::schema::kAxisExtent[1] &&
            t.nPoints[2] == wli::io::schema::kAxisExtent[2],
        "EOS nPoints == schema::kAxisExtent (nRho,nT,nYe)");

  const int nD = t.nPoints[0], nT = t.nPoints[1], nY = t.nPoints[2];
  const Real* Ds = t.axes[0].points.data();
  const Real* Ts = t.axes[1].points.data();
  const Real* Ys = t.axes[2].points.data();
  int slot = t.dvIndices.iPressure >= 0 ? t.dvIndices.iPressure : 0;
  const wli::io::HostDV& dv = t.dv[static_cast<std::size_t>(slot)];
  const Real* tbl = dv.values.data();
  const Real OS = dv.offset;

  // Node identity: querying at a grid node returns 10**(stored) - offset.
  int iD = nD / 2, iT = nT / 2, iY = nY / 2;
  Real D = Ds[iD], T = Ts[iT], Y = Ys[iY];
  Real got = wli::EosInterpolateSingleVariable3DPoint(D, T, Y, Ds, nD, Ts, nT,
                                                      Ys, nY, OS, tbl);
  Real want = wli::recover(
      tbl[static_cast<std::size_t>(iD) + nD * (iT + nT * iY)], OS);
  check(wli::is_close(got, want, wli::rtol_relaxed),
        "EOS node identity at an interior grid node");

  // Boundary: below the rho edge extrapolates (finite, not clamped, no error).
  Real below = Ds[0] * Real(0.5);
  check(std::isfinite(wli::EosInterpolateSingleVariable3DPoint(
            below, T, Y, Ds, nD, Ts, nT, Ys, nY, OS, tbl)),
        "EOS below-edge rho extrapolates to a finite value");

  // NaN propagation: non-positive raw rho makes the internal log10 NaN.
  check(std::isnan(wli::EosInterpolateSingleVariable3DPoint(
            Real(0.0), T, Y, Ds, nD, Ts, nT, Ys, nY, OS, tbl)),
        "EOS NaN propagation on non-positive rho");

  // ---- EOS inversion round-trip: DEY/DPY/DSY x NoGuess/Guess (6 cells) ------
  // For each dependent-variable family, forward-evaluate X at an interior
  // off-node (D, Tstar, Y), invert back to T, and assert Error==0, T==Tstar,
  // and forward(D,T,Y)==X (all at the relaxed 1e-10 tol pinned by
  // specs/eos-inversion.md:146-148). Bounds are built robustly (initialized set,
  // MinX/MaxX from a recover()-scan of the DV buffer) so the code-10 trap cannot
  // pass silently.
  using NoGuessFn = wli::EosInversionResult (*)(
      Real, Real, Real, const Real*, int, const Real*, int, const Real*, int,
      Real, const Real*, const wli::EosInversionBounds&);
  using GuessFn = wli::EosInversionResult (*)(
      Real, Real, Real, const Real*, int, const Real*, int, const Real*, int,
      Real, const Real*, Real, const wli::EosInversionBounds&);

  // Interior off-node query: log-midpoint in D and T, linear midpoint in Y.
  const Real Dq = std::sqrt(Ds[iD] * Ds[iD + 1]);
  const Real Tstar = std::sqrt(Ts[iT] * Ts[iT + 1]);
  const Real Yq = Real(0.5) * (Ys[iY] + Ys[iY + 1]);

  auto run_family = [&](const char* fam, int fslot, NoGuessFn ng, GuessFn g) {
    if (fslot < 0) {
      check(false, std::string("inversion: missing DV slot for ") + fam);
      return;
    }
    const wli::io::HostDV& fdv = t.dv[static_cast<std::size_t>(fslot)];
    const Real* ftbl = fdv.values.data();
    const Real fOS = fdv.offset;

    wli::EosInversionBounds b;
    b.MinD = Ds[0];
    b.MaxD = Ds[nD - 1];
    b.MinY = Ys[0];
    b.MaxY = Ys[nY - 1];
    Real lo = wli::recover(ftbl[0], fOS), hi = lo;
    for (std::size_t k = 0; k < fdv.values.size(); ++k) {
      Real v = wli::recover(ftbl[k], fOS);
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    b.MinX = lo;
    b.MaxX = hi;
    b.initialized = true;  // else CheckInputError returns vacuous code 10

    const Real X = wli::EosInterpolateSingleVariable3DPoint(
        Dq, Tstar, Yq, Ds, nD, Ts, nT, Ys, nY, fOS, ftbl);

    auto rn = ng(Dq, X, Yq, Ds, nD, Ts, nT, Ys, nY, fOS, ftbl, b);
    Real Xn = wli::EosInterpolateSingleVariable3DPoint(
        Dq, rn.T, Yq, Ds, nD, Ts, nT, Ys, nY, fOS, ftbl);
    check(rn.Error == 0 && wli::is_close(rn.T, Tstar, wli::rtol_relaxed) &&
              wli::is_close(Xn, X, wli::rtol_relaxed),
          std::string("inversion round-trip ") + fam + " NoGuess");

    auto rg = g(Dq, X, Yq, Ds, nD, Ts, nT, Ys, nY, fOS, ftbl, Tstar, b);
    Real Xg = wli::EosInterpolateSingleVariable3DPoint(
        Dq, rg.T, Yq, Ds, nD, Ts, nT, Ys, nY, fOS, ftbl);
    check(rg.Error == 0 && wli::is_close(rg.T, Tstar, wli::rtol_relaxed) &&
              wli::is_close(Xg, X, wli::rtol_relaxed),
          std::string("inversion round-trip ") + fam + " Guess");
  };

  run_family("DEY", t.dvIndices.iInternalEnergyDensity,
             &wli::ComputeTemperatureWith_DEY_NoGuess,
             &wli::ComputeTemperatureWith_DEY_Guess);
  run_family("DPY", t.dvIndices.iPressure,
             &wli::ComputeTemperatureWith_DPY_NoGuess,
             &wli::ComputeTemperatureWith_DPY_Guess);
  run_family("DSY", t.dvIndices.iEntropyPerBaryon,
             &wli::ComputeTemperatureWith_DSY_NoGuess,
             &wli::ComputeTemperatureWith_DSY_Guess);
}

void run_emab(const std::string& path) {
  wli::io::HostEmAbTable t = wli::io::read_emab_table(path);
  check(t.nOpacities == wli::io::schema::emab::kNOpacities,
        "EmAb nOpacities == schema::emab::kNOpacities");
  check(t.nPoints[0] == wli::io::schema::kNEnergy,
        "EmAb nPoints[0] == schema::kNEnergy");

  // Node identity through the 4D kernel: build the log-space grids the kernel
  // expects (E/rho/T already-log10, Ye raw).
  const int nE = t.nPoints[0], nD = t.nPoints[1], nT = t.nPoints[2],
            nY = t.nPoints[3];
  std::vector<Real> LogEs(nE), LogDs(nD), LogTs(nT), Ys(nY);
  for (int i = 0; i < nE; ++i) LogEs[i] = std::log10(t.common.energyGrid.points[i]);
  for (int i = 0; i < nD; ++i) LogDs[i] = std::log10(t.common.axes[0].points[i]);
  for (int i = 0; i < nT; ++i) LogTs[i] = std::log10(t.common.axes[1].points[i]);
  for (int i = 0; i < nY; ++i) Ys[i] = t.common.axes[2].points[i];

  const Real* tbl = t.values[0].data();
  const Real OS = t.offset[0];
  int iE = nE / 2, iD = nD / 2, iT = nT / 2, iY = nY / 2;
  Real got = wli::EmAbInterpolateSingleVariable4DPoint(
      LogEs[iE], LogDs[iD], LogTs[iT], Ys[iY], LogEs.data(), nE, LogDs.data(),
      nD, LogTs.data(), nT, Ys.data(), nY, OS, tbl);
  Real want = wli::recover(
      tbl[static_cast<std::size_t>(iE) + nE * (iD + nD * (iT + nT * iY))], OS);
  check(wli::is_close(got, want, wli::rtol_relaxed),
        "EmAb node identity at an interior grid node");

  // NaN propagation: a literal NaN Log argument (the opacity convention).
  check(std::isnan(wli::EmAbInterpolateSingleVariable4DPoint(
            std::nan(""), LogDs[iD], LogTs[iT], Ys[iY], LogEs.data(), nE,
            LogDs.data(), nD, LogTs.data(), nT, Ys.data(), nY, OS, tbl)),
        "EmAb NaN propagation on a NaN LogE argument");

  // FD cross-check of the evaluate-and-differentiate kernel against a 4th-order
  // central difference of the value-only leaf. Axes are pre-log10'd, so perturb
  // the PHYSICAL coordinate X*(1±h) and recompute LogX = log10(X) (∂value/∂X,
  // not ∂value/∂LogX). E/rho/T are LOG axes; Ye is the one LINEAR axis (perturbed
  // directly). Query at an interior cell CENTER to stay comfortably inside one
  // bracket cell, away from the node kinks of the piecewise-multilinear interp.
  {
    auto evalP = [&](Real E, Real D, Real T, Real Y) {
      return wli::EmAbInterpolateSingleVariable4DPoint(
          std::log10(E), std::log10(D), std::log10(T), Y, LogEs.data(), nE,
          LogDs.data(), nD, LogTs.data(), nT, Ys.data(), nY, OS, tbl);
    };
    auto fd = [](Real vm2, Real vm1, Real vp1, Real vp2, Real h) {
      return (-vp2 + 8.0 * vp1 - 8.0 * vm1 + vm2) / (12.0 * h);
    };
    Real E = std::pow(Real(10), Real(0.5) * (LogEs[iE] + LogEs[iE + 1]));
    Real D = std::pow(Real(10), Real(0.5) * (LogDs[iD] + LogDs[iD + 1]));
    Real T = std::pow(Real(10), Real(0.5) * (LogTs[iT] + LogTs[iT + 1]));
    Real Y = Real(0.5) * (Ys[iY] + Ys[iY + 1]);
    wli::EmAbPointDeriv d =
        wli::EmAbInterpolateDifferentiateSingleVariable4DPoint(
            std::log10(E), std::log10(D), std::log10(T), Y, LogEs.data(), nE,
            LogDs.data(), nD, LogTs.data(), nT, Ys.data(), nY, OS, tbl);
    Real hE = 1.0e-3 * E, hD = 1.0e-3 * D, hT = 1.0e-3 * T, hY = 1.0e-3 * Y;
    Real fdE = fd(evalP(E - 2 * hE, D, T, Y), evalP(E - hE, D, T, Y),
                  evalP(E + hE, D, T, Y), evalP(E + 2 * hE, D, T, Y), hE);
    Real fdD = fd(evalP(E, D - 2 * hD, T, Y), evalP(E, D - hD, T, Y),
                  evalP(E, D + hD, T, Y), evalP(E, D + 2 * hD, T, Y), hD);
    Real fdT = fd(evalP(E, D, T - 2 * hT, Y), evalP(E, D, T - hT, Y),
                  evalP(E, D, T + hT, Y), evalP(E, D, T + 2 * hT, Y), hT);
    Real fdY = fd(evalP(E, D, T, Y - 2 * hY), evalP(E, D, T, Y - hY),
                  evalP(E, D, T, Y + hY), evalP(E, D, T, Y + 2 * hY), hY);
    check(wli::is_close(d.dDE, fdE, wli::rtol_relaxed, wli::atol_default),
          "EmAb FD cross-check ∂value/∂E (1e-10)");
    check(wli::is_close(d.dDrho, fdD, wli::rtol_relaxed, wli::atol_default),
          "EmAb FD cross-check ∂value/∂rho (1e-10)");
    check(wli::is_close(d.dDT, fdT, wli::rtol_relaxed, wli::atol_default),
          "EmAb FD cross-check ∂value/∂T (1e-10)");
    check(wli::is_close(d.dDY, fdY, wli::rtol_relaxed, wli::atol_default),
          "EmAb FD cross-check ∂value/∂Ye (1e-10)");
  }
}

void run_iso(const std::string& path) {
  wli::io::HostScatIsoTable t = wli::io::read_scat_iso_table(path);
  check(t.nOpacities == wli::io::schema::iso::kNOpacities &&
            t.nMoments == wli::io::schema::iso::kNMoments,
        "Iso nOpacities/nMoments == schema::iso");
  check(t.nPoints[0] == wli::io::schema::kNEnergy && t.nPoints[1] > 0 &&
            t.nPoints[2] > 0 && t.nPoints[3] > 0 && t.nPoints[4] > 0,
        "Iso nPoints extents populated (nE == schema::kNEnergy)");

  // Live kernel cells through IsoInterpolateSingleVariable5DPoint. Build the
  // log-space grids the kernel expects (E/rho/T already-log10, Ye raw). Extents
  // are Fortran (nE, nMom, nRho, nT, nYe).
  const int nE = t.nPoints[0], nMom = t.nPoints[1], nD = t.nPoints[2],
            nT = t.nPoints[3], nY = t.nPoints[4];
  std::vector<Real> LogEs(nE), LogDs(nD), LogTs(nT), Ys(nY);
  for (int i = 0; i < nE; ++i)
    LogEs[i] = std::log10(t.common.energyGrid.points[i]);
  for (int i = 0; i < nD; ++i) LogDs[i] = std::log10(t.common.axes[0].points[i]);
  for (int i = 0; i < nT; ++i) LogTs[i] = std::log10(t.common.axes[1].points[i]);
  for (int i = 0; i < nY; ++i) Ys[i] = t.common.axes[2].points[i];

  const int iSpecies = 0, iMom = nMom / 2;
  const Real* tbl = t.values[static_cast<std::size_t>(iSpecies)].data();
  const Real OS = wli::IsoOffset(t.offset.data(), t.nOpacities, t.nMoments,
                                 iSpecies, iMom);
  int iE = nE / 2, iD = nD / 2, iT = nT / 2, iY = nY / 2;

  // Node identity: query at an interior node returns 10**(stored) - offset.
  Real got = wli::IsoInterpolateSingleVariable5DPoint(
      LogEs[iE], LogDs[iD], LogTs[iT], Ys[iY], LogEs.data(), nE, LogDs.data(),
      nD, LogTs.data(), nT, Ys.data(), nY, iMom, nMom, OS, tbl);
  Real want = wli::recover(
      tbl[wli::flat_index<5>({iE, iMom, iD, iT, iY}, {nE, nMom, nD, nT, nY})],
      OS);
  check(wli::is_close(got, want, wli::rtol_relaxed),
        "Iso node identity at an interior grid node");

  // Boundary: a quarter-cell below the E edge extrapolates to a finite value.
  Real Elo = LogEs[0] - Real(0.25) * (LogEs[1] - LogEs[0]);
  check(std::isfinite(wli::IsoInterpolateSingleVariable5DPoint(
            Elo, LogDs[iD], LogTs[iT], Ys[iY], LogEs.data(), nE, LogDs.data(),
            nD, LogTs.data(), nT, Ys.data(), nY, iMom, nMom, OS, tbl)),
        "Iso below-edge E extrapolates to a finite value");

  // NaN propagation on a NaN LogE argument (E/rho/T only, never Ye).
  check(std::isnan(wli::IsoInterpolateSingleVariable5DPoint(
            std::nan(""), LogDs[iD], LogTs[iT], Ys[iY], LogEs.data(), nE,
            LogDs.data(), nD, LogTs.data(), nT, Ys.data(), nY, iMom, nMom, OS,
            tbl)),
        "Iso NaN propagation on a NaN LogE argument");

  // FD cross-check: perturb the PHYSICAL coordinate and recompute LogX (axes are
  // pre-log10'd). E/rho/T are LOG axes; Ye is the one LINEAR axis. Query at an
  // interior cell center, away from node kinks.
  {
    auto evalP = [&](Real E, Real D, Real T, Real Y) {
      return wli::IsoInterpolateSingleVariable5DPoint(
          std::log10(E), std::log10(D), std::log10(T), Y, LogEs.data(), nE,
          LogDs.data(), nD, LogTs.data(), nT, Ys.data(), nY, iMom, nMom, OS,
          tbl);
    };
    auto fd = [](Real vm2, Real vm1, Real vp1, Real vp2, Real h) {
      return (-vp2 + 8.0 * vp1 - 8.0 * vm1 + vm2) / (12.0 * h);
    };
    Real E = std::pow(Real(10), Real(0.5) * (LogEs[iE] + LogEs[iE + 1]));
    Real D = std::pow(Real(10), Real(0.5) * (LogDs[iD] + LogDs[iD + 1]));
    Real T = std::pow(Real(10), Real(0.5) * (LogTs[iT] + LogTs[iT + 1]));
    Real Y = Real(0.5) * (Ys[iY] + Ys[iY + 1]);
    wli::IsoPointDeriv d =
        wli::IsoInterpolateDifferentiateSingleVariable5DPoint(
            std::log10(E), std::log10(D), std::log10(T), Y, LogEs.data(), nE,
            LogDs.data(), nD, LogTs.data(), nT, Ys.data(), nY, iMom, nMom, OS,
            tbl);
    Real hE = 1.0e-3 * E, hD = 1.0e-3 * D, hT = 1.0e-3 * T, hY = 1.0e-3 * Y;
    Real fdE = fd(evalP(E - 2 * hE, D, T, Y), evalP(E - hE, D, T, Y),
                  evalP(E + hE, D, T, Y), evalP(E + 2 * hE, D, T, Y), hE);
    Real fdD = fd(evalP(E, D - 2 * hD, T, Y), evalP(E, D - hD, T, Y),
                  evalP(E, D + hD, T, Y), evalP(E, D + 2 * hD, T, Y), hD);
    Real fdT = fd(evalP(E, D, T - 2 * hT, Y), evalP(E, D, T - hT, Y),
                  evalP(E, D, T + hT, Y), evalP(E, D, T + 2 * hT, Y), hT);
    Real fdY = fd(evalP(E, D, T, Y - 2 * hY), evalP(E, D, T, Y - hY),
                  evalP(E, D, T, Y + hY), evalP(E, D, T, Y + 2 * hY), hY);
    check(wli::is_close(d.dDE, fdE, wli::rtol_relaxed, wli::atol_default),
          "Iso FD cross-check ∂value/∂E (1e-10)");
    check(wli::is_close(d.dDrho, fdD, wli::rtol_relaxed, wli::atol_default),
          "Iso FD cross-check ∂value/∂rho (1e-10)");
    check(wli::is_close(d.dDT, fdT, wli::rtol_relaxed, wli::atol_default),
          "Iso FD cross-check ∂value/∂T (1e-10)");
    check(wli::is_close(d.dDY, fdY, wli::rtol_relaxed, wli::atol_default),
          "Iso FD cross-check ∂value/∂Ye (1e-10)");
  }
}

void run_nespair(const std::string& path, bool nes) {
  wli::io::HostScatNESPairTable t =
      nes ? wli::io::read_scat_nes_table(path)
          : wli::io::read_scat_pair_table(path);
  const int expOp =
      nes ? wli::io::schema::nes::kNOpacities : wli::io::schema::pair::kNOpacities;
  const int expMom =
      nes ? wli::io::schema::nes::kNMoments : wli::io::schema::pair::kNMoments;
  check(t.nOpacities == expOp && t.nMoments == expMom,
        std::string(nes ? "NES" : "Pair") +
            " nOpacities/nMoments == schema");
  check(t.nPoints[0] > 0 && t.nPoints[1] > 0 && t.nPoints[2] > 0 &&
            t.nPoints[3] > 0 && t.nPoints[4] > 0,
        std::string(nes ? "NES" : "Pair") + " nPoints extents populated");

  const std::string tag(nes ? "NES" : "Pair");

  // Live kernel cells through the channel-neutral 2D-aligned bilinear. The
  // interpolated plane is (log10 T, log10 eta); the two energy indices and the
  // kernel-component index are direct discrete table indices (NOT interpolated).
  // Extents are Fortran (nEp, nE, nMom, nT, nEta). LogTs feeds from the shared
  // /ThermoState temperature axis; LogXs from /EtaGrid.
  const int nEp = t.nPoints[0], nE = t.nPoints[1], nMom = t.nPoints[2],
            nT = t.nPoints[3], nEta = t.nPoints[4];
  std::vector<Real> LogTs(nT), LogXs(nEta);
  for (int i = 0; i < nT; ++i) LogTs[i] = std::log10(t.common.axes[1].points[i]);
  for (int i = 0; i < nEta; ++i) LogXs[i] = std::log10(t.etaGrid.points[i]);

  const int iSpecies = 0;      // nOpacities == 1
  const int kernel = nMom / 2;  // discrete kernel-component index
  const Real OS = wli::IsoOffset(t.offset.data(), t.nOpacities, t.nMoments,
                                 iSpecies, kernel);
  const Real* tbl = t.kernels.data();
  int iEp = nEp / 2, iE = nE / 2, iT = nT / 2, iX = nEta / 2;

  // Node identity at an interior (T, eta) node with fixed energy/kernel indices.
  Real got = wli::NESPairInterpolateSingleVariable2D2DAlignedPoint(
      LogTs[iT], LogXs[iX], LogTs.data(), nT, LogXs.data(), nEta, iEp, iE, nEp,
      nE, kernel, nMom, OS, tbl);
  Real want = wli::recover(
      tbl[wli::flat_index<5>({iEp, iE, kernel, iT, iX},
                             {nEp, nE, nMom, nT, nEta})],
      OS);
  check(wli::is_close(got, want, wli::rtol_relaxed),
        tag + " node identity at an interior grid node");

  // Boundary: a quarter-cell below the T edge extrapolates to a finite value.
  Real Tlo = LogTs[0] - Real(0.25) * (LogTs[1] - LogTs[0]);
  check(std::isfinite(wli::NESPairInterpolateSingleVariable2D2DAlignedPoint(
            Tlo, LogXs[iX], LogTs.data(), nT, LogXs.data(), nEta, iEp, iE, nEp,
            nE, kernel, nMom, OS, tbl)),
        tag + " below-edge T extrapolates to a finite value");

  // NaN propagation on a NaN LogT argument.
  check(std::isnan(wli::NESPairInterpolateSingleVariable2D2DAlignedPoint(
            std::nan(""), LogXs[iX], LogTs.data(), nT, LogXs.data(), nEta, iEp,
            iE, nEp, nE, kernel, nMom, OS, tbl)),
        tag + " NaN propagation on a NaN LogT argument");

  // FD cross-check: BOTH (T, eta) are LOG axes — perturb the PHYSICAL coordinate
  // and recompute the log (T in MeV, eta the physical degeneracy). Query at an
  // interior cell center, away from node kinks.
  {
    auto evalP = [&](Real T, Real X) {
      return wli::NESPairInterpolateSingleVariable2D2DAlignedPoint(
          std::log10(T), std::log10(X), LogTs.data(), nT, LogXs.data(), nEta,
          iEp, iE, nEp, nE, kernel, nMom, OS, tbl);
    };
    auto fd = [](Real vm2, Real vm1, Real vp1, Real vp2, Real h) {
      return (-vp2 + 8.0 * vp1 - 8.0 * vm1 + vm2) / (12.0 * h);
    };
    Real T = std::pow(Real(10), Real(0.5) * (LogTs[iT] + LogTs[iT + 1]));
    Real X = std::pow(Real(10), Real(0.5) * (LogXs[iX] + LogXs[iX + 1]));
    wli::NESPairPointDeriv d =
        wli::NESPairInterpolateDifferentiateSingleVariable2D2DAlignedPoint(
            std::log10(T), std::log10(X), LogTs.data(), nT, LogXs.data(), nEta,
            iEp, iE, nEp, nE, kernel, nMom, OS, tbl);
    Real hT = 1.0e-3 * T, hX = 1.0e-3 * X;
    Real fdT = fd(evalP(T - 2 * hT, X), evalP(T - hT, X), evalP(T + hT, X),
                  evalP(T + 2 * hT, X), hT);
    Real fdX = fd(evalP(T, X - 2 * hX), evalP(T, X - hX), evalP(T, X + hX),
                  evalP(T, X + 2 * hX), hX);
    check(wli::is_close(d.dDT, fdT, wli::rtol_relaxed, wli::atol_default),
          tag + " FD cross-check ∂value/∂T (1e-10)");
    check(wli::is_close(d.dDX, fdX, wli::rtol_relaxed, wli::atol_default),
          tag + " FD cross-check ∂value/∂eta (1e-10)");
  }
}

void run_brem(const std::string& path) {
  wli::io::HostScatBremTable t = wli::io::read_scat_brem_table(path);
  check(t.nOpacities == wli::io::schema::brem::kNOpacities &&
            t.nMoments == wli::io::schema::brem::kNMoments,
        "Brem nOpacities/nMoments == schema::brem");
  check(t.nPoints[0] > 0 && t.nPoints[1] > 0 && t.nPoints[2] > 0 &&
            t.nPoints[3] > 0 && t.nPoints[4] > 0,
        "Brem nPoints extents populated");

  // Live kernel cells through the single-density inner bilinear. CRITICAL: the
  // interpolated axes are (rho, T) — density BEFORE temperature — with extents
  // Fortran (nEp, nE, nMom, nRho, nT). LogDs = axes[0] (rho), LogTs = axes[1].
  const int nEp = t.nPoints[0], nE = t.nPoints[1], nMom = t.nPoints[2],
            nD = t.nPoints[3], nT = t.nPoints[4];
  std::vector<Real> LogDs(nD), LogTs(nT);
  for (int i = 0; i < nD; ++i) LogDs[i] = std::log10(t.common.axes[0].points[i]);
  for (int i = 0; i < nT; ++i) LogTs[i] = std::log10(t.common.axes[1].points[i]);

  const int iSpecies = 0, moment = 0;  // nOpacities == nMoments == 1
  const Real OS = wli::IsoOffset(t.offset.data(), t.nOpacities, t.nMoments,
                                 iSpecies, moment);
  const Real* tbl = t.sSigma.data();
  int iEp = nEp / 2, iE = nE / 2, iD = nD / 2, iT = nT / 2;

  // Node identity at an interior (rho, T) node with fixed energy/moment indices.
  Real got = wli::BremInterpolateSingleDensity2DAlignedPoint(
      LogDs[iD], LogTs[iT], LogDs.data(), nD, LogTs.data(), nT, iEp, iE, nEp, nE,
      moment, nMom, OS, tbl);
  Real want = wli::recover(
      tbl[wli::flat_index<5>({iEp, iE, moment, iD, iT},
                             {nEp, nE, nMom, nD, nT})],
      OS);
  check(wli::is_close(got, want, wli::rtol_relaxed),
        "Brem node identity at an interior grid node");

  // Boundary: a quarter-cell below the rho edge extrapolates to a finite value.
  Real Dlo = LogDs[0] - Real(0.25) * (LogDs[1] - LogDs[0]);
  check(std::isfinite(wli::BremInterpolateSingleDensity2DAlignedPoint(
            Dlo, LogTs[iT], LogDs.data(), nD, LogTs.data(), nT, iEp, iE, nEp, nE,
            moment, nMom, OS, tbl)),
        "Brem below-edge rho extrapolates to a finite value");

  // NaN propagation on a NaN LogD argument.
  check(std::isnan(wli::BremInterpolateSingleDensity2DAlignedPoint(
            std::nan(""), LogTs[iT], LogDs.data(), nD, LogTs.data(), nT, iEp, iE,
            nEp, nE, moment, nMom, OS, tbl)),
        "Brem NaN propagation on a NaN LogD argument");

  // FD cross-check of the single-effective-density differentiate kernel. BOTH
  // (rho, T) are LOG axes (rho BEFORE T — the Brem transpose); perturb the
  // PHYSICAL coordinate and recompute the log. Query at an interior cell center,
  // away from node kinks.
  {
    auto evalP = [&](Real D, Real T) {
      return wli::BremInterpolateSingleDensity2DAlignedPoint(
          std::log10(D), std::log10(T), LogDs.data(), nD, LogTs.data(), nT, iEp,
          iE, nEp, nE, moment, nMom, OS, tbl);
    };
    auto fd = [](Real vm2, Real vm1, Real vp1, Real vp2, Real h) {
      return (-vp2 + 8.0 * vp1 - 8.0 * vm1 + vm2) / (12.0 * h);
    };
    Real D = std::pow(Real(10), Real(0.5) * (LogDs[iD] + LogDs[iD + 1]));
    Real T = std::pow(Real(10), Real(0.5) * (LogTs[iT] + LogTs[iT + 1]));
    wli::BremPointDeriv d =
        wli::BremInterpolateSingleDensityDifferentiate2DAlignedPoint(
            std::log10(D), std::log10(T), LogDs.data(), nD, LogTs.data(), nT,
            iEp, iE, nEp, nE, moment, nMom, OS, tbl);
    Real hD = 1.0e-3 * D, hT = 1.0e-3 * T;
    Real fdD = fd(evalP(D - 2 * hD, T), evalP(D - hD, T), evalP(D + hD, T),
                  evalP(D + 2 * hD, T), hD);
    Real fdT = fd(evalP(D, T - 2 * hT), evalP(D, T - hT), evalP(D, T + hT),
                  evalP(D, T + 2 * hT), hT);
    check(wli::is_close(d.dDrho, fdD, wli::rtol_relaxed, wli::atol_default),
          "Brem FD cross-check ∂value/∂rho (1e-10)");
    check(wli::is_close(d.dDT, fdT, wli::rtol_relaxed, wli::atol_default),
          "Brem FD cross-check ∂value/∂T (1e-10)");

    // Summed differentiate kernel: value/dDT are populated (linearity), dDrho is
    // 0 by design (base-density chain rule is consumer-side, spec:104-113). Use a
    // single effective density so the sum reduces to Alpha*single-density.
    Real LogDspec[1] = {std::log10(D)};
    Real Alpha[1] = {Real(1)};
    wli::BremPointDeriv ds =
        wli::BremInterpolateSingleVariable2D2DAlignedSummedDifferentiatePoint(
            LogDspec, Alpha, 1, std::log10(T), LogDs.data(), nD, LogTs.data(),
            nT, iEp, iE, nEp, nE, moment, nMom, OS, tbl);
    check(wli::is_close(ds.dDT, fdT, wli::rtol_relaxed, wli::atol_default),
          "Brem summed FD cross-check ∂value/∂T (1e-10)");
    check(ds.dDrho == Real(0), "Brem summed dDrho stays 0 by design");
  }
}

// Probe + run one channel; returns true iff the table was present.
template <typename F>
bool one(const std::string& root, const char* base, F&& runner) {
  std::string path = resolve(root, base);
  if (path.empty()) {
    std::printf("SKIPPED %s (not found under WL_TABLES_ROOT)\n", base);
    return false;
  }
  std::printf("PRESENT %s -> %s\n", base, path.c_str());
  try {
    runner(path);
  } catch (const std::exception& e) {
    check(false, std::string("reader threw on present table ") + base + ": " +
                     e.what());
  } catch (...) {
    check(false, std::string("reader threw (unknown) on present table ") + base);
  }
  return true;
}

}  // namespace

// The reader logic. Bracketed by amrex::Initialize/Finalize in main() because,
// under an MPI build, the readers' ParallelDescriptor::Bcast runs on AMReX's
// communicator, which requires AMReX to be initialized (spec:171 precondition).
int run() {
  const char* rootEnv = std::getenv("WL_TABLES_ROOT");
  if (rootEnv == nullptr || rootEnv[0] == '\0') {
    std::printf(
        "SKIP production_tables: WL_TABLES_ROOT unset — no live .h5 to probe.\n"
        "(synthetic coverage_matrix is the always-on gate; this cell only adds "
        "real-table anchoring when the production tables are present.)\n");
    return 77;  // CTest SKIP_RETURN_CODE
  }
  const std::string root(rootEnv);

  int present = 0;
  present += one(root, kEosBase, [](const std::string& p) { run_eos(p); });
  present += one(root, kEmAbBase, [](const std::string& p) { run_emab(p); });
  present += one(root, kIsoBase, [](const std::string& p) { run_iso(p); });
  present +=
      one(root, kNesBase, [](const std::string& p) { run_nespair(p, true); });
  present +=
      one(root, kPairBase, [](const std::string& p) { run_nespair(p, false); });
  present += one(root, kBremBase, [](const std::string& p) { run_brem(p); });

  if (present == 0) {
    std::printf(
        "SKIP production_tables: WL_TABLES_ROOT set (%s) but none of the 6 "
        "named tables were found.\n",
        root.c_str());
    return 77;  // CTest SKIP_RETURN_CODE
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "production_tables: %d check(s) failed (%d/6 tables present)\n",
                 g_failures, present);
    return EXIT_FAILURE;
  }
  std::printf("PASS production_tables: %d/6 named tables present, all cells ok\n",
              present);
  return EXIT_SUCCESS;
}

int main(int argc, char** argv) {
  amrex::Initialize(argc, argv);
  const int rc = run();
  amrex::Finalize();
  return rc;
}
