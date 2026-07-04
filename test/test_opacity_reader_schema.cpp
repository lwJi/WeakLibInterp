// Structural-conformance self-test for the opacity HDF5 reader (five channels).
//
// Runs WITHOUT any live .h5 (spec check #1, specs/table-format-and-io.md:171-175).
// It parses the five committed structural snapshots
// specs/fixtures/wl-Op-SFHo-15-25-50-E40-{EmAb,Iso,NES,Pair,Brem}.h5ls into
// path -> shape maps, then asserts the schema the reader depends on (the fixed
// names + ranks/shapes exposed as static data in src/io/wli_io_opacity.H)
// matches each snapshot EXACTLY. The single load-bearing distinction is the
// 1D-vs-2D Offsets dimensionality (spec:162), asserted explicitly per channel.
//
// Each fixture path is injected at compile time via WLI_{EMAB,ISO,NES,PAIR,BREM}_H5LS.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "h5ls_snapshot.H"
#include "wli_io_opacity.H"

namespace {

using h5ls::Entry;
using h5ls::check;
using h5ls::expect_dataset;
using h5ls::expect_group;
using h5ls::expect_offsets;

using Snapshot = std::map<std::string, Entry>;

// constexpr int[N] -> std::vector<int> (schema shapes come straight from the
// reader's schema:: constants, never inline literals).
template <std::size_t N>
std::vector<int> vec(const int (&a)[N]) {
  return std::vector<int>(a, a + N);
}

// The shared /ThermoState group, byte-identical to the EOS file's (spec:85-87).
void check_thermostate(const Snapshot& m) {
  namespace S = wli::io::schema;
  expect_group(m, "/ThermoState");
  expect_dataset(m, "/ThermoState/Density", {185});
  expect_dataset(m, "/ThermoState/Temperature", {81});
  expect_dataset(m, "/ThermoState/Electron Fraction", {30});
  for (int a = 0; a < 3; ++a) {
    expect_dataset(m, std::string("/ThermoState/") + S::kThermoAxisNames[a],
                   {S::kThermoAxisExtent[a]});
  }
  expect_dataset(m, "/ThermoState/Names", {3});
  expect_dataset(m, "/ThermoState/Units", {3});
  expect_dataset(m, "/ThermoState/Dimensions", {3});
  expect_dataset(m, "/ThermoState/LogInterp", {3});
  expect_dataset(m, "/ThermoState/iRho", {1});
  expect_dataset(m, "/ThermoState/iT", {1});
  expect_dataset(m, "/ThermoState/iYe", {1});
}

// The shared /EnergyGrid base datasets (present on every channel).
void check_energygrid_base(const Snapshot& m) {
  namespace S = wli::io::schema;
  expect_group(m, "/EnergyGrid");
  expect_dataset(m, "/EnergyGrid/Values", {S::kNEnergy});
  expect_dataset(m, "/EnergyGrid/nPoints", {1});
  expect_dataset(m, "/EnergyGrid/LogInterp", {1});
  expect_dataset(m, "/EnergyGrid/Name", {1});
  expect_dataset(m, "/EnergyGrid/Unit", {1});
}

void check_emab(const Snapshot& m) {
  namespace E = wli::io::schema::emab;
  check_thermostate(m);
  check_energygrid_base(m);
  expect_group(m, E::kGroup);
  for (const char* s : E::kSpecies) {
    expect_dataset(m, std::string(E::kGroup) + "/" + s, vec(E::kValueShape));
  }
  // 1D Offsets{2} — EmAb offsets are rank-1 (mirror /DependentVariables/Offsets).
  expect_offsets(m, std::string(E::kGroup) + "/Offsets", vec(E::kOffsetShape));
  expect_dataset(m, std::string(E::kGroup) + "/nOpacities", {1});
  // Optional groups present in the committed table (spec:114).
  expect_group(m, "/EmAb Parameters");
  expect_group(m, "/EC_table");
}

void check_iso(const Snapshot& m) {
  namespace I = wli::io::schema::iso;
  check_thermostate(m);
  check_energygrid_base(m);
  expect_group(m, I::kGroup);
  for (const char* s : I::kSpecies) {
    expect_dataset(m, std::string(I::kGroup) + "/" + s, vec(I::kValueShape));
  }
  // 2D Offsets{2,2} — scattering offsets are rank-2.
  expect_offsets(m, std::string(I::kGroup) + "/Offsets", vec(I::kOffsetShape));
  expect_dataset(m, std::string(I::kGroup) + "/nOpacities", {1});
  expect_dataset(m, std::string(I::kGroup) + "/nMoments", {1});
  // Iso-only geometric /EnergyGrid extras (present iff the grid was zoomed).
  expect_dataset(m, "/EnergyGrid/Zoom", {1});
  expect_dataset(m, "/EnergyGrid/Edge", {I::kEnergyEdgeExtent});
  expect_dataset(m, "/EnergyGrid/Width", {I::kEnergyWidthExtent});
  expect_dataset(m, "/EnergyGrid/minEdge", {1});
  expect_dataset(m, "/EnergyGrid/maxEdge", {1});
  expect_dataset(m, "/EnergyGrid/minWidth", {1});
}

// NES and Pair share a layout; NES additionally carries the integer NPS flag.
void check_nes_pair(const Snapshot& m, const char* group, const char* valueName,
                    const std::vector<int>& valueShape,
                    const std::vector<int>& offsetShape, int etaExtent,
                    bool expectNPS) {
  check_thermostate(m);
  check_energygrid_base(m);
  // /EtaGrid (NES & Pair only), never geometric.
  expect_group(m, "/EtaGrid");
  expect_dataset(m, "/EtaGrid/Values", {etaExtent});
  expect_dataset(m, "/EtaGrid/nPoints", {1});
  expect_dataset(m, "/EtaGrid/LogInterp", {1});
  expect_dataset(m, "/EtaGrid/Name", {1});
  expect_dataset(m, "/EtaGrid/Unit", {1});

  expect_group(m, group);
  expect_dataset(m, std::string(group) + "/" + valueName, valueShape);
  // 2D Offsets{4,1} — scattering offsets are rank-2.
  expect_offsets(m, std::string(group) + "/Offsets", offsetShape);
  expect_dataset(m, std::string(group) + "/nOpacities", {1});
  expect_dataset(m, std::string(group) + "/nMoments", {1});
  if (expectNPS) expect_dataset(m, std::string(group) + "/NPS", {1});
}

void check_brem(const Snapshot& m) {
  namespace B = wli::io::schema::brem;
  check_thermostate(m);
  check_energygrid_base(m);
  // Brem has no /EtaGrid.
  check(m.find("/EtaGrid") == m.end(), "Brem must NOT carry an /EtaGrid group");
  expect_group(m, B::kGroup);
  // Literal S_sigma (not Kernels); trailing axes (rho,T).
  expect_dataset(m, std::string(B::kGroup) + "/" + B::kValueName, vec(B::kValueShape));
  // 2D Offsets{1,1} — scattering offsets are rank-2.
  expect_offsets(m, std::string(B::kGroup) + "/Offsets", vec(B::kOffsetShape));
  expect_dataset(m, std::string(B::kGroup) + "/nOpacities", {1});
  expect_dataset(m, std::string(B::kGroup) + "/nMoments", {1});
}

Snapshot load_or_die(const char* path) {
  const Snapshot m = h5ls::load_snapshot(path);
  if (m.empty()) {
    std::fprintf(stderr, "FAIL: empty/unreadable snapshot %s\n", path);
  }
  return m;
}

}  // namespace

