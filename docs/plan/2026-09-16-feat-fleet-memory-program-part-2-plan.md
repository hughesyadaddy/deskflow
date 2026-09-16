---
title: "feat: fleet memory program part 2 — Mouser signing and hotspots"
type: feat
date: 2026-09-16
repo: ~/Desktop/Mouser
---

## feat: fleet memory program — part 2 (Mouser repo) - Extensive

Umbrella plan with full background, hotspot evidence and adversarial-review
rationale: `~/Desktop/deskflow/docs/plan/2026-09-16-feat-fleet-memory-program-plan.md`.
This part is standalone for `/build` and applies only to `~/Desktop/Mouser`
(remote is named `fork`, branch `main`).

## Dependencies

- Part 1 Phases 1–5 (deskflow repo) must be merged and deployed first: they
  provide `tools/fleet-gui-exec.py`, `tools/fleet-soak`, `harness/` (scenario
  drivers, `swarm.workflow.js`, baselines), and the Mouser fleet-pull fix in
  `deskflow/scripts/fleet-deploy-*.sh|.ps1` (part 1 Phase 2).
- Part 1 Phase 7 (soak) runs after this part's Phase 2 merges.

## Overview

Make Mouser's macOS signing hard-fail without an identity, then execute the
Mouser hotspot registry under the swarm protocol — one PR per hotspot, each
proven on hackintosh with harness numbers.

## Problem Statement

Mouser 3.6.0 reached 5.3 GB footprint after 9 days on hackintosh vs 64 MB on
macbookpro. hackintosh is the seat where the mouse is normally absent, so
`core/hid_gesture.py:3266` retries connection at 4 Hz forever, each pass doing
a full IOHIDManager create/open/copy/close plus per-candidate opens
(`:1562,2922-2956`). `core/macos_iokit_scroll.py:167-171` is a confirmed bug:
on `start()` failure `stop()` runs before `self._manager` is assigned, leaking
an IOHIDManager per scroll tick whenever Input Monitoring is denied — the
state every ad-hoc-signed deploy leaves the app in. `main_qml.py:769-892`
rebuilds the `NSStatusItem`, its PyObjC target and a `QSvgRenderer` on every
window show/hide (twice). Signing: `build_macos_app.sh:201-203` falls back to
ad-hoc when the identity is empty and `build_and_install.py:181-183` passes
`MOUSER_SIGN_IDENTITY=-` verbatim; ad-hoc signatures reset TCC on every deploy.
The Windows build is unsigned.

## Technical Approach

- Signing: `build_macos_app.sh` and `scripts/build_and_install.py` reject an
  empty or `-` identity with a non-zero exit; `build_and_install.py` gains
  `--dry-run` (resolve identity, print plan, build nothing); identity comes only from
  `.env.local` `MOUSER_SIGN_IDENTITY` (hackintosh migrates from
  `MOUSER_TEAM_ID`). `scripts/build_macos_gui_session.py` becomes a thin
  wrapper over `deskflow/tools/fleet-gui-exec.py` when present (fallback to
  its own implementation). Windows install calls
  `deskflow/scripts/sign-windows.ps1`; an unset `DESKFLOW_SIGN_THUMBPRINT` or
  a missing signtool is fatal on Windows.
- Hotspot fixes are executed by `deskflow/harness/swarm.workflow.js` via the
  Workflow tool with `args: {repoPath: "/Users/alexhughes/Desktop/Mouser",
  ghRepo: "hughesyadaddy/Mouser", registry:
  "/Users/alexhughes/Desktop/deskflow/harness/registry.json"}` under the part-1 swarm protocol (auditor+adversary
  pair, one `hotspot/<id>` branch and PR per hotspot onto
  `fleet/memory-program`, finding with harness run id, adversary veto on
  hackintosh, mitigation-class cannot close, `mitigation:` log counter).
  Interpreter matrix = the pinned interpreter of each seat (macbookpro 3.13.2,
  hackintosh 3.14.7, tiny11 3.12.10).

## Implementation Phases

### Phase 1: Signing hard-fail

- **Status:** Not started
- **Scope:** No ad-hoc signing path remains; identity key standardized on both
  Macs; Windows dist signed via the fleet signtool wrapper;
  `.env.local.example` documents `MOUSER_SIGN_IDENTITY` only.
