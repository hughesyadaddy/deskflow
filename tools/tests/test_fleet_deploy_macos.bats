#!/usr/bin/env bats
# scripts/fleet-deploy-macos.sh — "kill silent success" contract.
#
# Every external tool (cmake, codesign, git, security, brew, python3) is a PATH
# shim in a temp dir that appends its argv to $SHIM_LOG. The script itself is
# the real one; only its environment is faked.

SCRIPT="$BATS_TEST_DIRNAME/../../scripts/fleet-deploy-macos.sh"

BRIDGE_PLIST='<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>Label</key><string>org.deskflow.vhid-bridge</string></dict></plist>'

stub_bridge_renderer() {
  # Stand-in for scripts/install-login-bridge-macos.sh --dry-run: logs its
  # argv + env and prints $1 (a plist) on stdout.
  printf '#!/usr/bin/env bash\necho "install-login-bridge-macos.sh $* APP=${DESKFLOW_INSTALL_APP:-unset}" >> "$SHIM_LOG"\n[[ "$*" == *--dry-run* ]] || { echo "must be dry-run" >&2; exit 99; }\ncat <<'"'"'PL'"'"'\n%s\nPL\n' "$1" >"$FAKE_ROOT/scripts/install-login-bridge-macos.sh"
}


setup() {
  TMP="$(mktemp -d "${BATS_TEST_TMPDIR:-${TMPDIR:-/tmp}}/fleet-deploy.XXXXXX")"
  SHIMS="$TMP/bin"
  FAKE_ROOT="$TMP/deskflow"
  MOUSER="$TMP/Mouser"
  export SHIM_LOG="$TMP/calls.log"
  export SHIM_STATE="$TMP/state"
  mkdir -p "$SHIMS" "$FAKE_ROOT/scripts" "$MOUSER/.git" "$MOUSER/scripts" "$SHIM_STATE"
  : >"$SHIM_LOG"

  # The deploy script calls the repo's install script; stub it inside the fake
  # root. Its own assert-single is report-only: SHIM_INSTALL_ASSERT_PROBLEMS
  # makes the stub print them (and still exit 0, like the real script).
  cat >"$FAKE_ROOT/scripts/install-macos.sh" <<'STUB'
#!/usr/bin/env bash
echo "install-macos.sh $* MOUSER_RESTART=${MOUSER_RESTART:-unset}" >> "$SHIM_LOG"
if [[ -n "${SHIM_INSTALL_ASSERT_PROBLEMS:-}" ]]; then
  echo "deskflow-ctl assert-single: FAIL"
  printf '  %s\n' "$SHIM_INSTALL_ASSERT_PROBLEMS"
  echo "warning: assert-single reported problems (above); human steps remain -- the install continues" >&2
fi
STUB
  # ... and deskflow-ctl: retire (exit 2 tolerated) then the ONE fatal
  # assert-single at the very end of the deploy. SHIM_CTL_ASSERT_RC=1 fakes a
  # seat with human steps left; the stub prints them like the real ctl.
  cat >"$FAKE_ROOT/scripts/deskflow-ctl" <<'STUB'
#!/usr/bin/env bash
echo "deskflow-ctl $* APP=${DESKFLOW_INSTALL_APP:-unset}" >> "$SHIM_LOG"
case "$1" in
  retire)
    if [[ "${SHIM_CTL_RETIRE_RC:-0}" == 2 ]]; then
      echo "== deskflow-ctl: retire: root-owned retired files remain; run once as admin: =="
      echo '  sudo rm -f "/usr/local/bin/deskflow-prio-apply.sh"'
    fi
    exit "${SHIM_CTL_RETIRE_RC:-0}" ;;
  assert-single)
    if [[ "${SHIM_CTL_ASSERT_RC:-0}" != 0 ]]; then
      echo "deskflow-ctl assert-single: FAIL" >&2
      echo "  login-items audit FAIL: enabled BTM app record launches \"Deskflow\" beside the LaunchAgent: 2.io.github.hughesyadaddy.deskflow" >&2
      echo "  retired file present: /usr/local/bin/deskflow-prio-apply.sh (deskflow-ctl retire)" >&2
    fi
    exit "${SHIM_CTL_ASSERT_RC:-0}" ;;
  login-items)
    echo '    manual step: System Settings -> General -> Login Items & Extensions -> "Open at Login" -> remove "Deskflow"'
    exit 0 ;;
esac
STUB
  chmod +x "$FAKE_ROOT/scripts/deskflow-ctl"
  # ... and the login-bridge renderer (--dry-run); the installed plist is a tmp path.
  export DESKFLOW_LOGIN_BRIDGE_PLIST="$TMP/org.deskflow.vhid-bridge.plist"
  stub_bridge_renderer "$BRIDGE_PLIST"
  printf '%s\n' "$BRIDGE_PLIST" >"$DESKFLOW_LOGIN_BRIDGE_PLIST"
  : >"$MOUSER/scripts/build_macos_gui_session.py"
  # Mouser's settings dir + log live in the sandbox (the script refuses real
  # paths under bats). The settle wait is 0 s so the suite stays fast.
  export MOUSER_SETTINGS_DIR="$TMP/AppSupport/Mouser"
  export MOUSER_LOG="$TMP/Logs/Mouser/mouser.log"
  export FLEET_SETTINGS_SETTLE_S=0
  mkdir -p "$MOUSER_SETTINGS_DIR" "$(dirname "$MOUSER_LOG")"
  printf '[MouseHook] boot\n' >"$MOUSER_LOG"

  make_shim cmake <<'EOF'
