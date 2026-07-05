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

#include <memory>
#include <stdexcept>

#include <H5Cpp.h>

#include "wli_io_bcast_detail.H"  // root-read + MPI broadcast primitives
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

// ---------------------------------------------------------------------------
// Broadcast helpers (spec:170-171). Fixed-size metadata first, then arrays.
// The gating flag precedes its conditionally-sized array (e.g. `geometric`
// before edge/width, so non-root allocates the same optional fields).
// ---------------------------------------------------------------------------

void bcast_grid(HostOpacityGrid& g) {
  detail::bcast_string(g.name);
  detail::bcast_axis_kind(g.kind);
  detail::bcast_vector(g.points);
  detail::bcast_flag(g.geometric);      // gate BEFORE the geometric extras
  detail::bcast_scalar(g.zoom);
  detail::bcast_vector(g.edge);
  detail::bcast_vector(g.width);
  detail::bcast_scalar(g.minEdge);
  detail::bcast_scalar(g.maxEdge);
  detail::bcast_scalar(g.minWidth);
}

void bcast_common(HostOpacityCommon& c) {
  bcast_grid(c.energyGrid);
  detail::bcast_thermo_state(c.axes, c.tsIndices);
}

// Distribute a HostTable from root to every rank: broadcast status FIRST
// (collective failure, no hang), then the metadata + arrays via bcast_fn. The
// root-only serial read (read_fn) may throw; that maps to status=0.
template <typename HostTable, typename ReadFn, typename BcastFn>
HostTable root_read_and_bcast(const std::string& path, ReadFn read_fn,
                              BcastFn bcast_fn) {
  HostTable t;
  int status = 1;  // 1 = ok, 0 = root-side open/read failure
  if (detail::io_is_root()) {
    try {
      H5::Exception::dontPrint();
      std::unique_ptr<H5::H5File> filePtr = detail::root_open(path);
      read_fn(*filePtr, t);
    } catch (H5::Exception&) {
      status = 0;
    } catch (std::exception&) {
      status = 0;
    }
  }
  detail::bcast_status(status);
  if (status == 0) {
    throw std::runtime_error(
        "wli::io: I/O root failed to open/read '" + path + "'");
  }
  bcast_fn(t);
  return t;
}

}  // namespace

namespace {

// ---- EmAb -----------------------------------------------------------------

void read_emab_root(const H5::H5File& file, HostEmAbTable& t) {
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
}

void bcast_emab(HostEmAbTable& t) {
  bcast_common(t.common);
  detail::bcast_flag(t.usedLegacyGroup);
  detail::bcast_scalar(t.nOpacities);
  detail::bcast_array(t.nPoints);
  detail::bcast_strings(t.speciesNames);
  detail::bcast_vector(t.offset);
  if (!detail::io_is_root()) {
    t.values.resize(t.speciesNames.size());
  }
  for (auto& v : t.values) detail::bcast_vector(v);
  detail::bcast_flag(t.hasEmAbParameters);
  detail::bcast_flag(t.hasECTable);
}

// ---- Iso ------------------------------------------------------------------

void read_iso_root(const H5::H5File& file, HostScatIsoTable& t) {
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
}

void bcast_iso(HostScatIsoTable& t) {
  bcast_common(t.common);
  detail::bcast_scalar(t.nOpacities);
  detail::bcast_scalar(t.nMoments);
  detail::bcast_array(t.nPoints);
  detail::bcast_strings(t.speciesNames);
  detail::bcast_vector(t.offset);
  if (!detail::io_is_root()) {
    t.values.resize(t.speciesNames.size());
  }
  for (auto& v : t.values) detail::bcast_vector(v);
}

// ---- NES / Pair (shared layout) -------------------------------------------

void read_nes_pair_root(const H5::H5File& file, const std::string& group,
                        HostScatNESPairTable& t) {
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
}

void bcast_nes_pair(HostScatNESPairTable& t) {
  bcast_common(t.common);
  bcast_grid(t.etaGrid);
  detail::bcast_scalar(t.nOpacities);
  detail::bcast_scalar(t.nMoments);
  detail::bcast_array(t.nPoints);
  detail::bcast_vector(t.offset);
  detail::bcast_vector(t.kernels);
  detail::bcast_flag(t.hasNPS);
}

// ---- Brem -----------------------------------------------------------------

void read_brem_root(const H5::H5File& file, HostScatBremTable& t) {
  t.common = read_common(file);  // Brem: /EnergyGrid + /ThermoState, no /EtaGrid

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
}

void bcast_brem(HostScatBremTable& t) {
  bcast_common(t.common);
  detail::bcast_scalar(t.nOpacities);
  detail::bcast_scalar(t.nMoments);
  detail::bcast_array(t.nPoints);
  detail::bcast_vector(t.offset);
  detail::bcast_vector(t.sSigma);
}

}  // namespace

HostEmAbTable read_emab_table(const std::string& path) {
  return root_read_and_bcast<HostEmAbTable>(
      path,
      [](const H5::H5File& f, HostEmAbTable& t) { read_emab_root(f, t); },
      [](HostEmAbTable& t) { bcast_emab(t); });
}

HostScatIsoTable read_scat_iso_table(const std::string& path) {
  return root_read_and_bcast<HostScatIsoTable>(
      path,
      [](const H5::H5File& f, HostScatIsoTable& t) { read_iso_root(f, t); },
      [](HostScatIsoTable& t) { bcast_iso(t); });
}

HostScatNESPairTable read_scat_nes_table(const std::string& path) {
  return root_read_and_bcast<HostScatNESPairTable>(
      path,
      [](const H5::H5File& f, HostScatNESPairTable& t) {
        read_nes_pair_root(f, "/Scat_NES_Kernels", t);
      },
      [](HostScatNESPairTable& t) { bcast_nes_pair(t); });
}

HostScatNESPairTable read_scat_pair_table(const std::string& path) {
  return root_read_and_bcast<HostScatNESPairTable>(
      path,
      [](const H5::H5File& f, HostScatNESPairTable& t) {
        read_nes_pair_root(f, "/Scat_Pair_Kernels", t);
      },
      [](HostScatNESPairTable& t) { bcast_nes_pair(t); });
}

HostScatBremTable read_scat_brem_table(const std::string& path) {
  return root_read_and_bcast<HostScatBremTable>(
      path,
      [](const H5::H5File& f, HostScatBremTable& t) { read_brem_root(f, t); },
      [](HostScatBremTable& t) { bcast_brem(t); });
}

}  // namespace io
}  // namespace wli
