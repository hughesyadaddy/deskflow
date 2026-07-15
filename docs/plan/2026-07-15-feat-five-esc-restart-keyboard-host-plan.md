---
title: "feat: five Esc taps restart Deskflow on keyboard host"
type: feat
date: 2026-07-15
vgv_next:
  skill: build
  artifact: docs/plan/2026-07-15-feat-five-esc-restart-keyboard-host-plan.md
---

# feat: five Esc taps restart Deskflow on keyboard host

## Overview

Replace the **Ctrl+Alt+Shift+Escape** rescue chord (yank keyboard / jump cursor to primary) with **five plain Escape presses within 2 seconds** on the machine that has the physical keyboard. The fifth tap soft-restarts local Deskflow core the same way as tray **Restart** (`CoreProcess::stop` → `start`, including daemon IPC in service mode).

## Problem Statement / Motivation

The old rescue chord recovered a stuck cursor/relay without bouncing the process. In practice the useful escape hatch on this fleet is **reset Deskflow on the keyboard host** (the machine whose keyboard is plugged in), not “force keys local until cursor moves.” A simple **5× Esc** gesture is easier to remember and targets restart instead of override/jump semantics.

## Decisions (locked)

| Topic | Decision |
|-------|----------|
| Old chord | **Removed** — no `relayLocalOverride` / `jumpToScreen` rescue |
| Gesture | **5** plain Esc key-downs within **2.0 s** (rolling window from last tap) |
| Which machine | **Keyboard host only** (intake sides: client relay + server `onKeyDown`) |
| Restart | **Soft** via GUI `CoreProcess::restart()` (service mode: daemon stop/start) |
| Count rule | Distinct **KeyDown** only — **not** OS auto-repeat (`KeyRepeat` / Repeat phase ignored) |
| Plain Esc | Chord mods Shift/Ctrl/Alt/Super must be **0**; Caps/NumLock ignored |
| Taps 1–4 | Esc **still delivered** to the current destination |
| Tap 5 | **Swallow** Esc, cancel any chord-remap hold-through session, request restart |
| Feedback | `LOG_INFO` (+ tray GUI log if wired); no new UI toast required |
| Config | Hard-coded N=5, window=2s (no settings UI in this change) |

## Proposed Solution

### Tap detector (shared, header-only)

Replace `isKeyboardRescueChord` entirely in `src/lib/coordination/KeyboardRescue.h` (no unused wrapper; add `.cpp` only if ODR forces it):

```cpp
struct EscTapRescue {
  static constexpr int kTaps = 5;
  static constexpr double kWindowSec = 2.0;

  using Clock = std::chrono::steady_clock; // injectable for tests

  // Call only for KeyDown / KeyPhase::Down (not Repeat).
  // Plain Esc: chord mods Shift|Ctrl|Alt|Super must be 0; Caps/Num ignored.
  // Rolling window from last counted tap; returns true on the 5th tap.
  bool noteEscDown(KeyID id, KeyModifierMask mask, Clock::time_point now = Clock::now());
  void reset();
};
```

Call sites gate on Down themselves so the detector stays free of a redundant `isRepeat` flag.

One `EscTapRescue` instance on `Coordinator` and one on `Server` (epochs are mutually exclusive for keyboard intake).

**Client local-pass gap:** when routing is already local, `sendKeyForward` is not called — Esc counting for client epoch only runs when keys would be forwarded. For rescue-while-stuck (keys already swallowed/forged local), also note Esc Downs in the keyboard-relay path *before* the local-pass short-circuit (e.g. monitor/`sendKeyForward` entry that still sees Down events, or `relayPassThroughLocal` side). Implementation must ensure wedged “forward would have run / keys are captured” still counts; document the chosen hook in the PR.

### Intake wiring

| Path | Where | Action on trigger |
|------|--------|-------------------|
| Client epoch keyboard relay | Down path that always sees physical Esc | Swallow this Esc (no forward); `requestLocalCoreRestart()` |
| Server epoch / local GUI keys | `Server::onKeyDown` | Cancel chord-remap session; do not relay Esc; `requestLocalCoreRestart()`; return |

Remove completely:

- `isKeyboardRescueChord`
- `Coordinator` `m_relayLocalOverride` / `m_overrideCursorHost` and cursor-host clear tied only to rescue
- `Server::onKeyDown` `jumpToScreen(m_primaryClient)` rescue branch

### Soft restart (core → GUI) — mutually exclusive paths

There is today **no** `restartCore` IPC from core → GUI. Contract:

