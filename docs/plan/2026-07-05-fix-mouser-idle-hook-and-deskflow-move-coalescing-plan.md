---
title: "fix: keep Mouser's LL hook out of the input path when idle + coalesce Deskflow injected moves"
type: fix
date: 2026-07-05
vgv_next:
  skill: build
  artifact: docs/plan/2026-07-05-fix-mouser-idle-hook-and-deskflow-move-coalescing-plan.md
---

## Mouser Idle-Hook Removal + Deskflow Move Coalescing - Standard

## Overview

Injected cursor movement on tiny11 freezes completely while Mouser runs and
recovers the instant Mouser quits. Root cause (see brainstorm
`docs/brainstorm/2026-07-05-mouser-ll-hook-blocks-injected-cursor-brainstorm-doc.md`):
every injected move must clear the `WH_MOUSE_LL` chain, Mouser's hook is a
Python/ctypes callback, and Deskflow's recent injection overhaul
(fire-and-forget + `MOUSEEVENTF_MOVE_NOCOALESCE`) delivers an uncoalesced
move firehose the callback cannot service — the system input queue backs up
behind the hook. Fix both sides:

- **A (Mouser)**: install the `WH_MOUSE_LL` hook (and the generic-mouse Raw
  Input registration) only while Mouser actually has something to
  intercept. On KVM clients with no bound local Logitech, Python leaves
  the input path entirely.
- **B (Deskflow)**: in the desk thread, supersede consecutively queued
  `FAKE_MOVE` messages and inject only the newest position, so any slow
  third-party hook degrades to fewer intermediate positions instead of an
  unbounded backlog.

## Problem Statement / Motivation

- tiny11 is unusable with Mouser running: cursor does not move.
- The same class of stall can be triggered by any slow LL hook (PowerToys,
  AutoHotkey, overlays) as long as Deskflow injects uncoalesced moves at
  full rate — Deskflow needs its own backpressure valve.
- At the Mouser known-good commit (`05bea85`) Deskflow still throttled and
  coalesced moves, which is why this never showed before the injection
  perf work.

## Proposed Solution

### Workstream A — Mouser (`/Users/alexhughes/Desktop/Mouser`)

**A1. One predicate: `_hook_should_be_installed()`** — `core/mouse_hook_windows.py`

```python
def _hook_should_be_installed(self) -> bool:
    # Blocking/remap work requires the LL hook only when a local Logitech
    # is bound and focus is local (_should_intercept_events), or when the
    # host-local scroll-invert fallback is enabled (the one intercept
    # that runs while KVM focus is remote; see 58932b1).
    return self._should_intercept_events() or self._scroll_invert_fallback_enabled()
```

`_scroll_invert_fallback_enabled()` reflects the user's invert toggles
(`invert_vscroll` / `invert_hscroll`) *and* firmware-invert state — reuse
the gating already inside `_apply_vscroll_invert_fallback` /
`_apply_hscroll_invert_fallback` minus the per-event parts.

**A2. Hook + Raw Input lifecycle follows the predicate** — `core/mouse_hook_windows.py`

- The hook thread keeps its message loop and `_ri_hwnd` window alive
  permanently (device-change and injection messages still need it), but:
  - `WH_MOUSE_LL` is installed/uninstalled on demand;
  - the generic-mouse Raw Input registration (usage page 0x01 / usage
    0x02, `RIDEV_INPUTSINK`) is registered/unregistered (`RIDEV_REMOVE`)
    together with the hook — it delivers one Python `WM_INPUT` per system
    mouse move and is only needed for sense-panel fallback gestures and
    wheel attribution, both of which are moot when the hook is absent.
    The Logitech vendor-page registrations (0xFF43, consumer 0x0C) stay —
    they only fire for Logitech HID reports.
- All transitions execute on the hook thread via a posted
  `WM_APP_SYNC_HOOK` message to `_ri_hwnd` (never call
  `SetWindowsHookExW`/`UnhookWindowsHookEx` cross-thread).
