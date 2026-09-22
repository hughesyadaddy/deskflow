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

# Real team signature but signed without --options runtime (flags=0x0):
# what hackintosh's pre-L3 build looked like.
UNHARDENED_DV='Executable=/Applications/Deskflow.app/Contents/MacOS/deskflow-prio
Identifier=org.deskflow.deskflow-prio
Format=Mach-O thin (arm64)
CodeDirectory v=20400 size=1234 flags=0x0(none) hashes=30+7 location=embedded
Signature size=4795
Authority=Apple Development: Alex Hughes (ABCDE12345)
Authority=Apple Worldwide Developer Relations Certification Authority
Authority=Apple Root CA
TeamIdentifier=ABCDE12345
Sealed Resources=none'

# Per-binary codesign -dvvv override: write the DV text to
# $SHIM_STATE/dv/<basename> and the shim answers with it for that file only.
dv_for() {
  mkdir -p "$SHIM_STATE/dv"
  printf '%s\n' "$2" >"$SHIM_STATE/dv/$1"
}

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
  # macdeployqt output: a framework binary and a dylib the signature walk
  # must cover, plus Resources/Headers files it must skip.
  fw="$prefix/Deskflow.app/Contents/Frameworks"
  mkdir -p "$fw/QtCore.framework/Versions/A/Resources" "$fw/QtCore.framework/Versions/A/Headers"
  : > "$fw/QtCore.framework/Versions/A/QtCore"
  chmod +x "$fw/QtCore.framework/Versions/A/QtCore"
  : > "$fw/QtCore.framework/Versions/A/Resources/Info.plist"
  : > "$fw/QtCore.framework/Versions/A/Headers/qglobal.h"
  : > "$fw/libcrypto.3.dylib"
  # Qt plugins and a Resources dylib: 22 of 60 Mach-Os in a real bundle live
  # under PlugIns; both trees are part of the signature walk.
  mkdir -p "$prefix/Deskflow.app/Contents/PlugIns/platforms" "$prefix/Deskflow.app/Contents/PlugIns/imageformats"
  : > "$prefix/Deskflow.app/Contents/PlugIns/platforms/libqcocoa.dylib"
  : > "$prefix/Deskflow.app/Contents/PlugIns/imageformats/libqbad.dylib"
  : > "$prefix/Deskflow.app/Contents/Resources/libres.dylib"
  # an executable script in Resources is not a Mach-O and must be skipped
  printf '#!/bin/sh\n' > "$prefix/Deskflow.app/Contents/Resources/helper.sh"
  chmod +x "$prefix/Deskflow.app/Contents/Resources/helper.sh"
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
    # script uses -dvvv; the shim only answers that flag. A per-binary
    # override in $SHIM_STATE/dv/<basename> wins over SHIM_CODESIGN_DV.
    if [[ -f "$SHIM_STATE/dv/$(basename "${2:-}")" ]]; then
      cat "$SHIM_STATE/dv/$(basename "${2:-}")" >&2
    else
      printf '%s\n' "${SHIM_CODESIGN_DV:-}" >&2
    fi
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
  # Fake launchd: nothing loaded; bootstrap of an agent reports a pid so
  # deskflow-ctl start and assert-single succeed (core 4242, GUI 4243).
  make_shim launchctl <<'EOF'
echo "launchctl $*" >> "$SHIM_LOG"
case "${1:-}" in
  print)
    [[ "$2" == */*/* ]] || exit 0   # domain listing: no runningboard instances
    [[ -f "$SHIM_STATE/loaded-${2##*/}" ]] || exit 113
    case "${2##*/}" in
      *deskflow-core) echo "	pid = 4242" ;;
      *deskflow) echo "	pid = 4243" ;;
    esac
    ;;
  bootstrap) touch "$SHIM_STATE/loaded-$(basename "$3" .plist)" ;;
  bootout) rm -f "$SHIM_STATE/loaded-${2##*/}" ;;
esac
exit 0
EOF
  # Background Task Management: fleet agents only (assert-single's audit).
  make_shim sfltool <<EOF
echo "sfltool \$*" >> "\$SHIM_LOG"
cat "$BATS_TEST_DIRNAME/fixtures/btm-dump-fleet-agents-only.txt"
EOF
  export DESKFLOW_CTL_RETIRED_PRIO_APPLY="$TMP/usr-local-bin/deskflow-prio-apply.sh"
  export DESKFLOW_CTL_RETIRED_SYNERGY_AGENT="$TMP/LibraryLaunchAgents/com.symless.synergy-agent.plist"
  # Nothing runs until deskflow-ctl start bootstrapped the agents; after
  # that the process table holds launchd's core (ppid 1) and GUI.
  make_shim ps <<'EOF'
