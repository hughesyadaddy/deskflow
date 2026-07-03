# Test Quality Review — Windows Input Injection & Fleet Keyboard Relay

**Scope:** `src/lib/platform/MSWindowsDesks.{cpp,h}`, `MSWindowsScreen.{cpp,h}`, `MSWindowsVhidPipeClient.{cpp,h}`, `src/lib/server/Server.cpp` (`relayForwardedKey`), `PrimaryClient::injectForwardedKey`, `src/lib/coordination/KeyboardRouter.{h,cpp}`, `RelayKeyEvent.h`, `KeyboardRescue.h`, `Coordinator` relay gating.
**Tests evaluated:** `src/unittests/coordination/` (KeyboardRouterTests, CoordinatorFleetPublishTests, CoordinationProtocolTests, FleetStateMergeTests), `src/unittests/platform/` (MSWindowsVhidPipeClientTests, MSWindowsCursorVisibilityTests), `src/unittests/server/` (ServerTests).
**Reviewed:** 2026-07-03

---

## Executive Summary

The pure routing/decision layer (KeyboardRouter, FleetStateMerge, CoordinationProtocol) is genuinely well tested: real assertions, regression-documented cases, and a data-driven acceptance matrix. Everything below that layer — the code that actually injects input on Windows and dispatches relayed keys on the server — is either untested, untestable as written, or covered by test files that **never build**. The two Windows test suites named in this review's scope are not registered in any `CMakeLists.txt`, so a green CI run says nothing about them. For the reported bug (cursor lag / injection correctness), the specific code most likely at fault (`deskMouseMove` absolute mapping, VHID relative-move chunking) has zero executable coverage.

---

## Critical Findings

### C1. The two Windows test suites are never built or run

`src/unittests/platform/CMakeLists.txt:4-37` registers exactly one Windows test (`MSWindowsClipboardTests`, WIN32-gated). Neither `MSWindowsVhidPipeClientTests.cpp` nor `MSWindowsCursorVisibilityTests.cpp` appears in any `create_test` call — a repo-wide search for their names in `CMakeLists.txt` files returns nothing. Both files are dead scaffolding: they compile-check nothing, run nowhere, and their presence creates a false impression of coverage. Any review that says "MSWindowsVhidPipeClient is tested" is describing a file, not a test.

**Fix:** register both under the `if (WIN32)` branch of `src/unittests/platform/CMakeLists.txt` (and see C2 — the production code they test must be in the build first).

### C2. `MSWindowsVhidPipeClient` production code is not in the build, and nothing calls it

`src/lib/platform/CMakeLists.txt:24-61` lists the WIN32 `PLATFORM_SOURCES`; `MSWindowsVhidPipeClient.cpp` and `MSWindowsCursorVisibility.cpp` are absent. A repo-wide search shows no production file includes `MSWindowsVhidPipeClient.h` — `MSWindowsDesks.cpp` never consults `MSWindowsVhidRouting`, so the secure-desktop routing layer is unreachable dead code. Consequences:

- `MSWindowsVhidPipeClientTests.cpp:14-20` (`virtualKeyToHidUsageMapsCommonKeys`) is a reasonable pure-function test in itself (real expected HID usages, a negative case for `VK_F1`), but it exercises code that ships nowhere.
- Untested behavior in this dead code contains bug-relevant logic (see I1) that will bite the moment someone wires it in.

**Fix:** either wire `MSWindowsVhidRouting` into `MSWindowsDesks::deskMouseMove`/`deskMouseRelativeMove`/`send_keyboard_input` and add both files to `PLATFORM_SOURCES`, or delete the pair plus their orphan tests. Don't leave a tested-looking corpse.

### C3. `deskMouseMove` coordinate math is untested and untestable as written — and it is the prime suspect for the injection bug

`src/lib/platform/MSWindowsDesks.cpp:453-464`:

```453:464:src/lib/platform/MSWindowsDesks.cpp
void MSWindowsDesks::deskMouseMove(int32_t x, int32_t y) const
{
  // when using absolute positioning with mouse_event(),
  // the normalized device coordinates range over only
  // the primary screen.
  int32_t w = GetSystemMetrics(SM_CXSCREEN);
  int32_t h = GetSystemMetrics(SM_CYSCREEN);
  send_mouse_input(
      MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, (DWORD)((65535.0f * x) / (w - 1) + 0.5f),
      (DWORD)((65535.0f * y) / (h - 1) + 0.5f), 0
  );
}
```

