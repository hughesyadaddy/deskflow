#!/usr/bin/env bats
# scripts/deskflow-ctl — launchd is the only owner of Deskflow on macOS.
#
# launchctl / ps / kill / sleep are PATH shims. Fake launchd state lives in
# $SHIM_STATE: loaded/<label> marks a bootstrapped agent, pid/<label> is the
# pid launchd reports, and ps.txt is the process table (pid uid path; uid
# defaults to the caller's, since stop only owns this user's processes).

SCRIPT="$BATS_TEST_DIRNAME/../../scripts/deskflow-ctl"
REPO="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
  TMP="$(mktemp -d "${BATS_TEST_TMPDIR:-${TMPDIR:-/tmp}}/deskflow-ctl.XXXXXX")"
  SHIMS="$TMP/bin"
  export SHIM_LOG="$TMP/calls.log"
  export SHIM_STATE="$TMP/state"
  export APP="$TMP/Applications/Deskflow.app"
  export HOME="$TMP/home"
  STATE="$HOME/Library/Application Support/Deskflow"
  mkdir -p "$SHIMS" "$SHIM_STATE/loaded" "$SHIM_STATE/pid" "$APP/Contents/MacOS" "$HOME"
  : >"$SHIM_LOG"
  : >"$SHIM_STATE/ps.txt"
  printf '#!/bin/sh\n' >"$APP/Contents/MacOS/deskflow-core"
  chmod +x "$APP/Contents/MacOS/deskflow-core"

  make_shim launchctl <<'EOF'
echo "launchctl $*" >> "$SHIM_LOG"
verb="$1"; shift
case "$verb" in
  print)
    label="${1##*/}"
    [[ -f "$SHIM_STATE/loaded/$label" ]] || exit 113
    echo "$label = {"
    [[ -f "$SHIM_STATE/pid/$label" ]] && echo "	pid = $(cat "$SHIM_STATE/pid/$label")"
    echo "}"
    ;;
  bootstrap)
    label="$(basename "$2" .plist)"
    touch "$SHIM_STATE/loaded/$label"
    if [[ -f "$SHIM_STATE/autostart/$label" ]]; then
      cp "$SHIM_STATE/autostart/$label" "$SHIM_STATE/pid/$label"
      echo "$(cat "$SHIM_STATE/pid/$label")	$(id -u)	$(cat "$SHIM_STATE/autostart/$label.path")" >> "$SHIM_STATE/ps.txt"
    fi
    ;;
  bootout)
    label="${1##*/}"
    rm -f "$SHIM_STATE/loaded/$label"
    if [[ -f "$SHIM_STATE/pid/$label" ]]; then
      pid="$(cat "$SHIM_STATE/pid/$label")"
      rm -f "$SHIM_STATE/pid/$label"
      # A stubborn process ignores bootout until it is signalled.
      if [[ ! -f "$SHIM_STATE/stubborn/$pid" ]]; then
        grep -v "^$pid	" "$SHIM_STATE/ps.txt" > "$SHIM_STATE/ps.tmp"; mv "$SHIM_STATE/ps.tmp" "$SHIM_STATE/ps.txt"
      fi
    fi
    ;;
  kickstart) ;;
esac
exit 0
EOF

  make_shim ps <<'EOF'
echo "ps $*" >> "$SHIM_LOG"
cat "$SHIM_STATE/ps.txt"
EOF

  make_shim kill <<'EOF'
echo "kill $*" >> "$SHIM_LOG"
sig="${1#-}"; pid="$2"
if [[ "$sig" == "KILL" || ! -f "$SHIM_STATE/stubborn/$pid" ]]; then
  grep -v "^$pid	" "$SHIM_STATE/ps.txt" > "$SHIM_STATE/ps.tmp"; mv "$SHIM_STATE/ps.tmp" "$SHIM_STATE/ps.txt"
fi
exit 0
EOF

  make_shim sleep <<'EOF'
