---
title: "feat: fleet memory program part 1 — deploy gate, harness, Deskflow hotspots, soak"
type: feat
date: 2026-09-16
repo: ~/Desktop/deskflow
---

## feat: fleet memory program — part 1 (deskflow repo) - Extensive

Umbrella plan with full background, hotspot evidence and adversarial-review
rationale: `2026-09-16-feat-fleet-memory-program-plan.md`. This part is
standalone for `/build` and applies only to `~/Desktop/deskflow`.

## Dependencies

- None to start. Part 2 (Mouser) depends on this part's Phases 1–5.
- Phases 6–7 additionally require part 2 Phase 1 (Mouser signing/git-flow) so
  the deploy self-test can sign both apps.

## Overview

Make the fleet deploy provably correct from any seat, build the memory
harness, fix Deskflow's hot loops under the swarm protocol, and run the soak.
Every silent-success path becomes a hard failure; no ad-hoc signing anywhere;
macOS builds route through the GUI session; Windows signing lives in-repo.

## Problem Statement

See umbrella plan §Problem Statement. Deskflow-specific: `deskflow-core` sits
at 699 MB on hackintosh vs ~30 MB elsewhere because hackintosh is the server
and the seat whose peers sleep, so `Coordinator.cpp:263-274,874-893` (two
blocking connects per peer every 3 s on the worker thread, 700 ms each) and
`AutoModeRunner.cpp:125,172-183` (full app rebuild per epoch flip) run
continuously. Deploy can only be driven from hackintosh, passes the keychain
password on the SSH argv, falls back to ad-hoc signing (`CMakeLists.txt:210-213`,
`deploy/mac/deploy.cmake:13-17`), warns instead of failing on verify
(`scripts/fleet-deploy-macos.sh:76-80`, `scripts/install-macos.sh:149`), and
contains no `signtool` call while `deskflow-vhid-bridge.exe` ships unsigned.

## Technical Approach

### Layout

`deskflow/.gitignore:50` ignores `/scripts/*` except `*.sh`, `*.ps1` and
`fleet.env.example`. All new tooling therefore lives in **`tools/`** (new,
tracked): `tools/fleet-doctor`, `tools/fleet-health`, `tools/fleet-soak`,
`tools/fleet-gui-exec.py`, `tools/launchd/*.plist`, `tools/tests/` (pytest
for Python tools, `bats-core` vendored under `tools/tests/bats/` for shell,
Pester for PowerShell). Scenario drivers and swarm artifacts live in
`harness/`. Existing `scripts/*.sh|*.ps1` stay and call into `tools/`.

### Fleet tooling

- `scripts/fleet.env` on every seat (self = `local`); `LOCAL_ID` derived from
  `hostname -s` / `$env:COMPUTERNAME`, never from the env file.
  `DESKFLOW_SIGN_THUMBPRINT` declared in `fleet.env.example`. `env.example` is
  canonical; `.env.example` is deleted.
- Strict signing is a CMake option `FLEET_STRICT_SIGNING` (default OFF; Phase 3's
  first task flips the default to ON after Phase 1 Validation exits 0; Phase 2
  tests configure with `-DFLEET_STRICT_SIGNING=ON` explicitly): `APPLE_CODESIGN_DEV` empty or `-` →
  `message(FATAL_ERROR)` in `CMakeLists.txt`, `deploy/mac/deploy.cmake`,
  `cmake/MacCodesign.cmake`; every `execute_process(codesign …)` checks
  `RESULT_VARIABLE`. Identity comes only from `.env` `DESKFLOW_CODESIGN_ID`;
  `fleet-deploy-macos.sh:55` no longer picks "any Apple Development cert".
- macOS build+sign routes through the GUI session via `tools/fleet-gui-exec.py`
  (lifted from `Mouser/scripts/build_macos_gui_session.py`: console-user check,
  osascript into Terminal, log sentinel, 1800 s timeout). `unlock_keychain`
  and all `FLEET_KEYCHAIN_PASSWORD_*` handling are deleted.
- `scripts/sign-windows.ps1`: `signtool sign /sha1 $env:DESKFLOW_SIGN_THUMBPRINT
  /fd SHA256` over every `.exe/.dll` under the Deskflow install root and the
  Mouser dist; called from `build-windows.ps1 -Install`.
  `install-windows.ps1:329-334` stops importing a self-signed cert into Root.
