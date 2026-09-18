#!/usr/bin/env bats
# scripts/install-macos.sh — the signature check is fatal, never a warning,
# and the process lifecycle goes through scripts/deskflow-ctl (launchctl).
#
# The real script runs against a temp install path with cmake/codesign/
# launchctl/ps/xattr/sleep replaced by PATH shims that log to $SHIM_LOG.
# `codesign -dvvv` output is controlled per test via $SHIM_CODESIGN_DV.

SCRIPT="$BATS_TEST_DIRNAME/../../scripts/install-macos.sh"

SIGNED_DV='Executable=/Applications/Deskflow.app/Contents/MacOS/deskflow-core
Identifier=org.deskflow.deskflow-core
Format=Mach-O thin (arm64)
CodeDirectory v=20500 size=1234 flags=0x10000(runtime) hashes=30+7 location=embedded
Signature size=4795
Authority=Apple Development: Alex Hughes (ABCDE12345)
Authority=Apple Worldwide Developer Relations Certification Authority
Authority=Apple Root CA
TeamIdentifier=ABCDE12345
Sealed Resources=none'

ADHOC_DV='Executable=/Applications/Deskflow.app/Contents/MacOS/deskflow-core
Identifier=deskflow-core
Format=Mach-O thin (arm64)
CodeDirectory v=20400 size=1234 flags=0x2(adhoc) hashes=30+7 location=embedded
Signature=adhoc
Info.plist=not bound
TeamIdentifier=not set
Sealed Resources=none'

NO_TEAM_DV='Executable=/Applications/Deskflow.app/Contents/MacOS/deskflow-core
Identifier=deskflow-core
Authority=Some Self-Signed Cert
TeamIdentifier=not set'

setup() {
  TMP="$(mktemp -d "${BATS_TEST_TMPDIR:-${TMPDIR:-/tmp}}/install-macos.XXXXXX")"
  SHIMS="$TMP/bin"
  BUILD="$TMP/build"
  APP="$TMP/Applications/Deskflow.app"
  export SHIM_LOG="$TMP/calls.log"
  export SHIM_STATE="$TMP/state"
  export HOME="$TMP/home"
  export DESKFLOW_CTL_AGENT_DIR="$TMP/LaunchAgents"
  export DESKFLOW_CTL_DAEMON_DIR="$TMP/LaunchDaemons"
  export DESKFLOW_CTL_DAEMON_STAGE_DIR="$TMP/stage"
  mkdir -p "$SHIMS" "$BUILD" "$TMP/Applications" "$SHIM_STATE" "$HOME"
  : >"$SHIM_LOG"
  # Presence of cmake_install.cmake selects the staged `cmake --install` path.
  : >"$BUILD/cmake_install.cmake"

  make_shim cmake <<'EOF'
echo "cmake $*" >> "$SHIM_LOG"
if [[ "${1:-}" == "--install" ]]; then
  prefix=""
  while [[ $# -gt 0 ]]; do
    [[ "$1" == "--prefix" ]] && prefix="$2"
    shift
  done
  mkdir -p "$prefix/Deskflow.app/Contents/MacOS" "$prefix/Deskflow.app/Contents/Resources"
  : > "$prefix/Deskflow.app/Contents/MacOS/deskflow-core"
  chmod +x "$prefix/Deskflow.app/Contents/MacOS/deskflow-core"
  # CMake bundles the login-bridge installer as a Resource before signing.
  [[ -n "${SHIM_OMIT_BRIDGE:-}" ]] || : > "$prefix/Deskflow.app/Contents/Resources/install-login-bridge-macos.sh"
  # deskflow-prio ships in the bundle (root LaunchDaemon, installed by deskflow-ctl prio).
  : > "$prefix/Deskflow.app/Contents/MacOS/deskflow-prio"
  chmod +x "$prefix/Deskflow.app/Contents/MacOS/deskflow-prio"
fi
exit 0
EOF

  make_shim codesign <<'EOF'
echo "codesign $*" >> "$SHIM_LOG"
case "${1:-}" in
  --verify) exit "${SHIM_CODESIGN_VERIFY_RC:-0}" ;;
  -dvvv)
    # Real codesign prints -dvvv details to stderr. Plain -dv never prints
    # Authority= lines regardless of signature, which is why the real
    # script uses -dvvv; the shim only answers that flag.
    printf '%s\n' "${SHIM_CODESIGN_DV:-}" >&2
    exit "${SHIM_CODESIGN_DV_RC:-0}"
    ;;
esac
exit 0
EOF

  # Any legacy process tool must never be reached; fail loudly if it is.
  for tool in osascript pkill pgrep open; do
    make_shim "$tool" <<EOF
echo "$tool \$*" >> "\$SHIM_LOG"
echo "$tool must not be called by install-macos.sh" >&2
exit 99
EOF
  done
  make_shim xattr <<'EOF'
echo "xattr $*" >> "$SHIM_LOG"
exit 0
EOF
  # Fake launchd: nothing loaded; bootstrap of the core reports a pid so
  # deskflow-ctl start succeeds.
  make_shim launchctl <<'EOF'
echo "launchctl $*" >> "$SHIM_LOG"
case "${1:-}" in
  print)
    [[ -f "$SHIM_STATE/loaded-${2##*/}" ]] || exit 113
    echo "	pid = 4242"
    ;;
  bootstrap) touch "$SHIM_STATE/loaded-$(basename "$3" .plist)" ;;