exit 0
EOF

  # The only osascript use is the converge toast; never show a real one.
  make_shim osascript <<'EOF'
echo "osascript $*" >> "$SHIM_LOG"
exit 0
EOF

  export PATH="$SHIMS:$PATH"
  export DESKFLOW_INSTALL_APP="$APP"
  export DESKFLOW_CTL_AGENT_DIR="$TMP/LaunchAgents"
  export DESKFLOW_CTL_DAEMON_DIR="$TMP/LaunchDaemons"
  export DESKFLOW_CTL_DAEMON_STAGE_DIR="$TMP/stage"
  export DESKFLOW_CTL_STOP_TIMEOUT=1
  export DESKFLOW_CTL_ESCALATE_TIMEOUT=1
  export DESKFLOW_CTL_START_TIMEOUT=1
}

teardown() {
  rm -rf "$TMP"
}

make_shim() {
  { echo '#!/usr/bin/env bash'; cat; } >"$SHIMS/$1"
  chmod +x "$SHIMS/$1"
}

add_proc() { printf '%s\t%s\t%s\n' "$1" "${3:-$(id -u)}" "$2" >>"$SHIM_STATE/ps.txt"; }
load_agent() { touch "$SHIM_STATE/loaded/$1"; [[ -n "${2:-}" ]] && echo "$2" >"$SHIM_STATE/pid/$1"; return 0; }
autostart() { mkdir -p "$SHIM_STATE/autostart"; echo "$2" >"$SHIM_STATE/autostart/$1"; echo "$3" >"$SHIM_STATE/autostart/$1.path"; }

log_has() { grep -qF -- "$1" "$SHIM_LOG" || { echo "missing from shim log: $1" >&2; return 1; }; }
log_lacks() { ! grep -qF -- "$1" "$SHIM_LOG" || { echo "unexpected in shim log: $1" >&2; return 1; }; }

CORE=io.github.hughesyadaddy.deskflow-core
GUI=io.github.hughesyadaddy.deskflow
CONVERGE=io.github.hughesyadaddy.deskflow-converge
DOMAIN="gui/$(id -u)"

# --- stop ---------------------------------------------------------------------

@test "stop boots out GUI then core and returns once the bundle's processes are gone" {
  load_agent $CORE 100; add_proc 100 "$APP/Contents/MacOS/deskflow-core"
  load_agent $GUI 200;  add_proc 200 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" stop
  [ "$status" -eq 0 ]
  log_has "launchctl bootout $DOMAIN/$GUI"
  log_has "launchctl bootout $DOMAIN/$CORE"
  log_lacks "kill "
  [[ "$output" == *"stopped"* ]]
  [ ! -s "$SHIM_STATE/ps.txt" ]
}

@test "stop waits, then escalates by PID: SIGTERM, then SIGKILL, and never pkill -f" {
  load_agent $CORE 100; add_proc 100 "$APP/Contents/MacOS/deskflow-core"
  mkdir -p "$SHIM_STATE/stubborn"; touch "$SHIM_STATE/stubborn/100"
  run bash "$SCRIPT" stop
  [ "$status" -eq 0 ]
  log_has "launchctl bootout $DOMAIN/$CORE"
  log_has "kill -TERM 100"
  log_has "kill -KILL 100"
  [[ "$output" == *"escalating: SIGTERM"* ]]
  [[ "$output" == *"escalating: SIGKILL"* ]]
  [ ! -s "$SHIM_STATE/ps.txt" ]
  # No pattern-based killing or `open` anywhere in the ctl (comments excluded).
  # osascript is allowed for one thing only: the converge toast (notify()).
  run grep -E '^[^#]*[[:space:]](pkill|pgrep|killall|open)[[:space:]]' "$SCRIPT"
  [ "$status" -ne 0 ]
  run grep -cE '^[^#]*[[:space:]]osascript[[:space:]]' "$SCRIPT"
  [ "$output" -eq 1 ]
  run grep -E '^[^#]*[[:space:]]osascript[[:space:]]' "$SCRIPT"
  [[ "$output" == *"display notification"* ]]
}

