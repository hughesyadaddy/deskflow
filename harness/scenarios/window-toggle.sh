#!/usr/bin/env bash
# proc: mouser
# automation: full
# seat: hackintosh
# description: show and hide the Mouser window repeatedly via AppleScript (per-show view/QML rebuild)
#
# Mouser.app registers no URL scheme (Info.plist has no CFBundleURLTypes), so
# the toggle goes through AppleScript: `reopen` + `activate` on the app
# re-shows its main window, and System Events hides the process.
# Sourced by harness/run-scenario.sh.

WT_APP="${HARNESS_MOUSER_APP:-Mouser}"
WT_SETTLE="${HARNESS_WT_SETTLE:-1}"

scenario_setup() {
  if ! pgrep -xq "$WT_APP"; then
    harness_log "$WT_APP is not running; launching"
    open -a "$WT_APP" || return 1
    harness_sleep 3
  fi
  osascript -e "tell application \"System Events\" to exists process \"$WT_APP\"" | grep -q true ||
    { harness_log "System Events cannot see process $WT_APP"; return 1; }
}

scenario_iter() {
  local n="$1"
  osascript -e "tell application \"$WT_APP\" to reopen" -e "tell application \"$WT_APP\" to activate" || return 1
  harness_sleep "$WT_SETTLE"
  osascript -e "tell application \"System Events\" to set visible of process \"$WT_APP\" to false" || return 1
  harness_sleep "$WT_SETTLE"
  if [ $((n % 100)) = 0 ]; then harness_log "iter $n done"; fi
  return 0
}

scenario_teardown() {
  osascript -e "tell application \"System Events\" to set visible of process \"$WT_APP\" to false" || true
}
