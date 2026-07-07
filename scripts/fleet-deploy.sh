#!/usr/bin/env bash
# Fleet redeploy orchestrator — run from hackintosh.
# Pulls refactor/fleet-state-hub on each machine and runs signed local build/install.
#
# Setup:
#   cp scripts/fleet.env.example scripts/fleet.env
#   bash scripts/fleet-setup-ssh.sh          # passwordless SSH
#   # Edit fleet.env: keychain passwords for remote Mac signing
#   bash scripts/fleet-deploy.sh
#
# Options:
#   --pull-only       git pull only, no build
#   --deskflow-only   skip Mouser
#   --mouser-only     skip Deskflow
#   --host NAME       deploy one host (hackintosh|macbookpro|tiny11)
#   --reconfigure     force cmake configure on Macs
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ROOT}/scripts/fleet.env"
EXAMPLE="${ROOT}/scripts/fleet.env.example"

PULL_ONLY=0
FILTER_HOST=""
EXTRA_ENV=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pull-only) PULL_ONLY=1; shift ;;
    --deskflow-only) EXTRA_ENV+=(FLEET_DEPLOY_MOUSER=0); shift ;;
    --mouser-only) EXTRA_ENV+=(FLEET_DEPLOY_DESKFLOW=0); shift ;;
    --reconfigure) EXTRA_ENV+=(FLEET_RECONFIGURE=1); shift ;;
    --host)
      FILTER_HOST="$2"
      shift 2
      ;;
    -h|--help)
      sed -n '2,20p' "$0"
      exit 0
      ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

if [[ ! -f "$ENV_FILE" ]]; then
  echo "Missing $ENV_FILE — copy from fleet.env.example:" >&2
  echo "  cp scripts/fleet.env.example scripts/fleet.env" >&2
  exit 1
fi

# shellcheck disable=SC1091
source "$ENV_FILE"

FLEET_BRANCH="${FLEET_BRANCH:-refactor/fleet-state-hub}"
FLEET_HOSTS="${FLEET_HOSTS:-hackintosh macbookpro tiny11}"
LOCAL_ID="$(hostname -s | tr '[:upper:]' '[:lower:]')"

deploy_macos() {
  local host_id="$1"
  local ssh_target="$2"
  local user="${3:-$USER}"
  local keychain_var="FLEET_KEYCHAIN_PASSWORD_${host_id}"
  local keychain_pass="${!keychain_var:-}"
  local deskflow_path="${FLEET_DESKFLOW_PATH_macos:-~/Desktop/deskflow}"
  local mouser_path="${FLEET_MOUSER_PATH_macos:-~/Desktop/Mouser}"
  local deploy_df="${FLEET_DEPLOY_DESKFLOW:-1}"
  local deploy_m="${FLEET_DEPLOY_MOUSER:-1}"
  local reconf="${FLEET_RECONFIGURE:-0}"

  for kv in ${EXTRA_ENV[@]+"${EXTRA_ENV[@]}"}; do
    case "$kv" in
      FLEET_DEPLOY_MOUSER=*) deploy_m="${kv#*=}" ;;
      FLEET_DEPLOY_DESKFLOW=*) deploy_df="${kv#*=}" ;;
      FLEET_RECONFIGURE=*) reconf="${kv#*=}" ;;
    esac
  done

  if [[ "$PULL_ONLY" == "1" ]]; then
    echo ">>> pull: $host_id"
    if [[ "$ssh_target" == "local" ]]; then
      cd "${deskflow_path/#\~/$HOME}"
      git fetch origin && git checkout "$FLEET_BRANCH" && git pull --ff-only origin "$FLEET_BRANCH"
      git log -1 --oneline
    else
      ssh "${user}@${ssh_target}" "cd '${deskflow_path}' && git fetch origin && git checkout '${FLEET_BRANCH}' && git pull --ff-only origin '${FLEET_BRANCH}' && git log -1 --oneline"
    fi
    return 0
  fi

  local script="${ROOT}/scripts/fleet-deploy-macos.sh"
  if [[ "$ssh_target" == "local" ]]; then
    echo ">>> LOCAL deploy: $host_id"
    export FLEET_BRANCH="$FLEET_BRANCH"
    export FLEET_DEPLOY_DESKFLOW="$deploy_df"
    export FLEET_DEPLOY_MOUSER="$deploy_m"
    export FLEET_RECONFIGURE="$reconf"
    export FLEET_DESKFLOW_ROOT="${deskflow_path/#\~/$HOME}"
    export FLEET_MOUSER_ROOT="${mouser_path/#\~/$HOME}"
    export FLEET_KEYCHAIN_PASSWORD="$keychain_pass"
    bash "$script"
  else
    echo ">>> SSH deploy: $host_id ($user@$ssh_target)"
    ssh -t "${user}@${ssh_target}" \
      "set -euo pipefail; \
       export FLEET_BRANCH='${FLEET_BRANCH}'; \
       export FLEET_DEPLOY_DESKFLOW='${deploy_df}'; \
       export FLEET_DEPLOY_MOUSER='${deploy_m}'; \
       export FLEET_RECONFIGURE='${reconf}'; \
       export FLEET_DESKFLOW_ROOT='${deskflow_path}'; \
       export FLEET_MOUSER_ROOT='${mouser_path}'; \
       export FLEET_KEYCHAIN_PASSWORD='${keychain_pass}'; \
       export FLEET_SKIP_GIT_PULL=1; \
       cd '${deskflow_path}' && \
       git fetch origin && git checkout '${FLEET_BRANCH}' && git pull --ff-only origin '${FLEET_BRANCH}' && \
       bash scripts/fleet-deploy-macos.sh"
  fi
}