- **Files touched:** `build_macos_app.sh`, `scripts/build_and_install.py`,
  `scripts/build_macos_gui_session.py`, `scripts/windows_install.py`,
  `scripts/install_lifecycle.py`, `.env.local.example`, `DEVELOPMENT.md`,
  `readme_mac_osx.md`, `tests/test_no_hardcoded_signing_secrets.py`,
  `tests/test_build_and_install_signing.py` (new), `tests/test_build_macos_gui_session.py`.
- **Acceptance criteria:** `MOUSER_SIGN_IDENTITY=- python3
  scripts/build_and_install.py --dry-run` exits non-zero; empty identity exits
  non-zero; `grep -n 'sign -' build_macos_app.sh` returns nothing;
  `.env.local` on hackintosh contains `MOUSER_SIGN_IDENTITY`; on tiny11 every `.exe/.dll`
  under `dist/Mouser` is `Valid` with the fleet thumbprint after a build.
- **Validation:** `.venv/bin/python -m pytest tests/test_no_hardcoded_signing_secrets.py
  tests/test_build_and_install_signing.py tests/test_build_macos_gui_session.py
  -q && ! grep -q 'sign -' build_macos_app.sh && ! MOUSER_SIGN_IDENTITY=-
  .venv/bin/python scripts/build_and_install.py --dry-run && ssh hackintosh
  'grep -q MOUSER_SIGN_IDENTITY ~/Desktop/Mouser/.env.local' && ssh tiny11
  'powershell -NoProfile -Command "Get-ChildItem C:/Users/alexh/Desktop/Mouser/dist/Mouser
  -Recurse -Include *.exe,*.dll | Get-AuthenticodeSignature | Where-Object
  Status -ne Valid | Measure-Object | ForEach-Object { exit $_.Count }"'`

### Phase 2: Mouser hotspots (one PR per hotspot)

- **Status:** Not started
- **Scope:** First task: `git checkout -b fleet/memory-program main`. Then
  `/build` runs `~/Desktop/deskflow/harness/swarm.workflow.js` via the
  Workflow tool for this repo; one `hotspot/<id>` branch and PR per entry;
  PRs open `--draft --label needs-harness` and become ready only after
  `deskflow/harness/check-pr.sh` exits 0; Alex merges ranked by `deltaMB`.
  Registry order by evidence then measured MB: (1) `core/macos_iokit_scroll.py:167-171` assign the manager
  before any `stop()` and release on failure; (2) `core/hid_gesture.py:3266`
  reconnect backoff ≥ 5 s with one long-lived IOHIDManager and a device-arrival
  notification instead of polling, and negative-cache the enumeration too;
  (3) `main_qml.py:769-892` build the `NSStatusItem`/target once, update the
  image only; (4) `core/app_detector.py:189` switch to
  `NSWorkspaceDidActivateApplicationNotification`; (5) `main_qml.py:1004`
  cache NSImage-backed icons across Add-Profile opens; (6)
  `core/mouse_hook_macos.py:293-336` reduce per-event bridge crossings;
  (7) `core/hid_gesture.py:522` bounded `_report_queue` with a drop counter —
  mitigation-class, may ship only alongside a root-cause finding for the
  stalled consumer that carries a harness run id.
- **Files touched:** `core/macos_iokit_scroll.py`, `core/hid_gesture.py`,
  `main_qml.py`, `core/app_detector.py`, `core/mouse_hook_macos.py`,
  `core/engine.py`, `tests/test_macos_iokit_scroll.py` (new),
  `tests/test_hid_gesture_reconnect.py` (new), `tests/test_app_detector.py`
  (new or extended), `tests/test_status_item.py` (new, PyObjC-mocked);
  `deskflow/harness/registry.json` (cross-repo).
- **Acceptance criteria:** each PR carries a finding with a harness run id and
  adversary sign-off on hackintosh; tests pass on the pinned interpreter of
  each seat; behavior bench within ±10 %; accelerated gate passes for
  mouse-absent-reconnect, bt-sleep-wake (manual row, operator-run),
  window-toggle, add-profile, scroll-im-denied, scroll-im-granted;
  `mitigation:` count over the runs is 0
  for root-cause PRs.
