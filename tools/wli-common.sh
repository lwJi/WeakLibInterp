# tools/wli-common.sh — shared helpers for the thin tools/*.sh drivers.
# Sourced, never executed. The drivers own workflow only; every -DWLI_* flag
# lives in CMakePresets.json, and the AMReX flag base lives in
# tools/provision-amrex.sh.

wli_repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Quiet-runner routing: stash a step's output via tools/q (one ✓ line on
# success, full log dump on failure, exit code passed through).
q() {
  "${wli_repo_root}/tools/q" "$@"
}

# One loud line stating what was refused and why, exit 2 — deliberately
# distinct from a test failure's exit code.
refuse() {
  echo "✗ refused: $*" >&2
  exit 2
}

# Prefix resolution, the whole chain in one place:
#   1. WLI_AMREX_INSTALL_DIR set in env → emitted as the scripts' only -D
#      (explicit override wins; how existing prefixes like the ET thorn's
#      amrex-lib plug in).
#   2. else → export WLI_AMREX_HOME (default $HOME/wli-amrex) and let the
#      preset's "$env{WLI_AMREX_HOME}/<config>" resolve the store prefix.
# Callers expand the array with the bash-3.2-safe idiom
#   ${wli_cmake_overrides[@]+"${wli_cmake_overrides[@]}"}
# (an empty array trips `set -u` on this host's /bin/bash otherwise).
resolve_amrex_env() {
  export WLI_AMREX_HOME="${WLI_AMREX_HOME:-$HOME/wli-amrex}"
  wli_cmake_overrides=()
  if [ -n "${WLI_AMREX_INSTALL_DIR:-}" ]; then
    wli_cmake_overrides+=("-DWLI_AMREX_INSTALL_DIR=${WLI_AMREX_INSTALL_DIR}")
  fi
}

# Map a configure preset to its build tree (must mirror the binaryDir
# declarations in CMakePresets.json).
preset_tree() {
  case "$1" in
    default)    echo "build" ;;
    guard-trip) echo "build-guard" ;;
    *)          echo "build-$1" ;;
  esac
}
