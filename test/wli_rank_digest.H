#pragma once

// Test-only cross-rank byte-digest helpers for the MPI rank-consistency suite
// (specs/regression-suite-design.md:65-72, specs/table-format-and-io.md:183).
//
// This is a test fixture header (analogous to h5ls_snapshot.H's test-only
// status): it lives under test/ and is NOT part of the src/ standard library,
// because digesting a loaded host table is regression instrumentation, not
// reader behavior (src/ stays MPI-agnostic — the only ParallelDescriptor usage
// in src/ is confined to src/io/). It holds ONE generic FNV-1a Hasher plus a
// digest() overload per host-table struct so multiple tests
// (test_mpi_root_bcast.cpp, test_rank_consistency.cpp) share a single
// implementation instead of duplicating the hasher (CLAUDE.md: no ad-hoc copies).
//
// The tier is EXACT/bitwise: broadcasts preserve bits, so a byte digest is the
// no-tolerance comparator (NOT wli::is_close). digest() folds ONLY the broadcast
// fields of each struct; it deliberately SKIPS locally-rebuilt members such as
// HostEosTable::dvByName (which is a pure function of the already-digested dv
// names, so including it would not strengthen the check). The optional
// geometric-grid fields (edge/width/zoom) and the NES/Pair etaGrid are walked
// unconditionally — they are always sized consistently after the broadcast, and
// omitting a conditionally-sized nested field would silently weaken the check.

#include <cstdint>
#include <string>
#include <vector>

#include "wli_io_eos.H"      // wli::io::HostAxis, HostDV, HostEosTable
#include "wli_io_opacity.H"  // wli::io::HostOpacity* / HostEmAb/Iso/NESPair/Brem

