// Host-level ParallelFor array-form wrapper acceptance probe.
//
// Exercises the 15 free wli:: array-form launchers that own one
// amrex::ParallelFor over the existing scalar _Point cores
// (src/eos/wli_eos.H, wli_eos_inversion.H, src/opacity/wli_opacity_{emab_iso,
// nes_pair,brem}.H; specs/amrex-device-interface.md:80-88,100-102). For every
// channel the test:
//   1. builds a small synthetic monotone axis grid + flat column-major table,
//      uploads it via ResidentTable<D>::upload(...) -> .view();
//   2. places the axis grids, per-point query inputs, and the output buffer in
//      amrex::Gpu::DeviceVector (device-correct; collapses to host on this
//      CPU-only build);
//   3. runs the wrapper over a handful of query points (this drives the
//      ParallelFor lambda — proving the _Point core is callable inside a kernel
//      and that the closure captures the table pointer + extents by value);
//   4. independently runs a serial host loop calling the SAME _Point core per
//      point on the host-side grids/table;
//   5. asserts per-element BIT-IDENTITY with raw == (both paths invoke the
//      identical _Point core on identical inputs — the task contract is
//      "bit-identical to a serial loop of the _Point core").
//
// For the differentiate / inversion forms the compared payload is the struct's
// fields (value/dDrho/dDT/dDY, T/Error). Pure synthetic host math: no HDF5, no
// SKIP guard. amrex::Initialize/Finalize bracket main (ParallelFor + arena-backed
// DeviceVector need the runtime), mirroring test/test_table_residency.cpp.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <AMReX.H>
#include <AMReX_GpuContainers.H>
#include <AMReX_GpuLaunch.H>

#include "wli_eos.H"
#include "wli_eos_inversion.H"
#include "wli_opacity_brem.H"
#include "wli_opacity_emab_iso.H"
#include "wli_opacity_nes_pair.H"
#include "wli_real.H"
#include "wli_table.H"

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

// Copy a host buffer into an arena-backed device buffer via the real
// Gpu::htod_memcpy (drop-in for a future GPU build; a plain memcpy on CPU).
amrex::Gpu::DeviceVector<double> to_device(std::vector<double> const& h) {
  amrex::Gpu::DeviceVector<double> d(h.size());
  amrex::Gpu::htod_memcpy(d.data(), h.data(), h.size() * sizeof(double));
  return d;
}

// Upload a host table (flat, column-major) into a ResidentTable<D> and return a
// by-value TableView<D> for the wrappers to consume.
template <int D>
wli::TableView<D> upload_view(wli::ResidentTable<D>& rt,
                              amrex::GpuArray<int, D> const& n,
                              std::vector<double> const& host) {
  wli::TableMeta<D> meta;
  meta.n = n;
  for (int d = 0; d < D; ++d) meta.kind[d] = wli::AxisKind::Log;
  meta.OS = 0.0;
  rt.upload(meta, host.data(), host.size());
  return rt.view();
}

// A bounded, finite, smooth log-stored table value keyed on the flat offset.
double tval(std::size_t k) { return 0.35 + 0.4 * std::sin(0.6 * double(k) + 0.2); }

