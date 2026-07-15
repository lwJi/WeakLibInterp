#!/usr/bin/env bash
# tools/provision-amrex.sh <config> [--force]
#
# Build and install AMReX once, out-of-band, into the shared prefix store
# ${WLI_AMREX_HOME:-$HOME/wli-amrex}/<config>. The root CMakeLists.txt only
# ever find_package()s an installed AMReX prefix; this script is the single
# home of the AMReX flag base and of the -j4 cap (uncapped AMReX builds OOM
# this host).
#
# Configs (each configure preset in CMakePresets.json maps to exactly one):
#   mpi     MPI ON,  GPU NONE, DOUBLE   (the default preset's prefix)
#   serial  MPI OFF, GPU NONE, DOUBLE
#   single  MPI ON,  GPU NONE, SINGLE
#   cuda    MPI ON,  GPU CUDA, DOUBLE   (compile-only consumers)
#   hip     MPI ON,  GPU HIP,  DOUBLE   (compile-only consumers)
#
# Idempotent: if <dest>/lib/cmake/AMReX/AMReXConfig.cmake exists the config is
# considered provisioned and the script exits 0 (--force wipes and rebuilds).
# That validity probe is the same check the root CMakeLists.txt preflights, so
# both ends of the contract agree on what "provisioned" means. The dest and
# the scratch build tree are wiped before building — never build over
# remnants. Source tree: ${AMREX_SRC:-<repo>/../amrex}. Scratch build trees
# live beside the store, under <store>/.scratch/ (never in the repo — the
# repo's own scratch dir is .scratch/). Compiler/toolchain comes from the
# environment (CXX/CC), as with any CMake build.

set -euo pipefail

usage() {
  echo "usage: tools/provision-amrex.sh <mpi|serial|single|cuda|hip> [--force]" >&2
  exit 2
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

config=""
force=0
for arg in "$@"; do
  case "$arg" in
    --force) force=1 ;;
    mpi|serial|single|cuda|hip)
      [ -z "$config" ] || usage
      config="$arg"
      ;;
    *) usage ;;
  esac
done
[ -n "$config" ] || usage

dest="${WLI_AMREX_HOME:-$HOME/wli-amrex}/${config}"

if [ -f "${dest}/lib/cmake/AMReX/AMReXConfig.cmake" ] && [ "$force" -eq 0 ]; then
  echo "✓ ${config} already provisioned at ${dest} (--force to rebuild)"
  exit 0
fi

src="${AMREX_SRC:-${repo_root}/../amrex}"
if [ ! -f "${src}/CMakeLists.txt" ]; then
  echo "✗ AMReX source not found at '${src}' (set AMREX_SRC to an AMReX checkout)" >&2
  exit 1
fi

scratch="${WLI_AMREX_HOME:-$HOME/wli-amrex}/.scratch/amrex-${config}"

# Per-config knobs on top of the shared lean base.
mpi=ON
precision=DOUBLE
gpu=NONE
case "$config" in
  mpi)    ;;
  serial) mpi=OFF ;;
  single) precision=SINGLE ;;
  cuda)   gpu=CUDA ;;
  hip)    gpu=HIP ;;
esac

flags=(
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX="${dest}"
  -DAMReX_GPU_BACKEND="${gpu}"
  -DAMReX_PRECISION="${precision}"
  -DAMReX_MPI="${mpi}"
  -DAMReX_FORTRAN=OFF
  -DAMReX_OMP=OFF
  -DAMReX_INSTALL=ON
  # Trim optional subsystems to cut build time / OOM risk on the RAM-limited
  # host (same lean base the deleted sibling-source arm FORCE-set).
  -DAMReX_LINEAR_SOLVERS=OFF
  -DAMReX_AMRLEVEL=OFF
  -DAMReX_PARTICLES=OFF
  -DAMReX_TINY_PROFILE=OFF
)
# Compile-check target architectures (nothing executes on the GPU configs):
# A100-class and MI250X-class, matching the WLI_CUDA_ARCH/WLI_AMD_ARCH defaults.
case "$config" in
  cuda) flags+=(-DAMReX_CUDA_ARCH=8.0) ;;
  hip)  flags+=(-DAMReX_AMD_ARCH=gfx90a) ;;
esac

# Never build over remnants: a half-installed dest or stale scratch cache
# must not be able to masquerade as a provisioned config.
rm -rf "${dest}" "${scratch}"

echo "provisioning AMReX '${config}' -> ${dest} (source: ${src})"
cmake -S "${src}" -B "${scratch}" "${flags[@]}"
# The -j4 OOM cap lives here, not in user-facing docs.
cmake --build "${scratch}" -j4 --target install

echo "✓ ${config} provisioned at ${dest}"
