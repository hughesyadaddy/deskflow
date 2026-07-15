# Test Quality Review: hotfix/windows-keyboard-shortcuts-uac

**Reviewer:** test-quality-review-agent  
**Date:** 2026-07-07  
**Scope:** Keyboard relay hook policy, key mapping, coordinator `sendKeyForward`, and platform monitor integration paths introduced or touched by the UAC / stale-fleet-routing hotfix.

---

## Executive Summary

The hotfix correctly extracts the Windows swallow decision into a pure, well-tested `keyboardRelayHookShouldPassThrough` function and adds targeted coordinator tests for `sendKeyForward` return semantics. macOS modifier and off-main-thread mapping regressions are partially covered. **The largest gap is platform asymmetry:** Windows `mapRelayKeyFromHook` and both `*KeyboardRelayMonitor` implementations have **no direct unit tests**; Windows behavior is inferred only from the hook-policy truth table. Several user-visible edge cases called out in the hotfix (modifier release, right-side modifiers, mesh-unreachable pass-through, macOS `kCGEventFlagsChanged` swallow vs pass-through) are under-tested or untested at the integration boundary.

| Severity | Count |
|----------|-------|
| Critical | 1 |
| Important | 6 |
| Suggestions | 5 |

---

## Files Reviewed

| File | Role | Test coverage |
|------|------|---------------|
| `KeyboardRelayHookPolicy.h` | Pure swallow/pass-through policy | `KeyboardRelayHookPolicyTests` (5 cases) |
| `KeyboardRelayMap.cpp` (Windows) | VK → KeyID, modifiers, media keys | **None** |
| `KeyboardRelayMap.mm` (macOS) | CGEvent mapping, modifiers, media | `KeyboardRelayMapTests` (6 cases, macOS CI only) |
| `MSWindowsKeyboardRelayMonitor.cpp` | LL hook + policy wiring | **None** (policy tested in isolation) |
| `OSXKeyboardRelayMonitor.mm` | Event tap, flags-changed, media | **None** (map helpers partially tested) |
| `Coordinator.cpp` (`sendKeyForward`) | Mesh forward + rescue chord | 2 new tests + `rescueChord_*` |

---

## KeyboardRelayHookPolicyTests

### What is covered well

The five tests map directly to the decision tree in `keyboardRelayHookShouldPassThrough`:

1. **`passLocal`** short-circuits to pass-through (`passLocalAlwaysPassesThrough`).
2. **`isInjected`** overrides swallow even when `mapped && forwarded` (`injectedWinsEvenWhenForwarded`, `injectedKeysPassThroughWhileForwarding`) — the core UAC / SendInput regression.
3. **`!mapped`** passes through (`unmappedKeysPassThrough`).
4. **`mapped && forwarded`** swallows; **`mapped && !forwarded`** passes through (`swallowOnlyWhenForwarded`).

Assertions are appropriate for a pure inline function: single boolean outcomes with regression comments tying cases to production incidents.

### Gaps

| Gap | Severity | Notes |
|-----|----------|-------|
| No parameterized truth-table test | Suggestion | 16 input combinations collapse to ~6 distinct outcomes due to short-circuiting; a `QTest::addColumn` table would document the contract and catch reordering regressions. |
| `mapped && !forwarded` not named for UAC scenario | Suggestion | `swallowOnlyWhenForwarded` covers it implicitly; an explicit comment/test name referencing “stale routing / mesh unreachable” would link policy to coordinator behavior. |

### Flakiness

None. Fully deterministic, no I/O, no threads.

---

## KeyboardRelayMapTests (macOS only)

### What is covered well

| Test | Assertion quality |
|------|-------------------|
| `mapRelayKeyFromCgEventOffMainThreadDoesNotCrash` | Regression for TIS main-queue dispatch (`runOnMainQueue`); verifies completion + `mapped == true` for Space key. |
| `mediaKeyIdFromNxType_mapsConsumerKeys` | Exact `KeyID` equality for 9 NX types + unmapped rejection. |
| `mapRelayMediaKeyFromCgEvent_ignoresPlainKey` | Negative path for non-system-defined events. |
| `modifierKeys_mapToFleetKeyIds` | Left modifiers: phase flips to `Down`, correct `KeyID`. |
| `mapRelayModifierFromCgEvent_handlesFlagsChanged` | **Modifier release (Command up):** phase `Up`, `id == kKeyNone`, `mask == 0`, `button` retains VK. |
| `mapModifiers_useNeutralMaskBits` | Command+Control → neutral mask bits on standard key down. |

CMake correctly gates this target behind `if(APPLE)` — appropriate because implementations live in `.mm`.

### Coverage gaps