echo "cmake $*" >> "$SHIM_LOG"
if [[ "${1:-}" == "-S" ]]; then
  id=""; strict=""
  for a in "$@"; do
    case "$a" in
      -DAPPLE_CODESIGN_DEV=*) id="${a#*=}" ;;
      -DFLEET_STRICT_SIGNING=*) strict="${a#*=}" ;;
    esac
  done
  mkdir -p build
  printf 'APPLE_CODESIGN_DEV:STRING=%s\nFLEET_STRICT_SIGNING:BOOL=%s\n' "$id" "$strict" > build/CMakeCache.txt
fi
exit "${SHIM_CMAKE_RC:-0}"
EOF

  make_shim codesign <<'EOF'
echo "codesign $*" >> "$SHIM_LOG"
exit "${SHIM_CODESIGN_VERIFY_RC:-0}"
EOF

  make_shim git <<'EOF'
echo "git $*" >> "$SHIM_LOG"
case "${1:-} ${2:-}" in
  "remote get-url") [[ -f "$SHIM_STATE/fork-exists" ]] && exit 0 || exit 1 ;;
  "remote add") touch "$SHIM_STATE/fork-exists"; exit 0 ;;
  "pull --ff-only") exit "${SHIM_GIT_PULL_RC:-0}" ;;
esac
exit 0
EOF

  # The identity must come from .env only; the fallback lookup is gone. The
  # only permitted verbs are the keychain preparation ones, and only when a
  # password is configured (SHIM_SECURITY_RC fakes a wrong password).
  make_shim security <<'EOF'
echo "security $1 $2 <redacted> $4 $5 $6 $7 $8" >> "$SHIM_LOG"
case "${1:-}" in
  unlock-keychain|set-key-partition-list) exit "${SHIM_SECURITY_RC:-0}" ;;
esac
echo "security $1 must never be called by fleet-deploy-macos.sh" >&2
exit 1
EOF

  make_shim brew <<'EOF'
echo "brew $*" >> "$SHIM_LOG"
[[ "${1:-}" == "--prefix" ]] && echo "/fake/opt/${2:-}"
exit 0
EOF

  # python3 stands in for both the Mouser build and tools/fleet-gui-exec.py.
  # For the latter it honours the documented interface: `-- cmd args...` is exec'd.
  # As the Mouser installer it appends what the freshly started Mouser logs
  # (SHIM_MOUSER_LOG_APPEND, default: the native-tap line) and, with
  # SHIM_PYTHON_WRITE_CONFIG, rewrites config.json the way a bad installer would.
  make_shim python3 <<'EOF'
