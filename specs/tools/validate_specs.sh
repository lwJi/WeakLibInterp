#!/usr/bin/env bash
#
# validate_specs.sh — spec-set linter / harness for the WeakLibInterp `specs/` set.
#
# PURPOSE
#   Mechanically enforce that every leaf spec is a complete, self-contained Ralph-loop
#   contract and that the README index is internally consistent — WITHOUT needing the
#   multi-GB production HDF5 tables. The default mode is fully CI-reproducible: it relies
#   only on files committed under specs/ (the specs themselves, the README index, the
#   Fortran/AMReX sibling source trees, and the committed structural snapshots
#   specs/fixtures/*.h5ls).
#
# TWO MODES
#   Default (no WL_TABLES_ROOT):
#     For each registered leaf spec:
#       (a) the 7 mandated section headers are present, in order;
#       (b) it names >= 1 concrete numeric tolerance;
#       (c) every "Source of truth" Fortran/AMReX path it cites resolves to a real file
#           under $WEAKLIB_ROOT / $AMREX_ROOT;
#       (d) every reference table it cites has a committed specs/fixtures/<table>.h5ls
#           snapshot, and any per-spec structural claims registered below are present in
#           that snapshot.
#     For the README:
#       (e) the index table links every registered spec file, every linked file exists,
#           and there are no broken intra-repo links and no orphan spec files.
#     The committed snapshots are themselves checksummed against tables.provenance so a
#     silent edit of a snapshot is caught.
#
#   Provenance / refresh (opt-in, WL_TABLES_ROOT=<dir-with-live-tables>):
#     Re-runs `h5ls -r` + `shasum -a 256` against each live .h5 table named in
#     tables.provenance, confirming the committed *.h5ls snapshot still matches reality
#     and the recorded table_sha256 is current. When WL_TABLES_ROOT is unset this stage
#     prints an explicit "SKIPPED (WL_TABLES_ROOT unset)" line — it never passes silently.
#
# EXTENDING (later phases)
#   Add one register_spec line per new leaf spec in the REGISTRY section below; optionally
#   add require_in_snapshot / require_in_spec assertions for that spec's concrete claims.
#   No other part of this script needs editing to grow coverage.
#
# EXIT: 0 if every check passes, non-zero on the first category that fails (all failures
# within a run are reported before exiting).

set -u

# --------------------------------------------------------------------------------------
# Paths
# --------------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPECS_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$SPECS_DIR/.." && pwd)"
FIXTURES_DIR="$SPECS_DIR/fixtures"
README="$SPECS_DIR/README.md"
PROVENANCE="$FIXTURES_DIR/tables.provenance"

# Sibling source-of-truth repos. Overridable via environment for CI portability; if
# unset, search a few candidate locations and pick the first that actually contains a
# probe file (so a symlinked repo root or a non-adjacent checkout still resolves).
resolve_root() {
  # $1 = override value (may be empty); $2 = probe file relative to root; $3.. = candidates.
  # Honor the override only if it actually contains the probe; otherwise fall through to
  # the candidate search (so a stale/incomplete env override self-heals on this checkout).
  local override="$1" probe="$2"; shift 2
  if [ -n "$override" ] && [ -f "$override/$probe" ]; then echo "$override"; return; fi
  local cand
  for cand in "$@"; do
    [ -n "$cand" ] || continue
    if [ -f "$cand/$probe" ]; then echo "$cand"; return; fi
  done
  # Nothing resolved: echo the override (or first candidate) for a meaningful error.
  echo "${override:-$1}"
}

PARENT="$(dirname "$REPO_ROOT")"
WEAKLIB_ROOT="$(resolve_root "${WEAKLIB_ROOT:-}" "Distributions/Library/wlInterpolationModule.F90" \
  "$PARENT/weaklib" "$HOME/docker-workspace/repos/weaklib")"
