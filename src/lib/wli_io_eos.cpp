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
#include <stdexcept>

#include <H5Cpp.h>

#include "wli_io_hdf5_detail.H"  // shared read primitives (extracted, no ad-hoc copy)

namespace wli {
namespace io {

using detail::read_double;
using detail::read_int;
using detail::read_int_scalar;
using detail::read_strings;
using detail::try_read_double;

HostEosTable read_eos_table(const std::string& path) {
  H5::Exception::dontPrint();
  H5::H5File file(path, H5F_ACC_RDONLY);

  HostEosTable t;

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
      t.dvByName.emplace(d.name, static_cast<int>(j));
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

  return t;
}

}  // namespace io
}  // namespace wli
