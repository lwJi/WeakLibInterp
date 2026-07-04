// WeakLibInterp HDF5 opacity-table reader implementation (H5Cpp API).
//
// Implements the five live-table read paths pinned by
// specs/table-format-and-io.md:85-182, one per channel file. Each reads the
// shared /EnergyGrid and /ThermoState, then its one channel group. Read order
// follows names-before-arrays (the /ThermoState axis arrays and grid /Values are
// stored under name datasets); nOpacities/nMoments are READ from the file, never
// hardcoded (matching the EOS nVariables-read precedent). Multidimensional value
// arrays are read raw (no byte permutation): the on-disk h5ls C-order shape is
// already Fortran column-major with the first Fortran index fastest, matching
// wli::flat_index. Value arrays stay log-stored; the additive offset is kept
// separate (physical = 10**stored - offset). The offset buffers preserve the
// 1D-vs-2D dimensionality (spec:162): EmAb 1D[nOpacities]; every scattering
// channel 2D[nOpacities,nMoments] stored flat species-major/moment-minor to
// match the wli::IsoOffset consumer convention (flat = iSpecies + nOpacities*iMom).
//
// Value-array names are hardcoded literals (there is no Names dataset under any
// channel group in any of the five fixtures; weaklib hardcodes them):
// {"Electron Neutrino","Electron Antineutrino"} for EmAb & Iso, "Kernels" for
// NES & Pair, "S_sigma" for Brem.

#include "wli_io_opacity.H"

#include <stdexcept>

#include <H5Cpp.h>

#include "wli_io_hdf5_detail.H"  // shared read primitives (no ad-hoc copy)