@test "stop is a no-op when nothing is loaded or running" {
  run bash "$SCRIPT" stop
  [ "$status" -eq 0 ]
  log_lacks "launchctl bootout"
}

@test "stop leaves Mouser and a foreign deskflow-core alone (bundle scope only)" {
  add_proc 900 "/Applications/Mouser.app/Contents/MacOS/Mouser"
  add_proc 901 "/opt/other/deskflow-core"
  run bash "$SCRIPT" stop
  [ "$status" -eq 0 ]
  log_lacks "kill "
  grep -q "^900" "$SHIM_STATE/ps.txt"
  grep -q "^901" "$SHIM_STATE/ps.txt"
}

@test "stop ignores root's LoginWindow bridge and deskflow-prio in the bundle (uid + basename scope)" {
  # Root's LoginWindow vhid-bridge and the deskflow-prio LaunchDaemon run from
  # the same bundle; the old path-only match waited on them forever / EPERM'd.
  load_agent $CORE 100; add_proc 100 "$APP/Contents/MacOS/deskflow-core"
  add_proc 910 "$APP/Contents/MacOS/deskflow-vhid-bridge" 0
  add_proc 911 "$APP/Contents/MacOS/deskflow-prio" 0
  add_proc 912 "$APP/Contents/MacOS/deskflow-core" 0
  run bash "$SCRIPT" stop
  [ "$status" -eq 0 ]
  [[ "$output" == *"stopped"* ]]
  log_lacks "kill "
  ! grep -q "^100" "$SHIM_STATE/ps.txt"
  grep -q "^910" "$SHIM_STATE/ps.txt"
  grep -q "^911" "$SHIM_STATE/ps.txt"
  grep -q "^912" "$SHIM_STATE/ps.txt"
}

@test "stop never signals another uid's core even when escalating" {
  load_agent $CORE 100; add_proc 100 "$APP/Contents/MacOS/deskflow-core"
  mkdir -p "$SHIM_STATE/stubborn"; touch "$SHIM_STATE/stubborn/100"
  add_proc 913 "$APP/Contents/MacOS/deskflow-core" 0
  run bash "$SCRIPT" stop
  [ "$status" -eq 0 ]
  log_has "kill -TERM 100"
  log_lacks "kill -TERM 913"
  log_lacks "kill -KILL 913"
  grep -q "^913" "$SHIM_STATE/ps.txt"
}

# --- start --------------------------------------------------------------------

@test "start renders both agents and bootstraps core then GUI; never open/pkill" {
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" start
  [ "$status" -eq 0 ]
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
  # order: core before GUI
  core_line="$(grep -n "bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CORE.plist" "$SHIM_LOG" | cut -d: -f1)"
  gui_line="$(grep -n "bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist" "$SHIM_LOG" | cut -d: -f1)"
  [ "$core_line" -lt "$gui_line" ]
  log_lacks "open "
  log_lacks "kill "
  grep -q "<string>$APP/Contents/MacOS/deskflow-core</string>" "$DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  grep -q "<string>auto</string>" "$DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  grep -q "<string>/dev/null</string>" "$DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  ! grep -q "Library/Logs/Deskflow/deskflow-core.log" "$DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  ! grep -q "__APP__\|__HOME__\|__CTL__" "$DESKFLOW_CTL_AGENT_DIR/$CORE.plist" "$DESKFLOW_CTL_AGENT_DIR/$GUI.plist" "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
  [ -d "$HOME/Library/Logs/Deskflow" ]
  [[ "$output" == *"started: core pid 300"* ]]
  # launchd-owned marker in both process plists; GUI comes back after a crash.
  grep -q "DESKFLOW_LAUNCHD" "$DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  grep -q "DESKFLOW_LAUNCHD" "$DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
  grep -q "SuccessfulExit" "$DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
  # enable precedes every bootstrap (a disabled label makes bootstrap a silent no-op at login).
  for label in $CORE $GUI $CONVERGE; do
    en="$(grep -n "launchctl enable $DOMAIN/$label" "$SHIM_LOG" | head -1 | cut -d: -f1)"
    bs="$(grep -n "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$label.plist" "$SHIM_LOG" | cut -d: -f1)"
    [ -n "$en" ] && [ "$en" -lt "$bs" ]
  done
  # The converge tick is bootstrapped last and points at this checkout's ctl.
  grep -q "<string>$REPO/scripts/deskflow-ctl</string>" "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
  grep -q "<string>--apply</string>" "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
  grep -q "<key>StartInterval</key>" "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
  gui_line="$(grep -n "bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist" "$SHIM_LOG" | cut -d: -f1)"
  conv_line="$(grep -n "bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist" "$SHIM_LOG" | cut -d: -f1)"
  [ "$gui_line" -lt "$conv_line" ]
}