echo "ps $*" >> "$SHIM_LOG"
if [[ "${1:-}" == "-o" && "${2:-}" == "ppid=" ]]; then echo 1; exit 0; fi
[[ -f "$SHIM_STATE/loaded-io.github.hughesyadaddy.deskflow-core" ]] && printf '4242\t%s\t%s/Contents/MacOS/deskflow-core\n' "$(id -u)" "$DESKFLOW_INSTALL_APP"
[[ -f "$SHIM_STATE/loaded-io.github.hughesyadaddy.deskflow" ]] && printf '4243\t%s\t%s/Contents/MacOS/Deskflow\n' "$(id -u)" "$DESKFLOW_INSTALL_APP"
exit 0
EOF
  # Keep the suite fast.
  make_shim sleep <<'EOF'
exit 0
EOF

  export PATH="$SHIMS:$BATS_TEST_DIRNAME/fakebin:$PATH"  # fakebin: sudo must never be real
  export DESKFLOW_BUILD_DIR="$BUILD"
  export DESKFLOW_INSTALL_APP="$APP"
  export SHIM_CODESIGN_DV="$SIGNED_DV"
  # The fixtures above carry the test team; the real default is J5KPG8ZR5C.
  export DESKFLOW_EXPECT_TEAM=ABCDE12345
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
  # every Mach-O walked: 2 in Contents/MacOS + QtCore + libcrypto + 2 PlugIns + Resources dylib
  # (framework Resources/Headers and the Resources shell script skipped)
  [[ "$output" == *"sign: total=7 apple=7 adhoc=0 hardened=7"* ]]
  grep -q '^codesign -dvvv .*/Contents/Frameworks/QtCore.framework/Versions/A/QtCore$' "$SHIM_LOG"
  grep -q '^codesign -dvvv .*/Contents/Frameworks/libcrypto.3.dylib$' "$SHIM_LOG"
  grep -q '^codesign -dvvv .*/Contents/PlugIns/platforms/libqcocoa.dylib$' "$SHIM_LOG"
  grep -q '^codesign -dvvv .*/Contents/PlugIns/imageformats/libqbad.dylib$' "$SHIM_LOG"
  grep -q '^codesign -dvvv .*/Contents/Resources/libres.dylib$' "$SHIM_LOG"
  log_lacks "Versions/A/Resources/Info.plist"
  log_lacks "Headers/qglobal.h"
  log_lacks "Resources/helper.sh"
  log_lacks "install-login-bridge-macos.sh"
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

@test "install ends with retire (keepalive log gone, root steps printed) and a REPORT-ONLY assert-single" {
  mkdir -p "$HOME/Library/Logs/Deskflow" "$(dirname "$DESKFLOW_CTL_RETIRED_PRIO_APPLY")"
  : >"$HOME/Library/Logs/Deskflow/deskflow-keepalive.log"
  : >"$DESKFLOW_CTL_RETIRED_PRIO_APPLY"
  run bash "$SCRIPT"
  # a root-owned retired file is a human step: retire (exit 2) and
  # assert-single both say so, but the install still completes -- the one
  # fatal gate is at the end of fleet-deploy-macos.sh, after Mouser.
  [ "$status" -eq 0 ]
  [[ "$output" == *"retire: removed $HOME/Library/Logs/Deskflow/deskflow-keepalive.log"* ]]
  [[ "$output" == *"sudo rm -f \"$DESKFLOW_CTL_RETIRED_PRIO_APPLY\""* ]]
  [[ "$output" == *"assert-single: FAIL"* ]]
  [[ "$output" == *"retired file present: $DESKFLOW_CTL_RETIRED_PRIO_APPLY"* ]]
  [[ "$output" == *"warning: assert-single reported problems"* ]]
  [[ "$output" == *"== Done:"* ]]
  [ ! -e "$HOME/Library/Logs/Deskflow/deskflow-keepalive.log" ]
  log_lacks "sudo"
  # opt-in fatal
  : >"$SHIM_LOG"
  DESKFLOW_INSTALL_ASSERT_FATAL=1 run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"assert-single: FAIL"* ]]
  [[ "$output" != *"== Done:"* ]]

  rm -f "$DESKFLOW_CTL_RETIRED_PRIO_APPLY"; : >"$SHIM_LOG"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"assert-single: OK"* ]]
  [[ "$output" == *"== Done:"* ]]
  # order: start (bootstrap) -> retire -> assert-single (sfltool audit)
  start_line="$(grep -n 'launchctl bootstrap' "$SHIM_LOG" | tail -1 | cut -d: -f1)"
  audit_line="$(grep -n '^sfltool dumpbtm' "$SHIM_LOG" | head -1 | cut -d: -f1)"
  [ "$start_line" -lt "$audit_line" ]
  # --no-restart: nothing running, so no assert-single
  : >"$SHIM_LOG"
  run bash "$SCRIPT" --no-restart
  [ "$status" -eq 0 ]
  log_lacks "sfltool dumpbtm"
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
if [[ "${1:-}" == "-o" && "${2:-}" == "ppid=" ]]; then echo 1; exit 0; fi
[[ -f "$HOME/Library/Deskflow/deploy.lock" ]] && echo "lock-at-stop pid=$(cat "$HOME/Library/Deskflow/deploy.lock")" >> "$SHIM_LOG"
# same process table as the default shim, so the closing assert-single passes
[[ -f "$SHIM_STATE/loaded-io.github.hughesyadaddy.deskflow-core" ]] && printf '4242\t%s\t%s/Contents/MacOS/deskflow-core\n' "$(id -u)" "$DESKFLOW_INSTALL_APP"
[[ -f "$SHIM_STATE/loaded-io.github.hughesyadaddy.deskflow" ]] && printf '4243\t%s\t%s/Contents/MacOS/Deskflow\n' "$(id -u)" "$DESKFLOW_INSTALL_APP"
exit 0
EOF
  make_shim launchctl <<'EOF'
