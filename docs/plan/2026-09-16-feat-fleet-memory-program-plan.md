---
title: "feat: fleet memory program — flat-footprint Mouser + Deskflow, any-seat deploy"
type: feat
date: 2026-09-16
---

## feat: fleet memory program — Extensive

> **Note:** This plan was split into parts. See the -part-N files in this
> directory: part-1 (deskflow repo: provisioning, signing gates, controller,
> harness, Deskflow hotspots, soak) and part-2 (Mouser repo: signing/git-flow,
> Mouser hotspots). The parts are canonical for `/build`; after review they
> supersede this file's phases and commands (tools live in `tools/` not
> `scripts/`, tests run via `ctest --test-dir build/src/unittests`,
> `fleet-soak preflight` became `fleet-doctor --check noise`, the `--print-*`
> flags became JSONL header fields, and `fleet-tcc-probe` became
> `deskflow-core --check-permissions`).

## Overview

Make Mouser and the Deskflow fork run for a week on every seat with a flat
physical-memory footprint, and make the fleet deploy provably correct from any
seat. Work is gated: nothing from the memory workstreams ships until the deploy
self-test is green on all three hosts, and no memory fix closes a hotspot
without before/after harness numbers on the seat where the problem was
observed (hackintosh).

Two repos are touched: `~/Desktop/deskflow` (fleet tooling, Deskflow fixes)
and `~/Desktop/Mouser` (Mouser fixes). Each phase names its repo.

Source brainstorm: `docs/brainstorm/2026-09-16-fleet-memory-program-brainstorm-doc.md`.
Prior audit whose ORDER OF WORK this plan absorbs:
`docs/brainstorm/2026-08-31-fleet-hardening-audit-brainstorm-doc.md`.

## Problem Statement

On 2026-09-15 the hackintosh (72 GB) hit ~0 free RAM. Mouser 3.6.0 had reached
5.3 GB footprint (851 MB RSS + 4.4 GB compressed) after 9 days; deskflow-core
sat at 699 MB. On the other two seats the same builds sit at 30–110 MB. The
hackintosh is the seat where the physical mouse is normally *absent* and where
Deskflow normally runs as *server*, so both apps' hottest loops run there
continuously:

- Mouser `core/hid_gesture.py:3266` reconnect loop at 4 Hz forever when the
  receiver is present but the mouse is on another host — a full
  IOHIDManager create/open/copy/close per pass.
- Mouser `core/macos_iokit_scroll.py:167-171` (confirmed bug): `stop()` runs
  before `self._manager` is assigned on `start()` failure, leaking an
  IOHIDManager per scroll tick whenever Input Monitoring is denied — which is
  exactly the state every ad-hoc-signed deploy leaves the app in.
- Mouser `main_qml.py:769-892`: `NSStatusItem` + PyObjC target +
  `QSvgRenderer` rebuilt on every window show/hide, twice.
- Deskflow `src/lib/coordination/Coordinator.cpp:263-274,874-893`: two blocking
  connects per peer every 3 s on the worker thread (700 ms each when a peer
  sleeps); `:927-957` blocking query to every peer every 15 s.
- Deskflow `src/apps/deskflow-core/AutoModeRunner.cpp:125,172-183`: full
  ServerApp/ClientApp + screen + event-tap rebuild on every epoch flip.
- Deskflow `src/lib/net/TCPSocket.cpp:157` unbounded output buffer;
  `Server.h:429,485` clipboard held twice on the server and once per client
  proxy; `Server.cpp:764` marshals every clipboard on every screen switch;
  `OSXScreen.mm:680,836` 1 s pasteboard + AX polling; unretained CFRunLoops in
  `OSXKeyboardRelayMonitor.mm:288` and `OSXLocalInputMonitor.mm:91`.

The deploy path cannot be trusted to ship fixes: it runs only from the
hackintosh (`scripts/fleet.env` exists nowhere else), passes the login-keychain
password on the SSH command line (`scripts/fleet-deploy.sh:114`) even though
unlock-over-SSH does not work on macOS, silently falls back to ad-hoc signing
(`CMakeLists.txt:210-213`, `deploy/mac/deploy.cmake:13-17`,
`Mouser/build_macos_app.sh:201-203`), treats verify failures as warnings
(`fleet-deploy-macos.sh:76-80`, `install-macos.sh:149`), pulls Mouser with
`git pull || true` and no branch (`fleet-deploy-macos.sh:89`), and has no
`signtool` call anywhere — the Windows binaries that *are* signed were signed
out-of-band, and `Mouser.exe` + `deskflow-vhid-bridge.exe` ship unsigned.
Ad-hoc signatures reset macOS TCC grants on every deploy, which both breaks the
apps and hides the scroll-tick leak path behind a permission denial.

## Proposed Solution

