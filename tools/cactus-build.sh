#!/usr/bin/env bash
# tools/cactus-build.sh [--fresh] — the one-command in-Cactus build
# (specs/cactus-integration.md V#5; manual and host-gated by spec — never in
# ctest or CI).
#
# Mirrors CarpetX/agent_scripts/build.sh: requires $CACTUSX; option file from
# ETKCFG (sandbox) else $ETKGUIDE/macos.cfg (host); ensures the
# arrangements/WeakLibInterp -> <repo>/cactus/thorns symlink; drives
# Compile-ETK against the committed compile thornlist cactus/wli.th into the
# isolated configs/wli (the working carpetx configuration is never touched),
# logging to $CACTUSX/last-build-wli.log.
#
# Incremental by default (Compile-ETK re-copies the thornlist and rebuilds);
# --fresh passes through to Compile-ETK --fresh, which deletes configs/wli and
# reconfigures from the option file. A missing configs/wli auto-freshens —
# Compile-ETK's thornlist copy needs the config to exist.
#
# Success is asserted at symbol level, not just exit code: exe/cactus_wli must
# exist and nm must find wli_value_type_size in it — the TestWeakLibInterp
# consumer's startup routine is what forces that libwli_lib.a member into the
# link, so a green run means the library really linked, not merely appeared on
# the link line.

set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/wli-common.sh"

fresh=0
for arg in "$@"; do
  case "${arg}" in
    --fresh) fresh=1 ;;
    *) refuse "unknown argument '${arg}' (usage: tools/cactus-build.sh [--fresh])" ;;
  esac
done

[ -n "${CACTUSX:-}" ] || refuse "CACTUSX not set — point it at the Cactus checkout"
[ -d "${CACTUSX}" ] || refuse "CACTUSX is not a directory: ${CACTUSX}"

cfg="${ETKCFG:-${ETKGUIDE:+${ETKGUIDE}/macos.cfg}}"
[ -n "${cfg}" ] || refuse "no option file — set ETKCFG (sandbox) or ETKGUIDE (host: uses \$ETKGUIDE/macos.cfg)"
[ -f "${cfg}" ] || refuse "option file not found: ${cfg}"

command -v Compile-ETK >/dev/null 2>&1 || refuse "Compile-ETK not on PATH"

thornlist="${wli_repo_root}/cactus/wli.th"
log="${CACTUSX}/last-build-wli.log"

# The arrangement is this repo's thorn directory, symlinked wholesale — both
# WeakLibInterp/WeakLibInterp and WeakLibInterp/TestWeakLibInterp resolve
# through it (build-from-checkout, zero drift, spec:65).
ln -sfn "${wli_repo_root}/cactus/thorns" "${CACTUSX}/arrangements/WeakLibInterp"

# Compile-ETK copies the thornlist into configs/wli before building, so the
# config must exist; first run (or a wiped checkout) auto-freshens.
if [ ! -d "${CACTUSX}/configs/wli" ]; then
  fresh=1
fi

compile_args=(-e wli -j8 -c "${cfg}" -t "${thornlist}")
if [ "${fresh}" -eq 1 ]; then
  compile_args=(--fresh "${compile_args[@]}")
fi

if (
  cd "${CACTUSX}" &&
  Compile-ETK "${compile_args[@]}"
) > "${log}" 2>&1; then
  echo "✓ in-Cactus build (configs/wli)"
else
  echo "--- first error lines ---"
  grep -n -m 12 -E 'error:|Error|\*\*\*' "${log}" || echo "(no obvious error lines)"
  echo "--- last 40 lines ---"
  tail -40 "${log}"
  echo "✗ in-Cactus build failed — full log: ${log}" >&2
  exit 1
fi

exe="${CACTUSX}/exe/cactus_wli"
if [ ! -x "${exe}" ]; then
  echo "✗ expected executable missing: ${exe} (log: ${log})" >&2
  exit 1
fi

# The mangled C++ name contains the identifier, so plain grep suffices.
# NOT grep -q: under pipefail its early exit SIGPIPEs nm and a found symbol
# reads as failure — plain grep drains the pipe.
if nm "${exe}" | grep wli_value_type_size >/dev/null; then
  echo "✓ cactus_wli links libwli_lib (wli_value_type_size present)"
else
  echo "✗ wli_value_type_size not in ${exe} — no libwli_lib.a member was pulled into the link (log: ${log})" >&2
  exit 1
fi
