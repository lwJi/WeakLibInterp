#! /bin/bash
#
# detect.sh — WeakLibInterp ExternalLibraries find-or-build detection.
#
# Mirrors ExternalLibraries-AMReX's detect script (specs/cactus-integration.md
# §25). It decides find-vs-build from WEAKLIBINTERP_DIR (spec:44), resolves the
# install prefix, verifies a detect-mode prefix loudly (spec:69 — a bad
# WEAKLIBINTERP_DIR must fail configuration, never warn), and emits the standard
# Cactus INCLUDE_DIRECTORY / LIBRARY_DIRECTORY / LIBRARY (spec:51). The build
# itself is driven by make.code.deps -> src/build.sh during the make phase; a
# build-mode build failure is caught there (the other half of spec:69).

set -e

################################################################################
# Resolve the install prefix (spec:45). WEAKLIBINTERP_INSTALL_DIR optionally
# overrides the default ${SCRATCH_BUILD}/external/WeakLibInterp.
################################################################################
WLI_PREFIX="${WEAKLIBINTERP_INSTALL_DIR:-${SCRATCH_BUILD}/external/WeakLibInterp}"

################################################################################
# Find-or-build decision, keyed on WEAKLIBINTERP_DIR (spec:44).
#   unset or "BUILD" -> build from this repo's checkout into WLI_PREFIX
#   a path          -> use that pre-installed prefix (detect-only, no build)
################################################################################
if [ -z "${WEAKLIBINTERP_DIR:-}" ] || [ "${WEAKLIBINTERP_DIR}" = "BUILD" ]; then

    echo "BEGIN MESSAGE"
    echo "Building WeakLibInterp from the repository checkout into ${WLI_PREFIX}"
    echo "END MESSAGE"

    # Hand the resolved prefix to the build script (make.code.deps -> build.sh).
    export WEAKLIBINTERP_INSTALL_DIR="${WLI_PREFIX}"

else

    # Detect-only: WEAKLIBINTERP_DIR names a pre-installed prefix; no build.
    WLI_PREFIX="${WEAKLIBINTERP_DIR}"

    echo "BEGIN MESSAGE"
    echo "Using pre-installed WeakLibInterp at ${WLI_PREFIX}"
    echo "END MESSAGE"

    # Loud verification (spec:69): the prefix must carry the public headers and
    # the static archive, or configuration fails — never a silent warning.
    if [ ! -r "${WLI_PREFIX}/include/wli_eos.H" ] || \
       [ ! -r "${WLI_PREFIX}/lib/libwli_lib.a" ]; then
        echo "BEGIN ERROR"
        echo "WeakLibInterp: WEAKLIBINTERP_DIR='${WLI_PREFIX}' does not contain a"
        echo "usable installation. Could neither find nor build the library."
        echo "  Expected header:  ${WLI_PREFIX}/include/wli_eos.H"
        echo "  Expected library: ${WLI_PREFIX}/lib/libwli_lib.a"
        echo "Set WEAKLIBINTERP_DIR=BUILD to build from this repo's checkout, or"
        echo "point it at a prefix containing include/wli_eos.H and lib/libwli_lib.a."
        echo "END ERROR"
        exit 1
    fi

fi

################################################################################
# Emit the Cactus include/library configuration (spec:51). LIBRARY is the base
# name "wli_lib" (-> libwli_lib.a, the archive from src/CMakeLists.txt), never
# "WeakLibInterp"/"wli", or the consumer link fails.
################################################################################
echo "BEGIN MAKE_DEFINITION"
echo "WeakLibInterp_DIR = ${WLI_PREFIX}"
echo "END MAKE_DEFINITION"

echo "INCLUDE_DIRECTORY ${WLI_PREFIX}/include"
echo "LIBRARY_DIRECTORY ${WLI_PREFIX}/lib"
echo "LIBRARY wli_lib"
