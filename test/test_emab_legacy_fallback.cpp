// Always-on serial test: /EmAb_CorrectedAbsorption legacy-group fallback in
// read_emab_table (spec: specs/table-format-and-io.md:182, Verification #4).
//
// Writes a synthetic EmAb fixture whose channel data lives ONLY under the
// legacy group /EmAb_CorrectedAbsorption (no /EmAb), then loads it via the live
// reader and asserts (1) usedLegacyGroup == true and (2) read-back parity of the
// offsets and both 4D value arrays (byte-identical: the stored ramp is exactly
// what the reader returns). Serial, no SKIP, no MPI guard.
//
// The synthetic HDF5 writers are duplicated per-TU (matching
// test_rank_consistency.cpp) so the readers stay read-only per spec:21 — there
// is intentionally no shared writer header in src/io/.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <H5Cpp.h>

#include <AMReX.H>
#include <AMReX_ParallelDescriptor.H>

#include "wli_io_opacity.H"

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

// ---- Minimal HDF5 writers (test-only, verbatim from test_rank_consistency). --

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

std::vector<double> ramp(std::size_t n, double base) {
  std::vector<double> v(n);
  for (std::size_t k = 0; k < n; ++k) v[k] = base + 0.5 * static_cast<double>(k);
  return v;
}

constexpr int kNE = 3, kNRho = 3, kNT = 2, kNYe = 2;

void write_grid(H5::H5File& f, const std::string& group, const std::string& name,
                int logInterp, const std::vector<double>& values) {
  f.createGroup(group);
  write_strings(f, group + "/Name", {name});
  write_int(f, group + "/LogInterp", {logInterp}, {1});
  write_double(f, group + "/Values", values,
               {static_cast<hsize_t>(values.size())});
}

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

void write_common(H5::H5File& f) {
  write_grid(f, "/EnergyGrid", "Energy", 1, ramp(kNE, 1.0));
  write_thermo_state(f);
}

// ---- Legacy EmAb writer: channel data ONLY under /EmAb_CorrectedAbsorption. --
// Mirrors write_synthetic_emab but targets the legacy group and writes NOTHING
// under /EmAb, so read_emab_table must take the fallback path.
void write_synthetic_emab_legacy(const std::string& path) {
  H5::H5File f(path, H5F_ACC_TRUNC);
  write_common(f);
  const std::string g = wli::io::schema::emab::kLegacyGroup;
  f.createGroup(g);
  write_int_scalar(f, g + "/nOpacities", 2);
  write_double(f, g + "/Offsets", {1.0, 2.0}, {2});  // 1D [nOpacities]
  const std::vector<hsize_t> d4 = {kNYe, kNT, kNRho, kNE};
  const std::size_t n4 = static_cast<std::size_t>(kNYe) * kNT * kNRho * kNE;
  write_double(f, g + "/Electron Neutrino", ramp(n4, 3.0), d4);
  write_double(f, g + "/Electron Antineutrino", ramp(n4, 7.0), d4);
}

int run() {
  H5::Exception::dontPrint();
  const std::string path = "test_emab_legacy_synth.h5";
  const std::size_t n4 = static_cast<std::size_t>(kNYe) * kNT * kNRho * kNE;

  write_synthetic_emab_legacy(path);

  auto t = wli::io::read_emab_table(path);
  std::remove(path.c_str());

  // The load-bearing assertion: the reader took the legacy-group fallback.
  check(t.usedLegacyGroup == true,
        "usedLegacyGroup == true (opened /EmAb_CorrectedAbsorption)");

  check(t.nOpacities == 2, "nOpacities == 2");

  const std::vector<double> expOffset = {1.0, 2.0};
  check(t.offset == expOffset, "offset == {1.0, 2.0}");

  check(t.values.size() == 2, "values.size() == 2");
  if (t.values.size() == 2) {
    check(t.values[0] == ramp(n4, 3.0),
          "values[0] byte-identical to written ramp(base=3.0)");
    check(t.values[1] == ramp(n4, 7.0),
          "values[1] byte-identical to written ramp(base=7.0)");
  }

  const std::vector<std::string> expNames = {"Electron Neutrino",
                                             "Electron Antineutrino"};
  check(t.speciesNames == expNames,
        "speciesNames == {Electron Neutrino, Electron Antineutrino}");
  check(t.hasEmAbParameters == false, "hasEmAbParameters == false");
  check(t.hasECTable == false, "hasECTable == false");

  if (g_failures == 0) {
    std::printf("PASS emab_legacy_fallback: legacy-group fallback verified\n");
    return EXIT_SUCCESS;
  }
  std::fprintf(stderr, "FAILED emab_legacy_fallback: %d check(s)\n", g_failures);
  return EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {
  amrex::Initialize(argc, argv, false);
  const int rc = run();
  amrex::Finalize();
  return rc;
}
