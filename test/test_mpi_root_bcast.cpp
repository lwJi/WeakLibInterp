// MPI root-read + broadcast tests for the HDF5 readers
// (specs/table-format-and-io.md:183-184, requirements 5-6). Two modes, both run
// under mpiexec at >= 2 ranks (registered with a custom multi-rank COMMAND that
// bypasses the global 1-rank CMAKE_TEST_LAUNCHER):
//
//   "open"  (test (a)): a synthetic EOS .h5 is written by the I/O root only,
//           then every rank calls read_eos_table. Asserts the shared open
//           counter (wli::io::hdf5_open_count) advances by exactly 1 on the root
//           and by 0 on every non-root rank (non-root ranks never open the file),
//           and that a byte digest of the loaded table is identical on all ranks.
//
//   "fail"  (test (b)): every rank calls read_eos_table on a nonexistent path.
//           Asserts every rank throws the SAME std::runtime_error (collective
//           failure delivered via the status-flag-first broadcast) and that the
//           test completes — no rank is left hanging in a later array broadcast.
//
// This is an MPI-only test: outside an MPI build (AMReX_MPI=OFF) AMREX_USE_MPI is
// undefined and main returns 77 (CTest SKIP), so the default build/ suite and the
// 1-rank build-mpi/ suite both stay green. Rank consistency uses an exact byte
// digest (not wli::is_close): broadcasts preserve bits, so the tier is bitwise.
//
// The synthetic writer lives here (test-only, never in src/io/) so the readers
// stay read-only per spec:21. It emits just enough datasets for read_eos_table
// to succeed: the /ThermoState + /DependentVariables groups, the 15 hardcoded
// i<Name> slot scalars the reader reads, and /DependentVariables/Repaired.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <AMReX.H>

#ifdef AMREX_USE_MPI

#include <H5Cpp.h>

#include <AMReX_ParallelDescriptor.H>

#include "wli_io_eos.H"