echo "python3 $* MOUSER_RESTART=${MOUSER_RESTART:-unset}" >> "$SHIM_LOG"
if [[ "${1:-}" == *fleet-gui-exec.py ]]; then
  shift
  while [[ $# -gt 0 && "$1" != "--" ]]; do shift; done
  shift
  exec "$@"
fi
if [[ "${1:-}" == *build_macos_gui_session.py || "${1:-}" == *build_and_install.py ]]; then
  line="${SHIM_MOUSER_LOG_APPEND-[MouseHook] CGEventTap created (native tap: /tmp/x/Contents/Frameworks/mouser_tap.dylib)}"
  [[ -n "$line" ]] && printf '%s\n' "$line" >> "$MOUSER_LOG"
  [[ -n "${SHIM_PYTHON_WRITE_CONFIG:-}" ]] && cp "$SHIM_PYTHON_WRITE_CONFIG" "$MOUSER_SETTINGS_DIR/config.json"
fi
exit "${SHIM_PYTHON_RC:-0}"
EOF

  # sleep is only called by the settle wait / native-tap poll; SHIM_SLEEP_CONFIG
  # lets a test change config.json "while Mouser runs" (between checkpoints 2 and 3).
  make_shim sleep <<'EOF'
echo "sleep $*" >> "$SHIM_LOG"
[[ -n "${SHIM_SLEEP_CONFIG:-}" ]] && cp "$SHIM_SLEEP_CONFIG" "$MOUSER_SETTINGS_DIR/config.json"
[[ -n "${SHIM_SLEEP_DEVICE:-}" ]] && printf '%s' "$SHIM_SLEEP_DEVICE" > "$MOUSER_SETTINGS_DIR/last_device.json"
exit 0
EOF

  export PATH="$SHIMS:$PATH"
  export FLEET_DESKFLOW_ROOT="$FAKE_ROOT"
  export FLEET_MOUSER_ROOT="$MOUSER"
  unset FLEET_BRANCH FLEET_DEPLOY_MOUSER FLEET_DEPLOY_DESKFLOW FLEET_RECONFIGURE FLEET_SKIP_GIT_PULL
  unset DESKFLOW_CODESIGN_ID FLEET_KEYCHAIN_PASSWORD
  unset SHIM_CMAKE_RC SHIM_CODESIGN_VERIFY_RC SHIM_GIT_PULL_RC SHIM_PYTHON_RC SHIM_SECURITY_RC
  unset SHIM_CTL_RETIRE_RC SHIM_CTL_ASSERT_RC SHIM_INSTALL_ASSERT_PROBLEMS
  unset SHIM_MOUSER_LOG_APPEND SHIM_PYTHON_WRITE_CONFIG SHIM_SLEEP_CONFIG SHIM_SLEEP_DEVICE FLEET_NATIVE_TAP_TIMEOUT_S
}

# Mouser config fixtures (core/config.py shape, version 12).
CONFIG_V12='{"version": 12, "settings": {"start_at_login": true, "scroll": {"speed": 3}}, "buttons": ["a", "b"]}'
CONFIG_V13_SUPERSET='{"version": 13, "settings": {"start_at_login": true, "scroll": {"speed": 3}, "new_key": 1}, "buttons": ["a", "b"], "extra": {"x": 1}}'
CONFIG_V13_SHRUNK='{"version": 13, "settings": {"start_at_login": true}, "buttons": ["a", "b"]}'
CONFIG_V12_EXTRA='{"version": 12, "settings": {"start_at_login": true, "scroll": {"speed": 3}}, "buttons": ["a", "b"], "extra": 1}'

write_mouser_settings() {
  printf '%s' "$CONFIG_V12" >"$MOUSER_SETTINGS_DIR/config.json"
  printf '{"vid": 1133}' >"$MOUSER_SETTINGS_DIR/last_device.json"
}

fixture_file() { # name text -> path
  printf '%s' "$2" >"$TMP/$1"
  printf '%s' "$TMP/$1"
}

teardown() {
  rm -rf "$TMP"
}

make_shim() {
  { echo '#!/usr/bin/env bash'; cat; } >"$SHIMS/$1"
  chmod +x "$SHIMS/$1"
}

write_env() {
  printf 'DESKFLOW_CODESIGN_ID=%s\n' "$1" >"$FAKE_ROOT/.env"
}

write_env_with_password() {
  printf 'DESKFLOW_CODESIGN_ID=%s\nDESKFLOW_KEYCHAIN_PASSWORD=%s\n' "$1" "$2" >"$FAKE_ROOT/.env"
  chmod "${3:-600}" "$FAKE_ROOT/.env"
}

log_has() {
  if ! grep -qF -- "$1" "$SHIM_LOG"; then
    echo "expected call missing from shim log: $1" >&2
    return 1
  fi
}

# Explicit negations: bash's `set -e` ignores `! cmd`, so a bare `! log_has`
# mid-test would never fail. These return 1 so bats sees the failure.
log_lacks() {
  if grep -qF -- "$1" "$SHIM_LOG"; then
    echo "unexpected call in shim log: $1" >&2
    return 1
  fi
}

script_lacks() {
  if grep -qE -- "$1" "$SCRIPT"; then
    echo "unexpected pattern in script: $1" >&2
    return 1
  fi
}

# --- identity -----------------------------------------------------------------

@test "unset DESKFLOW_CODESIGN_ID (no .env) exits 1 before configuring" {
  run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"DESKFLOW_CODESIGN_ID"* ]]
  log_lacks "cmake"
  log_lacks "security"
}

@test "empty DESKFLOW_CODESIGN_ID in .env exits 1" {
  write_env ""
  run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"DESKFLOW_CODESIGN_ID"* ]]
  log_lacks "cmake"
}

@test "ad-hoc identity '-' exits 1" {
  write_env "-"
  run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"ad-hoc"* ]]
  log_lacks "cmake"
}

@test "security find-identity fallback is gone" {
  write_env "ABCDEF0123456789"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_lacks "security"
  # The identity hint in the error message may mention the command; the script
  # must never run it (no `$(security ...)` / `security ... | awk` lookup).
  script_lacks '\$\(security'
  script_lacks 'find-identity.*awk'
}

@test "the keychain password is read from the seat .env only, never from fleet.env" {
  script_lacks 'FLEET_KEYCHAIN_PASSWORD'
  # exactly one consumer: prepare_keychain_for_ssh reads it, then unsets it
  [ "$(grep -v '^[[:space:]]*#' "$SCRIPT" | grep -c 'DESKFLOW_KEYCHAIN_PASSWORD')" -le 4 ]
  grep -q 'unset DESKFLOW_KEYCHAIN_PASSWORD' "$SCRIPT"
}

@test "every remaining '|| true' is tagged fleet:allow" {
  run bash -c "grep -n '|| true' '$SCRIPT' | grep -v 'fleet:allow'"
  [ "$status" -ne 0 ]
  [ -z "$output" ]
}

# --- configure ----------------------------------------------------------------

@test "configure passes -DFLEET_STRICT_SIGNING=ON and -DAPPLE_CODESIGN_DEV from .env" {
  write_env "ABCDEF0123456789"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  local cfg
  cfg="$(grep '^cmake -S' "$SHIM_LOG")"
  [[ "$cfg" == *"-DFLEET_STRICT_SIGNING=ON"* ]]
  [[ "$cfg" == *"-DAPPLE_CODESIGN_DEV=ABCDEF0123456789"* ]]
  grep -q '^APPLE_CODESIGN_DEV:STRING=ABCDEF0123456789$' "$FAKE_ROOT/build/CMakeCache.txt"
}

