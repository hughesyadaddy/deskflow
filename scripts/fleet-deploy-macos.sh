#!/usr/bin/env bash
# Runs ON a Mac (local or via SSH). Pull fleet branch, signed build, install.
#
# No silent success: every step either succeeds or the script exits non-zero.
# The signing identity comes ONLY from `.env` DESKFLOW_CODESIGN_ID; there is no
# "any Apple Development cert" fallback and no keychain-password plumbing.
# Steps that need the login keychain (build/sign/install) are routed through
# tools/fleet-gui-exec.py when it exists, which execs them in the console GUI
# session if this shell (e.g. SSH) cannot reach the keychain.
set -euo pipefail

# Non-interactive SSH shells skip login profiles; Homebrew tools must be on PATH.
# Only bootstrap when brew is not already resolvable so an interactive shell's
# (or a test harness's) PATH ordering is left alone.
if ! command -v brew >/dev/null 2>&1; then
  if [[ -x /opt/homebrew/bin/brew ]]; then
    eval "$(/opt/homebrew/bin/brew shellenv)"
  elif [[ -x /usr/local/bin/brew ]]; then
    eval "$(/usr/local/bin/brew shellenv)"
  else
    export PATH="/opt/homebrew/bin:/usr/local/bin:${PATH:-/usr/bin:/bin:/usr/sbin:/sbin}"
  fi
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DESKFLOW_ROOT="${FLEET_DESKFLOW_ROOT:-$ROOT}"
DESKFLOW_ROOT="${DESKFLOW_ROOT/#\~/$HOME}"
MOUSER_ROOT="${FLEET_MOUSER_ROOT:-$HOME/Desktop/Mouser}"
MOUSER_ROOT="${MOUSER_ROOT/#\~/$HOME}"
MOUSER_FORK_URL="${FLEET_MOUSER_FORK_URL:-https://github.com/hughesyadaddy/Mouser.git}"
BRANCH="${FLEET_BRANCH:-main}"
DEPLOY_DESKFLOW="${FLEET_DEPLOY_DESKFLOW:-1}"
DEPLOY_MOUSER="${FLEET_DEPLOY_MOUSER:-1}"
RECONFIGURE="${FLEET_RECONFIGURE:-0}"
HOST_TAG="$(hostname -s)"

fail() {
  echo "error: [$HOST_TAG] $*" >&2
  exit 1
}

# Run a command in a session that can reach the login keychain.
# tools/fleet-gui-exec.py decides: direct exec when the keychain is reachable,
# otherwise it drives the console GUI session and propagates the exit code.
gui_exec() {
  local runner="$DESKFLOW_ROOT/tools/fleet-gui-exec.py"
  if [[ -x "$runner" ]]; then
    python3 "$runner" -- "$@"
  else
    "$@"
  fi
}

load_dotenv() {
  cd "$DESKFLOW_ROOT"
  if [[ -f .env ]]; then
    set -a
    # shellcheck disable=SC1091
    source .env
    set +a
  fi
}

# The ONLY source of the signing identity. Empty or ad-hoc ("-") is fatal:
# an ad-hoc signature changes every build, which resets the app's TCC grants.
resolve_codesign_id() {
  local codesign_id="${DESKFLOW_CODESIGN_ID:-}"
  if [[ -z "$codesign_id" || "$codesign_id" == "-" ]]; then
    fail "DESKFLOW_CODESIGN_ID is unset or '-' (ad-hoc) in $DESKFLOW_ROOT/.env." \
      "Set it to the certificate hash from: security find-identity -v -p codesigning"
  fi
  printf '%s\n' "$codesign_id"
}

cmake_cache_value() {
  # $1 = cache file, $2 = variable name. Prints nothing when absent (awk exits 0 either way).
  [[ -f "$1" ]] || return 0
  awk -v k="$2" 'match($0, "^" k "(:[A-Z]+)?=") { print substr($0, RLENGTH + 1); exit }' "$1"
}

