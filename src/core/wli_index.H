#pragma once

// WeakLibInterp column-major flat-index helper (1D through 5D).
//
// Single source of truth for the raw-pointer table index->offset arithmetic
// shared by every kernel (3D EOS, 4D EmAb, 5D Iso/NES/Pair/Brem) and the HDF5
// reader. The correctness-bearing tables are a flat `double const*`, so the
// column-major index math is part of the device contract; see
// specs/amrex-device-interface.md:64-79.
//
// For a Fortran-logical shape (n0, n1, ..., n_{D-1}) with 0-based indices
// (i0, ..., i_{D-1}) the Fortran first index is fastest-varying:
//
//   offset = i0 + n0*( i1 + n1*( i2 + ... + n_{D-2}*( i_{D-1} ) ... ) )
//
// The top extent n_{D-1} is unused. No amrex::TableData / Table*D / Array4 is
// used (they cap at 4D; three opacity channels are genuinely 5D) —
// see specs/amrex-device-interface.md:93.

// AMREX_GPU_HOST_DEVICE lives in <AMReX_GpuQualifiers.H>; AMREX_FORCE_INLINE
// lives in <AMReX_Extension.H>. <AMReX_Array.H> pulls in both transitively and
// also supplies amrex::GpuArray (a trivially-copyable aggregate).
#include <AMReX_Array.H>

namespace wli {

// Column-major flat offset for a D-dimensional table, computed via a Horner
// fold that is identical for every D in {1,...,5}. Extents are passed in as a
// carrier (GpuArray) so this stays pure index arithmetic, agnostic to the
// extents-metadata convention owned by later increments. Returns amrex::Long
// (64-bit) so 5D extent products cannot overflow.
template <int D>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
amrex::Long flat_index(amrex::GpuArray<int, D> const& idx,
                       amrex::GpuArray<int, D> const& n) noexcept {
  static_assert(D >= 1 && D <= 5, "wli::flat_index supports 1D through 5D");
  // Horner fold from the slowest-varying index down to the fastest.
  //   off = i_{D-1}; off = i_{D-2} + n_{D-2}*off; ...; off = i0 + n0*off
  amrex::Long off = static_cast<amrex::Long>(idx[D - 1]);
  for (int d = D - 2; d >= 0; --d) {
    off = static_cast<amrex::Long>(idx[d]) +
          static_cast<amrex::Long>(n[d]) * off;
  }
  return off;
}

}  // namespace wli
