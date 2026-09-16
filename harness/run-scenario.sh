#!/usr/bin/env bash
# harness/run-scenario.sh -- run one scenario row from harness/scenarios/ under
# the fleet-soak sampler and report the footprint delta.
#
# usage: harness/run-scenario.sh <row> --seat S --iters N [--smoke] [--print-proc]
#                                [--out harness/runs/<row>.jsonl]
#
# exit codes
#   0  run completed (or --print-proc / smoke-on-manual marker written)
#   1  an iteration or driver hook failed
#   2  usage error / unknown row / driver header malformed
#   3  hackintosh baseline gate failed (fleet-soak report --min-hours 72 --validate)
#   4  row is `# automation: manual` and FLEET_OPERATOR != 1
#
# environment
#   FLEET_OPERATOR=1            allow `# automation: manual` rows
#   FLEET_NO_BASELINE_GATE=1    skip the hackintosh baseline gate
#   FLEET_HOSTNAME              override `hostname -s` (tests)
#   FLEET_SOAK_BIN              override tools/fleet-soak (tests)
#   HARNESS_RUNS_DIR            override harness/runs (tests)
#   HARNESS_BASELINES_DIR       override harness/baselines (tests)
#   HARNESS_SCENARIOS_DIR       override harness/scenarios (tests)
#   HARNESS_LOCK_FILE           override harness/.hackintosh.lock (tests)
#   HARNESS_SAMPLE_INTERVAL     seconds between background samples (default 60)
#   HARNESS_MESH_TOKEN          coordination/token if not readable from Deskflow.conf
#   HARNESS_PYTHON              python with pyobjc Quartz (default: first of
#                               /usr/bin/python3, python3)
#
# Drivers are sourced and must define scenario_setup, scenario_iter <n>,
# scenario_teardown. They may use the harness_* helpers defined below.
set -euo pipefail

HARNESS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$HARNESS_DIR/.." && pwd)"
SCENARIOS_DIR="${HARNESS_SCENARIOS_DIR:-$HARNESS_DIR/scenarios}"
RUNS_DIR="${HARNESS_RUNS_DIR:-$HARNESS_DIR/runs}"
BASELINES_DIR="${HARNESS_BASELINES_DIR:-$HARNESS_DIR/baselines}"
FLEET_SOAK="${FLEET_SOAK_BIN:-$REPO_DIR/tools/fleet-soak}"
SAMPLE_INTERVAL="${HARNESS_SAMPLE_INTERVAL:-60}"
MESH_PORT="${HARNESS_MESH_PORT:-24851}"

usage() {
  sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
  exit 2
}

die() {
  echo "run-scenario: $*" >&2
  exit "${2:-2}"
}

# ---------------------------------------------------------------- args ------
ROW=""
SEAT=""
ITERS=""
SMOKE=0
PRINT_PROC=0
OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --seat) SEAT="${2:-}"; shift 2 ;;
    --iters) ITERS="${2:-}"; shift 2 ;;
    --out) OUT="${2:-}"; shift 2 ;;
    --smoke) SMOKE=1; shift ;;
    --print-proc) PRINT_PROC=1; shift ;;
    -h|--help) usage ;;
    --*) echo "run-scenario: unknown option $1" >&2; usage ;;
    *)
      if [ -n "$ROW" ]; then echo "run-scenario: unexpected argument $1" >&2; usage; fi
      ROW="$1"; shift ;;
  esac
done
[ -n "$ROW" ] || usage
case "$ROW" in */*|*.sh) die "row must be a bare name (got '$ROW')" ;; esac

DRIVER="$SCENARIOS_DIR/$ROW.sh"
[ -f "$DRIVER" ] || die "unknown row '$ROW' (no $DRIVER)"

# ------------------------------------------------------------- header -------
header_field() {
  # header_field <key> -> value of "# <key>: value" in the driver header
  sed -n "s/^# $1:[[:space:]]*//p" "$DRIVER" | head -n1
}
PROC="$(header_field proc)"
AUTOMATION="$(header_field automation)"
ROW_SEAT="$(header_field seat)"
DESCRIPTION="$(header_field description)"

case "$PROC" in
  mouser|deskflow-core|deskflow) ;;
  *) die "$DRIVER: '# proc:' must be mouser|deskflow-core|deskflow (got '$PROC')" ;;
esac
case "$AUTOMATION" in
  full|partial|manual) ;;
  *) die "$DRIVER: '# automation:' must be full|partial|manual (got '$AUTOMATION')" ;;
esac

if [ "$PRINT_PROC" = 1 ]; then
  echo "$PROC"
  exit 0
fi

[ -n "$SEAT" ] || die "--seat is required"
[ -n "$ITERS" ] || die "--iters is required"
case "$ITERS" in ''|*[!0-9]*) die "--iters must be an integer" ;; esac
[ -n "$OUT" ] || OUT="$RUNS_DIR/$ROW.jsonl"
mkdir -p "$(dirname "$OUT")"