The mapping is three lines sandwiched between two Win32 syscalls (`GetSystemMetrics`, `SendInput`), inside a class whose constructor calls `CreateCursor`/`RegisterClassEx` (`MSWindowsDesks.cpp:113-129`). It cannot be instantiated in a unit test. Unverified behaviors that matter for injection correctness:

- **Primary-monitor-only normalization.** `SM_CXSCREEN`/`SM_CYSCREEN` are the primary monitor's dimensions. Without `MOUSEEVENTF_VIRTUALDESK` and `SM_XVIRTUALSCREEN`/`SM_CXVIRTUALSCREEN`, any coordinate on a secondary monitor is mapped against the wrong extent — injected positions drift or clamp on multi-monitor Windows hosts. `setShape` receives full multimon geometry (`MSWindowsDesks.cpp:207-218`, `m_multimon`) yet `deskMouseMove` ignores it.
- **Negative coordinates.** Monitors positioned left of or above the primary yield negative `x`/`y`; `(DWORD)` of a negative float is undefined/huge, producing a warp to the far edge.
- **Float rounding.** `65535.0f * x` loses integer precision for large virtual-desktop coordinates; the `+ 0.5f` rounding and `w - 1` denominator (division by zero on a hypothetical 1-px metric, off-by-one at the right edge) are exactly the kind of arithmetic a table-driven test pins down.

**Fix (extraction recipe):** add a pure, header-only function, e.g. in a new `src/lib/platform/MSWindowsMouseMapping.h`:

```cpp
struct AbsoluteNormalized { uint16_t nx; uint16_t ny; };
AbsoluteNormalized normalizeAbsolute(int32_t x, int32_t y,
                                     int32_t virtX, int32_t virtY,
                                     int32_t virtW, int32_t virtH);
```

`deskMouseMove` fetches metrics and calls it; a QtTest suite (buildable on all platforms since it has no Win32 dependency) covers: corners map to 0/65535, center maps to ~32767, negative-origin monitors, 1×1 degenerate shape, and rounding at each edge. This is the same extraction pattern already applied to `setCursorVisibility` → `MSWindowsCursorVisibility.cpp` — it just needs to be finished (see I2).

### C4. The relay injection path — `Server::relayForwardedKey` and `PrimaryClient::injectForwardedKey` — has zero tests

`Server.cpp:1853-1871` contains real branching logic: primary-active → `injectForwardedKey`, otherwise phase-switched dispatch into `onKeyUp`/`onKeyRepeat`/`onKeyDown`. `PrimaryClient.cpp:152-168` wraps injection in `fakeInputBegin()`/`fakeInputEnd()` and maps `RelayKeyPhase` to screen calls. Neither is touched by `ServerTests.cpp` (its four tests cover struct allocation, `resyncEnterIfActiveClient`, `peekConfiguredNeighbor`, and queued switches — `ServerTests.cpp:440-538`).

This is the exact seam where a forwarded key either types on the right screen or vanishes, and the existing `ServerTests` fixture (`TestPlatformScreen` + `TestClientProxy`, `ServerTests.cpp:70-383`) is *already sufficient* to test it: `TestClientProxy` records `enter()` calls; extend it to record `keyDown`/`keyUp`/`keyRepeat`, then assert:

1. `m_active == m_primaryClient` → key reaches the primary screen between `fakeInputBegin`/`fakeInputEnd` (extend `TestPlatformScreen` to count those too — the current no-op overrides at `ServerTests.cpp:124-130` silently swallow the evidence).
2. `m_active == remote` → key arrives at the remote proxy with the same id/mask/button and correct phase mapping (Down/Up/Repeat).
3. Repeat phase passes `count == 1`.

The rescue-chord jump in `Server::onKeyDown` (`Server.cpp:1794-1798`) is equally testable with this fixture and equally untested (see I3).

---

## Important Findings

### I1. VHID routing behavior contradicts its own comment and hides a cursor-lag mechanism — all unverified

