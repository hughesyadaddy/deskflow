#!/usr/bin/env bats
# Tests for harness/run-scenario.sh. tools/fleet-soak is stubbed through
# FLEET_SOAK_BIN and the hostname through FLEET_HOSTNAME; no real driver runs.
#
#   tools/tests/bats/bin/bats tools/tests/test_run_scenario.bats

REPO="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
RUN="$REPO/harness/run-scenario.sh"
ROWS="mouse-absent-reconnect bt-sleep-wake window-toggle add-profile scroll-im-denied scroll-im-granted screen-switch clipboard-10mb epoch-flip peer-unreachable lock-unlock login-bridge"

setup() {
  export TMP="$BATS_TEST_TMPDIR"
  export HARNESS_RUNS_DIR="$TMP/runs"
  export HARNESS_BASELINES_DIR="$TMP/baselines"
  export HARNESS_LOCK_FILE="$TMP/hackintosh.lock"
  export HARNESS_SAMPLE_INTERVAL=1
  export FLEET_SOAK_BIN="$TMP/fleet-soak"
  export STUB_LOG="$TMP/fleet-soak.calls"
  unset FLEET_OPERATOR FLEET_NO_BASELINE_GATE FLEET_HOSTNAME HARNESS_SCENARIOS_DIR
  mkdir -p "$HARNESS_BASELINES_DIR" "$TMP/scenarios"
  write_stub 0
  # trivial full-automation driver used by the run tests
  cat >"$TMP/scenarios/trivial.sh" <<'EOF'
# proc: deskflow-core
# automation: full
# seat: hackintosh
# description: trivial driver for run-scenario tests
scenario_setup() { echo "setup" >>"$TMP/driver.log"; }
scenario_iter() { echo "iter $1" >>"$TMP/driver.log"; }
scenario_teardown() { echo "teardown" >>"$TMP/driver.log"; }
EOF
  cat >"$TMP/scenarios/failing.sh" <<'EOF'
# proc: mouser
# automation: full
# seat: hackintosh
# description: iteration 2 fails
scenario_setup() { :; }
scenario_iter() { [ "$1" != 2 ]; }
scenario_teardown() { echo "teardown" >>"$TMP/driver.log"; }
EOF
  cat >"$TMP/scenarios/manual-row.sh" <<'EOF'
# proc: mouser
# automation: manual
# seat: hackintosh
# description: manual row for tests
scenario_setup() { :; }
scenario_iter() { echo "manual iter $1" >>"$TMP/driver.log"; }
scenario_teardown() { :; }
EOF
}

# write_stub <report-exit-code>: fleet-soak stub. `sample --once` emits a
# JSON line whose phys_footprint grows 1 MiB per call and appends to --out.
write_stub() {
  cat >"$FLEET_SOAK_BIN" <<EOF
#!/usr/bin/env bash
echo "\$*" >>"$STUB_LOG"
case "\$1" in
  sample)
    out=""; while [ \$# -gt 0 ]; do [ "\$1" = --out ] && out="\$2"; shift; done
    n=\$(grep -c '^sample' "$STUB_LOG")
    line="{\"ts\":\$n,\"phys_footprint\":\$((100*1048576 + n*1048576)),\"ports\":\$((50 + n))}"
    echo "\$line"; [ -n "\$out" ] && echo "\$line" >>"\$out"; exit 0 ;;
  report) exit $1 ;;
esac
exit 99
EOF
  chmod +x "$FLEET_SOAK_BIN"
}

@test "--print-proc prints a valid proc for all 12 rows" {
  for row in $ROWS; do
    run "$RUN" "$row" --print-proc
    [ "$status" -eq 0 ] || { echo "$row: exit $status: $output"; false; }
    case "$output" in mouser|deskflow-core|deskflow) ;; *) echo "$row: bad proc '$output'"; false ;; esac
  done
}

@test "every shipped driver declares the header and the three hooks" {
  for row in $ROWS; do
    f="$REPO/harness/scenarios/$row.sh"
    [ -f "$f" ]
    grep -Eq '^# automation: (full|partial|manual)$' "$f"
    grep -q '^# seat: hackintosh$' "$f"
    grep -q '^# description: .' "$f"
    for fn in scenario_setup scenario_iter scenario_teardown; do
      grep -q "^$fn()" "$f" || { echo "$row lacks $fn"; false; }
    done
  done
}