TS="$(date -u +%Y%m%dT%H%M%SZ)"
RUN_ID="$ROW-$TS"
HOST="${FLEET_HOSTNAME:-$(hostname -s)}"

# ----------------------------------------------------- manual refusal -------
if [ "$AUTOMATION" = manual ] && [ "${FLEET_OPERATOR:-0}" != 1 ]; then
  if [ "$SMOKE" = 1 ]; then
    printf '{"header":true,"smoke":"skipped-manual","scenario_source":"manual","row":"%s","proc":"%s","seat":"%s","run_id":"%s"}\n' \
      "$ROW" "$PROC" "$SEAT" "$RUN_ID" >>"$OUT"
    echo "run-scenario: $ROW is '# automation: manual'; smoke marker written to $OUT"
    exit 0
  fi
  echo "run-scenario: $ROW is '# automation: manual'; refusing without FLEET_OPERATOR=1" >&2
  exit 4
fi

# ------------------------------------------------- hackintosh serialize -----
LOCK_FILE="${HARNESS_LOCK_FILE:-$HARNESS_DIR/.hackintosh.lock}"
LOCK_DIR=""
release_lock() {
  if [ -n "$LOCK_DIR" ] && [ -d "$LOCK_DIR" ]; then
    rm -rf "$LOCK_DIR"
  fi
}
take_lock() {
  if command -v flock >/dev/null 2>&1; then
    exec 9>"$LOCK_FILE"
    echo "run-scenario: waiting for flock $LOCK_FILE"
    flock 9
  else
    # macOS ships no flock(1): mkdir is atomic, so spin on a lock directory.
    LOCK_DIR="$LOCK_FILE.d"
    local waited=0
    until mkdir "$LOCK_DIR" 2>/dev/null; do
      if [ "$waited" = 0 ]; then echo "run-scenario: waiting for $LOCK_DIR"; fi
      sleep 1
      waited=$((waited + 1))
    done
    echo "$$ $RUN_ID" >"$LOCK_DIR/owner"
  fi
}

ON_HACKINTOSH=0
if [ "$SEAT" = hackintosh ] && [ "$HOST" = hackintosh ]; then
  ON_HACKINTOSH=1
fi

# ------------------------------------------------------ baseline gate -------
baseline_gate() {
  local latest
  latest="$(ls -1 "$BASELINES_DIR"/*-hackintosh-"$PROC".json 2>/dev/null | sort | tail -n1 || true)"
  if [ -z "$latest" ]; then
    echo "run-scenario: no baseline matching $BASELINES_DIR/*-hackintosh-$PROC.json" >&2
    return 1
  fi
  echo "run-scenario: baseline gate on $latest"
  "$FLEET_SOAK" report --in "$latest" --min-hours 72 --validate
}

# ------------------------------------------------------- driver helpers -----
LOG_DIR="$RUNS_DIR/logs/$PROC"
LOG_FILE="$LOG_DIR/$ROW-$TS.log"
mkdir -p "$LOG_DIR"

harness_log() {
  # harness_log <msg...> -> timestamped line to the run log and stdout
  local line
  line="$(date -u +%H:%M:%S) [$ROW] $*"
  echo "$line" | tee -a "$LOG_FILE"
}

harness_todo() {
  # harness_todo <step> -> records an automation gap; the driver must be
  # `# automation: partial`. Never fails the iteration.
  harness_log "TODO(automation): $*"
  return 0
}

harness_python() {
  # harness_python [args...] -> python interpreter (needs pyobjc Quartz for
  # event synthesis; override with HARNESS_PYTHON)
  local py="${HARNESS_PYTHON:-}"
  if [ -z "$py" ]; then
    for cand in /usr/bin/python3 python3; do
      if command -v "$cand" >/dev/null 2>&1; then py="$cand"; break; fi
    done
  fi
  [ -n "$py" ] || { harness_log "no python3 found"; return 1; }
  "$py" "$@"
}

harness_require_quartz() {
  # harness_require_quartz -> 0 iff harness_python can import Quartz
  if ! harness_python -c 'import Quartz' 2>/dev/null; then
    harness_log "python cannot import Quartz; pip install pyobjc-framework-Quartz or set HARNESS_PYTHON"
    return 1
  fi
}

