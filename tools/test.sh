#!/usr/bin/env bash
# tools/test.sh [preset] [--tables [path]] — run one preset's ctest suite.
#
# Only tests — build first with tools/build.sh. Refuses the presets that must
# never ctest: cuda|rocm (compile-check only, nothing executes), notests
# (nothing registered), guard-trip (negative-only — its configure is supposed
# to fail).
#
# --tables makes the real-table cells (production_tables, ...) run instead of
# SKIP: an explicit path argument wins, else an already-exported
# WL_TABLES_ROOT, else a loud refusal naming the variable — a green --tables
# run can never silently mean the table cells SKIPped.

set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/wli-common.sh"

preset="default"
preset_given=0
want_tables=0
tables_path=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --tables)
      want_tables=1
      # Optional path argument: consume the next word unless it is a flag.
      if [ "$#" -gt 1 ] && [ "${2#-}" = "$2" ]; then
        tables_path="$2"
        shift
      fi
      ;;
    -*)
      refuse "unknown flag '$1' (usage: tools/test.sh [preset] [--tables [path]])"
      ;;
    *)
      if [ "${preset_given}" -eq 1 ]; then
        refuse "more than one preset given ('${preset}' and '$1')"
      fi
      preset="$1"
      preset_given=1
      ;;
  esac
  shift
done

case "${preset}" in
  cuda|rocm)
    refuse "preset '${preset}' is compile-check only — nothing executes, there is nothing to ctest (build it with tools/build.sh ${preset})"
    ;;
  notests)
    refuse "preset 'notests' is the library-only build — no tests are registered (ctest -N lists 0)"
    ;;
  guard-trip)
    refuse "preset 'guard-trip' exists solely so its configure FAILS (consistency-guard negative check) — it is never built or tested"
    ;;
esac

if [ "${want_tables}" -eq 1 ]; then
  if [ -n "${tables_path}" ]; then
    export WL_TABLES_ROOT="${tables_path}"
  elif [ -n "${WL_TABLES_ROOT:-}" ]; then
    export WL_TABLES_ROOT
  else
    refuse "--tables given with no path and no WL_TABLES_ROOT exported — pass a table directory or export WL_TABLES_ROOT (otherwise the real-table cells would silently SKIP)"
  fi
fi

resolve_amrex_env
cd "${wli_repo_root}"
q ctest --preset "${preset}"
