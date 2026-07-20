# fix: guaranteed modifier release + Caps Lock toggle sync across the fleet

**Type:** bug · **Branch:** `refactor/fleet-state-hub` · **Date:** 2026-07-20
**Structure:** two independently-shippable PRs (scope review, 2026-07-20)

## Problem

Three user-visible failures on the Windows target (tiny11), all input-state desync:

1. **Stuck modifiers after chord remaps** — Super+Tab (chord hold-through holds Alt on the
   Windows client) sometimes never releases; a modifier stays held "forever" until something
   external resets it.
2. **Random shortcuts while typing** — right after switching to Windows, letters fire
   Win/Alt shortcuts (windows opening/minimizing). This is symptom 1's payoff: a stale
   *injected* modifier is physically held while the user types real keys.
3. **Caps Lock unreliable** — especially on login screens (Windows LogonUI; macOS
   login-window bridge). Prior fix `826e20011` baked Shift/Caps *case* into relayed KeyIDs
   but did not sync caps **toggle state**.

## Evidence (2026-07-20 investigation)

- `tiny11 C:\ProgramData\Deskflow\deskflow-daemon.log`: **7+ `disconnected from server`
  events in one morning** (09:04–11:12) plus
  `coordination: keyboard relay not running in client epoch; restarting`. Every drop that
  lands mid-chord orphans the held out-mods.
- The Macs persist **no core log file** (GUI dock only) — server-side chord evidence is
  invisible. (Observability item included below.)
- Crash reports `ExcUserFault_deskflow-core-2026-07-13-*` on macbookpro (out of scope;
  noted for the soak).

## Root causes (file:line, HEAD `fa79bc881`)

| # | Cause | Where |
|---|-------|-------|
| R1 | Chord release is sent **to a client that already disconnected** — the clear goes into a dead socket | `Server::forceLeaveClient` → `cancelChordRemapSession` (`src/lib/server/Server.cpp:155-172, 2520-2525`) |
| R2 | Reconnect-adoption (`adoptClient`, commit `67935bc42`) drops the stale proxy with **no leave/clear**, and the still-active session matches the *new* proxy by screen name | `Server.cpp:379-441, 544-550` |
| R3 | Client only fully releases keys via `Screen::leave()` → `fakeAllKeysUp`, which **never runs** on TCP drops, relay restarts, epoch/role flips, or core relaunches (LogonUI desktop switch) | `src/lib/deskflow/Screen.cpp:456`, `MSWindowsScreen::enter` (no sanitize, `:235-263`) |
| R4 | `KeyState::fakeAllKeysUp` releases only ledger-tracked keys (`m_syntheticKeys`); desk switches / `updateKeys` can zero the ledger while the injected key is still physically down | `src/lib/deskflow/KeyState.cpp:942-956`, `MSWindowsDesks:925-956` |
| R5 | Five-Esc rescue clears the session only when the rescued core still has it `active` on `m_active`, then restarts; a server-side restart drops all clients with held keys | `Server.cpp:1962-1967` |
| R6 | Caps relay: toggle **state** is sampled independently on relay host and server (`GetKeyState(VK_CAPITAL)&1` twice); caps key-ups relay as `kKeyNone`; no authoritative toggle push on enter/reconnect; LogonUI samples from a different input desktop | `src/lib/coordination/KeyboardRelayMap.cpp:32-34,148-150`, `MSWindowsKeyState:804` |

## Design principle

**Belt and suspenders.** The server can never *guarantee* delivery of a release over a
dying TCP link, so correctness must not depend on it:

- **Belt (server):** always *attempt* orderly release at every session exit path.
- **Suspenders (Windows client):** self-sanitize from **physical/async key state** at every
  boundary where stale injected input can exist. This layer makes stuck keys structurally
  impossible. PR 1's fixes are co-dependent — land the client half even if the server half
  slips.

Simplicity review (2026-07-20): the existing single-slot `m_chordRemapSession` is
sufficient — only one screen is ever active in this single-user topology. **No per-client
ledger/map**; fix the four leak points on the existing struct.

---

## PR 1 — Stuck-modifier guarantee (server exits + Windows self-sanitize)

### Phase 1 — Server: clear the single session on every exit path

Files: `src/lib/server/Server.{h,cpp}`, `src/unittests/server/ServerTests.{h,cpp}`

- [ ] `adoptClient` (reconnect adoption): if the active session's screen name matches the
      adopted client, send `kKeyClearModifiers(heldOutMods)` to the **new** proxy, then
      clear the session — the fresh connection is exactly when a clear is deliverable again
      (fixes R2).
- [ ] `forceLeaveClient`/disconnect: still attempt the clear (harmless if dead), always
      clear the session state; never treat send as success (R1).
- [ ] Five-Esc rescue: clear any active session (send to its screen's live proxy if
      connected, not only `m_active`) **before** `requestLocalCoreRestart` (R5).
- [ ] Server teardown / role flip (epoch end): destructor/stop path sends the clear to the
      session's client if still connected (R3, server side).
- [ ] Tests (`ServerTests`): adoption-clears-mods; disconnect-clears-state;
      rescue-clears-before-restart; teardown-clears; second-chord-attempt-while-active
      leaves session consistent (single-slot semantics regression guard).

### Phase 2 — Windows client: physical-state self-sanitize (the real guarantee)

Files: `src/lib/platform/MSWindowsScreen.{h,cpp}`, `MSWindowsKeyState.{h,cpp}`,
trigger call sites (`MSWindowsDesks.cpp`, `src/lib/client/Client.cpp`, relay restart)