- `tools/fleet-doctor [--host all] [--check noise]`: exit non-zero on
  unreachable host, dirty tree, branch mismatch across both repos,
  `APPLE_CODESIGN_DEV=-` in any `CMakeCache.txt`, `FLEET_STRICT_SIGNING:BOOL=OFF`
  in any deployed seat's `CMakeCache.txt`, console user != target user,
  Windows interactive session absent/locked, cert missing, Python != the
  seat's pinned version, any `KEYCHAIN_PASSWORD` key in `fleet.env*`; `--check
  noise` (hackintosh) fails if `com.cursor.worker.*` is loaded, vitest workers
  exist, or Spotlight is indexing the app bundle.
- `tools/fleet-health --json [--check sign|no-adhoc|identifiers|tcc|authenticode|session|mesh|all] [--host H|all]`:
  macOS `codesign -dvv --verbose=4` per Mach-O (Authority, TeamIdentifier,
  Identifier allowlist, no `Signature=adhoc`), TCC `csreq -r- -t` decoded as
  `certificate leaf[subject.CN]` never `cdhash`, `deskflow-core
  --check-permissions` (new flag: prints AX + IOHID state, exit 0 only if both
  granted; uses the already-signed binary instead of a new probe),
  `launchctl print gui/$UID` shows the GUI session; Windows
  `Get-AuthenticodeSignature` Valid + fleet thumbprint on every binary, `sc
  query Deskflow` RUNNING, GUI `SessionId != 0`; mesh round-trip all pairs.
- `scripts/fleet-deploy.sh` / `scripts/fleet-deploy.ps1` (new, same CLI):
  `--dry-run --json`, `--self-test [--ref R] [--json]` (rebuild+install on
  every host then `fleet-health --check all`), `--rollback [--host H] [--app X]`
  from `tools/state/last-good.json` (gitignored), per-repo `flock`/`New-Item`
  lock, order clients → server, per-host result table, explicit exit-code
  checks for `ssh`, `&`+`$LASTEXITCODE`, and git under PS 5.1. Weekly
  `tools/launchd/com.fleet.selftest.plist`.

### Human steps (agents verify, never perform)

- Rotate the two login-keychain passwords exposed in `scripts/fleet.env` on
  hackintosh; agents only verify `! grep -rq KEYCHAIN_PASSWORD scripts/fleet.env*`.
- Approve the Terminal Automation prompt on hackintosh the first time
  `tools/fleet-gui-exec.py` runs.
- Quit Cursor on hackintosh (or run `tools/fleet-doctor --host hackintosh
  --check noise --fix`, which `launchctl bootout`s `com.cursor.worker.*`)
  before harness runs.
- Provide `DESKFLOW_CODESIGN_ID` (hash from `security find-identity -v -p
  codesigning`) in `.env` on both Macs; tiny11 user is `alexh`.
- Wait out the 72 h baseline and the 7-day soak (gated inline by `fleet-soak
  report --min-hours 72 --validate`; no separate phase). Run the three
  `# automation: manual` rows (`bt-sleep-wake`, `lock-unlock`, `login-bridge`)
  with `FLEET_OPERATOR=1 harness/run-scenario.sh <row> --seat hackintosh
  --iters 1000` on hackintosh; agents never run them.

### Harness

- `tools/fleet-soak sample --label L --exe P --out F` every 60 s (launchd /
  Task Scheduler): pid, start time, exe path, `phys_footprint` via
  `proc_pid_rusage` RUSAGE_INFO_V4 (ctypes), RSS, compressed, Mach ports,
  threads, fds; Windows PrivateMemorySize64, WorkingSet64, HandleCount,
  GDI/USER. JSONL header records `metric`, `scenario_source: observed`,
  sampler git SHA; scenario derived from app logs; heartbeat per sample.
- `tools/fleet-soak report --seat S --proc P --in F --window H [--accelerated]
  [--scenario X] --slope-max MB/h --cap MB --ports-slope-max n/h --max-step
  pct --max-restarts 0 --min-hours 72 --min-completeness 0.95 --validate`:
  Theil–Sen slope, first 2 h excluded; exit 0 pass, 1 fail, 2 invalid.
- `harness/scenarios/<row>.sh|.ps1` (≥12 rows: mouse-absent-reconnect,
  bt-sleep-wake, window-toggle, add-profile, scroll-im-denied,
  scroll-im-granted, screen-switch, clipboard-10mb, epoch-flip,
  peer-unreachable, lock-unlock, login-bridge), `harness/run-scenario.sh
  <row> --seat S --iters N [--smoke]`, `harness/behavior-bench.sh`,
  `harness/check-citations.py`, `harness/registry.json`.
  `harness/behavior-bench.sh --proc P --seat S [--baseline]`: deskflow-core =
  200 synthetic CGEvent mouse moves injected on `--seat`, timestamped at the
  client's CGEvent tap; mouser = 200 synthetic scroll ticks, tick→injected
  event. 5 runs × 200, first run dropped. Writes/compares
  `harness/baselines/behavior-<seat>.json` with schema `{proc, seat, runs,
  p50_ms, p95_ms, delivered, sent}`. Pass = p50 and p95 within ±10 % of
  baseline AND `delivered == sent`. `harness/check-pr.sh <pr-number>`: exits
  non-zero unless the PR body contains `run-id:` and `deltaMB:` and
  `check-citations.py` passes.

### Swarm workflow contract (`harness/swarm.workflow.js`)