@test "stop writes the quit-intent sentinel and boots out the converge tick; start/restart clear it" {
  load_agent $CONVERGE; load_agent $CORE 100; add_proc 100 "$APP/Contents/MacOS/deskflow-core"
  run bash "$SCRIPT" stop
  [ "$status" -eq 0 ]
  [ -f "$HOME/Library/Application Support/Deskflow/quit-intent" ]
  log_has "launchctl bootout $DOMAIN/$CONVERGE"
  # the sentinel lands before the first bootout so a racing converge sees it
  first_bootout="$(grep -n "launchctl bootout" "$SHIM_LOG" | head -1 | cut -d: -f1)"
  [ "$first_bootout" -ge 1 ]

  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" start
  [ "$status" -eq 0 ]
  [ ! -e "$HOME/Library/Application Support/Deskflow/quit-intent" ]

  bash "$SCRIPT" stop
  [ -f "$HOME/Library/Application Support/Deskflow/quit-intent" ]
  run bash "$SCRIPT" restart
  [ "$status" -eq 0 ]
  [ ! -e "$HOME/Library/Application Support/Deskflow/quit-intent" ]
}

@test "start kickstarts (without -k) an agent that is already loaded instead of bootstrapping twice" {
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" start
  [ "$status" -eq 0 ]
  log_has "launchctl kickstart $DOMAIN/$CORE"
  log_lacks "launchctl kickstart -k"
  log_lacks "bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
}

@test "start fails loudly when launchd never reports a core pid" {
  run bash "$SCRIPT" start
  [ "$status" -eq 1 ]
  [[ "$output" == *"deskflow-core did not start under launchd"* ]]
  log_lacks "bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
}

@test "start refuses when the bundle is missing" {
  rm -rf "$APP"
  run bash "$SCRIPT" start
  [ "$status" -eq 1 ]
  [[ "$output" == *"bundle missing"* ]]
  log_lacks "launchctl"
}

# --- restart ------------------------------------------------------------------

@test "restart uses launchctl kickstart -k for loaded agents and bootstrap for unloaded ones" {
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" restart
  [ "$status" -eq 0 ]
  log_has "launchctl kickstart -k $DOMAIN/$CORE"
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
  log_lacks "bootout"
  log_lacks "kill "
}

# --- status / assert-single ---------------------------------------------------

@test "status inventories agents and processes by executable path" {
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  add_proc 777 "/tmp/build/bin/deskflow-core"
  run bash "$SCRIPT" status
  [ "$status" -eq 0 ]
  [[ "$output" == *"agent: $CORE loaded pid=300"* ]]
  [[ "$output" == *"agent: $GUI not loaded"* ]]
  [[ "$output" == *"300 $(id -u) $APP/Contents/MacOS/deskflow-core yes"* ]]
  [[ "$output" == *"777 $(id -u) /tmp/build/bin/deskflow-core NO"* ]]
}

