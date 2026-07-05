#pragma once

// WeakLibInterp shared internal MPI root-read + broadcast primitives.
//
// Single source of truth for the rank-aware distribution layer used by BOTH the
// EOS reader (wli_io_eos.cpp) and the opacity reader (wli_io_opacity.cpp). The
// parallel-distribution contract (specs/table-format-and-io.md:170-171) is:
// only the I/O root rank opens/reads the .h5 file; a status flag is broadcast
// FIRST (so a root-side open/read failure is a collective failure with no hang),
// then the fixed-size metadata, then every value/name/offset array — so every
// rank ends byte-identical and no rank is ever left blocked in a broadcast.
//
// There is NO separate parallel entry point: the six reader entry points call
// these helpers on a single code path. Under a non-MPI AMReX build
// (AMReX_MPI=OFF) IOProcessorNumber()==0 unconditionally and every Bcast is a
// serial no-op, so this same code path IS the byte-for-byte serial read. This
// is an INTERNAL header: it pulls in <H5Cpp.h> and AMReX headers and is included
// only by reader .cpp files, never by a public reader .H or a test.
//
// Mirrors wli_io_hdf5_detail.H: header-only, inline linkage, namespace
// wli::io::detail, no src/CMakeLists.txt edit. The AMReX ParallelDescriptor
// surface (IOProcessor/IOProcessorNumber/Bcast) is the communicator API the
// loaders use instead of raw MPI (spec:30); Bcast<int>/<double>/<char> are the
// only collectives needed.

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <H5Cpp.h>

#include <AMReX_ParallelDescriptor.H>

#include "wli_io_eos.H"  // wli::io::HostAxis (the shared axis struct)
#include "wli_table.H"   // wli::AxisKind

namespace wli {
namespace io {
namespace detail {

// ---------------------------------------------------------------------------
// Root selection + open instrumentation.
// ---------------------------------------------------------------------------

// The single well-defined I/O root rank (spec:198). In a non-MPI build this is
// 0 unconditionally, so io_is_root() is always true and the serial path runs.
inline int io_root() { return amrex::ParallelDescriptor::IOProcessorNumber(); }
inline bool io_is_root() { return amrex::ParallelDescriptor::IOProcessor(); }

// Instrumented open counter (the test-(a) seam): the number of times a reader
// has actually opened a .h5 file via root_open(). Only the root rank ever
// calls root_open(), so under an MPI run at >= 2 ranks a non-root rank observes
// this stay at 0 — the mechanism the spec (spec:184) leaves to implementation
// freedom for proving non-root ranks perform no file open.
inline int& open_counter() {
  static int c = 0;
  return c;
}

// Root-gated file open. Called ONLY on the root rank (inside the reader's
// if (io_is_root()) block), it increments the instrumented open counter and
// opens the HDF5 file read-only. A non-root rank never reaches this, so it never
// touches the filesystem. Returned by unique_ptr so the caller can scope the
// H5File to the root-only try block. Throws H5::Exception on open failure, which
// the reader catches and turns into a broadcast status flag.
inline std::unique_ptr<H5::H5File> root_open(const std::string& path) {
  ++open_counter();
  return std::make_unique<H5::H5File>(path, H5F_ACC_RDONLY);
}

// ---------------------------------------------------------------------------
// Broadcast primitives (thin wrappers over ParallelDescriptor::Bcast).
// Each is a serial no-op under a non-MPI build.
// ---------------------------------------------------------------------------

// Broadcast the read-status flag FIRST, unconditionally on every rank
// (spec:171): 1 = root read succeeded, 0 = root-side open/read failure. Every
// rank then throws the same error on 0, so a failure never leaves a rank blocked
// in a later array Bcast (collective failure, no hang).
inline void bcast_status(int& status) {
  amrex::ParallelDescriptor::Bcast<int>(&status, 1, io_root());
}

inline void bcast_scalar(int& v) {
  amrex::ParallelDescriptor::Bcast<int>(&v, 1, io_root());
}

inline void bcast_scalar(double& v) {
  amrex::ParallelDescriptor::Bcast<double>(&v, 1, io_root());
}

// A bool travels as an int (portable, no vector<bool> / MPI_C_BOOL concerns).
inline void bcast_flag(bool& b) {
  int i = b ? 1 : 0;
  amrex::ParallelDescriptor::Bcast<int>(&i, 1, io_root());
  b = (i != 0);
}

template <std::size_t N>
inline void bcast_array(std::array<int, N>& a) {
  if (N > 0) {
    amrex::ParallelDescriptor::Bcast<int>(a.data(), N, io_root());
  }
}

// Size-prefixed vector broadcast: send the count first so non-root ranks resize
// before receiving the payload, then send the payload.
inline void bcast_vector(std::vector<double>& v) {
  int n = static_cast<int>(v.size());
  amrex::ParallelDescriptor::Bcast<int>(&n, 1, io_root());
  if (!io_is_root()) v.resize(static_cast<std::size_t>(n));
  if (n > 0) {
    amrex::ParallelDescriptor::Bcast<double>(v.data(),
                                             static_cast<std::size_t>(n),
                                             io_root());
  }
}

inline void bcast_vector(std::vector<int>& v) {
  int n = static_cast<int>(v.size());
  amrex::ParallelDescriptor::Bcast<int>(&n, 1, io_root());
  if (!io_is_root()) v.resize(static_cast<std::size_t>(n));
  if (n > 0) {
    amrex::ParallelDescriptor::Bcast<int>(v.data(),
                                          static_cast<std::size_t>(n),
                                          io_root());
  }
}

// Length-prefixed string broadcast (spec:197 leaves fixed-width vs length-prefix
// free; length-prefix is lower-friction because read_strings already trims to
// std::string).
inline void bcast_string(std::string& s) {
  int n = static_cast<int>(s.size());
  amrex::ParallelDescriptor::Bcast<int>(&n, 1, io_root());
  if (!io_is_root()) s.resize(static_cast<std::size_t>(n));
  if (n > 0) {
    amrex::ParallelDescriptor::Bcast<char>(s.data(),
                                           static_cast<std::size_t>(n),
                                           io_root());
  }
}

inline void bcast_strings(std::vector<std::string>& v) {
  int n = static_cast<int>(v.size());
  amrex::ParallelDescriptor::Bcast<int>(&n, 1, io_root());
  if (!io_is_root()) v.resize(static_cast<std::size_t>(n));
  for (auto& s : v) bcast_string(s);
}

// An AxisKind travels as its underlying int.
inline void bcast_axis_kind(wli::AxisKind& k) {
  int i = static_cast<int>(k);
  amrex::ParallelDescriptor::Bcast<int>(&i, 1, io_root());
  k = static_cast<wli::AxisKind>(i);
}

inline void bcast_axis(HostAxis& ax) {
  bcast_string(ax.name);
  bcast_axis_kind(ax.kind);
  bcast_vector(ax.points);
}

// The shared /ThermoState payload (three axes + iRho/iT/iYe), broadcast
// identically by the EOS reader and every opacity reader (the /ThermoState
// block is byte-identical between files, spec:85-87).
inline void bcast_thermo_state(std::array<HostAxis, 3>& axes,
                               std::array<int, 3>& tsIndices) {
  for (auto& ax : axes) bcast_axis(ax);
  bcast_array(tsIndices);
}

}  // namespace detail
}  // namespace io
}  // namespace wli
