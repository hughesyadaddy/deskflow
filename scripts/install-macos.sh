#!/usr/bin/env bash
# Stop Deskflow via launchd, install the built .app to /Applications, start it again.
#
# Process lifecycle goes through scripts/deskflow-ctl only (launchctl
# bootout / bootstrap): no pattern kills, no `open`. This script touches
# nothing but the Deskflow bundle and its two launchd agents; other apps that
# talk to deskflow-core are owned by their own installers.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ -f .env ]]; then
  set -a
  # shellcheck disable=SC1091
  source .env
  set +a
fi

BUILD_DIR="${DESKFLOW_BUILD_DIR:-build}"
INSTALL_APP="${DESKFLOW_INSTALL_APP:-/Applications/Deskflow.app}"
INSTALL_ROOT="$(dirname "$INSTALL_APP")"
APP_NAME="$(basename "$INSTALL_APP" .app)"
SOURCE_APP="$ROOT/$BUILD_DIR/bin/Deskflow.app"
CTL="$ROOT/scripts/deskflow-ctl"
RESTART=1

usage() {
  cat <<'EOF'
Usage: scripts/install-macos.sh [--no-restart] [--install-app PATH]

Stops Deskflow through scripts/deskflow-ctl (launchctl bootout), installs the
built bundle to /Applications/Deskflow.app (or DESKFLOW_INSTALL_APP), clears
quarantine, verifies codesign (fatal on failure), and starts it again through
scripts/deskflow-ctl (launchctl bootstrap). --no-restart leaves it stopped.

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
  local info
  if ! info="$(codesign -dv "$core" 2>&1)"; then
    echo "error: codesign -dv failed for $core" >&2
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

install_bundle() {
  echo "== Installing to $INSTALL_APP =="
  if [[ -e "$INSTALL_APP" ]]; then
    rm -rf "${INSTALL_APP}.bak"
    mv "$INSTALL_APP" "${INSTALL_APP}.bak"
    echo "Backed up previous install to ${INSTALL_APP}.bak"
  fi

  if [[ -d "$BUILD_DIR" ]] && [[ -f "$BUILD_DIR/cmake_install.cmake" ]]; then
    local stage
    stage="$(mktemp -d "${TMPDIR:-/tmp}/deskflow-install.XXXXXX")"
    echo "Using staged cmake --install (macdeployqt + bundle layout)"
    cmake --install "$BUILD_DIR" --prefix "$stage"
    if [[ ! -d "$stage/Deskflow.app" ]]; then
      rm -rf "$stage"
      echo "error: staged install did not produce Deskflow.app" >&2
      exit 1
    fi
    cp -R "$stage/Deskflow.app" "$INSTALL_APP"
    rm -rf "$stage"
  elif [[ -d "$SOURCE_APP" ]]; then
    echo "Using build tree copy from $SOURCE_APP"
    cp -R "$SOURCE_APP" "$INSTALL_APP"
  else
    echo "error: no install source — build first (missing $SOURCE_APP and $BUILD_DIR/cmake_install.cmake)" >&2
    exit 1
  fi

  if [[ ! -d "$INSTALL_APP" ]]; then
    echo "error: install failed — $INSTALL_APP not found" >&2
    exit 1
  fi

  xattr -cr "$INSTALL_APP" 2>/dev/null || true # fleet:allow quarantine attrs may simply not exist

  verify_signature "$INSTALL_APP"

  # libqsvgicon.dylib needs QtSvg.framework; without it tray/menu SVG icons are blank
  # in Release installs while Debug (Homebrew Qt) still works.
  if [[ -f "$INSTALL_APP/Contents/PlugIns/iconengines/libqsvgicon.dylib" ]] &&
     [[ ! -d "$INSTALL_APP/Contents/Frameworks/QtSvg.framework" ]]; then
    echo "error: libqsvgicon.dylib is present but QtSvg.framework is missing — rebuild with Qt6::Svg linked" >&2
    exit 1
  fi
}

# The login-bridge installer is bundled into Contents/Resources by CMake
# (src/apps/deskflow-gui/CMakeLists.txt, MACOSX_PACKAGE_LOCATION) BEFORE the
# bundle is signed, so it is covered by the seal. Never copy files into the
# bundle after verify_signature: that invalidates the signature we just checked.
verify_login_bridge_bundled() {
  local dest="$INSTALL_APP/Contents/Resources/install-login-bridge-macos.sh"
  if [[ ! -f "$dest" ]]; then
    echo "error: $dest missing — the bundle was built without the login bridge helper (rebuild; do not copy it in post-sign)" >&2
    exit 1
  fi
}

quit_deskflow
install_bundle
verify_login_bridge_bundled
start_deskflow
echo "== Done: $INSTALL_APP =="
