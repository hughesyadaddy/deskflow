# Code Simplicity Review — Windows Input Injection Path

**Date**: 2026-07-03
**Reviewer**: Code Simplicity Review Agent (VGV)
**Scope**: `src/lib/platform/MSWindowsDesks.{cpp,h}`, `MSWindowsScreen.{cpp,h}`, `MSWindowsVhidPipeClient.{cpp,h}`, `MSWindowsCursorVisibility.{cpp,h}`, `src/lib/coordination/KeyboardRouter.{h,cpp}`, `RelayKeyEvent.h`, `KeyboardRescue.h`, relay glue in `Server.cpp` / `Coordinator.cpp`

## Verdict

The live injection path is in decent shape — the desk-thread message hop is essential Win32 complexity and should stay. The problem is a large body of **dead scaffolding checked in but never wired into the build** (~850 lines from one WIP commit), plus a scattering of dead fields, dead duplicate functions, and vestigial parameters left behind by refactors. Roughly **~920 lines are removable today** with zero behavior change.

**Estimated removable lines: ~920**

| Severity | Count |
| --- | --- |
| Critical | 2 |
| Important | 5 |
| Suggestions | 6 |

---

## Critical

### C1. ~850 lines of unbuilt VHID phase-2 scaffolding — remove or move to a branch

Commit `76047f7cd` ("wip(windows): vhid core-pipe phase 2 scaffolding (not yet wired into build)") added six source files and four test files that appear in **no `CMakeLists.txt` anywhere in the tree**:

| File | Lines | In build? |
| --- | --- | --- |
| `src/lib/platform/MSWindowsVhidPipeClient.cpp` | 352 | No — absent from `src/lib/platform/CMakeLists.txt:25-61` |
| `src/lib/platform/MSWindowsVhidPipeClient.h` | 126 | No |
| `src/lib/platform/MSWindowsCursorVisibility.cpp` | 43 | No |
| `src/lib/platform/MSWindowsCursorVisibility.h` | 14 | No |
| `src/lib/gui/WindowsDaemonService.cpp` | 191 | No — absent from `src/lib/gui/CMakeLists.txt:18-125` |
| `src/lib/gui/WindowsDaemonService.h` | 39 | No |
| `src/unittests/platform/MSWindowsVhidPipeClientTests.{cpp,h}` | 39 | No — `src/unittests/platform/CMakeLists.txt` registers only clipboard tests |
| `src/unittests/platform/MSWindowsCursorVisibilityTests.{cpp,h}` | 42 | No |

Worse, the code **cannot compile if wired in**:

- `src/lib/platform/MSWindowsVhidPipeClient.cpp:222` reads `Settings::Daemon::VhidBridgeEnabled` — this settings key does not exist anywhere in `src/lib/common/Settings.{h,cpp}` (only the plan doc mentions it).
- `src/lib/platform/MSWindowsVhidPipeClient.cpp:214,225` reference `kSecureDesktopEventName` — defined nowhere in the tree (the plan doc at `docs/plan/2026-06-30-feat-windows-vhid-core-pipe-phase2-plan.md:107` says it *should* be added).

No production code calls `MSWindowsVhidRouting::instance()`, `WindowsDaemonService::*`, or `deskflow::platform::mswindows::setCursorVisibility` — the only references are the files' own definitions and the unregistered test files.

**Recommendation**: Delete all ten files (or park them on the phase-2 feature branch). Checked-in code that doesn't compile is worse than no code: it bit-rots silently, misleads readers into thinking the VHID path is live, and this review had to run three searches to prove it isn't. If keeping in-tree is required, add them to the WIN32 source list behind an `option(DESKFLOW_VHID_PHASE2 OFF)` gate so they at least compile in CI — but that means creating the missing settings key and event-name constant first.

**Removable: ~846 lines.**

### C2. Dead duplicate cursor/window helpers in `MSWindowsScreen`

`MSWindowsScreen` carries private copies of desk-window helpers that only `MSWindowsDesks` actually uses:

- `MSWindowsScreen::createBlankCursor` (`MSWindowsScreen.cpp:748-763`) — defined, never called; the live copy is `MSWindowsDesks::createBlankCursor` (`MSWindowsDesks.cpp:360-373`), byte-for-byte identical logic.
- `MSWindowsScreen::destroyCursor` (`MSWindowsScreen.cpp:765-770`) — defined, never called.
- `MSWindowsScreen.h:139` declares `ATOM createDeskWindowClass(bool isPrimary) const;` — **never defined in `MSWindowsScreen.cpp` at all**. Dead declaration.
- `MSWindowsScreen.h:26` forward-declares `class MSWindowsDropTarget;` — no member, parameter, or usage anywhere in the class.

Separately, the unbuilt `MSWindowsCursorVisibility.cpp` (C1) is a copy-paste of the static `setCursorVisibility` already living in `MSWindowsDesks.cpp:499-532` — an extraction that was started but never finished (the Desks copy was never deleted, the new file was never built).

