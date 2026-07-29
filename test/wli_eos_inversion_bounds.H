#pragma once

// Test-only builders for wli::EosInversionBounds (src/eos/wli_eos_inversion.H).
//
// This is a test fixture header (same status as h5ls_snapshot.H and
// wli_rank_digest.H): it lives under test/ and is deliberately NOT part of the
// src/ standard library. specs/eos-inversion.md:163,169 place the representation
// of the cached bounds and of the "initialized" state squarely in
// "implementation freedom" — the only fixed contract is that querying with
// absent bounds yields code 10. A builder in src/eos/ would therefore invent
// public contract the spec expressly declines to fix (e.g. that the X extents
// must come from a full scan of the stored sub-table rather than from the
// file-supplied extents). What DOES motivate sharing it is CLAUDE.md's "no
// ad-hoc copies": six regression TUs were each hand-filling the same six fields
// and re-typing the same min/max scan.
//
// Behavior transcribed verbatim from those call sites, so conversions are
// bit-identical: the scan seeds from element 0 and then re-visits element 0 in
// the loop, compares with strict </> (no <=, no std::minmax_element), and calls
// wli::recover exactly once per element in ascending index order. Host-only
// (no AMREX_GPU_* qualifiers): bounds are built on the host and passed by const
// reference into the device-callable kernels.

#include <cstddef>

#include "wli_eos_inversion.H"  // wli::EosInversionBounds
#include "wli_interp.H"         // wli::recover
#include "wli_real.H"

namespace wli {
namespace test {

// Explicit-X form: rho/Ye extents from the axis endpoints (the axes are
// strictly ascending, so endpoint == MIN/MAXVAL), X extents supplied by the
// caller, and `initialized = true` so CheckInputError does not take the vacuous
// code-10 path.
inline EosInversionBounds MakeBounds(const Real* Ds, int nD, const Real* Ys,
                                     int nY, Real MinX, Real MaxX) {
  EosInversionBounds b;
  b.MinD = Ds[0];
  b.MaxD = Ds[nD - 1];
  b.MinX = MinX;
  b.MaxX = MaxX;
  b.MinY = Ys[0];
  b.MaxY = Ys[nY - 1];
  b.initialized = true;
  return b;
}

// Scan form: derive MinX/MaxX from the recovered PHYSICAL values of the
// log-stored sub-table Xs[0..nX) at offset OS (spec:60 — the bounds are the
// extents of the recovered X sub-table values), then delegate to MakeBounds.
inline EosInversionBounds MakeBoundsFromTable(const Real* Ds, int nD,
                                              const Real* Ys, int nY,
                                              const Real* Xs, std::size_t nX,
                                              Real OS) {
  Real lo = wli::recover(Xs[0], OS), hi = lo;
  for (std::size_t k = 0; k < nX; ++k) {
    Real v = wli::recover(Xs[k], OS);
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  return MakeBounds(Ds, nD, Ys, nY, lo, hi);
}

}  // namespace test
}  // namespace wli