| Gap | Severity | Production reference |
|-----|----------|---------------------|
| **Right-side modifiers** (`kVK_RightShift`, `kVK_RightControl`, `kVK_RightOption`, `kVK_RightCommand`) | **Important** | `modifierKeyIdFromVirtualKey` maps all eight modifier VKs; tests only exercise left four. |
| **Modifier up for non-Command keys** | **Important** | Only Command release tested; Shift/Control/Option release paths share `modifierIsDown` but differ in flag masks. |
| **`mapRelayKeyFromCgEvent` KeyUp path** | Important | Production sets `id = kKeyNone` on key up and returns `true`; untested. |
| **Repeat phase** (`kCGKeyboardEventAutorepeat`) | Suggestion | `mapRelayKeyFromCgEvent` distinguishes Repeat vs Down; no assertion. |
| **`mapRelayMediaKeyFromCgEvent` positive path** | Important | Only negative (plain key) tested; volume/play/etc. decode path in monitor is untested. |
| **Standard key translation** (letters, Tab, Escape) | Suggestion | Off-main-thread test uses Space only; does not assert `id` value. |

### Flakiness risks

**`mapRelayKeyFromCgEventOffMainThreadDoesNotCrash`**

- Spawns a worker thread while main thread pumps `CFRunLoopRunInMode` with 50 ms slices and a **5 s deadline**.
- On timeout: calls `worker.detach()` and `QFAIL` — detached thread may continue running and leak, causing **cross-test pollution** on slow CI hosts.
- **Recommendation:** Prefer `QThread`/`QTest::qWait` with join always, or run mapping on main after verifying off-main dispatch via a mock/stub of `runOnMainQueue`.
- Generally stable on developer Macs; moderate flake risk on overloaded CI.

---

## KeyboardRelayMap.cpp (Windows) — No Tests

### Critical gap

`mapRelayKeyFromHook` is the Windows counterpart to macOS `mapRelayKeyFromCgEvent` and is called on **every** LL hook key event in `MSWindowsKeyboardRelayMonitor::hookProc`. It has **zero unit tests**.

Untested behavior includes:

- VK → KeyID mapping for special keys, letters (`ToUnicodeEx`), and **media VKs** (`VK_VOLUME_*`, `VK_MEDIA_*`) added for fleet relay.
- Left vs right modifier VK distinction (`VK_LSHIFT` vs `VK_RSHIFT`, etc.).
- `activeModifiers()` via `GetAsyncKeyState` / `GetKeyState` — environment-dependent in tests but mockable at API boundary.
- KeyUp clears `id` to `kKeyNone` while returning `true`.
- Repeat keys: `id != kKeyNone || isRepeat` return path.

**Severity: Critical** — the UAC hotfix’s Windows swallow path depends on `mapped` being correct; a mapping bug could cause silent key loss or incorrect swallow decisions even with perfect hook policy tests.

**Recommendation:** Add `KeyboardRelayMapTests.cpp` Windows variant (or `#ifdef _WIN32` sections) behind `if(WIN32)` in CMake, mirroring macOS structure.

---

## CoordinatorFleetPublishTests — New `sendKeyForward` Tests

### What is covered well

| Test | Behavior verified |
|------|-------------------|
| `sendKeyForward_returnsFalseWithoutDestination` | Client role, remote cursor (`hackintosh`), empty election address + no peer mesh address → returns `false`. Aligns with hook policy `forwarded == false` → pass-through. |
| `sendKeyForward_returnsTrueWhenDestinationReachable` | Loopback peer `hackintosh=127.0.0.1` + ephemeral mesh port → `sendKeyForward` returns `true` (mesh `sendTo` succeeds to self). |

These directly exercise the contract that `MSWindowsKeyboardRelayMonitor` relies on: `forwarded = m_send(...)`.

Related existing coverage:

- `rescueChord_forcesRelayLocalUntilCursorMoves` — override state via `relayPassThroughLocal()`; calls `sendKeyForward` for rescue chord but **does not assert its return value**.
- `keyForward_gatingMatrix` — inbound key injection gating (orthogonal to outbound forward).

### Coverage gaps

| Gap | Severity | Notes |
|-----|----------|-------|
| **Mesh unreachable (non-empty destination, send fails)** | **Important** | No test with valid peer IP that is not listening (e.g. `10.255.255.1:ephemeral`) asserting `sendKeyForward` returns `false`. This is the runtime counterpart to `swallowOnlyWhenForwarded`'s `forwarded == false` branch. |
| **No verification that mesh payload was sent** | Suggestion | `returnsTrueWhenDestinationReachable` only checks bool; does not decode received line or assert key/tab/modifiers in protocol. |
| **`sendKeyForward` when role is Server** | Suggestion | Early `return false` at line 586; untested in these two tests (covered indirectly by routing elsewhere). |
| **Local route (`routeKeyboard` → Local)** | Suggestion | Returns `false` without mesh attempt; could pair with `relayPassThroughLocal` test. |
| **Rescue chord return value** | Suggestion | `rescueChord_*` should `QVERIFY(!coordinator.sendKeyForward(...))` on chord down. |

