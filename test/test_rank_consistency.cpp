// MPI rank-consistency cells + cross-rank-mismatch meta-test
// (specs/regression-suite-design.md:65-72,112,121; specs/table-format-and-io.md:183).
// One executable, three argv[1] modes, all run under mpiexec at >= 2 ranks
// (registered with custom multi-rank COMMANDs that bypass the global 1-rank
// CMAKE_TEST_LAUNCHER), all at the EXACT/bitwise tier (no tolerance):
//
//   "load"    (cell a): for each of the six table types, the I/O root writes a
//             minimal synthetic .h5, then every rank calls the live reader and a
//             per-dataset byte digest (extents + offsets + names + every array,
//             via wli_rank_digest.H) of the loaded struct is compared across all
//             ranks. Additionally, when $WL_TABLES_ROOT is set and a production
//             table is present, the same digest check runs against it. The
//             synthetic pass is the always-on gate (never vacuous).
//
//   "result"  (cell b): identical synthetic in-memory tables are built on every
//             rank and each of the 8 public entry points is evaluated at a fixed
//             interior sample query; the root's double result(s) are broadcast
//             and compared bitwise on every rank (identical bytes + deterministic
//             arithmetic => exact equality across ranks). No HDF5.
//
//   "corrupt" (cell c): positive control proving the comparator catches a real
//             cross-rank divergence. After the genuine root-read + broadcast
//             (spec:171), exactly ONE non-root rank corrupts its own loaded copy,
//             then the SAME digest+Bcast+ReduceBoolAnd comparator runs. The
//             meta-test PASSES iff (1) the un-corrupted comparator reports
//             consistent AND (2) the corrupted comparator reports inconsistent.
//             Needs >= 2 ranks; returns 77 (SKIP) at 1 rank (no peer to diverge).
//
// MPI-only: outside an MPI build (AMReX_MPI=OFF) AMREX_USE_MPI is undefined and
// main returns 77 (CTest SKIP), so the default build/ suite stays green with all
// three tests listed and skipped. The comparison idiom is the same as
// test_mpi_root_bcast.cpp: local digest -> Bcast root's -> compare -> ReduceBoolAnd.
//
// The synthetic HDF5 writers live here (test-only, never in src/io/) so the
// readers stay read-only per spec:21. Each emits just enough datasets for the
// matching read_* to succeed (schema names/ranks from src/io/wli_io_opacity.H).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <AMReX.H>

#ifdef AMREX_USE_MPI

#include <cmath>

#include <H5Cpp.h>

#include <AMReX_ParallelDescriptor.H>

#include "wli_eos.H"
#include "wli_eos_inversion.H"
#include "wli_io_eos.H"
#include "wli_io_opacity.H"
#include "wli_opacity.H"
#include "wli_rank_digest.H"
#include "wli_real.H"

