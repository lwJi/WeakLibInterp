#!/usr/bin/env bash
#
# setup-cactus-tree.sh — assemble a minimal Cactus + CarpetX tree that
# consumes this repository's WeakLibInterp thorn from the live checkout
# (specs/cactus-integration.md: build-from-checkout, zero drift, spec:65).
#
# What it does (idempotent; every step skipped if already present):
#   1. Clones the Cactus flesh + CactusBase + the five ExternalLibraries
#      wrapper thorns into <cactus-dir> (default <repo>/.build/cactus).
#   2. Symlinks arrangements/CarpetX -> the sibling ../CarpetX checkout and
#      arrangements/WeakLibInterp -> this repo's cactus/thorns (GetComponents
#      style; the thorn's build.sh resolves symlinks with pwd -P).
#   3. Materializes wli-carpetx-aarch64.cfg from the .cfg.in template:
#      @WLI_AMREX_PREFIX@ -> the prefix where the ET AMReX thorn installs its
#      bundled AMReX (AMREX_DIR = BUILD; the bundled 25.11 is what this
#      CarpetX checkout compiles against — the newer sibling ../amrex breaks
#      CarpetX — and a pinned prefix keeps the LDFLAGS rpath stable).
#
# Then configure + build (printed at the end):
#   cd <cactus-dir>
#   make wli-carpetx-config PROMPT=no options=wli-carpetx-aarch64.cfg \
#        THORNLIST=<repo>/cactus/config/wli-carpetx.th
#   make wli-carpetx FJOBS=4
# FJOBS (files-per-thorn parallelism), never `make -j`: -j parallelizes the
# thorn libraries themselves, racing consumers ahead of the bundled-AMReX
# install (thorns build serially in sorted-name order — AMReX first).
#
# Host prerequisites (Ubuntu): g++ gcc cmake make patch libhdf5-dev libopenmpi-dev
# libyaml-cpp-dev zlib1g-dev pkgconf patch   (never Fortran — F90 = none).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd -P)"
REPOS_ROOT="$(cd "${REPO_ROOT}/.." && pwd -P)"

CACTUS_DIR="${1:-${REPO_ROOT}/.build/cactus}"
AMREX_PREFIX="${WLI_AMREX_PREFIX:-${REPO_ROOT}/.build/opt/amrex-cactus}"
CARPETX_SRC="${WLI_CARPETX_ROOT:-${REPOS_ROOT}/CarpetX}"
JOBS="${WLI_BUILD_JOBS:-4}"

step() { echo "== $*"; }

# ----------------------------------------------------------------------------
# 1. Cactus flesh + cloned arrangements.
# ----------------------------------------------------------------------------
clone() { # url dest
    if [ -d "$2/.git" ]; then step "already cloned: $2"; else
        step "cloning $1"; git clone -q --depth 1 "$1" "$2"; fi
}

clone https://bitbucket.org/cactuscode/cactus.git "${CACTUS_DIR}"
mkdir -p "${CACTUS_DIR}/arrangements/ExternalLibraries"
clone https://bitbucket.org/cactuscode/cactusbase.git \
      "${CACTUS_DIR}/arrangements/CactusBase"
clone https://github.com/EinsteinToolkit/ExternalLibraries-MPI \
      "${CACTUS_DIR}/arrangements/ExternalLibraries/MPI"
clone https://github.com/EinsteinToolkit/ExternalLibraries-HDF5 \
      "${CACTUS_DIR}/arrangements/ExternalLibraries/HDF5"
clone https://github.com/EinsteinToolkit/ExternalLibraries-zlib \
      "${CACTUS_DIR}/arrangements/ExternalLibraries/zlib"
clone https://github.com/rhaas80/ExternalLibraries-AMReX \
      "${CACTUS_DIR}/arrangements/ExternalLibraries/AMReX"
clone https://github.com/rhaas80/ExternalLibraries-yaml_cpp \
      "${CACTUS_DIR}/arrangements/ExternalLibraries/yaml_cpp"

# ----------------------------------------------------------------------------
# 2. Local checkouts as arrangements (zero drift, spec:65).
# ----------------------------------------------------------------------------
step "symlinking arrangements/CarpetX and arrangements/WeakLibInterp"
ln -sfn "${CARPETX_SRC}"             "${CACTUS_DIR}/arrangements/CarpetX"
ln -sfn "${REPO_ROOT}/cactus/thorns" "${CACTUS_DIR}/arrangements/WeakLibInterp"

# ----------------------------------------------------------------------------
# 3. Materialize the option list.
# ----------------------------------------------------------------------------
step "materializing wli-carpetx-aarch64.cfg (AMReX prefix: ${AMREX_PREFIX})"
sed "s|@WLI_AMREX_PREFIX@|${AMREX_PREFIX}|g" \
    "${SCRIPT_DIR}/wli-carpetx-aarch64.cfg.in" \
    > "${CACTUS_DIR}/wli-carpetx-aarch64.cfg"

cat <<EOF

Cactus tree ready at ${CACTUS_DIR}. Configure and build with:

  cd ${CACTUS_DIR}
  make wli-carpetx-config PROMPT=no \\
       options=wli-carpetx-aarch64.cfg \\
       THORNLIST=${SCRIPT_DIR}/wli-carpetx.th
  make wli-carpetx FJOBS=${JOBS}   # FJOBS, never -j (thorn-build ordering)

EOF