### Flakiness risks

**`sendKeyForward_returnsTrueWhenDestinationReachable`**

- Depends on coordinator `start()` binding mesh to port 0 and successfully sending to `127.0.0.1:<own-port>`.
- Generally reliable; could fail if mesh layer rejects loopback self-send in future refactors.
- Low flake risk today.

---

## Platform Monitor Integration (Untested)

### MSWindowsKeyboardRelayMonitor.cpp

No tests for:

- `LLKHF_INJECTED` early pass-through (duplicates policy `isInjected` but wiring is untested).
- `passLocal` query short-circuit before mapping.
- Hook return `1` (swallow) vs `CallNextHookEx` — end-to-end with injectable `m_send` / `m_passThrough` lambdas.
- `isRepeat` detection via `GetAsyncKeyState`.

**Severity: Important** — policy is tested; **wiring and `mapRelayKeyFromHook` integration are not**.

### OSXKeyboardRelayMonitor.mm

No tests for tap callback behavior:

| Path | Swallow rule | Test status |
|------|--------------|-------------|
| `kCGEventFlagsChanged` | `forwarded ? nullptr : event` | Map helper tested; **swallow decision untested** |
| `kCGEventKeyDown/Up` | Same | Untested |
| `NX_SYSDEFINED` media | Forward Down/Up pair; swallow when forwarded | Map negative only |
| `sourcePid == getpid()` feedback guard | Always pass | Untested |
| Injected media (own pid) | Pass through | Untested |

**Severity: Important** for `flagsChanged` — macOS modifiers do not go through the Windows hook policy at all; they use a **different swallow rule** (`return forwarded ? nullptr : event`) without `KeyboardRelayHookPolicy`. The new `mapRelayModifierFromCgEvent_handlesFlagsChanged` test validates mapping only, not monitor pass-through when `m_send` returns false.

---

## CMakeLists.txt

- `KeyboardRelayHookPolicyTests` and `CoordinatorFleetPublishTests` build on all platforms — correct.
- `KeyboardRelayMapTests` gated to `APPLE` — correct for `.mm` implementation.
- **Missing:** Windows `KeyboardRelayMapTests` or cross-platform map tests for `KeyboardRelayMap.cpp`.

---

## Assertions Quality Assessment

| Area | Verdict |
|------|---------|
| Hook policy | Strong — each branch has a dedicated test with incident context. |
| macOS map helpers | Good for covered paths; weak on right modifiers and media positive path. |
| Coordinator forward | Minimal but correct for bool contract; lacks negative mesh-send case. |
| Monitors | No assertions — highest integration risk. |

### Anti-patterns not observed

- No sleep-based timing assertions in new tests (except off-main-thread poll loop).
- No tautological `QVERIFY(true)` outcomes.
- Tests generally match project QTest style and naming.

---

## Recommended Test Additions (Priority Order)

1. **Windows `mapRelayKeyFromHook` unit tests** (Critical) — VK_TAB + Alt modifier mask, media VKs, modifier left/right, key up clears id.
2. **`sendKeyForward_returnsFalseWhenMeshUnreachable`** (Important) — peer with unroutable IP, remote cursor, assert `false`.
3. **Right-side modifier mapping tests on macOS** (Important) — extend `modifierKeys_mapToFleetKeyIds`.
4. **macOS modifier up matrix** (Important) — Shift/Control/Option release mirror Command test.
5. **`mapRelayMediaKeyFromCgEvent` positive decode** (Important) — synthetic `NX_SYSDEFINED` event or stub NSEvent fields.
6. **Monitor callback tests with injected `KeyForwardSend` mock** (Important) — verify swallow vs pass for `forwarded` true/false on both platforms.
7. **Parameterized hook policy table** (Suggestion).
8. **Assert protocol payload in reachable send test** (Suggestion).

---

## Conclusion

The hotfix demonstrates good engineering hygiene by extracting `KeyboardRelayHookPolicy` and testing the UAC/SendInput regression explicitly. Coordinator tests close the loop on `sendKeyForward`’s boolean contract for empty vs loopback destinations. However, **Windows key mapping and both platform monitors remain a test blind spot**, and macOS modifier coverage stops at left keys and Command release. The most user-visible remaining risk is: **keys swallowed when mesh forward fails on Windows** — policy says pass-through, coordinator test for unreachable mesh is missing, and no integration test proves the hook returns `CallNextHookEx` in that scenario.