@test "configure is skipped when the cache already matches identity and strict signing" {
  write_env "ABCDEF0123456789"
  mkdir -p "$FAKE_ROOT/build"
  printf 'APPLE_CODESIGN_DEV:STRING=ABCDEF0123456789\nFLEET_STRICT_SIGNING:BOOL=ON\n' >"$FAKE_ROOT/build/CMakeCache.txt"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_lacks "cmake -S"
  log_has "cmake --build build"
}

@test "configure re-runs when the cached APPLE_CODESIGN_DEV differs" {
  write_env "NEWID0000000000"
  mkdir -p "$FAKE_ROOT/build"
  printf 'APPLE_CODESIGN_DEV:STRING=OLDID0000000000\nFLEET_STRICT_SIGNING:BOOL=ON\n' >"$FAKE_ROOT/build/CMakeCache.txt"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"cache mismatch"* ]]
  grep '^cmake -S' "$SHIM_LOG" | grep -q -- '-DAPPLE_CODESIGN_DEV=NEWID0000000000'
}

@test "configure re-runs when the cached identity is ad-hoc '-'" {
  write_env "ABCDEF0123456789"
  mkdir -p "$FAKE_ROOT/build"
  printf 'APPLE_CODESIGN_DEV:STRING=-\nFLEET_STRICT_SIGNING:BOOL=ON\n' >"$FAKE_ROOT/build/CMakeCache.txt"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_has "cmake -S"
}

@test "configure re-runs when FLEET_STRICT_SIGNING is cached OFF" {
  write_env "ABCDEF0123456789"
  mkdir -p "$FAKE_ROOT/build"
  printf 'APPLE_CODESIGN_DEV:STRING=ABCDEF0123456789\nFLEET_STRICT_SIGNING:BOOL=OFF\n' >"$FAKE_ROOT/build/CMakeCache.txt"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"FLEET_STRICT_SIGNING"* ]]
  grep '^cmake -S' "$SHIM_LOG" | grep -q -- '-DFLEET_STRICT_SIGNING=ON'
}

@test "cmake configure failure propagates as a non-zero exit" {
  write_env "ABCDEF0123456789"
  SHIM_CMAKE_RC=3 run bash "$SCRIPT"
  [ "$status" -ne 0 ]
  log_lacks "install-macos.sh"
}

# --- verify -------------------------------------------------------------------

@test "codesign --verify failure after install exits 1" {
  write_env "ABCDEF0123456789"
  SHIM_CODESIGN_VERIFY_RC=1 run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"codesign --verify --deep --strict"* ]]
  log_has "codesign --verify --deep --strict /Applications/Deskflow.app"
  # Mouser must not deploy on top of a failed Deskflow verify.
  log_lacks "build_macos_gui_session.py"
}

# --- login bridge plist ---------------------------------------------------------

@test "the deploy ends with retire (exit 2 tolerated) then ONE fatal assert-single, after Mouser" {
  write_env "ABC123"
  export SHIM_CTL_RETIRE_RC=2
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"=== done:"* ]]
  log_has "deskflow-ctl retire APP=/Applications/Deskflow.app"
  log_has "deskflow-ctl assert-single APP=/Applications/Deskflow.app"
  install_line="$(grep -n 'install-macos.sh' "$SHIM_LOG" | head -1 | cut -d: -f1)"
  mouser_line="$(grep -n 'MOUSER_RESTART=1' "$SHIM_LOG" | head -1 | cut -d: -f1)"
  retire_line="$(grep -n 'deskflow-ctl retire' "$SHIM_LOG" | cut -d: -f1)"
  assert_line="$(grep -n 'deskflow-ctl assert-single' "$SHIM_LOG" | cut -d: -f1)"
  [ -n "$mouser_line" ]
  [ "$install_line" -lt "$mouser_line" ] && [ "$mouser_line" -lt "$retire_line" ] && [ "$retire_line" -lt "$assert_line" ]
  [ "$(grep -c 'deskflow-ctl assert-single' "$SHIM_LOG")" = 1 ]
}

@test "a seat with human steps left is still FULLY deployed (Mouser included) before the final assert-single exits non-zero with a HUMAN STEP REQUIRED block" {
  write_env "ABC123"
  # install-macos.sh's own assert-single only reports; the deploy goes on.
  export SHIM_INSTALL_ASSERT_PROBLEMS="login-items audit FAIL: enabled BTM app record" SHIM_CTL_RETIRE_RC=2 SHIM_CTL_ASSERT_RC=1
  run bash "$SCRIPT"
  [ "$status" -ne 0 ]
  # everything after install still ran: codesign verify, bridge plist, Mouser
  log_has "codesign --verify --deep --strict /Applications/Deskflow.app"
  log_has "install-login-bridge-macos.sh --dry-run"
  log_has "MOUSER_RESTART=1"
  log_has "deskflow-ctl retire"
  log_has "deskflow-ctl assert-single"
  mouser_line="$(grep -n 'MOUSER_RESTART=1' "$SHIM_LOG" | head -1 | cut -d: -f1)"
  assert_line="$(grep -n 'deskflow-ctl assert-single' "$SHIM_LOG" | cut -d: -f1)"
  [ "$mouser_line" -lt "$assert_line" ]
  # the report-only problems from install were printed, not fatal there
  [[ "$output" == *"warning: assert-single reported problems"* ]]
  # the final block lists each problem with its exact command / UI path
  [[ "$output" == *"HUMAN STEP REQUIRED"* ]]
  [[ "$output" == *"the seat IS deployed"* ]]
  [[ "$output" == *"- login-items audit FAIL: enabled BTM app record"* ]]
  [[ "$output" == *"- retired file present: /usr/local/bin/deskflow-prio-apply.sh"* ]]
  [[ "$output" == *'sudo rm -f "/usr/local/bin/deskflow-prio-apply.sh"'* ]]
  [[ "$output" == *'System Settings -> General -> Login Items & Extensions -> "Open at Login" -> remove "Deskflow"'* ]]
  [[ "$output" == *"assert-single failed on"* ]]
  [[ "$output" != *"=== done:"* ]]
  # a deploy never escalates
  log_lacks "sudo"
}