int main() {
  namespace SN = wli::io::schema::nes;
  namespace SP = wli::io::schema::pair;

  const Snapshot emab = load_or_die(WLI_EMAB_H5LS);
  const Snapshot iso = load_or_die(WLI_ISO_H5LS);
  const Snapshot nes = load_or_die(WLI_NES_H5LS);
  const Snapshot pair = load_or_die(WLI_PAIR_H5LS);
  const Snapshot brem = load_or_die(WLI_BREM_H5LS);
  if (emab.empty() || iso.empty() || nes.empty() || pair.empty() || brem.empty()) {
    std::fprintf(stderr, "FAIL: one or more snapshots empty/unreadable\n");
    return EXIT_FAILURE;
  }

  check_emab(emab);
  check_iso(iso);
  check_nes_pair(nes, SN::kGroup, SN::kValueName, vec(SN::kValueShape),
                 vec(SN::kOffsetShape), SN::kEtaExtent, /*expectNPS=*/true);
  check_nes_pair(pair, SP::kGroup, SP::kValueName, vec(SP::kValueShape),
                 vec(SP::kOffsetShape), SP::kEtaExtent, /*expectNPS=*/false);
  check_brem(brem);

  if (h5ls::g_failures != 0) {
    std::fprintf(stderr, "FAIL opacity_reader_schema: %d check(s) failed\n",
                 h5ls::g_failures);
    return EXIT_FAILURE;
  }
  std::printf(
      "PASS opacity_reader_schema: reader schema matches 5 snapshots "
      "(EmAb 1D Offsets{2}; Iso 2D{2,2}+geometric grid; NES/Pair 2D{4,1}; "
      "Brem 2D{1,1} S_sigma)\n");
  return EXIT_SUCCESS;
}
