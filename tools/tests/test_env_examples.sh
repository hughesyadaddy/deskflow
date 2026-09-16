#!/usr/bin/env bash
# Guards the per-seat config templates and .gitignore rules from the fleet
# memory program (docs/plan/2026-09-16-feat-fleet-memory-program-part-1-plan.md,
# Phase 1). Run from anywhere inside the repo:
#
#   bash tools/tests/test_env_examples.sh
#
# Exit 0 when every assertion passes; each failure is printed and counted.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2

FLEET_EXAMPLE="scripts/fleet.env.example"
ENV_EXAMPLE="env.example"

fails=0
pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; fails=$((fails + 1)); }
check() { # check <description> <command...>
  local desc="$1"; shift
  if "$@"; then pass "$desc"; else fail "$desc"; fi
}
# shellcheck disable=SC2329  # invoked indirectly through check()
has_key() { # has_key <file> <KEY>  — matches "KEY=" at line start (comment or not)
  grep -Eq "^#? ?${2}=" "$1"
}

# --- files exist / absent ---------------------------------------------------
check "$FLEET_EXAMPLE exists" test -f "$FLEET_EXAMPLE"
check "$ENV_EXAMPLE exists" test -f "$ENV_EXAMPLE"
check ".env.example is absent (env.example is canonical)" test ! -e .env.example
check ".env.example is not tracked" bash -c '! git ls-files --error-unmatch .env.example >/dev/null 2>&1'

# --- no secrets in the fleet template ---------------------------------------
check "no KEYCHAIN_PASSWORD in $FLEET_EXAMPLE" bash -c "! grep -q KEYCHAIN_PASSWORD '$FLEET_EXAMPLE'"
check "no KEYCHAIN_PASSWORD in any scripts/fleet.env*" bash -c '! grep -rq KEYCHAIN_PASSWORD scripts/fleet.env*'
check "no unlock_keychain mention in $FLEET_EXAMPLE" bash -c "! grep -qi unlock.keychain '$FLEET_EXAMPLE'"

# --- required keys: scripts/fleet.env.example -------------------------------
for key in FLEET_BRANCH FLEET_HOSTS \
           FLEET_SSH_hackintosh FLEET_SSH_macbookpro FLEET_SSH_tiny11 \
           FLEET_SSH_USER_hackintosh FLEET_SSH_USER_macbookpro FLEET_SSH_USER_tiny11 \
           FLEET_DEPLOY_DESKFLOW FLEET_DEPLOY_MOUSER FLEET_RECONFIGURE \
           FLEET_DESKFLOW_PATH_macos FLEET_MOUSER_PATH_macos \
           FLEET_DESKFLOW_PATH_windows FLEET_MOUSER_PATH_windows \
           DESKFLOW_SIGN_THUMBPRINT FLEET_OPERATOR; do
  check "$FLEET_EXAMPLE declares $key" has_key "$FLEET_EXAMPLE" "$key"
done
check "$FLEET_EXAMPLE ships with FLEET_OPERATOR=0" grep -Eq '^FLEET_OPERATOR=0$' "$FLEET_EXAMPLE"
check "$FLEET_EXAMPLE has exactly one FLEET_SSH_*=local" \
  bash -c "[ \"\$(grep -Ec '^FLEET_SSH_[A-Za-z0-9_]+=local$' '$FLEET_EXAMPLE')\" = 1 ]"
check "$FLEET_EXAMPLE does not define LOCAL_ID (derived from hostname)" \
  bash -c "! grep -Eq '^LOCAL_ID=' '$FLEET_EXAMPLE'"

# --- required keys: env.example ---------------------------------------------
for key in DESKFLOW_CODESIGN_ID DESKFLOW_QT_PATH OPENSSL_ROOT_DIR \
           DESKFLOW_INSTALL_DIR DESKFLOW_BUILD_DIR DESKFLOW_SIGN_THUMBPRINT; do
  check "$ENV_EXAMPLE declares $key" has_key "$ENV_EXAMPLE" "$key"
done
check "$ENV_EXAMPLE points at 'security find-identity -v -p codesigning'" \
  grep -q 'security find-identity -v -p codesigning' "$ENV_EXAMPLE"

# --- .gitignore: per-seat files and tool state ignored ----------------------
for p in .env scripts/fleet.env tools/state/x harness/runs/x harness/soak/x harness/.hackintosh.lock; do
  check "git ignores $p" git check-ignore -q "$p"
done

# --- .gitignore: tracked tooling NOT ignored --------------------------------
for p in tools/fleet-doctor tools/fleet-health tools/tests/test_env_examples.sh \
         harness/registry.json scripts/fleet.env.example env.example; do
  check "git does not ignore $p" bash -c "! git check-ignore -q '$p'"
done

# --- summary ----------------------------------------------------------------
if [ "$fails" -eq 0 ]; then
  echo "all env example checks passed"
  exit 0
fi
echo "$fails check(s) failed" >&2
exit 1
