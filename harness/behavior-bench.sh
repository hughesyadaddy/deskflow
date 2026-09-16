#!/usr/bin/env bash
# harness/behavior-bench.sh -- input-path latency bench with per-seat baselines.
#
#   harness/behavior-bench.sh --proc mouser|deskflow-core --seat S [--baseline] [--all] [--json]
#                             [--local] [--runs N] [--count N]
#   harness/behavior-bench.sh --listener --proc P [--count N] [--timeout S]
#   harness/behavior-bench.sh --evaluate FILE --proc P --seat S [--baseline] [--all] [--json]
#
# deskflow-core: 200 synthetic CGEvent mouse moves posted on the server seat,
#   tagged in kCGEventSourceUserData, timestamped again by a listen-only
#   CGEventTap on the client seat (`--listener` launched over `ssh <seat>`
#   unless --local or the seat is this host).
# mouser: 200 synthetic scroll ticks, timed tick -> Mouser's re-injected event.
# 5 runs x 200, first run dropped. --baseline writes
# harness/baselines/behavior-<seat>.json (merged per proc); otherwise compares:
# pass = p50 and p95 within +/-10 % of baseline AND delivered == sent.
# Exit: 0 pass, 1 regression, 2 invalid (no baseline / no Quartz / bad args),
# 3 refused (--baseline would overwrite an existing baseline; FLEET_OPERATOR=1).
#
# All logic lives in behavior-bench.py (python3 stdlib; Quartz via PyObjC only
# for live measurement). This wrapper only routes arguments.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="${BEHAVIOR_BENCH_PY:-$HERE/behavior-bench.py}"
PYTHON="${PYTHON:-python3}"

usage() { sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

mode=bench
eval_file=""
args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --listener) mode=listener; shift ;;
    --evaluate) mode=evaluate; eval_file="${2:?--evaluate needs a file}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) args+=("$1"); shift ;;
  esac
done

# ${args[@]+"${args[@]}"} keeps bash 3.2 + set -u happy on an empty array.
case "$mode" in
  listener) exec "$PYTHON" "$PY" listener ${args[@]+"${args[@]}"} ;;
  evaluate) exec "$PYTHON" "$PY" evaluate --runs-file "$eval_file" ${args[@]+"${args[@]}"} ;;
  bench)    exec "$PYTHON" "$PY" bench ${args[@]+"${args[@]}"} ;;
esac
