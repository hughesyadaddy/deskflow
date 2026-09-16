#!/usr/bin/env bash
# proc: deskflow-core
# automation: manual
# seat: hackintosh
# description: lock the session and wake the display so deskflow-core crosses the lock screen repeatedly (OSXScreen pasteboard/AX polling, tap re-enable)
#
# OPERATOR ROW. Locking requires the operator to unlock (password / Touch ID)
# unless auto-login unlock is configured, so run-scenario.sh refuses this row
# unless FLEET_OPERATOR=1:
#   FLEET_OPERATOR=1 harness/run-scenario.sh lock-unlock --seat hackintosh --iters 1000
# `CGSession -suspend` locks; `caffeinate -u -t 1` asserts user activity to
# wake the display and present the unlock prompt. Sourced by harness/run-scenario.sh.

CGSESSION=/System/Library/CoreServices/Menu\ Extras/User.menu/Contents/Resources/CGSession
LOCK_SECS="${HARNESS_LOCK_SECS:-10}"
UNLOCK_WAIT="${HARNESS_UNLOCK_WAIT:-20}"

scenario_setup() {
  [ -x "$CGSESSION" ] || { harness_log "CGSession not found at $CGSESSION"; return 1; }
  harness_log "operator must unlock after each lock (or configure auto-unlock); lock=${LOCK_SECS}s wait=${UNLOCK_WAIT}s"
}

scenario_iter() {
  local n="$1"
  "$CGSESSION" -suspend || return 1
  harness_sleep "$LOCK_SECS"
  caffeinate -u -t 1 || return 1
  harness_sleep "$UNLOCK_WAIT"
  if [ $((n % 10)) = 0 ]; then harness_log "iter $n done"; fi
  return 0
}

scenario_teardown() {
  caffeinate -u -t 1 || true
}
