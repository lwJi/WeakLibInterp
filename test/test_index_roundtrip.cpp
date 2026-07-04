// Column-major flat-index round-trip acceptance probe.
//
// Proves the index arithmetic in src/core/wli_index.H matches the documented
// column-major formula (specs/amrex-device-interface.md:64-79,101): for 3D, 4D
// and 5D shapes (plus cheap 1D/2D), write a distinct sentinel double into a
// flat Gpu::DeviceVector<double> at each (i0,...,i_{D-1}) using the helper's
// offset, read it back through the SAME helper, and require exact equality
// (no tolerance — index round-trips are exact per
// specs/fortran-parity-and-tolerances.md). Also confirms the helper is a
// bijection over the enumerated index box (no collisions, in-bounds) and that
// it is callable from inside a ParallelFor lambda (kernel-callability).

#include <cstdio>
#include <cstdlib>
#include <vector>

#include <AMReX_GpuContainers.H>
#include <AMReX_GpuLaunch.H>

#include "wli_index.H"
#include "wli_real.H"

namespace {

int g_failures = 0;

void check(bool ok, const char* msg) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  }
}

// A distinct, exactly-representable sentinel double for an index tuple. Uses a
// mixed-radix encoding of the tuple so every tuple maps to a unique integer
// value (small enough to be exact in double).
template <int D>
double sentinel(amrex::GpuArray<int, D> const& idx,
                amrex::GpuArray<int, D> const& n) {
  double s = 0.0;
  double scale = 1.0;
  for (int d = 0; d < D; ++d) {
    s += static_cast<double>(idx[d]) * scale;
    scale *= static_cast<double>(n[d] + 1);
  }
  // Offset by a fraction that is exact in binary so all-zero tuples are still a
  // recognizable, non-trivial sentinel and distinct from raw zero-fill.
  return s + 0.25;
}

// Enumerate every index tuple in the box [0,n0)x...x[0,n_{D-1}) in column-major
// order via an odometer, writing sentinels through wli::flat_index and reading
// them back through the same helper. Verifies exact round-trip, in-bounds
// offsets and no collisions (bijection). label names the shape in diagnostics.
template <int D>
void roundtrip(const char* label, amrex::GpuArray<int, D> const& n) {
  amrex::Long total = 1;
  for (int d = 0; d < D; ++d) total *= static_cast<amrex::Long>(n[d]);

  amrex::Gpu::DeviceVector<double> table(static_cast<std::size_t>(total));
  double* t = table.data();
  // Pre-fill with a value no sentinel can take, so an unwritten slot is caught.
  for (amrex::Long k = 0; k < total; ++k) t[k] = -1.0;

  std::vector<char> seen(static_cast<std::size_t>(total), 0);

  amrex::GpuArray<int, D> idx{};  // all zeros
  for (amrex::Long count = 0; count < total; ++count) {
    amrex::Long off = wli::flat_index<D>(idx, n);

    char buf[128];
    if (off < 0 || off >= total) {
      std::snprintf(buf, sizeof(buf), "%s: offset %lld out of [0,%lld)", label,
                    static_cast<long long>(off), static_cast<long long>(total));
      check(false, buf);
    } else {
      if (seen[static_cast<std::size_t>(off)]) {
        std::snprintf(buf, sizeof(buf), "%s: offset %lld collision", label,
                      static_cast<long long>(off));
        check(false, buf);
      }
      seen[static_cast<std::size_t>(off)] = 1;
      t[off] = sentinel<D>(idx, n);
    }

    // Odometer increment: i0 is fastest-varying (column-major).
    for (int d = 0; d < D; ++d) {
      if (++idx[d] < n[d]) break;
      idx[d] = 0;
    }
  }

  // Every slot must have been written exactly once (bijection onto [0,total)).
  amrex::Long nwritten = 0;
  for (amrex::Long k = 0; k < total; ++k) nwritten += seen[static_cast<std::size_t>(k)];
  if (nwritten != total) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s: wrote %lld of %lld slots", label,
                  static_cast<long long>(nwritten), static_cast<long long>(total));
    check(false, buf);
  }

  // Read back through the same helper: exact equality required.
  amrex::GpuArray<int, D> jdx{};
  for (amrex::Long count = 0; count < total; ++count) {
    amrex::Long off = wli::flat_index<D>(jdx, n);
    double got = t[off];
    double want = sentinel<D>(jdx, n);
    if (got != want) {  // exact ==, no tolerance
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s: readback off=%lld got=%.17g want=%.17g",
                    label, static_cast<long long>(off), got, want);
      check(false, buf);
    }
    for (int d = 0; d < D; ++d) {
      if (++jdx[d] < n[d]) break;
      jdx[d] = 0;
    }
  }
}