// ---------------------------------------------------------------------------
// EOS 3D: evaluate + evaluate-and-differentiate wrappers (wli_eos.H).
// ---------------------------------------------------------------------------
void test_eos() {
  const int nD = 4, nT = 5, nY = 3;
  std::vector<double> Ds{1.0e3, 5.0e5, 2.0e8, 6.0e11};
  std::vector<double> Ts{1.0e-1, 3.0e0, 1.0e1, 4.0e1, 1.0e2};
  std::vector<double> Ys{0.05, 0.30, 0.55};
  const double OS = 3.5;

  std::vector<double> host(static_cast<std::size_t>(nD) * nT * nY);
  for (std::size_t k = 0; k < host.size(); ++k) host[k] = tval(k);

  wli::ResidentTable<3> rt;
  auto view = upload_view<3>(rt, amrex::GpuArray<int, 3>{nD, nT, nY}, host);

  std::vector<double> D{7.3e6, 2.0e4, 1.0e10, 9.1e7, 3.3e5};
  std::vector<double> T{6.0, 20.0, 50.0, 2.5, 80.0};
  std::vector<double> Y{0.22, 0.10, 0.45, 0.33, 0.50};
  const int np = static_cast<int>(D.size());

  auto dDs = to_device(Ds), dTs = to_device(Ts), dYs = to_device(Ys);
  auto dD = to_device(D), dT = to_device(T), dY = to_device(Y);

  // --- evaluate ---
  amrex::Gpu::DeviceVector<double> ev_out(np);
  wli::EosInterpolateSingleVariable3D(np, dD.data(), dT.data(), dY.data(),
                                      dDs.data(), dTs.data(), dYs.data(), OS,
                                      view, ev_out.data());
  amrex::Gpu::streamSynchronize();

  bool ev_ok = true;
  for (int p = 0; p < np; ++p) {
    double want = wli::EosInterpolateSingleVariable3DPoint(
        D[p], T[p], Y[p], Ds.data(), nD, Ts.data(), nT, Ys.data(), nY, OS,
        host.data());
    ev_ok &= (ev_out.data()[p] == want);
  }
  check(ev_ok, "EosInterpolateSingleVariable3D array == serial _Point (bit-identical)");

  // --- differentiate ---
  amrex::Gpu::DeviceVector<wli::EosPointDeriv> df_out(np);
  wli::EosInterpolateDifferentiateSingleVariable3D(
      np, dD.data(), dT.data(), dY.data(), dDs.data(), dTs.data(), dYs.data(),
      OS, view, df_out.data());
  amrex::Gpu::streamSynchronize();

  bool df_ok = true;
  for (int p = 0; p < np; ++p) {
    wli::EosPointDeriv want = wli::EosInterpolateDifferentiateSingleVariable3DPoint(
        D[p], T[p], Y[p], Ds.data(), nD, Ts.data(), nT, Ys.data(), nY, OS,
        host.data());
    wli::EosPointDeriv got = df_out.data()[p];
    df_ok &= (got.value == want.value) && (got.dDrho == want.dDrho) &&
             (got.dDT == want.dDT) && (got.dDY == want.dDY);
  }
  check(df_ok,
        "EosInterpolateDifferentiateSingleVariable3D array == serial _Point "
        "(all 4 fields bit-identical)");
}

