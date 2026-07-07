// Standalone WL_TABLES_ROOT-guarded round-trip layout test
// (spec: specs/table-format-and-io.md:180, Verification #2).
//
// Confirms the column-major <-> C-order reversal is applied correctly by the
// live EOS reader. It addresses one interior element of a 3D dependent-variable
// sub-table two INDEPENDENT ways and asserts they are exactly bit-equal (no
// tolerance — this is a layout / byte-order check, not interpolation):
//
//   Path A: wli::io::read_eos_table(path) delivers dv[j].values as a flat,
//           log-stored, Fortran column-major (rho fastest-varying) buffer;
//           index it with the shared wli::flat_index<3> helper.
//   Path B: open the SAME file a SECOND time via a raw H5::H5File and read ONE
//           element of /DependentVariables/<name> with a hyperslab at the
//           corresponding REVERSED C-order start {iYe,iT,iRho}.
//
// Re-indexing Path A's own buffer against its C-order position would be
// self-comparison and would prove nothing about column-major byte ordering
// (spec-io.findings risk #1); the second, raw HDF5 open is what makes Path B
// independent. Both sides compare the raw stored double — no wli::recover() is
// applied (the reader delivers log-stored values, spec:194; any transcendental
// would break exact ==, risk #3).
//
// SKIP discipline (CTest SKIP_RETURN_CODE 77): if WL_TABLES_ROOT is unset, or
// the EOS table is not found under it, this test returns 77 so CTest reports it
// as Skipped rather than passing vacuously. No live .h5 exists in this sandbox,
// so only the skip branch runs here — expected, not a failure.
//
// Test-only helpers (resolve/file_exists/check) are duplicated per-TU, matching
// the established pattern (test_production_tables.cpp, test_emab_legacy_fallback).

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <H5Cpp.h>

#include <AMReX.H>

#include "wli_index.H"
#include "wli_io_eos.H"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& msg) {
  if (ok) {
    std::printf("  ok: %s\n", msg.c_str());
  } else {
    std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    ++g_failures;
  }
}

// The pinned EOS production table (basename from tables.provenance); same
// constant test_production_tables.cpp uses.
const char* kEosBase = "wl-EOS-SFHo-15-25-50.h5";

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

int run() {
  const char* rootEnv = std::getenv("WL_TABLES_ROOT");
  if (rootEnv == nullptr || rootEnv[0] == '\0') {
    std::printf(
        "SKIP layout_roundtrip: WL_TABLES_ROOT unset — no live EOS .h5 to "
        "round-trip.\n");
    return 77;  // CTest SKIP_RETURN_CODE
  }
  const std::string path = resolve(std::string(rootEnv), kEosBase);
  if (path.empty()) {
    std::printf(
        "SKIP layout_roundtrip: WL_TABLES_ROOT set (%s) but EOS table %s not "
        "found.\n",
        rootEnv, kEosBase);
    return 77;  // CTest SKIP_RETURN_CODE
  }

  // ---- Path A: the live reader (flat column-major buffer). -----------------
  wli::io::HostEosTable t = wli::io::read_eos_table(path);

  const int nRho = t.nPoints[0];  // Fortran (nRho, nT, nYe)
  const int nT = t.nPoints[1];
  const int nYe = t.nPoints[2];
  check(nRho == 185 && nT == 81 && nYe == 30,
        "extents == Fortran (185, 81, 30)");
  check(!t.dv.empty(), "at least one dependent variable loaded");
  if (t.dv.empty() || nRho <= 0 || nT <= 0 || nYe <= 0) {
    std::fprintf(stderr, "FAILED layout_roundtrip: no DV / bad extents\n");
    return EXIT_FAILURE;
  }

  // Pick the DV by loaded name (not hardcoded); build Path B's dataset path.
  const wli::io::HostDV& dv = t.dv[0];
  const std::string dsetName = "/DependentVariables/" + dv.name;

  // Interior index tuple (0-based Fortran), well inside {185, 81, 30}.
  const int iRho = 100, iT = 40, iYe = 15;
  check(iRho < nRho && iT < nT && iYe < nYe,
        "chosen index tuple is in range");

  const std::size_t expectN =
      static_cast<std::size_t>(nRho) * nT * nYe;
  check(dv.values.size() == expectN,
        "dv[0].values.size() == nRho*nT*nYe");
  if (dv.values.size() != expectN || iRho >= nRho || iT >= nT || iYe >= nYe) {
    std::fprintf(stderr, "FAILED layout_roundtrip: buffer/index mismatch\n");
    return EXIT_FAILURE;
  }

  // Column-major flat offset via the shared helper (i0 = rho fastest-varying).
  const amrex::Long off = wli::flat_index<3>({iRho, iT, iYe},
                                             {nRho, nT, nYe});
  const double pathA = dv.values[static_cast<std::size_t>(off)];

  // ---- Path B: an INDEPENDENT raw HDF5 open + single-element hyperslab. -----
  // h5ls / C-order shape is the reverse of the Fortran logical shape:
  // Fortran (nRho, nT, nYe) = (185, 81, 30)  =>  C-order {nYe, nT, nRho}.
  // So Fortran (iRho, iT, iYe) maps to C-order hyperslab start {iYe, iT, iRho}.
  double pathB = 0.0;
  bool readB = false;
  H5::Exception::dontPrint();
  try {
    H5::H5File f(path, H5F_ACC_RDONLY);
    H5::DataSet ds = f.openDataSet(dsetName);
    H5::DataSpace fileSpace = ds.getSpace();

    const hsize_t start[3] = {static_cast<hsize_t>(iYe),
                              static_cast<hsize_t>(iT),
                              static_cast<hsize_t>(iRho)};
    const hsize_t count[3] = {1, 1, 1};
    fileSpace.selectHyperslab(H5S_SELECT_SET, count, start);

    const hsize_t memDim = 1;
    H5::DataSpace memSpace(1, &memDim);

    ds.read(&pathB, H5::PredType::NATIVE_DOUBLE, memSpace, fileSpace);
    readB = true;
  } catch (const H5::Exception& e) {
    std::fprintf(stderr, "FAIL: raw HDF5 read of %s threw: %s\n",
                 dsetName.c_str(), e.getDetailMsg().c_str());
    ++g_failures;
  }
  check(readB, "Path B raw hyperslab read succeeded");

  // The load-bearing assertion: exact bit-equality (log-stored, no recover).
  if (readB) {
    check(pathA == pathB,
          "Path A (flat_index column-major) == Path B (raw C-order hyperslab), "
          "exact");
    if (pathA != pathB) {
      std::fprintf(stderr,
                   "  pathA=%.17g pathB=%.17g dv=%s (iRho=%d iT=%d iYe=%d)\n",
                   pathA, pathB, dv.name.c_str(), iRho, iT, iYe);
    }
  }

  if (g_failures == 0) {
    std::printf(
        "PASS layout_roundtrip: column-major reversal verified on %s (%s)\n",
        dv.name.c_str(), path.c_str());
    return EXIT_SUCCESS;
  }
  std::fprintf(stderr, "FAILED layout_roundtrip: %d check(s)\n", g_failures);
  return EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {
  // amrex::Initialize is required because read_eos_table calls
  // ParallelDescriptor::IOProcessor() (mirrors test_production_tables main()).
  amrex::Initialize(argc, argv, false);
  const int rc = run();
  amrex::Finalize();
  return rc;
}
