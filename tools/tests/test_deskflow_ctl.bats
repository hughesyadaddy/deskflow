#!/usr/bin/env bats
# scripts/deskflow-ctl — launchd is the only owner of Deskflow on macOS.
#
# launchctl / ps / kill / sleep / sfltool are PATH shims. Fake launchd state
# lives in $SHIM_STATE: loaded/<label> marks a bootstrapped agent, pid/<label>
# is the pid launchd reports, ps.txt is the process table (pid uid path; uid
# defaults to the caller's, since stop only owns this user's processes),
# ppid/<pid> a parent pid (default 1 = launchd), domain.txt the `launchctl
# print gui/$UID` listing, args/<label> + exit/<label> the `arguments = {`
# block and `last exit code` a loaded label prints, and btm.txt the `sfltool
# dumpbtm` output (default: the fleet-agents-only fixture; btm.rc + btm.err
# fake a privilege refusal).

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
  SAFE="$HOME/Library/Deskflow/bin"   # the launchd-safe copy lands in the sandbox, never the real ~/Library
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
    if [[ "$1" != */*/* ]]; then
      # domain listing (gui/$UID): runningboard instances etc.
      cat "$SHIM_STATE/domain.txt" 2>/dev/null
      exit 0
    fi
    label="${1##*/}"
    [[ -f "$SHIM_STATE/loaded/$label" ]] || exit 113
    echo "$label = {"
    [[ -f "$SHIM_STATE/pid/$label" ]] && echo "	pid = $(cat "$SHIM_STATE/pid/$label")"
    if [[ -f "$SHIM_STATE/args/$label" ]]; then
      echo "	arguments = {"; sed 's/^/		/' "$SHIM_STATE/args/$label"; echo "	}"
    fi
    # env/<label> (K=V lines) is the `environment = {` block launchd prints for the LOADED definition
    if [[ -f "$SHIM_STATE/env/$label" ]]; then
      echo "	environment = {"; sed -e 's/=/ => /' -e 's/^/		/' "$SHIM_STATE/env/$label"; echo "	}"
    fi
    [[ -f "$SHIM_STATE/exit/$label" ]] && echo "	last exit code = $(cat "$SHIM_STATE/exit/$label")"
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
if [[ "${1:-}" == "-o" && "${2:-}" == "ppid=" ]]; then
  cat "$SHIM_STATE/ppid/$4" 2>/dev/null || echo 1
  exit 0
fi
cat "$SHIM_STATE/ps.txt"
EOF

  # Background Task Management dump: the audit never PASSes without one.
  mkdir -p "$SHIM_STATE/ppid"
  cp "$BATS_TEST_DIRNAME/fixtures/btm-dump-fleet-agents-only.txt" "$SHIM_STATE/btm.txt"
  make_shim sfltool <<'EOF'
echo "sfltool $*" >> "$SHIM_LOG"
[[ "${1:-}" == "dumpbtm" ]] || { echo "unexpected sfltool verb $1" >&2; exit 64; }
# SHIM_AS_ROOT=1 (set by the sudo shim below) lifts the faked privilege refusal.
if [[ -f "$SHIM_STATE/btm.rc" && -z "${SHIM_AS_ROOT:-}" ]]; then
  cat "$SHIM_STATE/btm.err" 2>/dev/null >&2
  exit "$(cat "$SHIM_STATE/btm.rc")"
fi
cat "$SHIM_STATE/btm.txt"
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

  export PATH="$SHIMS:$BATS_TEST_DIRNAME/fakebin:$PATH"  # fakebin: sudo must never be real
  export DESKFLOW_INSTALL_APP="$APP"
  export DESKFLOW_CTL_AGENT_DIR="$TMP/LaunchAgents"
  export DESKFLOW_CTL_DAEMON_DIR="$TMP/LaunchDaemons"
  export DESKFLOW_CTL_DAEMON_STAGE_DIR="$TMP/stage"
  export DESKFLOW_CTL_STOP_TIMEOUT=1
  export DESKFLOW_CTL_ESCALATE_TIMEOUT=1
  export DESKFLOW_CTL_START_TIMEOUT=1
  # Retired root-owned files: never probe the real /usr/local or /Library.
  export DESKFLOW_CTL_RETIRED_PRIO_APPLY="$TMP/usr-local-bin/deskflow-prio-apply.sh"
  export DESKFLOW_CTL_RETIRED_SYNERGY_AGENT="$TMP/LibraryLaunchAgents/com.symless.synergy-agent.plist"
  unset SHIM_SUDO_EXPECT_PW SHIM_AS_ROOT
}

teardown() {
  rm -rf "$TMP"
}

make_shim() {
  { echo '#!/usr/bin/env bash'; cat; } >"$SHIMS/$1"
  chmod +x "$SHIMS/$1"
}

add_proc() { printf '%s\t%s\t%s\n' "$1" "${3:-$(id -u)}" "$2" >>"$SHIM_STATE/ps.txt"; }
del_proc() { { grep -v "^$1	" "$SHIM_STATE/ps.txt" || true; } >"$SHIM_STATE/ps.tmp"; mv "$SHIM_STATE/ps.tmp" "$SHIM_STATE/ps.txt"; }
set_ppid() { echo "$2" >"$SHIM_STATE/ppid/$1"; }
use_btm() { cp "$BATS_TEST_DIRNAME/fixtures/btm-dump-$1.txt" "$SHIM_STATE/btm.txt"; }
healthy_seat() {
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  load_agent $GUI 301;  add_proc 301 "$APP/Contents/MacOS/Deskflow"
}
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
  # osascript is allowed for two things only: the converge toast (notify())
  # and the legacy login-item delete behind `login-items audit --fix`.
  run grep -E '^[^#]*[[:space:]](pkill|pgrep|killall|open)[[:space:]]' "$SCRIPT"
  [ "$status" -ne 0 ]
  run grep -cE '^[^#]*[[:space:]]osascript[[:space:]]' "$SCRIPT"
  [ "$output" -eq 2 ]
  run grep -E '^[^#]*[[:space:]]osascript[[:space:]]' "$SCRIPT"
  [[ "$output" == *"display notification"* ]]
  [[ "$output" == *"delete login item"* ]]
}

@test "stop is a no-op when nothing is loaded or running" {
  run bash "$SCRIPT" stop
  [ "$status" -eq 0 ]
  log_lacks "launchctl bootout"
}

@test "stop terminates every deskflow-core/Deskflow of this user from ANY path (each one blocks start); Mouser is never touched" {
  # A dev build or a Deskflow.app.bak core makes `start` refuse (a duplicate)
  # and the remedy that refusal names is `stop`, so `stop` must clear it.
  add_proc 900 "/Applications/Mouser.app/Contents/MacOS/Mouser"
  add_proc 901 "/opt/other/deskflow-core"
  run bash "$SCRIPT" stop
  [ "$status" -eq 0 ]
  log_has "kill -TERM 901"
  log_lacks "kill -TERM 900"
  [[ "$output" == *"escalating: SIGTERM to 901 (/opt/other/deskflow-core)"* ]]
  [[ "$output" == *"stopped (escalated)"* ]]
  grep -q "^900" "$SHIM_STATE/ps.txt"
  ! grep -q "^901" "$SHIM_STATE/ps.txt"
}

@test "stop ignores root's LoginWindow bridge and deskflow-prio in the bundle (uid + basename scope)" {
  # Root's LoginWindow vhid-bridge and the deskflow-prio LaunchDaemon run from
  # the same bundle; the old path-only match waited on them forever / EPERM'd.
  load_agent $CORE 100; add_proc 100 "$APP/Contents/MacOS/deskflow-core"
  add_proc 910 "$APP/Contents/MacOS/deskflow-vhid-bridge" 0
  add_proc 911 "$APP/Contents/MacOS/deskflow-prio" 0
  run bash "$SCRIPT" stop
  [ "$status" -eq 0 ]
  [[ "$output" == *"stopped"* ]]
  log_lacks "kill "
  ! grep -q "^100" "$SHIM_STATE/ps.txt"
  grep -q "^910" "$SHIM_STATE/ps.txt"
  grep -q "^911" "$SHIM_STATE/ps.txt"
}