// ---------------------------------------------------------------------------
// EOS inversion _Many family: DEY/DPY/DSY x NoGuess/Guess (wli_eos_inversion.H).
// Affine-in-log table so interior queries invert successfully (Error==0, T>0),
// though bit-identity holds structurally regardless of the code.
// ---------------------------------------------------------------------------
void test_inversion() {
  const int nD = 4, nT = 5, nY = 3;
  std::vector<double> Ds{1.0e3, 5.0e5, 2.0e8, 6.0e11};
  std::vector<double> Ts{1.0e-1, 3.0e0, 1.0e1, 4.0e1, 1.0e2};
  std::vector<double> Ys{0.05, 0.30, 0.55};
  const double OS = 3.5;
  const double kA = 0.7, kB = 1.3, kC = -0.9, kE = 2.1;

  std::vector<double> host(static_cast<std::size_t>(nD) * nT * nY);
  for (int iY = 0; iY < nY; ++iY)
    for (int iT = 0; iT < nT; ++iT)
      for (int iD = 0; iD < nD; ++iD)
        host[static_cast<std::size_t>(iD) + nD * (iT + nT * iY)] =
            kA + kB * std::log10(Ds[iD]) + kC * std::log10(Ts[iT]) + kE * Ys[iY];

  wli::ResidentTable<3> rt;
  auto view = upload_view<3>(rt, amrex::GpuArray<int, 3>{nD, nT, nY}, host);

  wli::EosInversionBounds b;
  b.MinD = Ds[0];
  b.MaxD = Ds[nD - 1];
  b.MinY = Ys[0];
  b.MaxY = Ys[nY - 1];
  {
    double lo = wli::recover(host[0], OS), hi = lo;
    for (double v : host) {
      double r = wli::recover(v, OS);
      lo = std::min(lo, r);
      hi = std::max(hi, r);
    }
    b.MinX = lo;
    b.MaxX = hi;
  }
  b.initialized = true;

  // Query points: build X = forward(D,Tstar,Y) so each query is a genuine
  // dependent value in range; the per-point guess sits in Tstar's own cell.
  std::vector<double> D{7.3e6, 2.0e4, 1.0e10, 9.1e7};
  std::vector<double> Tstar{6.0, 20.0, 50.0, 2.5};
  std::vector<double> Y{0.22, 0.10, 0.45, 0.33};
  const int np = static_cast<int>(D.size());

  std::vector<double> X(np), Guess(np);
  for (int p = 0; p < np; ++p) {
    X[p] = wli::EosInterpolateSingleVariable3DPoint(
        D[p], Tstar[p], Y[p], Ds.data(), nD, Ts.data(), nT, Ys.data(), nY, OS,
        host.data());
    Guess[p] = Tstar[p];
  }

  auto dDs = to_device(Ds), dTs = to_device(Ts), dYs = to_device(Ys);
  auto dD = to_device(D), dX = to_device(X), dY = to_device(Y);
  auto dGuess = to_device(Guess);

  // Serial reference caller for one family's NoGuess/Guess _Point core.
  auto verify = [&](const char* label,
                    void (*many)(int, Real const*, Real const*, Real const*,
                                 Real const*, Real const*, Real const*, Real,
                                 wli::TableView<3> const&,
                                 wli::EosInversionBounds const&,
                                 wli::EosInversionResult*),
                    wli::EosInversionResult (*point)(
                        Real, Real, Real, Real const*, int, Real const*, int,
                        Real const*, int, Real, Real const*,
                        wli::EosInversionBounds const&)) {
    amrex::Gpu::DeviceVector<wli::EosInversionResult> out(np);
    many(np, dD.data(), dX.data(), dY.data(), dDs.data(), dTs.data(),
         dYs.data(), OS, view, b, out.data());
    amrex::Gpu::streamSynchronize();
    bool ok = true, any_solved = false;
    for (int p = 0; p < np; ++p) {
      wli::EosInversionResult want = point(D[p], X[p], Y[p], Ds.data(), nD,
                                           Ts.data(), nT, Ys.data(), nY, OS,
                                           host.data(), b);
      wli::EosInversionResult got = out.data()[p];
      ok &= (got.T == want.T) && (got.Error == want.Error);
      any_solved |= (want.Error == 0 && want.T > Real(0));
    }
    check(ok && any_solved, label);
  };

  verify("ComputeTemperatureWith_DEY_NoGuess_Many == serial _Point (T,Error)",
         &wli::ComputeTemperatureWith_DEY_NoGuess_Many,
         &wli::ComputeTemperatureWith_DEY_NoGuess);
  verify("ComputeTemperatureWith_DPY_NoGuess_Many == serial _Point (T,Error)",
         &wli::ComputeTemperatureWith_DPY_NoGuess_Many,
         &wli::ComputeTemperatureWith_DPY_NoGuess);
  verify("ComputeTemperatureWith_DSY_NoGuess_Many == serial _Point (T,Error)",
         &wli::ComputeTemperatureWith_DSY_NoGuess_Many,
         &wli::ComputeTemperatureWith_DSY_NoGuess);

  // Guess family: extra per-point T_Guess array.
  auto verify_guess =
      [&](const char* label,
          void (*many)(int, Real const*, Real const*, Real const*, Real const*,
                       Real const*, Real const*, Real, wli::TableView<3> const&,
                       Real const*, wli::EosInversionBounds const&,
                       wli::EosInversionResult*),
          wli::EosInversionResult (*point)(
              Real, Real, Real, Real const*, int, Real const*, int, Real const*,
              int, Real, Real const*, Real,
              wli::EosInversionBounds const&)) {
        amrex::Gpu::DeviceVector<wli::EosInversionResult> out(np);
        many(np, dD.data(), dX.data(), dY.data(), dDs.data(), dTs.data(),
             dYs.data(), OS, view, dGuess.data(), b, out.data());
        amrex::Gpu::streamSynchronize();
        bool ok = true, any_solved = false;
        for (int p = 0; p < np; ++p) {
          wli::EosInversionResult want =
              point(D[p], X[p], Y[p], Ds.data(), nD, Ts.data(), nT, Ys.data(),
                    nY, OS, host.data(), Guess[p], b);
          wli::EosInversionResult got = out.data()[p];
          ok &= (got.T == want.T) && (got.Error == want.Error);
          any_solved |= (want.Error == 0 && want.T > Real(0));
        }
        check(ok && any_solved, label);
      };

  verify_guess("ComputeTemperatureWith_DEY_Guess_Many == serial _Point (T,Error)",
               &wli::ComputeTemperatureWith_DEY_Guess_Many,
               &wli::ComputeTemperatureWith_DEY_Guess);
  verify_guess("ComputeTemperatureWith_DPY_Guess_Many == serial _Point (T,Error)",
               &wli::ComputeTemperatureWith_DPY_Guess_Many,
               &wli::ComputeTemperatureWith_DPY_Guess);
  verify_guess("ComputeTemperatureWith_DSY_Guess_Many == serial _Point (T,Error)",
               &wli::ComputeTemperatureWith_DSY_Guess_Many,
               &wli::ComputeTemperatureWith_DSY_Guess);
}