**Removable: ~30 lines** (on top of C1), and it eliminates a real trap: someone "fixing" the retry loop in one copy and not the other.

---

## Important

### I1. Three input injection paths on Windows; one is dead weight

1. **SendInput via desk thread** (`MSWindowsDesks.cpp:84-107`, `send_keyboard_input` / `send_mouse_input`) — the live, load-bearing path.
2. **Mouser HID passthrough** (`MouserBridge` server-side, `HidConsumer`/`MouserClient` client-side) — live but settings-gated (`Settings::Server::MouserBridgeEnabled`, checked at `Server.cpp:518`); a genuinely separate feature (raw HID++ device relay to an external Mouser instance), acceptably isolated.
3. **VHID pipe** (`MSWindowsVhidPipeClient` → `deskflow-vhid-bridge.exe pipe` → `deskflow-vhid.sys`) — the core-side half is dead (C1), while the bridge exe (`src/apps/deskflow-vhid-bridge-win/`, built via `src/apps/CMakeLists.txt:54`) and the driver (`src/driver/deskflow-vhid/`, own vcxproj) ship anyway.

The confusing part isn't having a secure-desktop strategy — it's that the tree currently ships the *outer* two-thirds of path 3 (bridge exe + driver) with no core code able to talk to it, plus unbuildable core code pretending to. Until phase 2 lands for real, readers tracing "how does input get injected on Windows" hit three candidate answers where only one works. Resolving C1 resolves this; alternatively, document in the bridge app header that the core-side client does not exist yet.

### I2. `RelayKeyEvent::from` is write-only

`RelayKeyEvent.h:31` declares `std::string from;`. It is populated at `Coordinator.cpp:65` (`event.from = message.name;`) and **read nowhere** — not in `Server::relayForwardedKey` (`Server.cpp:1853-1871`), `PrimaryClient::injectForwardedKey` (`PrimaryClient.cpp:152-168`), or `ClientApp::injectRelayedKey` (`ClientApp.cpp:385-403`). Sender identity is already logged at receive time (`Coordinator.cpp:609-611`) before the event is constructed. Delete the field and the assignment. (~3 lines, plus one less string copy per relayed keystroke.)

### I3. `KeyboardRouteInput::secondsSinceRelayStart` is vestigial