@test "after install the bridge plist is rendered via --dry-run, linted, and compared: up to date is reported" {
  write_env "ABCDEF0123456789"
  stub_bridge_renderer "$BRIDGE_PLIST"
  export DESKFLOW_LOGIN_BRIDGE_PLIST="$TMP/org.deskflow.vhid-bridge.plist"
  printf '%s\n' "$BRIDGE_PLIST" >"$DESKFLOW_LOGIN_BRIDGE_PLIST"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_has "install-login-bridge-macos.sh --dry-run APP=/Applications/Deskflow.app"
  [[ "$output" == *"bridge plist up to date"* ]]
  [[ "$output" != *"bridge plist stale"* ]]
  # rendered after the codesign verify of the installed bundle
  verify_line="$(grep -n '^codesign --verify' "$SHIM_LOG" | cut -d: -f1)"
  render_line="$(grep -n '^install-login-bridge-macos.sh' "$SHIM_LOG" | cut -d: -f1)"
  [ "$verify_line" -lt "$render_line" ]
  # this script never escalates (sudo only ever appears inside the printed hint)
  script_lacks '^[[:space:]]*sudo[[:space:]]'
  script_lacks '(\||&&|;)[[:space:]]*sudo[[:space:]]'
  [ "$(cat "$DESKFLOW_LOGIN_BRIDGE_PLIST")" = "$BRIDGE_PLIST" ]
}

@test "a stale or missing bridge plist prints the root step (no sudo here) and the deploy still succeeds" {
  write_env "ABCDEF0123456789"
  stub_bridge_renderer "$BRIDGE_PLIST"
  export DESKFLOW_LOGIN_BRIDGE_PLIST="$TMP/org.deskflow.vhid-bridge.plist"
  echo "<plist>old</plist>" >"$DESKFLOW_LOGIN_BRIDGE_PLIST"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"bridge plist stale — run root step: sudo env DESKFLOW_INSTALL_APP=/Applications/Deskflow.app bash $FAKE_ROOT/scripts/install-login-bridge-macos.sh"* ]]
  [ "$(cat "$DESKFLOW_LOGIN_BRIDGE_PLIST")" = "<plist>old</plist>" ]
  log_has "build_macos_gui_session.py"

  rm "$DESKFLOW_LOGIN_BRIDGE_PLIST"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"bridge plist stale"* ]]
}

@test "a bridge plist that does not lint fails the deploy; a renderer that cannot run (no peers) is reported, not fatal" {
  write_env "ABCDEF0123456789"
  stub_bridge_renderer "<plist>not xml"
  run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"does not lint"* ]]

  printf '#!/usr/bin/env bash\necho "error: no coordination peers configured (excluding macbookpro)" >&2; exit 1\n' >"$FAKE_ROOT/scripts/install-login-bridge-macos.sh"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"bridge plist: not rendered (error: no coordination peers configured"* ]]
}

@test "a renderer failing for any other reason (missing bridge binary, bad config) fails the deploy with the reason" {
  write_env "ABCDEF0123456789"
  printf '#!/usr/bin/env bash\necho "error: bridge binary not found at /Applications/Deskflow.app/Contents/MacOS/deskflow-vhid-bridge" >&2; exit 1\n' >"$FAKE_ROOT/scripts/install-login-bridge-macos.sh"
  run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"--dry-run failed: error: bridge binary not found"* ]]
  log_lacks "build_macos_gui_session.py"
}

# --- GUI-session exec ---------------------------------------------------------

@test "build and install go direct when tools/fleet-gui-exec.py is absent" {
  write_env "ABCDEF0123456789"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_lacks "fleet-gui-exec.py"
  log_has "cmake --build build --target deskflow-core Deskflow deskflow-vhid-bridge"
  log_has "install-macos.sh"
}

@test "build and install are routed through tools/fleet-gui-exec.py when it exists" {
  write_env "ABCDEF0123456789"
  mkdir -p "$FAKE_ROOT/tools"
  printf '#!/usr/bin/env python3\n' >"$FAKE_ROOT/tools/fleet-gui-exec.py"
  chmod +x "$FAKE_ROOT/tools/fleet-gui-exec.py"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_has "python3 $FAKE_ROOT/tools/fleet-gui-exec.py -- cmake --build build"
  log_has "python3 $FAKE_ROOT/tools/fleet-gui-exec.py -- bash scripts/install-macos.sh"
  # and the wrapped commands actually ran
  log_has "cmake --build build --target"
  log_has "install-macos.sh"
}