harness_mesh_token() {
  # harness_mesh_token -> coordination/token from HARNESS_MESH_TOKEN or Deskflow.conf
  if [ -n "${HARNESS_MESH_TOKEN:-}" ]; then
    echo "$HARNESS_MESH_TOKEN"
    return 0
  fi
  # Settings::UserSettingFile: ~/Library/Deskflow/Deskflow.conf on macOS,
  # $XDG_CONFIG_HOME/Deskflow/Deskflow.conf elsewhere; system file as fallback.
  local conf
  for conf in "${DESKFLOW_SETTINGS:-}" "$HOME/Library/Deskflow/Deskflow.conf" \
              "${XDG_CONFIG_HOME:-$HOME/.config}/Deskflow/Deskflow.conf" /Library/Deskflow/Deskflow.conf; do
    if [ -n "$conf" ] && [ -f "$conf" ]; then
      awk -F= '/^\[coordination\]/{s=1;next} /^\[/{s=0} s && $1=="token"{print $2; exit}' "$conf"
      return 0
    fi
  done
}

harness_mesh_send() {
  # harness_mesh_send <host> <json-without-token> [read-reply=0|1]
  # One connect per send, newline JSON, matching kvmctl / behavior-spec §2.
  local host="$1" json="$2" want_reply="${3:-0}" token line
  token="$(harness_mesh_token)"
  if [ -n "$token" ]; then
    line="${json%\}},\"token\":\"$token\"}"
  else
    line="$json"
  fi
  harness_python - "$host" "$MESH_PORT" "$line" "$want_reply" <<'PY'
import socket, sys
host, port, line, want = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4] == "1"
try:
    s = socket.create_connection((host, port), 3)
    s.sendall((line + "\n").encode())
    if want:
        s.settimeout(3)
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        sys.stdout.write(buf.decode(errors="replace").strip() + "\n")
    s.close()
except Exception as e:  # noqa: BLE001
    sys.stderr.write(f"mesh {host}:{port}: {e}\n")
    sys.exit(1)
PY
}

harness_mesh_status() {
  # harness_mesh_status [host=127.0.0.1] -> status reply JSON on stdout
  harness_mesh_send "${1:-127.0.0.1}" '{"t":"status"}' 1
}

harness_mesh_promote() {
  # harness_mesh_promote <host> -> that node becomes the Deskflow server
  # (same message `kvmctl primary <name>` sends)
  harness_mesh_send "$1" '{"t":"promote"}' 0
}

harness_mesh_field() {
  # harness_mesh_field <json> <dotted.path> -> value or empty
  HARNESS_JSON="$1" harness_python - "$2" <<'PY'
import json, os, sys
path = sys.argv[1].split(".")
try:
    obj = json.loads(os.environ.get("HARNESS_JSON") or "{}")
    for key in path:
        obj = obj[key]
    print(obj if not isinstance(obj, (dict, list)) else json.dumps(obj))
except Exception:  # noqa: BLE001
    pass
PY
}

harness_mesh_peer_hosts() {
  # harness_mesh_peer_hosts -> one reachable address per configured peer
  # other than self, from the local status reply (fleet.peers[]; lan first).
  local status
  status="$(harness_mesh_status 127.0.0.1)" || return 1
  HARNESS_JSON="$status" harness_python - <<'PY'
import json, os, socket
st = json.loads(os.environ.get("HARNESS_JSON") or "{}")
me = st.get("name", "")
for peer in st.get("fleet", {}).get("peers", []):
    if peer.get("name") == me:
        continue
    for addr in (peer.get("lan"), peer.get("ip")):
        if not addr:
            continue
        try:
            socket.create_connection((addr, 24851), 1).close()
            print(f"{peer['name']} {addr}")
            break
        except Exception:  # noqa: BLE001
            continue
PY
}

harness_sleep() {
  # harness_sleep <seconds> -> shortened to 0.2 s under --smoke
  if [ "${HARNESS_SMOKE:-0}" = 1 ]; then sleep 0.2; else sleep "$1"; fi
}

export HARNESS_SMOKE="$SMOKE"
export HARNESS_ROW="$ROW" HARNESS_PROC="$PROC" HARNESS_SEAT="$SEAT" HARNESS_RUN_ID="$RUN_ID"
export HARNESS_LOG_FILE="$LOG_FILE" HARNESS_OUT="$OUT" HARNESS_ITERS="$ITERS"

# ------------------------------------------------------------- sampler ------
sample_once() {
  # sample_once <phase> -> JSON line (also appended to $OUT by fleet-soak)
  "$FLEET_SOAK" sample --once --label "$PROC" --out "$OUT" 2>>"$LOG_FILE" || {
    harness_log "fleet-soak sample --once failed ($1)"
    echo "{}"
  }
}

SAMPLER_PID=""
start_sampler() {
  (
    while sleep "$SAMPLE_INTERVAL"; do
      "$FLEET_SOAK" sample --once --label "$PROC" --out "$OUT" >/dev/null 2>>"$LOG_FILE" || true
    done
  ) &
  SAMPLER_PID=$!
}
stop_sampler() {
  if [ -n "$SAMPLER_PID" ]; then
    kill "$SAMPLER_PID" 2>/dev/null || true
    wait "$SAMPLER_PID" 2>/dev/null || true
    SAMPLER_PID=""
  fi
}

