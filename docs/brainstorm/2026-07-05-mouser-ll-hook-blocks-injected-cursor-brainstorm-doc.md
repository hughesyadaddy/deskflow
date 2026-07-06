---
date: 2026-07-05
topic: mouser-ll-hook-blocks-injected-cursor
vgv_next:
  skill: plan
  artifact: docs/brainstorm/2026-07-05-mouser-ll-hook-blocks-injected-cursor-brainstorm-doc.md
---

# Mouser LL Hook Blocks the Injected Cursor on tiny11

## What We're Building

Two coordinated fixes so Mouser can never stall the Deskflow-injected
cursor on Windows clients again:

1. **Mouser: hook-off-when-idle.** Install the `WH_MOUSE_LL` hook only
   while Mouser actually needs to intercept events (a local Logitech is
   bound and a remap/block/gesture capture or scroll-invert fallback is
   active). On a KVM client like tiny11 — no bound local Logitech, gesture
   events arriving via Deskflow ingress — the hook is not installed at
   all, so Python is out of the input path entirely. Uninstall/reinstall
   follows the existing `_should_intercept_events()` gate, moved from
   "return early in the callback" to "don't be in the hook chain".

2. **Deskflow: coalesce queued injected moves.** In `MSWindowsDesks`'
   desk thread, when multiple `FAKE_MOVE` messages are queued, drain them
   and inject only the newest absolute position. `MOUSEEVENTF_MOVE_NOCOALESCE`
   stays on the injected event; the coalescing happens in our own queue
   before injection, so a slow third-party hook (Mouser, PowerToys,
   overlays) degrades to fewer intermediate positions instead of an
   ever-growing input backlog that freezes the cursor.

## Why This Approach

Root cause analysis (tiny11 logs, 2026-07-05, plus commit archaeology
back to known-good Mouser `05bea85`):

- Every injected move must clear the `WH_MOUSE_LL` chain before entering
  the input stream. Mouser's hook is a Python/ctypes callback — each
  invocation costs a GIL acquisition + marshalling even with the
  `WM_MOUSEMOVE` early return (`c1f2318`).
- Deskflow's injection overhaul (fire-and-forget `sendInputMessage`,
  removing the accidental per-move rate limit; `MOVE_NOCOALESCE`,
  removing the OS's queue-merging safety net) raised delivered move
  rates past what a Python callback can service. Input queues behind the
  hook; the cursor appears fully frozen; quitting Mouser (hook removed)
  recovers instantly.
- Aggravators on the same hook thread since `05bea85`: Raw Input
  `INPUTSINK` delivers a second Python callback (`WM_INPUT`) per move;
  `108d251` added `GetRawInputBuffer` work; the HID++ cold-start probe
  storms ~25 s against five receiver interfaces that can never answer
  (mouse not reachable), churning the GIL/logging lock, and its own
  open/close cycles fire devnode changes (12 rehooks in 30 min).
- At the `05bea85` known-good point, *Deskflow* still throttled and
  coalesced injected moves — the hook's cost was hidden. This is a
  Deskflow×Mouser interaction, so bisecting Mouser alone cannot find it.

Alternatives considered:

- **A only (Mouser fix)**: leaves every other slow LL hook (PowerToys,
  AutoHotkey, anti-cheat overlays) able to freeze the injected cursor.
- **B only (Deskflow fix)**: keeps Python callbacks on the move hot path
  on machines where Mouser has no interception to do — pure waste, and
  still fragile under GIL churn.
- **C (park the idle HID probe)**: real aggravator (GIL churn, rehook
  storms) but not the freezing mechanism; deferred as follow-up.

## Key Decisions

- **Fix both sides (A + B)**: A removes Python from the input path when
  idle; B makes Deskflow robust against any slow hook. Either alone
  leaves a failure mode.
- **Hook lifecycle keys off the existing intercept gate**: same
  conditions as `_should_intercept_events()` plus the scroll-invert
  fallback; transitions happen on the hook thread (post a message) to
  avoid cross-thread SetWindowsHookEx races.
- **Scroll-invert-while-remote (58932b1) keeps the hook only when that
  feature is enabled** — it is the one intercept that runs with KVM
  focus remote.
- **Deskflow coalesces in its own desk queue, not via the OS**: drain
  pending FAKE_MOVE messages, inject newest position; NOCOALESCE remains
  for the events actually injected.
- **Follow-up (not now)**: park the HID++ probe when Deskflow ingress is
  the gesture source and no local Logitech answers (C).

## Open Questions

- Does dynamic hook install/remove interact with the device-change
  rehook path (`_on_device_change` currently reinstalls unconditionally)?
  The plan should unify both under one "should the hook exist" predicate.
- tiny11 has receivers plugged in but the paired mouse lives elsewhere —
  should "bound local Logitech" mean "REPROG_V4 discovered" (device
  actually answering) rather than "receiver enumerated"?
- Verify in-game (Call of Duty) that desk-queue coalescing does not
  reintroduce perceptible cursor decimation; relative-move path should
  coalesce by summing deltas, not dropping them.