- `args`: `{repoPath, ghRepo, registry, baseBranch: "fleet/memory-program",
  dryRun, hackintoshSsh: "hackintosh"}`; `repoPath` absolute
  (`/Users/alexhughes/Desktop/deskflow` or `/Users/alexhughes/Desktop/Mouser`);
  `ghRepo` `hughesyadaddy/deskflow` or `hughesyadaddy/Mouser`; `registry`
  absolute path.
- Registry entry: `{id, repo, title, files[], scenario, proc, seat:
  "hackintosh", budgetMB, evidence: {file, line, token}, fixClass}`.
- Prompts read from `harness/prompts/auditor.md` and `harness/prompts/adversary.md`.
- Auditor: branch `hotspot/<id>` from `baseBranch`, implements + tests,
  pushes; runs the scenario on hackintosh via `ssh hackintosh 'git -C <repo>
  fetch && git checkout hotspot/<id> && harness/run-scenario.sh <scenario>
  --iters 1000'` before and after. hackintosh scenario runs are serialized by
  `flock harness/.hackintosh.lock` inside `run-scenario.sh`. `run-scenario.sh`
  exits 3 on hackintosh unless `tools/fleet-soak report --in
  harness/baselines/<latest>-hackintosh-<proc>.json --min-hours 72 --validate`
  exits 0; rows with `# automation: manual` exit 4 unless `FLEET_OPERATOR=1`.
- Adversary: independent rerun, compile matrix (`cmake -S . -B build` on
  both Macs; `ssh tiny11 … build-windows.ps1` for Deskflow; each seat's
  `.venv` pytest for Mouser), behavior bench; returns `{verdict, deltaMB}`.
- Merge step: `gh pr create --draft --base fleet/memory-program --head
  hotspot/<id> --label needs-harness` on `approve`; the adversary runs
  `harness/check-pr.sh` and marks the PR ready only when it exits 0. The
  script does **not** merge — Alex merges ranked by `deltaMB`, then runs
  `swarm.workflow.js --post-merge`, which rebases every open `hotspot/*` onto
  `fleet/memory-program` and reruns the compile matrix. Returns
  `[{id, prUrl, deltaMB, verdict}]`.
- Log paths: scenario runs write `harness/runs/<row>.jsonl` and
  `harness/runs/logs/<proc>/`; soak writes `harness/soak/<date>/` with
  `harness/soak/latest` symlink created by `fleet-soak sample --start-soak`.

### Swarm protocol and execution (Phase 7 here; part 2 Phase 2)

Execution mechanism: the hotspot phase is run by a **Workflow-tool script**
(`harness/swarm.workflow.js`, checked in) that `/build` invokes as the phase's
single task. Input is `harness/registry.json`. For each registered hotspot the
script runs, in a `pipeline`: an *auditor* agent (reproduce on hackintosh via
`harness/run-scenario.sh`, produce a finding with a run id, implement the fix
on branch `hotspot/<id>` in the owning repo, add tests) → an *adversary* agent
(independently rerun the scenario before/after on hackintosh, run the compile
matrix and behavior bench, return `veto|approve` with the Δ) → a *merge* step
that opens a PR against `fleet/memory-program` only on `approve`. PRs merge
serially, ranked by measured MB recovered; the matrix reruns after each merge.
The phase's first task creates `fleet/memory-program` from `main` in the repo.
Concurrency is capped at the number of hotspots (≈ 8 Deskflow, ≈ 7 Mouser), so
≈ 30 agents run for the two swarms plus the merge steps.

