#!/usr/bin/env bash
# tools/build.sh [preset] — configure + build one preset (default: default).
#
# Always reconfigures: configure is cheap in find-only mode, and this kills
# the forgot-to-reconfigure footgun after adding a test target. If configure
# fails over a pre-existing CMakeCache.txt, the tree is wiped and configure
# retried once — the generic stale-cache heal (foreign-compiler pin, moved
# prefix, CMake version bump all present the same way). A genuinely broken
# configure fails twice and reports normally; a fresh tree that fails (e.g.
# the missing-prefix FATAL_ERROR) fails immediately, no pointless retry.
#
# No -DWLI_* flag lives here — flags are CMakePresets.json's; the only -D this
# script ever passes is the WLI_AMREX_INSTALL_DIR override passthrough
# (explicit env wins over $WLI_AMREX_HOME/<config>).

set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/wli-common.sh"

preset="${1:-default}"

resolve_amrex_env
cd "${wli_repo_root}"
tree="$(preset_tree "${preset}")"

configure() {
  q cmake --preset "${preset}" \
    ${wli_cmake_overrides[@]+"${wli_cmake_overrides[@]}"}
}

if [ -f "${tree}/CMakeCache.txt" ]; then
  if ! configure; then
    echo "stale cache suspected — wiping ${tree}/ and retrying configure once"
    rm -rf "${tree}"
    configure
  fi
else
  configure
fi

q cmake --build --preset "${preset}"
