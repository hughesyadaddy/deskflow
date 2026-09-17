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

@test "signed bundle installs, verifies, and restarts through launchd (ctl stop -> swap -> ctl start)" {
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"Codesign verify OK"* ]]
  [[ "$output" == *"Authority=Apple Development"* ]]
  [[ "$output" == *"TeamIdentifier=ABCDE12345"* ]]
  [ -f "$APP/Contents/MacOS/deskflow-core" ]
  log_has "codesign --verify --deep --strict $APP"
  log_has "codesign -dvvv $APP/Contents/MacOS/deskflow-core"
  log_has "launchctl bootstrap gui/$(id -u) $DESKFLOW_CTL_AGENT_DIR/io.github.hughesyadaddy.deskflow-core.plist"
  log_has "launchctl bootstrap gui/$(id -u) $DESKFLOW_CTL_AGENT_DIR/io.github.hughesyadaddy.deskflow.plist"
  # stop (ps inventory) happens before the bundle swap, start after verify
  stop_line="$(grep -n '^ps ' "$SHIM_LOG" | head -1 | cut -d: -f1)"
  verify_line="$(grep -n '^codesign --verify' "$SHIM_LOG" | cut -d: -f1)"
  start_line="$(grep -n 'launchctl bootstrap' "$SHIM_LOG" | head -1 | cut -d: -f1)"
  [ "$stop_line" -lt "$verify_line" ]
  [ "$verify_line" -lt "$start_line" ]
  log_lacks "open "
  log_lacks "pkill"
  log_lacks "osascript"
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
