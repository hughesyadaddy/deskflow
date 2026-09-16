#!/usr/bin/env bash
# check-pr.sh <pr-number> [--body-file F] [--registry R] [--repo NAME]
#
# Exits 0 only when the PR body carries a `run-id:` line and a `deltaMB:` line
# and `harness/check-citations.py <registry>` passes. The body comes from
# --body-file when given, else `gh pr view <pr-number> --json body`.
set -euo pipefail

usage() {
  echo "usage: $0 <pr-number> [--body-file F] [--registry R] [--repo NAME]" >&2
  exit 2
}

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pr=""
body_file=""
registry="${here}/registry.json"
repo_name=""

while [ $# -gt 0 ]; do
  case "$1" in
    --body-file) [ $# -ge 2 ] || usage; body_file="$2"; shift 2 ;;
    --registry)  [ $# -ge 2 ] || usage; registry="$2"; shift 2 ;;
    --repo)      [ $# -ge 2 ] || usage; repo_name="$2"; shift 2 ;;
    -h|--help)   usage ;;
    -*)          echo "unknown option: $1" >&2; usage ;;
    *)           [ -z "$pr" ] || usage; pr="$1"; shift ;;
  esac
done

[ -n "$pr" ] || [ -n "$body_file" ] || usage

if [ -n "$body_file" ]; then
  [ -r "$body_file" ] || { echo "check-pr: cannot read body file $body_file" >&2; exit 2; }
  body="$(cat "$body_file")"
else
  command -v gh >/dev/null 2>&1 || { echo "check-pr: gh not found and no --body-file" >&2; exit 2; }
  body="$(gh pr view "$pr" --json body --jq .body)"
fi

fail=0
if ! printf '%s\n' "$body" | grep -Eq '^[[:space:]]*run-id:[[:space:]]*[^[:space:]]+'; then
  echo "check-pr: PR body is missing a 'run-id: <id>' line (no run id => hypothesis, not a finding)"
  fail=1
fi
if ! printf '%s\n' "$body" | grep -Eq '^[[:space:]]*deltaMB:[[:space:]]*-?[0-9]+(\.[0-9]+)?'; then
  echo "check-pr: PR body is missing a numeric 'deltaMB: <n>' line"
  fail=1
fi

cite_args=("$registry")
[ -n "$repo_name" ] && cite_args+=(--repo "$repo_name")
if ! python3 "${here}/check-citations.py" "${cite_args[@]}"; then
  echo "check-pr: check-citations.py failed for $registry"
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  echo "check-pr: FAIL${pr:+ (PR #$pr)}"
  exit 1
fi
echo "check-pr: OK${pr:+ (PR #$pr)}"