metric_of() {
  # metric_of <json> <phys_footprint|ports> -> number or empty
  HARNESS_JSON="$1" harness_python - "$2" <<'PY'
import json, os, sys
key = sys.argv[1]
aliases = {
    "phys_footprint": ("phys_footprint", "private_bytes", "value", "metric_value"),
    "ports": ("ports", "mach_ports", "handles", "handle_count"),
}
try:
    lines = (os.environ.get("HARNESS_JSON") or "").strip().splitlines()
    obj = json.loads(lines[-1]) if lines else {}
    for k in aliases[key]:
        if k in obj and isinstance(obj[k], (int, float)):
            print(obj[k]); break
except Exception:  # noqa: BLE001
    pass
PY
}

delta_mb() {
  # delta_mb <before-bytes> <after-bytes> -> "+x.xx" MB or n/a
  if [ -z "$1" ] || [ -z "$2" ]; then echo n/a; return; fi
  harness_python -c 'import sys; a,b=float(sys.argv[1]),float(sys.argv[2]); print(f"{(b-a)/1048576:+.2f}")' "$1" "$2"
}
delta_int() {
  if [ -z "$1" ] || [ -z "$2" ]; then echo n/a; return; fi
  echo $(( $2 - $1 ))
}

# ---------------------------------------------------------------- run -------
TEARDOWN_DONE=0
cleanup() {
  local rc=$?
  stop_sampler
  if [ "$TEARDOWN_DONE" = 0 ] && declare -F scenario_teardown >/dev/null 2>&1; then
    TEARDOWN_DONE=1
    scenario_teardown || true
  fi
  release_lock
  exit "$rc"
}

if [ "$ON_HACKINTOSH" = 1 ]; then
  take_lock
  trap cleanup EXIT INT TERM
  if [ "$SMOKE" = 1 ] || [ "${FLEET_NO_BASELINE_GATE:-0}" = 1 ]; then
    echo "run-scenario: baseline gate skipped"
  elif ! baseline_gate; then
    echo "run-scenario: baseline gate failed for $PROC on hackintosh" >&2
    exit 3
  fi
else
  trap cleanup EXIT INT TERM
fi

# shellcheck source=/dev/null
source "$DRIVER"
for fn in scenario_setup scenario_iter scenario_teardown; do
  declare -F "$fn" >/dev/null 2>&1 || die "$DRIVER does not define $fn"
done

printf '{"header":true,"row":"%s","proc":"%s","seat":"%s","host":"%s","automation":"%s","scenario_source":"scripted","run_id":"%s","iters":%s,"smoke":%s,"description":"%s"}\n' \
  "$ROW" "$PROC" "$SEAT" "$HOST" "$AUTOMATION" "$RUN_ID" "$ITERS" \
  "$([ "$SMOKE" = 1 ] && echo true || echo false)" "${DESCRIPTION//\"/\\\"}" >>"$OUT"

harness_log "run-id: $RUN_ID proc=$PROC seat=$SEAT iters=$ITERS smoke=$SMOKE"
harness_log "log: $LOG_FILE"
harness_log "out: $OUT"

BEFORE="$(sample_once before)"
start_sampler

FAILED=0
if ! scenario_setup; then
  harness_log "scenario_setup failed"
  FAILED=1
else
  n=1
  while [ "$n" -le "$ITERS" ]; do
    if ! scenario_iter "$n"; then
      harness_log "iteration $n failed"
      FAILED=1
      break
    fi
    n=$((n + 1))
  done
fi

TEARDOWN_DONE=1
scenario_teardown || { harness_log "scenario_teardown failed"; FAILED=1; }

stop_sampler
AFTER="$(sample_once after)"

B_FOOT="$(metric_of "$BEFORE" phys_footprint)"
A_FOOT="$(metric_of "$AFTER" phys_footprint)"
B_PORTS="$(metric_of "$BEFORE" ports)"
A_PORTS="$(metric_of "$AFTER" ports)"
DELTA_MB="$(delta_mb "$B_FOOT" "$A_FOOT")"
DELTA_PORTS="$(delta_int "$B_PORTS" "$A_PORTS")"

harness_log "Δ phys_footprint: $DELTA_MB MB (before=${B_FOOT:-n/a} after=${A_FOOT:-n/a})"
harness_log "Δ ports: $DELTA_PORTS (before=${B_PORTS:-n/a} after=${A_PORTS:-n/a})"
echo "run-id: $RUN_ID"
echo "deltaMB: $DELTA_MB"
echo "deltaPorts: $DELTA_PORTS"

if [ "$FAILED" != 0 ]; then
  harness_log "result: FAIL"
  exit 1
fi
harness_log "result: ok"
exit 0
