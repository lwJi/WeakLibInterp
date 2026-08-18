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
WLI_BUILD_DIR="${SCRATCH_BUILD}/external/WeakLibInterp-build"

################################################################################
# Derive the GPU backend from what the AMReX install itself records (spec:46,
# 63,88) — the same discipline as MPI: the prefix is the single source of
# truth, so the derived value can never trip the consistency guard; only an
# explicit standalone request can mismatch. The ET AMReX thorn's
# AMREX_ENABLE_* options are deliberately NOT consulted: CarpetX-style option
# lists never set them (they make the device compiler the global CXX
# directly), so those variables under-report the backend of the install at
# AMREX_DIR. Architecture flows transitively via AMReX::amrex in installed
# mode (the prefix caches AMREX_CUDA_ARCHS and the AMReX helper stamps it on
# every target), so no arch variable is forwarded here.
################################################################################
amrex_config=""
for d in "${AMREX_DIR}/lib/cmake/AMReX" "${AMREX_DIR}/lib64/cmake/AMReX"; do
    if [ -f "${d}/AMReXConfig.cmake" ]; then
        amrex_config="${d}/AMReXConfig.cmake"
        break
    fi
done
if [ -z "${amrex_config}" ]; then
    amrex_config="$(find "${AMREX_DIR}" -name AMReXConfig.cmake 2>/dev/null | head -n 1)"
fi
WLI_GPU_BACKEND=""
if [ -n "${amrex_config}" ]; then
    WLI_GPU_BACKEND="$(sed -n 's/^set(AMReX_GPU_BACKEND[[:space:]]*\([A-Za-z]*\)).*/\1/p' "${amrex_config}" | head -n 1)"
fi
case "${WLI_GPU_BACKEND}" in
    NONE|CUDA|HIP) ;;
    *)
        echo "BEGIN ERROR"
        echo "WeakLibInterp: could neither find nor build the library."
        echo "Error: cannot read AMReX_GPU_BACKEND from the AMReX install."
        echo "  AMREX_DIR      : ${AMREX_DIR}"
        echo "  AMReXConfig    : ${amrex_config:-<not found>}"
        echo "  parsed backend : '${WLI_GPU_BACKEND}'"
        echo "The nested configure derives its GPU backend from the install's"
        echo "AMReXConfig.cmake; an install without a readable one cannot be"
        echo "consumed. Point the ET AMReX thorn at a complete AMReX install."
        echo "END ERROR"
        exit 1
        ;;
esac

################################################################################
# Compiler environment for the nested configure (spec:88). Cactus GPU option
# lists make the device driver the global CXX ("nvcc -x cu",
# "clang++ -x hip") and pack device-only flags into CXXFLAGS; inherited
# verbatim, CMake's compiler sanity check links its test object through the
# "-x cu"/"-x hip" language override and fails ("unrecognized token" on the
# .o). The nested build gets a host C++ compiler instead:
#   CUDA: scrub CXX/CXXFLAGS/LDFLAGS; enable_language(CUDA) finds nvcc on
#         PATH and the AMReX helper compiles the wli sources as CUDA with the
#         prefix's recorded architectures.
#   HIP : the first word of CXX (the hip-capable clang of the very Cactus
#         config) becomes CMAKE_CXX_COMPILER, amdclang++ as fallback; its
#         ROCm root joins CMAKE_PREFIX_PATH so find_package(hip) resolves
#         (docs/BUILD.md rocm mechanism). "-x hip" propagates transitively
#         via AMReX::amrex -> hip::device.
#   NONE: environment untouched — the host compiler is already correct.
################################################################################
WLI_CMAKE_LAUNCH=()
WLI_CMAKE_EXTRA_ARGS=()
case "${WLI_GPU_BACKEND}" in
    CUDA)
        WLI_CMAKE_LAUNCH=(env -u CXX -u CXXFLAGS -u LDFLAGS)
        ;;
    HIP)
        WLI_CMAKE_LAUNCH=(env -u CXX -u CXXFLAGS -u LDFLAGS)
        hip_cxx="${CXX%% *}"
        command -v "${hip_cxx}" > /dev/null 2>&1 || hip_cxx=amdclang++
        WLI_CMAKE_EXTRA_ARGS+=("-DCMAKE_CXX_COMPILER=${hip_cxx}")
        # ROCm root = the ancestor of the compiler that carries lib/cmake/hip
        # (clang++ sits in <root>/llvm/bin, amdclang++ in <root>/bin).
        rocm_root=""
        hip_cxx_path="$(command -v "${hip_cxx}" 2>/dev/null || true)"
        if [ -n "${hip_cxx_path}" ]; then
            hip_cxx_bindir="$(dirname "${hip_cxx_path}")"
            for cand in "${hip_cxx_bindir}/.." "${hip_cxx_bindir}/../.."; do
                if [ -d "${cand}/lib/cmake/hip" ]; then
                    rocm_root="$(cd "${cand}" && pwd -P)"
                    break
                fi
            done
        fi
        [ -z "${rocm_root}" ] && [ -d /opt/rocm ] && rocm_root=/opt/rocm
        if [ -n "${rocm_root}" ]; then
            WLI_CMAKE_EXTRA_ARGS+=("-DCMAKE_PREFIX_PATH=${rocm_root}")
        fi
        ;;
