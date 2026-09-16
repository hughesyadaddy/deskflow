#!/usr/bin/env bash
# proc: deskflow-core
# automation: manual
# seat: hackintosh
# description: log out to the login window, drive the vhid login bridge from a peer, log back in (login-window core + org.deskflow.vhid-bridge lifecycle)
#
# OPERATOR ROW. Each iteration ends the GUI session, which ends this script's
# ability to sample or to log the operator back in, so run-scenario.sh refuses
# this row unless FLEET_OPERATOR=1 and the operator sits at hackintosh:
#   FLEET_OPERATOR=1 harness/run-scenario.sh login-bridge --seat hackintosh --iters 1000
# Per iteration the driver logs out (System Events), waits for the login-window
# bridge (org.deskflow.vhid-bridge, see scripts/install-login-bridge-macos.sh)
# to come up, and then waits for the operator to log back in from another seat
# via the bridge. The fleet-soak launchd sampler keeps sampling across the
# logout; run-scenario.sh's own samples only cover the logged-in windows.
# Sourced by harness/run-scenario.sh.

LB_AGENT_LABEL="org.deskflow.vhid-bridge"
LB_LOGOUT_WAIT="${HARNESS_LB_LOGOUT_WAIT:-30}"
LB_LOGIN_WAIT="${HARNESS_LB_LOGIN_WAIT:-120}"

_lb_bridge_loaded() {
  launchctl print "loginwindow/$LB_AGENT_LABEL" >/dev/null 2>&1 ||
    pgrep -xq deskflow-vhid-bridge
}

scenario_setup() {
  [ -f "/Library/LaunchAgents/$LB_AGENT_LABEL.plist" ] ||
    { harness_log "login bridge not installed (scripts/install-login-bridge-macos.sh)"; return 1; }
  harness_log "operator: log back in from a peer through the login bridge after each logout"
}

scenario_iter() {
  local n="$1" waited=0
  osascript -e 'tell application "System Events" to log out' || return 1
  harness_sleep "$LB_LOGOUT_WAIT"
  # From here on this script only survives if it was started under nohup /
  # a launchd job outside the GUI session; otherwise the iteration ends here
  # and the operator restarts the row after logging in.
  while [ "$waited" -lt "$LB_LOGIN_WAIT" ]; do
    if [ -n "$(who | grep -w console || true)" ] && ! _lb_bridge_loaded; then
      break
    fi
    sleep 5
    waited=$((waited + 5))
  done
  harness_log "iter $n: logged back in after ${waited}s (bridge loaded: $(_lb_bridge_loaded && echo yes || echo no))"
  return 0
}

scenario_teardown() { :; }
