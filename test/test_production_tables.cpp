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
}

void run_iso(const std::string& path) {
  wli::io::HostScatIsoTable t = wli::io::read_scat_iso_table(path);
  check(t.nOpacities == wli::io::schema::iso::kNOpacities &&
            t.nMoments == wli::io::schema::iso::kNMoments,
        "Iso nOpacities/nMoments == schema::iso");
  check(t.nPoints[0] == wli::io::schema::kNEnergy && t.nPoints[1] > 0 &&
            t.nPoints[2] > 0 && t.nPoints[3] > 0 && t.nPoints[4] > 0,
        "Iso nPoints extents populated (nE == schema::kNEnergy)");
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
}

void run_brem(const std::string& path) {
  wli::io::HostScatBremTable t = wli::io::read_scat_brem_table(path);
  check(t.nOpacities == wli::io::schema::brem::kNOpacities &&
            t.nMoments == wli::io::schema::brem::kNMoments,
        "Brem nOpacities/nMoments == schema::brem");
  check(t.nPoints[0] > 0 && t.nPoints[1] > 0 && t.nPoints[2] > 0 &&
            t.nPoints[3] > 0 && t.nPoints[4] > 0,
        "Brem nPoints extents populated");
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