@test "a non-executable tools/fleet-gui-exec.py is ignored" {
  write_env "ABCDEF0123456789"
  mkdir -p "$FAKE_ROOT/tools"
  : >"$FAKE_ROOT/tools/fleet-gui-exec.py"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_lacks "fleet-gui-exec.py"
}

# --- Mouser -------------------------------------------------------------------

@test "order is deskflow then Mouser, and MOUSER_RESTART=1 reaches only the Mouser step" {
  write_env "ABCDEF0123456789"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_has "install-macos.sh  MOUSER_RESTART=unset"
  log_has "python3 scripts/build_macos_gui_session.py MOUSER_RESTART=1"
  install_line="$(grep -n 'install-macos.sh' "$SHIM_LOG" | head -1 | cut -d: -f1)"
  mouser_line="$(grep -n 'build_macos_gui_session.py' "$SHIM_LOG" | head -1 | cut -d: -f1)"
  [ "$install_line" -lt "$mouser_line" ]
  # the deploy itself never stops/starts Mouser; the Mouser installer does
  script_lacks 'pkill|killall|Mouser.app|osascript'
}

@test "Mouser pull adds the fork remote when missing, then fetch/checkout/pull --ff-only fork BRANCH" {
  write_env "ABCDEF0123456789"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_has "git remote add fork https://github.com/hughesyadaddy/Mouser.git"
  log_has "git fetch fork"
  log_has "git checkout main"
  log_has "git pull --ff-only fork main"
  log_has "python3 scripts/build_macos_gui_session.py"
  log_lacks "build_and_install.py"
}

@test "Mouser pull honours FLEET_BRANCH and skips remote add when fork exists" {
  write_env "ABCDEF0123456789"
  touch "$SHIM_STATE/fork-exists"
  FLEET_BRANCH=feat/x run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_lacks "git remote add"
  log_has "git fetch fork"
  log_has "git checkout feat/x"
  log_has "git pull --ff-only fork feat/x"
}

@test "Mouser pull --ff-only failure exits non-zero (no '|| true')" {
  write_env "ABCDEF0123456789"
  SHIM_GIT_PULL_RC=1 run bash "$SCRIPT"
  [ "$status" -ne 0 ]
  log_lacks "build_macos_gui_session.py"
  script_lacks 'git pull --ff-only.*\|\| true'
}

@test "Mouser build failure exits non-zero" {
  write_env "ABCDEF0123456789"
  SHIM_PYTHON_RC=2 run bash "$SCRIPT"
  [ "$status" -ne 0 ]
  [[ "$output" != *"=== done"* ]]
}

@test "missing Mouser checkout is an error, not a silent skip" {
  write_env "ABCDEF0123456789"
  rm -rf "$MOUSER"
  run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"Mouser checkout missing"* ]]
}

@test "FLEET_DEPLOY_MOUSER=0 skips Mouser entirely" {
  write_env "ABCDEF0123456789"
  FLEET_DEPLOY_MOUSER=0 run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_lacks "git fetch fork"
  log_lacks "build_macos_gui_session.py"
}

@test "FLEET_SKIP_GIT_PULL=1 moves neither checkout (the controller already synced both)" {
  write_env "ABCDEF0123456789"
  FLEET_SKIP_GIT_PULL=1 FLEET_MOUSER_REF=v9 FLEET_DESKFLOW_REF=v9 run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_lacks "git fetch"
  log_lacks "git checkout"
  log_lacks "git pull"
  log_has "python3 scripts/build_macos_gui_session.py"
}

@test "standalone run honours FLEET_MOUSER_BRANCH and FLEET_*_REF (detach, HEAD = no sync)" {
  write_env "ABCDEF0123456789"
  FLEET_MOUSER_BRANCH=mouser-dev run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_has "git checkout main"
  log_has "git checkout mouser-dev"
  log_has "git pull --ff-only fork mouser-dev"
  log_lacks "git pull --ff-only fork main"
  : > "$SHIM_LOG"
  FLEET_DESKFLOW_REF=abc123 FLEET_MOUSER_REF=def456 run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_has "git checkout --detach abc123"
  log_has "git checkout --detach def456"
  log_lacks "git pull"
  : > "$SHIM_LOG"
  FLEET_DESKFLOW_REF=HEAD FLEET_MOUSER_REF=HEAD run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_lacks "git fetch"
  log_lacks "git checkout"
  log_has "python3 scripts/build_macos_gui_session.py"
}

# --- L4: Mouser settings-survival proof --------------------------------------

@test "settings hashes identical at all three checkpoints: FLEET_SETTINGS=ok, a pre-deploy backup is kept, native tap seen" {
  write_env "ABCDEF0123456789"
  write_mouser_settings
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"Mouser settings snapshot (pre-deploy): config.json="*"last_device.json="*"backup=$MOUSER_SETTINGS_DIR/config.json.pre-deploy-"* ]]
  [[ "$output" == *"Mouser settings identical after install"* ]]
  [[ "$output" == *"Mouser native tap: [MouseHook] CGEventTap created (native tap:"* ]]
  [[ "$output" == *"FLEET_SETTINGS=ok"* ]]
  [[ "$output" != *"FLEET_SETTINGS=FAIL"* ]]
  backups=("$MOUSER_SETTINGS_DIR"/config.json.pre-deploy-*)
  [ "${#backups[@]}" -eq 1 ]
  cmp -s "${backups[0]}" "$MOUSER_SETTINGS_DIR/config.json"
  # the live files are untouched
  [ "$(cat "$MOUSER_SETTINGS_DIR/config.json")" = "$CONFIG_V12" ]
  # snapshot happens before the build, verdict after the settle wait
  snap_line="$(grep -n 'settings snapshot' <<<"$output" | head -1 | cut -d: -f1)"
  build_line="$(grep -n 'Mouser build + install' <<<"$output" | head -1 | cut -d: -f1)"
  [ "$snap_line" -lt "$build_line" ]
  log_has "sleep 0"
}