namespace {

namespace pd = amrex::ParallelDescriptor;
using wli::Real;
using wli::test::digest;

// ---- Minimal HDF5 writers (test-only synthetic tables). -------------------

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

// A monotonically increasing filler so every synthetic array is distinct and
// deterministic (identical on every rank -> a consistent digest by construction).
std::vector<double> ramp(std::size_t n, double base) {
  std::vector<double> v(n);
  for (std::size_t k = 0; k < n; ++k) v[k] = base + 0.5 * static_cast<double>(k);
  return v;
}

// Small shared grid dimensions for the synthetic opacity files.
constexpr int kNE = 3, kNRho = 3, kNT = 2, kNYe = 2;

// Write a single-axis grid (/EnergyGrid or /EtaGrid): Name + LogInterp + Values.
// No /Zoom -> read_grid loads it as non-geometric.
void write_grid(H5::H5File& f, const std::string& group, const std::string& name,
                int logInterp, const std::vector<double>& values) {
  f.createGroup(group);
  write_strings(f, group + "/Name", {name});
  write_int(f, group + "/LogInterp", {logInterp}, {1});
  write_double(f, group + "/Values", values,
               {static_cast<hsize_t>(values.size())});
}

// Write the shared /ThermoState block (byte-identical layout to the EOS file).
void write_thermo_state(H5::H5File& f) {
  f.createGroup("/ThermoState");
  write_strings(f, "/ThermoState/Names",
                {"Density", "Temperature", "Electron Fraction"});
  write_int(f, "/ThermoState/LogInterp", {1, 1, 0}, {3});
  write_double(f, "/ThermoState/Density", ramp(kNRho, 1.0e8),
               {static_cast<hsize_t>(kNRho)});
  write_double(f, "/ThermoState/Temperature", ramp(kNT, 1.0e10),
               {static_cast<hsize_t>(kNT)});
  write_double(f, "/ThermoState/Electron Fraction", ramp(kNYe, 0.1),
               {static_cast<hsize_t>(kNYe)});
  write_int_scalar(f, "/ThermoState/iRho", 1);
  write_int_scalar(f, "/ThermoState/iT", 2);
  write_int_scalar(f, "/ThermoState/iYe", 3);
}

// The shared header every opacity file carries: /EnergyGrid + /ThermoState.
void write_common(H5::H5File& f) {
  write_grid(f, "/EnergyGrid", "Energy", 1, ramp(kNE, 1.0));
  write_thermo_state(f);
}

// ---- EOS synthetic writer (reused from the EOS root-bcast test). ----------

void write_synthetic_eos(const std::string& path) {
  const int nRho = kNRho, nT = kNT, nYe = kNYe;
  const std::size_t nElem = static_cast<std::size_t>(nRho) * nT * nYe;
  H5::H5File f(path, H5F_ACC_TRUNC);

  f.createGroup("/ThermoState");
  write_strings(f, "/ThermoState/Names",
                {"Density", "Temperature", "Electron Fraction"});
  write_int(f, "/ThermoState/LogInterp", {1, 1, 0}, {3});
  write_double(f, "/ThermoState/Density", ramp(nRho, 1.0e8),
               {static_cast<hsize_t>(nRho)});
  write_double(f, "/ThermoState/Temperature", ramp(nT, 1.0e10),
               {static_cast<hsize_t>(nT)});
  write_double(f, "/ThermoState/Electron Fraction", ramp(nYe, 0.1),
               {static_cast<hsize_t>(nYe)});
  write_int_scalar(f, "/ThermoState/iRho", 1);
  write_int_scalar(f, "/ThermoState/iT", 2);
  write_int_scalar(f, "/ThermoState/iYe", 3);

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
  const char* kI[15] = {
      "iAlphaMassFraction",       "iElectronChemicalPotential",
      "iEntropyPerBaryon",        "iGamma1",
      "iHeavyBindingEnergy",      "iHeavyChargeNumber",
      "iHeavyMassFraction",       "iHeavyMassNumber",
      "iInternalEnergyDensity",   "iNeutronChemicalPotential",
      "iNeutronMassFraction",     "iPressure",
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

// ---- EmAb synthetic writer (/EmAb: 1D Offsets{2}, 4D value arrays). --------

void write_synthetic_emab(const std::string& path) {
  H5::H5File f(path, H5F_ACC_TRUNC);
  write_common(f);
  const std::string g = "/EmAb";
  f.createGroup(g);
  write_int_scalar(f, g + "/nOpacities", 2);
  write_double(f, g + "/Offsets", {1.0, 2.0}, {2});  // 1D [nOpacities]
  // 4D value arrays, C-order dims (nYe,nT,nRho,nE) -> Fortran (nE,nRho,nT,nYe).
  const std::vector<hsize_t> d4 = {kNYe, kNT, kNRho, kNE};
  const std::size_t n4 = static_cast<std::size_t>(kNYe) * kNT * kNRho * kNE;
  write_double(f, g + "/Electron Neutrino", ramp(n4, 3.0), d4);
  write_double(f, g + "/Electron Antineutrino", ramp(n4, 7.0), d4);
}

// ---- Iso synthetic writer (/Scat_Iso_Kernels: 2D Offsets{2,2}, 5D arrays). -

void write_synthetic_iso(const std::string& path) {
  H5::H5File f(path, H5F_ACC_TRUNC);
  write_common(f);
  const std::string g = "/Scat_Iso_Kernels";
  f.createGroup(g);
  write_int_scalar(f, g + "/nOpacities", 2);
  write_int_scalar(f, g + "/nMoments", 2);
  write_double(f, g + "/Offsets", {1.0, 2.0, 3.0, 4.0}, {2, 2});  // 2D
  // 5D value arrays, C-order (nYe,nT,nRho,nMom,nE) -> Fortran (nE,nMom,nRho,nT,nYe)
  const int nMom = 2;
  const std::vector<hsize_t> d5 = {kNYe, kNT, kNRho,
                                   static_cast<hsize_t>(nMom), kNE};
  const std::size_t n5 =
      static_cast<std::size_t>(kNYe) * kNT * kNRho * nMom * kNE;
  write_double(f, g + "/Electron Neutrino", ramp(n5, 5.0), d5);
  write_double(f, g + "/Electron Antineutrino", ramp(n5, 9.0), d5);
}

// ---- NES / Pair synthetic writer (2D Offsets{4,1}, 5D Kernels, /EtaGrid). --

void write_synthetic_nes_pair(const std::string& path, const std::string& group,
                              bool withNPS) {
  H5::H5File f(path, H5F_ACC_TRUNC);
  write_common(f);
  write_grid(f, "/EtaGrid", "Eta", 1, ramp(kNYe, 2.0));
  f.createGroup(group);
  write_int_scalar(f, group + "/nOpacities", 1);
  write_int_scalar(f, group + "/nMoments", 4);
  write_double(f, group + "/Offsets", {1.0, 2.0, 3.0, 4.0}, {4, 1});  // 2D
  // 5D Kernels, C-order (nEta,nT,nMom,nE,nEp) -> Fortran (nEp,nE,nMom,nT,nEta).
  const int nMom = 4;
  const std::vector<hsize_t> d5 = {kNYe, kNT, static_cast<hsize_t>(nMom),
                                   kNE, kNE};
  const std::size_t n5 =
      static_cast<std::size_t>(kNYe) * kNT * nMom * kNE * kNE;
  write_double(f, group + "/Kernels", ramp(n5, 4.0), d5);
  if (withNPS) write_int_scalar(f, group + "/NPS", 1);
}

// ---- Brem synthetic writer (2D Offsets{1,1}, 5D S_sigma, no /EtaGrid). -----

void write_synthetic_brem(const std::string& path) {
  H5::H5File f(path, H5F_ACC_TRUNC);
  write_common(f);
  const std::string g = "/Scat_Brem_Kernels";
  f.createGroup(g);
  write_int_scalar(f, g + "/nOpacities", 1);
  write_int_scalar(f, g + "/nMoments", 1);
  write_double(f, g + "/Offsets", {1.0}, {1, 1});  // 2D [1,1]
  // 5D S_sigma, C-order (nT,nRho,nMom,nE,nEp) -> Fortran (nEp,nE,nMom,nRho,nT).
  const int nMom = 1;
  const std::vector<hsize_t> d5 = {kNT, kNRho, static_cast<hsize_t>(nMom),
                                   kNE, kNE};
  const std::size_t n5 =
      static_cast<std::size_t>(kNT) * kNRho * nMom * kNE * kNE;
  write_double(f, g + "/S_sigma", ramp(n5, 6.0), d5);
}

// ---- Cross-rank comparators (exact tier). ---------------------------------

// Broadcast the root's digest and AND the per-rank equality across all ranks:
// returns true iff every rank's digest equals the root's (all-reduced, so the
// same value lands on every rank).
bool ranks_agree(std::uint64_t localDigest) {
  long myDig = static_cast<long>(localDigest & 0x7fffffffffffffffLL);
  long rootDig = myDig;
  pd::Bcast(&rootDig, 1, pd::IOProcessorNumber());
  bool ok = (myDig == rootDig);
  pd::ReduceBoolAnd(ok);
  return ok;
}

// As above for a flat double result vector: bitwise compare against the root's
// broadcast copy (broadcasts preserve bits, so this is the exact-equality tier).
bool ranks_agree(const std::vector<double>& mine) {
  std::vector<double> root = mine;
  pd::Bcast(root.data(), root.size(), pd::IOProcessorNumber());
  bool ok = (mine.size() == root.size()) &&
            (std::memcmp(mine.data(), root.data(),
                         mine.size() * sizeof(double)) == 0);
  pd::ReduceBoolAnd(ok);
  return ok;
}

// ---- Production-table presence guard (mirrors test_production_tables.cpp). --

bool file_exists(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return f.good();
}

std::string resolve(const std::string& root, const char* base) {
  std::string a = root + "/use_for_production/" + base;
  if (file_exists(a)) return a;
  std::string b = root + "/" + base;
  if (file_exists(b)) return b;
  return std::string();
}

// ---- Cell (a): table-load consistency across ranks. ------------------------

// Root writes a synthetic .h5, every rank reads it via the live reader, and the
// loaded struct's digest is compared across ranks. Returns true iff consistent.
template <typename Writer, typename Reader>
bool load_consistent_synth(const std::string& path, Writer write, Reader read) {
  if (pd::IOProcessor()) write(path);
  pd::Barrier();
  auto t = read(path);              // collective: only root opens, then broadcast
  const bool ok = ranks_agree(digest(t));
  pd::Barrier();                    // all ranks done reading before root removes
  if (pd::IOProcessor()) std::remove(path.c_str());
  return ok;
}

int test_load() {
  bool all = true;

  all &= load_consistent_synth(
      "test_rc_synth_eos.h5",
      [](const std::string& p) { write_synthetic_eos(p); },
      [](const std::string& p) { return wli::io::read_eos_table(p); });
  all &= load_consistent_synth(
      "test_rc_synth_emab.h5",
      [](const std::string& p) { write_synthetic_emab(p); },
      [](const std::string& p) { return wli::io::read_emab_table(p); });
  all &= load_consistent_synth(
      "test_rc_synth_iso.h5",
      [](const std::string& p) { write_synthetic_iso(p); },
      [](const std::string& p) { return wli::io::read_scat_iso_table(p); });
  all &= load_consistent_synth(
      "test_rc_synth_nes.h5",
      [](const std::string& p) {
        write_synthetic_nes_pair(p, "/Scat_NES_Kernels", true);
      },
      [](const std::string& p) { return wli::io::read_scat_nes_table(p); });
  all &= load_consistent_synth(
      "test_rc_synth_pair.h5",
      [](const std::string& p) {
        write_synthetic_nes_pair(p, "/Scat_Pair_Kernels", false);
      },
      [](const std::string& p) { return wli::io::read_scat_pair_table(p); });
  all &= load_consistent_synth(
      "test_rc_synth_brem.h5",
      [](const std::string& p) { write_synthetic_brem(p); },
      [](const std::string& p) { return wli::io::read_scat_brem_table(p); });

  // Optional production-table pass (spec:183 "and, where present, production").
  // The synthetic pass above is the always-on gate; this only adds real-table
  // anchoring when $WL_TABLES_ROOT resolves a table. The env is identical on
  // every rank, so resolve() picks the same path and the reader stays collective.
  int production = 0;
  const char* rootEnv = std::getenv("WL_TABLES_ROOT");
  if (rootEnv != nullptr && rootEnv[0] != '\0') {
    const std::string root(rootEnv);
    struct Prod {
      const char* base;
      std::uint64_t (*dig)(const std::string&);
    };
    // Each reader wrapped so a present table is digested; absent -> skipped.
    auto eos = +[](const std::string& p) {
      return digest(wli::io::read_eos_table(p));
    };
    auto emab = +[](const std::string& p) {
      return digest(wli::io::read_emab_table(p));
    };
    auto iso = +[](const std::string& p) {
      return digest(wli::io::read_scat_iso_table(p));
    };
    auto nes = +[](const std::string& p) {
      return digest(wli::io::read_scat_nes_table(p));
    };
    auto pair = +[](const std::string& p) {
      return digest(wli::io::read_scat_pair_table(p));
    };
    auto brem = +[](const std::string& p) {
      return digest(wli::io::read_scat_brem_table(p));
    };
    const Prod prods[6] = {
        {"wl-EOS-SFHo-15-25-50.h5", eos},
        {"wl-Op-SFHo-15-25-50-E40-EmAb.h5", emab},
        {"wl-Op-SFHo-15-25-50-E40-Iso.h5", iso},
        {"wl-Op-SFHo-15-25-50-E40-NES.h5", nes},
        {"wl-Op-SFHo-15-25-50-E40-Pair.h5", pair},
        {"wl-Op-SFHo-15-25-50-E40-Brem.h5", brem}};
    for (const auto& pr : prods) {
      const std::string path = resolve(root, pr.base);
      if (path.empty()) continue;
      ++production;
      all &= ranks_agree(pr.dig(path));  // reader is collective on every rank
    }
  }

  int rc = 0;
  if (pd::IOProcessor()) {
    if (!all) {
      std::fprintf(stderr,
                   "FAIL load: a loaded table's digest differs across ranks\n");
      rc = 1;
    } else {
      std::printf(
          "PASS rank_consistency load: %d ranks, 6 synthetic + %d production "
          "table(s), all loads byte-identical across ranks\n",
          pd::NProcs(), production);
    }
  }
  return rc;
}

// ---- Cell (b): result consistency for the 8 public entry points. -----------

// Build small deterministic synthetic tables (identical on every rank) and
// evaluate the 8 entry points at a fixed interior query, collecting every
// double result into `out`.
void compute_entry_point_results(std::vector<double>& out) {
  out.clear();

  // --- EOS 3D grids + affine-in-log sub-table (entry points 1-3). ---
  const int nD = 4, nT = 3, nY = 3;
  const Real Ds[nD] = {1.0e3, 5.0e5, 2.0e8, 6.0e11};
  const Real Ts[nT] = {1.0e9, 3.0e10, 9.0e11};
  const Real Ys[nY] = {0.05, 0.30, 0.55};
  const Real kA = 0.7, kB = 1.3, kC = -0.9, kEc = 2.1, kOS = 3.5;
  auto affine = [&](Real D, Real T, Real Y) {
    return kA + kB * std::log10(D) + kC * std::log10(T) + kEc * Y;
  };
  std::vector<Real> eos(static_cast<std::size_t>(nD) * nT * nY);
  for (int iY = 0; iY < nY; ++iY)
    for (int iT = 0; iT < nT; ++iT)
      for (int iD = 0; iD < nD; ++iD)
        eos[static_cast<std::size_t>(iD) + nD * (iT + nT * iY)] =
            affine(Ds[iD], Ts[iT], Ys[iY]);
  const Real qD = 7.3e6, qT = 1.7e10, qY = 0.22;

  // 1. EOS evaluate.
  out.push_back(wli::EosInterpolateSingleVariable3DPoint(
      qD, qT, qY, Ds, nD, Ts, nT, Ys, nY, kOS, eos.data()));

  // 2. EOS evaluate + differentiate (value + 3 partials).
  const wli::EosPointDeriv der =
      wli::EosInterpolateDifferentiateSingleVariable3DPoint(
          qD, qT, qY, Ds, nD, Ts, nT, Ys, nY, kOS, eos.data());
  out.push_back(der.value);
  out.push_back(der.dDrho);
  out.push_back(der.dDT);
  out.push_back(der.dDY);

  // 3. EOS inversion (DEY, no guess). Es strictly increasing in T so the
  // full-range bracket is guaranteed; bounds initialized so the check is not
  // the vacuous uninitialized-code-10 path.
  std::vector<Real> Es(static_cast<std::size_t>(nD) * nT * nY);
  for (int iY = 0; iY < nY; ++iY)
    for (int iT = 0; iT < nT; ++iT)
      for (int iD = 0; iD < nD; ++iD)
        Es[static_cast<std::size_t>(iD) + nD * (iT + nT * iY)] =
            1.0 + 0.3 * static_cast<double>(iD) + 2.0 * static_cast<double>(iT) +
            0.1 * static_cast<double>(iY);
  wli::EosInversionBounds bnds;
  bnds.initialized = true;
  bnds.MinD = Ds[0];      bnds.MaxD = Ds[nD - 1];
  bnds.MinY = Ys[0];      bnds.MaxY = Ys[nY - 1];
  bnds.MinX = -1.0e30;    bnds.MaxX = 1.0e30;  // wide X bounds: exercise inversion
  const Real targetE = wli::recover(3.5, kOS);  // an interior dependent value
  const wli::EosInversionResult inv = wli::ComputeTemperatureWith_DEY_NoGuess(
      qD, targetE, qY, Ds, nD, Ts, nT, Ys, nY, kOS, Es.data(), bnds);
  out.push_back(inv.T);
  out.push_back(static_cast<double>(inv.Error));

  // --- Opacity grids (already-LOG10'd axes for the opacity kernels). ---
  const int nE = 4, nRho = 4, nTo = 3, nYo = 3;
  const Real LogEs[nE] = {0.0, 0.5, 1.0, 1.6};
  const Real LogDs[nRho] = {3.0, 5.0, 8.0, 11.0};
  const Real LogTs[nTo] = {9.0, 10.5, 12.0};
  const Real Yso[nYo] = {0.05, 0.30, 0.55};
  const Real qLogE = 0.7, qLogD = 6.4, qLogT = 10.2, qYe = 0.22, oOS = 1.25;

  // 4. EmAb 4D evaluate.
  std::vector<Real> emab(static_cast<std::size_t>(nE) * nRho * nTo * nYo);
  for (std::size_t k = 0; k < emab.size(); ++k) emab[k] = 0.1 * (k + 1);
  out.push_back(wli::EmAbInterpolateSingleVariable4DPoint(
      qLogE, qLogD, qLogT, qYe, LogEs, nE, LogDs, nRho, LogTs, nTo, Yso, nYo,
      oOS, emab.data()));

  // 5. Iso 5D-slice evaluate (fixed moment via the strided slice + IsoOffset).
  const int nMom = 2;
  std::vector<Real> iso(static_cast<std::size_t>(nE) * nMom * nRho * nTo * nYo);
  for (std::size_t k = 0; k < iso.size(); ++k) iso[k] = 0.2 * (k + 1);
  const Real isoOff[4] = {1.0, 2.0, 3.0, 4.0};  // 2D [nOpacities=2, nMoments=2]
  const Real osIso = wli::IsoOffset(isoOff, 2, nMom, /*iSpecies=*/0, /*iMom=*/1);
  out.push_back(wli::IsoInterpolateSingleVariable5DPoint(
      qLogE, qLogD, qLogT, qYe, LogEs, nE, LogDs, nRho, LogTs, nTo, Yso, nYo,
      /*iMom=*/1, nMom, osIso, iso.data()));

  // 6/7. NES + Pair aligned bilinear (same kernel; two representative calls).
  const int nEp = nE, nEeta = nYo, nMomK = 4;
  std::vector<Real> nespair(
      static_cast<std::size_t>(nEp) * nE * nMomK * nTo * nEeta);
  for (std::size_t k = 0; k < nespair.size(); ++k) nespair[k] = 0.05 * (k + 1);
  const Real LogXs[nYo] = {-1.0, 0.0, 1.0};  // log10(eta) grid
  const Real qLogEta = 0.3;
  out.push_back(wli::NESPairInterpolateSingleVariable2D2DAlignedPoint(
      qLogT, qLogEta, LogTs, nTo, LogXs, nEeta, /*iEp=*/1, /*iE=*/2, nEp, nE,
      /*kernel=*/0, nMomK, oOS, nespair.data()));  // NES row
  out.push_back(wli::NESPairInterpolateSingleVariable2D2DAlignedPoint(
      qLogT, qLogEta, LogTs, nTo, LogXs, nEeta, /*iEp=*/2, /*iE=*/1, nEp, nE,
      /*kernel=*/1, nMomK, oOS, nespair.data()));  // Pair row

  // 8. Brem summed aligned evaluate (fixed-weight effective-density decomp).
  const int nMomB = 1;
  std::vector<Real> brem(
      static_cast<std::size_t>(nEp) * nE * nMomB * nRho * nTo);
  for (std::size_t k = 0; k < brem.size(); ++k) brem[k] = 0.03 * (k + 1);
  const Real LogDb[3] = {5.0, 5.3, 5.15};
  const Real Alpha[3] = {1.0, 1.0, 28.0 / 3.0};
  out.push_back(wli::BremInterpolateSingleVariable2D2DAlignedSummedPoint(
      LogDb, Alpha, 3, qLogT, LogDs, nRho, LogTs, nTo, /*iEp=*/1, /*iE=*/2, nEp,
      nE, /*moment=*/0, nMomB, oOS, brem.data()));
}

int test_result() {
  std::vector<double> mine;
  compute_entry_point_results(mine);
  const bool ok = ranks_agree(mine);

  int rc = 0;
  if (pd::IOProcessor()) {
    if (!ok) {
      std::fprintf(stderr,
                   "FAIL result: an entry-point result differs across ranks\n");
      rc = 1;
    } else {
      std::printf(
          "PASS rank_consistency result: %d ranks, all 8 entry points return "
          "bitwise-identical doubles (%zu values)\n",
          pd::NProcs(), mine.size());
    }
  }
  return rc;
}

// ---- Cell (c): cross-rank-mismatch meta-test (positive control). -----------

int test_corrupt() {
  if (pd::NProcs() < 2) {
    std::printf(
        "SKIP rank_consistency corrupt: needs >= 2 ranks (no peer to diverge "
        "from at 1 rank)\n");
    return 77;  // CTest SKIP_RETURN_CODE
  }

  // Genuine spec-171 path: root writes, every rank loads via the collective
  // reader (root-read + broadcast). The corrupted rank still ran this real load.
  const std::string path = "test_rc_corrupt_eos.h5";
  if (pd::IOProcessor()) write_synthetic_eos(path);
  pd::Barrier();
  wli::io::HostEosTable t = wli::io::read_eos_table(path);
  pd::Barrier();
  if (pd::IOProcessor()) std::remove(path.c_str());

  // Baseline positive control: the un-corrupted comparator MUST report
  // consistent (else the meta-test is vacuous — a comparator that rejects
  // everything would also "catch" the injected divergence).
  const bool baseline_consistent = ranks_agree(digest(t));

  // Corrupt exactly ONE non-root rank's own loaded copy, strictly AFTER the real
  // broadcast, then re-run the SAME comparator. Root's broadcast digest stays
  // clean, so the divergence must surface as an inconsistency.
  const int victim = (pd::IOProcessorNumber() + 1) % pd::NProcs();  // != root
  if (pd::MyProc() == victim && !t.dv.empty() && !t.dv[0].values.empty()) {
    t.dv[0].values[0] += 1.0;  // flip one broadcast array element
  }
  const bool corrupted_consistent = ranks_agree(digest(t));

  // The meta-test PASSES iff baseline is consistent AND the corrupted run is
  // detected as inconsistent. Both flags are all-reduced, so every rank holds
  // the same values; root decides the exit code.
  int rc = 0;
  if (pd::IOProcessor()) {
    if (!baseline_consistent) {
      std::fprintf(stderr,
                   "FAIL corrupt: baseline (un-corrupted) load reported "
                   "inconsistent — comparator or fixture is broken\n");
      rc = 1;
    }
    if (corrupted_consistent) {
      std::fprintf(stderr,
                   "FAIL corrupt: a one-rank corruption was NOT detected — the "
                   "cross-rank comparator is not real (print-only)\n");
      rc = 1;
    }
    if (rc == 0) {
      std::printf(
          "PASS rank_consistency corrupt: %d ranks, baseline consistent and a "
          "one-rank divergence correctly flagged by the comparator\n",
          pd::NProcs());
    }
  }
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = (argc > 1) ? argv[1] : "load";

  // build_parm_parse=false: the mode selector is not an AMReX inputs file.
  amrex::Initialize(argc, argv, false);
  H5::Exception::dontPrint();

  int rc = 0;
  if (mode == "result") {
    rc = test_result();
  } else if (mode == "corrupt") {
    rc = test_corrupt();
  } else {
    rc = test_load();
  }

  // Propagate the root's result so ctest sees a consistent code on every rank.
  amrex::ParallelDescriptor::Bcast(&rc, 1,
                                   amrex::ParallelDescriptor::IOProcessorNumber());
  amrex::Finalize();
  return rc;
}

#else  // !AMREX_USE_MPI

int main() {
  std::printf(
      "SKIP rank_consistency: not an MPI build (AMReX_MPI=OFF) — the cross-rank "
      "load/result consistency and the corruption meta-test require >= 2 ranks; "
      "the 1-rank serial reads are exercised by the rest of the suite.\n");
  return 77;  // CTest SKIP_RETURN_CODE
}

#endif  // AMREX_USE_MPI