namespace {

namespace pd = amrex::ParallelDescriptor;

// ---- Minimal HDF5 writers (test-only synthetic table). --------------------

void write_int(H5::H5File& f, const std::string& name,
               const std::vector<int>& v, const std::vector<hsize_t>& dims) {
  H5::DataSpace sp(static_cast<int>(dims.size()), dims.data());
  H5::DataSet ds = f.createDataSet(name, H5::PredType::NATIVE_INT, sp);
  ds.write(v.data(), H5::PredType::NATIVE_INT);
}

void write_double(H5::H5File& f, const std::string& name,
                  const std::vector<double>& v,
                  const std::vector<hsize_t>& dims) {
  H5::DataSpace sp(static_cast<int>(dims.size()), dims.data());
  H5::DataSet ds = f.createDataSet(name, H5::PredType::NATIVE_DOUBLE, sp);
  ds.write(v.data(), H5::PredType::NATIVE_DOUBLE);
}

// Fixed-width string[32] dataset (matches the reader's read_strings).
void write_strings(H5::H5File& f, const std::string& name,
                   const std::vector<std::string>& v) {
  const std::size_t len = 32;
  std::vector<char> buf(v.size() * len, '\0');
  for (std::size_t i = 0; i < v.size(); ++i) {
    std::memcpy(buf.data() + i * len, v[i].data(),
                std::min(len, v[i].size()));
  }
  H5::StrType st(H5::PredType::C_S1, len);
  const hsize_t n = v.size();
  H5::DataSpace sp(1, &n);
  H5::DataSet ds = f.createDataSet(name, st, sp);
  ds.write(buf.data(), st);
}

void write_int_scalar(H5::H5File& f, const std::string& name, int v) {
  write_int(f, name, {v}, {1});
}

// Write a synthetic EOS table with just enough structure for read_eos_table.
void write_synthetic_eos(const std::string& path) {
  const int nRho = 3, nT = 2, nYe = 2;
  const std::size_t nElem =
      static_cast<std::size_t>(nRho) * nT * nYe;
  H5::H5File f(path, H5F_ACC_TRUNC);

  // /ThermoState.
  f.createGroup("/ThermoState");
  write_strings(f, "/ThermoState/Names",
                {"Density", "Temperature", "Electron Fraction"});
  write_int(f, "/ThermoState/LogInterp", {1, 1, 0}, {3});
  std::vector<double> rho(nRho), tmp(nT), ye(nYe);
  for (int i = 0; i < nRho; ++i) rho[i] = 1.0e8 * (i + 1);
  for (int i = 0; i < nT; ++i) tmp[i] = 1.0e10 * (i + 1);
  for (int i = 0; i < nYe; ++i) ye[i] = 0.1 * (i + 1);
  write_double(f, "/ThermoState/Density", rho, {static_cast<hsize_t>(nRho)});
  write_double(f, "/ThermoState/Temperature", tmp, {static_cast<hsize_t>(nT)});
  write_double(f, "/ThermoState/Electron Fraction", ye,
               {static_cast<hsize_t>(nYe)});
  write_int_scalar(f, "/ThermoState/iRho", 1);
  write_int_scalar(f, "/ThermoState/iT", 2);
  write_int_scalar(f, "/ThermoState/iYe", 3);

  // /DependentVariables. Two named DVs is enough (nVariables read, not fixed).
  f.createGroup("/DependentVariables");
  write_int(f, "/DependentVariables/Dimensions", {nRho, nT, nYe}, {3});
  const std::vector<std::string> dvNames = {"Pressure", "Entropy Per Baryon"};
  const int nV = static_cast<int>(dvNames.size());
  write_int_scalar(f, "/DependentVariables/nVariables", nV);
  write_strings(f, "/DependentVariables/Names", dvNames);
  write_double(f, "/DependentVariables/Offsets", {1.0, 2.0},
               {static_cast<hsize_t>(nV)});
  for (int j = 0; j < nV; ++j) {
    std::vector<double> vals(nElem);
    for (std::size_t k = 0; k < nElem; ++k) {
      vals[k] = static_cast<double>(j) + 0.25 * static_cast<double>(k);
    }
    write_double(f, "/DependentVariables/" + dvNames[j], vals,
                 {static_cast<hsize_t>(nYe), static_cast<hsize_t>(nT),
                  static_cast<hsize_t>(nRho)});
  }
  // The 15 hardcoded i<Name> slot scalars the reader reads unconditionally.
  const char* kI[15] = {
      "iAlphaMassFraction",      "iElectronChemicalPotential",
      "iEntropyPerBaryon",       "iGamma1",
      "iHeavyBindingEnergy",     "iHeavyChargeNumber",
      "iHeavyMassFraction",      "iHeavyMassNumber",
      "iInternalEnergyDensity",  "iNeutronChemicalPotential",
      "iNeutronMassFraction",    "iPressure",
      "iProtonChemicalPotential", "iProtonMassFraction",
      "iThermalEnergy"};
  for (int i = 0; i < 15; ++i) {
    write_int_scalar(f, std::string("/DependentVariables/") + kI[i], i + 1);
  }
  std::vector<int> rep(nElem);
  for (std::size_t k = 0; k < nElem; ++k) rep[k] = static_cast<int>(k);
  write_int(f, "/DependentVariables/Repaired", rep,
            {static_cast<hsize_t>(nYe), static_cast<hsize_t>(nT),
             static_cast<hsize_t>(nRho)});
}

// ---- Byte digest of a loaded table (exact / bitwise tier). ----------------

struct Hasher {
  std::uint64_t h = 1469598103934665603ULL;  // FNV-1a offset basis
  void bytes(const void* p, std::size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) {
      h ^= b[i];
      h *= 1099511628211ULL;
    }
  }
  void i(int v) { bytes(&v, sizeof(v)); }
  void d(double v) { bytes(&v, sizeof(v)); }
  void s(const std::string& v) {
    i(static_cast<int>(v.size()));
    bytes(v.data(), v.size());
  }
  void vd(const std::vector<double>& v) {
    i(static_cast<int>(v.size()));
    bytes(v.data(), v.size() * sizeof(double));
  }
  void vi(const std::vector<int>& v) {
    i(static_cast<int>(v.size()));
    bytes(v.data(), v.size() * sizeof(int));
  }
};

std::uint64_t digest(const wli::io::HostEosTable& t) {
  Hasher hh;
  hh.i(t.nVariables);
  for (int p : t.nPoints) hh.i(p);
  for (const auto& ax : t.axes) {
    hh.s(ax.name);
    hh.i(static_cast<int>(ax.kind));
    hh.vd(ax.points);
  }
  for (int v : t.tsIndices) hh.i(v);
  for (const auto& dv : t.dv) {
    hh.s(dv.name);
    hh.d(dv.offset);
    hh.d(dv.vmin);
    hh.d(dv.vmax);
    hh.i(dv.hasExtents ? 1 : 0);
    hh.vd(dv.values);
  }
  hh.i(t.dvIndices.iPressure);
  hh.i(t.dvIndices.iEntropyPerBaryon);
  hh.i(t.dvIndices.iGamma1);
  hh.vi(t.repaired);
  return hh.h;
}

