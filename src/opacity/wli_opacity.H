#pragma once

// WeakLibInterp opacity kernels — umbrella header. The single-point opacity
// evaluate kernels live in one header per opacity leaf spec:
//   wli_opacity_emab_iso.H  — EmAb 4D + Iso 5D-slice-to-4D + IsoOffset
//                             (specs/opacity-emab-iso.md)
//   wli_opacity_nes_pair.H  — NES/Pair shared 2D-aligned primitive + the two
//                             symmetry fills (specs/opacity-nes-pair.md)
//   wli_opacity_brem.H      — Brem inner + summed aligned kernels
//                             (specs/opacity-brem.md)
// Include this umbrella for everything, or the specific channel header alone.

#include "wli_opacity_brem.H"
#include "wli_opacity_emab_iso.H"
#include "wli_opacity_nes_pair.H"