| Core IPC clients | Action |
|------------------|--------|
| **≥1** | Only `ipcSendToClient("restartCore")`. Do **not** also quit / `stopProcessRequested`. GUI `CoreProcess::onCoreIpcMessageReceived` → `restart()`. |
| **0** | Do **not** enqueue `restartCore` (`broadcastCommand` queues when empty — that would fire a stale restart later). Only fallback: `stopProcessRequested` / quit AutoModeRunner so Windows daemon can respawn; on Mac desktop log that tray is required. |

Never both. No delayed “quit after broadcast” race with GUI `stop()`/`start()`.

Test seams: inject restart callback on Coordinator/Server (or free-function pointer) so the fifth tap is unit-assertable without a live GUI; inject clock for the 2s window without sleeps.

### Chord remap interaction

Call Esc-tap note **before** hold-through / fire-and-forget remap. On fire, `cancelChordRemapSession()` (server path).

## Technical Considerations

### Files to touch

| Area | Files |
|------|-------|
| Detector | `src/lib/coordination/KeyboardRescue.h` (+ `.cpp` if needed) |
| Client intake | `src/lib/coordination/Coordinator.{h,cpp}` |
| Server intake | `src/lib/server/Server.{h,cpp}` |
| Core→GUI IPC | `src/lib/deskflow/ipc/CoreIpc.h` (helper OK), `src/lib/gui/core/CoreProcess.cpp` |
| Tests | `KeyboardRouterTests`, `CoordinatorFleetPublishTests`, `ServerTests` (replace old rescue cases) |
| Docs (light) | Any user-facing rescue mention in `docs/` / INSTALL if present |

### Risks

- **False positive**: Escaping stacked dialogs / Vim can bounce core — accepted; 2s + Down-only + no-modifiers mitigates holds.
- **Semantic change**: Cursor may stay on remote until mesh/epoch recovers after restart (old chord yanked home without disconnect).
- **GUI absent**: Fallback may leave Mac without auto-respawn — document; fleet always has tray.

## Success Criteria

```yaml
success-criteria:
  - name: five_esc_triggers_restart_request
    verify: ctest --test-dir build -R 'KeyboardRouterTests|ServerTests|CoordinatorFleetPublishTests' --output-on-failure
  - name: old_chord_removed
    verify: "! rg -n 'isKeyboardRescueChord|relayLocalOverride' src/lib/coordination src/lib/server src/unittests"
  - name: manual_fleet_esc_restart
    verify: |
      manual 1) Cursor on tiny11 from hackintosh keyboard
      2) Tap Esc five times within 2s
      3) deskflow-core on hackintosh restarts (new PID); taps 1-4 still act as Esc on Windows
      4) Ctrl+Alt+Shift+Esc no longer jumps cursor home
```

## Implementation Checklist

- [x] Header-only `EscTapRescue` (injectable clock); delete `isKeyboardRescueChord`
- [x] Wire Down intake on Coordinator (including local-pass / always-seen Esc path) and `Server::onKeyDown`
- [x] Remove `relayLocalOverride` rescue machinery + server jump rescue
- [x] Mutually exclusive restart: GUI `restartCore` **or** no-client fallback quit — never both / never queue when empty
- [x] GUI: `restartCore` → `CoreProcess::restart()`
- [x] Unit tests: tap math (clock inject); fifth tap → restart callback; Server swallow + no jump + cancel hold-through; delete old chord tests
- [x] Grep stale Ctrl+Alt+Shift+Esc docs; update if trivial (only historical plans/brainstorms still mention old chord)
- [ ] Fleet kill-all deploy; manual success criterion 3

## Scope

**No split** — one vertical (~300–450 LOC): detector + intake + chord removal + IPC must ship together.

## Out of scope

- Configurable N / window in Settings UI
- Remote restart of a peer’s core
- Restoring yank-to-primary without restart
- Hard kill-all / service recreate path
- New tray toast / discoverability UI

## References

- Current chord: `src/lib/coordination/KeyboardRescue.h`
- Client override: `Coordinator::sendKeyForward` (~592)
- Server jump: `Server::onKeyDown` (~1953)
- Soft restart: `CoreProcess::restart()` (`src/lib/gui/core/CoreProcess.cpp`)
- Core→GUI broadcast: `ipcSendToClient` (`src/lib/deskflow/ipc/CoreIpc.cpp`)
- Review notes applied from `docs/reviews/raw/{code-simplicity-review,vgv-review,plan-splitting}.md`