@test "the three operator rows are manual and everything else is not" {
  for row in bt-sleep-wake lock-unlock login-bridge; do
    grep -q '^# automation: manual$' "$REPO/harness/scenarios/$row.sh"
  done
  for row in $ROWS; do
    case "$row" in bt-sleep-wake|lock-unlock|login-bridge) continue ;; esac
    ! grep -q '^# automation: manual$' "$REPO/harness/scenarios/$row.sh"
  done
}

@test "partial rows call harness_todo; full rows do not" {
  for row in $ROWS; do
    f="$REPO/harness/scenarios/$row.sh"
    if grep -q '^# automation: partial$' "$f"; then
      grep -q 'harness_todo' "$f" || { echo "$row is partial without harness_todo"; false; }
    elif grep -q '^# automation: full$' "$f"; then
      ! grep -q 'harness_todo' "$f" || { echo "$row is full but calls harness_todo"; false; }
    fi
  done
}

@test "unknown row exits 2" {
  run "$RUN" no-such-row --seat macbookpro --iters 1
  [ "$status" -eq 2 ]
}

@test "manual row without FLEET_OPERATOR exits 4 and runs nothing" {
  export HARNESS_SCENARIOS_DIR="$TMP/scenarios"
  run "$RUN" manual-row --seat macbookpro --iters 3
  [ "$status" -eq 4 ]
  [[ "$output" == *FLEET_OPERATOR=1* ]]
  [ ! -f "$TMP/driver.log" ]
  [ ! -f "$STUB_LOG" ]
}

@test "shipped manual rows exit 4 without FLEET_OPERATOR" {
  for row in bt-sleep-wake lock-unlock login-bridge; do
    run "$RUN" "$row" --seat hackintosh --iters 1
    [ "$status" -eq 4 ] || { echo "$row: exit $status"; false; }
  done
  [ ! -f "$STUB_LOG" ]
}

@test "--smoke on a manual row writes the skipped-manual marker and exits 0" {
  out="$TMP/bt.jsonl"
  run "$RUN" bt-sleep-wake --seat hackintosh --iters 5 --smoke --out "$out"
  [ "$status" -eq 0 ]
  [ -f "$out" ]
  grep -q '"header":true' "$out"
  grep -q '"smoke":"skipped-manual"' "$out"
  grep -q '"scenario_source":"manual"' "$out"
  [ ! -f "$STUB_LOG" ]
}

@test "manual row runs with FLEET_OPERATOR=1" {
  export HARNESS_SCENARIOS_DIR="$TMP/scenarios" FLEET_OPERATOR=1
  run "$RUN" manual-row --seat macbookpro --iters 2
  [ "$status" -eq 0 ]
  grep -q 'manual iter 2' "$TMP/driver.log"
}

@test "full row runs setup, N iterations, teardown with the stubbed sampler" {
  export HARNESS_SCENARIOS_DIR="$TMP/scenarios"
  run "$RUN" trivial --seat macbookpro --iters 3
  [ "$status" -eq 0 ]
  [ "$(cat "$TMP/driver.log")" = $'setup\niter 1\niter 2\niter 3\nteardown' ]
  # sampler: before + after (interval sampler may or may not have fired)
  [ "$(grep -c '^sample --once --label deskflow-core --out' "$STUB_LOG")" -ge 2 ]
  # out file: header + samples
  out="$HARNESS_RUNS_DIR/trivial.jsonl"
  grep -q '"header":true' "$out"
  grep -q '"scenario_source":"scripted"' "$out"
  grep -q '"phys_footprint"' "$out"
  # log under runs/logs/<proc>/<row>-<ts>.log
  ls "$HARNESS_RUNS_DIR/logs/deskflow-core/" | grep -Eq '^trivial-[0-9T]+Z\.log$'
  # deltas and run id printed
  [[ "$output" == *"run-id: trivial-"* ]]
  [[ "$output" == *"deltaMB: +"* ]]
  [[ "$output" == *"Δ phys_footprint"* ]]
  [[ "$output" == *"Δ ports"* ]]
}