// ---------------------------------------------------------------------------
// EmAb 4D + Iso 5D-slice (wli_opacity_emab_iso.H). Coordinates arrive already
// LOG10'd; grids are ascending log-space nodes.
// ---------------------------------------------------------------------------
void test_emab_iso() {
  const int nE = 4, nD = 3, nT = 5, nY = 3;
  std::vector<double> LogEs{0.0, 0.5, 1.0, 1.6};
  std::vector<double> LogDs{6.0, 9.0, 12.0};
  std::vector<double> LogTs{-1.0, 0.0, 1.0, 1.6, 2.0};
  std::vector<double> Ys{0.05, 0.30, 0.55};
  const double OS = 1.25;

  std::vector<double> host4(static_cast<std::size_t>(nE) * nD * nT * nY);
  for (std::size_t k = 0; k < host4.size(); ++k) host4[k] = tval(k);

  wli::ResidentTable<4> rt4;
  auto view4 =
      upload_view<4>(rt4, amrex::GpuArray<int, 4>{nE, nD, nT, nY}, host4);

  std::vector<double> LogE{0.2, 0.8, 1.3, 0.6, 1.1};
  std::vector<double> LogD{7.0, 10.5, 11.0, 8.2, 9.9};
  std::vector<double> LogT{-0.5, 0.5, 1.3, 0.1, 1.8};
  std::vector<double> Y{0.22, 0.10, 0.45, 0.33, 0.50};
  const int np = static_cast<int>(LogE.size());

  auto dLogEs = to_device(LogEs), dLogDs = to_device(LogDs),
       dLogTs = to_device(LogTs), dYs = to_device(Ys);
  auto dLogE = to_device(LogE), dLogD = to_device(LogD), dLogT = to_device(LogT),
       dY = to_device(Y);

  amrex::Gpu::DeviceVector<double> em_out(np);
  wli::EmAbInterpolateSingleVariable4D(
      np, dLogE.data(), dLogD.data(), dLogT.data(), dY.data(), dLogEs.data(),
      dLogDs.data(), dLogTs.data(), dYs.data(), OS, view4, em_out.data());
  amrex::Gpu::streamSynchronize();

  bool em_ok = true;
  for (int p = 0; p < np; ++p) {
    double want = wli::EmAbInterpolateSingleVariable4DPoint(
        LogE[p], LogD[p], LogT[p], Y[p], LogEs.data(), nE, LogDs.data(), nD,
        LogTs.data(), nT, Ys.data(), nY, OS, host4.data());
    em_ok &= (em_out.data()[p] == want);
  }
  check(em_ok, "EmAbInterpolateSingleVariable4D array == serial _Point (bit-identical)");

  // --- Iso: 5D table (nE, nMom, nD, nT, nY), fixed iMom. ---
  const int nMom = 2;
  const int iMom = 1;
  std::vector<double> host5(
      static_cast<std::size_t>(nE) * nMom * nD * nT * nY);
  for (std::size_t k = 0; k < host5.size(); ++k) host5[k] = tval(k + 7);

  wli::ResidentTable<5> rt5;
  auto view5 = upload_view<5>(
      rt5, amrex::GpuArray<int, 5>{nE, nMom, nD, nT, nY}, host5);

  amrex::Gpu::DeviceVector<double> iso_out(np);
  wli::IsoInterpolateSingleVariable5D(
      np, dLogE.data(), dLogD.data(), dLogT.data(), dY.data(), dLogEs.data(),
      dLogDs.data(), dLogTs.data(), dYs.data(), iMom, OS, view5, iso_out.data());
  amrex::Gpu::streamSynchronize();

  bool iso_ok = true;
  for (int p = 0; p < np; ++p) {
    double want = wli::IsoInterpolateSingleVariable5DPoint(
        LogE[p], LogD[p], LogT[p], Y[p], LogEs.data(), nE, LogDs.data(), nD,
        LogTs.data(), nT, Ys.data(), nY, iMom, nMom, OS, host5.data());
    iso_ok &= (iso_out.data()[p] == want);
  }
  check(iso_ok, "IsoInterpolateSingleVariable5D array == serial _Point (bit-identical)");
}