@test "stop never signals another uid's deskflow-core: it prints the exact sudo kill line and exits 1, never 'stopped'" {
  # No root deskflow-core exists by design (the login-window agent is the
  # vhid-bridge): one is somebody's sudo. `start` refuses beside it, so a
  # "stopped" here would be a lie; the remedy printed is the one that works.
  load_agent $CORE 100; add_proc 100 "$APP/Contents/MacOS/deskflow-core"
  mkdir -p "$SHIM_STATE/stubborn"; touch "$SHIM_STATE/stubborn/100"
  add_proc 913 "$APP/Contents/MacOS/deskflow-core" 0
  run bash "$SCRIPT" stop
  [ "$status" -eq 1 ]
  log_has "kill -TERM 100"
  log_lacks "kill -TERM 913"
  log_lacks "kill -KILL 913"
  grep -q "^913" "$SHIM_STATE/ps.txt"
  [[ "$output" == *"NOT stopped"* ]]
  [[ "$output" == *"sudo kill 913   # deskflow-core pid 913 uid 0: $APP/Contents/MacOS/deskflow-core"* ]]
  [[ "$output" != *"== deskflow-ctl: stopped"* ]]
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
  # the header comment may mention the old path; only a <string> value would re-enable the second sink
  [ "$(grep -c '<string>.*Library/Logs/Deskflow/deskflow-core.log</string>' "$DESKFLOW_CTL_AGENT_DIR/$CORE.plist")" = 0 ]
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
  # The converge tick is bootstrapped last and points at the launchd-safe
  # copy, never this checkout (TCC: launchd gets EPERM under ~/Desktop).
  grep -q "<string>$HOME/Library/Deskflow/bin/deskflow-ctl</string>" "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
  ! grep -q -F "$REPO" "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
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

@test "assert-single fails when the GUI pid is not launchd's (Login Item copy holds the lock)" {
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  load_agent $GUI 301;  add_proc 2141 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 1 ]
  [[ "$output" == *"Deskflow GUI pid 2141 is not launchd's (301)"* ]]

  : >"$SHIM_STATE/ps.txt"; rm -f "$SHIM_STATE/pid/$GUI"
  add_proc 300 "$APP/Contents/MacOS/deskflow-core"; add_proc 2141 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 1 ]
  [[ "$output" == *"$GUI has no running pid under launchd"* ]]
}

@test "assert-single fails when core ppid is not 1 (GUI-spawned core)" {
  healthy_seat
  set_ppid 300 301
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 1 ]
  [[ "$output" == *"deskflow-core pid 300 ppid=301 (want 1: launchd)"* ]]
  log_has "ps -o ppid= -p 300"
  set_ppid 300 1
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 0 ]
}

@test "assert-single fails on a runningboard application.* instance in the user domain" {
  healthy_seat
  printf 'gui/501 = {\n\tservices = {\n\t\t0\t-\t%s\n\t\t2141\t-\tapplication.%s.1234.5678\n\t\t0\t-\tcom.apple.foo\n\t}\n}\n' "$GUI" "$GUI" >"$SHIM_STATE/domain.txt"
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 1 ]
  [[ "$output" == *"runningboard instance application.$GUI.1234.5678"* ]]
  log_has "launchctl print $DOMAIN"
  # the agent's own label in the listing is not an instance
  printf 'gui/501 = {\n\tservices = {\n\t\t301\t-\t%s\n\t}\n}\n' "$GUI" >"$SHIM_STATE/domain.txt"
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 0 ]
}

@test "assert-single fails on retired files and passes once retire removed the user-owned one" {
  healthy_seat
  mkdir -p "$HOME/Library/Logs/Deskflow"; : >"$HOME/Library/Logs/Deskflow/deskflow-keepalive.log"
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 1 ]
  [[ "$output" == *"retired file present: $HOME/Library/Logs/Deskflow/deskflow-keepalive.log"* ]]
  run bash "$SCRIPT" retire
  [ "$status" -eq 0 ]
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 0 ]
}

@test "assert-single runs the login-items audit and fails on an enabled BTM app record" {
  healthy_seat
  use_btm two-app-records
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 1 ]
  [[ "$output" == *"login-items audit FAIL: enabled BTM app record"* ]]
  log_has "sfltool dumpbtm"
}

# --- login-items -------------------------------------------------------------

@test "login-items audit parses the dumpbtm fixture and fails on an enabled app record with the manual step" {
  use_btm two-app-records
  run bash "$SCRIPT" login-items audit
  [ "$status" -eq 1 ]
  [[ "$output" == *"login-items: FAIL"* ]]
  [[ "$output" == *"2.io.github.hughesyadaddy.deskflow (file:///Applications/Deskflow.app/)"* ]]
  [[ "$output" == *"2.org.deskflow.deskflow"* ]]
  [[ "$output" == *'System Settings -> General -> Login Items & Extensions -> "Open at Login" -> remove "Deskflow"'* ]]
  [[ "$output" == *"more than one enabled BTM app/login-item record for /Applications/Deskflow.app"* ]]
  [[ "$output" == *"sfltool resetbtm"* ]]
  # never the osascript delete by default
  log_lacks "osascript"
  # the pure parser yields exactly identifier/type/disposition/url per record
  sed -n '/^parse_btm_dump()/,/^}/p' "$SCRIPT" >"$TMP/parse_btm_dump.sh"
  run bash -c "source '$TMP/parse_btm_dump.sh'; parse_btm_dump <'$BATS_TEST_DIRNAME/fixtures/btm-dump-two-app-records.txt'"
  [ "$status" -eq 0 ]
  [ "$(echo "$output" | wc -l | tr -d ' ')" = 8 ]
  [[ "$output" == *$'2.io.github.hughesyadaddy.deskflow\tapp\tenabled, allowed, not notified\tfile:///Applications/Deskflow.app/'* ]]
  [[ "$output" == *$'16.com.carriez.RustDesk_service\tlegacy daemon\tenabled, allowed, notified\tfile:///Library/LaunchDaemons/com.carriez.RustDesk_service.plist'* ]]
  # a sub-list "#1: 16.com..." line never becomes a record
  [ "$(echo "$output" | awk -F'\t' 'NF != 4' | wc -l | tr -d ' ')" = 0 ]
}

@test "login-items audit allows the fleet agent records (core, gui, converge, prio daemon, vhid-bridge)" {
  use_btm fleet-agents-only
  run bash "$SCRIPT" login-items audit
  [ "$status" -eq 0 ]
  [[ "$output" == *"login-items: OK"* ]]
  # a foreign enabled login item (Synergy) is a second launcher
  use_btm synergy-login-item
  run bash "$SCRIPT" login-items audit
  [ "$status" -eq 1 ]
  [[ "$output" == *'remove "Synergy"'* ]]
  # a disabled app record is not a launcher, but two records still fail
  awk '/^ #5:/{skip=1} /^ #6:/{skip=0} !skip' "$BATS_TEST_DIRNAME/fixtures/btm-dump-two-app-records.txt" |
    sed 's/Disposition: \[enabled, allowed, not notified\] (0x3)/Disposition: [disabled, allowed, not notified] (0x2)/' >"$SHIM_STATE/btm.txt"
  run bash "$SCRIPT" login-items audit
  [ "$status" -eq 0 ]
}

@test "login-items audit passes the adversarial dump: look-alike names, a sibling's agent, a DISABLED Deskflow login item" {
  use_btm adversarial
  run bash "$SCRIPT" login-items audit
  [ "$status" -eq 0 ]
  [[ "$output" == *"login-items: OK"* ]]
  # "Desk Flow Notes" / "Barrier Breaker" are not launchers of ours; the
  # disabled 4.io.github.hughesyadaddy.deskflow record is not a launcher at all.
  [[ "$output" != *"Desk Flow"* ]]
  [[ "$output" != *"Barrier Breaker"* ]]
  # flip the Deskflow login item to enabled: the only thing that may fail it
  sed 's/Disposition: \[disabled, allowed, not notified\] (0x2)/Disposition: [enabled, allowed, not notified] (0x3)/' \
    "$BATS_TEST_DIRNAME/fixtures/btm-dump-adversarial.txt" >"$SHIM_STATE/btm.txt"
  run bash "$SCRIPT" login-items audit
  [ "$status" -eq 1 ]
  [[ "$output" == *"4.io.github.hughesyadaddy.deskflow"* ]]
  [[ "$output" != *"Desk Flow"* ]]
  # a real barrier bundle id (component match) is a launcher
  sed -e 's/2.com.game.barrierbreaker/2.com.github.debauchee.barrier/' -e 's#Barrier%20Breaker.app#Barrier.app#' \
    "$BATS_TEST_DIRNAME/fixtures/btm-dump-adversarial.txt" >"$SHIM_STATE/btm.txt"
  run bash "$SCRIPT" login-items audit
  [ "$status" -eq 1 ]
  [[ "$output" == *"2.com.github.debauchee.barrier"* ]]
}

