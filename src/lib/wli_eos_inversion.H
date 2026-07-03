#pragma once

// WeakLibInterp EOS temperature-inversion core, DEY family (header-only).
//
// 0-based C++ port of the DEY temperature-inversion routines in the oracle
//   weaklib/Distributions/EOSSource/wlEOSInversionModule.F90
//     CheckInputError                    (:188-227)  — input gatekeeper cascade
//     InverseLogInterp                   (:253-267)  — final log-linear inverse
//     ComputeTemperatureWith_DXY_Guess   (:270-440)  — guess-driven kernel
//     ComputeTemperatureWith_DXY_NoGuess (:443-573)  — highest-T-root kernel
//     ComputeTemperatureWith_DEY_*       (:576-1005) — input-check-then-dispatch
//   weaklib/Distributions/Library/wlInterpolationModule.F90
//     LogInterpolateSingleVariable_2D_Custom_Point (:1115-1165) — the fixed-T
//       (rho,Ye) face evaluation the bisection calls at each candidate node.
//
// Inversion recovers T from (rho, X, Ye) by interval bisection on the T *nodes*
// of Ts, followed by ONE log-linear inverse interpolation (specs/
// eos-inversion.md:73-90). There is no Newton iteration and no floating-point
// convergence tolerance: "converged" means the bracket is one table interval
// (i_b == i_a + 1). Failure is signaled by an integer error code and, on any
// non-zero code, T = 0 (the sole failure signal for a code-discarding caller).
//
// These kernels reuse the stateless primitives (wli_interp.H bracket/delta +
// BiLinear + recover, wli_index.H flat_index) — they re-derive none of the
// interpolation math. Every function is scalar, allocation-free, has no
// amrex::Initialize dependency, and uses wli::Real (= double) throughout, never
// amrex::Real. Raw Real const* + int extents are passed (never a TableView).
//
// Scope: DEY only, via a GENERIC _DXY core so the DPY/DSY wrappers (next
// increment) are wrapper-only. The _Many array form, the DescribeEOSInversionError
// string table, and the _Error/_NoError naming matrix are out of scope here.

#include <cmath>

#include <AMReX_Array.H>  // AMREX_GPU_HOST_DEVICE / AMREX_FORCE_INLINE / GpuArray

#include "wli_index.H"
#include "wli_interp.H"
#include "wli_real.H"

namespace wli {

// Per-family cached bounds + init flag. Represents the Fortran module state
// (InversionInitialized, MinD/MaxD, MinE/MaxE, MinY/MaxY) consumed by
// CheckInputError. MinT/MaxT are intentionally omitted: CheckInputError never
// reads T bounds (oracle :188-227). `initialized` defaults false so a
// default-constructed struct yields code 10 (uninitialized).
struct EosInversionBounds {
  Real MinD, MaxD;  // rho bounds (family-independent)
  Real MinX, MaxX;  // dependent-variable bounds (here E: MinE/MaxE)
  Real MinY, MaxY;  // Ye bounds (family-independent)
  bool initialized = false;
};

// Wrapper return: recovered T (K) and the integer error code. On any non-zero
// Error, T == 0 (spec:108-111, the sole failure signal for a NoError caller).
struct EosInversionResult {
  Real T;
  int  Error;
};

// Input gatekeeper cascade (oracle :198-219; spec:77-81). First match wins;
// order matters: uninitialized before NaN, NaN before bounds, D before X before
// Y. Codes: 0 ok, 1 D out of range, 2 X out of range, 3 Y out of range,
// 10 uninitialized, 11 NaN input (self-inequality test).
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
int CheckInputError(Real D, Real X, Real Y,
                    EosInversionBounds const& b) noexcept {
  if (!b.initialized) return 10;
  if (D != D || X != X || Y != Y) return 11;  // NaN self-inequality
  if (D < b.MinD || D > b.MaxD) return 1;
  if (X < b.MinX || X > b.MaxX) return 2;
  if (Y < b.MinY || Y > b.MaxY) return 3;
  return 0;
}

// Final inversion step (oracle :262-264; spec:86-89): log-linear in both the
// bracketing temperatures (x_a, x_b) and the dependent-variable values
// (y_a, y_b). Always called InverseLogInterp(T_a, T_b, X_a, X_b, X, OS): x_a/x_b
// are bracketing TEMPERATURES, y_a/y_b bracketing dependent values, y = X the
// target. The +OS here re-adds the offset to already-recovered physical values
// — a SECOND, distinct OS use vs the face eval's -OS; kept textually separate.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real InverseLogInterp(Real x_a, Real x_b, Real y_a, Real y_b, Real y,
                      Real OS) noexcept {
  return std::pow(Real(10),
                  std::log10(x_a) +
                      std::log10(x_b / x_a) *
                          std::log10((y + OS) / (y_a + OS)) /
                          std::log10((y_b + OS) / (y_a + OS)));
}