AMREX_ROOT="$(resolve_root "${AMREX_ROOT:-}" "Src/Base/AMReX_GpuContainers.H" \
  "$PARENT/amrex" "$HOME/docker-workspace/repos/amrex")"

FAILURES=0
fail()  { echo "FAIL: $*" >&2; FAILURES=$((FAILURES + 1)); }
pass()  { echo "ok:   $*"; }
info()  { echo "      $*"; }

# --------------------------------------------------------------------------------------
# The 7 mandated section headers, in order. Every leaf spec must contain these `## `
# headers in exactly this sequence (extra `## ` subsections may appear after, but these
# seven anchors must appear and be ordered).
# --------------------------------------------------------------------------------------
SECTION_HEADERS=(
  "Purpose & scope"
  "Source of truth"
  "Inputs & outputs"
  "Correctness requirements"
  "Verification"
  "Implementation freedom"
  "Open questions / assumptions"
)

# --------------------------------------------------------------------------------------
# REGISTRY
# --------------------------------------------------------------------------------------
# REGISTERED_SPECS : leaf spec basenames that must follow the 7-section template AND be
#                    linked from the README index. (Pure cross-cutting / technical specs
#                    can also be registered; the section + tolerance + source-of-truth
#                    checks apply to all of them.)
# Later phases append to this array (and to the assertion lists below).
REGISTERED_SPECS=(
  "fortran-parity-and-tolerances.md"
  "amrex-device-interface.md"
  "eos-interpolation.md"
  "eos-inversion.md"
  "table-format-and-io.md"
  "opacity-emab-iso.md"
  "opacity-nes-pair.md"
)

# Per-spec extra assertions, expressed as "specfile|||needle".
# require_in_spec  : the needle (a fixed string) must appear somewhere in the spec file.
# require_in_snapshot : "specfile|||snapshot.h5ls|||needle" — the needle must appear in
#                       the named committed structural snapshot AND the spec must cite
#                       that snapshot's table by name.
SPEC_REQUIRE_IN_SPEC=(
  "eos-interpolation.md|||LogInterpolateSingleVariable_3D_Custom_Point"
  "eos-interpolation.md|||LogInterpolateDifferentiateSingleVariable_3D_Custom_Point"
  "eos-interpolation.md|||1e-10"
  "fortran-parity-and-tolerances.md|||1e-12"
  "fortran-parity-and-tolerances.md|||1e-30"
  "eos-inversion.md|||InverseLogInterp"
  "eos-inversion.md|||ComputeTemperatureWith_DXY_Guess"
  "eos-inversion.md|||ComputeTemperatureWith_DXY_NoGuess"
  "eos-inversion.md|||1e-10"
  "eos-inversion.md|||wlEOSInversionModule.F90"
  "table-format-and-io.md|||H5T_NATIVE_DOUBLE"
  "table-format-and-io.md|||column-major"
  "table-format-and-io.md|||EmAb_CorrectedAbsorption"
  "table-format-and-io.md|||wlEOSIOModuleHDF.f90"
  "table-format-and-io.md|||wlOpacityTableIOModuleHDF.f90"
  "opacity-emab-iso.md|||LogInterpolateSingleVariable_4D_Custom_Point"
  "opacity-emab-iso.md|||wlInterpolationModule.F90"
  "opacity-emab-iso.md|||wlOpacityFieldsModule.f90"
  "opacity-nes-pair.md|||LogInterpolateSingleVariable_2D2D_Custom_Aligned"
  "opacity-nes-pair.md|||exp((E − E') / T)"
  "opacity-nes-pair.md|||crossing symmetry"
  "opacity-nes-pair.md|||detailed balance"
  "opacity-nes-pair.md|||wlInterpolationModule.F90"
  "opacity-nes-pair.md|||wlOpacityInterpolationModule.f90"
)