@test "login-items audit URL-decodes the bundle name in the System Events step" {
  sed 's#file:///Applications/Deskflow.app/#file:///Applications/Deskflow%20Fleet.app/#' \
    "$BATS_TEST_DIRNAME/fixtures/btm-dump-two-app-records.txt" >"$SHIM_STATE/btm.txt"
  run bash "$SCRIPT" login-items audit
  [ "$status" -eq 1 ]
  [[ "$output" == *'delete login item "Deskflow Fleet"'* ]]
  [[ "$output" != *'Deskflow%20Fleet'* ]]
  [[ "$output" == *"more than one enabled BTM app/login-item record for /Applications/Deskflow Fleet.app"* ]]
}

@test "login-items audit reports SKIP (exit 3, never PASS) with the sudo hint when dumpbtm needs privileges" {
  echo 1 >"$SHIM_STATE/btm.rc"; echo "Error: dumpbtm requires root privileges" >"$SHIM_STATE/btm.err"
  run bash "$SCRIPT" login-items audit
  [ "$status" -eq 3 ]
  [[ "$output" == *"login-items: SKIP"* ]]
  [[ "$output" == *"sudo sfltool dumpbtm | $REPO/scripts/deskflow-ctl login-items audit --stdin"* ]]
  # assert-single never passes on a SKIP
  healthy_seat
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 1 ]
  [[ "$output" == *"login-items audit SKIPPED (never a pass)"* ]]
  # --stdin is the way through
  run bash -c "bash '$SCRIPT' login-items audit --stdin <'$BATS_TEST_DIRNAME/fixtures/btm-dump-fleet-agents-only.txt'"
  [ "$status" -eq 0 ]
}

@test "login-items audit --fix tries the legacy System Events delete once, then re-audits; print-steps lists only the steps" {
  use_btm two-app-records
  run bash "$SCRIPT" login-items audit --fix
  [ "$status" -eq 1 ]
  log_has 'osascript -e tell application "System Events" to delete login item "Deskflow"'
  [ "$(grep -c '^sfltool dumpbtm' "$SHIM_LOG")" = 2 ]
  run bash "$SCRIPT" login-items print-steps
  [ "$status" -eq 0 ]
  [[ "$output" == *'remove "Deskflow"'* ]]
  [[ "$output" != *"FAIL:"* ]]
}

# --- retire ------------------------------------------------------------------

@test "retire removes the keepalive log and prints (never runs) the root steps; exit 2 while they remain" {
  mkdir -p "$HOME/Library/Logs/Deskflow" "$(dirname "$DESKFLOW_CTL_RETIRED_PRIO_APPLY")" "$(dirname "$DESKFLOW_CTL_RETIRED_SYNERGY_AGENT")"
  : >"$HOME/Library/Logs/Deskflow/deskflow-keepalive.log"
  : >"$DESKFLOW_CTL_RETIRED_PRIO_APPLY"
  ln -s /nonexistent "$DESKFLOW_CTL_RETIRED_SYNERGY_AGENT"
  run bash "$SCRIPT" retire
  [ "$status" -eq 2 ]
  [ ! -e "$HOME/Library/Logs/Deskflow/deskflow-keepalive.log" ]
  [[ "$output" == *"removed $HOME/Library/Logs/Deskflow/deskflow-keepalive.log"* ]]
  [[ "$output" == *"sudo rm -f \"$DESKFLOW_CTL_RETIRED_PRIO_APPLY\""* ]]
  [[ "$output" == *"sudo rm -f \"$DESKFLOW_CTL_RETIRED_SYNERGY_AGENT\""* ]]
  [ -e "$DESKFLOW_CTL_RETIRED_PRIO_APPLY" ]
  [ -L "$DESKFLOW_CTL_RETIRED_SYNERGY_AGENT" ]
  log_lacks "sudo"
  rm -f "$DESKFLOW_CTL_RETIRED_PRIO_APPLY" "$DESKFLOW_CTL_RETIRED_SYNERGY_AGENT"
  run bash "$SCRIPT" retire
  [ "$status" -eq 0 ]
  [[ "$output" == *"nothing left to retire"* ]]
}

# --- stale loaded agent ------------------------------------------------------

@test "start boots out a loaded agent whose installed plist is stale before bootstrapping the fresh render" {
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  mkdir -p "$DESKFLOW_CTL_AGENT_DIR"
  echo "<plist>stale: no DESKFLOW_LAUNCHD</plist>" >"$DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  run bash "$SCRIPT" start
  [ "$status" -eq 0 ]
  [[ "$output" == *"$CORE: installed plist was stale while loaded; bootout then bootstrap"* ]]
  log_has "launchctl bootout $DOMAIN/$CORE"
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  out_line="$(grep -n "bootout $DOMAIN/$CORE" "$SHIM_LOG" | cut -d: -f1)"
  bs_line="$(grep -n "bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CORE.plist" "$SHIM_LOG" | cut -d: -f1)"
  [ "$out_line" -lt "$bs_line" ]
  log_lacks "launchctl kickstart $DOMAIN/$CORE"
  grep -q "DESKFLOW_LAUNCHD" "$DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  # an up-to-date loaded agent is still only kickstarted (no bootout)
  : >"$SHIM_LOG"
  run bash "$SCRIPT" start
  [ "$status" -eq 0 ]
  log_lacks "launchctl bootout $DOMAIN/$CORE"
  log_has "launchctl kickstart $DOMAIN/$CORE"
}

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