1. **Deploy gate** (Phases 1–2): make every silent-success path a hard failure,
   route macOS signing through the GUI session on both apps, bring Windows
   signing in-repo, and turn the fleet scripts into a symmetric controller with
   `fleet-doctor`, `fleet-health`, `--self-test`, `--dry-run --json`, locking,
   last-good + `--rollback`, clients-before-server ordering, and a weekly
   self-test.
2. **Harness** (Phase 3): `fleet-soak` sampler/report judged on
   `phys_footprint` (macOS) / Private Bytes (Windows) slope, a scenario matrix
   with driver scripts, a signed `fleet-tcc-probe`, a behavior bench, and a
   hackintosh baseline run.
3. **Mouser fixes** (Phase 4) and **Deskflow fixes** (Phase 5): executed by
   auditor/adversary agent pairs under the swarm protocol below, one PR per
   hotspot, merged in order of measured MB recovered.
4. **Soak + canary + sign-off** (Phase 6).

## Technical Approach

### Architecture

**Fleet tooling (deskflow repo, `scripts/`)**

- `fleet.env` present on all three seats (self = `local`). `fleet-deploy.sh`
  derives `LOCAL_ID` from hostname, never from the env file, so running from
  macbookpro never deploys macbookpro twice and hackintosh never. New
  `fleet-deploy.ps1` controller for tiny11 with identical CLI.
- `fleet-doctor [--host all]`: exit non-zero on unreachable host, dirty tree,
  branch mismatch across both repos, `APPLE_CODESIGN_DEV=-` in any
  `CMakeCache.txt`, console user != target user, Windows interactive session
  absent/locked, cert missing, Python outside the pinned set, any
  `KEYCHAIN_PASSWORD` key in `fleet.env*`.
- macOS build+sign for **both** apps routes through the GUI session (extend
  `Mouser/scripts/build_macos_gui_session.py` into a shared
  `scripts/fleet-gui-exec.py`; osascript-into-Terminal with log sentinel).
  Preconditions checked by doctor; no ad-hoc fallback anywhere: `-` is
  rejected by CMake (`CMakeLists.txt`, `deploy/mac/deploy.cmake`,
  `cmake/MacCodesign.cmake` gain `message(FATAL_ERROR)`), by
  `build_macos_app.sh`, and by `build_and_install.py`. Identity comes from
  `deskflow/.env` `DESKFLOW_CODESIGN_ID` and `Mouser/.env.local`
  `MOUSER_SIGN_IDENTITY` — never "first Apple Development cert found".
- Windows: `scripts/sign-windows.ps1` runs `signtool sign /sha1
  $DESKFLOW_SIGN_THUMBPRINT /fd SHA256` over every `.exe/.dll` in both install
  roots (Deskflow incl. vhid bridge and Qt/OpenSSL DLLs; Mouser dist), called
  from `build-windows.ps1 -Install` and from the Mouser Windows install path.
  `install-windows.ps1:329-334` stops adding a self-signed cert to Root.
- `fleet-health --json [--check sign|no-adhoc|identifiers|tcc|authenticode|session|mesh|all]`:
  macOS `codesign -dvv --verbose=4` per Mach-O (Authority + TeamIdentifier +
  Identifier allowlist, no `Signature=adhoc`), TCC `csreq` decoded as
  `certificate leaf[subject.CN]` (never `cdhash`), `fleet-tcc-probe` (signed
  with the deskflow identity) returns AX + IOHID granted, `launchctl print`
  shows GUI session; Windows `Get-AuthenticodeSignature` Valid + fleet
  thumbprint on every binary, `sc query Deskflow` RUNNING, GUI `SessionId != 0`;
  mesh round-trip all pairs.
- `fleet-deploy --self-test [--json]`: `--ref HEAD` rebuild+install on every
  host then `fleet-health --check all`; weekly LaunchAgent
  `com.fleet.selftest`. `--dry-run --json` emits the host plan.
- Locking (`flock` / `New-Item`) per repo; `state/last-good.json` per host per
  app; `--rollback [--host H] [--app X]` checks out the recorded commit and
  rebuilds (signing keeps TCC identity stable). Order: clients → server. Final
  table `host | app | commit | signed-by | tcc | mesh | result`.
- Every remote step checks its exit code explicitly (`ssh` exit vs 255, `&`
  + `$LASTEXITCODE`, `git` under PS 5.1). Mouser pull: `git fetch fork &&
  git checkout $FLEET_BRANCH && git pull --ff-only fork $FLEET_BRANCH`.

**Harness (deskflow repo, `harness/`)**

- `scripts/fleet-soak sample --label <launchd-label|service> --exe <path> --out
  <jsonl>` every 60 s from launchd / Task Scheduler. Records pid, start time,
  exe path, `phys_footprint` (via `proc_pid_rusage` RUSAGE_INFO_V4 in a ctypes
  helper — works on hardened-runtime binaries without task ports), RSS,
  compressed, Mach ports (`top -l 2 -stats ports`), threads, fd count; Windows
  PrivateMemorySize64, WorkingSet64, HandleCount, GDI/USER via
  `GetGuiResources`. Scenario is *derived* from app logs (Mouser device-state
  line; deskflow-core role line), never tagged by hand. Heartbeat line every
  sample; sampler git SHA in the header.
