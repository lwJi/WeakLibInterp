# EOS inversion (recover T from `(ρ, X∈{E,P,S}, Yₑ)`)

> Leaf spec. Self-contained: an agent can implement the EOS temperature-inversion routine from this file alone. It restates only the conventions it uses and references `fortran-parity-and-tolerances.md` (numeric contract) and `amrex-device-interface.md` (device contract) for the shared cross-cutting details. `README.md` is canonical if any restated convention here conflicts with it.

## Purpose & scope

This spec defines the device-callable inversion of the EOS table: given a density `ρ`, an electron fraction `Yₑ`, and *one* dependent variable value `X` chosen from `{internal energy E, pressure P, entropy-per-baryon S}`, recover the temperature `T` such that interpolating that dependent variable at `(ρ, T, Yₑ)` reproduces `X`. The algorithm is **table-node interval bisection followed by a single log-linear inverse interpolation** — there is no Newton iteration and no floating-point convergence tolerance; "converged" means the bracket has been narrowed to one table interval (`i_b == i_a + 1`). Failure is signaled through an integer error-code protocol, and on any error the output temperature is set to `T = 0`.

In scope:
- The three inversion families `DEY` (X = internal energy), `DPY` (X = pressure), `DSY` (X = entropy per baryon) — identical algorithm, different dependent-variable sub-table.
- The single-point evaluate contract `(ρ, X, Yₑ [, T_guess]) → (T, Error)`, in both the `Guess` (a temperature guess is supplied) and `NoGuess` variants.
- The integer error-code set `{0, 01, 02, 03, 10, 11, 13}`, its meaning, the `DescribeEOSInversionError` mapping, and the `_Error` vs `_NoError` reporting contract.
- The `T = 0`-on-failure signaling convention.
- The log-linear inverse-interpolation final step `InverseLogInterp`.
- The round-trip Layer-1 invariant and its `1e-10` relaxed tolerance, with rationale.
- Boundary / NaN handling specific to inversion (input-bounds validation, NaN detection).

Out of scope:
- The forward direction `(ρ, T, Yₑ) → value` and its derivatives — see `eos-interpolation.md`. (Inversion *uses* the 2D `(ρ, Yₑ)` forward evaluation at fixed temperature nodes internally, but the forward single-variable 3D contract is specified there.)
- The multi-point array (`_Many`) host-level forms (the device contract here is the scalar single-point form; an array form is the host-level `ParallelFor` wrapper over it — see `amrex-device-interface.md`).
- The on-disk HDF5 layout the table/axes/offsets are read from — see `table-format-and-io.md`.
- Opacity channels — see the `opacity-*` specs.

## Source of truth

Pinned weaklib commit: see `weaklib_commit` in `specs/fixtures/tables.provenance`.

- `weaklib/Distributions/EOSSource/wlEOSInversionModule.F90` — the named generator-of-record. The relevant routines:
  - `CheckInputError( D, X, Y, MinX, MaxX )` (function at `wlEOSInversionModule.F90:188-227`) — the input gatekeeper. Returns `10` if inversion is not initialized, `11` if any of `D`/`X`/`Y` is NaN (detected via `D /= D`), `01`/`02`/`03` if `D`/`X`/`Y` respectively is below its table min or above its table max, else `0`.
  - `InverseLogInterp( x_a, x_b, y_a, y_b, y, OS )` (function at `wlEOSInversionModule.F90:253-267`) — the final inversion step, log-linear in both the bracketing temperatures `x_a,x_b` and the dependent-variable values `y_a,y_b`: `10**( log10(x_a) + log10(x_b/x_a) · log10((y+OS)/(y_a+OS)) / log10((y_b+OS)/(y_a+OS)) )`.
  - `ComputeTemperatureWith_DXY_Guess( D, X, Y, Ds, Ts, Ys, Xs, OS, T, T_Guess, Error )` (subroutine at `wlEOSInversionModule.F90:270-440`) — the guess-driven kernel: checks the guess cell first; if `f_a·f_b ≤ 0` there it inverts immediately; otherwise checks the full `[Ts(1), Ts(SizeTs)]` range, bisects on table nodes if it brackets, and if not linear-scans for the sign change nearest the guess index. Sets `Error = 13` if no root is found.
  - `ComputeTemperatureWith_DXY_NoGuess( D, X, Y, Ds, Ts, Ys, Xs, OS, T, Error )` (subroutine at `wlEOSInversionModule.F90:443-573`) — identical algorithm without a guess; on the no-bracket fall-through it selects the **highest-temperature** root.
  - `InitializeEOSInversion( Ds, Ts, Ys, Es, Ps, Ss, Verbose_Option )` (subroutine at `wlEOSInversionModule.F90:139-185`) — caches `MIN/MAXVAL` of each grid/value array into the bounds used by `CheckInputError`; it performs no interpolation.
  - `DescribeEOSInversionError( Error )` (subroutine at `wlEOSInversionModule.F90:230-250`) — maps each code to a human string; aborts (`STOP`) on `Error > 13`.
  - The public wrapper matrix `ComputeTemperatureWith_{DEY,DPY,DSY}_{Single,Many}_{Guess,NoGuess}_{Error,NoError}` (subroutines spanning `wlEOSInversionModule.F90:576-1005`) — thin dispatch over the two `_DXY_` kernels, selecting the `E`/`P`/`S` sub-table; the `_NoError` variants discard the returned code.