echo "launchctl $*" >> "$SHIM_LOG"
case "${1:-}" in
  print)
    [[ "$2" == */*/* ]] || exit 0
    [[ -f "$SHIM_STATE/loaded-${2##*/}" ]] || exit 113
    case "${2##*/}" in *deskflow-core) echo "	pid = 4242" ;; *deskflow) echo "	pid = 4243" ;; esac
    ;;
  bootstrap) touch "$SHIM_STATE/loaded-$(basename "$3" .plist)"; [[ -f "$HOME/Library/Deskflow/deploy.lock" ]] && echo "lock-at-start" >> "$SHIM_LOG" ;;
  bootout) rm -f "$SHIM_STATE/loaded-${2##*/}" ;;
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

@test "ad-hoc signature (Signature=adhoc) exits 1" {
  SHIM_CODESIGN_DV="$ADHOC_DV" run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"Signature=adhoc"* ]]
  [[ "$output" == *"sign: total=7 apple=0 adhoc=7 hardened=0"* ]]
  log_lacks "launchctl bootstrap"
}

@test "an ad-hoc Mach-O anywhere in the bundle (one framework dylib) exits 1" {
  dv_for libcrypto.3.dylib "$ADHOC_DV"
  run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"Contents/Frameworks/libcrypto.3.dylib: Signature=adhoc"* ]]
  [[ "$output" == *"sign: total=7 apple=6 adhoc=1 hardened=6"* ]]
  [[ "$output" != *"Codesign verify OK"* ]]
  log_lacks "launchctl bootstrap"
  [ ! -e "${APP}.bak" ]
}

@test "an ad-hoc Qt plugin or Resources dylib exits 1 (PlugIns/Resources are walked)" {
  dv_for libqbad.dylib "$ADHOC_DV"
  dv_for libres.dylib "$ADHOC_DV"
  run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"Contents/PlugIns/imageformats/libqbad.dylib: Signature=adhoc"* ]]
  [[ "$output" == *"Contents/Resources/libres.dylib: Signature=adhoc"* ]]
  [[ "$output" == *"sign: total=7 apple=5 adhoc=2 hardened=5"* ]]
  log_lacks "launchctl bootstrap"
}

@test "a first-party binary without the hardened runtime exits 1" {
  dv_for deskflow-prio "$UNHARDENED_DV"
  run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"Contents/MacOS/deskflow-prio is not signed with the hardened runtime"* ]]
  [[ "$output" == *"sign: total=7 apple=7 adhoc=0 hardened=6"* ]]
  log_lacks "launchctl bootstrap"
}

@test "a bundled framework without the hardened runtime is accepted (only Contents/MacOS/* must be hardened)" {
  dv_for QtCore "$UNHARDENED_DV"
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"sign: total=7 apple=7 adhoc=0 hardened=6"* ]]
  [[ "$output" == *"Codesign verify OK"* ]]
}

@test "a TeamIdentifier other than DESKFLOW_EXPECT_TEAM exits 1 (default J5KPG8ZR5C)" {
  DESKFLOW_EXPECT_TEAM=OTHER99999 run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"TeamIdentifier=ABCDE12345 != expected OTHER99999"* ]]
  log_lacks "launchctl bootstrap"
  run grep -n 'DESKFLOW_EXPECT_TEAM:-J5KPG8ZR5C' "$SCRIPT"
  [ "$status" -eq 0 ]
}

@test "a build tree marked ADHOC-DEV-BUILD is refused before any codesign call" {
  : >"$BUILD/ADHOC-DEV-BUILD"
  run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"ADHOC-DEV-BUILD exists"* ]]
  [[ "$output" == *"FLEET_ALLOW_ADHOC_DEV_BUILD=ON"* ]]
  log_lacks "codesign --verify"
  log_lacks "codesign -dvvv"
  log_lacks "launchctl bootstrap"
  [ ! -e "${APP}.bak" ]
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