@test "assert-single passes with exactly one launchd-owned core and one GUI" {
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  load_agent $GUI 301;  add_proc 301 "$APP/Contents/MacOS/Deskflow"
  add_proc 900 "/Applications/Mouser.app/Contents/MacOS/Mouser"
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 0 ]
  [[ "$output" == *"assert-single: OK"* ]]
}

@test "assert-single fails on two cores" {
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  add_proc 302 "$APP/Contents/MacOS/deskflow-core"
  load_agent $GUI 301;  add_proc 301 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 1 ]
  [[ "$output" == *"deskflow-core count=2 (want 1)"* ]]
}

@test "assert-single fails on a non-canonical path, a missing GUI, or a core launchd does not own" {
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  add_proc 777 "/tmp/build/bin/deskflow-core"
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 1 ]
  [[ "$output" == *"non-canonical process pid=777 /tmp/build/bin/deskflow-core"* ]]
  [[ "$output" == *"Deskflow GUI count=0"* ]]

  : >"$SHIM_STATE/ps.txt"; rm -f "$SHIM_STATE/pid/$CORE"
  add_proc 555 "$APP/Contents/MacOS/deskflow-core"
  add_proc 301 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 1 ]
  [[ "$output" == *"has no running pid under launchd"* ]]
}

# --- prio LaunchDaemon --------------------------------------------------------

PRIO=io.github.hughesyadaddy.deskflow-prio

add_prio_binary() { : >"$APP/Contents/MacOS/deskflow-prio"; chmod +x "$APP/Contents/MacOS/deskflow-prio"; }

@test "prio renders the LaunchDaemon template with the bundle path and prints the exact sudo steps when not root" {
  add_prio_binary
  run bash "$SCRIPT" prio
  [ "$status" -eq 0 ]
  staged="$DESKFLOW_CTL_DAEMON_STAGE_DIR/$PRIO.plist"
  [ -f "$staged" ]
  # Template rendering: label, the bundle's own deskflow-prio (not the literal
  # /Applications default), the 30 s interval and the root log path survive.
  grep -q "<string>$PRIO</string>" "$staged"
  grep -q "<string>$APP/Contents/MacOS/deskflow-prio</string>" "$staged"
  ! grep -q "/Applications/Deskflow.app" "$staged"
  ! grep -q "__APP__" "$staged"
  grep -q "<key>StartInterval</key>" "$staged"
  grep -q "<string>/var/log/deskflow-prio.log</string>" "$staged"
  # Staged OUTSIDE ~/Library/LaunchAgents (launchd must not load it as a user agent).
  [ ! -e "$DESKFLOW_CTL_AGENT_DIR/$PRIO.plist" ]
  # Human step, exact commands, nothing executed privileged.
  [[ "$output" == *"needs root"* ]]
  [[ "$output" == *"sudo install -m 644 -o root -g wheel \"$staged\" \"$DESKFLOW_CTL_DAEMON_DIR/$PRIO.plist\""* ]]
  [[ "$output" == *"sudo chown root:wheel $APP/Contents/MacOS/deskflow-prio"* ]]
  [[ "$output" == *"sudo launchctl bootstrap system \"$DESKFLOW_CTL_DAEMON_DIR/$PRIO.plist\""* ]]
  log_has "launchctl print system/$PRIO"
  log_lacks "launchctl bootstrap system"
  [ ! -e "$DESKFLOW_CTL_DAEMON_DIR/$PRIO.plist" ]
}