esac
exit 0
EOF
  # Nothing is running.
  make_shim ps <<'EOF'
echo "ps $*" >> "$SHIM_LOG"
exit 0
EOF
  # Keep the suite fast.
  make_shim sleep <<'EOF'
exit 0
EOF

  export PATH="$SHIMS:$PATH"
  export DESKFLOW_BUILD_DIR="$BUILD"
  export DESKFLOW_INSTALL_APP="$APP"
  export SHIM_CODESIGN_DV="$SIGNED_DV"
  unset SHIM_CODESIGN_VERIFY_RC SHIM_CODESIGN_DV_RC SHIM_OMIT_BRIDGE
}

teardown() {
  rm -rf "$TMP"
}

make_shim() {
  { echo '#!/usr/bin/env bash'; cat; } >"$SHIMS/$1"
  chmod +x "$SHIMS/$1"
}

log_has() {
  if ! grep -qF -- "$1" "$SHIM_LOG"; then
    echo "expected call missing from shim log: $1" >&2
    return 1
  fi
}

log_lacks() {
  if grep -qF -- "$1" "$SHIM_LOG"; then
    echo "unexpected call in shim log: $1" >&2
    return 1
  fi
}

@test "signed bundle installs, verifies, and restarts through launchd (verify staged -> ctl stop -> swap -> ctl start)" {
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"Codesign verify OK"* ]]
  [[ "$output" == *"Authority=Apple Development"* ]]
  [[ "$output" == *"TeamIdentifier=ABCDE12345"* ]]
  [ -f "$APP/Contents/MacOS/deskflow-core" ]
  # The signature is checked on the STAGED bundle, never on the live path.
  log_has "codesign --verify --deep --strict "
  log_lacks "codesign --verify --deep --strict $APP"
  log_lacks "codesign -dvvv $APP/Contents/MacOS/deskflow-core"
  grep -q '^codesign -dvvv .*/deskflow-install\..*/Deskflow.app/Contents/MacOS/deskflow-core$' "$SHIM_LOG"
  log_has "launchctl bootstrap gui/$(id -u) $DESKFLOW_CTL_AGENT_DIR/io.github.hughesyadaddy.deskflow-core.plist"
  log_has "launchctl bootstrap gui/$(id -u) $DESKFLOW_CTL_AGENT_DIR/io.github.hughesyadaddy.deskflow.plist"
  # verify happens BEFORE stop (ps inventory), start after the swap
  verify_line="$(grep -n '^codesign --verify' "$SHIM_LOG" | cut -d: -f1)"
  stop_line="$(grep -n '^ps ' "$SHIM_LOG" | head -1 | cut -d: -f1)"
  start_line="$(grep -n 'launchctl bootstrap' "$SHIM_LOG" | head -1 | cut -d: -f1)"
  [ "$verify_line" -lt "$stop_line" ]
  [ "$stop_line" -lt "$start_line" ]
  log_lacks "open "
  log_lacks "pkill"
  log_lacks "osascript"
  # the deploy lock and the stage dir are gone once the install finished
  [ ! -e "$HOME/Library/Deskflow/deploy.lock" ]
  stage="$(grep -o 'deskflow-install\.[A-Za-z0-9]*' "$SHIM_LOG" | head -1)"
  [ -n "$stage" ]
  [ ! -e "${TMPDIR:-/tmp}/$stage" ]
}

