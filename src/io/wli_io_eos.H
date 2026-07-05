#pragma once

// WeakLibInterp HDF5 EOS-table reader + shared in-memory host table struct.
//
// Single source of truth for loading the weaklib EOS production table
// (wl-EOS-SFHo-15-25-50.h5) into host memory in the layout the interpolators
// and the device-residency upload (src/core/wli_table.H) consume, with NO
// adapter layer. The reader contract (group/dataset names, Fortran column-major
// shapes, dtypes, 1D per-variable Offsets, names-before-arrays read order) is
// pinned by specs/table-format-and-io.md and anchored to the committed
// structural snapshot specs/fixtures/wl-EOS-SFHo-15-25-50.h5ls.
//
// The in-memory representation (spec freedom, :187) is decided here as:
// flat per-DV value buffers + recorded integer extents + name/slot keys,
// TableMeta-compatible and log-stored. Each HostDV.values is a flat
// std::vector<double> in on-disk (Fortran column-major, rho fastest-varying)
// byte order, so dv[j].values.data() plus a wli::TableMeta<3> hand straight to
// wli::ResidentTable<3>::upload(...). Values stay log-stored; the additive
// offset is kept separate (physical = 10**stored - offset applied during/after
// interpolation, spec:188).

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "wli_real.H"   // wli::Real (pinned to double)
#include "wli_table.H"  // wli::AxisKind (Log/Linear), reused — no new enum