- `scripts/fleet-soak report --seat S --proc P --in F --window H
  [--accelerated] [--scenario X] --slope-max MB/h --cap MB --ports-slope-max
  n/h --max-step pct --max-restarts 0 --min-hours 72 --min-completeness 0.95
  --validate --print-metric --print-scenario-source`: Theil–Sen slope, first
  2 h excluded; exit 0 pass, 1 fail, 2 verdict-invalid (gap > 5 min,
  completeness < 95 %, duration < min).
- `scripts/fleet-soak preflight --seat hackintosh`: fails if
  `com.cursor.worker.*` agents are loaded, vitest workers exist, or Spotlight is
  indexing the app bundle.
- `harness/scenarios/<row>.sh|.ps1` drivers, one per matrix row, each runnable
  as `harness/run-scenario.sh <row> --seat hackintosh --iters 1000` and judged
  by Δfootprint over the run. Minimum rows: mouse-absent reconnect loop;
  BT sleep/wake ×200; window show/hide ×1000; Add-Profile open ×200;
  scroll with Input Monitoring denied; scroll with it granted; screen switch
  ×1000; clipboard 10 MB image ×100; epoch flip (peer sleep) ×50; peer
  unreachable heartbeat 2 h; lock/unlock ×50; login-window vhid-bridge 1 h.
- `harness/behavior-bench.sh`: gesture latency p50/p99 and reconnect-on-return
  time, ±10 % of baseline; runs on both Macs and tiny11.
- Mitigation counters: any fix that drops data, restarts anything, or forces
  GC logs a `mitigation:` line; soak requires the count to be 0.

**Swarm protocol (applies to Phases 4–5)**

- Hotspot registry `harness/registry.json` with lock; one auditor+adversary
  pair per hotspot; one PR per hotspot; a PR may not touch two hotspots' files.
- Finding schema: `id, hotspot, seat, scenario-row, file@commit:line, evidence
  (harness run id + Δ), confidence, fix-class {root-cause, mitigation}`. No
  harness run id → it is a hypothesis, not a finding. Citations are checked by
  `harness/check-citations.py` (file:line exists at the pinned commit and
  contains the quoted token).
- Adversary veto: merge only with "reproduced before, absent after, on the
  originating seat". Mitigation-class PRs cannot close a hotspot.
- Compile matrix per PR: macOS arm64, macOS x86_64, Windows MSVC (Deskflow);
  Python 3.12/3.13/3.14 (Mouser). Behavior bench + existing unit tests
  required. PRs merge serially onto `fleet/memory-program` ranked by measured
  MB recovered; the matrix reruns after each merge. Deploy only after the
  merged branch passes the full matrix on hackintosh.

## Implementation Phases

### Phase 1: Kill silent success (deskflow + Mouser repos)

- **Status:** Not started
- **Scope:** Every path that can report success while shipping the wrong thing
  becomes a hard failure. No ad-hoc signing anywhere; identity comes only from
  per-seat env; keychain secrets removed; Windows signing brought in-repo;
  Mouser git flow fixed.
- **Files touched:** `deskflow/CMakeLists.txt`, `deskflow/deploy/mac/deploy.cmake`,
  `deskflow/cmake/MacCodesign.cmake`, `deskflow/scripts/fleet-deploy-macos.sh`,
  `deskflow/scripts/fleet-deploy.sh`, `deskflow/scripts/fleet-deploy-windows.ps1`,
  `deskflow/scripts/build-windows.ps1`, `deskflow/scripts/install-windows.ps1`,
  `deskflow/scripts/install-macos.sh`, `deskflow/scripts/sign-windows.ps1` (new),
  `deskflow/scripts/fleet.env.example`, `deskflow/env.example`,
  `Mouser/build_macos_app.sh`, `Mouser/scripts/build_and_install.py`,
  `Mouser/scripts/windows_install.py`, tests under `deskflow/scripts/tests/`
  (new, bats or bash) and `Mouser/tests/`.
- **Acceptance criteria:** `grep -rn '|| true' deskflow/scripts/*.sh
  deskflow/scripts/*.ps1` returns only lines annotated `# fleet:allow`; CMake
  configure with `-DAPPLE_CODESIGN_DEV=-` fails; `MOUSER_SIGN_IDENTITY=-` fails;
  `fleet.env.example` has no `KEYCHAIN_PASSWORD` keys; `sign-windows.ps1` signs
  every `.exe/.dll` under both install roots; Mouser pull uses `fork` +
  `$FLEET_BRANCH` with `--ff-only` and fails loudly.