@test "a rejected build never touches the running seat: no bootout, no .bak, live bundle intact" {
  mkdir -p "$APP/Contents/MacOS"; echo live >"$APP/Contents/MacOS/deskflow-core"
  SHIM_CODESIGN_VERIFY_RC=1 run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  log_lacks "launchctl bootout"
  log_lacks "ps "
  [ ! -e "$APP.bak" ]
  [ "$(cat "$APP/Contents/MacOS/deskflow-core")" = "live" ]
  [[ "$output" != *"Stopping Deskflow"* ]]
  [[ "$output" != *"Installing to"* ]]
  [ ! -e "$HOME/Library/Deskflow/deploy.lock" ]
}

@test "the deploy lock (pid) exists from before ctl stop until after ctl start, then is removed" {
  # ps is called by ctl stop; launchctl bootstrap by ctl start. Record the
  # lock state at both moments.
  make_shim ps <<'EOF'
echo "ps $*" >> "$SHIM_LOG"
[[ -f "$HOME/Library/Deskflow/deploy.lock" ]] && echo "lock-at-stop pid=$(cat "$HOME/Library/Deskflow/deploy.lock")" >> "$SHIM_LOG"
exit 0
EOF
  make_shim launchctl <<'EOF'
echo "launchctl $*" >> "$SHIM_LOG"
case "${1:-}" in
  print) [[ -f "$SHIM_STATE/loaded-${2##*/}" ]] || exit 113; echo "	pid = 4242" ;;
  bootstrap) touch "$SHIM_STATE/loaded-$(basename "$3" .plist)"; [[ -f "$HOME/Library/Deskflow/deploy.lock" ]] && echo "lock-at-start" >> "$SHIM_LOG" ;;
esac
exit 0
EOF
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  grep -qE '^lock-at-stop pid=[0-9]+$' "$SHIM_LOG"
  log_has "lock-at-start"
  [ ! -e "$HOME/Library/Deskflow/deploy.lock" ]
}

@test "no pkill/pgrep/open/osascript and no Mouser reference remain in the script" {
  run grep -nE '^[^#]*[[:space:]](pkill|pgrep|killall|osascript|open)[[:space:]]' "$SCRIPT"
  [ "$status" -ne 0 ]
  run grep -ni 'mouser' "$SCRIPT"
  [ "$status" -ne 0 ]
  run grep -nE 'restart_mouser|copy_login_bridge_script' "$SCRIPT"
  [ "$status" -ne 0 ]
}

@test "nothing is copied into the bundle after signature verification (login bridge must be bundled by CMake)" {
  SHIM_OMIT_BRIDGE=1 run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"install-login-bridge-macos.sh missing"* ]]
  log_lacks "launchctl bootstrap"
  run grep -n 'install -m 755' "$SCRIPT"
  [ "$status" -ne 0 ]
}

@test "codesign --verify failure exits 1 and does not relaunch" {
  SHIM_CODESIGN_VERIFY_RC=1 run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"codesign --verify --deep --strict failed"* ]]
  [[ "$output" != *"Codesign verify OK"* ]]
  [[ "$output" != *"Installed unsigned"* ]]
  log_lacks "launchctl bootstrap"
}

