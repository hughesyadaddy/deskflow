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
MOUSER_BRANCH="${FLEET_MOUSER_BRANCH:-$BRANCH}"
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
    # .env is this seat's persistent config; an explicit environment
    # variable set on invocation (as tests/CI callers do) must win, not get
    # silently clobbered -- preserve and restore anything .env also declares
    # that was already set. See scripts/install-macos.sh for the same fix.
    # Plain indexed array only: /usr/bin/env bash on macOS is 3.2, no
    # `declare -A`.
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
  # FLEET_SKIP_GIT_PULL=1: the controller already synced this checkout.
  [[ "${FLEET_SKIP_GIT_PULL:-0}" == "1" ]] && return 0
  cd "$DESKFLOW_ROOT"
  local ref="${FLEET_DESKFLOW_REF:-}"
  if [[ "$ref" == "HEAD" ]]; then
    echo "== [$HOST_TAG] deskflow: building checked-out HEAD =="
  elif [[ -n "$ref" ]]; then
    echo "== [$HOST_TAG] deskflow checkout --detach $ref =="
    git fetch origin
    git checkout --detach "$ref"
  else
    echo "== [$HOST_TAG] deskflow pull ($BRANCH) =="
    git fetch origin
    git checkout "$BRANCH"
    git pull --ff-only origin "$BRANCH"
  fi
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
  gui_exec cmake --build build --target deskflow-core Deskflow deskflow-vhid-bridge deskflow-prio -j"$(sysctl -n hw.ncpu)"
  # install-macos.sh restarts Deskflow through scripts/deskflow-ctl (launchd).
  # It never touches Mouser; MOUSER_RESTART is set only in deploy_mouser.
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

  # FLEET_SKIP_GIT_PULL=1: the controller (fleet-deploy.sh/.ps1) already synced
  # Mouser — branch fast-forward, --ref detach or --rollback — so nothing here
  # may move the checkout.
  if [[ "${FLEET_SKIP_GIT_PULL:-0}" != "1" ]]; then
    local ref="${FLEET_MOUSER_REF:-}"
    if ! git remote get-url fork >/dev/null 2>&1; then
      git remote add fork "$MOUSER_FORK_URL"
    fi
    if [[ "$ref" == "HEAD" ]]; then
      echo "== [$HOST_TAG] Mouser: building checked-out HEAD =="
    elif [[ -n "$ref" ]]; then
      echo "== [$HOST_TAG] Mouser checkout --detach $ref =="
      git fetch fork
      git checkout --detach "$ref"
    else
      echo "== [$HOST_TAG] Mouser pull (fork/$MOUSER_BRANCH) =="
      git fetch fork
      git checkout "$MOUSER_BRANCH"
      git pull --ff-only fork "$MOUSER_BRANCH"
    fi
    git log -1 --oneline
  fi

  # MOUSER_RESTART=1 is scoped to the Mouser step only: the Mouser installer
  # owns Mouser's restart. The Deskflow step above never touches Mouser.
  echo "== [$HOST_TAG] Mouser build + install (GUI session, MOUSER_RESTART=1) =="
  MOUSER_RESTART=1 python3 scripts/build_macos_gui_session.py
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