- `sync_hook_state()` (thread-safe, posts the message) is invoked from
  every event that can change the predicate:
  - `_on_hid_connect` / `_on_hid_disconnect` (device bound/unbound)
  - `RemoteForwarder` focus transitions (`set_remote_forwarder` and the
    forwarder's focus callback)
  - settings changes to invert toggles / blocked events (engine applies)
  - `_on_device_change` — **replaces** the current unconditional
    `_reinstall_hook()`: re-evaluate the predicate; reinstall only when
    the hook should exist (this also kills the probe-driven rehook storm).
- `stop()` unchanged: uninstalls whatever is installed.
- **Startup ordering:** `sync_hook_state()` calls arriving before
  `_ri_hwnd` exists must not be lost — `_run_hook` evaluates the
  predicate once itself right after `_setup_raw_input()` completes, so
  the initial state is always correct without racing the window creation.
- **Never uninstall mid-hold:** if a remapped/blocked button or gesture
  capture is currently active, defer the uninstall until the capture/hold
  ends (the up event must pass through the same hook that swallowed the
  down, or the OS is left with a stuck button). On deferred uninstall,
  force-release any held gesture state first (`_force_release`-style path
  already exists for stale holds).
- **`RIDEV_REMOVE` semantics:** unregistering requires
  `dwFlags = RIDEV_REMOVE` with `hwndTarget = NULL` for the same
  usage page/usage; re-registration must restore the same tier that
  `_setup_raw_input`'s 4/2/1 fallback originally achieved (remember the
  tier, don't re-run the fallback blindly).

**A3. Tests** — `tests/test_mouse_hook.py` (or new `tests/test_hook_lifecycle.py`)

- Predicate truth table: no device bound → False; bound + focus local →
  True; bound + forwarder remote → False unless invert fallback enabled;
  invert enabled with firmware-invert active → False for that axis.
- Lifecycle: with mocked Win32, `sync_hook_state()` installs when the
  predicate flips True and uninstalls when False; `_on_device_change`
  does not reinstall while the predicate is False; transitions are
  executed via the posted message path (assert `PostMessageW` target).
- Edge cases: sync posted before `_ri_hwnd` exists is not lost; uninstall
  requested mid-gesture-hold is deferred and held state force-released;
  re-registration restores the recorded Raw Input tier; rapid
  focus-flap (remote/local toggles) converges to the final state.
- Regression: existing hook/gesture/scroll suites stay green.

### Workstream B — Deskflow (`src/lib/platform/MSWindowsDesks.cpp`)

**B1. Supersede consecutive queued moves in `deskThread`**

In the `DESKFLOW_MSG_FAKE_MOVE` case (line ~776): after receiving a move,
peek the head of the thread queue (`PeekMessage(..., PM_NOREMOVE)`); while
the *next* message is also `DESKFLOW_MSG_FAKE_MOVE`, remove it and adopt
its coordinates. Inject only the final position. Head-of-queue peeking
(not message-filter draining) preserves ordering with interleaved button /
key / wheel messages exactly.

`DESKFLOW_MSG_FAKE_REL_MOVE` (line ~781): same loop but **sum** the deltas
of consecutive relative moves (relative motion must not be dropped — it is
the in-game aim path).

Cap the supersession loop (e.g. 64 messages per injection): under a
sustained flood the head of the queue can stay a move indefinitely, and an
uncapped drain would delay the actual injection instead of bounding the
backlog.

`MOUSEEVENTF_MOVE_NOCOALESCE` stays on the injected event: we coalesce in
our own queue under backlog, while what we do inject still lands
uncoalesced and smooth.

**B2. Pure helper for testability** — extract the "adopt newer absolute
position / accumulate relative delta" decision into a small header-inline
helper (e.g. `coalesceQueuedMove(...)` in `MSWindowsDesks.h` or a tiny
`InjectionCoalesce.h`) so the arithmetic (delta summing, overflow clamp)
gets unit coverage even though the Win32 loop itself is Windows-only.

**B3. Verification on tiny11**

- Desktop: with Mouser running (post-A build), cursor is responsive; with
  an artificially slowed hook (Mouser debug), cursor degrades gracefully
  instead of freezing.
- In-game (Call of Duty): relative-move summing preserves aim feel; no
  perceptible decimation vs current build.

### Rollout

1. Land + test Workstream A in Mouser; rebuild/install on tiny11 (and
   macbookpro Mouser at leisure — macOS CGEventTap path is untouched).
2. Land + test Workstream B in Deskflow; build/install tiny11; Macs
   unaffected (Windows-only file) but keep fleet versions in sync.

## Technical Considerations

- **Thread affinity:** `SetWindowsHookExW` callbacks are serviced by the
  installing thread's message loop; all lifecycle transitions must run on
  the hook thread (posted message), matching how the loop already handles
  `WM_APP_INJECT_*`.
- **Ordering correctness (B):** only consecutive same-type messages are
  superseded via head-of-queue peeks — a MOVE, BUTTON, MOVE sequence
  injects MOVE before BUTTON as today.
- **Anti-cheat:** no new injection APIs; strictly fewer `SendInput` calls.
  Mouser-side change reduces hooks present during gameplay — if anything,
  friendlier to Ricochet.
- **Scroll-invert-while-remote:** feature keeps working, but it is now the
  only reason a hook exists while focus is remote; if the user has invert
  off (tiny11 today), no hook at all.
- **macOS parity:** CGEventTap has the same theoretical exposure but no
  observed problem (no GIL-bound tap; different delivery); explicitly out
  of scope (YAGNI).

## Acceptance Criteria

Workstream A (Mouser):

- [ ] Predicate truth-table unit tests pass
- [ ] `sync_hook_state()` installs/uninstalls only via the hook-thread message path (mocked Win32 test)
- [ ] `_on_device_change` no longer reinstalls the hook when the predicate is False
- [ ] Generic-mouse Raw Input registration is removed whenever the hook is uninstalled
- [ ] Full Mouser suite green
- [ ] Live tiny11: Mouser running, no bound Logitech → `Get-Process` shows Mouser, **no** `WH_MOUSE_LL` present (verify via cursor responsiveness + absence of "Hook installed" log), cursor perfectly responsive

Workstream B (Deskflow):

- [ ] Coalescing helper unit tests: newest absolute position wins; relative deltas sum; non-move messages never skipped
- [ ] All existing Deskflow suites green (macOS + the Windows build compiles)
- [ ] Live tiny11: with an artificially slow hook installed, cursor stays usable (degrades, never freezes)
- [ ] In-game aim feel unchanged (user verification)

## Success Metrics

- Cursor on tiny11 fully responsive with Mouser running (the reported
  blocker is gone).
- Zero "Hook reinstalled" log lines during steady-state operation on
  tiny11.
- No cursor-freeze reports with any third-party hook software running.

## Dependencies & Risks

- **Lifecycle races (A):** capture start racing hook uninstall — the
  predicate flips before capture begins, and transitions are serialized on
  the hook thread; tests must cover flip-during-capture.
- **Invert-fallback regression (A):** users with scroll invert enabled
  keep the hook; verify invert still works after a remote→local focus
  flip reinstalls it.
- **Reordering bugs (B):** only head-of-queue consecutive supersession is
  allowed; a filtered drain would reorder clicks vs moves.
- **The freeze must be re-verified after A alone** — if the cursor still
  stalls with the hook absent, the diagnosis is wrong and B alone must be
  validated before shipping further Mouser changes.

## References & Research

- Brainstorm: `docs/brainstorm/2026-07-05-mouser-ll-hook-blocks-injected-cursor-brainstorm-doc.md`
- Deskflow desk-thread message loop: `src/lib/platform/MSWindowsDesks.cpp:709-835` (`FAKE_MOVE` at 776, `FAKE_REL_MOVE` at 781)
- NOCOALESCE injection: `src/lib/platform/MSWindowsDesks.cpp:481-486`
- Mouser intercept gate: `Mouser/core/mouse_hook_base.py:163-194` (`_should_intercept_events`)
- Mouser hook thread + RI setup: `Mouser/core/mouse_hook_windows.py:599-717` (`_setup_raw_input`, `_run_hook`, `_on_device_change`)
- Scroll-invert-while-remote: Mouser commit `58932b1`; wheel attribution: `108d251`
- Move passthrough (insufficient alone): Mouser commit `c1f2318`
- Known-good baseline: Mouser `05bea85` (worked because Deskflow then throttled + coalesced injected moves)
