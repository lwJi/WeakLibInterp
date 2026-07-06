#!/usr/bin/env bash
#
# validate_thorn.sh — structural verifier for the WeakLibInterp
# ExternalLibraries-style Cactus thorn (specs/cactus-integration.md §25,39-51).
#
# PURPOSE
#   Mechanically enforce the file-shape of the thorn at
#   cactus/thorns/WeakLibInterp/: the four thorn files exist and carry the
#   required stanzas (PROVIDES block + detect emissions + build-script CMake
#   invocation + make.code.deps stamp). This is the empirical check the build
#   loop runs; it needs no Cactus/Einstein-Toolkit checkout.
#
# SCOPE
#   File-shape only. Verification #5 (in-Cactus acceptance, spec:79) is manual /
#   environment-gated — run where a Cactus checkout exists — and is out of scope
#   here. The consistency-guard message string is CMake-owned (CMakeLists.txt)
#   and is intentionally NOT re-asserted here.
#
# INVOCATION
#   Standalone, run by hand like validate_specs.sh:
#       bash specs/tools/validate_thorn.sh
#   It is NOT registered in ctest, so the default build/ regression suite stays
#   byte-unchanged (spec:70 #4).
#
# EXIT: 0 if every check passes, non-zero otherwise. All failures within a run
# are reported before exiting.

set -u

# --------------------------------------------------------------------------------------
# Paths
# --------------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPECS_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$SPECS_DIR/.." && pwd)"
THORN_DIR="$REPO_ROOT/cactus/thorns/WeakLibInterp"

CONFIG_CCL="$THORN_DIR/configuration.ccl"
DETECT_SH="$THORN_DIR/src/detect.sh"
BUILD_SH="$THORN_DIR/src/build.sh"
CODE_DEPS="$THORN_DIR/src/make.code.deps"

FAILURES=0
fail()  { echo "FAIL: $*" >&2; FAILURES=$((FAILURES + 1)); }
pass()  { echo "ok:   $*"; }
info()  { echo "      $*"; }

# Require a fixed string in a file; if the file is missing, record a failure
# rather than letting grep error out under set -u.
require() {
  local file="$1" needle="$2" label="$3"
  if [ ! -f "$file" ]; then
    fail "$label: file missing, cannot check for: $needle"
    return
  fi
  if grep -Fq -e "$needle" "$file"; then
    pass "$label: contains '$needle'"
  else
    fail "$label: missing '$needle'"
  fi
}

# --------------------------------------------------------------------------------------
# (1) All four thorn files exist under cactus/thorns/WeakLibInterp/.
# --------------------------------------------------------------------------------------
echo "--- (1) thorn files exist ---"
for f in "$CONFIG_CCL" "$DETECT_SH" "$BUILD_SH" "$CODE_DEPS"; do
  rel="${f#"$REPO_ROOT"/}"
  if [ -f "$f" ]; then pass "exists: $rel"; else fail "missing: $rel"; fi
done

# --------------------------------------------------------------------------------------
# (2) configuration.ccl PROVIDES block: PROVIDES WeakLibInterp, the detect
#     SCRIPT, and both option variables.
# --------------------------------------------------------------------------------------
echo "--- (2) configuration.ccl PROVIDES block ---"
require "$CONFIG_CCL" "PROVIDES WeakLibInterp"       "configuration.ccl"
require "$CONFIG_CCL" "SCRIPT src/detect.sh"         "configuration.ccl"
require "$CONFIG_CCL" "WEAKLIBINTERP_DIR"            "configuration.ccl"
require "$CONFIG_CCL" "WEAKLIBINTERP_INSTALL_DIR"    "configuration.ccl"

# --------------------------------------------------------------------------------------
# (3) detect.sh emits the three Cactus lines and branches on WEAKLIBINTERP_DIR.
# --------------------------------------------------------------------------------------
echo "--- (3) detect.sh emissions + find-or-build branch ---"
require "$DETECT_SH" "INCLUDE_DIRECTORY"  "detect.sh"
require "$DETECT_SH" "LIBRARY_DIRECTORY"  "detect.sh"
require "$DETECT_SH" "LIBRARY wli_lib"    "detect.sh"
require "$DETECT_SH" "WEAKLIBINTERP_DIR"  "detect.sh"

# --------------------------------------------------------------------------------------
# (4) build.sh drives this repo's CMake into the prefix.
# --------------------------------------------------------------------------------------
echo "--- (4) build.sh CMake driver ---"
require "$BUILD_SH" "WLI_AMREX_INSTALL_DIR" "build.sh"
require "$BUILD_SH" "WLI_BUILD_TESTS=OFF"   "build.sh"
require "$BUILD_SH" "CMAKE_INSTALL_PREFIX"  "build.sh"
require "$BUILD_SH" "--target install"      "build.sh"

# --------------------------------------------------------------------------------------
# (5) make.code.deps gates a done/ stamp on build.sh.
# --------------------------------------------------------------------------------------
echo "--- (5) make.code.deps stamp ---"
require "$CODE_DEPS" "build.sh" "make.code.deps"
require "$CODE_DEPS" "done/"    "make.code.deps"

# --------------------------------------------------------------------------------------
# Report
# --------------------------------------------------------------------------------------
echo
if [ "$FAILURES" -eq 0 ]; then
  echo "ALL CHECKS PASSED ($(date -u +%Y-%m-%dT%H:%M:%SZ))"
  exit 0
else
  echo "VALIDATION FAILED: $FAILURES check(s) failed"
  exit 1
fi