@test "prio is silent (no sudo step) when the daemon is loaded, the installed plist matches and the binary is root:wheel" {
  add_prio_binary
  make_shim stat <<'EOF'
echo "root:wheel"
EOF
  load_agent $PRIO 77
  mkdir -p "$DESKFLOW_CTL_DAEMON_DIR"
  sed -e "s#/Applications/Deskflow\.app#$APP#g" "$REPO/tools/launchd/$PRIO.plist" >"$DESKFLOW_CTL_DAEMON_DIR/$PRIO.plist"
  run bash "$SCRIPT" prio
  [ "$status" -eq 0 ]
  [[ "$output" == *"up to date"* ]]
  [[ "$output" != *"sudo "* ]]
}

@test "prio reports a stale installed plist even when the daemon is loaded" {
  add_prio_binary
  make_shim stat <<'EOF'
echo "root:wheel"
EOF
  load_agent $PRIO 77
  mkdir -p "$DESKFLOW_CTL_DAEMON_DIR"
  echo "<plist>old</plist>" >"$DESKFLOW_CTL_DAEMON_DIR/$PRIO.plist"
  run bash "$SCRIPT" prio
  [ "$status" -eq 0 ]
  [[ "$output" == *"needs root"* ]]
  [[ "$output" == *"sudo launchctl bootout system/$PRIO"* ]]
}

@test "prio skips cleanly when the bundle has no deskflow-prio" {
  run bash "$SCRIPT" prio
  [ "$status" -eq 0 ]
  [[ "$output" == *"skipping LaunchDaemon"* ]]
  [ ! -e "$DESKFLOW_CTL_DAEMON_STAGE_DIR/$PRIO.plist" ]
}

@test "start also renders the prio daemon and prints the root step" {
  add_prio_binary
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" start
  [ "$status" -eq 0 ]
  [ -f "$DESKFLOW_CTL_DAEMON_STAGE_DIR/$PRIO.plist" ]
  [[ "$output" == *"sudo launchctl bootstrap system"* ]]
  log_has "launchctl print system/$PRIO"
}

# --- converge -----------------------------------------------------------------

healthy() {
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  load_agent $GUI 301;  add_proc 301 "$APP/Contents/MacOS/Deskflow"
  load_agent $CONVERGE
  bash "$SCRIPT" start >/dev/null 2>&1 || true  # renders the plists; agents already loaded
  : >"$SHIM_LOG"
}

@test "converge on a healthy seat does nothing, writes health.json, no toast" {
  healthy
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 0 ]
  [[ "$output" == *"nothing to do"* ]]
  [[ "$output" == *"assert-single: OK"* ]]
  log_lacks "launchctl bootstrap"
  log_lacks "launchctl kickstart"
  log_lacks "osascript"
  [ -f "$STATE/health.json" ]
  python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert d['assert_single']=='OK' and d['actions']==[] and d['counts']['core']==1 and d['mode']=='apply'" "$STATE/health.json"
  run bash "$SCRIPT" converge --apply --quiet
  [ "$status" -eq 0 ]
  [ -z "$output" ]
}

@test "converge without --apply only prints the plan; --apply bootstraps the missing GUI and toasts once" {
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  load_agent $CONVERGE
  bash "$SCRIPT" start >/dev/null 2>&1 || true
  rm -f "$SHIM_STATE/loaded/$GUI"   # a GUI booted out of launchd entirely
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge
  [ "$status" -eq 0 ]
  [[ "$output" == *"plan (plan): bootstrap $GUI"* ]]
  log_lacks "launchctl bootstrap"
  log_lacks "launchctl enable"
  log_lacks "osascript"
  python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert d['mode']=='plan' and d['plan']==['bootstrap $GUI'] and d['actions']==[]" "$STATE/health.json"

  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 0 ]
  [[ "$output" == *"acted: bootstrap $GUI"* ]]
  log_has "launchctl enable $DOMAIN/$GUI"
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
  log_has "osascript -e display notification"
  log_lacks "kill "
  log_lacks "kickstart -k"
  python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert d['actions']==['bootstrap $GUI'] and d['assert_single']=='OK' and d['budget']['used']==1" "$STATE/health.json"
  [ "$(wc -l <"$STATE/converge-actions")" -eq 1 ]
}