Two behaviors in the (currently dead, see C2) `MSWindowsVhidPipeClient.cpp` deserve tests before anyone wires it in:

- **First-move warp.** `refreshState` comments "Keep m_lastX/m_lastY from the normal desktop so the first UAC move is a delta, not a warp from (0,0)" (`MSWindowsVhidPipeClient.cpp:247`), yet `routeAbsoluteMove` does the opposite: when `!m_hasLastPosition` it resets `m_lastX = m_lastY = 0` (`MSWindowsVhidPipeClient.cpp:278-282`), so the first absolute move sends `dx = x, dy = y` — a full-screen relative warp. A unit test of the delta sequence would have caught the comment/code divergence immediately.
- **±127 chunking = latency.** `sendMove` clamps each step to ±127 and loops (`MSWindowsVhidPipeClient.cpp:102-116` via `clampAxis`, `:31-34`). A 3840-px traversal becomes ~30 sequential blocking pipe writes per move event. That is a plausible cursor-lag mechanism, and nothing pins down the chunk count or ordering.

Both are testable today by extracting the delta/chunking logic from the pipe write (inject a `std::function` sink or split a pure `planMoveChunks(dx, dy) -> vector<step>`).

### I2. `setCursorVisibility` was extracted for testability, then duplicated — the copy that runs is not the copy that would be tested

`src/lib/platform/MSWindowsCursorVisibility.cpp:14-41` is a testable, `bool`-returning extraction. But `MSWindowsDesks.cpp:504-532` still contains its own private `void setCursorVisibility(bool)` with the same retry loop, and `deskEnter`/`deskLeave` (`MSWindowsDesks.cpp:543,568`) call the local copy. The extracted file is in no `CMakeLists.txt` (see C2), and its test (`MSWindowsCursorVisibilityTests.cpp:13-22`) never runs (C1). Even if enabled, that test asserts only that the real Win32 `ShowCursor` loop returns `true` in whatever state the CI session's cursor counter happens to be — environment-dependent, no counter injection, borderline no-assertion value. Delete the duplicate in `MSWindowsDesks.cpp`, route callers through the extracted function, add it to the build, and test it with an injected counter (make the `ShowCursor` call a template/functor parameter) rather than the live desktop.

### I3. Rescue chord: detection is tested; everything the chord *does* is not

`KeyboardRouterTests::rescueChord_detection` (`KeyboardRouterTests.cpp:152-164`) is a good test — it covers extra-modifier tolerance (CapsLock bit) and negative cases. But the chord's two behavioral consumers are untested:

- **Client override lifecycle:** `Coordinator::sendKeyForward` sets `m_relayLocalOverride` and snapshots `m_overrideCursorHost` on chord-down (`Coordinator.cpp:637-645`); `relayPassThroughLocal` clears the override only when the fleet cursor host *changes value* (`Coordinator.cpp:708-715`). Untested: chord during forwarding stops forwarding; override persists across identical cursor updates; override clears on a genuine cursor move. `CoordinatorFleetPublishTests` already demonstrates the friend-fixture pattern needed (`armAsClient`, direct state seeding — `CoordinatorFleetPublishTests.cpp:65-69`), so this is incremental work, not new infrastructure.
- **Server jump:** `Server.cpp:1794-1798` jumps to the primary screen on chord when a remote is active. Coverable with the existing `ServerTests` fixture (see C4).

### I4. Relay gating in `Coordinator` is untested: mesh-version gates, unknown-peer drop, and routing handoff

`handleKeyForwardMessage` (`Coordinator.cpp:556-618`) implements security- and correctness-relevant gating: v2 mesh rejects legacy `KeyFwd` and records a version mismatch (`:561-568`), only server-epoch or client-cursor-host machines accept keys (`:582-587`), and the server drops keys from unknown peers (`:594-597`). `sendKeyForward` (`Coordinator.cpp:620-689`) picks destination via `routeKeyboard` + `peerMeshAddress` with a fallback to the election server address (`:665-672`). None of this has a test. `CoordinatorFleetPublishTests` proves the Coordinator is unit-testable offline (protocol-encoded inbound messages, `handleFleetMessage`, `CoordinatorFleetPublishTests.cpp:110-168`); the same pattern applies directly: feed `protocol::encodeKey(...)` through `onMessage` and assert whether a `CoordinationKeyForward` event lands in the `EventQueue`.