esac

################################################################################
# MPI is NOT forwarded: WLI's CMake defaults its MPI knob to AUTO and derives
# the value from the AMReX_MPI fact find_package reads out of the prefix —
# this script neither guesses nor parses anything. The nested configure
# prints the derived value ("derived MPI=... from the AMReX prefix").
################################################################################

WLI_BUILD_JOBS="${WLI_BUILD_JOBS:-4}"

echo "BEGIN MESSAGE"
echo "Configuring WeakLibInterp: root=${WLI_REPO_ROOT}"
echo "  AMReX install : ${AMREX_DIR}"
echo "  GPU backend   : ${WLI_GPU_BACKEND} (derived from the AMReX prefix)"
echo "  MPI           : derived from the AMReX prefix (see nested configure)"
echo "  install prefix: ${WLI_PREFIX}"
echo "END MESSAGE"

################################################################################
# Configure + build + install via this repo's CMake (spec:53-57).
#
# Each stage carries its own explicit diagnostic — the build half of "find-or-
# build is never silent" (spec:69). `set -e` alone aborts without naming the
# stage, the inputs that produced it, or the remedy; and it does NOT fire for a
# command on the left of `||`, so every guard below ends in an explicit exit 1.
# The BEGIN/END ERROR markers mirror detect.sh for human/log consistency only:
# this script is run by make (make.code.deps), not by Cactus's
# ConfigScriptParser.pl, so unlike detect.sh's markers they carry no protocol
# meaning here.
################################################################################
"${WLI_CMAKE_LAUNCH[@]}" cmake -S "${WLI_REPO_ROOT}" -B "${WLI_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DWLI_AMREX_INSTALL_DIR="${AMREX_DIR}" \
    -DWLI_BUILD_TESTS=OFF \
    -DWLI_GPU_BACKEND="${WLI_GPU_BACKEND}" \
    -DCMAKE_INSTALL_PREFIX="${WLI_PREFIX}" \
    "${WLI_CMAKE_EXTRA_ARGS[@]}" || {
    echo "BEGIN ERROR"
    echo "WeakLibInterp: could neither find nor build the library."
    echo "Error: the nested CMake configure of this repository failed."
    echo "  repository root: ${WLI_REPO_ROOT}"
    echo "  AMReX install  : ${AMREX_DIR}"
    echo "  GPU backend    : ${WLI_GPU_BACKEND} (derived from the AMReX prefix)"
    echo "  install prefix : ${WLI_PREFIX}"
    echo "Scroll up for the CMake output: an MPI mismatch against the AMReX"
    echo "install above is named there by the configure-time guard."
    echo "Fix the ET AMReX thorn's options, or point WEAKLIBINTERP_DIR at an"
    echo "already-installed WeakLibInterp prefix instead of BUILD."
    echo "END ERROR"
    exit 1
}

"${WLI_CMAKE_LAUNCH[@]}" cmake --build "${WLI_BUILD_DIR}" -j"${WLI_BUILD_JOBS}" --target install || {
    echo "BEGIN ERROR"
    echo "WeakLibInterp: could neither find nor build the library."
    echo "Error: the library build/install step failed after a successful configure."
    echo "  build tree     : ${WLI_BUILD_DIR}"
    echo "  install prefix : ${WLI_PREFIX}"
    echo "Scroll up for the compiler/linker output. Re-run once it is fixed, or"
    echo "point WEAKLIBINTERP_DIR at an already-installed WeakLibInterp prefix"
    echo "instead of BUILD."
    echo "END ERROR"
    exit 1
}

echo "BEGIN MESSAGE"
echo "WeakLibInterp built and installed into ${WLI_PREFIX}"
echo "END MESSAGE"
