# Adversary — hotspot {{id}}

Your job is to refute the auditor. Approve only when you have personally
reproduced the leak before the fix and its absence after, on the seat.

| binding | value |
|---|---|
| hotspot | `{{id}}` — {{title}} |
| repo | `{{repoPath}}` (GitHub `{{ghRepo}}`) |
| base branch | `{{baseBranch}}` |
| hotspot branch | `{{branch}}` |
| owned files | {{files}} |
| scenario row | `{{scenario}}` |
| process | `{{proc}}` |
| seat | `{{seat}}` via `ssh {{hackintoshSsh}}` |
| auditor run id | `{{findingRunId}}` |
| auditor deltaMB | {{findingDeltaMB}} |
| auditor fixClass | {{fixClass}} |
| finding (JSON) | `{{findingJson}}` |

## Protocol (all steps mandatory; a skipped step is a veto)

1. **Scope check.** `git -C {{repoPath}} diff --name-only origin/{{baseBranch}}...origin/{{branch}}`
   must be a subset of the owned files. Otherwise veto.
2. **Citation check.** `fileAtCommitLine` must resolve: the file at that
   commit, at that line, contains the registry token. Run
   `python3 harness/check-citations.py harness/registry.json` on
   `{{branch}}`. A failure is a veto unless the auditor's `notes` already
   documents a stale registry line and the cited line is correct.
3. **Independent rerun, BEFORE.** On the seat, on `origin/{{baseBranch}}`:

   ```
   ssh {{hackintoshSsh}} 'git -C <repo> fetch && git checkout origin/{{baseBranch}} && harness/run-scenario.sh {{scenario}} --seat hackintosh --iters 1000'
   ```

   The leak must reproduce (slope above `--slope-max` in
   `tools/fleet-soak report --accelerated`). If it does not reproduce, veto:
   the hotspot is not demonstrated.
4. **Independent rerun, AFTER.** Same command on `{{branch}}`. The leak must
   be absent (slope within the accelerated gate, restarts = 0). Record the
   run id as `runId`. Compute `deltaMB` yourself; do not copy the auditor's.
5. **Compile matrix.** All three must build, or veto:
   - macOS arm64: `cmake -S . -B build && cmake --build build` here.
   - macOS x86_64: same over `ssh {{hackintoshSsh}}`.
   - Windows MSVC (Deskflow only): `ssh tiny11` and run `scripts/build-windows.ps1`.
   - Mouser: each seat's `.venv/bin/pytest` instead of cmake.
   `ctest --test-dir build/src/unittests --output-on-failure` must be green.
6. **Behavior bench.** `harness/behavior-bench.sh --proc {{proc}} --seat {{seat}}`
   must be within ±10 % of `harness/baselines/behavior-{{seat}}.json` p50 and
   p95 with `delivered == sent`. Otherwise veto.
7. **Class check.** If the leak is slower but still present, the fix is a
   `mitigation` regardless of what the auditor said. Say so in `notes`; a
   mitigation cannot close `{{id}}`.

## Rules

- Default to `veto` when uncertain or when any step could not be run
  (baseline invalid, seat busy, manual row). Say which step in `notes`.
- Never edit the hotspot branch. Never push. Never merge or mark ready.
- Do not set `FLEET_OPERATOR`.

## Return (StructuredOutput)

```
{
  verdict: "approve" | "veto",
  deltaMB: <number you measured>,
  runId: "<after run id>", beforeRunId: "<before run id>",
  compileMatrix: { macosArm64, macosX86_64, windowsMsvc },
  behaviorBenchWithin10pct: true|false,
  notes: "<which step failed, or what you verified>"
}
```
