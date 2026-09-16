#!/usr/bin/env bash
# proc: mouser
# automation: full
# seat: hackintosh
# description: synthesize scroll-wheel bursts with Input Monitoring denied to Mouser (permission-retry / fallback path)
#
# Same event generator as scroll-im-granted. The driver never touches TCC
# (no tccutil): it records the Input Monitoring state visible to the probe
# process (CGPreflightListenEventAccess) and tags the run with
# `im_expected: denied` so the report can separate the two scroll rows.
# Revoke Input Monitoring for Mouser in System Settings before running.
# Sourced by harness/run-scenario.sh.

SCROLL_EXPECT=denied
SCROLL_TICKS="${HARNESS_SCROLL_TICKS:-200}"

_scroll_im_state() {
  harness_python - <<'PY'
import Quartz
print("granted" if Quartz.CGPreflightListenEventAccess() else "denied")
PY
}

_scroll_burst() {
  harness_python - "$SCROLL_TICKS" <<'PY'
import sys, time, Quartz
ticks = int(sys.argv[1])
for i in range(ticks):
    delta = 3 if (i // 25) % 2 == 0 else -3
    ev = Quartz.CGEventCreateScrollWheelEvent(None, Quartz.kCGScrollEventUnitLine, 1, delta)
    Quartz.CGEventPost(Quartz.kCGHIDEventTap, ev)
    time.sleep(0.004)
PY
}

scenario_setup() {
  local state
  harness_require_quartz || return 1
  state="$(_scroll_im_state)" || return 1
  harness_log "im_state=$state expected=$SCROLL_EXPECT (probe: harness python, not Mouser)"
  printf '{"tag":true,"run_id":"%s","im_state":"%s","im_expected":"%s"}\n' \
    "$HARNESS_RUN_ID" "$state" "$SCROLL_EXPECT" >>"$HARNESS_OUT"
  if [ "$state" != "$SCROLL_EXPECT" ]; then
    harness_log "WARNING: probe Input Monitoring state is '$state', row expects '$SCROLL_EXPECT'"
  fi
  pgrep -xq "${HARNESS_MOUSER_APP:-Mouser}" || harness_log "WARNING: Mouser is not running"
}

scenario_iter() {
  local n="$1"
  _scroll_burst || return 1
  harness_sleep 0.5
  if [ $((n % 100)) = 0 ]; then harness_log "iter $n done"; fi
  return 0
}

scenario_teardown() { :; }