- `weaklib/Distributions/Library/wlInterpolationModule.F90` — `LogInterpolateSingleVariable_2D_Custom_Point` (subroutine at `wlInterpolationModule.F90:1115-1165`), the bilinear-in-log evaluation of the dependent variable at a fixed temperature node over the `(ρ, Yₑ)` face that the bisection calls at each candidate `T`.

These `_Point`/`_Single` routines are the generator-of-record for Layer-2 parity (see `fortran-parity-and-tolerances.md`).

## Inputs & outputs

Value type is `double` throughout (weaklib `dp = 8`); see `fortran-parity-and-tolerances.md`. Device contract (qualifiers, `Gpu::DeviceVector<double>` residency, `double const*` table passing, `ParallelFor` launch) is `amrex-device-interface.md`.

### Independent inputs, order and units

| Slot | Symbol | Quantity | Unit | Role |
|---|---|---|---|---|
| 1 | ρ | mass density | grams per cm³ | known (log-located internally) |
| 2 | X | one dependent variable: E, P, or S | E: erg per g; P: dyn per cm²; S: k_B per baryon | the target value to invert for |
| 3 | Yₑ | electron fraction | dimensionless | known (linear-located internally) |
| 4 | T_guess | temperature guess (`Guess` variant only) | K (Kelvin) | optional starting node |