- **Validation:** `cd ~/Desktop/deskflow && bash scripts/tests/run.sh &&
  ! grep -rq KEYCHAIN_PASSWORD scripts/fleet.env.example && cd ~/Desktop/Mouser
  && .venv/bin/python -m pytest tests/test_no_hardcoded_signing_secrets.py
  tests/test_build_and_install*.py -q`

### Phase 2: Symmetric controller, doctor, health, self-test (deskflow repo)

- **Status:** Not started
- **Scope:** Any seat can run one command that deploys both apps to all three
  hosts with local pull + local build + local sign, GUI-session routing on
  macOS for both apps, per-repo lock, last-good + rollback, clients-before-
  server, `fleet-doctor`, `fleet-health`, `--self-test`, `--dry-run --json`,
  weekly self-test agent.
- **Files touched:** `deskflow/scripts/fleet-deploy.sh`,
  `deskflow/scripts/fleet-deploy.ps1` (new), `deskflow/scripts/fleet-doctor`
  (new), `deskflow/scripts/fleet-health` (new, bash + ps1 backends),
  `deskflow/scripts/fleet-gui-exec.py` (new, from
  `Mouser/scripts/build_macos_gui_session.py`), `deskflow/scripts/fleet-tcc-probe/`
  (new tiny signed Mach-O), `deskflow/scripts/fleet-deploy-macos.sh`,
  `deskflow/scripts/fleet-deploy-windows.ps1`, `deskflow/scripts/state/` (new,
  gitignored), `deskflow/scripts/launchd/com.fleet.selftest.plist` (new),
  `deskflow/docs/building-signed.md`, `deskflow/scripts/tests/`.
- **Acceptance criteria:** From each seat, `fleet-deploy --dry-run --json`
  lists exactly three distinct targets; `fleet-doctor --host all` exits 0;
  `fleet-deploy --self-test --json` exits 0 and `fleet-health --check all
  --host all` exits 0 (identity exact, no adhoc, identifiers stable, TCC csreq
  cert-based, AX+IOHID granted, Windows Authenticode Valid + fleet thumbprint
  on every binary, service RUNNING, GUI session != 0, mesh round-trip all
  pairs); `--rollback --host macbookpro --app deskflow` restores last-good and
  health stays green; `launchctl print gui/$(id -u)/com.fleet.selftest`
  exists on the initiating seat.
- **Validation:** `manual 1. On macbookpro run scripts/fleet-deploy --self-test
  --json report-mbp.json; 2. On hackintosh run the same to report-hack.json;
  3. On tiny11 run scripts/fleet-deploy.ps1 -SelfTest -Json report-t11.json;
  4. jq -e '.ok==true' on all three; 5. scripts/fleet-deploy --rollback --host
  macbookpro --app deskflow && scripts/fleet-health --host macbookpro`

### Phase 3: Harness and hackintosh baseline (deskflow repo)

- **Status:** Not started
- **Scope:** `fleet-soak` sampler + report + preflight, scenario matrix
  drivers, behavior bench, citation checker, registry; install samplers on all
  seats; capture a 72 h baseline on hackintosh with noise removed.
- **Files touched:** `deskflow/scripts/fleet-soak` (new, python3),
  `deskflow/harness/scenarios/*.sh|*.ps1` (new, ≥12), `deskflow/harness/run-scenario.sh`
  (new), `deskflow/harness/behavior-bench.sh` (new),
  `deskflow/harness/check-citations.py` (new), `deskflow/harness/registry.json`
  (new), `deskflow/scripts/launchd/com.fleet.soak.plist` (new),
  `deskflow/scripts/fleet-soak-task.ps1` (new), `deskflow/harness/README.md`.
- **Acceptance criteria:** `fleet-soak report --print-metric` prints
  `phys_footprint` on macOS and `private_bytes` on Windows;
  `--print-scenario-source` prints `observed`; `--validate` fails on a synthetic
  gap; each scenario driver runs to completion on hackintosh and emits a Δ;
  `fleet-soak preflight --seat hackintosh` exits 0 with cursor workers
  unloaded and vitest capped; baseline report for Mouser and deskflow-core on
  hackintosh exists in `harness/baselines/2026-09-*.json` showing the
  pre-fix slope.
- **Validation:** `cd ~/Desktop/deskflow && python3 -m pytest scripts/tests/test_fleet_soak.py -q
  && scripts/fleet-soak preflight --seat hackintosh && for r in
  harness/scenarios/*.sh; do harness/run-scenario.sh "$(basename "$r" .sh)"
  --seat hackintosh --iters 10 --smoke; done`

### Phase 4: Mouser hotspots (Mouser repo)