# Inversion error-code set: every code must be documented in the inversion spec.
SPEC_REQUIRE_ERROR_CODES=(
  "eos-inversion.md|||0"
  "eos-inversion.md|||01"
  "eos-inversion.md|||02"
  "eos-inversion.md|||03"
  "eos-inversion.md|||10"
  "eos-inversion.md|||11"
  "eos-inversion.md|||13"
)

SPEC_REQUIRE_IN_SNAPSHOT=(
  "eos-interpolation.md|||wl-EOS-SFHo-15-25-50.h5ls|||/ThermoState/Density"
  "eos-interpolation.md|||wl-EOS-SFHo-15-25-50.h5ls|||/DependentVariables/Pressure"
  "eos-inversion.md|||wl-EOS-SFHo-15-25-50.h5ls|||/ThermoState/Temperature"
  "eos-inversion.md|||wl-EOS-SFHo-15-25-50.h5ls|||/DependentVariables/Pressure"
  "table-format-and-io.md|||wl-EOS-SFHo-15-25-50.h5ls|||/ThermoState/Density"
  "table-format-and-io.md|||wl-EOS-SFHo-15-25-50.h5ls|||/DependentVariables/Offsets"
  "table-format-and-io.md|||wl-Op-SFHo-15-25-50-E40-EmAb.h5ls|||/EmAb/Offsets"
  "table-format-and-io.md|||wl-Op-SFHo-15-25-50-E40-Iso.h5ls|||/Scat_Iso_Kernels/Offsets"
  "table-format-and-io.md|||wl-Op-SFHo-15-25-50-E40-NES.h5ls|||/Scat_NES_Kernels/Kernels"
  "table-format-and-io.md|||wl-Op-SFHo-15-25-50-E40-Pair.h5ls|||/Scat_Pair_Kernels/Kernels"
  "table-format-and-io.md|||wl-Op-SFHo-15-25-50-E40-Brem.h5ls|||/Scat_Brem_Kernels/S_sigma"
  # opacity-emab-iso: anchor the EmAb-1D vs Iso-2D offset-dimensionality claims to the
  # committed snapshots — the EmAb /EmAb/Offsets is a 1D {2} dataset, the Iso
  # /Scat_Iso_Kernels/Offsets is a 2D {2, 2} (nOpacities, nMoments) dataset.
  "opacity-emab-iso.md|||wl-Op-SFHo-15-25-50-E40-EmAb.h5ls|||/EmAb/Offsets            Dataset {2}"
  "opacity-emab-iso.md|||wl-Op-SFHo-15-25-50-E40-Iso.h5ls|||/Scat_Iso_Kernels/Offsets Dataset {2, 2}"
  # opacity-nes-pair: anchor the 5D kernel-table geometry and the 2D offset-dimensionality
  # claims to the committed snapshots — both NES and Pair carry a {120, 81, 4, 40, 40}
  # Kernels dataset (Fortran (nE', nE, nMom, nT, nEta)) and a 2D {4, 1} Offsets dataset.
  "opacity-nes-pair.md|||wl-Op-SFHo-15-25-50-E40-NES.h5ls|||/Scat_NES_Kernels/Kernels Dataset {120, 81, 4, 40, 40}"
  "opacity-nes-pair.md|||wl-Op-SFHo-15-25-50-E40-NES.h5ls|||/Scat_NES_Kernels/Offsets Dataset {4, 1}"
  "opacity-nes-pair.md|||wl-Op-SFHo-15-25-50-E40-Pair.h5ls|||/Scat_Pair_Kernels/Kernels Dataset {120, 81, 4, 40, 40}"
  "opacity-nes-pair.md|||wl-Op-SFHo-15-25-50-E40-Pair.h5ls|||/Scat_Pair_Kernels/Offsets Dataset {4, 1}"
)

# --------------------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------------------

# Extract the ordered list of `## ` header titles (trimmed) from a markdown file.
spec_headers() {
  # shellcheck disable=SC2016
  grep -E '^## ' "$1" | sed -E 's/^##[[:space:]]+//; s/[[:space:]]+$//'
}