@test "converge --apply exits 1 with a toast while a BTM app record is enabled; plan mode only reports it" {
  healthy_seat; load_agent $CONVERGE
  use_btm two-app-records
  run bash "$SCRIPT" converge --apply --quiet
  [ "$status" -eq 1 ]
  [[ "$output" == *"converge: FAILED: login-items: enabled BTM app record"* ]]
  [[ "$output" == *"System Settings -> General -> Login Items"* ]]
  log_has "osascript -e display notification"
  log_lacks "delete login item"
  grep -q '"errors": \["login-items: ' "$STATE/health.json"
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge
  [ "$status" -eq 0 ]
  [[ "$output" == *"converge: plan (plan): login-items: enabled BTM app record"* ]]
  log_lacks "osascript -e display notification"
  use_btm fleet-agents-only
  run bash "$SCRIPT" converge --apply --quiet
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

# --- launchd-safe copy, TCC self-check, duplicate guard (K10, 2026-09-25) -----
#
# macOS TCC gives a launchd job EPERM (no prompt, exit 126) on anything under
# ~/Desktop, ~/Documents or ~/Downloads, so the converge tick must run the
# launchd-safe copy under ~/Library/Deskflow/bin and never a checkout path.
# Under bats $HOME is the sandbox, so the safe copy lands in $TMP.

@test "start installs the launchd-safe copy and the converge plist points at it, never at the checkout" {
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" start
  [ "$status" -eq 0 ]
  [[ "$output" == *"safe-copy: installed $SAFE/deskflow-ctl"* ]]
  [ -x "$SAFE/deskflow-ctl" ]
  cmp -s "$SCRIPT" "$SAFE/deskflow-ctl"
  [ "$(stat -f %Lp "$SAFE/deskflow-ctl")" = 755 ]
  for t in "$REPO"/tools/launchd/*.plist; do
    cmp -s "$t" "$SAFE/launchd/$(basename "$t")"
    [ "$(stat -f %Lp "$SAFE/launchd/$(basename "$t")")" = 644 ]
  done
  [ -x "$SAFE/fleet-soak" ]
  grep -q "<string>$SAFE/deskflow-ctl</string>" "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
  ! grep -q -F "$REPO" "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
  # start ends the tick log with a marker (so an old EPERM block is never its last line)
  tail -n 1 "$HOME/Library/Logs/Deskflow/converge.log" | grep -q "start: $CONVERGE bootstrapped; program $SAFE/deskflow-ctl"
  # diff-and-replace: a second start touches nothing
  : >"$SHIM_LOG"
  run bash "$SCRIPT" start
  [ "$status" -eq 0 ]
  [[ "$output" == *"safe-copy: $SAFE up to date"* ]]
  [[ "$output" != *"safe-copy: installed"* ]]
  # a template that left the checkout leaves the safe copy: simulate with a stray file
  : >"$SAFE/launchd/com.example.gone.plist"
  run bash "$SCRIPT" safe-copy
  [ "$status" -eq 0 ]
  [ ! -e "$SAFE/launchd/com.example.gone.plist" ]
  [[ "$output" == *"removed $SAFE/launchd/com.example.gone.plist"* ]]
}

@test "no rendered plist references the checkout path (core, GUI, converge, prio, soak); a checkout-only template is refused" {
  add_prio_binary
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" start
  [ "$status" -eq 0 ]
  [ -f "$DESKFLOW_CTL_AGENT_DIR/$CORE.plist" ] && [ -f "$DESKFLOW_CTL_AGENT_DIR/$GUI.plist" ] && \
    [ -f "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist" ] && [ -f "$DESKFLOW_CTL_DAEMON_STAGE_DIR/$PRIO.plist" ]
  run grep -l -F "$REPO" "$DESKFLOW_CTL_AGENT_DIR"/*.plist "$DESKFLOW_CTL_DAEMON_STAGE_DIR"/*.plist
  [ "$status" -ne 0 ]
  run grep -l -E '__(APP|HOME|CTL|CTL_DIR|SOAK_DIR|REPO)__' "$DESKFLOW_CTL_AGENT_DIR"/*.plist "$DESKFLOW_CTL_DAEMON_STAGE_DIR"/*.plist
  [ "$status" -ne 0 ]
  # the soak sampler renders onto the safe copy and a soak dir outside any checkout
  run bash "$SCRIPT" render-plist com.fleet.soak
  [ "$status" -eq 0 ]
  [[ "$output" != *"$REPO"* ]]
  [[ "$output" != *"__REPO__"* ]] && [[ "$output" != *"__CTL_DIR__"* ]] && [[ "$output" != *"__SOAK_DIR__"* ]]
  [[ "$output" == *"<string>$SAFE/fleet-soak</string>"* ]]
  [[ "$output" == *"<string>$HOME/Library/Deskflow/soak</string>"* ]]
  [[ "$output" == *"<string>$HOME/Library/Logs/fleet-soak.log</string>"* ]]
  # com.fleet.selftest rebuilds from the checkout under ~/Desktop: launchd could never run it
  run bash "$SCRIPT" render-plist com.fleet.selftest
  [ "$status" -eq 1 ]
  [[ "$output" == *"rendered com.fleet.selftest.plist points launchd into ~/Desktop, ~/Documents, ~/Downloads, iCloud Drive (~/Library/Mobile Documents) or /Volumes"* ]]
  # nothing was installed or loaded by render-plist
  [ ! -e "$DESKFLOW_CTL_AGENT_DIR/com.fleet.soak.plist" ]
  log_lacks "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/com.fleet"
  # and the safe dir itself may never sit under a TCC-protected folder
  DESKFLOW_CTL_SAFE_DIR="$HOME/Desktop/bin" run bash "$SCRIPT" safe-copy
  [ "$status" -eq 1 ]
  [[ "$output" == *"is under ~/Desktop, ~/Documents, ~/Downloads, iCloud Drive (~/Library/Mobile Documents) or /Volumes; launchd could never execute it"* ]]
}

@test "converge refuses to kick or bootstrap the core when one already runs that launchd does not own (GUI-spawned / orphan)" {
  healthy
  # launchd lost its core (KeepAlive gave up: loaded, no pid) while a core the
  # GUI spawned (ppid = GUI) lives on. The old tick kickstarted a second one here.
  rm -f "$SHIM_STATE/pid/$CORE"
  del_proc 300
  add_proc 555 "$APP/Contents/MacOS/deskflow-core"; set_ppid 555 301
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 1 ]
  [[ "$output" == *"converge: FAILED: refused kickstart $CORE: deskflow-core pid 555 already running outside launchd"* ]]
  [[ "$output" == *"assert-single: FAIL"* ]]
  [[ "$output" == *"ppid=301 (want 1: launchd)"* ]]
  log_lacks "launchctl kickstart"
  log_lacks "launchctl bootstrap"
  log_lacks "kill "
  grep -q "^555" "$SHIM_STATE/ps.txt"      # never killed either
  python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert d['errors'][0].startswith('refused kickstart $CORE') and d['actions']==[] and d['budget']['used']==0 and d['plan'][0].startswith('kickstart $CORE REFUSED')" "$STATE/health.json"
  [ ! -e "$STATE/converge-actions" ]        # a refusal spends no budget
  # plan mode reports the refusal and never fails
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge
  [ "$status" -eq 0 ]
  [[ "$output" == *"kickstart $CORE REFUSED: deskflow-core pid 555 already running outside launchd"* ]]
  log_lacks "launchctl kickstart"
  # not loaded at all: bootstrap is refused the same way
  rm -f "$SHIM_STATE/loaded/$CORE"
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge --apply --quiet
  [ "$status" -eq 1 ]
  [[ "$output" == *"refused bootstrap $CORE: deskflow-core pid 555 already running outside launchd"* ]]
  log_lacks "launchctl bootstrap"
  # once the stray is gone (deskflow-ctl stop escalates by pid) converge repairs as before
  del_proc 555
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 0 ]
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
}

@test "start and restart refuse to start launchd's core beside a stray one; the GUI is guarded the same way" {
  add_proc 555 "$APP/Contents/MacOS/deskflow-core"; set_ppid 555 301
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" start
  [ "$status" -eq 1 ]
  [[ "$output" == *"bootstrap $CORE refused: deskflow-core pid 555 already running outside launchd"* ]]
  log_lacks "launchctl bootstrap"
  log_lacks "launchctl kickstart"
  load_agent $CORE                            # loaded, no pid, stray alive
  : >"$SHIM_LOG"
  run bash "$SCRIPT" restart
  [ "$status" -eq 1 ]
  [[ "$output" == *"kickstart -k $CORE refused: deskflow-core pid 555"* ]]
  log_lacks "launchctl kickstart"
  # a Login Item / Finder GUI copy blocks the GUI agent the same way
  del_proc 555
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  add_proc 777 "$APP/Contents/MacOS/Deskflow"
  : >"$SHIM_LOG"
  run bash "$SCRIPT" start
  [ "$status" -eq 1 ]
  [[ "$output" == *"bootstrap $GUI refused: Deskflow pid 777 already running outside launchd"* ]]
  log_has "launchctl kickstart $DOMAIN/$CORE"   # launchd's own core is fine
  log_lacks "bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
}

@test "start waits for the old core to exit after bootout and refuses to bootstrap beside a stubborn one" {
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  mkdir -p "$DESKFLOW_CTL_AGENT_DIR"
  echo "<plist>stale</plist>" >"$DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  mkdir -p "$SHIM_STATE/stubborn"; touch "$SHIM_STATE/stubborn/300"   # ignores bootout
  run bash "$SCRIPT" start
  [ "$status" -eq 1 ]
  log_has "launchctl bootout $DOMAIN/$CORE"
  log_lacks "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  [[ "$output" == *"$CORE: pid 300 still running ${DESKFLOW_CTL_STOP_TIMEOUT}s after bootout; not bootstrapping a second deskflow-core"* ]]
  grep -q "^300" "$SHIM_STATE/ps.txt"
}

@test "converge is report-only when its own program cannot be executed (TCC evidence, exit 126, protected path, missing file); assert-single names it" {
  healthy
  rm -f "$SHIM_STATE/loaded/$GUI"; del_proc 301        # the GUI died and launchd lost the label: a free tick would repair ...
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"    # ... and could
  # (a) evidence: the tick log ends with launchd's shell refused at the ctl, written just now
  printf '/bin/bash: %s: Operation not permitted\n' "$HOME/Desktop/deskflow/scripts/deskflow-ctl" >"$HOME/Library/Logs/Deskflow/converge.log"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 1 ]
  [[ "$output" == *"gated: converge agent cannot execute its program (TCC): $HOME/Desktop/deskflow/scripts/deskflow-ctl; report-only until deskflow-ctl start"* ]]
  [[ "$output" == *"  converge agent cannot execute its program (TCC): $HOME/Desktop/deskflow/scripts/deskflow-ctl"* ]]
  log_lacks "launchctl bootstrap"
  log_lacks "launchctl kickstart"
  python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert d['gate'].startswith('converge agent cannot execute its program (TCC)') and d['actions']==[] and 'bootstrap $GUI' in d['plan'] and d['assert_single'].startswith('FAIL')" "$STATE/health.json"
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 1 ]
  [[ "$output" == *"converge agent cannot execute its program (TCC): $HOME/Desktop/deskflow/scripts/deskflow-ctl"* ]]
  # the detection line itself must never re-trigger the detection (it lands in the same log)
  bash "$SCRIPT" converge --apply >>"$HOME/Library/Logs/Deskflow/converge.log" 2>&1 || true
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 0 ]
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
  # (b) launchd's LOADED definition still runs the checkout and reports exit 126 (a re-rendered
  #     file changes nothing until the next bootstrap): report-only again
  rm -f "$SHIM_STATE/loaded/$GUI"; del_proc 301
  mkdir -p "$SHIM_STATE/args" "$SHIM_STATE/exit"
  printf '/bin/bash\n%s\nconverge\n--apply\n--quiet\n' "$HOME/Desktop/deskflow/scripts/deskflow-ctl" >"$SHIM_STATE/args/$CONVERGE"
  echo 126 >"$SHIM_STATE/exit/$CONVERGE"
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 1 ]
  [[ "$output" == *"(TCC): $HOME/Desktop/deskflow/scripts/deskflow-ctl (launchd last exit code 126)"* ]]
  log_lacks "launchctl bootstrap"
  rm -f "$SHIM_STATE/args/$CONVERGE" "$SHIM_STATE/exit/$CONVERGE"
  # (c) the installed plist points into ~/Desktop: named by assert-single (twice: program + plist line),
  #     re-rendered onto the safe copy as a pending change, nothing started
  sed -i '' "s#<string>$SAFE/deskflow-ctl</string>#<string>$HOME/Desktop/deskflow/scripts/deskflow-ctl</string>#" "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 1 ]
  [[ "$output" == *"(TCC): $HOME/Desktop/deskflow/scripts/deskflow-ctl is under ~/Desktop, ~/Documents, ~/Downloads, iCloud Drive"* ]]
  [[ "$output" == *"fleet launchd plist points into a TCC-protected folder"*"$CONVERGE.plist:"*":$HOME/Desktop/deskflow/scripts/deskflow-ctl"* ]]
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 1 ]
  [[ "$output" == *"gated: converge agent cannot execute its program (TCC): $HOME/Desktop/deskflow/scripts/deskflow-ctl is under"* ]]
  [[ "$output" == *"render $CONVERGE (takes effect at next deskflow-ctl start/restart)"* ]]
  grep -q "<string>$SAFE/deskflow-ctl</string>" "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
  log_lacks "launchctl bootstrap"
  log_lacks "launchctl bootout"
  # (d) the program file is gone
  rm -f "$SHIM_STATE/loaded/$CONVERGE"
  sed -i '' "s#<string>$SAFE/deskflow-ctl</string>#<string>$TMP/nowhere/deskflow-ctl</string>#" "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 1 ]
  [[ "$output" == *"converge agent cannot execute its program (missing or unreadable): $TMP/nowhere/deskflow-ctl"* ]]
  log_lacks "launchctl bootstrap"
  # (e) start is the repair: safe copy refreshed, converge re-rendered + bootstrapped, tick free again
  : >"$SHIM_LOG"
  run bash "$SCRIPT" start
  [ "$status" -eq 0 ]
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
  grep -q "<string>$SAFE/deskflow-ctl</string>" "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 0 ]
  [[ "$output" == *"nothing to do"* ]]
}

@test "the launchd-safe copy runs converge without any checkout, knows it is the copy, and refreshes only from DESKFLOW_ROOT" {
  # a throwaway checkout: start installs the safe copy from it, then it disappears
  fake="$TMP/checkout"
  mkdir -p "$fake/scripts" "$fake/tools/launchd"
  cp "$SCRIPT" "$fake/scripts/deskflow-ctl"; cp "$REPO"/tools/launchd/*.plist "$fake/tools/launchd/"
  healthy_seat; load_agent $CONVERGE
  run bash "$fake/scripts/deskflow-ctl" start
  [ "$status" -eq 0 ]
  rm -rf "$fake"
  : >"$SHIM_LOG"
  run bash "$SAFE/deskflow-ctl" converge --apply
  [ "$status" -eq 0 ]
  [[ "$output" == *"nothing to do"* ]]
  [[ "$output" == *"assert-single: OK"* ]]
  run bash "$SAFE/deskflow-ctl" status
  [ "$status" -eq 0 ]
  [[ "$output" == *"ctl: $SAFE/deskflow-ctl (launchd-safe copy; checkout: unknown, DESKFLOW_ROOT unset)"* ]]
  run bash "$SAFE/deskflow-ctl" safe-copy
  [ "$status" -eq 0 ]
  [[ "$output" == *"safe-copy: no checkout known"* ]]
  # a newer checkout named by DESKFLOW_ROOT refreshes the copy; the checkout's own converge sees the gap first
  mkdir -p "$fake/scripts" "$fake/tools/launchd"
  cp "$SCRIPT" "$fake/scripts/deskflow-ctl"; cp "$REPO"/tools/launchd/*.plist "$fake/tools/launchd/"
  echo "# newer" >>"$fake/scripts/deskflow-ctl"
  run bash "$fake/scripts/deskflow-ctl" converge
  [ "$status" -eq 0 ]
  [[ "$output" == *"launchd-safe copy $SAFE differs from this checkout (deskflow-ctl start refreshes it)"* ]]
  DESKFLOW_ROOT="$fake" run bash "$SAFE/deskflow-ctl" safe-copy
  [ "$status" -eq 0 ]
  [[ "$output" == *"safe-copy: installed $SAFE/deskflow-ctl"* ]]
  cmp -s "$fake/scripts/deskflow-ctl" "$SAFE/deskflow-ctl"
  run bash "$SCRIPT" status
  [[ "$output" == *"(checkout); safe copy: $SAFE/deskflow-ctl"* ]]
}

# --- K10 review regressions (adversarial review, 2026-09-25) -----------------
#
# Each of these was a proven defect; the assertions state the behaviour the
# review demanded. env/<label> (K=V lines) is the `environment = {` block a
# loaded label prints, beside args/<label> (its `arguments = {` block).

set_args() { mkdir -p "$SHIM_STATE/args"; printf '%s\n' "$2" >"$SHIM_STATE/args/$1"; }
set_env() { mkdir -p "$SHIM_STATE/env"; printf '%s\n' "$2" >"$SHIM_STATE/env/$1"; }
# Whole-line matches: the GUI label is a prefix of the -core and -converge
# labels, so a substring check on "...$GUI" also matches their lines.
log_has_line() { grep -qxF -- "$1" "$SHIM_LOG" || { echo "missing line from shim log: $1" >&2; return 1; }; }
log_lacks_line() { ! grep -qxF -- "$1" "$SHIM_LOG" || { echo "unexpected line in shim log: $1" >&2; return 1; }; }
make_fake_checkout() {
  # A throwaway checkout beside the sandbox; fake_real is its physical path
  # (what the ctl records: $TMP sits behind the /var -> /private/var symlink).
  fake="$TMP/checkout"
  mkdir -p "$fake/scripts" "$fake/tools/launchd"
  cp "$SCRIPT" "$fake/scripts/deskflow-ctl"; cp "$REPO"/tools/launchd/*.plist "$fake/tools/launchd/"
  cp "$REPO/tools/fleet-soak" "$fake/tools/fleet-soak"
  fake_real="$(cd "$fake" && pwd -P)"
}

@test "K10-review: every verb runs from the launchd-safe copy with the checkout gone and never names it" {
  make_fake_checkout
  add_prio_binary
  healthy_seat; load_agent $CONVERGE
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  run bash "$fake/scripts/deskflow-ctl" start
  [ "$status" -eq 0 ]
  [ "$(cat "$SAFE/checkout")" = "$fake_real" ]     # the copy remembers where it came from
  rm -rf "$fake"
  all="$TMP/all-output.txt"; : >"$all"
  for verb in "status" "assert-single" "converge" "converge --apply --quiet" "prio" "retire" "login-items audit" "login-items print-steps" "render-plist com.fleet.soak" "render-plist $CORE" "safe-copy" "help" "restart" "stop"; do
    # shellcheck disable=SC2086
    run bash "$SAFE/deskflow-ctl" $verb
    echo "--- $verb (rc=$status) ---" >>"$all"; echo "$output" >>"$all"
    [ "$status" -eq 0 ] || { echo "verb '$verb' rc=$status: $output" >&2; false; }
  done
  run bash "$SAFE/deskflow-ctl" start
  echo "--- start (rc=$status) ---" >>"$all"; echo "$output" >>"$all"
  [ "$status" -eq 0 ]
  [[ "$output" == *"safe-copy: no checkout known"* ]]
  ! grep -qF "$TMP/checkout" "$all"      # a vanished checkout is never named ...
  ! grep -qF "$fake_real" "$all"         # ... under either spelling
  ! grep -q "missing template" "$all"
  grep -q "assert-single: OK" "$all"
  grep -q "<string>$SAFE/fleet-soak</string>" "$all"
}

@test "K10-review: a DESKFLOW_ROOT that is not a checkout is a warning for start (fallback: the recorded checkout, else none) and an error for safe-copy" {
  make_fake_checkout
  healthy_seat; load_agent $CONVERGE
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  bash "$fake/scripts/deskflow-ctl" start >/dev/null
  # the copy refreshes itself from the recorded checkout, no DESKFLOW_ROOT needed
  echo "# newer" >>"$fake/scripts/deskflow-ctl"
  run bash "$SAFE/deskflow-ctl" safe-copy
  [ "$status" -eq 0 ]
  [[ "$output" == *"safe-copy: installed $SAFE/deskflow-ctl"* ]]
  cmp -s "$fake/scripts/deskflow-ctl" "$SAFE/deskflow-ctl"
  run bash "$SAFE/deskflow-ctl" status
  [[ "$output" == *"checkout: $fake_real (recorded by the last safe-copy)"* ]]
  # a stale DESKFLOW_ROOT: start warns, names the variable, falls back and starts
  bash "$SCRIPT" stop >/dev/null 2>&1 || true
  DESKFLOW_ROOT="$TMP/nowhere" run bash "$SAFE/deskflow-ctl" start
  [ "$status" -eq 0 ]
  [[ "$output" == *"warning: safe-copy: DESKFLOW_ROOT=$TMP/nowhere is not a deskflow checkout"* ]]
  [[ "$output" == *"safe-copy: $SAFE up to date"* ]]
  # ... and is an error for the safe-copy verb itself
  DESKFLOW_ROOT="$TMP/nowhere" run bash "$SAFE/deskflow-ctl" safe-copy
  [ "$status" -eq 1 ]
  [[ "$output" == *"DESKFLOW_ROOT=$TMP/nowhere is not a deskflow checkout"* ]]
  # the checkout gone AND a stale DESKFLOW_ROOT naming it (K10ADV-2): a warning, and the seat still starts
  rm -rf "$fake"
  bash "$SCRIPT" stop >/dev/null 2>&1 || true
  DESKFLOW_ROOT="$fake" run bash "$SAFE/deskflow-ctl" start
  [ "$status" -eq 0 ]
  [[ "$output" == *"safe-copy: no checkout known"* ]]
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
}

@test "K10-review: a deskflow-core from a NON-canonical path blocks start with a remedy that works: stop terminates it, start then owns the seat" {
  add_proc 555 "$TMP/Applications/Deskflow.app.bak/Contents/MacOS/deskflow-core"
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" start
  [ "$status" -eq 1 ]
  [[ "$output" == *"bootstrap $CORE refused: deskflow-core pid 555 already running outside launchd; deskflow-ctl stop"* ]]
  log_lacks "launchctl bootstrap"
  run bash "$SCRIPT" stop
  [ "$status" -eq 0 ]
  log_has "kill -TERM 555"
  [[ "$output" == *"escalating: SIGTERM to 555 ($TMP/Applications/Deskflow.app.bak/Contents/MacOS/deskflow-core)"* ]]
  ! grep -q "^555" "$SHIM_STATE/ps.txt"
  : >"$SHIM_LOG"
  run bash "$SCRIPT" start
  [ "$status" -eq 0 ]
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
}

@test "K10-review: a deskflow-core of another uid blocks start/restart with the exact sudo line; stop never signals it and exits 1" {
  add_proc 556 "$APP/Contents/MacOS/deskflow-core" 0
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  run bash "$SCRIPT" start
  [ "$status" -eq 1 ]
  [[ "$output" == *"bootstrap $CORE refused: deskflow-core pid 556 already running outside launchd; not this user's to signal (deskflow-ctl stop cannot clear it); run as admin: sudo kill 556  # $APP/Contents/MacOS/deskflow-core (uid 0); then deskflow-ctl start"* ]]
  [[ "$output" != *"deskflow-ctl stop (bootout"* ]]     # never a remedy that cannot work
  run bash "$SCRIPT" stop
  [ "$status" -eq 1 ]
  log_lacks "kill "
  [[ "$output" == *"NOT stopped"* ]]
  [[ "$output" == *"sudo kill 556   # deskflow-core pid 556 uid 0: $APP/Contents/MacOS/deskflow-core"* ]]
  [[ "$output" != *"== deskflow-ctl: stopped"* ]]
  grep -q "^556" "$SHIM_STATE/ps.txt"
  load_agent $CORE
  : >"$SHIM_LOG"
  run bash "$SCRIPT" restart
  [ "$status" -eq 1 ]
  [[ "$output" == *"kickstart -k $CORE refused: deskflow-core pid 556"*"sudo kill 556"* ]]
  log_lacks "launchctl kickstart"
  # both kinds at once: each pid gets the remedy that clears it
  add_proc 557 "$APP/Contents/MacOS/deskflow-core"
  run bash "$SCRIPT" start
  [ "$status" -eq 1 ]
  [[ "$output" == *"pid 556 557 already running outside launchd; deskflow-ctl stop (bootout, then TERM/KILL of every deskflow-core/Deskflow this user runs, any path) first, then start; not this user's to signal"*"sudo kill 556"* ]]
}

@test "K10-review: the TCC refusal is canonical and case-insensitive (APFS): ~/desktop, ~/Library/../Desktop, a symlink into ~/Desktop, iCloud Drive, /Volumes, and a <string> under ~/documents" {
  for dir in "$HOME/desktop/bin" "$HOME/Library/../Desktop/bin" "$HOME/Library/Mobile Documents/com~apple~CloudDocs/bin" "/Volumes/Stick/bin"; do
    DESKFLOW_CTL_SAFE_DIR="$dir" run bash "$SCRIPT" safe-copy
    [ "$status" -eq 1 ] || { echo "$dir was accepted: $output" >&2; false; }
    [[ "$output" == *"launchd could never execute it"* ]]
  done
  mkdir -p "$HOME/Desktop"; ln -s "$HOME/Desktop" "$HOME/link-to-desktop"
  DESKFLOW_CTL_SAFE_DIR="$HOME/link-to-desktop/bin" run bash "$SCRIPT" safe-copy
  [ "$status" -eq 1 ]
  [[ "$output" == *"launchd could never execute it"* ]]
  # a render is refused the same way whatever the spelling
  DESKFLOW_CTL_SOAK_DIR="$HOME/documents/soak" run bash "$SCRIPT" render-plist com.fleet.soak
  [ "$status" -eq 1 ]
  [[ "$output" == *"points launchd into"* ]]
  # the default and a merely similar folder are fine
  run bash "$SCRIPT" safe-copy
  [ "$status" -eq 0 ]
  DESKFLOW_CTL_SAFE_DIR="$HOME/Desktopish/bin" run bash "$SCRIPT" safe-copy
  [ "$status" -eq 0 ]
}

@test "K10-review: start replaces a LOADED definition that differs from the render (converge --apply re-rendered the file, launchd still ran the checkout path) and restarts nothing else" {
  healthy
  old="$HOME/Desktop/deskflow/scripts/deskflow-ctl"
  sed -i '' "s#<string>$SAFE/deskflow-ctl</string>#<string>$old</string>#" "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
  set_args $CONVERGE "$(printf '/bin/bash\n%s\nconverge\n--apply\n--quiet' "$old")"
  mkdir -p "$SHIM_STATE/exit"; echo 126 >"$SHIM_STATE/exit/$CONVERGE"
  # runbook 1b: converge --apply by hand from the checkout: gated, and the FILE is re-rendered
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 1 ]
  [[ "$output" == *"(launchd last exit code 126)"* ]]
  [[ "$output" == *"render $CONVERGE (takes effect at next deskflow-ctl start/restart)"* ]]
  grep -q "<string>$SAFE/deskflow-ctl</string>" "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
  # the next tick: the file is current, launchd still runs the old definition -- named, not hidden
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 1 ]
  [[ "$output" == *"rebootstrap $CONVERGE (launchd runs an older definition than $DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist; takes effect at next deskflow-ctl start/restart)"* ]]
  python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert d['needs_rebootstrap'] is True and d['actions']==[]" "$STATE/health.json"
  log_lacks "launchctl bootout"
  # runbook 1d: `start` is the repair: bootout + bootstrap of THAT label only; the current core and GUI are just kickstarted
  : >"$SHIM_LOG"
  run bash "$SCRIPT" start
  [ "$status" -eq 0 ]
  [[ "$output" == *"$CONVERGE: launchd's loaded definition (argv/environment) differs from the rendered plist; bootout then bootstrap"* ]]
  log_has "launchctl bootout $DOMAIN/$CONVERGE"
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
  log_lacks "launchctl bootout $DOMAIN/$CORE"
  log_lacks_line "launchctl bootout $DOMAIN/$GUI"
  log_has "launchctl kickstart $DOMAIN/$CORE"
  log_has_line "launchctl kickstart $DOMAIN/$GUI"
  # the shim keeps args/exit until a bootout, as launchd keeps the loaded definition
  rm -f "$SHIM_STATE/args/$CONVERGE" "$SHIM_STATE/exit/$CONVERGE"
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 0 ]
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 0 ]
  [[ "$output" == *"nothing to do"* ]]
}

@test "K10-review: a loaded GUI whose launchd environment lacks DESKFLOW_LAUNCHD=1 is re-bootstrapped by start and restart although the file matches (env drift)" {
  healthy
  set_args $GUI "$APP/Contents/MacOS/Deskflow"
  set_env $GUI "$(printf 'OSLogRateLimit=64\nXPC_SERVICE_NAME=%s' "$GUI")"   # the Sep 16 macbookpro GUI: no DESKFLOW_LAUNCHD
  : >"$SHIM_LOG"
  run bash "$SCRIPT" start
  [ "$status" -eq 0 ]
  [[ "$output" == *"$GUI: launchd's loaded definition (argv/environment) differs from the rendered plist; bootout then bootstrap"* ]]
  log_has_line "launchctl bootout $DOMAIN/$GUI"
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
  log_lacks "launchctl bootout $DOMAIN/$CORE"
  # the same definition, complete: current, only kickstarted
  set_env $GUI "$(printf 'OSLogRateLimit=64\nXPC_SERVICE_NAME=%s\nDESKFLOW_LAUNCHD=1' "$GUI")"
  : >"$SHIM_LOG"
  run bash "$SCRIPT" start
  [ "$status" -eq 0 ]
  log_lacks_line "launchctl bootout $DOMAIN/$GUI"
  log_has_line "launchctl kickstart $DOMAIN/$GUI"
  # restart: -k would only restart the OLD definition, so drift goes through bootout + bootstrap there too
  set_env $GUI "$(printf 'OSLogRateLimit=64\nXPC_SERVICE_NAME=%s' "$GUI")"
  : >"$SHIM_LOG"
  run bash "$SCRIPT" restart
  [ "$status" -eq 0 ]
  log_has_line "launchctl kickstart -k $DOMAIN/$CORE"
  log_lacks_line "launchctl kickstart -k $DOMAIN/$GUI"
  log_has_line "launchctl bootout $DOMAIN/$GUI"
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
}

@test "K10-review: EPERM evidence is shape-specific and windowed: an old refusal never gates, a tick's own 'Permission denied' is not TCC on its program, /bin/sh's refusal is" {
  healthy
  rm -f "$SHIM_STATE/loaded/$GUI"; del_proc 301
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  log="$HOME/Library/Logs/Deskflow/converge.log"
  printf '/bin/bash: %s: Operation not permitted\n' "$HOME/Desktop/deskflow/scripts/deskflow-ctl" >"$log"
  touch -t "$(date -v-11M +%Y%m%d%H%M.%S)" "$log"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 0 ]
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
  # a tick that died on its own redirection ran, so its program is fine
  rm -f "$SHIM_STATE/loaded/$GUI"; del_proc 301
  printf '%s: line 810: %s/.tmp: Permission denied\n' "$SAFE/deskflow-ctl" "$HOME/Library/Application Support/Deskflow" >"$log"
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 0 ]
  [[ "$output" != *"cannot execute its program"* ]]
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
  # the refusal shape from /bin/sh is evidence too
  rm -f "$SHIM_STATE/loaded/$GUI"; del_proc 301
  printf '/bin/sh: %s: Permission denied\n' "$HOME/Documents/x/deskflow-ctl" >"$log"
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge --apply
  [ "$status" -eq 1 ]
  [[ "$output" == *"cannot execute its program (TCC): $HOME/Documents/x/deskflow-ctl"* ]]
  log_lacks "launchctl bootstrap"
}

@test "K10-review: a stubborn old core: start leaves the label booted out; converge refuses the core AND holds the GUI; stop KILLs it; start then owns the seat" {
  load_agent $CORE 300; add_proc 300 "$APP/Contents/MacOS/deskflow-core"
  load_agent $CONVERGE
  autostart $CORE 300 "$APP/Contents/MacOS/deskflow-core"
  autostart $GUI 301 "$APP/Contents/MacOS/Deskflow"
  mkdir -p "$DESKFLOW_CTL_AGENT_DIR"
  echo "<plist>stale</plist>" >"$DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  mkdir -p "$SHIM_STATE/stubborn"; touch "$SHIM_STATE/stubborn/300"
  run bash "$SCRIPT" start
  [ "$status" -eq 1 ]
  [ ! -f "$SHIM_STATE/loaded/$CORE" ]          # booted out, never bootstrapped beside the stubborn pid
  : >"$SHIM_LOG"
  run bash "$SCRIPT" converge --apply --quiet
  [ "$status" -eq 1 ]
  [[ "$output" == *"refused bootstrap $CORE: deskflow-core pid 300 already running outside launchd"* ]]
  log_lacks "launchctl bootstrap"
  # the GUI plist render is a free repair (as for any unloaded agent); no start action at all
  python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert 'bootstrap $GUI held while $CORE is refused (deskflow-ctl stop, then start)' in d['plan'] and not [a for a in d['actions'] if not a.startswith('render')] and d['errors'][0].startswith('refused bootstrap $CORE')" "$STATE/health.json"
  run bash "$SCRIPT" stop
  [ "$status" -eq 0 ]
  log_has "kill -KILL 300"
  : >"$SHIM_LOG"
  run bash "$SCRIPT" start
  [ "$status" -eq 0 ]
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$CORE.plist"
  log_has "launchctl bootstrap $DOMAIN $DESKFLOW_CTL_AGENT_DIR/$GUI.plist"
}

@test "K10-review: assert-single polices fleet-label plists only: a third party's plist under ~/Documents is ignored, a sibling product's fleet-label plist (any spelling) is not" {
  healthy
  mkdir -p "$DESKFLOW_CTL_AGENT_DIR"
  printf '<plist><dict><key>ProgramArguments</key><array><string>%s/Documents/backup.sh</string></array></dict></plist>\n' "$HOME" >"$DESKFLOW_CTL_AGENT_DIR/com.example.backup.plist"
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 0 ]
  printf '<plist><dict><key>ProgramArguments</key><array><string>%s/desktop/Mouser/tools/x</string></array></dict></plist>\n' "$HOME" >"$DESKFLOW_CTL_AGENT_DIR/io.github.hughesyadaddy.mouser.plist"
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 1 ]
  [[ "$output" == *"io.github.hughesyadaddy.mouser.plist:"*"$HOME/desktop/Mouser/tools/x"* ]]
}

@test "K10-review: a template change in a NEWER checkout is a pending start, never applied from the old safe copy, and the copy's own tick never reports the drift" {
  make_fake_checkout
  healthy_seat; load_agent $CONVERGE
  bash "$fake/scripts/deskflow-ctl" start >/dev/null
  echo "# newer checkout" >>"$fake/scripts/deskflow-ctl"
  run bash "$SAFE/deskflow-ctl" converge --apply --quiet
  [ "$status" -eq 0 ]
  ! grep -q "differs" "$STATE/health.json"
  sed -i '' 's#<integer>60</integer>#<integer>30</integer>#' "$fake/tools/launchd/$CONVERGE.plist"
  run bash "$fake/scripts/deskflow-ctl" converge --apply
  [ "$status" -eq 0 ]
  [[ "$output" == *"differs from this checkout"* ]]
  [[ "$output" != *"render $CONVERGE"* ]]
  grep -q "<integer>60</integer>" "$DESKFLOW_CTL_AGENT_DIR/$CONVERGE.plist"
}

# --- --sudo-stdin: root steps through sudo -S, password on stdin -------------------

# sudo shim for these tests only (setup() leaves tools/tests/fakebin/sudo, exit
# 97, on PATH): logs the argv -- which must never hold the password -- records
# every stdin line in $SHIM_STATE/sudo-stdin.log, rejects anything but
# SHIM_SUDO_EXPECT_PW when that is set, then runs launchctl/sfltool/rm through
# the shims (sandbox paths only) and merely acknowledges install/chown/true.
make_sudo_shim() {
  make_shim sudo <<'EOF'
opts=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    -S|-k) opts+=("$1"); shift ;;
    -p) opts+=("$1" "$2"); shift 2 ;;
    *) break ;;
  esac
done
echo "sudo ${opts[*]} $*" >> "$SHIM_LOG"
pw=""
if [[ " ${opts[*]} " == *" -S "* ]]; then
  IFS= read -r pw || pw=""
  printf '%s\n' "$pw" >> "$SHIM_STATE/sudo-stdin.log"
else
  echo "sudo shim: called without -S" >> "$SHIM_LOG"
fi
if [[ -n "${SHIM_SUDO_EXPECT_PW:-}" && "$pw" != "$SHIM_SUDO_EXPECT_PW" ]]; then
  echo "Sorry, try again." >&2
  exit 1
fi
case "$1" in
  launchctl|rm) exec "$@" ;;
  sfltool) SHIM_AS_ROOT=1 exec "$@" ;;
  install|chown|true) exit 0 ;;
  *) echo "sudo shim: unexpected command: $*" >&2; exit 98 ;;
esac
EOF
}

with_stdin() { # pw verb args... -> run the ctl with pw as its one stdin line
  local pw="$1"; shift
  run bash -c 'printf "%s\n" "$0" | bash "$1" "${@:2}"' "$pw" "$SCRIPT" "$@"
}

# $output assertions that FAIL the test (a false [[ ]] mid-test is ignored under bash 3.2 bats).
out_has() { if [[ "$output" != *"$1"* ]]; then echo "expected in output: $1" >&2; return 1; fi; }
out_lacks() { if [[ "$output" == *"$1"* ]]; then echo "unexpected in output: $1" >&2; return 1; fi; }

@test "prio --sudo-stdin installs and bootstraps the LaunchDaemon through sudo -S (password on stdin, never argv)" {
  add_prio_binary
  make_sudo_shim
  export SHIM_SUDO_EXPECT_PW=r00t
  with_stdin r00t prio --sudo-stdin
  [ "$status" -eq 0 ]
  staged="$DESKFLOW_CTL_DAEMON_STAGE_DIR/$PRIO.plist"
  log_has "sudo -S -p  -k install -d $DESKFLOW_CTL_DAEMON_DIR"
  log_has "sudo -S -p  -k install -d -m 1777 /private/var/db/deskflow"
  log_has "sudo -S -p  -k install -m 644 -o root -g wheel $staged $DESKFLOW_CTL_DAEMON_DIR/$PRIO.plist"
  log_has "sudo -S -p  -k chown root:wheel $APP/Contents/MacOS/deskflow-prio"
  log_has "sudo -S -p  -k launchctl bootstrap system $DESKFLOW_CTL_DAEMON_DIR/$PRIO.plist"
  [ "$(grep -c '^sudo ' "$SHIM_LOG")" -eq "$(grep -c '^sudo -S -p  -k ' "$SHIM_LOG")" ]
  log_lacks "r00t"
  log_lacks "without -S"
  [ "$(sort -u "$SHIM_STATE/sudo-stdin.log")" = "r00t" ]
  [ -f "$SHIM_STATE/loaded/$PRIO" ]
  out_has "system/$PRIO bootstrapped"
  out_lacks "needs root"
  # a loaded daemon whose installed plist is stale is booted out first, through sudo too
  : >"$SHIM_LOG"
  mkdir -p "$DESKFLOW_CTL_DAEMON_DIR"
  echo "<plist>old</plist>" >"$DESKFLOW_CTL_DAEMON_DIR/$PRIO.plist"
  with_stdin r00t prio --sudo-stdin
  [ "$status" -eq 0 ]
  log_has "sudo -S -p  -k launchctl bootout system/$PRIO"
  log_has "sudo -S -p  -k launchctl bootstrap system $DESKFLOW_CTL_DAEMON_DIR/$PRIO.plist"
  log_lacks "r00t"
}

@test "retire --sudo-stdin removes the root-owned files through sudo -S rm and exits 0" {
  make_sudo_shim
  mkdir -p "$(dirname "$DESKFLOW_CTL_RETIRED_PRIO_APPLY")" "$(dirname "$DESKFLOW_CTL_RETIRED_SYNERGY_AGENT")"
  : >"$DESKFLOW_CTL_RETIRED_PRIO_APPLY"
  ln -s /nonexistent "$DESKFLOW_CTL_RETIRED_SYNERGY_AGENT"
  with_stdin r00t retire --sudo-stdin
  [ "$status" -eq 0 ]
  log_has "sudo -S -p  -k rm -f $DESKFLOW_CTL_RETIRED_PRIO_APPLY"
  log_has "sudo -S -p  -k rm -f $DESKFLOW_CTL_RETIRED_SYNERGY_AGENT"
  [ ! -e "$DESKFLOW_CTL_RETIRED_PRIO_APPLY" ]
  [ ! -L "$DESKFLOW_CTL_RETIRED_SYNERGY_AGENT" ]
  out_has "removed $DESKFLOW_CTL_RETIRED_PRIO_APPLY (sudo -S"
  out_has "nothing left to retire"
  log_lacks "r00t"
}

@test "login-items audit --sudo-stdin retries dumpbtm through sudo -S only after the unprivileged call is refused; without it SKIP and no sudo at all" {
  make_sudo_shim
  echo 1 >"$SHIM_STATE/btm.rc"; echo "Error: dumpbtm requires root privileges" >"$SHIM_STATE/btm.err"
  run bash "$SCRIPT" login-items audit
  [ "$status" -eq 3 ]
  log_lacks "sudo"
  : >"$SHIM_LOG"
  with_stdin r00t login-items audit --sudo-stdin
  [ "$status" -eq 0 ]
  out_has "login-items: OK"
  [ "$(grep -c '^sfltool dumpbtm' "$SHIM_LOG")" -eq 2 ]      # unprivileged first, then under sudo
  log_has "sudo -S -p  -k sfltool dumpbtm"
  [ "$(grep -c '^sudo ' "$SHIM_LOG")" -eq 1 ]
  log_lacks "r00t"
  # when the unprivileged call works, sudo is never used even with a password
  rm -f "$SHIM_STATE/btm.rc" "$SHIM_STATE/btm.err"; : >"$SHIM_LOG"
  with_stdin r00t login-items audit --sudo-stdin
  [ "$status" -eq 0 ]
  log_lacks "sudo"
  # and assert-single (what converge runs every 60 s) never escalates on its own
  healthy_seat
  echo 1 >"$SHIM_STATE/btm.rc"; : >"$SHIM_LOG"
  run bash "$SCRIPT" assert-single
  [ "$status" -eq 1 ]
  log_lacks "sudo"
}

@test "a rejected stdin password is reported once and the sudo lines are printed: prio exits 1, retire exits 2" {
  add_prio_binary
  make_sudo_shim
  export SHIM_SUDO_EXPECT_PW=r00t
  with_stdin wrong prio --sudo-stdin
  [ "$status" -eq 1 ]
  out_has "sudo password rejected for $(hostname -s)"
  [ "$(grep -c 'sudo password rejected' <<<"$output")" -eq 1 ]
  out_has "needs root; run once as admin"
  out_has "sudo launchctl bootstrap system"
  log_lacks "wrong"
  [ ! -f "$SHIM_STATE/loaded/$PRIO" ]
  mkdir -p "$(dirname "$DESKFLOW_CTL_RETIRED_PRIO_APPLY")"; : >"$DESKFLOW_CTL_RETIRED_PRIO_APPLY"
  with_stdin wrong retire --sudo-stdin
  [ "$status" -eq 2 ]
  out_has "sudo rm -f \"$DESKFLOW_CTL_RETIRED_PRIO_APPLY\""
  [ -e "$DESKFLOW_CTL_RETIRED_PRIO_APPLY" ]
}

@test "--sudo-stdin with an empty line falls back to print mode without touching sudo" {
  add_prio_binary
  make_sudo_shim
  with_stdin "" prio --sudo-stdin
  [ "$status" -eq 0 ]
  out_has "empty password line on stdin"
  out_has "needs root; run once as admin"
  log_lacks "sudo"
}

@test "bash -x never echoes the stdin password" {
  make_sudo_shim
  mkdir -p "$(dirname "$DESKFLOW_CTL_RETIRED_PRIO_APPLY")"; : >"$DESKFLOW_CTL_RETIRED_PRIO_APPLY"
  run bash -c 'printf "%s\n" "$0" | bash -x "$1" retire --sudo-stdin' r00t "$SCRIPT"
  [ "$status" -eq 0 ]
  out_has "+ cmd_retire"
  out_lacks "r00t"
  log_lacks "r00t"
}