- **Status:** Not started
- **Scope:** Swarm executes the Mouser registry under the protocol. Ordered
  by evidence: (1) `core/macos_iokit_scroll.py:167-171` manager assigned before
  any `stop()`; (2) `core/hid_gesture.py:3266` reconnect backoff to ≥ 5 s with
  a single long-lived IOHIDManager and a device-arrival notification instead
  of polling; (3) `main_qml.py:769-892` build the `NSStatusItem`/target once,
  update image only; (4) bounded `_report_queue` with a drop counter
  (mitigation-class, cannot close alone) plus the root cause of the stalled
  consumer; (5) `core/app_detector.py:189` switch to
  `NSWorkspaceDidActivateApplicationNotification`; (6) `main_qml.py:1004`
  cache NSImage-backed icons across Add-Profile opens; (7) `mouse_hook_macos.py`
  reduce per-event bridge crossings.
- **Files touched:** `Mouser/core/macos_iokit_scroll.py`,
  `Mouser/core/hid_gesture.py`, `Mouser/main_qml.py`, `Mouser/core/app_detector.py`,
  `Mouser/core/mouse_hook_macos.py`, `Mouser/core/engine.py`, `Mouser/tests/`,
  `deskflow/harness/registry.json`.
- **Acceptance criteria:** each hotspot PR carries a finding with a harness run
  id, adversary sign-off on hackintosh, compile/test pass on Python
  3.12/3.13/3.14, behavior bench within ±10 %; accelerated gate passes for
  every Mouser scenario row; `grep -c mitigation:` in Mouser logs over the
  accelerated run is 0 for root-cause PRs.
- **Validation:** `cd ~/Desktop/Mouser && .venv/bin/python -m pytest -q && cd
  ~/Desktop/deskflow && python3 harness/check-citations.py harness/registry.json
  && for r in mouse-absent-reconnect bt-sleep-wake window-toggle add-profile
  scroll-im-denied scroll-im-granted; do harness/run-scenario.sh $r --seat
  hackintosh --iters 1000 && scripts/fleet-soak report --accelerated --proc
  mouser --in harness/runs/$r.jsonl --window 2 --slope-max 5
  --ports-slope-max 10; done && harness/behavior-bench.sh --proc mouser`

### Phase 5: Deskflow hotspots (deskflow repo)

