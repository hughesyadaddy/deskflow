---
date: 2026-07-08
topic: chord-remap-settings-and-hold-through
vgv_next:
  skill: plan
  artifact: docs/brainstorm/2026-07-08-chord-remap-settings-and-hold-through-brainstorm-doc.md
---

# Chord remap settings + hold-through chords

## What We're Building

Make Deskflow’s **server-side keyboard chord remap table** (today hardcoded in `Server.cpp` for screen `tiny11`) **visible and editable** in the Server Config UI, and upgrade the runtime so remaps that need a **held modifier** behave like real chords — not one-shot taps.

Today, when focus is on `tiny11`, the server rewrites incoming chords before relay (e.g. Mac **Super+Tab** → **Alt+Tab**). That switches windows but does **not** show Windows’ Alt+Tab switcher UI, because each Tab event is rewritten independently while the physical Super key stays held; Windows only sees a brief Alt tap around each Tab.

The fleet wants **both**:

1. **Settings** — per-target-screen remap rows (seeded with today’s PowerToys-style table for `tiny11`), editable like Hotkeys.
2. **Hold-through** — when a remap maps a held source modifier to a held output modifier (Super+Tab → Alt+Tab), keep the **output** modifier down until the **source** modifier is released, so Alt+Tab switcher UI and Tab-stepping work.

## Why This Approach

Three approaches were considered:

### **A. Server Config table + hold-through sessions on Server** ← **Recommended**

Mirror the existing **Hotkeys** pattern in `ServerConfigDialog`: `QListWidget` + New/Edit/Remove, `QSettings` write-array persistence, generated `deskflow-server.conf` section consumed by `deskflow-core`. Replace `kChordRemaps` / `kChordRemapTargetScreen` with loaded config. Add a small **active remap session** on `Server` that tracks “holding output Alt because user is still holding source Super” across `onKeyDown` / `onKeyRepeat` / `onKeyUp` (including relayed keys via `relayForwardedKey`).

- **Pros:** Matches established GUI + conf + server relay path; minimal new surfaces; remaps stay server-side (required for KVM-injected input PowerToys can’t remap).
- **Cons:** Session state must be correct for repeats, modifier-only flag-changed events, and rescue chord / screen jump.
- **Best when:** Fleet has one server (hackintosh) and per-screen Windows target — today’s reality.

### **B. Preferences scalar + JSON blob**

Store remaps in `SettingsDialog` / flat `QSettings` keys.

- **Pros:** Faster to wire a read-only viewer.
- **Cons:** Breaks the split between scalar prefs and server layout; `deskflow-core` may not see updates; no precedent for table editing in Preferences.
- **Best when:** Never — wrong layer for server relay behavior.

### **C. Per-row “hold through” checkbox in UI**

Same as A, but users explicitly mark which rows need hold-through.

- **Pros:** Explicit control; fire-and-forget remaps stay obvious.
- **Cons:** Extra UI complexity; users must understand Alt+Tab semantics; easy to misconfigure.
- **Best when:** We discover auto-detection (output modifier ≠ source modifier on same key) is wrong for some chords.

**Recommendation:** **A**, with **implicit** hold-through when the remap changes which chord modifier is “held” for the same key (Super+Tab → Alt+Tab). Defer per-row checkbox unless auto rules fail in testing.

## Key Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Scope | Settings **and** hold-through equally | User priority |
| Config home | **Server Config**, per **target screen** | Same gate as `kChordRemapTargetScreen`; follows Hotkeys / screen options |
| Hold-through model | **Remap sessions** — output modifier held until source modifier up | Fixes Alt+Tab switcher without per-event mask fighting `KeyMap` |
| Migration | **Seed** current 11-row PowerToys table as default for `tiny11` | No behavior regression on upgrade; table becomes visible/editable |
| UI pattern | Hotkeys-style list + edit dialog (`KeySequenceWidget` for in/out chords) | Proven in `HotkeyDialog`, `ServerConfigDialog` |
| Runtime location | Keep apply in `Server::onKeyDown/Up/Repeat` before relay | Same injection point as today; includes `relayForwardedKey` |
| Implicit vs explicit hold | **Implicit** for modifier-changing remaps on same key | YAGNI — Alt+Tab is the motivating case |

## Technical notes (for planning)

**Current code**

- Hardcoded: `src/lib/server/Server.cpp` — `kChordRemaps`, `applyChordRemap`, gated on `getName(m_active) == "tiny11"`.
- Injection: remapped `(id, mask)` → client `KeyState` / `KeyMap` synthesizes modifiers **per event** — root cause of switcher failure.
- GUI precedent: `src/lib/gui/dialogs/ServerConfigDialog.*`, `Hotkey.h`, `HotkeyDialog.*`, `ServerConfig.cpp` (`beginWriteArray("hotkeys")`).
- Settings: new keys must be registered in `src/lib/common/Settings.h` `m_validKeys` if stored in Ini.

**Hold-through sketch**

```text
User holds Super → Tab down matches remap Super+Tab → Alt+Tab
  → Start session: inject/set Alt DOWN (once)
  → Tab down/repeat: send Tab with Alt already held (mask includes Alt; do not release Alt between repeats)
User releases Super
  → End session: Alt UP
  → Tab up: normal release
```

Must not fight `applyChordRemap` rewriting mask on every event; session owns the **effective** modifier state for the target until source modifier button-up.

**Out of scope (YAGNI)**

- PowerToys import/export UI
- Global remaps across all screens (user chose per-screen)
- Client-side remaps on non-server machines
- Replacing coarse per-screen modifier maps (`ctrl = alt` in screen options)

## Success criteria

- [ ] Server Config shows chord remaps for `tiny11` (and other screens when added), pre-filled with today’s 11 entries.
- [ ] User can add/edit/remove rows; changes persist across restart and reach `deskflow-core`.
- [ ] **Super+Tab** (from Mac Cmd) on `tiny11` shows **Windows Alt+Tab switcher UI** and Tab steps while Super held.
- [ ] Fire-and-forget remaps (Win+Q → Alt+F4, Win+X → Ctrl+X, etc.) still work.
- [ ] No regression for fleet keyboard relay path when cursor on `tiny11`.

## Open Questions (for `/plan`)

1. **Conf format** — New `section: chordRemaps` in server conf vs extend `internalConfig` only? (Hotkeys use both Settings array and conf `keystroke(...)` lines.)
2. **Session detection** — Rule for implicit hold-through: “outMods includes a chord modifier not present in inMods on the same inKey” sufficient?
3. **Modifier key-up source** — Track by `KeyButton` of source Super vs by mask bit? (Left vs right Super/Alt.)
4. **Screen change / rescue chord** — Cancel active hold session when jumping screens or on keyboard rescue?
5. **Read-only indicator** — Show in UI that remaps apply only when that screen is **active** (cursor focused there)?
