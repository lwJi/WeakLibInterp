// WeakLibInterp HDF5 EOS-table reader implementation (H5Cpp API).
//
// Implements the live-table read path pinned by specs/table-format-and-io.md.
// Read order follows weaklib ReadEquationOfStateTableHDF
// (wlEOSIOModuleHDF.f90:183-213): open /DependentVariables, read Dimensions +
// nVariables; then /ThermoState (read Names + LogInterp before the axis arrays,
// which are stored under their Names values); then /DependentVariables (read
// Names + Offsets before the value arrays, each stored under its Names value).
// Multidimensional value arrays are read raw (no byte permutation): the on-disk
// h5ls shape {nYe,nT,nRho} is C-order for Fortran (nRho,nT,nYe) with rho
// fastest-varying, which already matches wli::flat_index<3>.

#include "wli_io_eos.H"

#include <cstring>
#include <memory>
#include <stdexcept>

#include <H5Cpp.h>

#include "wli_io_bcast_detail.H"  // root-read + MPI broadcast primitives
#include "wli_io_hdf5_detail.H"  // shared read primitives (extracted, no ad-hoc copy)

namespace wli {
namespace io {

using detail::read_double;
using detail::read_int;
using detail::read_int_scalar;
using detail::read_strings;
using detail::try_read_double;

namespace {

// Root-only serial read: open + every read_* into `t`. Byte-for-byte the former
// serial body of read_eos_table (spec requirements 1-6). Only the I/O root rank
// runs this; it may throw H5::Exception / std::runtime_error, which the caller
// turns into a broadcast status flag (collective failure).
void read_eos_table_root(const std::string& path, HostEosTable& t) {
  H5::Exception::dontPrint();
  std::unique_ptr<H5::H5File> filePtr = detail::root_open(path);
  H5::H5File& file = *filePtr;

  // (1) /DependentVariables: Dimensions {3} + nVariables {1} (allocate first).
  {
    const std::vector<int> dims = read_int(file, "/DependentVariables/Dimensions");
    if (dims.size() != 3) {
      throw std::runtime_error(
          "wli::io::read_eos_table: /DependentVariables/Dimensions is not rank-3");
    }
    t.nPoints = {dims[0], dims[1], dims[2]};  // Fortran (nRho, nT, nYe)
    t.nVariables = read_int_scalar(file, "/DependentVariables/nVariables");
  }
  const std::size_t nElem = static_cast<std::size_t>(t.nPoints[0]) *
                            static_cast<std::size_t>(t.nPoints[1]) *
                            static_cast<std::size_t>(t.nPoints[2]);

  // (2) /ThermoState: read Names + LogInterp BEFORE the axis arrays (which are
  // stored under their Names values). Axis identity is index-keyed via
  // iRho/iT/iYe. Shared with every opacity file (byte-identical /ThermoState).
  detail::read_thermo_state(file, t.axes, t.tsIndices);

  // (3) /DependentVariables: read Names + Offsets (+ optional min/maxValues)
  // BEFORE the value arrays (each stored under its Names value).
  {
    const std::vector<std::string> dvNames =
        read_strings(file, "/DependentVariables/Names");
    if (static_cast<int>(dvNames.size()) != t.nVariables) {
      throw std::runtime_error(
          "wli::io::read_eos_table: /DependentVariables/Names length != nVariables");
    }
    const std::vector<double> offsets =
        read_double(file, "/DependentVariables/Offsets");
    if (static_cast<int>(offsets.size()) != t.nVariables) {
      throw std::runtime_error(
          "wli::io::read_eos_table: /DependentVariables/Offsets is not 1D[nVariables]");
    }
    std::vector<double> vmin, vmax;
    const bool haveMin = try_read_double(file, "/DependentVariables/minValues", vmin);
    const bool haveMax = try_read_double(file, "/DependentVariables/maxValues", vmax);

    t.dv.reserve(dvNames.size());
    for (std::size_t j = 0; j < dvNames.size(); ++j) {
      HostDV d;
      d.name = dvNames[j];
      d.offset = offsets[j];
      d.values = read_double(file, "/DependentVariables/" + dvNames[j]);  // log-stored, flat
      if (d.values.size() != nElem) {
        throw std::runtime_error(
            "wli::io::read_eos_table: DV '" + dvNames[j] + "' element count mismatch");
      }
      if (haveMin && j < vmin.size() && haveMax && j < vmax.size()) {
        d.vmin = vmin[j];
        d.vmax = vmax[j];
        d.hasExtents = true;
      }
      t.dv.push_back(std::move(d));
    }

    // Role -> slot indices: the 15 i<Name> {1} datasets (index-keyed identity).
    t.dvIndices.iAlphaMassFraction =
        read_int_scalar(file, "/DependentVariables/iAlphaMassFraction");
    t.dvIndices.iElectronChemicalPotential =
        read_int_scalar(file, "/DependentVariables/iElectronChemicalPotential");
    t.dvIndices.iEntropyPerBaryon =
        read_int_scalar(file, "/DependentVariables/iEntropyPerBaryon");
    t.dvIndices.iGamma1 =
        read_int_scalar(file, "/DependentVariables/iGamma1");
    t.dvIndices.iHeavyBindingEnergy =
        read_int_scalar(file, "/DependentVariables/iHeavyBindingEnergy");
    t.dvIndices.iHeavyChargeNumber =
        read_int_scalar(file, "/DependentVariables/iHeavyChargeNumber");
    t.dvIndices.iHeavyMassFraction =
        read_int_scalar(file, "/DependentVariables/iHeavyMassFraction");
    t.dvIndices.iHeavyMassNumber =
        read_int_scalar(file, "/DependentVariables/iHeavyMassNumber");
    t.dvIndices.iInternalEnergyDensity =
        read_int_scalar(file, "/DependentVariables/iInternalEnergyDensity");
    t.dvIndices.iNeutronChemicalPotential =
        read_int_scalar(file, "/DependentVariables/iNeutronChemicalPotential");
    t.dvIndices.iNeutronMassFraction =
        read_int_scalar(file, "/DependentVariables/iNeutronMassFraction");
    t.dvIndices.iPressure =
        read_int_scalar(file, "/DependentVariables/iPressure");
    t.dvIndices.iProtonChemicalPotential =
        read_int_scalar(file, "/DependentVariables/iProtonChemicalPotential");
    t.dvIndices.iProtonMassFraction =
        read_int_scalar(file, "/DependentVariables/iProtonMassFraction");
    t.dvIndices.iThermalEnergy =
        read_int_scalar(file, "/DependentVariables/iThermalEnergy");

    // Repaired is a fixed extra int array (shape nPoints), NOT a named DV.
    const std::vector<int> rep = read_int(file, "/DependentVariables/Repaired");
    if (rep.size() != nElem) {
      throw std::runtime_error(
          "wli::io::read_eos_table: /DependentVariables/Repaired element count mismatch");
    }
    t.repaired = rep;
  }
}

// Broadcast the whole HostEosTable from root to every rank: fixed-size metadata
// first, then every value/name/offset array (spec:171). dvByName is NOT
// broadcast (unordered_map) — it is rebuilt locally on every rank from the
// now-identical dv[j].name (cheapest, guarantees identical map).
void bcast_eos_table(HostEosTable& t) {
  detail::bcast_scalar(t.nVariables);
  detail::bcast_array(t.nPoints);

  // /ThermoState axes + iRho/iT/iYe.
  detail::bcast_thermo_state(t.axes, t.tsIndices);

  // Dependent variables: resize the vector on non-root, then broadcast each.
  if (!detail::io_is_root()) {
    t.dv.resize(static_cast<std::size_t>(t.nVariables));
  }
  for (auto& d : t.dv) {
    detail::bcast_string(d.name);
    detail::bcast_scalar(d.offset);
    detail::bcast_scalar(d.vmin);
    detail::bcast_scalar(d.vmax);
    detail::bcast_flag(d.hasExtents);
    detail::bcast_vector(d.values);
  }

  // Role -> slot indices (15 ints), broadcast as a fixed block.
  std::array<int, 15> idx = {
      t.dvIndices.iPressure,
      t.dvIndices.iEntropyPerBaryon,
      t.dvIndices.iInternalEnergyDensity,
      t.dvIndices.iElectronChemicalPotential,
      t.dvIndices.iProtonChemicalPotential,
      t.dvIndices.iNeutronChemicalPotential,
      t.dvIndices.iProtonMassFraction,
      t.dvIndices.iNeutronMassFraction,
      t.dvIndices.iAlphaMassFraction,
      t.dvIndices.iHeavyMassFraction,
      t.dvIndices.iHeavyMassNumber,
      t.dvIndices.iHeavyChargeNumber,
      t.dvIndices.iHeavyBindingEnergy,
      t.dvIndices.iThermalEnergy,
      t.dvIndices.iGamma1};
  detail::bcast_array(idx);
  t.dvIndices.iPressure = idx[0];
  t.dvIndices.iEntropyPerBaryon = idx[1];
  t.dvIndices.iInternalEnergyDensity = idx[2];
  t.dvIndices.iElectronChemicalPotential = idx[3];
  t.dvIndices.iProtonChemicalPotential = idx[4];
  t.dvIndices.iNeutronChemicalPotential = idx[5];
  t.dvIndices.iProtonMassFraction = idx[6];
  t.dvIndices.iNeutronMassFraction = idx[7];
  t.dvIndices.iAlphaMassFraction = idx[8];
  t.dvIndices.iHeavyMassFraction = idx[9];
  t.dvIndices.iHeavyMassNumber = idx[10];
  t.dvIndices.iHeavyChargeNumber = idx[11];
  t.dvIndices.iHeavyBindingEnergy = idx[12];
  t.dvIndices.iThermalEnergy = idx[13];
  t.dvIndices.iGamma1 = idx[14];

  detail::bcast_vector(t.repaired);

  // Rebuild the name -> slot map identically on every rank (root included).
  t.dvByName.clear();
  for (std::size_t j = 0; j < t.dv.size(); ++j) {
    t.dvByName.emplace(t.dv[j].name, static_cast<int>(j));
  }
}

}  // namespace

HostEosTable read_eos_table(const std::string& path) {
  HostEosTable t;

  // Root-read + broadcast (spec:170-171). Only the I/O root opens/reads the
  // file, inside a try/catch that maps any failure to status=0. In a non-MPI
  // build io_is_root() is always true and every Bcast is a no-op, so this is
  // byte-for-byte the serial read.
  int status = 1;  // 1 = ok, 0 = root-side open/read failure
  if (detail::io_is_root()) {
    try {
      read_eos_table_root(path, t);
    } catch (H5::Exception&) {
      status = 0;
    } catch (std::exception&) {
      status = 0;
    }
  }

  // Broadcast status FIRST so a root-side failure is delivered to every rank as
  // the same error — no rank left blocked in a later array Bcast (no hang).
  detail::bcast_status(status);
  if (status == 0) {
    throw std::runtime_error(
        "wli::io::read_eos_table: I/O root failed to open/read '" + path + "'");
  }

  bcast_eos_table(t);
  return t;
}

int hdf5_open_count() { return detail::open_counter(); }

}  // namespace io
}  // namespace wli