namespace wli {
namespace io {

namespace {

// Reverse the h5ls (C-order) dims to the Fortran logical shape (first index
// fastest-varying) and copy into a fixed-size array. Throws if the rank differs.
template <std::size_t D>
std::array<int, D> fortran_shape(const H5::H5File& f, const std::string& name) {
  const std::vector<hsize_t> d = detail::dims(f, name);
  if (d.size() != D) {
    throw std::runtime_error("wli::io: unexpected rank for " + name);
  }
  std::array<int, D> out{};
  for (std::size_t i = 0; i < D; ++i) {
    out[i] = static_cast<int>(d[D - 1 - i]);  // reverse C-order -> Fortran
  }
  return out;
}

// Read a single-axis grid descriptor (/EnergyGrid or /EtaGrid). Reads Name +
// LogInterp + Values; then, gated on /Zoom presence-then-value (> 0), the
// optional geometric extras Edge/Width/minEdge/maxEdge/minWidth (spec:97-102).
HostOpacityGrid read_grid(const H5::H5File& f, const std::string& group) {
  HostOpacityGrid g;
  const std::vector<std::string> names = detail::read_strings(f, group + "/Name");
  g.name = names.empty() ? std::string() : names[0];
  const std::vector<int> li = detail::read_int(f, group + "/LogInterp");
  g.kind = (!li.empty() && li[0] != 0) ? wli::AxisKind::Log : wli::AxisKind::Linear;
  g.points = detail::read_double(f, group + "/Values");

  // Geometric extras: probe /Zoom first; only load Edge/Width if Zoom > 0.
  if (f.nameExists(group + "/Zoom")) {
    const std::vector<double> z = detail::read_double(f, group + "/Zoom");
    if (!z.empty() && z[0] > 0.0) {
      g.geometric = true;
      g.zoom = z[0];
      g.edge = detail::read_double(f, group + "/Edge");
      g.width = detail::read_double(f, group + "/Width");
      std::vector<double> tmp;
      if (detail::try_read_double(f, group + "/minEdge", tmp) && !tmp.empty())
        g.minEdge = tmp[0];
      if (detail::try_read_double(f, group + "/maxEdge", tmp) && !tmp.empty())
        g.maxEdge = tmp[0];
      if (detail::try_read_double(f, group + "/minWidth", tmp) && !tmp.empty())
        g.minWidth = tmp[0];
    }
  }
  return g;
}

// Read the shared header: /EnergyGrid + /ThermoState (axes + iRho/iT/iYe).
HostOpacityCommon read_common(const H5::H5File& f) {
  HostOpacityCommon c;
  c.energyGrid = read_grid(f, "/EnergyGrid");
  detail::read_thermo_state(f, c.axes, c.tsIndices);
  return c;
}

}  // namespace

HostEmAbTable read_emab_table(const std::string& path) {
  H5::Exception::dontPrint();
  H5::H5File file(path, H5F_ACC_RDONLY);

  HostEmAbTable t;
  t.common = read_common(file);

  // Legacy fallback: open /EmAb if present, else /EmAb_CorrectedAbsorption.
  std::string group = "/EmAb";
  if (!file.nameExists(group)) {
    group = "/EmAb_CorrectedAbsorption";
    t.usedLegacyGroup = true;
  }

  t.nOpacities = detail::read_int_scalar(file, group + "/nOpacities");
  t.offset = detail::read_double(file, group + "/Offsets");  // 1D [nOpacities]
  if (static_cast<int>(t.offset.size()) != t.nOpacities) {
    throw std::runtime_error("wli::io: EmAb Offsets is not 1D[nOpacities]");
  }

  // Value-array names are hardcoded (no Names dataset under the channel group).
  t.speciesNames = {"Electron Neutrino", "Electron Antineutrino"};
  t.nPoints = fortran_shape<4>(file, group + "/" + t.speciesNames[0]);
  t.values.reserve(t.speciesNames.size());
  for (const std::string& s : t.speciesNames) {
    t.values.push_back(detail::read_double(file, group + "/" + s));  // log-stored
  }

  t.hasEmAbParameters = file.nameExists("/EmAb Parameters");
  t.hasECTable = file.nameExists("/EC_table");
  return t;
}

HostScatIsoTable read_scat_iso_table(const std::string& path) {
  H5::Exception::dontPrint();
  H5::H5File file(path, H5F_ACC_RDONLY);

  HostScatIsoTable t;
  t.common = read_common(file);

  const std::string group = "/Scat_Iso_Kernels";
  t.nOpacities = detail::read_int_scalar(file, group + "/nOpacities");
  t.nMoments = detail::read_int_scalar(file, group + "/nMoments");

  // 2D Offsets[nOpacities, nMoments], flat species-major/moment-minor.
  t.offset = detail::read_double(file, group + "/Offsets");
  if (static_cast<int>(t.offset.size()) != t.nOpacities * t.nMoments) {
    throw std::runtime_error("wli::io: Iso Offsets is not 2D[nOpacities,nMoments]");
  }

  t.speciesNames = {"Electron Neutrino", "Electron Antineutrino"};
  t.nPoints = fortran_shape<5>(file, group + "/" + t.speciesNames[0]);
  t.values.reserve(t.speciesNames.size());
  for (const std::string& s : t.speciesNames) {
    t.values.push_back(detail::read_double(file, group + "/" + s));  // log-stored
  }
  return t;
}

// Shared NES/Pair reader (identical layout; caller passes the channel group).
namespace {
HostScatNESPairTable read_scat_nes_pair(const std::string& path,
                                        const std::string& group) {
  H5::Exception::dontPrint();
  H5::H5File file(path, H5F_ACC_RDONLY);

  HostScatNESPairTable t;
  t.common = read_common(file);
  t.etaGrid = read_grid(file, "/EtaGrid");

  t.nOpacities = detail::read_int_scalar(file, group + "/nOpacities");
  t.nMoments = detail::read_int_scalar(file, group + "/nMoments");

  t.offset = detail::read_double(file, group + "/Offsets");  // 2D [nOp,nMom] flat
  if (static_cast<int>(t.offset.size()) != t.nOpacities * t.nMoments) {
    throw std::runtime_error("wli::io: NES/Pair Offsets is not 2D[nOpacities,nMoments]");
  }

  t.nPoints = fortran_shape<5>(file, group + "/Kernels");
  t.kernels = detail::read_double(file, group + "/Kernels");  // log-stored
  t.hasNPS = file.nameExists(group + "/NPS");
  return t;
}
}  // namespace

HostScatNESPairTable read_scat_nes_table(const std::string& path) {
  return read_scat_nes_pair(path, "/Scat_NES_Kernels");
}

HostScatNESPairTable read_scat_pair_table(const std::string& path) {
  return read_scat_nes_pair(path, "/Scat_Pair_Kernels");
}

HostScatBremTable read_scat_brem_table(const std::string& path) {
  H5::Exception::dontPrint();
  H5::H5File file(path, H5F_ACC_RDONLY);

  HostScatBremTable t;
  t.common = read_common(file);  // Brem has /EnergyGrid + /ThermoState, no /EtaGrid

  const std::string group = "/Scat_Brem_Kernels";
  t.nOpacities = detail::read_int_scalar(file, group + "/nOpacities");
  t.nMoments = detail::read_int_scalar(file, group + "/nMoments");

  t.offset = detail::read_double(file, group + "/Offsets");  // 2D [nOp,nMom] flat
  if (static_cast<int>(t.offset.size()) != t.nOpacities * t.nMoments) {
    throw std::runtime_error("wli::io: Brem Offsets is not 2D[nOpacities,nMoments]");
  }

  // The Brem value array is named S_sigma (not Kernels).
  t.nPoints = fortran_shape<5>(file, group + "/S_sigma");
  t.sSigma = detail::read_double(file, group + "/S_sigma");  // log-stored
  return t;
}

}  // namespace io
}  // namespace wli
