#!/usr/bin/env bash
# check-pr.sh <pr-number> [--body-file F] [--registry R] [--repo NAME] [--runs-dir D]
#
# Exits 0 only when
#   * the PR body carries a `run-id:` line whose id is the `run_id` of a
#     header line in <runs-dir>/*.jsonl (a run that actually happened here),
#   * the PR body carries a numeric `deltaMB:` line equal to the `deltaMB:`
#     that harness/run-scenario.sh logged for that run in
#     <runs-dir>/logs/<proc>/<run-id>.log,
#   * `harness/check-citations.py <registry>` passes.
# The body comes from --body-file when given, else `gh pr view <pr-number>
# --json body`. <runs-dir> defaults to harness/runs (HARNESS_RUNS_DIR).
set -euo pipefail

usage() {
  echo "usage: $0 <pr-number> [--body-file F] [--registry R] [--repo NAME] [--runs-dir D]" >&2
  exit 2
}

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pr=""
body_file=""
registry="${here}/registry.json"
repo_name=""
runs_dir="${HARNESS_RUNS_DIR:-$here/runs}"

while [ $# -gt 0 ]; do
  case "$1" in
    --body-file) [ $# -ge 2 ] || usage; body_file="$2"; shift 2 ;;
    --registry)  [ $# -ge 2 ] || usage; registry="$2"; shift 2 ;;
    --repo)      [ $# -ge 2 ] || usage; repo_name="$2"; shift 2 ;;
    --runs-dir)  [ $# -ge 2 ] || usage; runs_dir="$2"; shift 2 ;;
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
run_id="$(printf '%s\n' "$body" | sed -nE 's/^[[:space:]]*run-id:[[:space:]]*([^[:space:]]+).*/\1/p' | head -n1)"
delta="$(printf '%s\n' "$body" | sed -nE 's/^[[:space:]]*deltaMB:[[:space:]]*([-+]?[0-9]+(\.[0-9]+)?)([^0-9.].*)?$/\1/p' | head -n1)"

if [ -z "$run_id" ]; then
  echo "check-pr: PR body is missing a 'run-id: <id>' line (no run id => hypothesis, not a finding)"
  fail=1
fi
if [ -z "$delta" ]; then
  echo "check-pr: PR body is missing a numeric 'deltaMB: <n>' line"
  fail=1
fi

# verify_run <run-id> <deltaMB>: the id must be a header run_id in
# <runs-dir>/*.jsonl and the logged deltaMB must equal the PR's.
verify_run() {
  RUNS_DIR="$runs_dir" python3 - "$1" "$2" <<'PY'
import glob, json, os, re, sys
run_id, claimed = sys.argv[1], sys.argv[2]
runs = os.environ["RUNS_DIR"]
proc = None
for path in sorted(glob.glob(os.path.join(runs, "*.jsonl"))):
    try:
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if obj.get("header") and obj.get("run_id") == run_id:
                    if obj.get("smoke") == "skipped-manual" or obj.get("scenario_source") != "scripted":
                        print(f"check-pr: run-id {run_id} in {path} is not a scripted run "
                              f"(scenario_source={obj.get('scenario_source')!r})")
                        sys.exit(1)
                    proc = obj.get("proc")
                    break
    except OSError:
        continue
    if proc is not None:
        break
if proc is None:
    print(f"check-pr: run-id {run_id} not found in any header of {runs}/*.jsonl (no run id => hypothesis, not a finding)")
    sys.exit(1)
log = os.path.join(runs, "logs", str(proc), f"{run_id}.log")
try:
    text = open(log, encoding="utf-8").read()
except OSError:
    print(f"check-pr: run log {log} missing for run-id {run_id}")
    sys.exit(1)
m = None
for m in re.finditer(r"^deltaMB:[ \t]*([-+]?[0-9]+(?:\.[0-9]+)?|n/a)[ \t]*$", text, re.M):
    pass
if m is None:
    print(f"check-pr: {log} has no 'deltaMB:' line for run-id {run_id}")
    sys.exit(1)
logged = m.group(1)
if logged == "n/a":
    print(f"check-pr: run {run_id} logged 'deltaMB: n/a' (sampler produced no footprint); not a finding")
    sys.exit(1)
if abs(float(logged) - float(claimed)) > 1e-9:
    print(f"check-pr: PR deltaMB {claimed} != logged deltaMB {logged} in {log}")
    sys.exit(1)
print(f"check-pr: run-id {run_id} verified (proc={proc}, deltaMB={logged})")
PY
}

if [ -n "$run_id" ] && [ -n "$delta" ]; then
  verify_run "$run_id" "$delta" || fail=1
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
