#!/usr/bin/env bash
# harness/canary.sh -- weekly regression canary (plan Phase 7).
#
#   harness/canary.sh --seat hackintosh [--iters 200]
#
# For every harness/scenarios/*.sh tagged `# automation: full`:
#   1. harness/run-scenario.sh <row> --seat <seat> --iters <iters>
#   2. tools/fleet-soak report --accelerated --proc $(run-scenario.sh <row> --print-proc)
#        --in harness/runs/<row>.jsonl --window 1 --slope-max 5
#        --ports-slope-max 10 --max-restarts 0
# then harness/behavior-bench.sh --all --seat <seat>.
# Exits non-zero if any step fails and prints a per-row summary.
#
# Overridable for tests (PATH shims / stub roots):
#   CANARY_ROOT      repo root (default: parent of this script's directory)
#   SCENARIO_DIR     default $CANARY_ROOT/harness/scenarios
#   RUNS_DIR         default $CANARY_ROOT/harness/runs
#   RUN_SCENARIO     default $CANARY_ROOT/harness/run-scenario.sh
#   FLEET_SOAK       default $CANARY_ROOT/tools/fleet-soak
#   BEHAVIOR_BENCH   default $CANARY_ROOT/harness/behavior-bench.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${CANARY_ROOT:-$(cd "$HERE/.." && pwd)}"
SCENARIO_DIR="${SCENARIO_DIR:-$ROOT/harness/scenarios}"
RUNS_DIR="${RUNS_DIR:-$ROOT/harness/runs}"
RUN_SCENARIO="${RUN_SCENARIO:-$ROOT/harness/run-scenario.sh}"
FLEET_SOAK="${FLEET_SOAK:-$ROOT/tools/fleet-soak}"
BEHAVIOR_BENCH="${BEHAVIOR_BENCH:-$ROOT/harness/behavior-bench.sh}"

seat=""
iters=200
while [ $# -gt 0 ]; do
  case "$1" in
    --seat) seat="${2:?--seat needs a value}"; shift 2 ;;
    --iters) iters="${2:?--iters needs a value}"; shift 2 ;;
    -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "canary: unknown argument: $1" >&2; exit 2 ;;
  esac
done
if [ -z "$seat" ]; then
  echo "canary: --seat is required" >&2
  exit 2
fi

mkdir -p "$RUNS_DIR"

rows=()
for f in "$SCENARIO_DIR"/*.sh; do
  [ -e "$f" ] || continue
  grep -Eq '^# automation: full[[:space:]]*$' "$f" || continue
  rows+=("$(basename "$f" .sh)")
done

summary=()
failures=0

record() { # name status
  summary+=("$(printf '%-28s %s' "$1" "$2")")
  [ "$2" = "PASS" ] || failures=$((failures + 1))
}

for row in ${rows[@]+"${rows[@]}"}; do
  if ! "$RUN_SCENARIO" "$row" --seat "$seat" --iters "$iters"; then
    record "scenario:$row" "FAIL (run-scenario)"
    continue
  fi
  proc="$("$RUN_SCENARIO" "$row" --print-proc)" || proc=""
  if [ -z "$proc" ]; then
    record "scenario:$row" "FAIL (no proc)"
    continue
  fi
  if "$FLEET_SOAK" report --accelerated --proc "$proc" \
       --in "$RUNS_DIR/$row.jsonl" --window 1 --slope-max 5 \
       --ports-slope-max 10 --max-restarts 0; then
    record "scenario:$row" "PASS"
  else
    record "scenario:$row" "FAIL (fleet-soak rc=$?)"
  fi
done

if "$BEHAVIOR_BENCH" --all --seat "$seat"; then
  record "behavior-bench" "PASS"
else
  record "behavior-bench" "FAIL (rc=$?)"
fi

echo
echo "canary summary (seat=$seat, iters=$iters, rows=${#rows[@]})"
for line in ${summary[@]+"${summary[@]}"}; do echo "  $line"; done
if [ "$failures" -gt 0 ]; then
  echo "canary: $failures failure(s)"
  exit 1
fi
echo "canary: all green"
exit 0
