#pragma once

// WeakLibInterp shared internal HDF5 read primitives (H5Cpp API).
//
// Single source of truth for the low-level, channel-agnostic HDF5 read helpers
// used by BOTH the EOS reader (wli_io_eos.cpp) and the opacity reader
// (wli_io_opacity.cpp). CLAUDE.md forbids ad-hoc copies; these were extracted
// verbatim (behavior-preserving) from the original wli_io_eos.cpp anonymous
// namespace so the two readers share one implementation. This is an INTERNAL
// header: it pulls in <H5Cpp.h> and is included only by reader .cpp files, never
// by a public reader .H or a test.
//
// Also hosts read_thermo_state(): the shared /ThermoState read block. The
// opacity files carry a /ThermoState group byte-identical to the EOS file's
// (specs/table-format-and-io.md:85-87), so the EOS reader and every opacity
// reader read it through this one helper.

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

#include <H5Cpp.h>

#include "wli_io_eos.H"  // wli::io::HostAxis (the shared axis struct)
#include "wli_table.H"   // wli::AxisKind (Log/Linear)

namespace wli {
namespace io {
namespace detail {

// Total element count of a dataspace (product of all extents).
inline hssize_t npoints(const H5::DataSet& ds) {
  return ds.getSpace().getSimpleExtentNpoints();
}

// h5ls (C-order) extents of a dataset. The first entry is the slowest-varying
// index; reverse it to recover the Fortran logical shape (first index fastest).
inline std::vector<hsize_t> dims(const H5::H5File& f, const std::string& name) {
  H5::DataSet ds = f.openDataSet(name);
  H5::DataSpace sp = ds.getSpace();
  const int rank = sp.getSimpleExtentNdims();
  std::vector<hsize_t> out(static_cast<std::size_t>(rank));
  sp.getSimpleExtentDims(out.data(), nullptr);
  return out;
}

// Read a full integer dataset as a flat std::vector<int> (H5T_NATIVE_INT).
inline std::vector<int> read_int(const H5::H5File& f, const std::string& name) {
  H5::DataSet ds = f.openDataSet(name);
  std::vector<int> out(static_cast<std::size_t>(npoints(ds)));
  ds.read(out.data(), H5::PredType::NATIVE_INT);
  return out;
}

// Read a single integer scalar dataset ({1}).
inline int read_int_scalar(const H5::H5File& f, const std::string& name) {
  const std::vector<int> v = read_int(f, name);
  if (v.empty()) {
    throw std::runtime_error("wli::io: empty int dataset " + name);
  }
  return v[0];
}

// Read a full double dataset as a flat std::vector<double> (H5T_NATIVE_DOUBLE).
// The raw bytes are read contiguously and NOT permuted, so a C-order (h5ls)
// dataset lands as Fortran column-major with the first Fortran index fastest.
inline std::vector<double> read_double(const H5::H5File& f, const std::string& name) {
  H5::DataSet ds = f.openDataSet(name);
  std::vector<double> out(static_cast<std::size_t>(npoints(ds)));
  ds.read(out.data(), H5::PredType::NATIVE_DOUBLE);
  return out;
}

// Read a fixed-width string dataset (string[LEN]) into trimmed std::strings.
inline std::vector<std::string> read_strings(const H5::H5File& f,
                                             const std::string& name) {
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

// Try to read a double dataset that may be absent; returns false without
// throwing if the dataset does not exist.
inline bool try_read_double(const H5::H5File& f, const std::string& name,
                            std::vector<double>& out) {
  if (!f.nameExists(name)) return false;
  out = read_double(f, name);
  return true;
}

// Read the shared /ThermoState block: Names + LogInterp BEFORE the three axis
// arrays (which are stored under their Names values), then iRho/iT/iYe. Fills
// `axes` in Fortran order (rho, T, Ye) and `tsIndices` = {iRho, iT, iYe}. This
// is byte-identical between the EOS and every opacity file.
inline void read_thermo_state(const H5::H5File& f,
                              std::array<HostAxis, 3>& axes,
                              std::array<int, 3>& tsIndices) {
  const std::vector<std::string> tsNames = read_strings(f, "/ThermoState/Names");
  const std::vector<int> logInterp = read_int(f, "/ThermoState/LogInterp");
  if (tsNames.size() != 3 || logInterp.size() != 3) {
    throw std::runtime_error("wli::io: /ThermoState Names/LogInterp not rank-3");
  }
  for (int a = 0; a < 3; ++a) {
    HostAxis ax;
    ax.name = tsNames[a];
    ax.kind = (logInterp[a] != 0) ? wli::AxisKind::Log : wli::AxisKind::Linear;
    ax.points = read_double(f, "/ThermoState/" + tsNames[a]);
    axes[a] = std::move(ax);
  }
  tsIndices = {read_int_scalar(f, "/ThermoState/iRho"),
               read_int_scalar(f, "/ThermoState/iT"),
               read_int_scalar(f, "/ThermoState/iYe")};
}

}  // namespace detail
}  // namespace io
}  // namespace wli
