#!/usr/bin/env bash
# Runs ON a Mac (local or via SSH). Pull fleet branch, signed build, install.
set -euo pipefail

# Non-interactive SSH shells skip login profiles; Homebrew tools must be on PATH.
if [[ -x /opt/homebrew/bin/brew ]]; then
  eval "$(/opt/homebrew/bin/brew shellenv)"
elif [[ -x /usr/local/bin/brew ]]; then
  eval "$(/usr/local/bin/brew shellenv)"
else
  export PATH="/opt/homebrew/bin:/usr/local/bin:${PATH:-/usr/bin:/bin:/usr/sbin:/sbin}"
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DESKFLOW_ROOT="${FLEET_DESKFLOW_ROOT:-$ROOT}"
DESKFLOW_ROOT="${DESKFLOW_ROOT/#\~/$HOME}"
MOUSER_ROOT="${FLEET_MOUSER_ROOT:-$HOME/Desktop/Mouser}"
MOUSER_ROOT="${MOUSER_ROOT/#\~/$HOME}"
BRANCH="${FLEET_BRANCH:-refactor/fleet-state-hub}"
DEPLOY_DESKFLOW="${FLEET_DEPLOY_DESKFLOW:-1}"
DEPLOY_MOUSER="${FLEET_DEPLOY_MOUSER:-1}"
RECONFIGURE="${FLEET_RECONFIGURE:-0}"

unlock_keychain() {
  local password="${FLEET_KEYCHAIN_PASSWORD:-}"
  [[ -n "$password" ]] || return 0
  local keychain="${HOME}/Library/Keychains/login.keychain-db"
  [[ -f "$keychain" ]] || keychain="${HOME}/Library/Keychains/login.keychain"
  [[ -f "$keychain" ]] || return 0
  echo "== Unlocking login keychain for codesign =="
  security unlock-keychain -p "$password" "$keychain" 2>/dev/null || true
  security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$password" "$keychain" 2>/dev/null || true
}

git_pull_deskflow() {
  [[ "${FLEET_SKIP_GIT_PULL:-0}" == "1" ]] && return 0
  echo "== [$HOSTNAME] deskflow pull ($BRANCH) =="
  cd "$DESKFLOW_ROOT"
  git fetch origin
  git checkout "$BRANCH"
  git pull --ff-only origin "$BRANCH"
  git log -1 --oneline
}

configure_deskflow() {
  cd "$DESKFLOW_ROOT"
  if [[ -f .env ]]; then
    set -a
    # shellcheck disable=SC1091
    source .env
    set +a
  fi
  local codesign_id="${DESKFLOW_CODESIGN_ID:-}"
  if [[ -z "$codesign_id" ]]; then
    codesign_id="$(security find-identity -v -p codesigning 2>/dev/null | awk '/Apple Development/{print $2; exit}')"
  fi
  if [[ -z "$codesign_id" ]]; then
    echo "error: DESKFLOW_CODESIGN_ID not set in .env and no Apple Development cert found" >&2
    exit 1
  fi
  if [[ ! -f build/CMakeCache.txt ]] || [[ "$RECONFIGURE" == "1" ]]; then
    echo "== [$HOSTNAME] cmake configure (signed) =="
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$(brew --prefix qt);$(brew --prefix openssl@3)" \
      -DAPPLE_CODESIGN_DEV="$codesign_id" \
      -DSKIP_BUILD_TESTS=ON \
      -DBUILD_TESTS=OFF
  fi
}

build_install_deskflow() {
  cd "$DESKFLOW_ROOT"
  echo "== [$HOSTNAME] deskflow build + install =="
  cmake --build build --target deskflow-core Deskflow deskflow-vhid-bridge -j"$(sysctl -n hw.ncpu)"
  bash scripts/install-macos.sh
  if codesign --verify --deep --strict /Applications/Deskflow.app 2>/dev/null; then
    echo "== [$HOSTNAME] codesign verify OK =="
  else
    echo "warning: [$HOSTNAME] codesign verify failed — build may be unsigned" >&2
  fi
}

deploy_mouser() {
  [[ "$DEPLOY_MOUSER" == "1" ]] || return 0
  [[ -d "$MOUSER_ROOT" ]] || { echo "skip Mouser: missing $MOUSER_ROOT"; return 0; }
  echo "== [$HOSTNAME] Mouser build + install =="
  cd "$MOUSER_ROOT"
  if [[ -d .git ]]; then
    git pull --ff-only 2>/dev/null || true
  fi
  python3 scripts/build_and_install.py
}

main() {
  echo "=== fleet-deploy-macos on $(hostname -s) ==="
  unlock_keychain
  if [[ "$DEPLOY_DESKFLOW" == "1" ]]; then
    git_pull_deskflow
    configure_deskflow
    build_install_deskflow
  fi
  deploy_mouser
  echo "=== done: $(hostname -s) ==="
}

main "$@"