@test "converge kickstarts (never -k) a loaded agent with no pid, but not its own StartInterval job" {
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  load_agent $GUI;  # loaded, dead, KeepAlive gave up
  load_agent $CONVERGE  # loaded, idle between ticks: normal
  bash "$SCRIPT" start >/dev/null 2>&1 || true
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge --apply
  log_has "launchctl kickstart $DOMAIN/$GUI"
  log_lacks "launchctl kickstart -k"
  log_lacks "launchctl kickstart $DOMAIN/$CONVERGE"
  log_lacks "launchctl bootout"
  log_lacks "kill "
}

@test "converge --apply exits 1 when assert-single still fails after acting (or nothing to act on)" {
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  add_proc 302 "$APP/Contents/MacOS/deskflow-core"   # a second core: converge never kills it
  load_agent $GUI 301;  add_proc 301 "$APP/Contents/MacOS/Deskflow"
  load_agent $CONVERGE
  bash "$SCRIPT" start >/dev/null 2>&1 || true
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge --apply --quiet
  [ "$status" -eq 1 ]
  [[ "$output" == *"deskflow-core count=2"* ]]
  log_lacks "kill "
  grep -q "^302" "$SHIM_STATE/ps.txt"
  # plan mode reports the same state but never fails
  run bash "$SCRIPT" converge
  [ "$status" -eq 0 ]
}

@test "converge re-renders a stale plist of a LOADED agent even when gated, but reports it as pending, not repaired" {
  healthy
  echo "<plist>stale</plist>" >"$DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  bash "$SCRIPT" stop >/dev/null   # writes quit-intent
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  load_agent $GUI 301;  add_proc 301 "$APP/Contents/MacOS/Deskflow"
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 0 ]
  [[ "$output" == *"render $CORE (takes effect at next deskflow-ctl start/restart)"* ]]
  [[ "$output" != *"acted:"* ]]
  grep -q "<string>$APP/Contents/MacOS/deskflow-core</string>" "$DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  log_lacks "launchctl kickstart"
  log_lacks "launchctl bootout"
  log_lacks "osascript"
  python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert d['needs_rebootstrap'] is True and d['actions']==[] and d['plan'][0]=='render $CORE (takes effect at next deskflow-ctl start/restart)'" "$STATE/health.json"
  # renders are free: the start budget is untouched
  [ ! -e "$STATE/converge-actions" ]
}

@test "converge renders the plist of an UNLOADED agent as a completed repair (it is what bootstrap will load)" {
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 0 ]
  python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert d['needs_rebootstrap'] is False and 'render $CORE' in d['actions'] and 'bootstrap $CORE' in d['actions']" "$STATE/health.json"
}

@test "a failing launchctl spends budget: 5 ticks make at most 3 attempts, health.json records the error, exit 1 every time" {
  load_agent $CONVERGE
  make_shim launchctl <<'EOF'
echo "launchctl $*" >> "$SHIM_LOG"
case "$1" in
  print) label="${2##*/}"; [[ -f "$SHIM_STATE/loaded/$label" ]] || exit 113; exit 0 ;;
  bootstrap) echo "Bootstrap failed: 5: Input/output error" >&2; exit 5 ;;
esac
exit 0
EOF
  for i in 1 2 3 4 5; do
    run bash "$SCRIPT" converge --apply --quiet
    [ "$status" -eq 1 ]
    [[ "$output" == *"FAILED: bootstrap $CORE: Bootstrap failed: 5: Input/output error"* ]] || [[ "$output" == *"budget exhausted"* ]]
    python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert not [a for a in d['actions'] if not a.startswith('render')] and (d['errors'] or d['gate'].startswith('action budget'))" "$STATE/health.json"
  done
  [ "$(grep -c "launchctl bootstrap" "$SHIM_LOG")" -eq 3 ]
  [ "$(wc -l <"$STATE/converge-actions")" -eq 3 ]
  # tick 1 tries core+GUI, tick 2 the third attempt, ticks 3-5 are gated:
  # two toasts naming the failure, never "repaired", gated ticks stay silent
  [ "$(grep -c "osascript" "$SHIM_LOG")" -eq 2 ]
  log_lacks "converge repaired"
  log_has "converge FAILED: bootstrap $CORE"
}