`X` is whichever of internal energy `E`, pressure `P`, or entropy-per-baryon `S` the chosen family (`DEY`/`DPY`/`DSY`) inverts. The units above are the on-disk dependent-variable units (verified in the reference table's `/DependentVariables/Units`; the recovered `T` is in Kelvin).

### Axis arrays, sub-table, and offset

- `Ds(1:nD)`, `Ts(1:nT)`, `Ys(1:nY)` — the strictly monotone-ascending grid-node coordinates for ρ, T, Yₑ (raw physical values; ρ is located in log space, Yₑ in linear space, and the temperature search walks `Ts` node-by-node).
- `Xs` — the log-stored sub-table `log10(physical + OS)` for the chosen dependent variable, indexed `(iD, iT, iY)` in Fortran column-major order: as a flat `double const*` the element `Xs(iD,iT,iY)` (0-based) is `xs[ iD + nD*( iT + nT*iY ) ]` (see `amrex-device-interface.md`). This is the *same* sub-table the forward `eos-interpolation.md` evaluates; inversion holds `(ρ, Yₑ)` fixed and searches over the `T` axis.
- `OS` — the scalar additive offset for the chosen dependent variable.
- Bounds `MinD/MaxD`, `MinX/MaxX`, `MinY/MaxY` — the per-array `MIN/MAXVAL` cached by the initialization step and consumed by the input check; in the C++ contract these are the extents of `Ds`/`Ys` and of the recovered `X` sub-table values.

### Outputs

- `T` — the recovered temperature in Kelvin. On any non-zero `Error`, `T = 0`.
- `Error` — the integer error code (returned by the `_Error` variants; discarded by the `_NoError` variants).

### Reference table (anchors fixtures and Layer-1 checks)

`wl-EOS-SFHo-15-25-50.h5`, pinned by path + `sha256` in `specs/fixtures/tables.provenance`; its structure is committed at `specs/fixtures/wl-EOS-SFHo-15-25-50.h5ls`. The thermodynamic axes are under group `/ThermoState` (`/ThermoState/Density` `[185]`, `/ThermoState/Temperature` `[81]`, `/ThermoState/Electron Fraction` `[30]`), and the invertible dependent variables under group `/DependentVariables` — `/DependentVariables/Internal Energy Density`, `/DependentVariables/Pressure`, `/DependentVariables/Entropy Per Baryon` (each shape `{30, 81, 185}` in `h5ls` C-order = Fortran `(185, 81, 30) = (nρ, nT, nYe)`) — with per-variable additive offsets in `/DependentVariables/Offsets` `[15]`. Real grid extents in this table: ρ ∈ [1.66054e3, 3.16409e15] g/cm³, T ∈ [1.16045e9, 1.83919e12] K, Yₑ ∈ [0.01, 0.6]. Full on-disk contract: `table-format-and-io.md`.

## Correctness requirements

### Algorithm contract

Inversion recovers `T` from `(ρ, X, Yₑ)` by interval bisection on the temperature *nodes* of `Ts`, followed by one log-linear inverse interpolation. The exact, observable contract:

1. **Input check (runs first).** Compute the error code exactly as `CheckInputError`:
   - `10` if inversion bounds were never initialized (uninitialized state);
   - `11` if any input is NaN (the `value /= value` test);
   - `01` if `ρ < MinD` or `ρ > MaxD`; `02` if `X < MinX` or `X > MaxX`; `03` if `Yₑ < MinY` or `Yₑ > MaxY`;
   - `0` otherwise. The order matters: uninitialized is checked before NaN, NaN before bounds, and `D` before `X` before `Y`.
2. **Fixed-`T`-node evaluation.** At each candidate temperature node `Ts(i)`, the dependent variable at `(ρ, Yₑ)` is evaluated by the bilinear-in-log forward routine over the `(ρ, Yₑ)` face: `X_i = LogInterpolateSingleVariable_2D_Custom_Point(log10(ρ), Yₑ, log10(Ds(iD:iD+1)), Ys(iY:iY+1), OS, Xs(iD:iD+1, i, iY:iY+1))`. The residual is `f_i = X − X_i`.
3. **Bracket search (convergence = one table interval).** Locate adjacent nodes `i_a, i_b = i_a + 1` whose residuals satisfy `f_a · f_b ≤ 0` (a sign change brackets the root). The `Guess` variant tries the guess cell `(iT, iT+1)` first, then the full range with node bisection, then a nearest-to-guess linear scan; the `NoGuess` variant does the same but on the no-bracket fall-through picks the **highest-temperature** root. The only convergence criterion is `i_b == i_a + 1`; there is **no floating-point tolerance** and no iteration-count limit beyond exhausting the `nT` nodes.
4. **No root → `Error = 13`.** If no sign-change bracket exists across all nodes, set `Error = 13`.
5. **Inverse interpolation (the final step).** With the bracketing nodes `(T_a, X_a)`, `(T_b, X_b)`, recover `T` log-linearly in both `T` and `X`:
   ```
   T = 10**( log10(T_a) + log10(T_b/T_a) · log10((X+OS)/(X_a+OS)) / log10((X_b+OS)/(X_a+OS)) )
   ```
   This is `InverseLogInterp(T_a, T_b, X_a, X_b, X, OS)`; it is the algebraic inverse of the log-linear forward interpolation in `T`.
6. **Failure output.** On any non-zero `Error`, set `T = 0` and return; the recovered `T` is meaningful only when `Error == 0`.

### Error-code protocol

The complete set, matching `DescribeEOSInversionError`:

| Code | Meaning |
|---|---|
| 0 | Returned successfully |
| 01 | First argument `D` (ρ) outside table bounds |
| 02 | Second argument `X` (E, P, or S) outside table bounds |
| 03 | Third argument `Y` (Yₑ) outside table bounds |
| 10 | EOS inversion not initialized |
| 11 | NaN in argument(s) |
| 13 | Unable to find any root |

Codes `04–09` and `12` are undefined and never produced; a code `> 13` is a programming error (`DescribeEOSInversionError` aborts on it). The C++ contract must produce exactly these codes under exactly these conditions.

### `_Error` vs `_NoError` reporting (must be unambiguous)

- The `_Error` variants return the integer `Error` to the caller (the caller may map it to a string via the `DescribeEOSInversionError` mapping above). The recovered `T` is valid iff `Error == 0`.
- The `_NoError` variants discard the code. Because failure also sets `T = 0`, **`T = 0` is the only failure signal available to a `_NoError` caller** — a returned `T == 0` means "inversion failed" and any non-zero `T` is a success. A C++ port must preserve this: `_NoError` callers cannot distinguish *why* it failed, only *that* it did, via `T = 0`.

### Round-trip invariant (the Layer-1 closure check)

The inversion is the inverse of the forward dependent-variable interpolation, so for an in-bounds query the composition must close:

```
GIVEN  in-bounds (ρ, T_true, Yₑ) on reference table wl-EOS-SFHo-15-25-50.h5,
       and X = forward_interp_X(ρ, T_true, Yₑ)            # E, P, or S
WHEN   T_recovered = invert_X(ρ, X, Yₑ)
THEN   |T_recovered - T_true| / (|T_true| + atol) <= 1e-10   (relaxed tier; atol = 1e-30)
  AND  re-evaluating X_recovered = forward_interp_X(ρ, T_recovered, Yₑ)
       satisfies |X_recovered - X| / (|X| + atol) <= 1e-10.
```

The check that "counts as failure" is the relative residual exceeding `1e-10`, on a query that is strictly in-bounds and returns `Error == 0`. (Recovered `T` need not equal `T_true` to machine precision because inversion is a log-linear interpolation between table nodes, not an exact root solve; the invariant that is exact-to-tolerance is the *re-evaluation* residual on `X`.)

**Rationale for the `1e-10` relaxation:** inversion is interpolation-of-an-interpolation — the recovered `T` is produced by a log-linear inverse of values that are themselves bilinear-in-log interpolants, so order-of-operations and the transcendental `log10`/`10**` round-trip accumulate beyond the `1e-12` single-value tier. `1e-10` bounds that accumulation while still catching real regressions.

### Boundary / NaN behavior (bit-for-bit with weaklib)

- Inversion is the one EOS path with explicit input validation: out-of-bounds `(ρ, X, Yₑ)` returns the corresponding code `01`/`02`/`03` and `T = 0` — it does **not** extrapolate (unlike the forward interpolator). NaN inputs return `11` and `T = 0`.
- The internal fixed-`T`-node evaluation reuses the forward bilinear-in-log routine, which itself clamps the bracket index but not the delta and NaNs on a non-positive log argument (see `eos-interpolation.md` / `fortran-parity-and-tolerances.md`); but because the input check rejects out-of-bounds and NaN inputs first, those forward edge cases are not reached for valid inversion queries.

### Conventions restated (the subset this leaf uses)

- ρ located in log space, Yₑ in linear space; the dependent variable is log-stored `log10(value + offset)` and the inverse recovers via the `InverseLogInterp` formula above.
- Universal forward recovery `value = 10**(stored) - offset`.
- Fortran column-major sub-table: first index (ρ) fastest-varying; flat offset `iD + nD*(iT + nT*iY)`.
- `double` (`dp = 8`) throughout.

## Verification

### Layer 1 — self-contained checks (the active gate)

Run against both synthetic in-suite tables and the real reference table `wl-EOS-SFHo-15-25-50.h5`:

1. **Round-trip recovery (relaxed tier `1e-10`).** For in-bounds `(ρ, T_true, Yₑ)`: forward-evaluate `X`, invert to `T_recovered`, and assert both `|T_recovered − T_true| / (|T_true| + atol) ≤ 1e-10` and the re-evaluation residual `|forward_X(ρ, T_recovered, Yₑ) − X| / (|X| + atol) ≤ 1e-10`. Run for all three families (DEY, DPY, DSY) and with both `Guess` (good guess) and `NoGuess` variants.
2. **Affine-in-log exactness (machine-precision tier `~1e-14`).** On a synthetic sub-table whose stored value is exactly affine in `(log10 ρ, log10 T, Yₑ)`, the log-linear inverse is exact: a query `X` taken from an interior `(ρ, T, Yₑ)` must recover that `T` to machine precision (the inverse-interpolation formula is the algebraic inverse of the affine forward map). The constant-in-`T` sub-table is the degenerate "no root / flat" case — see check 5.
3. **Error codes (exact-equality).** Construct queries that trigger each code and assert the returned `Error` equals it: out-of-bounds ρ → `01`; out-of-bounds X → `02`; out-of-bounds Yₑ → `03`; an uninitialized-bounds state → `10`; a NaN input → `11`; a value with no sign-change bracket (e.g. `X` strictly outside the table's `[X_min, X_max]` along `T` at fixed ρ, Yₑ, or a monotone sub-table queried beyond its range) → `13`. In every non-zero-code case assert `T == 0`.
4. **`T = 0`-on-failure signaling (exact-equality).** Confirm that the `_NoError`-style path returns `T == 0` for every failing query and a non-zero `T` for every succeeding one — the sole failure signal when the code is discarded.
5. **No-root / non-monotone handling.** For a sub-table that is non-monotone in `T` at fixed `(ρ, Yₑ)`, confirm the `NoGuess` variant selects the highest-temperature root among the sign-change brackets, and the `Guess` variant selects the root nearest the guess index; a target with no bracket returns `Error = 13`, `T = 0`.

### Layer 2 — Fortran parity (specified, PENDING)

Compare recovered `T` (relaxed tier `1e-10`) and the returned `Error` codes (exact-equality) against committed golden fixtures generated offline by the `ComputeTemperatureWith_{DEY,DPY,DSY}_Single_{Guess,NoGuess}_Error` routines at the pinned commit, over a query set drawn from `wl-EOS-SFHo-15-25-50.h5` (in-bounds, on-edge, out-of-range, and NaN inputs to exercise every code). Until fixtures exist these tests report **pending**, not passing (see `fortran-parity-and-tolerances.md`).

### Mechanical (validator)

`bash specs/tools/validate_specs.sh` (default mode) asserts: the 7 mandated sections in order; the full error-code set `{0, 01, 02, 03, 10, 11, 13}` is documented; the inversion source-of-truth file `wlEOSInversionModule.F90` resolves; the `InverseLogInterp` and `ComputeTemperatureWith_DXY_*` routine names are present; the `1e-10` round-trip relaxation is named; and the documented `/DependentVariables/Internal Energy Density` and `/ThermoState/Temperature` structures appear in the committed `wl-EOS-SFHo-15-25-50.h5ls` snapshot with the table named in this spec.

## Implementation freedom

- The internal bracket-search structure (how the guess cell, full-range bisection, and nearest-to-guess scan are organized), provided the observable `(T, Error)` contract and the `i_b == i_a + 1` convergence criterion are met.
- Whether the fixed-`T`-node evaluation calls the shared forward bilinear core or an inlined equivalent, provided results meet tolerance.
- Whether the three families (DEY/DPY/DSY) are separate entry points, a single entry point parameterized by the sub-table + bounds, or templated; likewise `Guess`/`NoGuess` and `Error`/`NoError`.
- How the cached bounds (`MinD/MaxD`, `MinX/MaxX`, `MinY/MaxY`) and the "initialized" state are represented and supplied to the device-callable check.
- Whether the multi-point (`_Many`) form is a hand-written loop or a `ParallelFor` over the single-point core.

## Open questions / assumptions

- **Layer-2 golden fixtures are future work (assumption, non-blocking).** No Fortran-generated golden inversion outputs exist or can be generated in this environment; Layer-2 tests ship **pending**, the named `ComputeTemperatureWith_*_Error` routines remain the generator-of-record, and the Ralph loop gates on Layer 1. See `fortran-parity-and-tolerances.md`.
- **Concrete per-variable offsets and value bounds (assumption, non-blocking).** The `OS` offsets and the `MinX/MaxX` value extents for E/P/S live only in the `.h5` file (`/DependentVariables/Offsets`, and the `MIN/MAXVAL` of each sub-table; research OQ#3). This spec pins the algorithm and the bounds-check contract; the fixture/table supplies the numbers. Layer-1 round-trip checks use the offsets/bounds read from the chosen (synthetic or real) table, so they do not depend on hard-coded production values.
- **Initialization-state representation (assumption, non-blocking).** weaklib carries a module-level `InversionInitialized` flag (and module-global bounds scalars) that `CheckInputError` consults for code `10`. The C++ port is free to represent this however it likes (e.g. an explicit bounds struct passed to the device function); the only fixed contract is that querying with uninitialized/absent bounds yields code `10`.
- **Highest-temperature-root tie-breaking (assumption, non-blocking).** The `NoGuess` fall-through selects the highest-temperature root and the `Guess` fall-through the nearest-to-guess root, mirroring weaklib; for well-posed monotone sub-tables there is a single root and the tie-break is irrelevant. The non-monotone tie-break behavior is pinned to the weaklib routines named in "Source of truth".
