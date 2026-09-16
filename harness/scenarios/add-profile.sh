#!/usr/bin/env bash
# proc: mouser
# automation: full
# seat: hackintosh
# description: open and dismiss Mouser's Add-Profile dialog repeatedly via the accessibility tree (dialog + model churn)
#
# Same AppleScript route as window-toggle (Mouser has no URL scheme): show
# the window, click the "+" button next to the app combo box at the bottom of
# the profiles list (Mouser DEVELOPMENT.md "Add profile"), then press Escape
# to dismiss without saving. HARNESS_ADD_PROFILE_BUTTON overrides the AX
# name if the button label changes. Sourced by harness/run-scenario.sh.

AP_APP="${HARNESS_MOUSER_APP:-Mouser}"
AP_BUTTON="${HARNESS_ADD_PROFILE_BUTTON:-+}"
AP_SETTLE="${HARNESS_AP_SETTLE:-1}"

_ap_click_add() {
  osascript - "$AP_APP" "$AP_BUTTON" <<'APPLESCRIPT'
on run argv
  set appName to item 1 of argv
  set wanted to item 2 of argv
  tell application "System Events"
    tell process appName
      set frontmost to true
      set btns to every button of window 1 whose (name is wanted or description is wanted or title is wanted)
      if (count of btns) is 0 then
        set btns to every button of (every group of window 1) whose (name is wanted or description is wanted)
      end if
      if (count of btns) is 0 then error "no button named " & wanted & " in " & appName & " window 1"
      click item 1 of btns
    end tell
  end tell
end run
APPLESCRIPT
}

scenario_setup() {
  if ! pgrep -xq "$AP_APP"; then
    harness_log "$AP_APP is not running; launching"
    open -a "$AP_APP" || return 1
    harness_sleep 3
  fi
  osascript -e "tell application \"$AP_APP\" to reopen" -e "tell application \"$AP_APP\" to activate" || return 1
  harness_sleep "$AP_SETTLE"
  osascript -e "tell application \"System Events\" to tell process \"$AP_APP\" to exists window 1" | grep -q true ||
    { harness_log "$AP_APP has no window; is Accessibility granted to the terminal?"; return 1; }
}

scenario_iter() {
  local n="$1"
  _ap_click_add || return 1
  harness_sleep "$AP_SETTLE"
  osascript -e "tell application \"System Events\" to tell process \"$AP_APP\" to key code 53" || return 1
  harness_sleep "$AP_SETTLE"
  if [ $((n % 100)) = 0 ]; then harness_log "iter $n done"; fi
  return 0
}

scenario_teardown() {
  osascript -e "tell application \"System Events\" to tell process \"$AP_APP\" to key code 53" || true
  osascript -e "tell application \"System Events\" to set visible of process \"$AP_APP\" to false" || true
}
