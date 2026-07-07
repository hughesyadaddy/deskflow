#!/usr/bin/env bash
# One-time helper: install SSH public key on fleet Macs / Windows for passwordless deploy.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ROOT}/scripts/fleet.env"
EXAMPLE="${ROOT}/scripts/fleet.env.example"

if [[ ! -f "$ENV_FILE" ]]; then
  cp "$EXAMPLE" "$ENV_FILE"
  echo "Created $ENV_FILE — edit FLEET_SSH_* targets, then re-run."
  exit 0
fi

# shellcheck disable=SC1091
source "$ENV_FILE"

KEY="${FLEET_SSH_KEY:-$HOME/.ssh/id_ed25519}"
PUB="${KEY}.pub"

if [[ ! -f "$PUB" ]]; then
  echo "Generating SSH key at $KEY"
  ssh-keygen -t ed25519 -f "$KEY" -N "" -C "deskflow-fleet-deploy@$(hostname -s)"
fi

echo "Public key:"
cat "$PUB"
echo

for host_id in $FLEET_HOSTS; do
  var="FLEET_SSH_${host_id}"
  target="${!var:-}"
  [[ "$target" == "local" || -z "$target" ]] && continue

  user_var="FLEET_SSH_USER_${host_id}"
  user="${!user_var:-$USER}"

  if [[ "$host_id" == "tiny11" ]]; then
    echo "== Copy key to Windows $host_id ($user@$target) =="
    echo "Run on tiny11 (as $user) in elevated PowerShell if needed:"
    echo "  mkdir \$env:USERPROFILE\\.ssh -Force"
    echo "  Add-Content \$env:USERPROFILE\\.ssh\\authorized_keys '<paste pub key>'"
    echo "Or use: ssh-copy-id ${user}@${target}  (if OpenSSH server configured)"
    ssh-copy-id -i "$PUB" "${user}@${target}" 2>/dev/null || \
      echo "  manual: ssh ${user}@${target} and add key to authorized_keys"
  else
    echo "== Copy key to $host_id ($user@$target) =="
    ssh-copy-id -i "$PUB" "${user}@${target}" || true
  fi
done

echo
echo "Test: bash scripts/fleet-deploy.sh --pull-only"