// ---------------------------------------------------------------------------
// NES/Pair 5D aligned + the two symmetry fills (wli_opacity_nes_pair.H).
// Table shape (nEp, nE, nMom, nT, nEta). Selectors (iEp,iE,kernel) shared.
// ---------------------------------------------------------------------------
void test_nes_pair() {
  const int nEp = 3, nE = 3, nMom = 2, nT = 5, nEta = 4;
  std::vector<double> LogTs{-1.0, 0.0, 1.0, 1.6, 2.0};
  std::vector<double> LogXs{-2.0, -1.0, 0.0, 1.0};
  const double OS = 0.75;
  const int iEp = 2, iE = 1, kernel = 1;

  std::vector<double> host(
      static_cast<std::size_t>(nEp) * nE * nMom * nT * nEta);
  for (std::size_t k = 0; k < host.size(); ++k) host[k] = tval(k + 3);

  wli::ResidentTable<5> rt;
  auto view = upload_view<5>(
      rt, amrex::GpuArray<int, 5>{nEp, nE, nMom, nT, nEta}, host);

  std::vector<double> LogT{-0.5, 0.5, 1.3, 0.1, 1.8};
  std::vector<double> LogX{-1.5, -0.5, 0.5, -1.0, 0.9};
  const int np = static_cast<int>(LogT.size());

  auto dLogTs = to_device(LogTs), dLogXs = to_device(LogXs);
  auto dLogT = to_device(LogT), dLogX = to_device(LogX);

  // Physical energies + T for the NES detailed-balance Boltzmann factor.
  std::vector<double> E{5.0, 12.0, 25.0};  // length nEp==nE
  const double Tphys = 8.0;
  auto dE = to_device(E);

  // --- aligned primitive ---
  amrex::Gpu::DeviceVector<double> al_out(np);
  wli::NESPairInterpolateSingleVariable2D2DAligned(
      np, dLogT.data(), dLogX.data(), dLogTs.data(), dLogXs.data(), iEp, iE,
      kernel, OS, view, al_out.data());
  amrex::Gpu::streamSynchronize();
  bool al_ok = true;
  for (int p = 0; p < np; ++p) {
    double want = wli::NESPairInterpolateSingleVariable2D2DAlignedPoint(
        LogT[p], LogX[p], LogTs.data(), nT, LogXs.data(), nEta, iEp, iE, nEp, nE,
        kernel, nMom, OS, host.data());
    al_ok &= (al_out.data()[p] == want);
  }
  check(al_ok,
        "NESPairInterpolateSingleVariable2D2DAligned array == serial _Point "
        "(bit-identical)");

  // --- NES detailed-balance fill (extra shared E[], T) ---
  amrex::Gpu::DeviceVector<double> nes_out(np);
  wli::NESDetailedBalanceFill(np, dLogT.data(), dLogX.data(), dLogTs.data(),
                              dLogXs.data(), iEp, iE, kernel, OS, view,
                              dE.data(), Tphys, nes_out.data());
  amrex::Gpu::streamSynchronize();
  bool nes_ok = true;
  for (int p = 0; p < np; ++p) {
    double want = wli::NESDetailedBalanceFillPoint(
        LogT[p], LogX[p], LogTs.data(), nT, LogXs.data(), nEta, iEp, iE, nEp, nE,
        kernel, nMom, OS, host.data(), E.data(), Tphys);
    nes_ok &= (nes_out.data()[p] == want);
  }
  check(nes_ok, "NESDetailedBalanceFill array == serial _Point (bit-identical)");

  // --- Pair crossing-symmetry fill (no E/T; swapped kernel index) ---
  const int kernelSwapped = 0;
  amrex::Gpu::DeviceVector<double> pr_out(np);
  wli::PairCrossingSymmetryFill(np, dLogT.data(), dLogX.data(), dLogTs.data(),
                                dLogXs.data(), iEp, iE, kernelSwapped, OS, view,
                                pr_out.data());
  amrex::Gpu::streamSynchronize();
  bool pr_ok = true;
  for (int p = 0; p < np; ++p) {
    double want = wli::PairCrossingSymmetryFillPoint(
        LogT[p], LogX[p], LogTs.data(), nT, LogXs.data(), nEta, iEp, iE, nEp, nE,
        kernelSwapped, nMom, OS, host.data());
    pr_ok &= (pr_out.data()[p] == want);
  }
  check(pr_ok, "PairCrossingSymmetryFill array == serial _Point (bit-identical)");
}

