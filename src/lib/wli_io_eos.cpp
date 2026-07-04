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

#include "wli_index.H"  // wli::flat_index (offset convention, reused not re-derived)

namespace wli {
namespace io {

namespace {

// Total element count of a dataspace (product of all extents).
hssize_t npoints(const H5::DataSet& ds) {
  return ds.getSpace().getSimpleExtentNpoints();
}

// Read a full integer dataset as a flat std::vector<int> (H5T_NATIVE_INT).
std::vector<int> read_int(const H5::H5File& f, const std::string& name) {
  H5::DataSet ds = f.openDataSet(name);
  std::vector<int> out(static_cast<std::size_t>(npoints(ds)));
  ds.read(out.data(), H5::PredType::NATIVE_INT);
  return out;
}

// Read a single integer scalar dataset ({1}).
int read_int_scalar(const H5::H5File& f, const std::string& name) {
  const std::vector<int> v = read_int(f, name);
  if (v.empty()) {
    throw std::runtime_error("wli::io::read_eos_table: empty int dataset " + name);
  }
  return v[0];
}

// Read a full double dataset as a flat std::vector<double> (H5T_NATIVE_DOUBLE).
// The raw bytes are read contiguously and NOT permuted, so a C-order (h5ls)
// dataset lands as Fortran column-major with the first Fortran index fastest.
std::vector<double> read_double(const H5::H5File& f, const std::string& name) {
  H5::DataSet ds = f.openDataSet(name);
  std::vector<double> out(static_cast<std::size_t>(npoints(ds)));
  ds.read(out.data(), H5::PredType::NATIVE_DOUBLE);
  return out;
}

// Read a fixed-width string dataset (string[LEN]) into trimmed std::strings.
std::vector<std::string> read_strings(const H5::H5File& f, const std::string& name) {
  H5::DataSet ds = f.openDataSet(name);
  const H5::StrType st = ds.getStrType();
  const std::size_t len = st.getSize();
  const std::size_t n = static_cast<std::size_t>(npoints(ds));
  std::vector<char> buf(n * len, '\0');
  ds.read(buf.data(), st);
  std::vector<std::string> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::string s(buf.data() + i * len, len);
    const std::size_t e = s.find_last_not_of(std::string(" \0", 2));
    out.push_back(e == std::string::npos ? std::string() : s.substr(0, e + 1));
  }
  return out;
}

// Try to read a double dataset that may be absent (min/maxValues); returns
// false without throwing if the dataset does not exist.
bool try_read_double(const H5::H5File& f, const std::string& name,
                     std::vector<double>& out) {
  if (!f.nameExists(name)) return false;
  out = read_double(f, name);
  return true;
}

}  // namespace

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
  // iRho/iT/iYe.
  {
    const std::vector<std::string> tsNames = read_strings(file, "/ThermoState/Names");
    const std::vector<int> logInterp = read_int(file, "/ThermoState/LogInterp");
    if (tsNames.size() != 3 || logInterp.size() != 3) {
      throw std::runtime_error(
          "wli::io::read_eos_table: /ThermoState Names/LogInterp not rank-3");
    }
    for (int a = 0; a < 3; ++a) {
      HostAxis ax;
      ax.name = tsNames[a];
      ax.kind = (logInterp[a] != 0) ? wli::AxisKind::Log : wli::AxisKind::Linear;
      ax.points = read_double(file, "/ThermoState/" + tsNames[a]);
      t.axes[a] = std::move(ax);
    }
    t.tsIndices = {read_int_scalar(file, "/ThermoState/iRho"),
                   read_int_scalar(file, "/ThermoState/iT"),
                   read_int_scalar(file, "/ThermoState/iYe")};
  }

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
