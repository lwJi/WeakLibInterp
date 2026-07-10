#pragma once

// WeakLibInterp device table-residency / upload helper + metadata convention.
//
// Single source of truth for the ONE metadata-carrying + residency/upload
// convention reused by every correctness-bearing kernel (3D EOS, 4D EmAb, 5D
// Iso/NES/Pair/Brem). The device-resident table is a flat, arena-backed
// amrex::Gpu::DeviceVector<double> filled once on the host and uploaded once
// with Gpu::htod_memcpy, then kept resident for the run (the persistent-
// resident-table model); kernels capture a lightweight raw-pointer-plus-extents
// handle (TableView) by value. No amrex::TableData / Table*D / Array4 is used
// (they cap at 4D; three opacity channels are genuinely 5D).
// See specs/amrex-device-interface.md:36-44,46-62,80-88,90-105.
//
// Small fixed-size by-value metadata (axis extents, per-axis log/linear flags,
// the additive log-recovery offset) is carried in amrex::GpuArray — a trivially-
// copyable aggregate, not a Table class — captured by value into kernels
// (spec:44,112). The static_asserts below enforce that capture-by-value contract
// at compile time.

#include <cstddef>
#include <type_traits>

#include <AMReX_GpuContainers.H>  // Gpu::DeviceVector, GpuArray
#include <AMReX_GpuDevice.H>      // Gpu::htod_memcpy

#include "wli_index.H"  // wli::flat_index (column-major offset arithmetic)
#include "wli_real.H"   // wli::Real (pinned to double)

namespace wli {

// Per-axis interpolation kind. Semantics from
// specs/fortran-parity-and-tolerances.md:56-63: NotInterp axes are not
// interpolated over; Linear axes interpolate in the raw value; Log axes
// interpolate in log10-space with the 10**-OS additive recovery offset.
enum class AxisKind : int { NotInterp = 0, Linear = 1, Log = 2 };

// Fixed-size, trivially-copyable axis geometry + interpolation metadata for a
// D-dimensional table. Carries the genuinely-uniform part across all channels:
// axis extents (n), per-axis kind, and the single additive 10**-OS log-recovery
// offset (matches the illustrative `double OS` in spec:58). Captured by value
// into kernels.
//
// Multi-moment opacity channels that need several offsets carry a separate
// device-resident offsets array selected by a component axis, rather than
// growing this fixed struct — only the axis geometry (n, kind) is pinned here.
//
// The `kind` and `OS` fields are caller-set bridge convention for the IO->device
// path, not read back by any kernel. No src/ code bridges a HostEosTable into a
// TableMeta today (only tests populate these slots); they are the convention slots
// such a builder would fill — a HostEosTable->ResidentTable<3> builder would set
// kind = {Log, Log, Linear} and OS = offset. But view() intentionally forwards
// only n + data (the TableView<D> below): every kernel statically knows its own
// axis kinds (EOS hardcodes Log/Log/Lin, opacity Lin) and receives OS as a
// separate scalar, so wiring kind/OS through view() would only add runtime
// dispatch no spec asks for (spec:112 names per-axis metadata packaging an
// implementation freedom). The AxisKind enum itself is live elsewhere — HostAxis::kind
// and HostOpacityGrid::kind in the IO layer (wli_io_eos.H:41, wli_io_opacity.H:44)
// — so neither the enum nor these fields are orphaned stubs.
template <int D>
struct TableMeta {
  amrex::GpuArray<int, D> n;         // axis extents (n0, ..., n_{D-1})
  amrex::GpuArray<AxisKind, D> kind; // per-axis interpolation kind
  double OS = 0.0;                   // additive 10**-OS log-recovery offset

  // Total number of table elements (extent product) as a 64-bit Long so 5D
  // products cannot overflow.
  AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
  amrex::Long size() const noexcept {
    amrex::Long p = 1;
    for (int d = 0; d < D; ++d) p *= static_cast<amrex::Long>(n[d]);
    return p;
  }
};

// Lightweight raw-pointer-plus-extents handle kernels capture by value (spec:
// 48,82). This is NOT a Table*D / Array4 (forbidden, spec:93): just the flat
// device pointer + axis extents, indexed column-major via wli::flat_index.
template <int D>
struct TableView {
  double const* data;
  amrex::GpuArray<int, D> n;

  // Column-major gather at a D-dimensional index tuple. Reuses the shared
  // offset arithmetic (wli_index.H) — the offset math is not re-derived here.
  AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
  double operator()(amrex::GpuArray<int, D> const& idx) const noexcept {
    return data[wli::flat_index<D>(idx, n)];
  }
};

// Residency owner: holds the arena-backed device buffer plus its metadata,
// uploads once, and hands out a by-value TableView for kernels to capture.
template <int D>
class ResidentTable {
 public:
  // Fill once on the host and upload once (host -> device). `count` is the
  // element count (== meta.size()); the byte count is count*sizeof(double).
  // Uses the real Gpu::htod_memcpy so the code is drop-in for a future GPU
  // build (collapses to a blocking memcpy + no-op sync on a CPU build).
  void upload(TableMeta<D> const& meta, double const* host, std::size_t count) {
    meta_ = meta;
    d_.resize(count);
    amrex::Gpu::htod_memcpy(d_.data(), host, count * sizeof(double));
  }

  // By-value handle for kernel capture.
  TableView<D> view() const noexcept { return TableView<D>{d_.data(), meta_.n}; }

  TableMeta<D> const& meta() const noexcept { return meta_; }

 private:
  amrex::Gpu::DeviceVector<double> d_;
  TableMeta<D> meta_;
};

// Compile-time backpressure: the by-value capture contract requires the
// metadata carrier and the kernel handle to be trivially copyable. Instantiate
// the checks for D = 3, 4, 5 (the live dimensionalities: 3D EOS, 4D EmAb, 5D
// opacity kernels).
namespace detail {
template <int D>
struct trivially_copyable_check {
  static_assert(std::is_trivially_copyable_v<TableView<D>>,
                "wli::TableView must be trivially copyable (kernel capture-by-value)");
  static_assert(std::is_trivially_copyable_v<TableMeta<D>>,
                "wli::TableMeta must be trivially copyable (kernel capture-by-value)");
};
template struct trivially_copyable_check<3>;
template struct trivially_copyable_check<4>;
template struct trivially_copyable_check<5>;
}  // namespace detail

}  // namespace wli