// ---------------------------------------------------------------------------
// Brem 5D single-density + summed (wli_opacity_brem.H). Table shape
// (nEp, nE, nMom, nD, nT) — rho BEFORE T (the Brem-specific axis order).
// ---------------------------------------------------------------------------
void test_brem() {
  const int nEp = 3, nE = 3, nMom = 2, nD = 3, nT = 5;
  std::vector<double> LogDs{6.0, 9.0, 12.0};
  std::vector<double> LogTs{-1.0, 0.0, 1.0, 1.6, 2.0};
  const double OS = 0.9;
  const int iEp = 1, iE = 2, moment = 1;

  std::vector<double> host(
      static_cast<std::size_t>(nEp) * nE * nMom * nD * nT);
  for (std::size_t k = 0; k < host.size(); ++k) host[k] = tval(k + 5);

  wli::ResidentTable<5> rt;
  auto view = upload_view<5>(
      rt, amrex::GpuArray<int, 5>{nEp, nE, nMom, nD, nT}, host);

  std::vector<double> LogD{7.0, 10.5, 11.0, 8.2, 9.9};
  std::vector<double> LogT{-0.5, 0.5, 1.3, 0.1, 1.8};
  const int np = static_cast<int>(LogT.size());

  auto dLogDs = to_device(LogDs), dLogTs = to_device(LogTs);
  auto dLogD = to_device(LogD), dLogT = to_device(LogT);

  // --- single-effective-density inner kernel ---
  amrex::Gpu::DeviceVector<double> sd_out(np);
  wli::BremInterpolateSingleDensity2DAligned(
      np, dLogD.data(), dLogT.data(), dLogDs.data(), dLogTs.data(), iEp, iE,
      moment, OS, view, sd_out.data());
  amrex::Gpu::streamSynchronize();
  bool sd_ok = true;
  for (int p = 0; p < np; ++p) {
    double want = wli::BremInterpolateSingleDensity2DAlignedPoint(
        LogD[p], LogT[p], LogDs.data(), nD, LogTs.data(), nT, iEp, iE, nEp, nE,
        moment, nMom, OS, host.data());
    sd_ok &= (sd_out.data()[p] == want);
  }
  check(sd_ok,
        "BremInterpolateSingleDensity2DAligned array == serial _Point "
        "(bit-identical)");

  // --- summed: per-species LogD[nSpecies]/Alpha[nSpecies] shared, LogT varies ---
  const int nSpecies = 3;
  std::vector<double> LogDspec{7.5, 9.5, 10.0};        // shared effective dens
  std::vector<double> Alpha{1.0, 1.0, 28.0 / 3.0};     // Brem weights
  auto dLogDspec = to_device(LogDspec), dAlpha = to_device(Alpha);

  amrex::Gpu::DeviceVector<double> sm_out(np);
  wli::BremInterpolateSingleVariable2D2DAlignedSummed(
      np, dLogDspec.data(), dAlpha.data(), nSpecies, dLogT.data(),
      dLogDs.data(), dLogTs.data(), iEp, iE, moment, OS, view, sm_out.data());
  amrex::Gpu::streamSynchronize();
  bool sm_ok = true;
  for (int p = 0; p < np; ++p) {
    double want = wli::BremInterpolateSingleVariable2D2DAlignedSummedPoint(
        LogDspec.data(), Alpha.data(), nSpecies, LogT[p], LogDs.data(), nD,
        LogTs.data(), nT, iEp, iE, nEp, nE, moment, nMom, OS, host.data());
    sm_ok &= (sm_out.data()[p] == want);
  }
  check(sm_ok,
        "BremInterpolateSingleVariable2D2DAlignedSummed array == serial _Point "
        "(bit-identical)");
}

}  // namespace

int main(int argc, char* argv[]) {
  amrex::Initialize(argc, argv);

  test_eos();
  test_inversion();
  test_emab_iso();
  test_nes_pair();
  test_brem();

  amrex::Finalize();

  if (g_failures != 0) {
    std::fprintf(stderr, "parallelfor_wrappers: %d check(s) failed\n",
                 g_failures);
    return EXIT_FAILURE;
  }
  std::printf(
      "PASS parallelfor_wrappers: 15 array-form wrappers bit-identical to "
      "serial _Point loops\n");
  return EXIT_SUCCESS;
}