Rules: one auditor+adversary pair and one PR per hotspot; no PR touches two
hotspots' files (`tools/tests/test_swarm_workflow.js` asserts registry
`files[]` are pairwise disjoint; hotspot branches may not be created until it
passes); finding schema `id, hotspot, seat, scenario-row,
file@commit:line, evidence(run id + Δ), confidence,
fix-class{root-cause,mitigation}`; no run id → hypothesis, not a finding;
citations verified by `check-citations.py`; adversary veto ("reproduced
before, absent after, on hackintosh"); mitigation-class cannot close a hotspot
and must log `mitigation:`; compile matrix macOS arm64 / macOS x86_64 /
Windows MSVC; behavior bench within ±10 % of the Phase 5 baseline.

Every scenario file declares `# proc: mouser|deskflow-core|deskflow` in its
header; `run-scenario.sh` and the accelerated gate read it, so no `--proc
auto` exists.

## Implementation Phases

### Phase 1: Provision seats and rotate secrets

- **Status:** Not started
- **Scope:** Make strict signing possible before enforcing it. Both Macs get
  `deskflow/.env` with `DESKFLOW_CODESIGN_ID`; hackintosh's leaked keychain
  passwords are rotated, `scripts/fleet.env.bak-20260814` deleted, and
  `FLEET_KEYCHAIN_PASSWORD_*` removed from `scripts/fleet.env`; `fleet.env`
  created on macbookpro and tiny11; `env.example` becomes canonical.
- **Files touched:** `env.example`, `.env.example` (delete), `scripts/fleet.env.example`,
  `.gitignore` (verify `!/scripts/fleet.env.example`; add `tools/state/`,
  `harness/runs/`, `harness/soak/`), `docs/building-signed.md`. Per-seat untracked: `.env`,
  `scripts/fleet.env`.
- **Acceptance criteria:** `test -f .env` and `grep -q DESKFLOW_CODESIGN_ID
  .env` on both Macs; `test -f scripts/fleet.env` on all three seats;
  `! grep -rq KEYCHAIN_PASSWORD scripts/fleet.env*` on all seats;
  `! test -e scripts/fleet.env.bak-20260814` on hackintosh; `.env.example`
  absent; `DESKFLOW_SIGN_THUMBPRINT` present in `fleet.env.example`.
- **Validation:** `test -f .env && grep -q DESKFLOW_CODESIGN_ID .env && ! grep -rq
  KEYCHAIN_PASSWORD scripts/fleet.env* && ! test -e .env.example && grep -q
  DESKFLOW_SIGN_THUMBPRINT scripts/fleet.env.example && ssh hackintosh 'cd
  ~/Desktop/deskflow && test -f .env && ! grep -rq KEYCHAIN_PASSWORD
  scripts/fleet.env* && ! test -e scripts/fleet.env.bak-20260814' && ssh
  tiny11 'powershell -NoProfile -Command "Test-Path
  C:/Users/alexh/Desktop/deskflow/scripts/fleet.env"'`

### Phase 2: Kill silent success (deskflow)

- **Status:** Not started
- **Scope:** `FLEET_STRICT_SIGNING` CMake option (default ON) makes empty/ad-hoc
  identity fatal and checks every codesign result; `fleet-deploy-macos.sh`
  loses `unlock_keychain`, the "any cert" lookup, `|| true`, and warns-only
  verify; `install-macos.sh` verify becomes fatal; `sign-windows.ps1` added and
  wired; `install-windows.ps1` stops importing a Root cert; `fleet-deploy-windows.ps1`
  checks git and `$LASTEXITCODE`; `fleet-deploy.sh` derives `LOCAL_ID` from
  hostname and drops keychain-password plumbing; the Mouser pull in
  `fleet-deploy-macos.sh:85-91` and `fleet-deploy-windows.ps1:37-58` becomes
  `git fetch fork && git checkout "$FLEET_BRANCH" && git pull --ff-only fork
  "$FLEET_BRANCH"` with no `|| true`.
- **Files touched:** `CMakeLists.txt`, `deploy/mac/deploy.cmake`,
  `cmake/MacCodesign.cmake`, `scripts/fleet-deploy.sh`, `scripts/fleet-deploy-macos.sh`,
  `scripts/fleet-deploy-windows.ps1`, `scripts/build-windows.ps1`,
  `scripts/install-windows.ps1`, `scripts/install-macos.sh`,
  `scripts/sign-windows.ps1` (new), `tools/tests/bats/` (bats-core v1.11.0 as a
  git subtree; `tools/tests/run.sh` skips `*.Tests.ps1` when `pwsh` is absent),
  `tools/tests/test_fleet_deploy_macos.bats` (new), `tools/tests/test_install_macos.bats`
  (new), `tools/tests/SignWindows.Tests.ps1` (new, Pester), `tools/tests/run.sh` (new).
- **Acceptance criteria:** `cmake -S . -B /tmp/b -DFLEET_STRICT_SIGNING=ON
  -DAPPLE_CODESIGN_DEV=-` fails
  and with the identity empty fails; `grep -rn '|| true' scripts/*.sh` returns
  only lines tagged `# fleet:allow`; `FLEET_DEPLOY_MOUSER`/`FLEET_DEPLOY_DESKFLOW`
  are declared in `fleet.env.example`; the Mouser remote `fork` is added if
  missing;
  `sign-windows.ps1` signs every `.exe/.dll` under both roots (Pester test on a
  fixture tree with a test cert); Mouser pull uses `fork` + `$FLEET_BRANCH`.
- **Validation:** `tools/tests/run.sh && ! cmake -S . -B /tmp/fleet-strict
  -DFLEET_STRICT_SIGNING=ON -DAPPLE_CODESIGN_DEV=- >/dev/null 2>&1 && ! grep -rn '|| true' scripts/*.sh
  | grep -v 'fleet:allow' && grep -q 'git pull --ff-only fork'
  scripts/fleet-deploy-macos.sh`

### Phase 3: Doctor, health, GUI-session exec, permission check

- **Status:** Not started
- **Scope:** First task: flip `FLEET_STRICT_SIGNING` default to ON (gated on
  Phase 1 Validation exit 0). Then `tools/fleet-doctor`, `tools/fleet-health`
  (bash + ps1 backends),
  `tools/fleet-gui-exec.py` lifted from Mouser's GUI-session helper and used
  by `fleet-deploy-macos.sh` for the Deskflow build, and a `deskflow-core
  --check-permissions` flag that prints AX + IOHID state and exits 0 only when
  both are granted.
- **Files touched:** `tools/fleet-doctor` (new), `tools/fleet-health` (new),
  `tools/fleet-health.ps1` (new), `tools/fleet-gui-exec.py` (new),
  `scripts/fleet-deploy-macos.sh`, `src/apps/deskflow-core/deskflow-core.cpp`,
  `src/lib/platform/OSXScreen.mm`, `tools/tests/test_fleet_doctor.py`,
  `tools/tests/test_fleet_health.py`, `tools/tests/test_fleet_gui_exec.py`
  (new), `tools/tests/FleetHealth.Tests.ps1` (new), `docs/building-signed.md`.
- **Acceptance criteria:** `fleet-doctor --host all` exits 0 on this seat;
  `fleet-health --check all --host all` exits 0 against the currently
  installed apps *after* a Phase-2-hardened deploy; `deskflow-core
  --check-permissions` exits 0 on both Macs; a Deskflow build launched over
  SSH to hackintosh signs with the configured identity (routed through the GUI
  session). Windows session probe is `quser` over SSH with state `Active`.
- **Validation:** `python3 -m pytest tools/tests/test_fleet_doctor.py
  tools/tests/test_fleet_health.py tools/tests/test_fleet_gui_exec.py -q &&
  tools/fleet-doctor --host all && ssh hackintosh 'cd ~/Desktop/deskflow &&
  FLEET_DEPLOY_MOUSER=0 bash scripts/fleet-deploy-macos.sh' && bash
  scripts/fleet-deploy-macos.sh && tools/fleet-health --check all --host all`

### Phase 4: Symmetric controller, self-test, rollback

- **Status:** Not started
- **Scope:** `scripts/fleet-deploy.sh` and new `scripts/fleet-deploy.ps1` with
  the same CLI: `--dry-run --json`, `--self-test [--ref R] [--json]`,
  `--rollback [--host H] [--app X]`, per-repo lock, `tools/state/last-good.json`
  (written by every successful `--self-test` and deploy),
  clients-before-server order, per-host result table, explicit exit-code
  checks; weekly `com.fleet.selftest` LaunchAgent that also runs
  `harness/canary.sh` once it exists.
- **Files touched:** `scripts/fleet-deploy.sh`, `scripts/fleet-deploy.ps1` (new),
  `scripts/fleet-deploy-windows.ps1`, `tools/launchd/com.fleet.selftest.plist`
  (new), `tools/state/` (gitignored), `tools/tests/test_fleet_deploy.bats`
  (new), `tools/tests/FleetDeploy.Tests.ps1` (new), `docs/building-signed.md`.
- **Acceptance criteria:** from each seat `fleet-deploy --dry-run --json` lists
  exactly three distinct targets; `fleet-deploy --self-test --json` exits 0 on
  all three seats; `--rollback --host macbookpro --app deskflow` restores
  last-good and health stays green; `launchctl print
  gui/$(id -u)/com.fleet.selftest` exists.
- **Validation:** `tools/tests/run.sh &&
  scripts/fleet-deploy.sh --dry-run --json - | jq -e '.hosts|length==3 and
  (map(.target)|unique|length==3)' && tools/fleet-doctor --host all &&
  scripts/fleet-deploy.sh --self-test --json /tmp/st-mbp.json && jq -e
  '.ok==true' /tmp/st-mbp.json && ssh hackintosh 'cd ~/Desktop/deskflow &&
  scripts/fleet-deploy.sh --self-test --json /tmp/st.json && jq -e .ok
  /tmp/st.json' && ssh tiny11 'powershell -NoProfile -Command "cd
  C:/Users/alexh/Desktop/deskflow; ./scripts/fleet-deploy.ps1 --self-test
  --json $env:TEMP/st.json; exit $LASTEXITCODE"' && scripts/fleet-deploy.sh
  --rollback --host macbookpro --app
  deskflow && tools/fleet-health --host macbookpro && launchctl print
  gui/$(id -u)/com.fleet.selftest >/dev/null`

### Phase 5: Harness and hackintosh baseline

- **Status:** Not started
- **Scope:** `fleet-soak` sampler/report, `fleet-doctor --check noise`,
  scenario drivers (each with a `# proc:` header), behavior bench, citation
  checker, registry; samplers installed on all seats; 72 h footprint baseline
  on hackintosh and a behavior-bench baseline on all three seats, both
  captured before any fix lands.
- **Files touched:** `tools/fleet-soak` (new), `tools/fleet-doctor` (`--check
  noise`), `harness/scenarios/*.sh|*.ps1` (new, ≥12), `harness/run-scenario.sh`
  (new), `harness/behavior-bench.sh` (new), `harness/check-citations.py` (new),
  `harness/registry.json` (new), `tools/launchd/com.fleet.soak.plist` (new),
  `tools/fleet-soak-task.ps1` (new), `harness/README.md`,
  `tools/tests/test_fleet_soak.py`, `tools/tests/test_check_citations.py`,
  `tools/tests/test_run_scenario.bats` (new), `harness/baselines/` (new),
  `harness/swarm.workflow.js` (new), `harness/prompts/auditor.md` (new),
  `harness/prompts/adversary.md` (new), `tools/tests/test_swarm_workflow.js` (new).
  Driver automation: `bt-sleep-wake` (`blueutil`), `lock-unlock` (`CGSession
  -suspend` + `caffeinate -u`) and `login-bridge` are `# automation: manual`:
  the commands live in the driver but `run-scenario.sh` refuses them unless
  `FLEET_OPERATOR=1`; `--smoke` on a manual row writes `smoke: skipped-manual`
  and exits 0; their JSONL header carries `scenario_source: manual`.
  `mouse-absent-reconnect` switches the Deskflow server to another screen over
  the mesh; `screen-switch` uses `deskflow-core` mesh commands. Every other
  driver is `# automation: full`. Also new: `harness/check-pr.sh`,
  `.github/workflows/pr-harness-check.yml`.
- **Acceptance criteria:** JSONL header `metric` is `phys_footprint` on macOS
  and `private_bytes` on Windows and `scenario_source` is `observed`
  (asserted by `test_fleet_soak.py`); `--validate` fails on a synthetic gap;
  every driver completes with `--smoke` on hackintosh; `fleet-doctor --host
  hackintosh --check noise` exits 0; `harness/baselines/2026-09-*-hackintosh-{mouser,deskflow-core}.json`
  exist with the pre-fix slope; `harness/baselines/behavior-{macbookpro,hackintosh,tiny11}.json`
  exist; `harness/swarm.workflow.js` dry-runs against a two-entry fixture
  registry without spawning agents; `harness/baselines/behavior-*.json` match
  the schema `{proc, seat, runs, p50_ms, p95_ms, delivered, sent}` with
  `delivered == sent`; `test_swarm_workflow.js` asserts registry `files[]`
  pairwise disjoint.
- **Validation:** `python3 -m pytest tools/tests/test_fleet_soak.py
  tools/tests/test_check_citations.py -q && tools/fleet-doctor --host
  hackintosh --check noise && for r in harness/scenarios/*.sh; do
  harness/run-scenario.sh "$(basename "$r" .sh)" --seat hackintosh --iters 10
  --smoke; done && ls harness/baselines/*-hackintosh-mouser.json
  harness/baselines/*-hackintosh-deskflow-core.json
  harness/baselines/behavior-macbookpro.json harness/baselines/behavior-hackintosh.json
  harness/baselines/behavior-tiny11.json && node tools/tests/test_swarm_workflow.js`

### Phase 6: Deskflow hotspots (one PR per hotspot)

- **Status:** Not started
- **Scope:** First task: `git checkout -b fleet/memory-program main` (hotspot
  code drafted earlier on branches from `main` is rebased by the workflow).
  Then `/build` runs `harness/swarm.workflow.js` via the Workflow tool with
  `args: {repo: "deskflow", registry: "harness/registry.json"}`; the workflow
  produces one `hotspot/<id>` branch and PR per entry onto
  `fleet/memory-program` (see §Swarm protocol). Registry order unless measured
  MB says otherwise: (1) Coordinator peer connects off the worker
  thread, never inside an input callback, non-blocking with per-peer backoff;
  (2) AutoModeRunner reuses screen/tap across epoch flips or bounds flip rate;
  (3) `TCPSocket`/`SecureSocket` output-buffer cap + `StreamBuffer::peek`
  without full consolidation; (4) single clipboard copy on the server, size
  check without `marshall()`; (5) `CFRetain` the run loops in the two
  monitors; (6) replace 1 s pasteboard/AX polling with change-count checks at
  lower rate.
- **Files touched:** `src/lib/coordination/Coordinator.cpp`,
  `src/lib/coordination/CoordinationMesh.cpp`, `src/apps/deskflow-core/AutoModeRunner.cpp`,
  `src/lib/net/TCPSocket.cpp`, `src/lib/net/SecureSocket.cpp`, `src/lib/io/StreamBuffer.cpp`,
  `src/lib/server/Server.cpp`, `src/lib/server/Server.h`, `src/lib/server/ClientProxy1_0.cpp`,
  `src/lib/platform/OSXScreen.mm`, `src/lib/platform/OSXKeyboardRelayMonitor.mm`,
  `src/lib/platform/OSXLocalInputMonitor.mm`,
  `src/unittests/lib/net/TCPSocketTests.cpp` (new), `src/unittests/lib/io/StreamBufferTests.cpp`
  (new or extended), `src/unittests/lib/server/ServerClipboardTests.cpp` (new),
  `src/unittests/lib/coordination/CoordinatorTests.cpp` (new or extended),
  `harness/registry.json`.
- **Acceptance criteria:** each PR carries a finding with a harness run id and
  adversary sign-off on hackintosh; compiles on macOS arm64, macOS x86_64,
  Windows MSVC; `ctest` green; behavior bench within ±10 %; accelerated gate
  passes for epoch-flip, peer-unreachable, screen-switch, clipboard-10mb,
  lock-unlock, login-bridge (the last two operator-run); deskflow-core
  restarts = 0 during the runs; every PR was opened `--draft --label
  needs-harness` and `harness/check-pr.sh` exited 0 before ready.
- **Validation:** `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake
  --build build --target unittests && ctest --test-dir build/src/unittests
  --output-on-failure && python3 harness/check-citations.py
  harness/registry.json && for r in epoch-flip peer-unreachable screen-switch
  clipboard-10mb lock-unlock login-bridge; do harness/run-scenario.sh $r --seat
  hackintosh --iters 1000 && tools/fleet-soak report --accelerated --proc
  deskflow-core --in harness/runs/$r.jsonl --window 2 --slope-max 5
  --ports-slope-max 10 --max-restarts 0; done && harness/behavior-bench.sh
  --proc deskflow-core`

### Phase 7: Fleet soak, canary, sign-off

- **Status:** Not started
- **Scope:** Deploy merged `fleet/memory-program` (both repos) via the
  self-tested deploy, run the 7-day soak on all seats, add `harness/canary.sh`
  (reruns the scenario matrix on hackintosh against the Phase 5 baselines and
  exits non-zero on regression) to the weekly `com.fleet.selftest` agent, and
  record the sign-off table.
- **Files touched:** `harness/soak/<date>/`, `harness/canary.sh` (new),
  `tools/launchd/com.fleet.selftest.plist`, `docs/building-signed.md`,
  `docs/FORK_ROADMAP.md`, `harness/README.md`.
- **Acceptance criteria:** the Success Criteria block is green on every seat;
  sign-off table in `docs/FORK_ROADMAP.md` has one row per seat × {server,
  client} × {mouse present, absent}; `harness/canary.sh` exits 0 on the
  merged branch and the weekly agent invokes it.
- **Validation:** `for s in macbookpro hackintosh tiny11; do for p in mouser
  deskflow-core deskflow; do tools/fleet-soak report --seat $s --proc $p --in
  harness/soak/latest/$s-$p.jsonl --window 168 --min-hours 72
  --min-completeness 0.95 --slope-max 0.1 --ports-slope-max 1 --max-step 5
  --max-restarts 0 --validate; done; done && harness/canary.sh --seat
  hackintosh && grep -q canary.sh tools/launchd/com.fleet.selftest.plist`

## Success Criteria

```success-criteria
GOAL: Deskflow and Mouser run on all three seats with a flat physical-memory footprint under every scenario in the matrix, deployed by a self-tested fleet deploy that any seat can drive.

SUCCESS CRITERIA:
- Any seat plans exactly three distinct targets | verify: scripts/fleet-deploy.sh --dry-run --json - | jq -e '.hosts|length==3 and (map(.target)|unique|length==3)'
- Doctor preconditions hold on all hosts | verify: tools/fleet-doctor --host all
- No keychain secrets in fleet config | verify: ! grep -rq KEYCHAIN_PASSWORD scripts/fleet.env*
- No ad-hoc identity anywhere | verify: ! grep -q 'APPLE_CODESIGN_DEV=-' build/CMakeCache.txt && tools/fleet-health --check no-adhoc --host all
- Exact identity on every Mach-O of both apps on both Macs | verify: tools/fleet-health --check sign --host all
- Stable code identifiers | verify: tools/fleet-health --check identifiers --host all
- TCC csreq cert-based and AX + IOHID granted, surviving a rebuild | verify: scripts/fleet-deploy.sh --self-test --ref HEAD && tools/fleet-health --check tcc --host all
- Windows: every .exe/.dll Valid with the fleet thumbprint | verify: tools/fleet-health --check authenticode --host tiny11
- GUI in console session; Deskflow service RUNNING | verify: tools/fleet-health --check session --host all
- Mesh round-trip between all pairs | verify: tools/fleet-health --check mesh
- Rollback restores last-good and stays healthy | verify: scripts/fleet-deploy.sh --rollback --host macbookpro --app deskflow && tools/fleet-health --host macbookpro
- Weekly self-test installed | verify: launchctl print gui/$(id -u)/com.fleet.selftest
- hackintosh noise removed before any run | verify: tools/fleet-doctor --host hackintosh --check noise
- Accelerated gate per scenario row on hackintosh | verify: for r in $(ls harness/scenarios | sed 's/\..*//' | sort -u); do tools/fleet-soak report --accelerated --proc "$(harness/run-scenario.sh $r --print-proc)" --in harness/runs/$r.jsonl --window 2 --slope-max 5 --ports-slope-max 10 --max-restarts 0; done
- Canary passes on the merged branch and is scheduled weekly | verify: harness/canary.sh --seat hackintosh && grep -q canary.sh tools/launchd/com.fleet.selftest.plist
- 7-day soak slope ≤ 0.1 MB/h, ports ≤ 1/h, max daily step ≤ 5 %, restarts = 0, per seat per process | verify: for s in macbookpro hackintosh tiny11; do for p in mouser deskflow-core deskflow; do tools/fleet-soak report --seat $s --proc $p --in harness/soak/latest/$s-$p.jsonl --window 168 --min-hours 72 --min-completeness 0.95 --slope-max 0.1 --ports-slope-max 1 --max-step 5 --max-restarts 0 --validate; done; done && tools/fleet-soak report --seat tiny11 --proc deskflow-daemon --in harness/soak/latest/tiny11-deskflow-daemon.jsonl --window 168 --min-hours 72 --min-completeness 0.95 --slope-max 0.1 --max-step 5 --max-restarts 0 --validate
- Idle caps: Mouser ≤ 150 MB hidden, deskflow-core ≤ 60 MB, deskflow GUI ≤ 150 MB, deskflow-daemon ≤ 40 MB | verify: tools/fleet-soak report --proc mouser --cap 150 --in harness/soak/latest/hackintosh-mouser.jsonl && tools/fleet-soak report --proc deskflow-core --cap 60 --in harness/soak/latest/hackintosh-deskflow-core.jsonl && tools/fleet-soak report --proc deskflow --cap 150 --in harness/soak/latest/hackintosh-deskflow.jsonl && tools/fleet-soak report --proc deskflow-daemon --cap 40 --in harness/soak/latest/tiny11-deskflow-daemon.jsonl
- Mitigation counters are zero over the soak | verify: ! grep -rq 'mitigation:' harness/soak/latest/logs/
- Behavior within ±10 % of baseline | verify: harness/behavior-bench.sh --all
- Every registry finding cites a real line at the pinned commit | verify: python3 harness/check-citations.py harness/registry.json
- Every hotspot PR carries run id and delta | verify: for p in $(gh pr list --base fleet/memory-program --json number -q '.[].number'); do harness/check-pr.sh $p; done
- Deskflow unit tests and tool tests pass | verify: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target unittests && ctest --test-dir build/src/unittests --output-on-failure && python3 -m pytest tools/tests -q && tools/tests/run.sh

NON-GOALS:
- Rewriting Mouser natively; notarization; push-to-deploy; Qt trimming; `CMAKE_INTERPROCEDURAL_OPTIMIZATION`; fixing vitest fan-out or the Bloc language server (harness preconditions only).

VERIFICATION COMMAND: tools/fleet-doctor --host all && ! grep -rq KEYCHAIN_PASSWORD scripts/fleet.env* && tools/fleet-health --check all --host all && tools/fleet-doctor --host hackintosh --check noise && python3 harness/check-citations.py harness/registry.json && cmake --build build --target unittests && ctest --test-dir build/src/unittests --output-on-failure && python3 -m pytest tools/tests -q && tools/tests/run.sh && for s in macbookpro hackintosh tiny11; do for p in mouser deskflow-core deskflow; do tools/fleet-soak report --seat $s --proc $p --in harness/soak/latest/$s-$p.jsonl --window 168 --min-hours 72 --min-completeness 0.95 --slope-max 0.1 --ports-slope-max 1 --max-step 5 --max-restarts 0 --validate; done; done && harness/behavior-bench.sh --all
```

## Risk Analysis & Mitigation

See umbrella plan §Risk Analysis. Part-1-specific: strict signing must not
land before Phase 1 provisions `.env` on both Macs (ordering enforced by
phases); GUI-session routing requires Terminal Automation permission and a
logged-in console user (doctor precondition, hard fail).

## References

Umbrella plan §References & Research. Key deskflow lines:
`scripts/fleet-deploy.sh:82-118,139-153,170`, `scripts/fleet-deploy-macos.sh:24-32,47-59,61,76-80,89-91`,
`scripts/fleet-deploy-windows.ps1:24-26,46,57,64-67`, `scripts/install-windows.ps1:329-334,434`,
`scripts/install-macos.sh:147-152`, `CMakeLists.txt:164,210-218`, `deploy/mac/deploy.cmake:13-27`,
`cmake/MacCodesign.cmake:40-67`, `.gitignore:50`, `src/unittests/CMakeLists.txt:36-41,91`,
`src/lib/coordination/Coordinator.cpp:37-38,263-294,832-957`, `src/apps/deskflow-core/AutoModeRunner.cpp:119-183`,
`src/lib/net/TCPSocket.cpp:23,157,316`, `src/lib/io/StreamBuffer.cpp:16-116`,
`src/lib/server/Server.cpp:764,1641-1647,1980-1990`, `src/lib/server/Server.h:429,485`,
`src/lib/platform/OSXScreen.mm:680-683,714-731,836,1416-1475,1541`,
`src/lib/platform/OSXKeyboardRelayMonitor.mm:288`, `src/lib/platform/OSXLocalInputMonitor.mm:91`.