// Spot-check the exact documented formulas against hand-expanded expressions,
// including corner tuples (all-zeros, max-index).
void formula_spotchecks() {
  // 3D EOS (nD,nT,nY): table[i0 + nD*(i1 + nT*i2)].
  {
    amrex::GpuArray<int, 3> n{4, 3, 5};
    auto expect3 = [&](int i0, int i1, int i2) {
      return static_cast<amrex::Long>(i0) + n[0] * (i1 + static_cast<amrex::Long>(n[1]) * i2);
    };
    check(wli::flat_index<3>({0, 0, 0}, n) == expect3(0, 0, 0), "3D all-zeros");
    check(wli::flat_index<3>({2, 1, 3}, n) == expect3(2, 1, 3), "3D mid");
    check(wli::flat_index<3>({3, 2, 4}, n) == expect3(3, 2, 4), "3D max-index");
  }
  // 4D EmAb (nE,nD,nT,nY): table[i0 + nE*(i1 + nD*(i2 + nT*i3))].
  {
    amrex::GpuArray<int, 4> n{2, 4, 3, 5};
    auto expect4 = [&](int i0, int i1, int i2, int i3) {
      return static_cast<amrex::Long>(i0) +
             n[0] * (i1 + static_cast<amrex::Long>(n[1]) * (i2 + static_cast<amrex::Long>(n[2]) * i3));
    };
    check(wli::flat_index<4>({0, 0, 0, 0}, n) == expect4(0, 0, 0, 0), "4D all-zeros");
    check(wli::flat_index<4>({1, 3, 2, 4}, n) == expect4(1, 3, 2, 4), "4D max-index");
  }
  // 5D: table[i0 + n0*(i1 + n1*(i2 + n2*(i3 + n3*i4)))].
  {
    amrex::GpuArray<int, 5> n{2, 3, 2, 4, 3};
    auto expect5 = [&](int i0, int i1, int i2, int i3, int i4) {
      return static_cast<amrex::Long>(i0) +
             n[0] * (i1 + static_cast<amrex::Long>(n[1]) *
             (i2 + static_cast<amrex::Long>(n[2]) *
             (i3 + static_cast<amrex::Long>(n[3]) * i4)));
    };
    check(wli::flat_index<5>({0, 0, 0, 0, 0}, n) == expect5(0, 0, 0, 0, 0), "5D all-zeros");
    check(wli::flat_index<5>({1, 2, 1, 3, 2}, n) == expect5(1, 2, 1, 3, 2), "5D max-index");
  }
}

// Kernel-callability: the helper must compile and run inside a ParallelFor
// lambda captured by value, producing the same offsets as the host path.
void parallelfor_callable() {
  amrex::GpuArray<int, 3> n{4, 3, 5};
  amrex::Long total = static_cast<amrex::Long>(n[0]) * n[1] * n[2];
  amrex::Gpu::DeviceVector<amrex::Long> got(static_cast<std::size_t>(total));
  amrex::Long* g = got.data();
  amrex::ParallelFor(static_cast<int>(total), [=] AMREX_GPU_DEVICE (int p) noexcept {
    int i2 = p / (n[0] * n[1]);
    int rem = p - i2 * (n[0] * n[1]);
    int i1 = rem / n[0];
    int i0 = rem - i1 * n[0];
    g[p] = wli::flat_index<3>({i0, i1, i2}, n);
  });
  amrex::Gpu::streamSynchronize();
  for (amrex::Long p = 0; p < total; ++p) {
    if (g[static_cast<std::size_t>(p)] != p) {
      check(false, "ParallelFor helper offset mismatch");
      break;
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  amrex::Initialize(argc, argv);

  formula_spotchecks();

  roundtrip<1>("1D", amrex::GpuArray<int, 1>{7});
  roundtrip<2>("2D", amrex::GpuArray<int, 2>{5, 4});
  roundtrip<3>("3D", amrex::GpuArray<int, 3>{4, 3, 5});
  roundtrip<4>("4D", amrex::GpuArray<int, 4>{2, 4, 3, 5});
  roundtrip<5>("5D", amrex::GpuArray<int, 5>{2, 3, 2, 4, 3});

  parallelfor_callable();

  amrex::Finalize();

  if (g_failures != 0) {
    std::fprintf(stderr, "index_roundtrip: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("PASS index_roundtrip: 1D-5D column-major round-trip exact\n");
  return EXIT_SUCCESS;
}