- **Status:** Not started
- **Scope:** Swarm executes the Deskflow registry. Ordered: (1) Coordinator
  peer connects off the worker thread and never inside an input callback
  (absorbs the Aug-31 audit's item 2), non-blocking with per-peer backoff;
  (2) AutoModeRunner reuses screen/tap across epoch flips where the role
  allows, or bounds flip rate; (3) `TCPSocket` output buffer cap + `StreamBuffer`
  peek without full consolidation; (4) single clipboard copy on the server,
  size check without `marshall()`; (5) retain CFRunLoops in the two monitors;
  (6) replace 1 s pasteboard/AX polling with change-count checks at lower
  rate; (7) `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` for Release, measured.
- **Files touched:** `src/lib/coordination/Coordinator.cpp`,
  `src/lib/coordination/CoordinationMesh.cpp`, `src/apps/deskflow-core/AutoModeRunner.cpp`,
  `src/lib/net/TCPSocket.cpp`, `src/lib/net/SecureSocket.cpp`,
  `src/lib/io/StreamBuffer.cpp`, `src/lib/server/Server.cpp`, `src/lib/server/Server.h`,
  `src/lib/server/ClientProxy1_0.cpp`, `src/lib/platform/OSXScreen.mm`,
  `src/lib/platform/OSXKeyboardRelayMonitor.mm`, `src/lib/platform/OSXLocalInputMonitor.mm`,
  `CMakeLists.txt`, `src/unittests/`, `harness/registry.json`.
- **Acceptance criteria:** as Phase 4, with the compile matrix macOS arm64 /
  macOS x86_64 / Windows MSVC; accelerated gate passes for epoch-flip,
  peer-unreachable, screen-switch, clipboard-10MB, lock-unlock, login-bridge
  rows; deskflow-core restarts during the accelerated runs = 0.
- **Validation:** `cd ~/Desktop/deskflow && cmake --build build --target
  unittests && build/bin/unittests && python3 harness/check-citations.py
  harness/registry.json && for r in epoch-flip peer-unreachable screen-switch
  clipboard-10mb lock-unlock login-bridge; do harness/run-scenario.sh $r --seat
  hackintosh --iters 1000 && scripts/fleet-soak report --accelerated --proc
  deskflow-core --in harness/runs/$r.jsonl --window 2 --slope-max 5
  --ports-slope-max 10 --max-restarts 0; done && harness/behavior-bench.sh
  --proc deskflow-core`

### Phase 6: Fleet soak, canary, sign-off (deskflow repo)

- **Status:** Not started
- **Scope:** Deploy the merged `fleet/memory-program` branch via the
  self-tested deploy, run the 7-day soak on all seats, add the canary job, and
  record the per-seat × role sign-off table.
- **Files touched:** `harness/soak/2026-*/`, `.github/workflows/canary-memory.yml`
  (new; runs the scenario matrix on one seat via self-hosted runner or
  documents the manual trigger), `docs/building-signed.md`, `docs/FORK_ROADMAP.md`.
- **Acceptance criteria:** the Success Criteria block below is green on every
  seat; sign-off table has one row per seat × {server, client} × {mouse
  present, absent}; canary is required on PRs touching `requirements.txt`,
  `Mouser-*.spec`, `vcpkg`/Qt version, or upstream merges.
- **Validation:** `cd ~/Desktop/deskflow && for s in macbookpro hackintosh
  tiny11; do for p in mouser deskflow-core deskflow; do scripts/fleet-soak
  report --seat $s --proc $p --in harness/soak/latest/$s-$p.jsonl --window 168
  --min-hours 72 --min-completeness 0.95 --slope-max 0.1 --ports-slope-max 1
  --max-step 5 --max-restarts 0 --validate; done; done`

## Alternative Approaches Considered

- **Rewrite Mouser natively (Swift/C++ daemon + thin UI).** Rejected for now:
  months, full TCC/signing churn across the fleet; revisit if flat footprint
  is achieved and idle size still matters.
- **Cold 40-agent line-by-line audit without a harness.** Rejected: produces
  findings without a number to beat; the strongest suspects are native-churn
  patterns that need reproduction on hackintosh.
- **Fix the three confirmed bugs and watch.** Rejected: leaves the reconnect
  loop and Coordinator hot loops, which are the hackintosh-specific
  conditions.
- **Push-to-deploy on a timer.** Deferred: fires a build on every merge during
  a high-churn period, no interactive gate.
- **`fleet-buildd` LaunchAgent build worker instead of osascript routing.**
  Deferred: better (no Terminal spam, survives locked screen) but larger;
  osascript routing already works in Mouser and is extended first. Tracked as
  a future consideration.
- **30-day RSS endpoint test.** Rejected by adversarial review: RSS excludes
  compressed pages (the observed failure had 851 MB RSS / 4.4 GB compressed),
  and an endpoint test passes a leak that fires three times a month.

## Success Criteria

```success-criteria
GOAL: Mouser and Deskflow run on all three seats with a flat physical-memory footprint under every scenario in the matrix, deployed by a self-tested fleet deploy that any seat can drive.

SUCCESS CRITERIA:
- Any seat plans exactly three distinct targets | verify: scripts/fleet-deploy --dry-run --json - | jq -e '.hosts|length==3 and (map(.target)|unique|length==3)'
- Doctor preconditions hold on all hosts | verify: scripts/fleet-doctor --host all
- No keychain secrets in fleet config | verify: ! grep -rq KEYCHAIN_PASSWORD scripts/fleet.env*
- No ad-hoc identity anywhere | verify: ! grep -q 'APPLE_CODESIGN_DEV=-' build/CMakeCache.txt && scripts/fleet-health --check no-adhoc --host all
- Exact identity on every Mach-O of both apps on both Macs | verify: scripts/fleet-health --check sign --host all
- Stable code identifiers | verify: scripts/fleet-health --check identifiers --host all
- TCC csreq cert-based and AX + IOHID granted, surviving a rebuild | verify: scripts/fleet-deploy --self-test --ref HEAD && scripts/fleet-health --check tcc --host all
- Windows: every .exe/.dll Valid with the fleet thumbprint | verify: scripts/fleet-health --check authenticode --host tiny11
- GUI in console session; Deskflow service RUNNING | verify: scripts/fleet-health --check session --host all
- Mesh round-trip between all pairs | verify: scripts/fleet-health --check mesh
- Rollback restores last-good and stays healthy | verify: scripts/fleet-deploy --rollback --host macbookpro --app deskflow && scripts/fleet-health --host macbookpro
- Weekly self-test installed | verify: launchctl print gui/$(id -u)/com.fleet.selftest
- Metric is phys_footprint / private_bytes, RSS advisory only | verify: scripts/fleet-soak report --print-metric | grep -Eqx 'phys_footprint|private_bytes'
- Scenario source is observed, never manual | verify: scripts/fleet-soak report --print-scenario-source | grep -qx observed
- hackintosh noise removed before any run | verify: scripts/fleet-soak preflight --seat hackintosh
- Accelerated gate per scenario row on hackintosh (footprint ≤ 5 MB/h, ports ≤ 10/h at 100x) | verify: for r in $(ls harness/scenarios | sed 's/\..*//'); do scripts/fleet-soak report --accelerated --proc auto --in harness/runs/$r.jsonl --window 2 --slope-max 5 --ports-slope-max 10 --max-restarts 0; done
- 7-day soak slope ≤ 0.1 MB/h, ports ≤ 1/h, max daily step ≤ 5 %, restarts = 0, per seat per process | verify: for s in macbookpro hackintosh tiny11; do for p in mouser deskflow-core deskflow; do scripts/fleet-soak report --seat $s --proc $p --in harness/soak/latest/$s-$p.jsonl --window 168 --min-hours 72 --min-completeness 0.95 --slope-max 0.1 --ports-slope-max 1 --max-step 5 --max-restarts 0 --validate; done; done
- Idle caps: Mouser ≤ 150 MB (window hidden), deskflow-core ≤ 60 MB, deskflow GUI ≤ 150 MB, deskflow-daemon ≤ 40 MB | verify: scripts/fleet-soak report --proc mouser --cap 150 --in harness/soak/latest/hackintosh-mouser.jsonl && scripts/fleet-soak report --proc deskflow-core --cap 60 --in harness/soak/latest/hackintosh-deskflow-core.jsonl && scripts/fleet-soak report --proc deskflow --cap 150 --in harness/soak/latest/hackintosh-deskflow.jsonl && scripts/fleet-soak report --proc deskflow-daemon --cap 40 --in harness/soak/latest/tiny11-deskflow-daemon.jsonl
- Mitigation counters are zero over the soak | verify: ! grep -rq 'mitigation:' harness/soak/latest/logs/
- Behavior within ±10 % of baseline | verify: harness/behavior-bench.sh --all
- Every registry finding cites a real line at the pinned commit | verify: python3 harness/check-citations.py harness/registry.json
- Deskflow unit tests and Mouser tests pass | verify: (cd ~/Desktop/deskflow && cmake --build build --target unittests && build/bin/unittests) && (cd ~/Desktop/Mouser && .venv/bin/python -m pytest -q)

NON-GOALS:
- Rewriting Mouser in Swift/C++.
- Notarization or Developer ID distribution (private fleet).
- Push-to-deploy / self-updating seats.
- Qt module trimming beyond what Mouser-mac.spec already excludes.
- Fixing vitest fan-out or the Bloc language server (handled separately; only capped/disabled as harness preconditions).

VERIFICATION COMMAND: scripts/fleet-doctor --host all && ! grep -rq KEYCHAIN_PASSWORD scripts/fleet.env* && scripts/fleet-health --check all --host all && scripts/fleet-soak preflight --seat hackintosh && python3 harness/check-citations.py harness/registry.json && for s in macbookpro hackintosh tiny11; do for p in mouser deskflow-core deskflow; do scripts/fleet-soak report --seat $s --proc $p --in harness/soak/latest/$s-$p.jsonl --window 168 --min-hours 72 --min-completeness 0.95 --slope-max 0.1 --ports-slope-max 1 --max-step 5 --max-restarts 0 --validate; done; done && harness/behavior-bench.sh --all
```

## Success Metrics

- Footprint slope on hackintosh, Mouser: from ~24 MB/h today to ≤ 0.1 MB/h.
- deskflow-core on hackintosh: from 699 MB to ≤ 60 MB idle, slope ≤ 0.1 MB/h.
- Deploy self-test: 3/3 seats green, weekly, with a JSON report archived.
- TCC re-grants after deploy: 0 (was: every deploy on macbookpro).
- Unsigned binaries on tiny11: 0 (was: Mouser.exe, deskflow-vhid-bridge.exe).

## Dependencies & Prerequisites

- All three seats reachable by SSH from each other (verified 2026-09-16).
- `deskflow/.env` on both Macs with `DESKFLOW_CODESIGN_ID` (today only tiny11
  has `.env`); `Mouser/.env.local` on both Macs with `MOUSER_SIGN_IDENTITY`
  (hackintosh currently uses `MOUSER_TEAM_ID` — standardize).
- Rotate the two keychain passwords exposed in `scripts/fleet.env` /
  `.bak-20260814` on hackintosh (Aug-31 audit) and delete the backup.
- Terminal Automation permission granted on both Macs for the GUI-session
  route; console user logged in during deploys.
- Pin Python per seat (3.12.10 / 3.13.2 / 3.14.7) in `fleet-doctor`.
- vitest `maxWorkers` capped in
  `sea_trials_universal/scripts/error_report_adjudication/*/client/vite.config.ts`
  and `com.cursor.worker.*` agents unloaded before hackintosh harness runs.

## Risk Analysis & Mitigation

| Risk | Mitigation |
|---|---|
| Deploy restarts the Deskflow server mid-rollout and strands the mouse | clients-before-server order; final health gate includes mesh round-trip |
| GUI-session route hangs on a modal Terminal dialog | doctor precondition; 1800 s timeout with log sentinel; `fleet-buildd` as follow-up |
| Fix verified on macbookpro does not reproduce on hackintosh | protocol: reproduction and sign-off only on the originating seat |
| Agents ship mitigations that flatten the curve | fix-class tagging, mitigation counters must be 0, adversary veto |
| 40 PRs conflict on `hid_gesture.py` / `Coordinator.cpp` | one PR per hotspot, no shared-file overlap, serial merge with matrix rerun |
| Sampler dies silently | heartbeat + completeness ≥ 95 % or verdict-invalid |
| Heap snapshots stall a 5 GB process | snapshots only < 500 MB or on demand; soak judged from sampler only |
| Upstream/PySide6/hidapi bump reintroduces churn | canary job on dependency-touching PRs |

## Resource Requirements

- Agents: ~40 in Phases 4–5 as auditor/adversary pairs (≈ 12 Mouser hotspots,
  ≈ 8 Deskflow hotspots), plus 3 review agents per plan phase.
- Wall clock: Phases 1–3 sequential (each one context window plus manual
  self-test runs); Phases 4–5 parallel; Phase 6 needs ≥ 7 days of soak.
- Infra: hackintosh must be free of vitest/cursor-worker noise during runs.

## Future Considerations

- `fleet-buildd` per-seat build worker replacing osascript routing.
- Push-to-deploy once the canary is trusted.
- Native Mouser HID daemon if idle footprint becomes the goal.
- Upstreaming Deskflow fixes (Coordinator is fork-only; socket/clipboard
  changes are upstreamable).

## Documentation Plan

- `deskflow/docs/building-signed.md`: symmetric controller, doctor, health,
  self-test, rollback.
- `deskflow/harness/README.md`: sampler, report CLI, scenario matrix, protocol.
- `deskflow/docs/FORK_ROADMAP.md`: link this plan and the sign-off table.
- `Mouser/DEVELOPMENT.md`: signing identity requirement (no ad-hoc), GUI-session
  route now shared.

## References & Research

### Internal References

- Brainstorm: `docs/brainstorm/2026-09-16-fleet-memory-program-brainstorm-doc.md`
- Prior audit: `docs/brainstorm/2026-08-31-fleet-hardening-audit-brainstorm-doc.md` (SYMPTOM 1, SECURITY, ORDER OF WORK)
- Deploy: `scripts/fleet-deploy.sh:82-118,139-153,170`, `scripts/fleet-deploy-macos.sh:24-32,47-59,61,76-80,89-91`, `scripts/fleet-deploy-windows.ps1:24-26,46,57,64-67`, `scripts/install-windows.ps1:329-334,434`, `scripts/install-macos.sh:147-152`
- Signing: `CMakeLists.txt:164,210-218`, `deploy/mac/deploy.cmake:13-27`, `cmake/MacCodesign.cmake:40-67`, `Mouser/build_macos_app.sh:13,154-205`, `Mouser/scripts/build_and_install.py:180-214`, `Mouser/scripts/build_macos_gui_session.py:42-122`
- Mouser hotspots: `core/macos_iokit_scroll.py:148-171`, `core/hid_gesture.py:507-815,1562,2922-2956,3199,3266`, `main_qml.py:645-657,709-713,756,769-892,984-1029`, `core/app_detector.py:189,276`, `core/mouse_hook_macos.py:222,275,293,409,565,709`, `core/engine.py:247,805-816`
- Deskflow hotspots: `src/lib/coordination/Coordinator.cpp:37-38,263-294,832-957`, `src/lib/coordination/CoordinationMesh.cpp:333-381`, `src/apps/deskflow-core/AutoModeRunner.cpp:119-183`, `src/lib/net/TCPSocket.cpp:23,157,316`, `src/lib/io/StreamBuffer.cpp:16-116`, `src/lib/server/Server.cpp:764,1641-1647,1980-1990`, `src/lib/server/Server.h:429,485`, `src/lib/platform/OSXScreen.mm:159,680-683,714-731,836,1416-1475,1541,1740`, `src/lib/platform/OSXKeyboardRelayMonitor.mm:288`, `src/lib/platform/OSXLocalInputMonitor.mm:91`, `src/lib/gui/core/CoreProcess.cpp:177-203,379-390`
- Build: `cmake/Libraries.cmake:12,22-36`, `src/apps/CMakeLists.txt:48-52`, `scripts/install-login-bridge-macos.sh:11-13,126-146`

### External References

- Jetsam / phys_footprint: `proc_pid_rusage` RUSAGE_INFO_V4 (`<sys/resource.h>`), `top -stats mem,cmprs,ports`
- Windows: `Get-Process` PrivateMemorySize64, `GetGuiResources`, `Get-AuthenticodeSignature`, `signtool sign /sha1 … /fd SHA256`
- macOS TCC requirement decoding: `csreq -r- -t`

### Related Work

- Commits: deskflow `843d0743b` (CFRunLoop retain), `b0ceb27b8` (UIAccess), `ccb02ca3c` (bundle id), `b05f46928` (fleet audit); Mouser `a3223da` (GUI-session signing route)
- Related session findings (2026-09-15): Bloc language server scan blowup and vitest fan-out on hackintosh — out of scope, listed as harness preconditions