---

## What Is Well Covered (credit where due)

- **`routeKeyboard` decision logic** — `KeyboardRouterTests.cpp:36-150`. Local/forward split, case-insensitive self-match (regression-annotated, `:43-50`), unknown-cursor-stays-local with incident reference (`:59-77`), and a 12-row data-driven acceptance matrix over three hosts (`:99-150`). Assertions check both route and forward host; nothing tautological.
- **Fleet seq/authority merge** — `FleetStateMergeTests.cpp:20-179`. Stale-seq rejection, equal-seq replace, new-server-authority accepting lower seq then rejecting same-author stale (`:149-179`), topology-ready edge detection. This is exactly the fleet-seq/authority coverage the scope asked about, and it is meaningful.
- **Protocol round-trips** — `CoordinationProtocolTests.cpp` covers `Key`/`KeyFwd` encode/decode including phases (`:182-224`), malformed input (`:76-82`), and legacy-shape compatibility. Solid boundary validation.
- **Coordinator publish/takeover** — `CoordinatorFleetPublishTests.cpp:244-277` (`serverTakeover_continuesFleetSeq`) verifies the seq-continuation invariant that previously froze the fleet cursor; a real regression test with a real failure mode documented inline.

## Anti-Pattern Notes (minor)

- **Redundant assertion:** `CoordinatorFleetPublishTests.cpp:164-165` — `QCOMPARE(snapshot.links.size(), 1)` immediately followed by `QVERIFY(!snapshot.links.empty())`; the second can never add information after the first.
- **Stale test name:** `KeyboardRouterTests.cpp:59` `unknownCursor_usesBootGrace` — both the during-grace and after-grace branches assert `Local`, because the grace-window semantics were removed from `routeKeyboard` (`KeyboardRouter.cpp:16-29` ignores `secondsSinceRelayStart` entirely). The test is still valid; the name and structure imply a behavior difference that no longer exists. Rename (e.g. `unknownCursor_alwaysLocal`) and drop the dead `elapsed` fixture parameter, or the field itself if nothing else consumes it (the v1 path in `relayPassThroughLocal`, `Coordinator.cpp:717-725`, still does).
- **No-assertion risk if enabled:** `MSWindowsCursorVisibilityTests.cpp:13-22` asserts only the boolean of a live Win32 side effect (see I2).

## Coverage Gaps Ranked Against the Reported Bug (cursor lag / injection correctness)

1. `deskMouseMove` absolute normalization (C3) — wrong-position injection on multi-monitor; untestable until extracted.
2. VHID `sendMove` chunking and first-move delta (I1) — direct lag mechanism; currently dead code with a live-looking test.
3. `relayForwardedKey`/`injectForwardedKey` dispatch (C4) — key loss/duplication on the primary-vs-remote boundary.
4. Rescue override lifecycle (I3) — a stuck override silently eats all keystrokes, indistinguishable from "injection broken" in a bug report.
5. Relay gating (I4) — dropped keys from casing/peer-list mismatches present as intermittent injection failure.

## Recommendations (ordered)

1. Register the two Windows suites in `src/unittests/platform/CMakeLists.txt` under `if (WIN32)`; add their production files to `src/lib/platform/CMakeLists.txt` or delete all four files (C1/C2).
2. Extract `normalizeAbsolute` from `deskMouseMove` into a Win32-free header and add a cross-platform table-driven test covering virtual-desktop offsets, negative origins, and edge rounding (C3).
3. Extend `ServerTests` with `relayForwardedKey` tests using the existing fixture: primary-active injection, remote-active dispatch, phase mapping, and the rescue-chord jump (C4, I3).
4. Add `Coordinator` relay tests following the `CoordinatorFleetPublishTests` pattern: rescue override set/persist/clear, unknown-peer drop, mesh-version gates, and `sendKeyForward` destination selection (I3, I4).
5. Deduplicate `setCursorVisibility`, keep only the extracted testable version, and rewrite its test with an injected counter (I2).
6. Rename `unknownCursor_usesBootGrace` and remove the redundant assertion in `CoordinatorFleetPublishTests` (minor).
