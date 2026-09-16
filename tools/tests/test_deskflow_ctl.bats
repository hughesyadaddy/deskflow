#!/usr/bin/env bats
# scripts/deskflow-ctl — launchd is the only owner of Deskflow on macOS.
#
# launchctl / ps / kill / sleep are PATH shims. Fake launchd state lives in
# $SHIM_STATE: loaded/<label> marks a bootstrapped agent, pid/<label> is the
# pid launchd reports, and ps.txt is the process table (pid uid path; uid
# defaults to the caller's, since stop only owns this user's processes).

SCRIPT="$BATS_TEST_DIRNAME/../../scripts/deskflow-ctl"
REPO="$BATS_TEST_DIRNAME/../.."

setup() {
  TMP="$(mktemp -d "${BATS_TEST_TMPDIR:-${TMPDIR:-/tmp}}/deskflow-ctl.XXXXXX")"
  SHIMS="$TMP/bin"
  export SHIM_LOG="$TMP/calls.log"
  export SHIM_STATE="$TMP/state"
  export APP="$TMP/Applications/Deskflow.app"
  export HOME="$TMP/home"
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
  run grep -E '^[^#]*[[:space:]](pkill|pgrep|killall|osascript|open)[[:space:]]' "$SCRIPT"
  [ "$status" -ne 0 ]
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
  grep -q "<string>$HOME/Library/Logs/Deskflow/deskflow-core.log</string>" "$DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  ! grep -q "__APP__\|__HOME__" "$DESKFLOW_CTL_AGENT_DIR/$CORE.plist" "$DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
  [ -d "$HOME/Library/Logs/Deskflow" ]
  [[ "$output" == *"started: core pid 300"* ]]
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

@test "unknown verb exits 1 with usage" {
  run bash "$SCRIPT" bogus
  [ "$status" -eq 1 ]
  [[ "$output" == *"unknown verb"* ]]
}
