#!/usr/bin/env bats
# scripts/fleet-deploy-macos.sh — "kill silent success" contract.
#
# Every external tool (cmake, codesign, git, security, brew, python3) is a PATH
# shim in a temp dir that appends its argv to $SHIM_LOG. The script itself is
# the real one; only its environment is faked.

SCRIPT="$BATS_TEST_DIRNAME/../../scripts/fleet-deploy-macos.sh"

setup() {
  TMP="$(mktemp -d "${BATS_TEST_TMPDIR:-${TMPDIR:-/tmp}}/fleet-deploy.XXXXXX")"
  SHIMS="$TMP/bin"
  FAKE_ROOT="$TMP/deskflow"
  MOUSER="$TMP/Mouser"
  export SHIM_LOG="$TMP/calls.log"
  export SHIM_STATE="$TMP/state"
  mkdir -p "$SHIMS" "$FAKE_ROOT/scripts" "$MOUSER/.git" "$MOUSER/scripts" "$SHIM_STATE"
  : >"$SHIM_LOG"

  # The deploy script calls the repo's install script; stub it inside the fake root.
  printf '#!/usr/bin/env bash\necho "install-macos.sh $*" >> "$SHIM_LOG"\n' >"$FAKE_ROOT/scripts/install-macos.sh"
  : >"$MOUSER/scripts/build_macos_gui_session.py"

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

  # The identity must come from .env only; the fallback lookup is gone.
  make_shim security <<'EOF'
echo "security $*" >> "$SHIM_LOG"
echo "security must never be called by fleet-deploy-macos.sh" >&2
exit 1
EOF

  make_shim brew <<'EOF'
echo "brew $*" >> "$SHIM_LOG"
[[ "${1:-}" == "--prefix" ]] && echo "/fake/opt/${2:-}"
exit 0
EOF

  # python3 stands in for both the Mouser build and tools/fleet-gui-exec.py.
  # For the latter it honours the documented interface: `-- cmd args...` is exec'd.
  make_shim python3 <<'EOF'
echo "python3 $*" >> "$SHIM_LOG"
if [[ "${1:-}" == *fleet-gui-exec.py ]]; then
  shift
  while [[ $# -gt 0 && "$1" != "--" ]]; do shift; done
  shift
  exec "$@"
fi
exit "${SHIM_PYTHON_RC:-0}"
EOF

  export PATH="$SHIMS:$PATH"
  export FLEET_DESKFLOW_ROOT="$FAKE_ROOT"
  export FLEET_MOUSER_ROOT="$MOUSER"
  unset FLEET_BRANCH FLEET_DEPLOY_MOUSER FLEET_DEPLOY_DESKFLOW FLEET_RECONFIGURE FLEET_SKIP_GIT_PULL
  unset DESKFLOW_CODESIGN_ID FLEET_KEYCHAIN_PASSWORD
  unset SHIM_CMAKE_RC SHIM_CODESIGN_VERIFY_RC SHIM_GIT_PULL_RC SHIM_PYTHON_RC
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

@test "no keychain-password plumbing remains" {
  script_lacks 'unlock_keychain'
  script_lacks 'KEYCHAIN_PASSWORD'
  script_lacks 'unlock-keychain'
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
