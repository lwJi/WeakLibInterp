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

set -e

################################################################################
# Locate the repository root from the thorn's src/ directory (spec:87):
#   src -> WeakLibInterp -> thorns -> cactus -> <repo root>
################################################################################
WLI_SRCDIR="$(cd "$(dirname "$0")" && pwd)"
WLI_REPO_ROOT="$(cd "${WLI_SRCDIR}/../../../.." && pwd)"

################################################################################
# Resolve prefix + build tree (matches detect.sh, spec:45). Both live under
# ${SCRATCH_BUILD}, outside this repo's tree, so nothing is written into the
# checkout (build-from-checkout, zero drift, spec:65).
################################################################################
WLI_PREFIX="${WEAKLIBINTERP_INSTALL_DIR:-${SCRATCH_BUILD}/external/WeakLibInterp}"
WLI_BUILD_DIR="${SCRATCH_BUILD}/external/WeakLibInterp-build"

################################################################################
# Translate the ET AMReX thorn's GPU option vocabulary into WLI's (spec:46,88):
#   AMREX_ENABLE_CUDA=yes -> WLI_GPU_BACKEND=CUDA
#   AMREX_ENABLE_HIP=yes  -> WLI_GPU_BACKEND=HIP
#   otherwise             -> NONE
# Architecture flows transitively via AMReX::amrex in installed mode, so no
# arch variable is forwarded here.
################################################################################
WLI_GPU_BACKEND=NONE
if [ "${AMREX_ENABLE_CUDA:-no}" = "yes" ]; then
    WLI_GPU_BACKEND=CUDA
elif [ "${AMREX_ENABLE_HIP:-no}" = "yes" ]; then
    WLI_GPU_BACKEND=HIP
fi

################################################################################
# Forward MPI to WLI_AMREX_MPI when the AMReX thorn advertises it; otherwise
# leave the default OFF. The nested CMake guard reconciles this against the
# install's recorded AMReX_MPI, so a wrong guess fails loudly at configure.
################################################################################
WLI_AMREX_MPI=OFF
if [ "${AMREX_ENABLE_MPI:-no}" = "yes" ]; then
    WLI_AMREX_MPI=ON
fi

WLI_BUILD_JOBS="${WLI_BUILD_JOBS:-4}"

echo "BEGIN MESSAGE"
echo "Configuring WeakLibInterp: root=${WLI_REPO_ROOT}"
echo "  AMReX install : ${AMREX_DIR}"
echo "  GPU backend   : ${WLI_GPU_BACKEND}"
echo "  MPI           : ${WLI_AMREX_MPI}"
echo "  install prefix: ${WLI_PREFIX}"
echo "END MESSAGE"

################################################################################
# Configure + build + install via this repo's CMake (spec:53-57).
################################################################################
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
