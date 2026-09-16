# harness/ — fleet memory program

Tooling for the hotspot swarm described in
`docs/plan/2026-09-16-feat-fleet-memory-program-part-1-plan.md`
(§Swarm workflow contract, §Swarm protocol). This directory holds the
registry, the Workflow script that drives one auditor + adversary pair per
hotspot, the prompt templates those agents follow, and the two PR gates.

| path | role |
|---|---|
| `registry.json` | hotspot registry: `{id, repo, title, files[], scenario, proc, seat, budgetMB, evidence{file,line,token}, fixClass}`; optional `commit` (top-level or per hotspot) pins citations |
| `swarm.workflow.js` | Workflow-tool script: Audit → Adversary → PR pipeline; `--post-merge` mode |
| `prompts/auditor.md`, `prompts/adversary.md` | templates the agents read and bind (`{{id}}`, `{{scenario}}`, `{{hackintoshSsh}}`, …) |
| `check-citations.py` | every registry `evidence` must resolve at the pinned commit |
| `check-pr.sh` | PR gate: body has `run-id:` and `deltaMB:` lines and citations pass |
| `../.github/workflows/pr-harness-check.yml` | runs `check-pr.sh` on PRs against `fleet/memory-program` |

Scenario drivers (`scenarios/`, `run-scenario.sh`, `behavior-bench.sh`,
`baselines/`) are owned by the Phase 5 harness tasks and are documented in the
plan; the workflow only invokes them over `ssh hackintosh`.

## Running the swarm

The script runs under the Workflow tool, which has **no filesystem**: pass the
parsed registry object in `args.registry` (or an absolute path — the script
then spends one low-effort agent to read it, which `dryRun` refuses).

```
Workflow({ name: 'fleet-memory-swarm', args: {
  repoPath: '/Users/alexhughes/Desktop/deskflow',
  ghRepo: 'hughesyadaddy/deskflow',
  registry: <parsed harness/registry.json>,
  baseBranch: 'fleet/memory-program',   // default
  hackintoshSsh: 'hackintosh',          // default
  dryRun: false, postMerge: false       // defaults
}})
```

- `dryRun: true` spawns nothing and returns
  `{repo, hotspots: [{id, branch, scenario, proc, files, …}], disjoint: true}`.
- Normal mode: for each hotspot whose `repo` equals `basename(repoPath)`,
  `pipeline(hotspots, auditor, adversary, pr)`:
  1. **Auditor** (`audit:<id>`) — branch `hotspot/<id>` from `baseBranch`,
     reproduce before/after on hackintosh with
     `harness/run-scenario.sh <scenario> --seat hackintosh --iters 1000`, fix, test, push.
     Returns the finding schema. No run id → hypothesis; the adversary is
     skipped.
  2. **Adversary** (`adversary:<id>`) — independent before/after rerun,
     compile matrix (macOS arm64, macOS x86_64, Windows MSVC), behavior bench
     within ±10 %. Returns `{verdict: approve|veto, deltaMB, notes}`.
  3. **PR** (`pr:<id>`) — on `approve` only:
     `gh pr create --draft --base <baseBranch> --head hotspot/<id> --label needs-harness`
     with a body carrying `run-id:` and `deltaMB:` lines (and `mitigation:`
     for mitigation-class fixes, which never close a hotspot). The agent then
     runs `check-pr.sh <pr-number>` and reports its exit code. Nothing is
     merged or marked ready by the script.
- Returns `[{id, prUrl, deltaMB, verdict, fixClass, checkPrExit}]` where
  `verdict` is `approve | veto | hypothesis | audit-failed | adversary-failed`.
- `postMerge: true` — after Alex merges PRs ranked by `deltaMB`: rebase every
  open `hotspot/*` branch onto `baseBranch`, force-with-lease push, rerun the
  compile matrix. Returns `[{id, verdict: rebased | rebased-matrix-red | rebase-conflict, notes}]`.

Registry `files[]` must be pairwise disjoint within a repo; the script throws
before spawning anything otherwise (`assertDisjoint`).

## Gates

```
python3 harness/check-citations.py harness/registry.json [--repo NAME] [--commit REV] [--worktree] [--all]
harness/check-pr.sh <pr-number> [--body-file F] [--registry R] [--repo NAME]
```

`check-citations.py` selects hotspots by repo: `--repo`, else the checkout's
toplevel basename, else the `origin` URL basename (so it works inside git
worktrees). Exit 0 all resolve, 1 with one `FAIL <id>: …` line per bad
citation (with a hint of where the token actually is), 2 on usage errors —
including an explicit `--repo` that matches nothing.

`check-pr.sh` reads the body from `--body-file` or `gh pr view <n> --json body`.

## Tests

```
node tools/tests/test_swarm_workflow.js
python3 -m pytest tools/tests/test_check_citations.py -q
bats tools/tests/test_check_pr.bats      # bats-core; e.g. git clone https://github.com/bats-core/bats-core
```

`test_swarm_workflow.js` imports everything above the `RUNTIME` marker in
`swarm.workflow.js` under plain node, so keep the pure helpers above that
marker and the Workflow-global code below it.