# Check the 7 mandated headers appear in order. The spec's `## ` headers, filtered to the
# mandated set, must equal the mandated sequence as a prefix-in-order match.
check_section_order() {
  local file="$1" base; base="$(basename "$file")"
  local -a found=()
  while IFS= read -r h; do
    for want in "${SECTION_HEADERS[@]}"; do
      if [ "$h" = "$want" ]; then found+=("$h"); break; fi
    done
  done < <(spec_headers "$file")

  if [ "${#found[@]}" -ne "${#SECTION_HEADERS[@]}" ]; then
    fail "$base: expected ${#SECTION_HEADERS[@]} mandated sections, found ${#found[@]} (${found[*]:-none})"
    return
  fi
  local i
  for i in "${!SECTION_HEADERS[@]}"; do
    if [ "${found[$i]}" != "${SECTION_HEADERS[$i]}" ]; then
      fail "$base: section #$((i+1)) is '${found[$i]}', expected '${SECTION_HEADERS[$i]}' (out of order)"
      return
    fi
  done
  pass "$base: 7 mandated sections present and in order"
}

# Check the spec names at least one concrete numeric tolerance (e.g. 1e-12, 1e-30, 1e-10,
# 1e-14, or a scientific-notation float).
check_has_tolerance() {
  local file="$1" base; base="$(basename "$file")"
  if grep -Eiq '[0-9](\.[0-9]+)?[eE][-+]?[0-9]+' "$file"; then
    pass "$base: names a concrete numeric tolerance"
  else
    fail "$base: no concrete numeric tolerance found"
  fi
}