// Dependent variable at the (rho, Ye) face for a fixed temperature node iTnode
// (oracle :1129-1165; spec:82). The 3D sub-table Xs is flat column-major with
// rho fastest-varying: Xs[iD + nD*(iT + nT*iY)]. (iD,dD)/(iY,dY) are the ONE-TIME
// full-axis brackets — do NOT recompute per node. Bilinear-in-log then recover:
// -OS lives inside recover (distinct from InverseLogInterp's +OS).
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
Real EosInvertFaceEval(int iTnode, int iD, Real dD, int iY, Real dY, int nD,
                       int nT, int nY, Real OS, Real const* Xs) noexcept {
  amrex::GpuArray<int, 3> const n{nD, nT, nY};
  Real const p00 = Xs[flat_index<3>({iD,     iTnode, iY    }, n)];
  Real const p10 = Xs[flat_index<3>({iD + 1, iTnode, iY    }, n)];
  Real const p01 = Xs[flat_index<3>({iD,     iTnode, iY + 1}, n)];
  Real const p11 = Xs[flat_index<3>({iD + 1, iTnode, iY + 1}, n)];
  return recover(BiLinear(p00, p10, p01, p11, dD, dY), OS);  // dX1=dD, dX2=dY
}

// Generic no-guess kernel (0-based port of oracle :469-571). Returns Error and
// writes T. Full-range bracket -> node bisection; else a highest-T-root
// descending scan (spec:83, :152). Only convergence criterion is i_b==i_a+1;
// NO floating-point tolerance. On error T=0.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
int ComputeTemperatureWith_DXY_NoGuess(
    Real D, Real X, Real Y, Real const* Ds, int nD, Real const* Ts, int nT,
    Real const* Ys, int nY, Real OS, Real const* Xs, Real& T) noexcept {
  int Error = 0;

  // One-time full-axis brackets; deltas reused across all T-node faces.
  auto const idD = GetIndexAndDeltaLog(D, Ds, nD);
  auto const idY = GetIndexAndDeltaLin(Y, Ys, nY);
  int const iD = idD.i;
  Real const dD = idD.d;
  int const iY = idY.i;
  Real const dY = idY.d;

  auto face = [&](int i) {
    return EosInvertFaceEval(i, iD, dD, iY, dY, nD, nT, nY, OS, Xs);
  };

  int  i_a = 0, i_b = nT - 1;
  Real T_a = Ts[i_a], T_b = Ts[i_b];
  Real X_a = face(i_a), X_b = face(i_b);
  Real f_a = X - X_a, f_b = X - X_b;

  if (f_a * f_b <= Real(0)) {  // full-range bracket -> node bisection
    while (i_b > i_a + 1) {
      int  i_c = std::max(i_a + 1, (i_a + i_b) / 2);  // floor int-div
      Real T_c = Ts[i_c], X_c = face(i_c), f_c = X - X_c;
      if (f_a * f_c <= Real(0)) {
        i_b = i_c; T_b = T_c; X_b = X_c; f_b = f_c;
      } else {
        i_a = i_c; T_a = T_c; X_a = X_c; f_a = f_c;
      }
    }
  } else {  // highest-T-root descending scan (Fortran DO i=SizeTs-1,2,-1)
    for (int i = nT - 2; i >= 1; --i) {
      Real T_i = Ts[i], X_i = face(i);
      f_a = X - X_i;  // candidate lower node
      f_b = X - X_b;  // current upper node (carried from full-range top)
      if (f_a * f_b <= Real(0)) {
        i_a = i; T_a = T_i; X_a = X_i;  // first top-down hit = highest-T root
        break;
      } else {
        i_b = i; T_b = T_i; X_b = X_i;  // shift window down, keep scanning
      }
    }
    // Re-derive: X_a is the EXIT hit OR the untouched node-0 value; this checks
    // the (node0,node1) fallback pair -- the ONLY place 13 is set here.
    f_a = X - X_a;
    f_b = X - X_b;
    if (f_a * f_b > Real(0)) Error = 13;
  }

  if (Error != 0)
    T = Real(0);
  else
    T = InverseLogInterp(T_a, T_b, X_a, X_b, X, OS);
  return Error;
}

