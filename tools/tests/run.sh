#!/usr/bin/env bash
# Fleet tooling test runner.
#
#   tools/tests/run.sh            run everything
#   tools/tests/run.sh -v         pass extra flags through to bats
#
# Runs, in order:
#   1. every tools/tests/*.bats with the vendored bats-core (tools/tests/bats/)
#   2. every tools/tests/*.sh test script other than this runner
#   3. every tools/tests/*.Tests.ps1 with Pester, or SKIP when pwsh is absent
#
# Exit code is non-zero if any suite fails. Nothing here is allowed to
# succeed silently: a missing runner is an error, a missing optional runtime
# (pwsh) is a printed SKIP.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
BATS="$HERE/bats/bin/bats"
export REPO_ROOT

status=0
ran_any=0

run_suite() {
  # $1 = label, rest = command
  local label="$1"
  shift
  echo "== $label =="
  if "$@"; then
    echo "== PASS: $label =="
  else
    echo "== FAIL: $label (exit $?) ==" >&2
    status=1
  fi
  ran_any=1
}

# --- 1. bats -----------------------------------------------------------------
shopt -s nullglob
bats_files=("$HERE"/*.bats)
shopt -u nullglob
if ((${#bats_files[@]})); then
  if [[ ! -x "$BATS" ]]; then
    echo "error: vendored bats missing or not executable at $BATS" >&2
    exit 1
  fi
  run_suite "bats (${#bats_files[@]} files)" "$BATS" "$@" "${bats_files[@]}"
fi

# --- 2. shell test scripts ---------------------------------------------------
shopt -s nullglob
sh_files=("$HERE"/*.sh)
shopt -u nullglob
for f in "${sh_files[@]}"; do
  [[ "$f" == "$HERE/run.sh" ]] && continue
  run_suite "$(basename "$f")" bash "$f"
done

# --- 3. Pester ---------------------------------------------------------------
shopt -s nullglob
ps_files=("$HERE"/*.Tests.ps1)
shopt -u nullglob
if ((${#ps_files[@]})); then
  if command -v pwsh >/dev/null 2>&1; then
    for f in "${ps_files[@]}"; do
      # -CI makes Pester exit non-zero on any failed test.
      run_suite "$(basename "$f")" pwsh -NoProfile -NonInteractive -Command \
        "Invoke-Pester -Path '$f' -CI"
    done
  else
    for f in "${ps_files[@]}"; do
      echo "SKIP: $(basename "$f") (pwsh not installed)"
    done
  fi
fi

if ((ran_any == 0)); then
  echo "error: no tests found under $HERE" >&2
  exit 1
fi

if ((status == 0)); then
  echo "== ALL SUITES PASSED =="
else
  echo "== SOME SUITES FAILED ==" >&2
fi
exit "$status"