deploy_windows() {
  local host_id="$1"
  local ssh_target="$2"
  local user="${3:-alexh}"
  local deskflow_path="${FLEET_DESKFLOW_PATH_windows:-C:/Users/alexh/Desktop/deskflow}"
  local deploy_df="${FLEET_DEPLOY_DESKFLOW:-1}"
  local deploy_m="${FLEET_DEPLOY_MOUSER:-1}"

  for kv in ${EXTRA_ENV[@]+"${EXTRA_ENV[@]}"}; do
    case "$kv" in
      FLEET_DEPLOY_MOUSER=*) deploy_m="${kv#*=}" ;;
      FLEET_DEPLOY_DESKFLOW=*) deploy_df="${kv#*=}" ;;
    esac
  done

  if [[ "$PULL_ONLY" == "1" ]]; then
    echo ">>> pull: $host_id"
    ssh "${user}@${ssh_target}" "cd /d \"${deskflow_path}\" && git fetch origin && git checkout ${FLEET_BRANCH} && git pull --ff-only origin ${FLEET_BRANCH} && git log -1 --oneline"
    return 0
  fi

  echo ">>> SSH deploy: $host_id ($user@$ssh_target)"
  local ps_cmd
  ps_cmd="\
\$env:FLEET_BRANCH='${FLEET_BRANCH}'; \
\$env:FLEET_DEPLOY_DESKFLOW='${deploy_df}'; \
\$env:FLEET_DEPLOY_MOUSER='${deploy_m}'; \
\$env:FLEET_DESKFLOW_ROOT='${deskflow_path}'; \
\$env:FLEET_MOUSER_ROOT='${FLEET_MOUSER_PATH_windows:-C:/Users/alexh/Desktop/Mouser}'; \
& '${deskflow_path}/scripts/fleet-deploy-windows.ps1'"

  ssh "${user}@${ssh_target}" "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"$ps_cmd\""
}

for host_id in $FLEET_HOSTS; do
  [[ -z "$FILTER_HOST" || "$host_id" == "$FILTER_HOST" ]] || continue

  ssh_var="FLEET_SSH_${host_id}"
  target="${!ssh_var:-}"
  user_var="FLEET_SSH_USER_${host_id}"
  user="${!user_var:-}"

  if [[ "$host_id" == "tiny11" ]]; then
    user="${user:-alexh}"
    [[ -n "$target" ]] || target="tiny11"
    deploy_windows "$host_id" "$target" "$user"
  else
    # Treat current machine as local when host id matches or target is local
    if [[ "$target" == "local" ]] || [[ "$(echo "$host_id" | tr '[:upper:]' '[:lower:]')" == "$LOCAL_ID" ]]; then
      target="local"
    fi
    [[ -n "$target" ]] || target="$host_id"
    user="${user:-$USER}"
    deploy_macos "$host_id" "$target" "$user"
  fi
done

echo "=== Fleet deploy complete ==="