git_pull_deskflow() {
  [[ "${FLEET_SKIP_GIT_PULL:-0}" == "1" ]] && return 0
  echo "== [$HOST_TAG] deskflow pull ($BRANCH) =="
  cd "$DESKFLOW_ROOT"
  git fetch origin
  git checkout "$BRANCH"
  git pull --ff-only origin "$BRANCH"
  git log -1 --oneline
}

configure_deskflow() {
  load_dotenv
  local codesign_id
  codesign_id="$(resolve_codesign_id)"

  local cache="build/CMakeCache.txt"
  local reason=""
  if [[ "$RECONFIGURE" == "1" ]]; then
    reason="FLEET_RECONFIGURE=1"
  elif [[ ! -f "$cache" ]]; then
    reason="no $cache"
  else
    local cached_id cached_strict
    cached_id="$(cmake_cache_value "$cache" APPLE_CODESIGN_DEV)"
    cached_strict="$(cmake_cache_value "$cache" FLEET_STRICT_SIGNING)"
    if [[ "$cached_id" != "$codesign_id" ]]; then
      reason="APPLE_CODESIGN_DEV cache mismatch ('${cached_id:-<unset>}' != '$codesign_id')"
    elif [[ "$cached_strict" != "ON" ]]; then
      reason="FLEET_STRICT_SIGNING cache is '${cached_strict:-<unset>}', need ON"
    fi
  fi

  if [[ -z "$reason" ]]; then
    echo "== [$HOST_TAG] cmake cache matches (identity + strict signing); skipping configure =="
    return 0
  fi

  echo "== [$HOST_TAG] cmake configure (signed, strict): $reason =="
  gui_exec cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$(brew --prefix qt);$(brew --prefix openssl@3)" \
    -DFLEET_STRICT_SIGNING=ON \
    -DAPPLE_CODESIGN_DEV="$codesign_id" \
    -DSKIP_BUILD_TESTS=ON \
    -DBUILD_TESTS=OFF
}

build_install_deskflow() {
  cd "$DESKFLOW_ROOT"
  echo "== [$HOST_TAG] deskflow build + install =="
  gui_exec cmake --build build --target deskflow-core Deskflow deskflow-vhid-bridge -j"$(sysctl -n hw.ncpu)"
  gui_exec bash scripts/install-macos.sh
  if ! codesign --verify --deep --strict /Applications/Deskflow.app; then
    fail "codesign --verify --deep --strict /Applications/Deskflow.app failed — refusing to call this deploy a success"
  fi
  echo "== [$HOST_TAG] codesign verify OK =="
}

deploy_mouser() {
  [[ "$DEPLOY_MOUSER" == "1" ]] || return 0
  [[ -d "$MOUSER_ROOT" ]] || fail "Mouser checkout missing at $MOUSER_ROOT (set FLEET_DEPLOY_MOUSER=0 to skip Mouser)"
  cd "$MOUSER_ROOT"
  [[ -d .git ]] || fail "$MOUSER_ROOT is not a git checkout"

  if [[ "${FLEET_SKIP_GIT_PULL:-0}" != "1" ]]; then
    echo "== [$HOST_TAG] Mouser pull (fork/$BRANCH) =="
    if ! git remote get-url fork >/dev/null 2>&1; then
      git remote add fork "$MOUSER_FORK_URL"
    fi
    git fetch fork
    git checkout "$BRANCH"
    git pull --ff-only fork "$BRANCH"
    git log -1 --oneline
  fi

  echo "== [$HOST_TAG] Mouser build + install (GUI session) =="
  python3 scripts/build_macos_gui_session.py
}

main() {
  echo "=== fleet-deploy-macos on $HOST_TAG ==="
  if [[ "$DEPLOY_DESKFLOW" == "1" ]]; then
    git_pull_deskflow
    configure_deskflow
    build_install_deskflow
  fi
  deploy_mouser
  echo "=== done: $HOST_TAG ==="
}

main "$@"