`KeyboardRouter.h:36` declares it; `routeKeyboard()` (`KeyboardRouter.cpp:16-29`) never reads it. The boot-grace behavior it fed was deliberately removed — unknown cursor host is now *always* Local (see the comment at `KeyboardRouter.cpp:18-22` and the test at `KeyboardRouterTests.cpp:59-77`, whose name `unknownCursor_usesBootGrace` no longer matches what it asserts). Yet both call sites still compute it: `Coordinator.cpp:658` and `Coordinator.cpp:731`. Note the v1 path legitimately still uses elapsed time via `passKeyToLocalOs` (`KeyboardRelayDecision.h:22`) — only the v2 `KeyboardRouteInput` field is dead. Remove the field, the two assignments, and rename the test. (~10 lines, and it stops implying grace-window behavior that doesn't exist.)

### I4. Commented-out code and no-op overrides in `MSWindowsKeyState`

- `MSWindowsKeyState.cpp:1197-1200`: a commented-out older signature of `m_desks->fakeKeyEvent(...)` directly below the live call, plus the orphaned breadcrumb comment `// vk,sc,flags,keystroke.m_data.m_button.m_repeat` at line 1193. Delete both.
- `MSWindowsKeyState.cpp:740-750`: `fakeKeyDown` and `fakeKeyRepeat` overrides that do nothing but call the base class. They add a stack frame's worth of indirection to every injected key for zero value. Delete the overrides (and their declarations).

(~18 lines.)

### I5. `deskMouseRelativeMove` fires 4–6 `SystemParametersInfo` syscalls per mouse move, and has a save/restore asymmetry

`MSWindowsDesks.cpp:477-496`: every relative move does `SPI_GETMOUSE` + `SPI_GETMOUSESPEED`, two `SPI_SET*`, moves, then two more `SPI_SET*` to restore. Relative mode is per-event, so during a drag this is hundreds of system-wide settings writes per second (and each `SPI_SET*` can broadcast `WM_SETTINGCHANGE`).

Also, line 485 combines the two setter calls with `||` while the getter pair at line 480 uses `&&` — short-circuit means **`SPI_SETMOUSESPEED` is never called when `SPI_SETMOUSE` succeeds**, so speed isn't actually zeroed (and correspondingly gets "restored" anyway at 494-495). This is either a long-standing bug or accidental cleverness; either way the code doesn't do what the comment says.

Pragmatic fix: disable acceleration once on `deskLeave` and restore on `deskEnter` when `m_relativeMouseMoves` is set, rather than per event — and make the setter use `&&`. This is upstream-inherited code, so treat it as a deliberate, tested change, not a drive-by.

---

## Suggestions

### S1. Per-call `GetSystemMetrics` in the absolute-move hot path

`MSWindowsDesks.cpp:458-459` (`deskMouseMove`) queries `SM_CXSCREEN`/`SM_CYSCREEN` on every absolute move. These are cheap cached lookups, but the class already receives shape updates via `setShape()` (`MSWindowsDesks.cpp:207-218`) driven by `WM_DISPLAYCHANGE` — primary-monitor width/height could be cached there. Same pattern in `fakeMouseButton` (`MSWindowsDesks.cpp:257`, `SM_SWAPBUTTON` per click) — that one is per-click, fine to leave.

### S2. Dead members and a typo default in `MSWindowsDesks`

- `MSWindowsDesks.h:252`: `int32_t m_y = 9;` — clearly a typo for `0`. Harmless only because `setShape` runs before any use; still, fix it.
- `m_multimon` (`MSWindowsDesks.h:259`) is assigned at `MSWindowsDesks.cpp:217` and never read. Delete member + the `isMultimon` constructor plumbing through `setShape`.
- `Desk::m_targetID` (`MSWindowsDesks.h:185`) is assigned at `MSWindowsDesks.cpp:806` and never read. Delete.

(~8 lines.)

### S3. Duplicate source entries in coordination CMakeLists

`src/lib/coordination/CMakeLists.txt:7-10` lists `CoordinationProtocol.cpp` / `CoordinationProtocol.h` twice. Harmless to CMake, confusing to humans. (2 lines.)

### S4. `MSWindowsVhidPipeClient.h` formatting rot

Every logical line in the header is separated by a blank line (126 lines for ~45 lines of content) — a mangled generation/paste artifact. Moot if C1 deletes the file; if the file survives, reformat.

### S5. Misleading API in dead pipe client

`MSWindowsVhidPipeClient::sendKeyboardState(uint8_t modifiers, const unsigned char keys[6])` (`MSWindowsVhidPipeClient.cpp:125-134`) accepts a 6-key HID array but only ever inspects `keys[0]`, and the single production-adjacent caller passes `nullptr` (`MSWindowsVhidPipeClient.cpp:340`). A one-key API pretending to be a full HID boot report. Moot under C1; noted so the phase-2 branch doesn't inherit it.

### S6. Blank-cursor mask math is copy-pasted cleverness

Both `MSWindowsDesks.cpp:365-368` and the dead `MSWindowsScreen.cpp:755-758` size the AND/XOR masks as `ch * ((cw + 31) >> 2)` — that's width-rounded-to-32 *times 8 bytes* per row, roughly 8× what a 1-bpp mask needs (`((cw + 31) / 32) * 4`). Over-allocation is harmless but the expression looks like it means something it doesn't. After C2 removes the duplicate, either correct the arithmetic or add a comment admitting it over-allocates.

---

## Load-bearing complexity — keep it

Explicitly assessed and **not** flagged:

- **The desk message-queue hop** (`fakeMouseMove` → `sendMessage` → `PostThreadMessage` → `deskThread` → `deskMouseMove` → `send_mouse_input`, `MSWindowsDesks.cpp:303-357,649-799`): five layers, each thin, and the hop is essential — `SendInput` must run on a thread attached via `SetThreadDesktop` to the current input desktop (`MSWindowsDesks.cpp:658`) or injection fails on secure/winlogon desktops. The synchronous `waitForDesk()` handshake per message is also required to keep event ordering. Do not flatten.
- **Keyboard rescue chord duplication** (`Server.cpp:1794`, `Coordinator.cpp:637`, `Coordinator.cpp:708-715`): three checks of `isKeyboardRescueChord` look redundant but are deliberate defense-in-depth at each grab layer, per the doc comment in `KeyboardRescue.h:15-19`. The predicate itself lives in exactly one header. Keep.
- **`routeKeyboard` vs `passKeyToLocalOs` coexistence** (`KeyboardRouter.cpp` vs `KeyboardRelayDecision.h`): two deciders, but they serve mesh v1 vs v2 protocol epochs, branched explicitly at `Coordinator.cpp:647/717`. Fine for a migration window; schedule v1 removal when mesh v2 becomes the floor.
- **`RelayKeyEvent` as a neutral struct**: three consumers (`Server`, `PrimaryClient`, `ClientApp`) with a shared payload type is the right amount of abstraction; the switch statements at each injection site are honest and obvious.

## Removable-lines summary

| Finding | Lines |
| --- | --- |
| C1 dead VHID/daemon/cursor scaffolding + tests | ~846 |
| C2 dead duplicates in MSWindowsScreen | ~30 |
| I2 `RelayKeyEvent::from` | ~3 |
| I3 `secondsSinceRelayStart` | ~10 |
| I4 commented-out code + no-op overrides | ~18 |
| S2 dead members / typo | ~8 |
| S3 CMake duplicates | 2 |
| **Total** | **~917** |
