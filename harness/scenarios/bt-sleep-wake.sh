#!/usr/bin/env bash
# proc: mouser
# automation: manual
# seat: hackintosh
# description: power Bluetooth off and on so the mouse drops and reconnects (Mouser device-rescan / HID re-open path)
#
# OPERATOR ROW. Toggling Bluetooth kills the operator's own input, so
# run-scenario.sh refuses this row unless FLEET_OPERATOR=1:
#   FLEET_OPERATOR=1 harness/run-scenario.sh bt-sleep-wake --seat hackintosh --iters 1000
# Needs `blueutil` (brew install blueutil). Sourced by harness/run-scenario.sh.

BT_OFF_SECS="${HARNESS_BT_OFF_SECS:-8}"
BT_ON_SECS="${HARNESS_BT_ON_SECS:-12}"

scenario_setup() {
  command -v blueutil >/dev/null 2>&1 || { harness_log "blueutil not installed (brew install blueutil)"; return 1; }
  harness_log "bluetooth power: $(blueutil -p)"
  [ "$(blueutil -p)" = 1 ] || { harness_log "bluetooth is off; turn it on before the run"; return 1; }
}

scenario_iter() {
  local n="$1"
  blueutil -p 0 || return 1
  harness_sleep "$BT_OFF_SECS"
  blueutil -p 1 || return 1
  harness_sleep "$BT_ON_SECS"
  if [ $((n % 10)) = 0 ]; then harness_log "iter $n done (power=$(blueutil -p))"; fi
  return 0
}

scenario_teardown() {
  blueutil -p 1 || true
}
