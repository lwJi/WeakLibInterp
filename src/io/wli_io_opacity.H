#pragma once

// WeakLibInterp HDF5 opacity-table reader + shared in-memory host structs.
//
// Single source of truth for loading the five weaklib opacity production tables
// (wl-Op-SFHo-15-25-50-E40-{EmAb,Iso,NES,Pair,Brem}.h5), one channel per file,
// into host memory in the log-stored, column-major layout the opacity kernels
// (src/opacity/wli_opacity.H) and the device-residency upload (src/core/wli_table.H)
// consume, with NO adapter layer. The reader contract (group/dataset names,
// Fortran column-major shapes, dtypes, the load-bearing 1D-vs-2D Offsets
// dimensionality, names-before-arrays read order, the /EmAb legacy fallback,
// the optional geometric /EnergyGrid extras) is pinned by
// specs/table-format-and-io.md:85-182 and anchored to the committed structural
// snapshots specs/fixtures/wl-Op-SFHo-15-25-50-E40-*.h5ls.
//
// Mirrors wli_io_eos.H: host structs (flat log-stored value buffers + recorded
// integer extents + separate additive offsets) + free read_* decls + a schema
// namespace of expected names/shapes for the structural self-test. Values stay
// log-stored; the additive offset is kept separate (physical = 10**stored -
// offset applied during/after interpolation, spec:188). The shared /ThermoState
// axes reuse wli::io::HostAxis (wli_io_eos.H) verbatim.

#include <array>
#include <string>
#include <vector>

#include "wli_io_eos.H"  // wli::io::HostAxis (reused as-is for /ThermoState)
#include "wli_real.H"    // wli::Real (pinned to double)
#include "wli_table.H"   // wli::AxisKind (Log/Linear), reused — no new enum

namespace wli {
namespace io {

// ----------------------------------------------------------------------------
// Shared in-memory host structs.
// ----------------------------------------------------------------------------

// A single-axis grid descriptor (/EnergyGrid or /EtaGrid), written by weaklib
// WriteGridHDF. `points` is the node coordinate array (/Values). The geometric
// extras (Zoom/Edge/Width/minEdge/maxEdge/minWidth) are present only on a zoomed
// /EnergyGrid (spec:97-102); `geometric` is true iff Zoom was present and > 0.
struct HostOpacityGrid {
  std::string name;                    // /Name
  wli::AxisKind kind = wli::AxisKind::Linear;  // from /LogInterp
  std::vector<double> points;          // /Values node coordinates

