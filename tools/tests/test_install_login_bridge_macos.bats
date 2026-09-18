#!/usr/bin/env bats
# scripts/install-login-bridge-macos.sh — the single generator of the
# LoginWindow bridge plist. Peer entries carry wake hints
# (name=ip|lan|mac|wakeCommand, Peer.h); only ip/lan may become server hosts.
#
# Everything runs against tmp paths (DESKFLOW_LOGIN_BRIDGE_PLIST/_LOG,
# DESKFLOW_MACHINE_LOCK_DIR, DESKFLOW_INSTALL_APP); the script refuses any
# other write path under bats. osascript/launchctl/id are PATH shims.

bats_require_minimum_version 1.5.0

SCRIPT="$BATS_TEST_DIRNAME/../../scripts/install-login-bridge-macos.sh"

setup() {
  TMP="$(mktemp -d "${BATS_TEST_TMPDIR:-${TMPDIR:-/tmp}}/login-bridge.XXXXXX")"
  SHIMS="$TMP/bin"
  export SHIM_LOG="$TMP/calls.log"
  export SHIM_STATE="$TMP/state"
  export HOME="$TMP/home"
  export APP="$TMP/Applications/Deskflow.app"
  export DESKFLOW_INSTALL_APP="$APP"
  export DESKFLOW_SETTINGS="$TMP/Deskflow.conf"
  export DESKFLOW_LOGIN_BRIDGE_PLIST="$TMP/LaunchAgents/org.deskflow.vhid-bridge.plist"
  export DESKFLOW_LOGIN_BRIDGE_LOG="$TMP/log/deskflow-vhid-bridge.log"
  export DESKFLOW_MACHINE_LOCK_DIR="$TMP/db/deskflow"
  unset DESKFLOW_LOGIN_BRIDGE_SCALE SUDO_UID
  mkdir -p "$SHIMS" "$SHIM_STATE" "$HOME" "$APP/Contents/MacOS" "$TMP/log"
  : >"$SHIM_LOG"
  : >"$APP/Contents/MacOS/deskflow-vhid-bridge"; chmod +x "$APP/Contents/MacOS/deskflow-vhid-bridge"

  write_conf "hackintosh=192.168.1.10|hackintosh.local|bc:24:11:aa:bb:cc|ssh proxmox qm resume 103, tiny11=192.168.1.100|tiny11.local|bc:24:11:dd:ee:ff|ssh proxmox qm start 100, macbookpro=192.168.1.138|macbookpro.local"

  make_shim osascript <<'EOF'
echo "osascript $*" >> "$SHIM_LOG"
# Mirror the admin prompt: run the shell fragment as this user (tmp paths only).
cmd="$2"; cmd="${cmd#do shell script \"}"; cmd="${cmd%\" with administrator privileges}"
bash -c "${cmd//\\\"/\"}"
EOF
  make_shim launchctl <<'EOF'
echo "launchctl $*" >> "$SHIM_LOG"
case "$1" in
  print) [[ -f "$SHIM_STATE/loaded-${2##*/}" ]] || exit 113 ;;
  bootout) rm -f "$SHIM_STATE/loaded-${2##*/}" ;;
esac
exit 0
EOF
  export PATH="$SHIMS:$PATH"
}

teardown() { rm -rf "$TMP"; }

make_shim() { { echo '#!/usr/bin/env bash'; cat; } >"$SHIMS/$1"; chmod +x "$SHIMS/$1"; }

write_conf() {
  cat >"$DESKFLOW_SETTINGS" <<EOF
[core]
computerName=macbookpro
port=24800

[coordination]
peers="$1"
EOF
}

fake_root() {
  make_shim id <<'EOF'
[[ "$1" == "-u" ]] && { echo 0; exit 0; }
exec /usr/bin/id "$@"
EOF
}

hosts_arg() {
  # ProgramArguments[1] of the rendered plist on stdout of --dry-run.
  plutil -extract ProgramArguments.1 raw -o - - <<<"$1"
}