@test "iteration failure exits 1 and still runs teardown" {
  export HARNESS_SCENARIOS_DIR="$TMP/scenarios"
  run "$RUN" failing --seat macbookpro --iters 5
  [ "$status" -eq 1 ]
  [ "$(grep -c teardown "$TMP/driver.log")" -eq 1 ]
  [[ "$output" == *"iteration 2 failed"* ]]
}

@test "hackintosh baseline gate exits 3 when fleet-soak report exits 2" {
  export HARNESS_SCENARIOS_DIR="$TMP/scenarios" FLEET_HOSTNAME=hackintosh
  touch "$HARNESS_BASELINES_DIR/2026-09-10-hackintosh-deskflow-core.json"
  touch "$HARNESS_BASELINES_DIR/2026-09-14-hackintosh-deskflow-core.json"
  write_stub 2
  run "$RUN" trivial --seat hackintosh --iters 1
  [ "$status" -eq 3 ]
  grep -q 'report --in .*2026-09-14-hackintosh-deskflow-core.json --min-hours 72 --validate' "$STUB_LOG"
  [ ! -f "$TMP/driver.log" ]
  [ ! -d "$HARNESS_LOCK_FILE.d" ]
}

@test "hackintosh baseline gate exits 3 when no baseline exists" {
  export HARNESS_SCENARIOS_DIR="$TMP/scenarios" FLEET_HOSTNAME=hackintosh
  run "$RUN" trivial --seat hackintosh --iters 1
  [ "$status" -eq 3 ]
}

@test "hackintosh gate passes when report exits 0 and the lock is released" {
  export HARNESS_SCENARIOS_DIR="$TMP/scenarios" FLEET_HOSTNAME=hackintosh
  touch "$HARNESS_BASELINES_DIR/2026-09-14-hackintosh-deskflow-core.json"
  run "$RUN" trivial --seat hackintosh --iters 1
  [ "$status" -eq 0 ]
  grep -q '^report' "$STUB_LOG"
  [ ! -d "$HARNESS_LOCK_FILE.d" ]
}

@test "--smoke and FLEET_NO_BASELINE_GATE skip the gate on hackintosh" {
  export HARNESS_SCENARIOS_DIR="$TMP/scenarios" FLEET_HOSTNAME=hackintosh
  write_stub 2
  run "$RUN" trivial --seat hackintosh --iters 1 --smoke
  [ "$status" -eq 0 ]
  ! grep -q '^report' "$STUB_LOG"
  export FLEET_NO_BASELINE_GATE=1
  run "$RUN" trivial --seat hackintosh --iters 1
  [ "$status" -eq 0 ]
  ! grep -q '^report' "$STUB_LOG"
}

@test "gate is not applied when the seat is hackintosh but the host is not" {
  export HARNESS_SCENARIOS_DIR="$TMP/scenarios" FLEET_HOSTNAME=macbookpro
  write_stub 2
  run "$RUN" trivial --seat hackintosh --iters 1
  [ "$status" -eq 0 ]
  ! grep -q '^report' "$STUB_LOG"
}

@test "hackintosh lock serializes: a held mkdir lock blocks until released" {
  export HARNESS_SCENARIOS_DIR="$TMP/scenarios" FLEET_HOSTNAME=hackintosh FLEET_NO_BASELINE_GATE=1
  if command -v flock >/dev/null 2>&1; then skip "flock present; mkdir fallback not exercised"; fi
  mkdir "$HARNESS_LOCK_FILE.d"
  ( sleep 2; rm -rf "$HARNESS_LOCK_FILE.d" ) &
  start=$(date +%s)
  run "$RUN" trivial --seat hackintosh --iters 1
  end=$(date +%s)
  [ "$status" -eq 0 ]
  [ $((end - start)) -ge 2 ]
  [[ "$output" == *"waiting for"* ]]
  [ ! -d "$HARNESS_LOCK_FILE.d" ]
}