- **Validation:** `.venv/bin/python -m pytest -q && ssh hackintosh 'cd
  ~/Desktop/Mouser && .venv/bin/python -m pytest -q' && ssh tiny11
  'powershell -NoProfile -Command "cd C:/Users/alexh/Desktop/Mouser;
  .venv/Scripts/python.exe -m pytest -q"' && cd ~/Desktop/deskflow && python3
  harness/check-citations.py harness/registry.json && for r in
  mouse-absent-reconnect bt-sleep-wake window-toggle add-profile
  scroll-im-denied scroll-im-granted; do harness/run-scenario.sh $r --seat
  hackintosh --iters 1000 && tools/fleet-soak report --accelerated --proc
  mouser --in harness/runs/$r.jsonl --window 2 --slope-max 5
  --ports-slope-max 10 --max-restarts 0; done && harness/behavior-bench.sh
  --proc mouser`

## Success Criteria

```success-criteria
GOAL: Mouser's footprint is flat under every Mouser scenario row on hackintosh and every build is signed with a stable identity on every seat.

SUCCESS CRITERIA:
- No ad-hoc signing path | verify: ! grep -q 'sign -' build_macos_app.sh && ! MOUSER_SIGN_IDENTITY=- .venv/bin/python scripts/build_and_install.py --dry-run
- Identity key standardized on both Macs | verify: grep -q MOUSER_SIGN_IDENTITY .env.local && ssh hackintosh 'grep -q MOUSER_SIGN_IDENTITY ~/Desktop/Mouser/.env.local'
- Windows dist fully signed | verify: ssh tiny11 'powershell -NoProfile -Command "Get-ChildItem C:/Users/alexh/Desktop/Mouser/dist/Mouser -Recurse -Include *.exe,*.dll | Get-AuthenticodeSignature | Where-Object Status -ne Valid | Measure-Object | ForEach-Object { if ($_.Count -ne 0) { exit 1 } }"'
- Tests pass on each seat's pinned interpreter | verify: .venv/bin/python -m pytest -q && ssh hackintosh 'cd ~/Desktop/Mouser && .venv/bin/python -m pytest -q'
- Every registry finding cites a real line | verify: (cd ~/Desktop/deskflow && python3 harness/check-citations.py harness/registry.json)
- Accelerated gate on hackintosh for all Mouser rows | verify: (cd ~/Desktop/deskflow && for r in mouse-absent-reconnect bt-sleep-wake window-toggle add-profile scroll-im-denied scroll-im-granted; do tools/fleet-soak report --accelerated --proc mouser --in harness/runs/$r.jsonl --window 2 --slope-max 5 --ports-slope-max 10 --max-restarts 0; done)
- Mitigation counters zero for root-cause PRs | verify: (cd ~/Desktop/deskflow && ! grep -rq 'mitigation:' harness/runs/logs/mouser/)  # same path convention as part 1 §Swarm workflow contract
- Behavior within ±10 % | verify: (cd ~/Desktop/deskflow && harness/behavior-bench.sh --proc mouser)

NON-GOALS:
- Native rewrite; Qt module trimming beyond Mouser-mac.spec; changing gesture semantics.

VERIFICATION COMMAND: ! grep -q 'sign -' build_macos_app.sh && .venv/bin/python -m pytest -q && ssh hackintosh 'cd ~/Desktop/Mouser && .venv/bin/python -m pytest -q' && (cd ~/Desktop/deskflow && python3 harness/check-citations.py harness/registry.json && for r in mouse-absent-reconnect bt-sleep-wake window-toggle add-profile scroll-im-denied scroll-im-granted; do tools/fleet-soak report --accelerated --proc mouser --in harness/runs/$r.jsonl --window 2 --slope-max 5 --ports-slope-max 10 --max-restarts 0; done && harness/behavior-bench.sh --proc mouser)
```

## References

Umbrella plan §References. Key Mouser lines: `build_macos_app.sh:13,154-205`,
`scripts/build_and_install.py:35-52,180-214,241-249`, `scripts/build_macos_gui_session.py:42-122`,
`scripts/install_lifecycle.py:60-101,176-204`, `core/macos_iokit_scroll.py:148-171`,
`core/hid_gesture.py:507-815,1562,2817-2863,2922-2956,3199,3266`,
`main_qml.py:645-657,709-713,756,769-892,984-1029`, `core/app_detector.py:189,276`,
`core/mouse_hook_macos.py:222,275,293-336,409,565,709`, `core/engine.py:247,805-816`,
`tests/test_no_hardcoded_signing_secrets.py:18-33,126-140`.