# --- peer parsing -------------------------------------------------------------

@test "dry-run renders the plist to stdout with only ip/lan fields as hosts; MACs and wake commands never appear" {
  run --separate-stderr bash "$SCRIPT" --dry-run
  [ "$status" -eq 0 ]
  plutil -lint - <<<"$output" >/dev/null
  [[ "$stderr" == *"Servers: 192.168.1.10,hackintosh.local,192.168.1.100,tiny11.local"* ]]
  hosts="$(hosts_arg "$output")"
  [ "$hosts" = "192.168.1.10,hackintosh.local,192.168.1.100,tiny11.local" ]
  [[ "$output" != *"bc:24:11"* ]]
  [[ "$output" != *"ssh proxmox"* ]]
  [[ "$output" != *"macbookpro.local"* ]]   # self excluded
  [[ "$output" == *"<key>ExitTimeOut</key><integer>3</integer>"* ]]
  [[ "$output" == *"<string>$APP/Contents/MacOS/deskflow-vhid-bridge</string>"* ]]
  [[ "$output" == *"<string>--calibrate</string>"* ]]
  [[ "$output" == *"<string>$DESKFLOW_LOGIN_BRIDGE_LOG</string>"* ]]
  [ ! -e "$DESKFLOW_LOGIN_BRIDGE_PLIST" ]
  log_lacks_any
}

log_lacks_any() { [ ! -s "$SHIM_LOG" ] || { echo "unexpected calls: $(cat "$SHIM_LOG")" >&2; return 1; }; }

@test "bare names expand to name and name.local; bare addresses stay; entries are de-duplicated and self is skipped case-insensitively" {
  write_conf "gamepc, MacBookPro, tiny11.local, gamepc=gamepc"
  run --separate-stderr bash "$SCRIPT" --dry-run
  [ "$status" -eq 0 ]
  [ "$(hosts_arg "$output")" = "gamepc,gamepc.local,tiny11.local" ]
}

@test "a peer whose only fields are a MAC and a wake command yields no host, and no peers at all is an error" {
  write_conf "vm=bc:24:11:aa:bb:cc|ssh proxmox qm wakeup 100"
  run bash "$SCRIPT" --dry-run
  [ "$status" -eq 1 ]
  [[ "$output" == *"no coordination peers configured"* ]]
}

@test "--scale N and --scale=N seed the bridge; --scale-fixed disables calibration; junk is rejected" {
  run --separate-stderr bash "$SCRIPT" --dry-run --scale 4
  [ "$status" -eq 0 ]
  [[ "$output" == *"<string>--scale=4</string>"* ]]
  [[ "$output" == *"<string>--calibrate</string>"* ]]
  run --separate-stderr bash "$SCRIPT" --dry-run --scale=2.5 --scale-fixed
  [ "$status" -eq 0 ]
  [[ "$output" == *"<string>--scale=2.5</string>"* ]]
  [[ "$output" == *"<string>--scale-fixed</string>"* ]]
  [[ "$output" != *"--calibrate"* ]]
  run bash "$SCRIPT" --dry-run --scale "4; rm -rf /"
  [ "$status" -eq 1 ]
  run bash "$SCRIPT" --dry-run --scale-fixed
  [ "$status" -eq 1 ]
}

@test "the rendered plist is byte-stable across runs (what fleet-deploy and the GUI compare against)" {
  bash "$SCRIPT" --dry-run >"$TMP/a.plist" 2>/dev/null
  bash "$SCRIPT" --dry-run >"$TMP/b.plist" 2>/dev/null
  cmp -s "$TMP/a.plist" "$TMP/b.plist"
}

# --- install paths --------------------------------------------------------------

