#!/usr/bin/env bats
# scripts/install-macos.sh — the signature check is fatal, never a warning.
#
# The real script runs against a temp install path with cmake/codesign/osascript/
# pgrep/pkill/xattr/open/sleep replaced by PATH shims that log to $SHIM_LOG.
# `codesign -dv` output is controlled per test via $SHIM_CODESIGN_DV.

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
  mkdir -p "$SHIMS" "$BUILD" "$TMP/Applications"
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
  mkdir -p "$prefix/Deskflow.app/Contents/MacOS"
  : > "$prefix/Deskflow.app/Contents/MacOS/deskflow-core"
fi
exit 0
EOF

  make_shim codesign <<'EOF'
echo "codesign $*" >> "$SHIM_LOG"
case "${1:-}" in
  --verify) exit "${SHIM_CODESIGN_VERIFY_RC:-0}" ;;
  -dv)
    # Real codesign prints -dv details to stderr.
    printf '%s\n' "${SHIM_CODESIGN_DV:-}" >&2
    exit "${SHIM_CODESIGN_DV_RC:-0}"
    ;;
esac
exit 0
EOF

  for tool in osascript pkill xattr open; do
    make_shim "$tool" <<EOF
echo "$tool \$*" >> "\$SHIM_LOG"
exit 0
EOF
  done
  # Nothing is running.
  make_shim pgrep <<'EOF'
echo "pgrep $*" >> "$SHIM_LOG"
exit 1
EOF
  # Keep the suite fast.
  make_shim sleep <<'EOF'
exit 0
EOF

  export PATH="$SHIMS:$PATH"
  export DESKFLOW_BUILD_DIR="$BUILD"
  export DESKFLOW_INSTALL_APP="$APP"
  export SHIM_CODESIGN_DV="$SIGNED_DV"
  unset SHIM_CODESIGN_VERIFY_RC SHIM_CODESIGN_DV_RC
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

@test "signed bundle installs, verifies, and relaunches" {
  run bash "$SCRIPT"
  [ "$status" -eq 0 ]
  [[ "$output" == *"Codesign verify OK"* ]]
  [[ "$output" == *"Authority=Apple Development"* ]]
  [[ "$output" == *"TeamIdentifier=ABCDE12345"* ]]
  [ -f "$APP/Contents/MacOS/deskflow-core" ]
  log_has "codesign --verify --deep --strict $APP"
  log_has "codesign -dv $APP/Contents/MacOS/deskflow-core"
  log_has "open $APP --args --show"
}

@test "codesign --verify failure exits 1 and does not relaunch" {
  SHIM_CODESIGN_VERIFY_RC=1 run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"codesign --verify --deep --strict failed"* ]]
  [[ "$output" != *"Codesign verify OK"* ]]
  [[ "$output" != *"Installed unsigned"* ]]
  log_lacks "open "
}

@test "ad-hoc signature (no Authority) exits 1" {
  SHIM_CODESIGN_DV="$ADHOC_DV" run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"no Authority="* ]]
  log_lacks "open "
}

@test "Authority without a TeamIdentifier exits 1" {
  SHIM_CODESIGN_DV="$NO_TEAM_DV" run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"no TeamIdentifier="* ]]
  log_lacks "open "
}

@test "codesign -dv itself failing exits 1" {
  SHIM_CODESIGN_DV="" SHIM_CODESIGN_DV_RC=1 run bash "$SCRIPT"
  [ "$status" -eq 1 ]
  [[ "$output" == *"codesign -dv failed"* ]]
}

@test "--no-restart still enforces the signature check" {
  SHIM_CODESIGN_VERIFY_RC=1 run bash "$SCRIPT" --no-restart
  [ "$status" -eq 1 ]
  run bash "$SCRIPT" --no-restart
  [ "$status" -eq 0 ]
  log_lacks "open "
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
