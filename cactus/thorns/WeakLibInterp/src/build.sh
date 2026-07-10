#! /bin/bash
#
# build.sh — thin driver of THIS repo's own CMake to build + install the
# WeakLibInterp library into the Cactus scratch prefix.
#
# It carries no second source list (spec:64) and never bundles or builds AMReX
# (spec:62): it configures this repository in installed-AMReX mode against the
# AMReX install the ET AMReX thorn provides (AMREX_DIR, read-only, spec:46),
# tests off (spec:56), and installs the library + public headers into the
# resolved prefix (spec:50). The configuration-consistency guard lives in this
# repo's CMake (single source of truth, spec:63/64) — a backend/MPI mismatch
# hard-errors at the nested configure, so this script does not re-check it.
#
# Environment: SCRATCH_BUILD is exported globally by the flesh
# (lib/make/make.configuration); AMREX_DIR / AMREX_ENABLE_{CUDA,HIP} and
# WEAKLIBINTERP_INSTALL_DIR are MAKE_DEFINITIONs exported by make.code.deps.

set -e

################################################################################
# Locate the repository root from the thorn's src/ directory (spec:87):
#   src -> WeakLibInterp -> thorns -> cactus -> <repo root>
# pwd -P: the arrangement entry is a symlink into this repo (GetComponents
# style), so the logical path would walk up the Cactus tree, not the checkout.
################################################################################
WLI_SRCDIR="$(cd "$(dirname "$0")" && pwd -P)"
WLI_REPO_ROOT="$(cd "${WLI_SRCDIR}/../../../.." && pwd -P)"

################################################################################
# Resolve prefix + build tree (matches detect.sh, spec:45). Both live under
# ${SCRATCH_BUILD}, outside this repo's tree, so nothing is written into the
# checkout (build-from-checkout, zero drift, spec:65).
################################################################################
WLI_PREFIX="${WEAKLIBINTERP_INSTALL_DIR:-${SCRATCH_BUILD}/external/WeakLibInterp}"
WLI_BUILD_DIR="${SCRATCH_BUILD}/build/WeakLibInterp"

################################################################################
# Translate the ET AMReX thorn's GPU option vocabulary into WLI's (spec:46,88):
#   AMREX_ENABLE_CUDA=yes -> WLI_GPU_BACKEND=CUDA
#   AMREX_ENABLE_HIP=yes  -> WLI_GPU_BACKEND=HIP
#   otherwise             -> NONE
# Architecture flows transitively via AMReX::amrex in installed mode, so no
# arch variable is forwarded here.
################################################################################
WLI_GPU_BACKEND=NONE
if [ "$(echo "${AMREX_ENABLE_CUDA:-no}" | tr '[:upper:]' '[:lower:]')" = "yes" ]; then
    WLI_GPU_BACKEND=CUDA
elif [ "$(echo "${AMREX_ENABLE_HIP:-no}" | tr '[:upper:]' '[:lower:]')" = "yes" ]; then
    WLI_GPU_BACKEND=HIP
fi

################################################################################
# MPI: the ET AMReX thorn has no MPI switch — its bundled build leaves AMReX's
# default (ON), and a detect-mode install records its own choice. The one
# source of truth is the install's AMReXConfig.cmake (`set(AMReX_MPI ...)`),
# so read WLI_AMREX_MPI from there; the nested CMake guard then reconciles the
# pair loudly if this parse ever drifts. The [[:space:]] anchor keeps
# AMReX_MPI_THREAD_MULTIPLE from matching.
################################################################################
AMREX_CONFIG="$(find "${AMREX_DIR}" -name AMReXConfig.cmake 2>/dev/null | head -1)"
if [ -z "${AMREX_CONFIG}" ]; then
    echo "BEGIN ERROR"
    echo "WeakLibInterp: no AMReXConfig.cmake under AMREX_DIR='${AMREX_DIR}'."
    echo "The ET AMReX thorn must provide an installed AMReX (spec:62)."
    echo "END ERROR"
    exit 1
fi
WLI_AMREX_MPI=OFF
if grep -Eq 'set\(AMReX_MPI[[:space:]]+ON\)' "${AMREX_CONFIG}"; then
    WLI_AMREX_MPI=ON
fi

WLI_BUILD_JOBS="${WLI_BUILD_JOBS:-4}"

echo "BEGIN MESSAGE"
echo "Configuring WeakLibInterp: root=${WLI_REPO_ROOT}"
echo "  AMReX install : ${AMREX_DIR}"
echo "  GPU backend   : ${WLI_GPU_BACKEND}"
echo "  MPI           : ${WLI_AMREX_MPI} (from ${AMREX_CONFIG})"
echo "  install prefix: ${WLI_PREFIX}"
echo "END MESSAGE"

################################################################################
# Configure + build + install via this repo's CMake (spec:53-57).
################################################################################
rm -rf "${WLI_BUILD_DIR}"
cmake -S "${WLI_REPO_ROOT}" -B "${WLI_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DWLI_AMREX_INSTALL_DIR="${AMREX_DIR}" \
    -DWLI_BUILD_TESTS=OFF \
    -DWLI_GPU_BACKEND="${WLI_GPU_BACKEND}" \
    -DWLI_AMREX_MPI="${WLI_AMREX_MPI}" \
    -DCMAKE_INSTALL_PREFIX="${WLI_PREFIX}"

cmake --build "${WLI_BUILD_DIR}" -j"${WLI_BUILD_JOBS}" --target install

echo "BEGIN MESSAGE"
echo "WeakLibInterp built and installed into ${WLI_PREFIX}"
echo "END MESSAGE"