# Resolve every "Source of truth" path the spec cites. We scan the whole file for tokens
# that look like a weaklib/amrex source path and check each resolves under its repo root.
# A path is any token containing a '/' that ends in a Fortran (.F90/.f90) or AMReX (.H)
# extension. weaklib paths are expected to start with "weaklib/"; amrex with "amrex/".
check_source_of_truth_paths() {
  local file="$1" base; base="$(basename "$file")"
  local any=0 ok=1
  # Pull candidate paths out of inline code / prose. Strip surrounding backticks/commas/parens.
  while IFS= read -r tok; do
    [ -z "$tok" ] && continue
    any=1
    local root rel resolved
    case "$tok" in
      weaklib/*) root="$WEAKLIB_ROOT"; rel="${tok#weaklib/}" ;;
      amrex/*)   root="$AMREX_ROOT";   rel="${tok#amrex/}" ;;
      *) continue ;;
    esac
    resolved="$root/$rel"
    if [ -f "$resolved" ]; then
      info "$base: source-of-truth resolves: $tok"
    else
      fail "$base: source-of-truth path does not resolve: $tok (looked at $resolved)"
      ok=0
    fi
  done < <(grep -oE '(weaklib|amrex)/[A-Za-z0-9_./-]+\.(F90|f90|H)' "$file" | sort -u)

  if [ "$any" -eq 0 ]; then
    fail "$base: no weaklib/ or amrex/ source-of-truth path cited"
  elif [ "$ok" -eq 1 ]; then
    pass "$base: all cited source-of-truth paths resolve"
  fi
}

# Per-spec fixed-string assertions.
check_require_in_spec() {
  local entry="$1" file needle base
  file="${entry%%|||*}"; needle="${entry##*|||}"
  base="$file"; file="$SPECS_DIR/$file"
  if grep -Fq "$needle" "$file"; then
    pass "$base: contains required claim: $needle"
  else
    fail "$base: missing required claim: $needle"
  fi
}

# Inversion error-code assertion: the code must appear as a markdown table cell
# `| <code> |` in the spec's error-code table (precise match, not a loose substring).
check_require_error_code() {
  local entry="$1" file code base
  file="${entry%%|||*}"; code="${entry##*|||}"
  base="$file"; file="$SPECS_DIR/$file"
  if grep -Eq "^\|[[:space:]]*${code}[[:space:]]*\|" "$file"; then
    pass "$base: documents error code: $code"
  else
    fail "$base: missing error code in error-code table: $code"
  fi
}

# Per-spec snapshot-structure assertions: the needle must be present in the named
# committed snapshot, and the spec must cite that snapshot's table by name.
check_require_in_snapshot() {
  local entry="$1" file snap needle base table
  file="${entry%%|||*}"
  local rest="${entry#*|||}"
  snap="${rest%%|||*}"
  needle="${rest##*|||}"
  base="$file"
  table="${snap%.h5ls}.h5"

  local snapfile="$FIXTURES_DIR/$snap"
  if [ ! -f "$snapfile" ]; then
    fail "$base: committed snapshot missing: specs/fixtures/$snap"
    return
  fi
  if ! grep -Fq "$needle" "$snapfile"; then
    fail "$base: snapshot $snap does not contain documented structure: $needle"
    return
  fi
  if ! grep -Fq "$table" "$SPECS_DIR/$file"; then
    fail "$base: spec does not cite its reference table by name: $table"
    return
  fi
  pass "$base: documented structure '$needle' confirmed in snapshot $snap"
}

# --------------------------------------------------------------------------------------
# README index integrity
# --------------------------------------------------------------------------------------
check_readme_links() {
  if [ ! -f "$README" ]; then fail "README.md missing at specs/README.md"; return; fi

  # Collect markdown link targets that point at sibling .md spec files.
  local -a linked=()
  while IFS= read -r target; do
    target="${target#./}"
    linked+=("$target")
  done < <(grep -oE '\]\(\.?/?[A-Za-z0-9_-]+\.md\)' "$README" | sed -E 's/^\]\(\.?\/?//; s/\)$//' | sort -u)

  # (e1) every registered spec is linked.
  local s
  for s in "${REGISTERED_SPECS[@]}"; do
    local hit=0 l
    for l in "${linked[@]}"; do [ "$l" = "$s" ] && hit=1; done
    if [ "$hit" -eq 1 ]; then
      info "README links registered spec: $s"
    else
      fail "README index does not link registered spec: $s"
    fi
  done

  # (e2) every link target exists on disk (no broken intra-repo links).
  local l
  for l in "${linked[@]}"; do
    if [ ! -f "$SPECS_DIR/$l" ]; then
      fail "README links a nonexistent file: $l"
    fi
  done

  # (e3) no orphan spec files: every registered spec must be on disk.
  for s in "${REGISTERED_SPECS[@]}"; do
    if [ ! -f "$SPECS_DIR/$s" ]; then
      fail "registered spec file is missing on disk: $s"
    fi
  done

  if [ "$FAILURES" -eq 0 ]; then pass "README index: all registered specs linked, all links resolve, no orphans"; fi
}

# --------------------------------------------------------------------------------------
# Snapshot integrity (default mode): committed *.h5ls match tables.provenance checksums.
# --------------------------------------------------------------------------------------
check_snapshot_checksums() {
  if [ ! -f "$PROVENANCE" ]; then fail "tables.provenance missing"; return; fi
  local snap expected actual
  while IFS= read -r line; do
    case "$line" in
      snapshot:\ *)        snap="${line#snapshot: }" ;;
      snapshot_sha256:\ *) expected="${line#snapshot_sha256: }"
        local f="$FIXTURES_DIR/$snap"
        if [ ! -f "$f" ]; then fail "snapshot referenced in provenance is missing: $snap"; continue; fi
        actual="$(shasum -a 256 "$f" | awk '{print $1}')"
        if [ "$actual" = "$expected" ]; then
          info "snapshot checksum ok: $snap"
        else
          fail "snapshot checksum mismatch for $snap (edited without refresh?)"
        fi
        ;;
    esac
  done < "$PROVENANCE"
  if [ "$FAILURES" -eq 0 ]; then pass "committed snapshots match tables.provenance checksums"; fi
}

# --------------------------------------------------------------------------------------
# Provenance / refresh mode (opt-in).
# --------------------------------------------------------------------------------------
check_live_tables() {
  if [ -z "${WL_TABLES_ROOT:-}" ]; then
    echo "SKIPPED (WL_TABLES_ROOT unset): live-table cross-check not run; default-mode snapshot checks above stand in for it."
    return
  fi
  if ! command -v h5ls >/dev/null 2>&1; then
    fail "refresh mode requested but h5ls not on PATH"; return
  fi
  local table table_sha snap
  while IFS= read -r line; do
    case "$line" in
      table:\ *)        table="${line#table: }" ;;
      table_sha256:\ *) table_sha="${line#table_sha256: }" ;;
      snapshot:\ *)
        snap="${line#snapshot: }"
        # The provenance manifest pins the canonical clean set in use_for_production/;
        # prefer that subdir, then fall back to WL_TABLES_ROOT itself.
        local live="$WL_TABLES_ROOT/use_for_production/$table"
        if [ ! -f "$live" ]; then
          live="$WL_TABLES_ROOT/$table"
        fi
        if [ ! -f "$live" ]; then
          fail "refresh: live table not found under WL_TABLES_ROOT: $table"
          continue
        fi
        local got_sha; got_sha="$(shasum -a 256 "$live" | awk '{print $1}')"
        if [ "$got_sha" != "$table_sha" ]; then
          fail "refresh: live table sha256 differs from tables.provenance: $table"
        fi
        local tmp; tmp="$(mktemp)"
        h5ls -r "$live" > "$tmp"
        if diff -q "$tmp" "$FIXTURES_DIR/$snap" >/dev/null 2>&1; then
          pass "refresh: committed snapshot $snap still matches live table $table"
        else
          fail "refresh: committed snapshot $snap drifted from live table $table"
        fi
        rm -f "$tmp"
        ;;
    esac
  done < "$PROVENANCE"
}

# --------------------------------------------------------------------------------------
# Run
# --------------------------------------------------------------------------------------
echo "=== WeakLibInterp spec-set validator (default mode) ==="
echo "specs dir:    $SPECS_DIR"
echo "weaklib root: $WEAKLIB_ROOT"
echo "amrex root:   $AMREX_ROOT"
echo

echo "--- per-spec: section order, tolerance, source-of-truth ---"
for s in "${REGISTERED_SPECS[@]}"; do
  f="$SPECS_DIR/$s"
  if [ ! -f "$f" ]; then fail "registered spec not found: $s"; continue; fi
  check_section_order "$f"
  check_has_tolerance "$f"
  check_source_of_truth_paths "$f"
done

echo
echo "--- per-spec: registered fixed-string claims ---"
for e in "${SPEC_REQUIRE_IN_SPEC[@]}"; do check_require_in_spec "$e"; done

echo
echo "--- per-spec: inversion error-code coverage ---"
for e in "${SPEC_REQUIRE_ERROR_CODES[@]}"; do check_require_error_code "$e"; done

echo
echo "--- per-spec: documented structure vs committed snapshots ---"
for e in "${SPEC_REQUIRE_IN_SNAPSHOT[@]}"; do check_require_in_snapshot "$e"; done

echo
echo "--- README index integrity ---"
check_readme_links

echo
echo "--- committed snapshot checksums (CI-reproducible ground truth) ---"
check_snapshot_checksums

echo
echo "--- provenance / refresh mode (live tables) ---"
check_live_tables

echo
if [ "$FAILURES" -eq 0 ]; then
  echo "ALL CHECKS PASSED ($(date -u +%Y-%m-%dT%H:%M:%SZ))"
  exit 0
else
  echo "VALIDATION FAILED: $FAILURES check(s) failed"
  exit 1
fi