@test "absent settings files are a valid (identical) snapshot" {
  write_env "ABCDEF0123456789"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"config.json=absent last_device.json=absent backup=none"* ]]
  [[ "$output" == *"FLEET_SETTINGS=ok"* ]]
}

@test "a version bump that keeps every pre-deploy key passes as FLEET_SETTINGS=changed" {
  write_env "ABCDEF0123456789"
  write_mouser_settings
  SHIM_SLEEP_CONFIG="$(fixture_file v13.json "$CONFIG_V13_SUPERSET")" run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"Mouser config.json migrated (version increased, pre-deploy keys preserved): config.json "* ]]
  [[ "$output" == *"FLEET_SETTINGS=changed"* ]]
}

@test "a shrunk config after the run fails with a diff summary and FLEET_SETTINGS=FAIL" {
  write_env "ABCDEF0123456789"
  write_mouser_settings
  SHIM_SLEEP_CONFIG="$(fixture_file shrunk.json "$CONFIG_V13_SHRUNK")" run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"FLEET_SETTINGS=FAIL"* ]]
  [[ "$output" == *"not an allowed migration"* ]]
  [[ "$output" == *"config.json "*" -> "* ]]
  [[ "$output" == *"restore from $MOUSER_SETTINGS_DIR/config.json.pre-deploy-"* ]]
  [[ "$output" != *"=== done"* ]]
}

@test "a config rewritten without a version increase fails even when nothing was lost" {
  write_env "ABCDEF0123456789"
  write_mouser_settings
  SHIM_SLEEP_CONFIG="$(fixture_file extra.json "$CONFIG_V12_EXTRA")" run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"FLEET_SETTINGS=FAIL"* ]]
}

@test "a config.json touched by the install itself fails before the native-tap gate" {
  write_env "ABCDEF0123456789"
  write_mouser_settings
  SHIM_PYTHON_WRITE_CONFIG="$(fixture_file v13.json "$CONFIG_V13_SUPERSET")" run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"FLEET_SETTINGS=FAIL"* ]]
  [[ "$output" == *"Mouser settings changed by the install: config.json "* ]]
  [[ "$output" != *"Mouser native tap:"* ]]
  [[ "$output" != *"identical after install"* ]]
}

@test "a rewritten last_device.json (HID warm-path cache) after the run is reported, not judged" {
  write_env "ABCDEF0123456789"
  write_mouser_settings
  SHIM_SLEEP_DEVICE='{"vid": 1133, "pid": 45}' run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"Mouser cache rewritten while running (not judged): last_device.json "* ]]
  [[ "$output" == *"FLEET_SETTINGS=ok"* ]]
}

@test "pre-deploy backups are pruned to the newest 5" {
  write_env "ABCDEF0123456789"
  write_mouser_settings
  for i in 1 2 3 4 5 6; do printf 'old' >"$MOUSER_SETTINGS_DIR/config.json.pre-deploy-2026090${i}-000000"; done
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  backups=($(ls "$MOUSER_SETTINGS_DIR"/config.json.pre-deploy-* | sort))
  [ "${#backups[@]}" -eq 5 ]
  [[ "${backups[0]}" == *pre-deploy-20260903-000000 ]]
  [[ "${backups[4]}" == *pre-deploy-2026$(date +%m%d)-* ]]
  cmp -s "${backups[4]}" "$MOUSER_SETTINGS_DIR/config.json"
  [ -f "$MOUSER_SETTINGS_DIR/last_device.json" ]
}

# --- M4: native-tap deploy gate ------------------------------------------------

@test "native-tap gate fails when Mouser never logs the native tap within the timeout" {
  write_env "ABCDEF0123456789"
  SHIM_MOUSER_LOG_APPEND="" FLEET_NATIVE_TAP_TIMEOUT_S=0 run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"did not log 'CGEventTap created (native tap:' within 0s"* ]]
  [[ "$output" != *"FLEET_SETTINGS="* ]]
}

@test "native-tap gate fails when Mouser comes up on the PyObjC tap (own run loop)" {
  write_env "ABCDEF0123456789"
  SHIM_MOUSER_LOG_APPEND="[MouseHook] CGEventTap enabled on its own run loop" run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"Mouser came up on the PyObjC tap"* ]]
  [[ "$output" != *"FLEET_SETTINGS="* ]]
}

@test "native-tap gate only reads the log past the pre-install offset" {
  write_env "ABCDEF0123456789"
  printf '[MouseHook] CGEventTap created (native tap: /old)\n' >>"$MOUSER_LOG"
  SHIM_MOUSER_LOG_APPEND="" FLEET_NATIVE_TAP_TIMEOUT_S=0 run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"did not log"* ]]
}