@test "quit-intent from this boot gates every start; a sentinel older than the boot is stale and removed" {
  load_agent $CONVERGE
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  mkdir -p "$STATE"; date +%s >"$STATE/quit-intent"
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 0 ]                       # a quit is intended, not a failure
  [[ "$output" == *"gated: quit-intent"* ]]
  log_lacks "launchctl bootstrap"
  log_lacks "launchctl enable"
  log_lacks "launchctl kickstart"
  [ -f "$STATE/quit-intent" ]
  python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert d['gate'].startswith('quit-intent') and 'bootstrap $CORE' in d['plan'] and 'bootstrap $GUI' in d['plan'] and not [a for a in d['actions'] if not a.startswith('render')]" "$STATE/health.json"

  # Pre-boot sentinel: a reboot ends a quit.
  touch -t 200001010000 "$STATE/quit-intent"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 0 ]
  [ ! -e "$STATE/quit-intent" ]
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
}

@test "a deploy lock younger than 30 min makes converge a no-op exit 0; an old one is ignored" {
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  mkdir -p "$HOME/Library/Deskflow"; echo $$ >"$HOME/Library/Deskflow/deploy.lock"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 0 ]
  [[ "$output" == *"deploy lock"* ]]
  log_lacks "launchctl bootstrap"
  log_lacks "launchctl enable"
  python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert d['gate'].startswith('deploy lock')" "$STATE/health.json"

  touch -t 200001010000 "$HOME/Library/Deskflow/deploy.lock"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 0 ]
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
}

@test "converge stops acting after 3 start actions in a rolling hour and then exits 1 while still broken" {
  load_agent $CONVERGE
  mkdir -p "$STATE"
  now="$(date +%s)"
  printf '%s\n%s\n' "$((now - 3000))" "$((now - 100))" >"$STATE/converge-actions"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  # Nothing rendered yet: converge renders (free) and bootstraps the core as
  # the third start of the hour; the GUI must wait.
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 1 ]
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  log_lacks "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
  [ "$(wc -l <"$STATE/converge-actions")" -eq 3 ]
  python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert 'bootstrap $CORE' in d['actions'] and 'bootstrap $GUI' in d['plan'] and 'bootstrap $GUI' not in d['actions'] and d['gate'].startswith('action budget')" "$STATE/health.json"
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 1 ]
  [[ "$output" == *"budget exhausted"* ]]
  log_lacks "launchctl bootstrap"
  log_lacks "osascript"
  # entries older than the window fall out of the count
  printf '%s\n%s\n%s\n' "$((now - 4000))" "$((now - 3900))" "$((now - 3800))" >"$STATE/converge-actions"
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge --apply
  log_has "launchctl kickstart $DOMAIN/$CORE"
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
}

@test "converge rejects unknown options and a missing bundle" {
  run bash "$SCRIPT" converge --force
  [ "$status" -eq 1 ]
  rm -rf "$APP"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 1 ]
  [[ "$output" == *"bundle missing"* ]]
  log_lacks "launchctl"
}

@test "prio prints the machine lock dir step (1777) alongside the daemon install" {
  add_prio_binary
  run bash "$SCRIPT" prio
  [ "$status" -eq 0 ]
  [[ "$output" == *"sudo install -d -m 1777 /private/var/db/deskflow"* ]]
}

@test "unknown verb exits 1 with usage" {
  run bash "$SCRIPT" bogus
  [ "$status" -eq 1 ]
  [[ "$output" == *"unknown verb"* ]]
}