  bool geometric = false;              // true iff Zoom present and > 0
  double zoom = 0.0;                    // /Zoom
  std::vector<double> edge;            // /Edge  {nPoints+1}
  std::vector<double> width;           // /Width {nPoints}
  double minEdge = 0.0;
  double maxEdge = 0.0;
  double minWidth = 0.0;
};

// The shared header every opacity file carries: /EnergyGrid + /ThermoState axes.
struct HostOpacityCommon {
  HostOpacityGrid energyGrid;                 // /EnergyGrid
  std::array<HostAxis, 3> axes{};             // /ThermoState (rho, T, Ye)
  std::array<int, 3> tsIndices{{-1, -1, -1}}; // iRho, iT, iYe (role -> slot)
};

// /EmAb — emission/absorption opacity, 4D (E, rho, T, Ye) per species.
struct HostEmAbTable {
  HostOpacityCommon common;
  int nOpacities = 0;                      // /EmAb/nOpacities (READ, not hardcoded)
  std::array<int, 4> nPoints{{0, 0, 0, 0}}; // Fortran (nE, nRho, nT, nYe)
  std::vector<std::string> speciesNames;   // "Electron Neutrino", "Electron Antineutrino"
  std::vector<std::vector<double>> values; // per species: flat log-stored buffer
  std::vector<double> offset;              // 1D [nOpacities] additive offsets
  bool hasEmAbParameters = false;          // /EmAb Parameters group present
  bool hasECTable = false;                 // /EC_table group present
  bool usedLegacyGroup = false;            // opened /EmAb_CorrectedAbsorption fallback
};

// /Scat_Iso_Kernels — isoenergetic scattering, 5D (E, moment, rho, T, Ye).
struct HostScatIsoTable {
  HostOpacityCommon common;
  int nOpacities = 0;                      // 2
  int nMoments = 0;                        // 2
  std::array<int, 5> nPoints{{0, 0, 0, 0, 0}}; // Fortran (nE, nMom, nRho, nT, nYe)
  std::vector<std::string> speciesNames;   // "Electron Neutrino", "Electron Antineutrino"
  std::vector<std::vector<double>> values; // per species: flat log-stored buffer
  // 2D Offsets[nOpacities, nMoments], flat species-major/moment-minor to match
  // the wli::IsoOffset convention (flat = iSpecies + nOpacities*iMom).
  std::vector<double> offset;
};

// /Scat_NES_Kernels and /Scat_Pair_Kernels — 5D (E', E, kernel-index, T, eta).
struct HostScatNESPairTable {
  HostOpacityCommon common;
  HostOpacityGrid etaGrid;                 // /EtaGrid (NES & Pair only)
  int nOpacities = 0;                      // 1
  int nMoments = 0;                        // 4 (kernel-component count)
  std::array<int, 5> nPoints{{0, 0, 0, 0, 0}}; // Fortran (nEp, nE, nMom, nT, nEta)
  std::vector<double> kernels;             // /Kernels flat log-stored buffer
  std::vector<double> offset;              // 2D [nOpacities, nMoments] flat
  bool hasNPS = false;                     // /Scat_NES_Kernels/NPS (NES only)
};

// /Scat_Brem_Kernels — 5D (E', E, moment, rho, T). No /EtaGrid.
struct HostScatBremTable {
  HostOpacityCommon common;
  int nOpacities = 0;                      // 1
  int nMoments = 0;                        // 1
  std::array<int, 5> nPoints{{0, 0, 0, 0, 0}}; // Fortran (nEp, nE, nMom, nRho, nT)
  std::vector<double> sSigma;              // /S_sigma flat log-stored buffer
  std::vector<double> offset;              // 2D [nOpacities, nMoments] flat
};

// ----------------------------------------------------------------------------
// The readers (one per channel file). Each reads the shared /EnergyGrid +
// /ThermoState, then its channel group. Follows names-before-arrays order and
// reads nOpacities/nMoments (never hardcodes them). Throws H5::Exception /
// std::runtime_error on a malformed/absent file. These are the full live-table
// paths; they require a real production table present (guard on WL_TABLES_ROOT
// in callers). The structural self-test does NOT exercise these.
// ----------------------------------------------------------------------------

HostEmAbTable read_emab_table(const std::string& path);
HostScatIsoTable read_scat_iso_table(const std::string& path);
HostScatNESPairTable read_scat_nes_table(const std::string& path);
HostScatNESPairTable read_scat_pair_table(const std::string& path);
HostScatBremTable read_scat_brem_table(const std::string& path);

// ----------------------------------------------------------------------------
// Expected schema (reader expectations, exposed for the structural self-test).
// The fixed dataset names, ranks, and h5ls (C-order) shapes the reader depends
// on. Value-array names are hardcoded literals (NOT read from a Names dataset;
// confirmed absent under every channel group in all five fixtures). Offset
// ranks are the load-bearing 1D-vs-2D distinction (spec:162).
// ----------------------------------------------------------------------------

namespace schema {

// Shared /ThermoState, byte-identical to the EOS file's (spec:85-87).
inline constexpr const char* kThermoAxisNames[3] = {
    "Density", "Temperature", "Electron Fraction"};
inline constexpr int kThermoAxisExtent[3] = {185, 81, 30};  // nRho, nT, nYe

// Shared /EnergyGrid base datasets (present on every channel). nPoints = 40.
inline constexpr int kNEnergy = 40;

// /EmAb (EmAb file). 1D Offsets{2}; value arrays h5ls {30,81,185,40}.
namespace emab {
inline constexpr const char* kGroup = "/EmAb";
inline constexpr const char* kLegacyGroup = "/EmAb_CorrectedAbsorption";
inline constexpr int kNOpacities = 2;
inline constexpr const char* kSpecies[2] = {
    "Electron Neutrino", "Electron Antineutrino"};
inline constexpr int kValueShape[4] = {30, 81, 185, 40};  // (nE,nRho,nT,nYe) reversed
inline constexpr int kOffsetShape[1] = {2};               // 1D [nOpacities]
}  // namespace emab

// /Scat_Iso_Kernels (Iso file). 2D Offsets{2,2}; value arrays {30,81,185,2,40}.
// The Iso /EnergyGrid is the only geometric one (Zoom/Edge/Width extras).
namespace iso {
inline constexpr const char* kGroup = "/Scat_Iso_Kernels";
inline constexpr int kNOpacities = 2;
inline constexpr int kNMoments = 2;
inline constexpr const char* kSpecies[2] = {
    "Electron Neutrino", "Electron Antineutrino"};
inline constexpr int kValueShape[5] = {30, 81, 185, 2, 40};  // (nE,nMom,nRho,nT,nYe)
inline constexpr int kOffsetShape[2] = {2, 2};               // 2D [nOp,nMom]
inline constexpr int kEnergyEdgeExtent = 41;                 // /EnergyGrid/Edge
inline constexpr int kEnergyWidthExtent = 40;                // /EnergyGrid/Width
}  // namespace iso

// /Scat_NES_Kernels (NES file). 2D Offsets{4,1}; Kernels {120,81,4,40,40}.
// Carries /EtaGrid (Values{120}) and an extra integer NPS flag.
namespace nes {
inline constexpr const char* kGroup = "/Scat_NES_Kernels";
inline constexpr const char* kValueName = "Kernels";
inline constexpr int kNOpacities = 1;
inline constexpr int kNMoments = 4;
inline constexpr int kValueShape[5] = {120, 81, 4, 40, 40};  // (nEp,nE,nMom,nT,nEta)
inline constexpr int kOffsetShape[2] = {4, 1};               // 2D [nOp,nMom]
inline constexpr int kEtaExtent = 120;                       // /EtaGrid/Values
}  // namespace nes

// /Scat_Pair_Kernels (Pair file). As NES but with no NPS flag.
namespace pair {
inline constexpr const char* kGroup = "/Scat_Pair_Kernels";
inline constexpr const char* kValueName = "Kernels";
inline constexpr int kNOpacities = 1;
inline constexpr int kNMoments = 4;
inline constexpr int kValueShape[5] = {120, 81, 4, 40, 40};  // (nEp,nE,nMom,nT,nEta)
inline constexpr int kOffsetShape[2] = {4, 1};               // 2D [nOp,nMom]
inline constexpr int kEtaExtent = 120;                       // /EtaGrid/Values
}  // namespace pair

// /Scat_Brem_Kernels (Brem file). 2D Offsets{1,1}; S_sigma {81,185,1,40,40}
// (literal S_sigma, not Kernels; trailing axes (rho,T)). No /EtaGrid.
namespace brem {
inline constexpr const char* kGroup = "/Scat_Brem_Kernels";
inline constexpr const char* kValueName = "S_sigma";
inline constexpr int kNOpacities = 1;
inline constexpr int kNMoments = 1;
inline constexpr int kValueShape[5] = {81, 185, 1, 40, 40};  // (nEp,nE,nMom,nRho,nT)
inline constexpr int kOffsetShape[2] = {1, 1};               // 2D [nOp,nMom]
}  // namespace brem

}  // namespace schema

}  // namespace io
}  // namespace wli
