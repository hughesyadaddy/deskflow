---
title: "feat: chord remap hold-through sessions (part 2 of 2)"
type: feat
date: 2026-07-08
part: 2
vgv_next:
  skill: build
  artifact: docs/plan/2026-07-08-feat-chord-remap-settings-hold-through-part-2-plan.md
brainstorm: docs/brainstorm/2026-07-08-chord-remap-settings-and-hold-through-brainstorm-doc.md
parent: docs/plan/2026-07-08-feat-chord-remap-settings-hold-through-plan.md
---

# feat: chord remap hold-through sessions — Part 2 of 2

## Dependencies

**Requires Part 1 merged:** [2026-07-08-feat-chord-remap-settings-hold-through-part-1-plan.md](2026-07-08-feat-chord-remap-settings-hold-through-part-1-plan.md)

Depends on config-loaded chord table, `needsHoldThrough()`, and refactored `Server::onKeyDown`/`onKeyUp`/`onKeyRepeat` from Part 1.

## Overview

Add **hold-through remap sessions** on `Server` so chords like **Super+Tab → Alt+Tab** keep the **output** modifier held until the **physical source** modifier is released — enabling Windows’ Alt+Tab switcher UI and Tab-stepping. Includes teardown on screen jump, rescue chord, and client disconnect.

**Primary user fix:** Mac Cmd+Tab with cursor on `tiny11` shows the Windows switcher UI.

## Problem Statement / Motivation

Part 1 delivers editable, config-driven remaps with fire-and-forget behavior identical to today. The remap engine still rewrites `(KeyID, KeyModifierMask)` **per event**. `KeyMap::keysToRestoreModifiers` synthesizes modifiers per event, so Alt only exists for an instant around each Tab — Windows treats that as quick-switch, not switcher UI.

## Proposed Solution

### Implicit hold-through rule

```cpp
bool needsHoldThrough(const ChordRemapEntry &e) {
  const auto chord = KeyModifierShift | KeyModifierControl | KeyModifierAlt | KeyModifierSuper;
  const auto in = e.inMods & chord;
  const auto out = e.outMods & chord;
  return e.inKey == e.outKey && out != in && out != 0;
}
```

| Seeded row | Hold-through? |
|------------|---------------|
| Super+Tab → Alt+Tab | **Yes** (primary) |
| Ctrl+Alt+F12 → Win+Tab | Implicit (test Task View manually; per-row flag only if wrong) |
| All other seeded rows | No |

### Session state (private to `Server` first)

Start as nested struct + methods on `Server`; extract to `ChordRemapSession.h` only if >~80 lines.

```text
Fields: active, entry, sourceModifierButton, heldOutMods
```

### Modifier injection

Use **active client proxy**, not `InputFilter` event queue:

```cpp
m_active->keyDown(kKeySetModifiers, heldOutMods, 0, lang);
m_active->keyUp(kKeyClearModifiers, heldOutMods, 0);  // via keyDown with clear id
```

(`ClientProxy1_8::keyDown` path — same as normal relay.)

### `onKeyDown` / `onKeyUp` / `onKeyRepeat` ordering

```text
onKeyDown:
  1. Keyboard rescue chord? → cancelChordRemapSession() → jumpToScreen → return
  2. Source modifier keyUp (session)? → endSession() → relay
  3. Session active? → OR heldOutMods into effective mask; relay (no re-match)
  4. Match remap for active screen
     a. needsHoldThrough? → startSession(); inject set modifiers once
     b. else → applyChordRemap in place
  5. Relay to m_active

onKeyRepeat:
  Session active → relay with held mask; no modifier pulse

onKeyUp:
  Mirror remap for fire-and-forget
  Source modifier button-up → endSession()
  Relay
```

### Session lifecycle

```text
Super down (no session)
Super+Tab down → match + needsHoldThrough → startSession
  → keyDown(kKeySetModifiers, Alt, ...)
  → relay Tab with Alt in effective mask
Super+Tab repeat → session continues; Alt stays down
Super up (source button) → endSession → keyDown(kKeyClearModifiers, Alt, ...)
Tab up → normal release
```

