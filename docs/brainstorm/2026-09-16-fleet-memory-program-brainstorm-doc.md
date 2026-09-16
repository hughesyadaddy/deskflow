---
date: 2026-09-16
topic: fleet-memory-program
---

# Fleet Memory Program: Mouser + Deskflow steady-state RSS, and any-seat fleet deploy

## What We're Building

Two memory-optimization plans (Plan A: Mouser, Plan B: Deskflow) executed by a
large adversarial audit swarm, gated by a hardened fleet deploy that works from
any of the three machines. Success bar: **steady-state RSS** — both apps run for
30 days on every machine without growing, and deskflow-core never re-enters a
CPU death spiral. Idle footprint is secondary (report it, don't chase it).

Trigger: on 2026-09-15 the hackintosh hit ~0 free RAM. Mouser 3.6.0 had grown
to 5.3 GB over 9 days; deskflow-core sat at 699 MB (vs ~30 MB on the other two
machines). Those were two of several stacked consumers (vitest fan-out and the
Bloc language server were the others and are handled separately).

Fixes ship through the existing fleet-deploy scripts, which already do the
right thing (each machine `git pull`s its own checkout and builds + signs
locally with its own identity) but can only be driven from the hackintosh.

## Why This Approach

**Memory: instrument first, then audit, then fix in place.**
Considered (a) a cold "40 agents read every line" audit, (b) fix the three
already-confirmed bugs and watch, (c) instrument → hypothesis-driven audit →
fix. Chose (c): the research pass already produced ~22 ranked file:line
hotspots, most marked *inference*. A cheap measurement harness (RSS, Mach
ports, `heap`/`leaks` snapshots, reconnect-scenario reproduction) turns those
into confirmed/rejected before anyone edits code, and gives the swarm a
ground-truth number to beat. (a) produces noise without a baseline; (b) risks
missing the actual leak because the strongest Mouser suspects are native churn
patterns, not the confirmed bug.

**Mouser stays Python.** "Bare native" was considered (Swift/C++ HID daemon +
thin UI). Rejected for now: months of work and full TCC/signing churn across the
fleet. Fix-in-place targets flat RSS at ~60–100 MB idle (PySide6+QML floor).
A native daemon remains a follow-up brainstorm if flat-RSS is achieved and idle
footprint still matters.

**Deploy: symmetric controller.** Considered designated controller (status
quo) and push-to-deploy (self-updating timers). Chose symmetric: every machine
carries its own `fleet.env` and can drive the whole fleet; the remote side is
unchanged (pull + local build + local sign). Push-to-deploy revisited after the
memory work lands — it would fire a build on every merge during a high-churn
period with no interactive gate.

**Swarm shape.** ~40 agents, but structured, not flat: each hotspot gets an
*auditor* (prove or disprove with code reading + harness data) paired with an
*adversary* (find why the auditor is wrong / what else the fix breaks). Fixes
ranked by measured MB recovered, not by count of findings. Deploy hardening
gets its own auditor/adversary pairs that must run the deploy from each of the
three seats.

## Key Decisions

- **Success metric = 30-day flat RSS per machine, per process**, sampled by a
  small launchd/scheduled-task logger written as part of the harness. Deploy
  is not "done" until the post-deploy health gate passes (version, signature
  verify, RSS after 60 s, service/agent running).
- **Deploy hardening is a gate, not a workstream**: nothing from Plan A/B ships
  until any seat can run `fleet-deploy` end-to-end. Concretely: `fleet.env` on
  all three machines (`local` for self); a `fleet-deploy.ps1` controller for
  tiny11; keychain password never on the SSH command line (target reads a
  `chmod 600` file locally, or routes via the console session like
  `Mouser/scripts/build_macos_gui_session.py`); Windows signs `Mouser.exe` and
  `deskflow-vhid-bridge.exe` with the existing "Deskflow Fleet Code Signing"
  cert (they ship unsigned today); a `fleet-doctor` preflight that diffs the
  three configs, toolchains, and cert presence.
- **Mouser: fix in place.** First targets, by evidence strength:
  1. `core/macos_iokit_scroll.py:167-171` — confirmed bug: on `start()` failure
     `stop()` runs before `self._manager` is assigned, leaking an IOHIDManager
     per scroll tick while Input Monitoring is denied.
  2. `core/hid_gesture.py:3266` — reconnect loop at 4 Hz forever when the
     receiver is present but the mouse is on another KVM host (the normal
     state in a KVM fleet): full IOHIDManager create/open/copy/close per pass.
  3. `main_qml.py:769-892` — `NSStatusItem` + PyObjC target + `QSvgRenderer`
     rebuilt on every window show/hide (twice: 0 ms and 250 ms).
  4. `core/hid_gesture.py:522` unbounded `queue.Queue` fed by the IOKit
     callback; `core/app_detector.py:189` 3.3 Hz `frontmostApplication()`;
     `main_qml.py:1004` NSImage-backed icons rebuilt on every Add-Profile open.
- **Deskflow: fix in place.** First targets:
  1. `src/lib/coordination/Coordinator.cpp:263-274, 874-893` — two blocking
     connects per peer every 3 s on the worker thread (700 ms each when peers
     sleep); `:927-957` blocking query to every peer every 15 s.
  2. `src/apps/deskflow-core/AutoModeRunner.cpp:125,172-183` — full
     ServerApp/ClientApp + screen + event-tap rebuild on every epoch flip.
  3. `src/lib/net/TCPSocket.cpp:157` unbounded output buffer;
     `src/lib/io/StreamBuffer.cpp:29-38` O(n) consolidation on `peek`.
  4. Clipboard held twice on server (`Server.h:429,485`) and once per client
     proxy; `Server.cpp:764` marshals every clipboard on every screen switch.
  5. `OSXScreen.mm:680,836` 1 s pasteboard + AX polling;
     `OSXKeyboardRelayMonitor.mm:288` / `OSXLocalInputMonitor.mm:91` unretained
     CFRunLoops (same class as the fixed 843d0743b crash).
  6. Build: no LTO/IPO, Unix relies on CMake's default `-O3`; add
     `CMAKE_INTERPROCEDURAL_OPTIMIZATION` and measure, don't assume.
- **Harness before edits**: per-OS sampler (RSS, `lsmp` Mach-port count,
  `heap`/`leaks` diff on macOS; Working Set + handle count on Windows) plus a
  reproducible "mouse parked on the other host for N hours" scenario. Every
  hotspot fix must show its delta on the harness.
- **Reject list** (agents must not spend time here): rewriting Mouser in
  Swift/C++; Qt module trimming beyond what `Mouser-mac.spec` already excludes;
  push-to-deploy; notarization (explicitly not done, fine for a private fleet).

## Open Questions

- Which hackintosh-only condition explains the 20× RSS gap (699 MB vs ~30 MB
  deskflow-core; 549 MB vs 64 MB Mouser)? Leading hypothesis: it is the
  Deskflow server/coordinator most of the time *and* the seat where the mouse
  is usually absent, so both hot loops run there. Harness must confirm.
- Mouser's remote is named `fork`, not `origin`; `fleet-deploy-macos.sh:89` does
  a bare `git pull --ff-only` (relies on upstream tracking) while the Deskflow
  path pulls `origin` explicitly. Standardize.
- Python skew across seats (3.13.2 / 3.14.7 / 3.12.10) with PyInstaller only in
  each `.venv`. Pin a version per seat or accept and test all three.
- Windows Deskflow build number (1.26.0.462) differs from macOS (1.26.0.9999):
  version stamping source differs per platform; the health gate needs one
  truth.
- `com.cursor.worker.*` LaunchAgents crash-loop every 30 s on the hackintosh
  (worker.lock conflict) — unrelated to memory but adds noise to any
  measurement; disable during harness runs.
- vitest fan-out in `sea_trials_universal/scripts/error_report_adjudication/*`
  (13 × 3.5–4 GB) is out of scope here but should get `maxWorkers` capped
  before the harness runs on the hackintosh, or it will contaminate results.

## Evidence Appendix (for `/plan`)

Fleet inventory 2026-09-16 (all reachable; all six checkouts clean and on
identical commits: deskflow `b05f46928`, Mouser `ef43412`):

| Seat | fleet.env | deskflow/.env | Mouser env | Sign identity | Keychain over SSH |
|---|---|---|---|---|---|
| macbookpro | missing | missing | `.env.local` → `MOUSER_SIGN_IDENTITY` | Apple Development (2 copies) | unlocked |
| hackintosh | **present** (only seat) | missing | `.env.local` → `MOUSER_TEAM_ID` | Apple Development | **locked (exit 36)** |
| tiny11 | missing | **present** (`DESKFLOW_CODESIGN_ID`, `DESKFLOW_SIGN_THUMBPRINT`, `DESKFLOW_QT_PATH`, ...) | missing | "Deskflow Fleet Code Signing" (LocalMachine) | n/a |

Deploy flow today: `scripts/fleet-deploy.sh` (run on hackintosh) → per host:
`local` → `bash scripts/fleet-deploy-macos.sh`; macOS SSH → inline
`git fetch/checkout/pull --ff-only` then `fleet-deploy-macos.sh` with
`FLEET_KEYCHAIN_PASSWORD` exported **in the SSH command string**; Windows SSH →
`powershell fleet-deploy-windows.ps1` → `build-windows.ps1 -Install` +
`Mouser/scripts/build_and_install.py`. Mouser macOS signing:
`MOUSER_SIGN_IDENTITY` or `MOUSER_TEAM_ID` lookup (`build_and_install.py:180-214`),
hard-fail if absent; ad-hoc signing was the historical cause of TCC re-prompts
(commit a3223da). Deskflow macOS signing: `APPLE_CODESIGN_DEV` →
`cmake/MacCodesign.cmake` + `macdeployqt -codesign` + final `--deep` re-seal.
Windows: `build-windows.ps1` (signtool use to be verified in plan; bridge and
Mouser currently unsigned).

Full hotspot lists with rationale are in the research transcripts from this
session; the top items are reproduced above.
