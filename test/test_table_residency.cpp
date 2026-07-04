// Device table-residency / upload helper acceptance probe.
//
// Exercises the ONE metadata + residency/upload convention in
// src/lib/wli_table.H (specs/amrex-device-interface.md:36-44,80-88,90-105):
//
//  1. Upload round-trip (3D, 4D, 5D): fully populate a host buffer with distinct
//     exactly-representable per-index sentinels, ResidentTable::upload(...), then
//     read every element back through TableView::operator() and require EXACT ==
//     (index/byte round-trips carry no tolerance).
//  2. Device/host equivalence + kernel-callability: call the AMREX_GPU_HOST_DEVICE
//     TableView accessor directly on host AND from inside a ParallelFor lambda
//     capturing the view + extents by value; require the results identical at the
//     machine-precision tier (wli::is_close(rtol_machine)) — for exact sentinels
//     this is effectively ==.
//  3. Trivially-copyable static_asserts (the capture-by-value contract) live in
//     wli_table.H and fire at compile time; re-asserted here for locality.

#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <vector>

#include <AMReX.H>
#include <AMReX_GpuContainers.H>
#include <AMReX_GpuLaunch.H>

#include "wli_compare.H"
#include "wli_table.H"

namespace {

int g_failures = 0;

void check(bool ok, const char* msg) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  }
}

// A distinct, exactly-representable sentinel double for an index tuple (mixed-
// radix encoding, cf. test_index_roundtrip.cpp): every tuple maps to a unique
// integer value, small enough to be exact in double.
template <int D>
double sentinel(amrex::GpuArray<int, D> const& idx,
                amrex::GpuArray<int, D> const& n) {
  double s = 0.0;
  double scale = 1.0;
  for (int d = 0; d < D; ++d) {
    s += static_cast<double>(idx[d]) * scale;
    scale *= static_cast<double>(n[d] + 1);
  }
  return s + 0.25;  // exact-in-binary fraction; distinguishes tuple 0 from zero-fill
}

// Fully populate a host buffer with sentinels (column-major via wli::flat_index),
// upload through ResidentTable, and read every element back through the
// TableView accessor. Exact equality required. Returns via out-params a single
// probe index + its host-computed sentinel for the equivalence test.
template <int D>
void upload_roundtrip(const char* label, amrex::GpuArray<int, D> const& n) {
  wli::TableMeta<D> meta;
  meta.n = n;
  for (int d = 0; d < D; ++d) meta.kind[d] = wli::AxisKind::Log;
  meta.OS = 0.0;

  const amrex::Long total = meta.size();
  // DeviceVector/PODVector does NOT zero-init; write the whole host buffer first.
  std::vector<double> host(static_cast<std::size_t>(total));

  // Odometer over the column-major box: write sentinel at each tuple's offset.
  amrex::GpuArray<int, D> idx{};
  for (amrex::Long count = 0; count < total; ++count) {
    host[static_cast<std::size_t>(wli::flat_index<D>(idx, n))] = sentinel<D>(idx, n);
    for (int d = 0; d < D; ++d) {
      if (++idx[d] < n[d]) break;
      idx[d] = 0;
    }
  }

  wli::ResidentTable<D> rt;
  rt.upload(meta, host.data(), static_cast<std::size_t>(total));

  // Metadata survives the upload.
  check(rt.meta().size() == total, "meta.size preserved through upload");

  wli::TableView<D> v = rt.view();

  // Read back every element through the accessor: exact ==.
  amrex::GpuArray<int, D> jdx{};
  for (amrex::Long count = 0; count < total; ++count) {
    double got = v(jdx);
    double want = sentinel<D>(jdx, n);
    if (got != want) {  // exact, no tolerance
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%s: readback got=%.17g want=%.17g", label,
                    got, want);
      check(false, buf);
    }
    for (int d = 0; d < D; ++d) {
      if (++jdx[d] < n[d]) break;
      jdx[d] = 0;
    }
  }
}

