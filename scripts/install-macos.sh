#!/usr/bin/env bash
# Stage + verify the built .app, then stop Deskflow via launchd, swap it into
# /Applications and start it again. Verification happens BEFORE the running
# seat is touched, so a rejected build never leaves agents booted out.
#
# Process lifecycle goes through scripts/deskflow-ctl only (launchctl
# bootout / bootstrap): no pattern kills, no `open`. This script touches
# nothing but the Deskflow bundle and its two launchd agents; other apps that
# talk to deskflow-core are owned by their own installers.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ -f .env ]]; then
  # .env is this seat's persistent config, but an explicit environment
  # variable set on invocation (as the test suite and CI callers do, e.g.
  # DESKFLOW_INSTALL_APP pointed at a temp dir) must win, not get silently
  # clobbered by a real seat's .env -- preserve and restore any variable
  # .env also declares that was already set in the environment. Plain
  # indexed arrays only: /usr/bin/env bash on macOS is 3.2, no `declare -A`.
  _env_overrides=()
  while IFS='=' read -r _env_key _; do
    [[ -z "$_env_key" || "$_env_key" == \#* ]] && continue
    if [[ -n "${!_env_key+x}" ]]; then
      _env_overrides+=("$_env_key=${!_env_key}")
    fi
  done < <(grep -E '^[A-Za-z_][A-Za-z0-9_]*=' .env)
  set -a
  # shellcheck disable=SC1091
  source .env
  set +a
  if [[ ${#_env_overrides[@]} -gt 0 ]]; then
    for _env_kv in "${_env_overrides[@]}"; do
      export "$_env_kv"
    done
  fi
  unset _env_overrides _env_key _env_kv
fi

BUILD_DIR="${DESKFLOW_BUILD_DIR:-build}"
INSTALL_APP="${DESKFLOW_INSTALL_APP:-/Applications/Deskflow.app}"
INSTALL_ROOT="$(dirname "$INSTALL_APP")"
APP_NAME="$(basename "$INSTALL_APP" .app)"
SOURCE_APP="$ROOT/$BUILD_DIR/bin/Deskflow.app"
CTL="$ROOT/scripts/deskflow-ctl"
RESTART=1

# Hard structural guard, independent of the .env-precedence fix above: a
# 2026-09-16 incident had this script run under `bats` with its own .env
# clobbering the test's DESKFLOW_INSTALL_APP override, so install_bundle()
# overwrote the REAL /Applications/Deskflow.app with the test's empty
# placeholder binary. That specific bug is fixed, but this check exists so
# no *future* bug of the same shape (here or in a caller) can ever again
# point a destructive install at a real path while under a test harness --
# BATS_TEST_FILENAME is set by bats for every test, unconditionally.
if [[ -n "${BATS_TEST_FILENAME:-}" ]]; then
  case "$INSTALL_APP" in
    "$TMPDIR"*|/tmp/*|/private/tmp/*|/private/var/folders/*|"${BATS_TMPDIR:-__unset__}"*|\
    "${BATS_RUN_TMPDIR:-__unset__}"*|"${BATS_TEST_TMPDIR:-__unset__}"*|"${BATS_FILE_TMPDIR:-__unset__}"*)
      ;;
    *)
      echo "FATAL: running under bats (BATS_TEST_FILENAME set) but DESKFLOW_INSTALL_APP" \
           "('$INSTALL_APP') is not inside a tmp sandbox. Refusing to touch it -- this is" \
           "exactly the bug class that overwrote the real /Applications/Deskflow.app on" \
           "2026-09-16. Aborting." >&2
      exit 90
      ;;
  esac
fi

usage() {
  cat <<'EOF'
Usage: scripts/install-macos.sh [--no-restart] [--install-app PATH]

Stages the built bundle, verifies codesign on the STAGED copy (fatal on
failure, nothing touched yet), then stops Deskflow through scripts/deskflow-ctl
(launchctl bootout), swaps the bundle into /Applications/Deskflow.app (or
DESKFLOW_INSTALL_APP), clears quarantine, and starts it again through
scripts/deskflow-ctl (launchctl bootstrap). --no-restart leaves it stopped.
A deploy lock (~/Library/Deskflow/deploy.lock) keeps `deskflow-ctl converge`
from bootstrapping agents mid-swap.

The deskflow-prio LaunchDaemon (system/io.github.hughesyadaddy.deskflow-prio,
root: task_for_pid) is rendered by `deskflow-ctl prio`; this script never
escalates privileges, so when the daemon is missing or stale the exact
admin commands (install plist to /Library/LaunchDaemons, chown the binary,
launchctl bootstrap system) are printed as a human step.

Requires a prior Release build (build/bin/Deskflow.app or cmake --install).
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-restart) RESTART=0; shift ;;
    --install-app)
      INSTALL_APP="$2"
      INSTALL_ROOT="$(dirname "$INSTALL_APP")"
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

quit_deskflow() {
  echo "== Stopping Deskflow (deskflow-ctl stop) =="
  [[ -x "$CTL" ]] || { echo "error: $CTL missing or not executable" >&2; exit 1; }
  DESKFLOW_INSTALL_APP="$INSTALL_APP" "$CTL" stop
}

start_deskflow() {
  if [[ "$RESTART" -ne 1 ]]; then
    # `deskflow-ctl start` also handles the prio LaunchDaemon; with
    # --no-restart still surface it so the operator sees the root step.
    echo "== Deskflow left stopped; deskflow-prio LaunchDaemon (deskflow-ctl prio) =="
    DESKFLOW_INSTALL_APP="$INSTALL_APP" "$CTL" prio
    return 0
  fi
  echo "== Starting Deskflow (deskflow-ctl start) =="
  DESKFLOW_INSTALL_APP="$INSTALL_APP" "$CTL" start
}

# Fatal unless the bundle verifies AND deskflow-core carries a real (non ad-hoc)
# signature: an Authority chain and a TeamIdentifier. An ad-hoc or unsigned
# install "works" once and then loses its Accessibility / Input Monitoring
# grants on the next build, so it must never be reported as a success.
verify_signature() {
  local app="$1"
  local core="$app/Contents/MacOS/deskflow-core"
  if ! codesign --verify --deep --strict "$app"; then
    echo "error: codesign --verify --deep --strict failed for $app — installed bundle is unsigned or broken" >&2
    exit 1
  fi
  # Plain `codesign -dv` never prints Authority= lines regardless of how the
  # binary is signed -- that chain is only emitted at -vvv verbosity. Using
  # plain -dv here made this check fail on every real (non-ad-hoc) signature.
  local info
  if ! info="$(codesign -dvvv "$core" 2>&1)"; then
    echo "error: codesign -dvvv failed for $core" >&2
    exit 1
  fi
  if ! grep -q '^Authority=' <<<"$info"; then
    echo "error: $core has no Authority= (ad-hoc signature) — set DESKFLOW_CODESIGN_ID to a real identity" >&2
    exit 1
  fi
  if ! grep -Eq '^TeamIdentifier=[A-Z0-9]+$' <<<"$info"; then
    echo "error: $core has no TeamIdentifier= — not signed with a Developer certificate" >&2
    exit 1
  fi
  echo "== Codesign verify OK =="
  grep -E '^(Authority|TeamIdentifier)=' <<<"$info"
}

# Stage the new bundle somewhere it can be verified in full BEFORE the running
# seat is touched. Sets STAGED_APP; STAGE_TMP is the mktemp dir to remove
# afterwards (empty when the build tree is used in place).
STAGE_TMP=""
STAGED_APP=""
stage_bundle() {
  if [[ -d "$BUILD_DIR" ]] && [[ -f "$BUILD_DIR/cmake_install.cmake" ]]; then
    STAGE_TMP="$(mktemp -d "${TMPDIR:-/tmp}/deskflow-install.XXXXXX")"
    echo "Using staged cmake --install (macdeployqt + bundle layout)"
    cmake --install "$BUILD_DIR" --prefix "$STAGE_TMP"
    if [[ ! -d "$STAGE_TMP/Deskflow.app" ]]; then
      echo "error: staged install did not produce Deskflow.app" >&2
      exit 1
    fi
    STAGED_APP="$STAGE_TMP/Deskflow.app"
  elif [[ -d "$SOURCE_APP" ]]; then
    echo "Using build tree copy from $SOURCE_APP"
    STAGED_APP="$SOURCE_APP"
  else
    echo "error: no install source — build first (missing $SOURCE_APP and $BUILD_DIR/cmake_install.cmake)" >&2
    exit 1
  fi
}

# Everything that can reject a bundle runs here, against the staged copy,
# while the old install is still running. A rejected build must never leave
# the seat with its agents booted out and no bundle to bootstrap.
verify_staged() {
  local app="$1"
  verify_signature "$app"
  # The login-bridge installer is bundled into Contents/Resources by CMake
  # (src/apps/deskflow-gui/CMakeLists.txt, MACOSX_PACKAGE_LOCATION) BEFORE the
  # bundle is signed, so it is covered by the seal. Never copy files into the
  # bundle after verify_signature: that invalidates the signature just checked.
  if [[ ! -f "$app/Contents/Resources/install-login-bridge-macos.sh" ]]; then
    echo "error: $app/Contents/Resources/install-login-bridge-macos.sh missing — the bundle was built without the login bridge helper (rebuild; do not copy it in post-sign)" >&2
    exit 1
  fi
  # libqsvgicon.dylib needs QtSvg.framework; without it tray/menu SVG icons are blank
  # in Release installs while Debug (Homebrew Qt) still works.
  if [[ -f "$app/Contents/PlugIns/iconengines/libqsvgicon.dylib" ]] &&
     [[ ! -d "$app/Contents/Frameworks/QtSvg.framework" ]]; then
    echo "error: libqsvgicon.dylib is present but QtSvg.framework is missing — rebuild with Qt6::Svg linked" >&2
    exit 1
  fi
}

install_bundle() {
  local staged="$1"
  echo "== Installing to $INSTALL_APP =="
  if [[ -e "$INSTALL_APP" ]]; then
    rm -rf "${INSTALL_APP}.bak"
    mv "$INSTALL_APP" "${INSTALL_APP}.bak"
    echo "Backed up previous install to ${INSTALL_APP}.bak"
  fi
  if ! cp -R "$staged" "$INSTALL_APP" || [[ ! -d "$INSTALL_APP" ]]; then
    echo "error: copying $staged to $INSTALL_APP failed" >&2
    if [[ -d "${INSTALL_APP}.bak" ]]; then
      rm -rf "$INSTALL_APP"
      mv "${INSTALL_APP}.bak" "$INSTALL_APP"
      echo "restored previous install from ${INSTALL_APP}.bak" >&2
    fi
    exit 1
  fi
  xattr -cr "$INSTALL_APP" 2>/dev/null || true # fleet:allow quarantine attrs may simply not exist
}

# `deskflow-ctl converge` (a 60 s launchd tick) must not bootstrap agents
# back in the middle of the swap; it stands down while this lock is younger
# than 30 min.
DEPLOY_LOCK="$HOME/Library/Deskflow/deploy.lock"
LOCK_TAKEN=0
take_deploy_lock() {
  mkdir -p "$(dirname "$DEPLOY_LOCK")"
  echo "$$" >"$DEPLOY_LOCK"
  LOCK_TAKEN=1
}
cleanup() {
  # Only this run's lock: a concurrent install's lock must survive our exit.
  (( LOCK_TAKEN )) && rm -f "$DEPLOY_LOCK"
  [[ -n "$STAGE_TMP" ]] && rm -rf "$STAGE_TMP"
  return 0
}
trap cleanup EXIT

stage_bundle
verify_staged "$STAGED_APP"
take_deploy_lock
quit_deskflow
install_bundle "$STAGED_APP"
start_deskflow
echo "== Done: $INSTALL_APP =="