@test "native-tap gate tolerates a log rotated during the install" {
  write_env "ABCDEF0123456789"
  head -c 4096 /dev/zero | tr '\0' 'x' >"$MOUSER_LOG"
  # the installer shim appends to a fresh (smaller) log
  rm -f "$MOUSER_LOG"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"Mouser native tap:"* ]]
}

@test "refuses to run under bats when MOUSER_SETTINGS_DIR or MOUSER_LOG point outside the sandbox" {
  write_env "ABCDEF0123456789"
  MOUSER_SETTINGS_DIR="/Library/Application Support/DeskflowGuardRegressionTest" run bash "$SCRIPT"
  [ "$status" -eq 90 ]
  [[ "$output" == *"FATAL: running under bats"* ]]
  MOUSER_LOG="/Library/Logs/DeskflowGuardRegressionTest/mouser.log" run bash "$SCRIPT"
  [ "$status" -eq 90 ]
  [ ! -e "/Library/Application Support/DeskflowGuardRegressionTest" ]
}

@test "refuses to run under bats against a non-sandboxed FLEET_MOUSER_ROOT (2026-09-16 incident regression)" {
  # FLEET_MOUSER_ROOT defaults to the real ~/Desktop/Mouser when unset; this
  # proves the independent hard guard catches ANY non-tmp override under
  # bats before any git/build/install step runs, regardless of whether the
  # override itself was set correctly.
  export FLEET_MOUSER_ROOT="/Library/Application Support/DeskflowGuardRegressionTest/Mouser"
  run bash "$SCRIPT"
  [ "$status" -eq 90 ]
  [[ "$output" == *"FATAL: running under bats"* ]]
  [[ "$output" == *"is not inside a tmp sandbox"* ]]
  [ ! -e "/Library/Application Support/DeskflowGuardRegressionTest" ]
  [ ! -s "$SHIM_LOG" ]
}

# --- keychain password: sign from this (SSH) session, never via the GUI -------

gui_runner_present() {
  mkdir -p "$FAKE_ROOT/tools"; : > "$FAKE_ROOT/tools/fleet-gui-exec.py"; chmod +x "$FAKE_ROOT/tools/fleet-gui-exec.py"
}

@test "with DESKFLOW_KEYCHAIN_PASSWORD the seat prepares the keychain, proves codesign, and never routes through the GUI" {
  write_env_with_password "ABCDEF0123456789" "s3cret-pw"
  gui_runner_present
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_has "security unlock-keychain -p <redacted>"
  log_has "security set-key-partition-list -S <redacted>"
  grep -q '^codesign --force --sign ABCDEF0123456789 .*deskflow-sign-probe\.' "$SHIM_LOG"
  log_lacks "fleet-gui-exec.py"
  log_has "python3 scripts/build_and_install.py"
  log_lacks "build_macos_gui_session.py"
  [[ "$output" != *"s3cret-pw"* ]]
  [[ "$output" == *"codesign verified from this session"* ]]
  prep_line="$(grep -n '^security unlock-keychain' "$SHIM_LOG" | cut -d: -f1)"
  first_cmake="$(grep -n '^cmake' "$SHIM_LOG" | head -1 | cut -d: -f1)"
  [ "$prep_line" -lt "$first_cmake" ]
}

@test "the password never reaches child processes" {
  write_env_with_password "ABCDEF0123456789" "s3cret-pw"
  make_shim cmake <<'EOF'
echo "cmake $*" >> "$SHIM_LOG"
[[ -n "${DESKFLOW_KEYCHAIN_PASSWORD:-}" ]] && echo "LEAK: cmake saw the keychain password" >> "$SHIM_LOG"
if [[ "${1:-}" == "-S" ]]; then mkdir -p build; printf 'APPLE_CODESIGN_DEV:STRING=ABCDEF0123456789\nFLEET_STRICT_SIGNING:BOOL=ON\n' > build/CMakeCache.txt; fi
exit 0
EOF
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_lacks "LEAK"
}

@test "a .env holding the password must be mode 600, checked before any keychain call" {
  write_env_with_password "ABCDEF0123456789" "s3cret-pw" 644
  run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"mode 644"* ]]
  [[ "$output" == *"chmod 600"* ]]
  log_lacks "security"
  log_lacks "cmake"
}

@test "a wrong keychain password fails loudly before building; no GUI fallback" {
  write_env_with_password "ABCDEF0123456789" "wrong-pw"
  gui_runner_present
  SHIM_SECURITY_RC=51 run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"unlock-keychain failed"* ]]
  [[ "$output" != *"wrong-pw"* ]]
  log_lacks "cmake"
  log_lacks "fleet-gui-exec.py"
}

@test "a failed codesign probe after preparation fails loudly; no GUI fallback" {
  write_env_with_password "ABCDEF0123456789" "s3cret-pw"
  gui_runner_present
  make_shim codesign <<'EOF'
echo "codesign $*" >> "$SHIM_LOG"
[[ "$*" == *deskflow-sign-probe* ]] && exit 1
exit 0
EOF
  run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"codesign probe"* ]]
  log_lacks "cmake"
  log_lacks "fleet-gui-exec.py"
}

@test "without a password the GUI-session routing is unchanged" {
  write_env "ABCDEF0123456789"
  gui_runner_present
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  log_lacks "security"
  log_has "fleet-gui-exec.py"
  log_has "build_macos_gui_session.py"
}