// Generic guess-driven kernel (0-based port of oracle :299-438). Returns Error
// and writes T. Tries the guess cell first, then full-range bisection (with a
// guess-biased first midpoint), then a nearest-to-guess ascending scan (spec:83,
// :152). On error T=0.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
int ComputeTemperatureWith_DXY_Guess(
    Real D, Real X, Real Y, Real const* Ds, int nD, Real const* Ts, int nT,
    Real const* Ys, int nY, Real OS, Real const* Xs, Real T_Guess,
    Real& T) noexcept {
  int Error = 0;

  auto const idD = GetIndexAndDeltaLog(D, Ds, nD);
  auto const idY = GetIndexAndDeltaLin(Y, Ys, nY);
  int const iD = idD.i;
  Real const dD = idD.d;
  int const iY = idY.i;
  Real const dY = idY.d;
  int const iT = GetIndexAndDeltaLog(T_Guess, Ts, nT).i;  // clamped [0,nT-2]

  auto face = [&](int i) {
    return EosInvertFaceEval(i, iD, dD, iY, dY, nD, nT, nY, OS, Xs);
  };

  // --- Step 1: guess cell (iT, iT+1). Immediate return on a bracket. ---
  {
    Real X_a = face(iT), X_b = face(iT + 1);
    if ((X - X_a) * (X - X_b) <= Real(0)) {
      T = InverseLogInterp(Ts[iT], Ts[iT + 1], X_a, X_b, X, OS);
      return 0;
    }
  }

  // --- Step 2: full-range bracket (rebuilt exactly as NoGuess). ---
  int  i_a = 0, i_b = nT - 1;
  Real T_a = Ts[i_a], T_b = Ts[i_b];
  Real X_a = face(i_a), X_b = face(i_b);
  Real f_a = X - X_a, f_b = X - X_b;

  if (f_a * f_b <= Real(0)) {  // guess-biased node bisection
    int i_c = std::min(std::max(i_a + 1, iT), i_b - 1);  // oracle :368
    while (i_b > i_a + 1) {
      Real T_c = Ts[i_c], X_c = face(i_c), f_c = X - X_c;
      if (f_a * f_c <= Real(0)) {
        i_b = i_c; T_b = T_c; X_b = X_c; f_b = f_c;
      } else {
        i_a = i_c; T_a = T_c; X_a = X_c; f_a = f_c;
      }
      i_c = std::max(i_a + 1, (i_a + i_b) / 2);  // recompute at loop bottom
    }
  } else {  // --- Step 3: nearest-to-guess ASCENDING scan (no early exit). ---
    int  d_i = nT;                        // sentinel "not found"
    int  i_c_t = clamp_index(iT, 0, nT - 2);  // guess node for distance metric
    Real T_c = T_a, X_c = X_a;            // running previous node, seeded node 0
    for (int i = 1; i <= nT - 2; ++i) {   // Fortran DO i=2,SizeTs-1
      Real T_i = Ts[i], X_i = face(i);
      Real f_prev = X - X_c, f_cur = X - X_i;
      int  d_c = std::abs(i - i_c_t);
      if (f_prev * f_cur <= Real(0) && d_c < d_i) {
        // Record best-so-far bracket (i-1, i); keep scanning. T_c/X_c are NOT
        // advanced here (running window freezes on a record).
        d_i = d_c;
        i_a = i - 1; T_a = T_c; X_a = X_c;
        i_b = i;     T_b = T_i; X_b = X_i;
      } else {
        T_c = T_i; X_c = X_i;  // advance running window ONLY in the else branch
      }
    }
    if (d_i >= nT) Error = 13;
  }

  if (Error != 0)
    T = Real(0);
  else
    T = InverseLogInterp(T_a, T_b, X_a, X_b, X, OS);
  return Error;
}

// DEY no-guess wrapper (oracle :616-717): input-check-then-dispatch. T is
// pre-zeroed AND re-zeroed on any failure = the spec's sole failure signal
// (spec:108-111).
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
EosInversionResult ComputeTemperatureWith_DEY_NoGuess(
    Real D, Real E, Real Y, Real const* Ds, int nD, Real const* Ts, int nT,
    Real const* Ys, int nY, Real OS, Real const* Es,
    EosInversionBounds const& b) noexcept {
  Real T = Real(0);
  int  Error = CheckInputError(D, E, Y, b);
  if (Error == 0)
    Error = ComputeTemperatureWith_DXY_NoGuess(D, E, Y, Ds, nD, Ts, nT, Ys, nY,
                                               OS, Es, T);
  if (Error != 0) T = Real(0);
  return EosInversionResult{T, Error};
}

// DEY guess-driven wrapper (oracle :616-717): same cascade, extra T_Guess.
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
EosInversionResult ComputeTemperatureWith_DEY_Guess(
    Real D, Real E, Real Y, Real const* Ds, int nD, Real const* Ts, int nT,
    Real const* Ys, int nY, Real OS, Real const* Es, Real T_Guess,
    EosInversionBounds const& b) noexcept {
  Real T = Real(0);
  int  Error = CheckInputError(D, E, Y, b);
  if (Error == 0)
    Error = ComputeTemperatureWith_DXY_Guess(D, E, Y, Ds, nD, Ts, nT, Ys, nY,
                                              OS, Es, T_Guess, T);
  if (Error != 0) T = Real(0);
  return EosInversionResult{T, Error};
}

}  // namespace wli