namespace wli {
namespace test {

// Generic FNV-1a byte accumulator. Every field is folded through the same POD
// byte stream, so two structs digest equal iff their broadcast fields are
// byte-identical (exact tier). Lifted verbatim from the original inline copy in
// test/test_mpi_root_bcast.cpp so both tests share it.
struct Hasher {
  std::uint64_t h = 1469598103934665603ULL;  // FNV-1a offset basis
  void bytes(const void* p, std::size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) {
      h ^= b[i];
      h *= 1099511628211ULL;
    }
  }
  void i(int v) { bytes(&v, sizeof(v)); }
  void d(double v) { bytes(&v, sizeof(v)); }
  void s(const std::string& v) {
    i(static_cast<int>(v.size()));
    bytes(v.data(), v.size());
  }
  void vd(const std::vector<double>& v) {
    i(static_cast<int>(v.size()));
    bytes(v.data(), v.size() * sizeof(double));
  }
  void vi(const std::vector<int>& v) {
    i(static_cast<int>(v.size()));
    bytes(v.data(), v.size() * sizeof(int));
  }
};

// ---- Shared sub-folds (nested structs every table embeds). -----------------

inline void fold(Hasher& hh, const wli::io::HostAxis& ax) {
  hh.s(ax.name);
  hh.i(static_cast<int>(ax.kind));
  hh.vd(ax.points);
}

inline void fold(Hasher& hh, const wli::io::HostOpacityGrid& g) {
  hh.s(g.name);
  hh.i(static_cast<int>(g.kind));
  hh.vd(g.points);
  hh.i(g.geometric ? 1 : 0);  // gate BEFORE the geometric extras (mirrors bcast)
  hh.d(g.zoom);
  hh.vd(g.edge);
  hh.vd(g.width);
  hh.d(g.minEdge);
  hh.d(g.maxEdge);
  hh.d(g.minWidth);
}

inline void fold(Hasher& hh, const wli::io::HostOpacityCommon& c) {
  fold(hh, c.energyGrid);
  for (const auto& ax : c.axes) fold(hh, ax);
  for (int v : c.tsIndices) hh.i(v);
}

// ---- Per-table digests (broadcast fields only). ----------------------------

inline std::uint64_t digest(const wli::io::HostEosTable& t) {
  Hasher hh;
  hh.i(t.nVariables);
  for (int p : t.nPoints) hh.i(p);
  for (const auto& ax : t.axes) fold(hh, ax);
  for (int v : t.tsIndices) hh.i(v);
  for (const auto& dv : t.dv) {
    hh.s(dv.name);
    hh.d(dv.offset);
    hh.d(dv.vmin);
    hh.d(dv.vmax);
    hh.i(dv.hasExtents ? 1 : 0);
    hh.vd(dv.values);
  }
  // Fold all 15 HostDVIndices in declaration order (== broadcast pack order,
  // src/io/wli_io_eos.cpp idx[0]=iPressure … idx[14]=iGamma1): the digest must
  // cover every loaded array's slot index so a divergence in any one is caught
  // (spec table-format-and-io §183 Verification #5).
  hh.i(t.dvIndices.iPressure);
  hh.i(t.dvIndices.iEntropyPerBaryon);
  hh.i(t.dvIndices.iInternalEnergyDensity);
  hh.i(t.dvIndices.iElectronChemicalPotential);
  hh.i(t.dvIndices.iProtonChemicalPotential);
  hh.i(t.dvIndices.iNeutronChemicalPotential);
  hh.i(t.dvIndices.iProtonMassFraction);
  hh.i(t.dvIndices.iNeutronMassFraction);
  hh.i(t.dvIndices.iAlphaMassFraction);
  hh.i(t.dvIndices.iHeavyMassFraction);
  hh.i(t.dvIndices.iHeavyMassNumber);
  hh.i(t.dvIndices.iHeavyChargeNumber);
  hh.i(t.dvIndices.iHeavyBindingEnergy);
  hh.i(t.dvIndices.iThermalEnergy);
  hh.i(t.dvIndices.iGamma1);
  hh.vi(t.repaired);
  return hh.h;  // NOTE: dvByName intentionally NOT folded (locally rebuilt).
}

inline std::uint64_t digest(const wli::io::HostEmAbTable& t) {
  Hasher hh;
  fold(hh, t.common);
  hh.i(t.usedLegacyGroup ? 1 : 0);
  hh.i(t.nOpacities);
  for (int p : t.nPoints) hh.i(p);
  for (const auto& s : t.speciesNames) hh.s(s);
  hh.vd(t.offset);
  for (const auto& v : t.values) hh.vd(v);
  hh.i(t.hasEmAbParameters ? 1 : 0);
  hh.i(t.hasECTable ? 1 : 0);
  return hh.h;
}

inline std::uint64_t digest(const wli::io::HostScatIsoTable& t) {
  Hasher hh;
  fold(hh, t.common);
  hh.i(t.nOpacities);
  hh.i(t.nMoments);
  for (int p : t.nPoints) hh.i(p);
  for (const auto& s : t.speciesNames) hh.s(s);
  hh.vd(t.offset);
  for (const auto& v : t.values) hh.vd(v);
  return hh.h;
}

inline std::uint64_t digest(const wli::io::HostScatNESPairTable& t) {
  Hasher hh;
  fold(hh, t.common);
  fold(hh, t.etaGrid);
  hh.i(t.nOpacities);
  hh.i(t.nMoments);
  for (int p : t.nPoints) hh.i(p);
  hh.vd(t.offset);
  hh.vd(t.kernels);
  hh.i(t.hasNPS ? 1 : 0);
  return hh.h;
}

inline std::uint64_t digest(const wli::io::HostScatBremTable& t) {
  Hasher hh;
  fold(hh, t.common);
  hh.i(t.nOpacities);
  hh.i(t.nMoments);
  for (int p : t.nPoints) hh.i(p);
  hh.vd(t.offset);
  hh.vd(t.sSigma);
  return hh.h;
}

}  // namespace test
}  // namespace wli