namespace wli {
namespace io {

// ----------------------------------------------------------------------------
// Shared in-memory host table struct (the decide-once representation).
// ----------------------------------------------------------------------------

// One independent EOS axis (rho, T, or Ye): its on-disk name, interpolation
// kind (from /ThermoState/LogInterp), and node coordinates.
struct HostAxis {
  std::string name;                 // e.g. "Density"
  wli::AxisKind kind = wli::AxisKind::Linear;
  std::vector<double> points;       // node coordinates
};

// One dependent variable (EOS output): its on-disk name, flat log-stored value
// buffer (C-contiguous, rho fastest-varying = Fortran column-major), the 1D
// additive recovery offset, and (optional) per-variable extents.
struct HostDV {
  std::string name;                 // e.g. "Pressure" (verbatim, spaces kept)
  std::vector<double> values;       // flat, log-stored, shape (nRho,nT,nYe)
  double offset = 0.0;              // additive: physical = 10**stored - offset
  double vmin = 0.0;
  double vmax = 0.0;
  bool hasExtents = false;         // true iff min/maxValues were present
};

// Named slot indices for the 15 dependent variables (role -> slot, read from
// the /DependentVariables/i<Name> datasets, never assumed by position).
struct HostDVIndices {
  int iPressure = -1;
  int iEntropyPerBaryon = -1;
  int iInternalEnergyDensity = -1;
  int iElectronChemicalPotential = -1;
  int iProtonChemicalPotential = -1;
  int iNeutronChemicalPotential = -1;
  int iProtonMassFraction = -1;
  int iNeutronMassFraction = -1;
  int iAlphaMassFraction = -1;
  int iHeavyMassFraction = -1;
  int iHeavyMassNumber = -1;
  int iHeavyChargeNumber = -1;
  int iHeavyBindingEnergy = -1;
  int iThermalEnergy = -1;
  int iGamma1 = -1;
};

// The full EOS table in host memory, ready for per-DV upload to the device.
struct HostEosTable {
  int nVariables = 0;
  std::array<int, 3> nPoints{{0, 0, 0}};      // Fortran order (nRho, nT, nYe)
  std::array<HostAxis, 3> axes{};             // Fortran order (rho, T, Ye)
  std::array<int, 3> tsIndices{{-1, -1, -1}}; // iRho, iT, iYe (role -> slot)
  std::vector<HostDV> dv;                      // size nVariables, /Names order
  std::unordered_map<std::string, int> dvByName; // name -> slot
  HostDVIndices dvIndices{};
  std::vector<int> repaired;                   // flat, shape nPoints
};

// ----------------------------------------------------------------------------
// The reader.
// ----------------------------------------------------------------------------

// Read the EOS HDF5 table at `path` into host memory. Follows the weaklib
// ReadEquationOfStateTableHDF order (open /DependentVariables, read Dimensions +
// nVariables, then /ThermoState, then /DependentVariables), reading the name
// datasets before the value arrays they key. Throws H5::Exception (or
// std::runtime_error) on a malformed/absent file. This is the full live-table
// path; it requires a real production table present (guard live opens on
// WL_TABLES_ROOT in callers).
HostEosTable read_eos_table(const std::string& path);

// Instrumentation seam (specs/table-format-and-io.md:184): the number of times
// ANY reader entry point has actually opened a .h5 file via the shared
// root-gated open wrapper. Only the I/O root rank opens, so under an MPI run at
// >= 2 ranks a non-root rank observes this counter unchanged across a load —
// the mechanism the rank-consistency / root-only-open test uses to prove
// non-root ranks perform no file open. Shared across all six readers (one
// counter). Serial builds see it increment once per successful load.
int hdf5_open_count();

// ----------------------------------------------------------------------------
// Expected schema (reader expectations, exposed for the structural self-test).
//
// These are the fixed dataset names + ranks the reader depends on. The self-test
// checks reader-expectation vs the committed snapshot (not snapshot
// self-consistency), so this data is the authoritative expectation.
// ----------------------------------------------------------------------------

namespace schema {

// Exactly three top-level groups.
inline constexpr const char* kGroups[3] = {
    "ThermoState", "DependentVariables", "Metadata"};

// The three independent axes, stored under their /ThermoState/Names values, in
// Fortran order (rho, T, Ye) with their h5ls extents.
inline constexpr const char* kAxisNames[3] = {
    "Density", "Temperature", "Electron Fraction"};
inline constexpr int kAxisExtent[3] = {185, 81, 30}; // nRho, nT, nYe

// nVariables for this pinned table (= number of named DVs; NOT hardcoded in the
// reader, which reads /DependentVariables/nVariables).
inline constexpr int kNVariables = 15;

// The 15 dependent-variable names (verbatim, spaces kept), stored under their
// /DependentVariables/Names values. Each value dataset has h5ls shape
// {30,81,185} = Fortran (nRho,nT,nYe).
inline constexpr const char* kDVNames[kNVariables] = {
    "Alpha Mass Fraction",
    "Electron Chemical Potential",
    "Entropy Per Baryon",
    "Gamma1",
    "Heavy Binding Energy",
    "Heavy Charge Number",
    "Heavy Mass Fraction",
    "Heavy Mass Number",
    "Internal Energy Density",
    "Neutron Chemical Potential",
    "Neutron Mass Fraction",
    "Pressure",
    "Proton Chemical Potential",
    "Proton Mass Fraction",
    "Thermal Energy",
};

// The 15 role->slot index datasets /DependentVariables/i<Name> {1}. Note the
// i-names are the space-stripped CamelCase of the DV names (weaklib convention),
// so this list is authoritative and separate from kDVNames.
inline constexpr const char* kIDVNames[kNVariables] = {
    "iAlphaMassFraction",
    "iElectronChemicalPotential",
    "iEntropyPerBaryon",
    "iGamma1",
    "iHeavyBindingEnergy",
    "iHeavyChargeNumber",
    "iHeavyMassFraction",
    "iHeavyMassNumber",
    "iInternalEnergyDensity",
    "iNeutronChemicalPotential",
    "iNeutronMassFraction",
    "iPressure",
    "iProtonChemicalPotential",
    "iProtonMassFraction",
    "iThermalEnergy",
};

// The 7 human-readable provenance datasets /Metadata/* {1}.
inline constexpr const char* kMetadata[7] = {
    "IDTag", "TableResolution", "NucEOSLink", "LeptonEOSLink",
    "SourceLink", "WLRevision", "TableLink"};

}  // namespace schema

}  // namespace io
}  // namespace wli