- [ ] New `MSWindowsScreen::sanitizeStaleModifiers()`: for VK_LWIN/RWIN, L/R ALT, L/R CTRL,
      L/R SHIFT — if `GetAsyncKeyState` reports down **and** our hook's live view says the
      key is not physically held, inject the key-up via `SendInput`. Releases by
      **physical state, not the synthetic ledger** (R4). Log
      `WARNING: released stuck modifier <vk>` per release (auditable in daemon log).
- [ ] Triggers (5, each mapped to R3): client connect, `Screen::enter`, core start (covers
      the LogonUI relaunch — the *incoming* core cleans up the outgoing core's stragglers),
      keyboard-relay restart, desktop switch (LogonUI/secure-desktop attach).
      *(Wake-from-sleep deliberately excluded — no evidence; add only if the soak
      reproduces it.)*
- [ ] Secure desktop: on `OpenInputDesktop` failure (UAC consent up), **log and skip** —
      the desktop-switch trigger re-runs sanitize when the desktop returns (same pattern as
      `MSWindowsScreen.cpp:488`). No stateful deferral mechanism.
- [ ] PowerToys KBM compatibility: inject plain VK key-ups (PT sees ordinary input);
      verified manually with PT active.
- [ ] **Known gap (accepted):** `sanitizeStaleModifiers` is manual-verify only — Windows
      `GetAsyncKeyState`/`SendInput` seams aren't unit-testable without an injection shim.
      The WARNING log line is the observability that compensates.

**PR 1 done-gate:** unit tests green **plus** the one-day soak (below). "Merged" ≠
"verified" — the soak is the real gate.

---

## PR 2 — Caps Lock toggle-state sync (independent; can land before/after/parallel)

### Phase 3 — Caps: sync the toggle state, not just the case

Files: `src/lib/coordination/KeyboardRelayMap.{cpp,mm}`, relay monitor call sites,
`src/lib/deskflow/KeyState.cpp`, `MSWindowsKeyState.cpp`,
`src/unittests/coordination/KeyboardRelayMapTests.*`

- [ ] Authoritative caps push: on client `enter`/reconnect the server's toggle mask must
      actually apply on Windows **including the elevated LogonUI core** — trace the
      existing `enter` mask path and fix where it's dropped (R6).
- [ ] Relay: forward caps **key-ups** (stop mapping them to `kKeyNone`) so half-duplex
      targets toggle correctly; single-source the caps state used for case-baking (relay
      host's sample wins; server must not re-sample).
- [ ] LogonUI: elevated core applies the server-pushed toggle state at start; don't trust
      `GetKeyState` sampled from the pre-switch desktop.
- [ ] macOS login-window bridge: **verify-only** — if caps is broken through the Karabiner
      vhid bridge, that's a separate subsystem → split to a follow-up PR, don't absorb it
      here (scope-review flag).
- [ ] Tests (`KeyboardRelayMapTests`): caps down+up relayed; case-baking with caps on/off;
      caps flipped mid-disconnect → resync on enter.

---

## Shared verification (attach to whichever PR lands last)

- [ ] Macs: persistent core file logging via existing `log/toFile` settings — **confirm the
      setting is actually wired on macOS first** (scope-review flag: may be small real code,
      not pure config).
- [ ] Regression matrix (manual): autostart on all three · Windows LogonUI control incl.
      Caps · macOS login-window bridge · UAC secure desktop · PowerToys hotkeys still fire ·
      five-Esc rescue leaves no held keys on any client.

## Success criteria

```success-criteria
- id: server-session-exit-tests
  desc: Single-slot chord session clears on adoption, disconnect, rescue, teardown, and second-chord attempt
  verify: ninja -C build ServerTests && ./build/src/unittests/server/ServerTests
- id: caps-relay-tests
  desc: Caps down+up relay, case-baking, and mid-disconnect resync covered by tests
  verify: ninja -C build KeyboardRelayMapTests && ./build/src/unittests/coordination/KeyboardRelayMapTests
- id: all-platforms-compile
  desc: macOS (hackintosh x86_64, macbookpro arm64) and Windows (tiny11 MSVC) build green
  verify: ninja -C build Deskflow deskflow-core  # per machine; tiny11 via build_deskflow.bat (vcvars WITHOUT >nul)
- id: no-stuck-modifier-soak
  desc: One-day soak with zero user-felt stuck modifiers; every self-sanitize release is logged
  verify: manual 1) use fleet normally for a day incl. Super+Tab chords and switches 2) grep tiny11 daemon log for "released stuck modifier" — occurrences are the suspenders catching drops (expected, fine) 3) zero user-felt stuck-key incidents
- id: caps-login-screens
  desc: Caps Lock toggles correctly at Windows LogonUI and macOS login window
  verify: manual 1) lock tiny11, drive from Mac, toggle Caps, password casing correct 2) same at macOS login window via vhid bridge 3) flip Caps, disconnect/reconnect mid-toggle, state resyncs
- id: regressions-hold
  desc: Autostart, login-screen flows, UAC, PowerToys, five-Esc all still work
  verify: manual 1) reboot each machine — Deskflow self-starts 2) drive Windows over a UAC prompt 3) PowerToys hotkeys fire while Deskflow runs 4) five-Esc restarts keyboard-host core and no client keeps held keys
```

## Risks / notes

- `SendInput` modifier key-ups are visible to other hooks (PowerToys, Mouser) — sanitize
  fires only when async state says down and the hook says not-physical; the WARNING log
  makes every firing auditable.
- Elevated LogonUI core injects on a separate desktop; the incoming-core sanitize covers
  the outgoing core's stragglers — verify explicitly in the matrix.
- Out of scope: macbookpro `ExcUserFault` crash reports (separate investigation if the soak
  reproduces them); Mouser gesture pipeline; vhid-bridge caps fix if verification shows it
  broken (follow-up PR).