// ---- Test (a): non-root ranks never open; loads are byte-identical. -------

int test_open() {
  const std::string path = "test_mpi_synth_eos.h5";
  if (pd::IOProcessor()) {
    write_synthetic_eos(path);
  }
  pd::Barrier();

  const int before = wli::io::hdf5_open_count();
  wli::io::HostEosTable t = wli::io::read_eos_table(path);
  const int delta = wli::io::hdf5_open_count() - before;

  // Per-rank open-count expectation: exactly 1 on root, 0 elsewhere.
  const int expect = pd::IOProcessor() ? 1 : 0;
  bool open_ok = (delta == expect);
  pd::ReduceBoolAnd(open_ok);

  // Byte-identical replication: every rank's digest must equal the root's.
  long myDig = static_cast<long>(digest(t) & 0x7fffffffffffffffLL);
  long rootDig = myDig;
  pd::Bcast(&rootDig, 1, pd::IOProcessorNumber());
  bool digest_ok = (myDig == rootDig);
  pd::ReduceBoolAnd(digest_ok);

  int rc = 0;
  if (pd::IOProcessor()) {
    std::remove(path.c_str());
    if (!open_ok) {
      std::fprintf(stderr,
                   "FAIL open: open-counter delta != expected (root 1 / "
                   "non-root 0)\n");
      rc = 1;
    }
    if (!digest_ok) {
      std::fprintf(stderr,
                   "FAIL open: loaded table digests differ across ranks\n");
      rc = 1;
    }
    if (rc == 0) {
      std::printf(
          "PASS mpi_root_open: %d ranks, only root opened the .h5, all "
          "loads byte-identical\n",
          pd::NProcs());
    }
  }
  return rc;
}

// ---- Test (b): collective root-open failure, no hang. ---------------------

int test_fail() {
  const std::string path = "definitely_nonexistent_dir/no_such_table.h5";
  bool threw = false;
  bool right_type = false;
  try {
    wli::io::read_eos_table(path);
  } catch (const std::runtime_error&) {
    threw = true;
    right_type = true;
  } catch (const H5::Exception&) {
    // H5::Exception is NOT std::exception-derived on this host; caught by its
    // own type. (The reader converts it to std::runtime_error, so this arm is
    // defensive.)
    threw = true;
  } catch (const std::exception&) {
    threw = true;
  } catch (...) {
    threw = true;
  }

  // Every rank must have thrown (reaching here at all proves no rank hung in a
  // broadcast) and thrown the same std::runtime_error type.
  bool all_threw = threw;
  pd::ReduceBoolAnd(all_threw);
  bool all_same = right_type;
  pd::ReduceBoolAnd(all_same);

  int rc = 0;
  if (pd::IOProcessor()) {
    if (!all_threw) {
      std::fprintf(stderr, "FAIL fail: some rank did not throw on bad path\n");
      rc = 1;
    }
    if (!all_same) {
      std::fprintf(stderr,
                   "FAIL fail: not every rank threw the same "
                   "std::runtime_error\n");
      rc = 1;
    }
    if (rc == 0) {
      std::printf(
          "PASS mpi_root_open_failure: %d ranks, root-side open failure "
          "surfaced as the same error on every rank (no hang)\n",
          pd::NProcs());
    }
  }
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = (argc > 1) ? argv[1] : "open";

  // build_parm_parse=false: our "open"/"fail" mode selector is NOT an AMReX
  // inputs file, so keep amrex::Initialize from parsing argv as ParmParse args.
  amrex::Initialize(argc, argv, false);
  H5::Exception::dontPrint();

  int rc = 0;
  if (mode == "fail") {
    rc = test_fail();
  } else {
    rc = test_open();
  }

  // Propagate the root's result to every rank so ctest sees a consistent code.
  amrex::ParallelDescriptor::Bcast(&rc, 1,
                                   amrex::ParallelDescriptor::IOProcessorNumber());
  amrex::Finalize();
  return rc;
}

#else  // !AMREX_USE_MPI

int main() {
  std::printf(
      "SKIP mpi_root_bcast: not an MPI build (AMReX_MPI=OFF) — the readers' "
      "root-read+broadcast path reduces to the serial read exercised by the "
      "rest of the suite.\n");
  return 77;  // CTest SKIP_RETURN_CODE
}

#endif  // AMREX_USE_MPI