@test "ad-hoc signature (no Authority) exits 1" {
  SHIM_CODESIGN_DV="$ADHOC_DV" run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"no Authority="* ]]
  log_lacks "launchctl bootstrap"
}

@test "Authority without a TeamIdentifier exits 1" {
  SHIM_CODESIGN_DV="$NO_TEAM_DV" run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"no TeamIdentifier="* ]]
  log_lacks "launchctl bootstrap"
}

@test "codesign -dvvv itself failing exits 1" {
  SHIM_CODESIGN_DV="" SHIM_CODESIGN_DV_RC=1 run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"codesign -dvvv failed"* ]]
}

@test "--no-restart still enforces the signature check" {
  SHIM_CODESIGN_VERIFY_RC=1 run bash "$SCRIPT" --no-restart
  [ "$status" -eq 1 ]
  run bash "$SCRIPT" --no-restart
  [ "$status" -eq 0 ]
  log_lacks "launchctl bootstrap"
  log_has "launchctl print"
}

@test "the deskflow-prio LaunchDaemon is rendered and its root install printed as a human step (never sudo/osascript here)" {
  PRIO=io.github.hughesyadaddy.deskflow-prio
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  staged="$DESKFLOW_CTL_DAEMON_STAGE_DIR/$PRIO.plist"
  [ -f "$staged" ]
  grep -q "<string>$APP/Contents/MacOS/deskflow-prio</string>" "$staged"
  grep -q "<string>$PRIO</string>" "$staged"
  [[ "$output" == *"sudo install -m 644 -o root -g wheel \"$staged\" \"$DESKFLOW_CTL_DAEMON_DIR/$PRIO.plist\""* ]]
  [[ "$output" == *"sudo chown root:wheel $APP/Contents/MacOS/deskflow-prio"* ]]
  [[ "$output" == *"sudo launchctl bootstrap system \"$DESKFLOW_CTL_DAEMON_DIR/$PRIO.plist\""* ]]
  log_has "launchctl print system/$PRIO"
  log_lacks "launchctl bootstrap system"
  log_lacks "osascript"
  # The script itself never escalates.
  run grep -En '^[^#]*\bsudo\b' "$SCRIPT"
  [ "$status" -ne 0 ]

  # --no-restart still surfaces the step so the operator sees it after a deploy.
  : >"$SHIM_LOG"
  run bash "$SCRIPT" --no-restart
  [ "$status" -eq 0 ]
  [[ "$output" == *"deskflow-ctl prio"* ]]
  [[ "$output" == *"sudo launchctl bootstrap system"* ]]
  log_lacks "launchctl bootstrap"
}

@test "every remaining '|| true' is tagged fleet:allow" {
  run bash -c "grep -n '|| true' '$SCRIPT' | grep -v 'fleet:allow'"
  [ "$status" -ne 0 ]
  [ -z "$output" ]
}

@test "the warn-only 'Installed unsigned' path is gone" {
  run grep -n 'Installed unsigned' "$SCRIPT"
  [ "$status" -ne 0 ]
}

@test "refuses to run under bats against a non-sandboxed install path (2026-09-16 incident regression)" {
  # A real .env clobbering this override is exactly what corrupted the real
  # /Applications/Deskflow.app on 2026-09-16 -- this proves the independent
  # hard guard catches ANY non-tmp DESKFLOW_INSTALL_APP under bats, even
  # without that specific bug, and aborts before touching anything (no
  # stop/install/codesign log lines at all).
  export DESKFLOW_INSTALL_APP="/Library/Application Support/DeskflowGuardRegressionTest/Deskflow.app"
  run bash "$SCRIPT"
  [ "$status" -eq 90 ]
  [[ "$output" == *"FATAL: running under bats"* ]]
  [[ "$output" == *"is not inside a tmp sandbox"* ]]
  [ ! -e "/Library/Application Support/DeskflowGuardRegressionTest" ]
  log_lacks "Stopping Deskflow"
  log_lacks "Installing to"
}
