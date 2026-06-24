# AMReX device interface (cross-cutting device contract)

> Cross-cutting spec. Leaf specs (`eos-interpolation`, `eos-inversion`, `opacity-emab-iso`, `opacity-nes-pair`, `opacity-brem`) reference this file for the shared device/residency/launch contract and restate, in their own sections, only the pieces (e.g. the dimensionality of the index formula) they actually use. If a leaf spec and this file ever disagree, `README.md` is the canonical arbiter; absent that, this file governs the device contract.

## Purpose & scope

This spec defines how the correctness-bearing interpolators are exposed as AMReX-native, GPU-callable C++: the qualifier macros, the table-residency container and its host→device upload, the raw-pointer-plus-extents column-major indexing convention, the scalar allocation-free `_Point` device-callable contract, and the host-level `ParallelFor` launch pattern.

In scope:
- The AMReX qualifier macros device-callable functions are marked with.
- `amrex::Gpu::DeviceVector<double>` as the table-residency container, its arena backing, and `Gpu::htod_memcpy` upload.
- The flat `double const*` + integer extents/strides table-passing convention and the exact column-major index→offset formula, for every dimensionality 1D through 5D.
- The scalar, allocation-free `_Point` device-callable contract (mirroring weaklib's `_Point` / `!$ACC ROUTINE SEQ` routines).
- The host-level `ParallelFor` launch pattern for the array forms.
- Why `amrex::TableData` / `Table*D` / `Array4` are explicitly NOT used for the correctness-bearing tables.
- The value type being pinned to `double` regardless of `amrex::Real`.

Out of scope:
- The interpolation arithmetic, tolerances, log-space convention (see `fortran-parity-and-tolerances.md`).
- The on-disk HDF5 layout the table is read from (see `table-format-and-io.md`).
- The per-channel geometry (each leaf spec owns its own).
- Build/link configuration of AMReX (see `build-integration.md`).

## Source of truth

AMReX behavior is the sibling `amrex` repository at the commit recorded alongside the table provenance; the relevant headers:

- `amrex/Src/Base/AMReX_GpuQualifiers.H` — the qualifier macros `AMREX_GPU_HOST_DEVICE`, `AMREX_GPU_DEVICE`, and (with `AMReX_Extension.H`) `AMREX_FORCE_INLINE`. A function marked `AMREX_GPU_HOST_DEVICE` is compiled for both host and device and is callable from inside a kernel; under a CPU-only build the macros expand to nothing.
- `amrex/Src/Base/AMReX_GpuContainers.H` — `Gpu::DeviceVector<T> = PODVector<T, ArenaAllocator<T>>` (arena-backed device memory), and `Gpu::htod_memcpy` / `dtoh_memcpy` / `dtod_memcpy` (host↔device copies that collapse to `std::memcpy` on a CPU build). Under a CPU build `DeviceVector` allocates ordinary host memory whose `.data()` is a valid host pointer.
- `amrex/Src/Base/AMReX_GpuLaunchFunctsG.H` — the `ParallelFor(N, lambda(i))`, `ParallelFor(Box, lambda(i,j,k))`, and `ParallelFor(Box, ncomp, lambda(i,j,k,n))` launch overloads (the CPU build supplies sequential-loop overloads with identical lambda signatures, so the same kernel source compiles for host and device).

The device idiom these specs mirror — a standalone `AMREX_GPU_DEVICE`/`AMREX_GPU_HOST_DEVICE` function called from inside a `ParallelFor` lambda captured `[=] ... noexcept` — is exemplified by `amrex/Tests/GPU/CNS/Source/*_K.H` and its call sites.

## Inputs & outputs

### Qualifier macros

Device-callable scalar interpolation functions are declared `AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE` so they compile for both host and device and inline at the call site; hot inner helpers may use the same qualifiers. Lambdas passed to `ParallelFor` are written `[=] AMREX_GPU_DEVICE (int i, ...) noexcept { ... }` — `[=]` copies the lightweight table pointer + extents into the closure by value; `noexcept` is required because device code cannot throw.

### Table residency container

The device-resident table is `amrex::Gpu::DeviceVector<double>` (arena-backed). It is filled once on the host and uploaded once with `Gpu::htod_memcpy` (or an async copy + stream sync) and kept resident for the run — the persistent-resident-table model. The container is dimensionality-agnostic: the same flat `DeviceVector<double>` serves the 3D EOS table, the 4D EmAb table, and the 5D Iso/NES/Pair/Brem kernels uniformly.

Small fixed-size by-value metadata (axis extents, per-axis log/linear flags, per-moment offsets) may be carried in `amrex::GpuArray<...>` (a trivially-copyable aggregate — not a `Table` class) and captured by value into kernels.

### The device entry-point contract (the `_Point` form)

Public device-callable single-query entry points are **scalar in their query coordinates and allocation-free**, mirroring weaklib's `_Point` / `!$ACC ROUTINE SEQ` routines. They receive the table as a raw device pointer plus its integer extents (from `DeviceVector::data()` and the known shape), not as a view object:

```cpp
// Illustrative shape of the device-callable contract (EOS single-point; see eos-interpolation.md).
// The table is amrex::Gpu::DeviceVector<double>; .data() is passed in as double const*.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
double eos_interp_point(double D, double T, double Y,             // scalar query (rho, T, Ye)
                        double const* Ds, int nD,                 // axis arrays + extents
                        double const* Ts, int nT,
                        double const* Ys, int nY,
                        double OS,                                // additive offset
                        double const* table);                     // flat (iD,iT,iY) col-major
```

Whether the innermost multilinear helper takes the pointer+strides or plain unpacked corner scalars is implementation freedom. The signature shapes above are illustrative; the binding constraints are: scalar query coordinates, allocation-free, table passed as `double const*` + extents.

### Column-major indexing convention (part of the contract)

Because the table is a flat `double const*`, the index→offset arithmetic is part of the contract. The Fortran first index is fastest-varying (column-major; see `table-format-and-io.md`). For a Fortran-logical shape `(n0, n1, ..., n_{D-1})` with 0-based indices `(i0, ..., i_{D-1})`:

```
offset = i0 + n0*( i1 + n1*( i2 + ... + n_{D-2}*( i_{D-1} ) ... ) )
```

Concretely:
- 3D EOS `Table(i0,i1,i2)` with logical shape `(nD,nT,nY)`:
  `table[ i0 + nD*( i1 + nT*i2 ) ]`.
- 4D EmAb `Table(i0,i1,i2,i3)` with logical shape `(nE,nD,nT,nY)`:
  `table[ i0 + nE*( i1 + nD*( i2 + nT*i3 ) ) ]`.
- 5D `Table(i0,i1,i2,i3,i4)`:
  `table[ i0 + n0*( i1 + n1*( i2 + n2*( i3 + n3*i4 ) ) ) ]`.

### Host-level launch

The array (multi-query) forms are not device-callable; they own the parallel loop and launch one kernel from host level via `ParallelFor`, capturing the resident table pointer + extents by value:

```cpp
amrex::ParallelFor(npoints, [=] AMREX_GPU_DEVICE (int p) noexcept {
    out[p] = eos_interp_point(D[p], T[p], Y[p], Ds, nD, Ts, nT, Ys, nY, OS, table);
});
```

## Correctness requirements

- **Value type pinned to `double`.** The table container is `Gpu::DeviceVector<double>` and the entry points take `double const*`; the correctness-bearing value type is fixed to `double` (= weaklib `dp = 8`) regardless of how `amrex::Real` is configured. A single-precision AMReX build must not be able to silently degrade bit-level parity. The HDF5 reader contract remains `H5T_NATIVE_DOUBLE` (see `table-format-and-io.md`).
- **`TableData` / `Table*D` / `Array4` are NOT used for the correctness-bearing tables.** AMReX's `TableData` provides only `Table1D`–`Table4D` and `Array4` is likewise ≤ 4D, but three opacity channels are genuinely 5D — Iso `(E, moment, ρ, T, Yₑ)`, NES/Pair `(E', E, kernel, T, η)`, Brem `(E', E, moment, ρ, T)`. No AMReX table-view template can represent them, so a single uniform contract is only possible over a flat `double const*` + extents. (`GpuArray` is allowed for small fixed-size by-value metadata only.)
- **Scalar, allocation-free `_Point` contract.** Device-callable single-query entry points must allocate nothing and must be legal to call from inside an already-running kernel thread. They take scalar query coordinates and the table as `double const*` + extents.
- **Column-major indexing.** The in-kernel index→offset arithmetic must follow the column-major formula above; this is the same layout the `table-format-and-io` reader produces, extended to in-kernel indexing.
- **Host/CPU parity needs no GPU.** Because the qualifier macros expand to nothing, `ParallelFor` becomes a sequential loop, and `Gpu::DeviceVector` allocates host memory under a CPU build, the entire regression suite runs on host with no GPU — but it does link AMReX (its default double-precision CPU configuration; see `build-integration.md`).

## Verification

- **Device/host equivalence.** Any `_Point` function compiled `AMREX_GPU_HOST_DEVICE` must produce identical results when called directly on the host and when called from inside a `ParallelFor` lambda over the same inputs (on a CPU build these are the same code path; the check guards against accidental host-only divergence and confirms the closure-capture-by-value contract).
- **Indexing round-trip.** A test that writes known sentinel values into a flat `DeviceVector<double>` at chosen `(i0,...,i_{D-1})` using the column-major formula above and reads them back through a `_Point` call must recover the sentinels exactly, for 3D, 4D, and 5D shapes — proving the index arithmetic matches the documented formula.
- **No-allocation / kernel-callability.** The `_Point` entry points must compile and run inside a `ParallelFor` lambda (the CPU build is sufficient to exercise this).
- **Mechanical (validator).** `bash specs/tools/validate_specs.sh` (default mode) asserts this file carries the 7 mandated sections in order, names a concrete numeric tolerance, and that its cited `amrex/...` source-of-truth paths resolve under `$AMREX_ROOT`.

Tolerances for the equivalence/indexing checks are the machine-precision exactness tier (`~1e-14`, a few ULP) for value comparisons and exact equality for index round-trips, per `fortran-parity-and-tolerances.md`.

## Implementation freedom

- Whether the innermost multilinear helpers take the pointer+strides or unpacked corner scalars.
- The exact argument grouping/order of the `_Point` signatures (the shapes above are illustrative; the binding constraints are scalar query, allocation-free, `double const*` + extents).
- Which arena backs the `DeviceVector` and whether the upload is synchronous `htod_memcpy` or an async copy + stream sync.
- Whether per-axis metadata is carried in `GpuArray`, passed as separate scalar/pointer arguments, or packed into a small POD struct — provided it is trivially copyable and captured by value.
- The `ParallelFor` overload chosen for the array forms (`N`, `Box`, or `Box+ncomp`).

## Open questions / assumptions

- **CPU-only build is the test target (assumption, non-blocking).** No GPU is required or assumed in this environment; AMReX is built CPU-only / double precision and the suite runs on host. The contract is written so a GPU build is a drop-in (the macros and `ParallelFor` overloads switch to device forms), but GPU execution is not verified here. See `build-integration.md`.
- **`amrex::Real` configuration (assumption, non-blocking).** The contract pins the value type to `double` independently of `amrex::Real`; if an AMReX build sets `amrex::Real = float`, the interpolator's tables and entry points still use `double`. This is a stated requirement, not a property AMReX enforces.