Track source modifier release by **`KeyButton`**, not mask bit alone (L-Super vs R-Super).

### Teardown hooks

Call `cancelChordRemapSession()` (sends clear modifiers, resets state) from:

- `jumpToScreen` / `switchScreen` — **before** `m_active = dst` when leaving previous screen that had active session
- Keyboard rescue (before return)
- Active client disconnect during session
- Config reload not supported mid-session — restart clears state (document in Part 1 UI text)

## Technical Considerations

### Files to touch

| Area | Files |
|------|-------|
| Session + integration | `src/lib/server/Server.{h,cpp}` |
| Tests | `src/unittests/server/ChordRemapSessionTests.cpp` (or extend `ChordRemapTests.cpp` if small) |
| CMake | `src/unittests/server/CMakeLists.txt` |

**No GUI changes** in Part 2.

### Risks

| Risk | Mitigation |
|------|------------|
| Stuck Alt/Win | Teardown on screen jump, rescue, disconnect |
| Double remap on repeat | Session branch skips re-apply |
| Fleet relay | `relayForwardedKey` → same `onKeyDown` path |
| Ctrl+Alt+F12→Win+Tab | Manual fleet test; defer per-row flag |

## Implementation Plan

### Phase 1 — Hold-through sessions

- [ ] Add private session state to `Server`.
- [ ] `startSession`, `stepSession`, `endSession`, `cancelChordRemapSession`.
- [ ] Integrate ordering in `onKeyDown` / `onKeyUp` / `onKeyRepeat` per pseudocode above.
- [ ] Teardown: `switchScreen` (leaving active), `jumpToScreen`, rescue, client disconnect.
- [ ] Unit tests: start → Tab repeat → Super up → Alt cleared; screen jump mid-session; rescue mid-session; fire-and-forget during idle session.

### Manual fleet verification (acceptance, not a build phase)

- [ ] hackintosh server + tiny11: local Cmd+Tab → switcher UI visible
- [ ] MacBook keyboard relay + cursor on tiny11 → same
- [ ] Win+Q, Win+X unchanged
- [ ] Edge cross mid-Cmd-hold → no stuck Alt
- [ ] Ctrl+Alt+F12 → Win+Tab (Task View) — note result
- [ ] Deploy: `scripts/fleet-deploy.sh --deskflow-only` (full kill-everything restart)

## Acceptance Criteria

### Hold-through

- [ ] **Super+Tab** (Mac Cmd+Tab) with cursor on `tiny11` shows **Windows Alt+Tab switcher UI**.
- [ ] Holding Super + Tab repeat steps through windows with switcher visible.
- [ ] Releasing Super dismisses switcher; no stuck modifiers.
- [ ] **Fleet relay parity:** MacBook keyboard + cursor on `tiny11` matches local input.

### Regression

- [ ] Fire-and-forget remaps unchanged from Part 1.
- [ ] Keyboard rescue ends session; no stuck Alt on tiny11.
- [ ] Screen change during hold releases output modifiers on target.
- [ ] CapsLock and non-chord bits preserved.

### Tests

- [ ] Session lifecycle + teardown unit tests.
- [ ] Optional: `relayForwardedKey` triggers hold-through (extend server tests).
- [ ] All existing unit tests pass.

## References

- Parent overview: [2026-07-08-feat-chord-remap-settings-hold-through-plan.md](2026-07-08-feat-chord-remap-settings-hold-through-plan.md)
- Part 1: [2026-07-08-feat-chord-remap-settings-hold-through-part-1-plan.md](2026-07-08-feat-chord-remap-settings-hold-through-part-1-plan.md)
- KeyMap modifier restore: `src/lib/deskflow/KeyMap.cpp:542`, `766`
- Modifier keys: `src/lib/deskflow/KeyTypes.h:262-263`
- Rescue chord: `src/lib/server/Server.cpp:1880-1883`