@test "as root (ssh + sudo) it installs directly: no osascript, log 0600, lock dir 1777, legacy user agent booted out" {
  fake_root
  touch "$SHIM_STATE/loaded-com.kvm.autoswitch"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  grep -q osascript "$SHIM_LOG" && { echo "osascript used as root" >&2; false; }
  [ -f "$DESKFLOW_LOGIN_BRIDGE_PLIST" ]
  plutil -lint "$DESKFLOW_LOGIN_BRIDGE_PLIST" >/dev/null
  [ "$(stat -f %Lp "$DESKFLOW_LOGIN_BRIDGE_PLIST")" = "644" ]
  [ "$(stat -f %Lp "$DESKFLOW_LOGIN_BRIDGE_LOG")" = "600" ]
  [ "$(stat -f "%Mp%Lp" "$DESKFLOW_MACHINE_LOCK_DIR")" = "1777" ]
  grep -q "launchctl bootout gui/0/com.kvm.autoswitch" "$SHIM_LOG"
  [ ! -e "$SHIM_STATE/loaded-com.kvm.autoswitch" ]
  [[ "$output" == *"Retired legacy gui/0/com.kvm.autoswitch"* ]]
  [[ "$output" == *".kvm-autoswitch"* ]]
  [[ "$output" == *"Installed $DESKFLOW_LOGIN_BRIDGE_PLIST"* ]]
}

@test "root under sudo boots the legacy agent out of the invoking user's domain (SUDO_UID)" {
  fake_root
  SUDO_UID=501 run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  grep -q "launchctl print gui/501/com.kvm.autoswitch" "$SHIM_LOG"
  log_lacks "launchctl bootout"
}

log_lacks() { ! grep -qF -- "$1" "$SHIM_LOG" || { echo "unexpected: $1" >&2; return 1; }; }

@test "as a normal user the same install fragment runs through the osascript admin prompt" {
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  grep -q "^osascript -e do shell script" "$SHIM_LOG"
  grep -q "with administrator privileges" "$SHIM_LOG"
  [ -f "$DESKFLOW_LOGIN_BRIDGE_PLIST" ]
  [ "$(stat -f %Lp "$DESKFLOW_LOGIN_BRIDGE_LOG")" = "600" ]
  [ -d "$DESKFLOW_MACHINE_LOCK_DIR" ]
  # the legacy per-user agent is only retired when loaded
  log_lacks "launchctl bootout"
}

@test "an existing world-readable log is tightened to 0600 on reinstall" {
  fake_root
  echo "key down id=0x61" >"$DESKFLOW_LOGIN_BRIDGE_LOG"; chmod 644 "$DESKFLOW_LOGIN_BRIDGE_LOG"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [ "$(stat -f %Lp "$DESKFLOW_LOGIN_BRIDGE_LOG")" = "600" ]
}

@test "no pkill/pgrep/killall remains; the legacy launcher is retired via launchctl bootout of its label only" {
  run grep -nE '^[^#]*[[:space:]](pkill|pgrep|killall)[[:space:]]' "$SCRIPT"
  [ "$status" -ne 0 ]
}

@test "refuses to install (not dry-run) under bats against a non-tmp plist path" {
  export DESKFLOW_LOGIN_BRIDGE_PLIST="/Library/LaunchAgents/org.deskflow.vhid-bridge.guard-test.plist"
  run bash "$SCRIPT"
  [ "$status" -eq 90 ]
  [ ! -e "$DESKFLOW_LOGIN_BRIDGE_PLIST" ]
  log_lacks_any
}

@test "missing config or bridge binary is a clear error" {
  rm "$DESKFLOW_SETTINGS"
  run bash "$SCRIPT" --dry-run
  [ "$status" -eq 1 ]
  [[ "$output" == *"settings not found"* ]]
  write_conf "tiny11"
  rm "$APP/Contents/MacOS/deskflow-vhid-bridge"
  run bash "$SCRIPT" --dry-run
  [ "$status" -eq 1 ]
  [[ "$output" == *"bridge binary not found"* ]]
}