// Device/host equivalence + kernel-callability. Call the TableView accessor at
// every index directly on host AND inside a ParallelFor lambda capturing the
// view + extents by value; require identical results at the machine tier.
template <int D>
void device_host_equivalence(const char* label, amrex::GpuArray<int, D> const& n) {
  wli::TableMeta<D> meta;
  meta.n = n;
  for (int d = 0; d < D; ++d) meta.kind[d] = wli::AxisKind::Linear;
  meta.OS = 0.0;

  const amrex::Long total = meta.size();
  std::vector<double> host(static_cast<std::size_t>(total));
  amrex::GpuArray<int, D> idx{};
  for (amrex::Long count = 0; count < total; ++count) {
    host[static_cast<std::size_t>(wli::flat_index<D>(idx, n))] = sentinel<D>(idx, n);
    for (int d = 0; d < D; ++d) {
      if (++idx[d] < n[d]) break;
      idx[d] = 0;
    }
  }

  wli::ResidentTable<D> rt;
  rt.upload(meta, host.data(), static_cast<std::size_t>(total));
  wli::TableView<D> v = rt.view();

  // Device path: one query per element via ParallelFor, capturing v by value.
  amrex::Gpu::DeviceVector<double> dev(static_cast<std::size_t>(total));
  double* dp = dev.data();
  const amrex::GpuArray<int, D> nn = n;
  amrex::ParallelFor(static_cast<int>(total), [=] AMREX_GPU_DEVICE (int p) noexcept {
    // Decode the linear thread id p into a column-major index tuple.
    amrex::GpuArray<int, D> k{};
    int rem = p;
    for (int d = 0; d < D; ++d) {
      k[d] = rem % nn[d];
      rem /= nn[d];
    }
    dp[p] = v(k);
  });
  amrex::Gpu::streamSynchronize();

  // Host path: same accessor, same decode; require identical at machine tier.
  for (amrex::Long p = 0; p < total; ++p) {
    amrex::GpuArray<int, D> k{};
    amrex::Long rem = p;
    for (int d = 0; d < D; ++d) {
      k[d] = static_cast<int>(rem % n[d]);
      rem /= n[d];
    }
    double host_val = v(k);
    double dev_val = dp[static_cast<std::size_t>(p)];
    if (!wli::is_close(dev_val, host_val, wli::rtol_machine)) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%s: host/device diverge host=%.17g dev=%.17g",
                    label, host_val, dev_val);
      check(false, buf);
    }
  }
}

}  // namespace

// Trivially-copyable contract (also enforced inside wli_table.H at instantiation).
static_assert(std::is_trivially_copyable_v<wli::TableView<3>>, "TableView<3> POD");
static_assert(std::is_trivially_copyable_v<wli::TableView<4>>, "TableView<4> POD");
static_assert(std::is_trivially_copyable_v<wli::TableView<5>>, "TableView<5> POD");
static_assert(std::is_trivially_copyable_v<wli::TableMeta<3>>, "TableMeta<3> POD");
static_assert(std::is_trivially_copyable_v<wli::TableMeta<4>>, "TableMeta<4> POD");
static_assert(std::is_trivially_copyable_v<wli::TableMeta<5>>, "TableMeta<5> POD");

int main(int argc, char* argv[]) {
  amrex::Initialize(argc, argv);

  upload_roundtrip<3>("3D", amrex::GpuArray<int, 3>{4, 3, 5});
  upload_roundtrip<4>("4D", amrex::GpuArray<int, 4>{2, 4, 3, 5});
  upload_roundtrip<5>("5D", amrex::GpuArray<int, 5>{2, 3, 2, 4, 3});

  device_host_equivalence<3>("3D", amrex::GpuArray<int, 3>{4, 3, 5});
  device_host_equivalence<4>("4D", amrex::GpuArray<int, 4>{2, 4, 3, 5});
  device_host_equivalence<5>("5D", amrex::GpuArray<int, 5>{2, 3, 2, 4, 3});

  amrex::Finalize();

  if (g_failures != 0) {
    std::fprintf(stderr, "table_residency: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("PASS table_residency: upload round-trip + device/host equivalence (3D-5D)\n");
  return EXIT_SUCCESS;
}
