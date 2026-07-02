## VGV Code Review

**Remediation (2026-07-01):** All findings addressed in `1e91916c0` and follow-up test hardening:
- `copyUnicodeKeyLayoutOnMainQueue()` in `platform_osx_keys` — DRY, no coordination→`g_tisMutex` import
- Unified `TISCopyCurrentKeyboardLayoutInputSource`; `LMGetKbdType()` on main queue
- Shared `OSXMainQueueTestHarness` — no `worker.detach()`; `ScopedCGEvent` RAII
- Expanded off-main-thread tests (key down/up/repeat, assertions, main-thread parity)
- New `OSXKeyLayoutTests`, `mapRelayKeyFromCgEvent_invalidEventType`
- `fakePoll*` tests skip when OS injection unavailable (Input Monitoring)

**Date:** 2026-07-01  
**Scope:** Commits `d4dec8aa9` and `facee934a` — macOS 14+ `_dispatch_assert_queue_fail` hotfixes for TIS APIs called from CGEventTap threads  
**Files reviewed:**

- `src/lib/platform/OSXAutoTypes.h`
- `src/lib/platform/OSXKeyState.cpp`
- `src/lib/coordination/KeyboardRelayMap.mm`
- `src/lib/coordination/CMakeLists.txt`
- `src/unittests/platform/OSXKeyStateTests.h`
- `src/unittests/platform/OSXKeyStateTests.cpp`
- `src/unittests/coordination/KeyboardRelayMapTests.h`
- `src/unittests/coordination/KeyboardRelayMapTests.cpp`
- `src/unittests/coordination/CMakeLists.txt`

**Context:** Two production crashes (`EXC_BAD_INSTRUCTION` in `_dispatch_assert_queue_fail`) when `TISCopy*` / `TISGetInputSourceProperty` ran on CGEventTap threads. Fix marshals TIS work to the main dispatch queue via `runOnMainQueue`, retains layout `CFData` across the thread hop, and skips TIS entirely on relay key-up.

---

### Summary

The hotfix correctly addresses the root cause: macOS 14+ asserts that Text Input Source (TIS) APIs run on the main dispatch queue, while both `OSXKeyState::mapKeyFromEvent` and `mapRelayKeyFromCgEvent` execute on dedicated CGEventTap / relay CFRunLoop threads. The chosen pattern (`runOnMainQueue` + `g_tisMutex` + `CFRetain`/`AutoCFData`) matches existing code in `getKeyMap`, `getGroups`, and `AppUtilUnix.cpp`, and the relay key-up early return is a sensible optimization that also removes unnecessary main-queue hops.

Production logic looks sound and the diff is appropriately minimal for a crash hotfix. Test coverage exists for the off-main-thread path but is thin: tests mainly prove the call completes without hanging, not that TIS assertions are avoided or that mapping semantics are preserved. A few test-structure issues (notably `worker.detach()` with stack captures) should be fixed before treating these tests as reliable regression guards.

**Verdict:** **Approve with minor follow-ups** — safe to merge for the crash fix; improve tests and consider DRY/consistency follow-ups.

---

### 🔴 Critical — Must Fix Before Merge

None identified in production code paths.

---

### 🟡 Important — Should Fix

- **`src/unittests/platform/OSXKeyStateTests.cpp:146-148` and `src/unittests/coordination/KeyboardRelayMapTests.cpp:45-47`** — `worker.detach()` on timeout with `[&]` lambda captures
  - Why: On timeout, the worker thread may still be blocked in `dispatch_sync(main_queue, …)` while the test function returns via `QFAIL`. The detached thread holds references to stack locals (`event`, `keyState`, atomics). That is undefined behavior in the failure path and can crash or flake CI.
  - Fix: Do not detach. Either join with a hard process exit on timeout, or use `std::async` / a shared control block. Minimum fix: remove `detach()` and `QFAIL` while worker is still joinable only after ensuring the main run loop cannot unblock the worker (hard to guarantee). Prefer restructuring so timeout fails the test without abandoning the thread.

- **`src/unittests/coordination/KeyboardRelayMapTests.cpp` (entire test)** — No coverage for key-up TIS skip
  - Why: Commit `facee934a` intentionally moves TIS fetch after the key-up early return. That behavioral change is untested; a future refactor could reintroduce TIS calls on key-up and regress the crash fix silently.
  - Fix: Add `mapRelayKeyFromCgEventOffMainThreadKeyUpSkipsTis` (or extend existing test) with a key-up event, assert `id == kKeyNone`, `phase == Up`, and return value `true`.

- **`src/lib/coordination/KeyboardRelayMap.mm:127` vs `src/lib/platform/OSXKeyState.cpp:302`** — Inconsistent TIS source APIs (`TISCopyCurrentKeyboardInputSource` vs `TISCopyCurrentKeyboardLayoutInputSource`)
  - Why: Pre-existing divergence, but both paths now share the same threading wrapper, making the inconsistency more visible. When an IME or non-layout input source is active, relay mapping may use a different layout than server-side `mapKeyFromEvent`, causing character mismatches in fleet keyboard relay.
  - Fix: Align relay mapping with `TISCopyCurrentKeyboardLayoutInputSource` unless there is a documented reason not to. If intentional, add a one-line comment explaining why relay uses the current input source.

- **`src/unittests/platform/OSXKeyStateTests.cpp:122-154` and `KeyboardRelayMapTests.cpp:23-54`** — Tests validate completion, not crash prevention or mapping correctness
  - Why: Test names say `DoesNotCrash`, but CI cannot detect `_dispatch_assert_queue_fail`; passing only proves `dispatch_sync` completed within 5 s. `KeyboardRelayMapTests` uses `kVK_Space`, which maps via a hard-coded switch in `translateVirtualKey` — TIS is invoked, but layout-dependent translation is not asserted. Weak guard for regressions.
  - Fix: Assert expected `KeyID`/`KeyButton` for a layout-dependent key (e.g. `kVK_ANSI_A` → non-zero button and populated `ids` in `OSXKeyStateTests`; non-special VK in relay test). Document that the test requires main-thread run-loop pumping (already commented in `OSXKeyStateTests`).

---

### 🔵 Suggestions — Nice to Have

- **`src/lib/platform/OSXKeyState.cpp:300-313` and `src/lib/coordination/KeyboardRelayMap.mm:125-136`** — Duplicated TIS layout fetch block
  - Suggestion: Extract a small helper in `OSXAutoTypes.h` or a new `OSXKeyboardLayout.h` (e.g. `copyUnicodeKeyLayoutDataOnMainQueue()`) to centralize mutex, API choice, and retain/release semantics. Reduces drift risk between the two hot paths.

- **`src/lib/coordination/KeyboardRelayMap.mm:124`** — `LMGetKbdType()` called on the tap thread
  - Suggestion: Likely fine (pre-existing), but if future macOS tightens Carbon global state threading, consider reading keyboard type inside the main-queue block alongside layout data.

- **`src/unittests/platform/OSXKeyStateTests.cpp` and `KeyboardRelayMapTests.cpp`** — Duplicated off-main-thread harness
  - Suggestion: Shared test utility (e.g. `runOffMainThreadWithMainQueuePump(std::function<void()>)`) to DRY the CFRunLoop pump loop and timeout handling.

- **`src/lib/platform/OSXAutoTypes.h:14`** — `AutoCFData` uses `const __CFData` like sibling aliases
  - Suggestion: Consistent with existing `AutoCFArray` style; no change required unless project adopts non-const CF type aliases elsewhere.

---

### Simplicity Assessment

- Lines that could be removed: ~25–30 (via shared TIS layout fetch helper)
- Unnecessary abstractions: None added; `AutoCFData` is appropriate
- YAGNI violations: None — `runOnMainQueue` reuse is justified
- Complexity verdict: **Already minimal** for a hotfix; minor DRY opportunity only

---

### Testing Assessment

- New code with tests: ✅ (both production paths)
- Test quality: **Superficial** — proves no hang, weak behavioral assertions, missing key-up case
- State management test coverage: N/A (no new state units)
- UI component test coverage: N/A

---

## Pass 1: Regressions & Breaking Changes

| Check | Result |
| --- | --- |
| Deleted production code | None — TIS logic relocated, not removed |
| Public API signature changes | None |
| Relay key-up behavior | **Improved** — TIS no longer called on key-up (previously fetched layout then discarded) |
| `mapKeyFromEvent` null layout | Slight edge-case change: missing layout data now returns early with `kKeyNone` instead of falling through to `UCKeyTranslate` failure paths; return value `0` / `kKeyNone` equivalent for `KeyButton` |
| CMake / linkage | Carbon framework linked explicitly for `coordination` — correct for `LMGetKbdType` |
| Existing tests weakened | No |

No breaking changes identified. The relay key-up path is strictly better (less work, same output).

---

## Pass 2: Architecture & Conventions

### Threading model

The fix applies the established `deskflow::platform::osx::runOnMainQueue` pattern documented in `OSXMainQueue.h`:

```cpp
// Main thread in QApplication::exec(); dispatch_sync does not deadlock
// when called from CGEventTap / QThread worker threads.
CFDataRef layoutRef = deskflow::platform::osx::runOnMainQueue([]() -> CFDataRef { ... });
AutoCFData layoutData(layoutRef, CFRelease);
```

Strengths:

1. **Correct retain semantics:** `TISGetInputSourceProperty` does not guarantee an retained object; `CFRetain` before returning from the main-queue block and `AutoCFData` for release is correct.
2. **Mutex preserved:** `g_tisMutex` remains held during TIS calls, consistent with `getKeyMapImpl`, `updateActiveGroupCache`, etc.
3. **Main-thread fast path:** `runOnMainQueue` skips `dispatch_sync` when already on the main thread (`pthread_main_np()`).

Residual risk (accepted, pre-existing with `getKeyMap`):

- **Hot-path latency:** Every key-down that reaches TIS now pays a main-queue round trip. Acceptable for crash hotfix; consider cached layout data (similar to `pollActiveGroup` / `m_activeGroupCache`) if profiling shows input lag.

### Layer separation

`coordination` importing `platform/OSXMainQueue.h` and `platform/OSXAutoTypes.h` is consistent with existing Apple-specific sources (`OSXKeyboardRelayMonitor.mm`, `OSXLocalInputMonitor.mm`). No new cross-layer violation.

### Naming & clarity

Comments at both call sites clearly explain *why* main-queue marshalling is required and reference related patterns (`getKeyMap/getGroups`, `OSXKeyboardRelayMonitor`). Good.

---

## Pass 3: Code Walkthrough

### `OSXAutoTypes.h`

Adds `AutoCFData` — minimal, follows existing RAII aliases. Enables safe cross-thread layout data lifetime.

### `OSXKeyState.cpp` — `mapKeyFromEvent`

Before: TIS calls inline on the calling thread (CGEventTap).  
After: Layout data fetched on main queue; `UCKeyTranslate` still runs on the caller thread using retained layout bytes — valid because `AutoCFData` outlives translation.

Uses `TISCopyCurrentKeyboardLayoutInputSource` — consistent with `updateActiveGroupCache` and `getKeyMapImpl`.

### `KeyboardRelayMap.mm` — `mapRelayKeyFromCgEvent`

Notable improvements beyond threading fix:

1. Key-up returns before TIS — fixes unnecessary work and removes crash surface on key-up.
2. Same retain/`AutoCFData` pattern as `OSXKeyState`.

Uses `TISCopyCurrentKeyboardInputSource` — same as pre-change code; see Important finding on API alignment.

### CMake changes

- `coordination`: explicit `-framework Carbon` — appropriate.
- `unittests/coordination`: `KeyboardRelayMapTests` gated on `APPLE` — matches platform-specific implementation.

---

## Pass 4: Test Review Detail

### `OSXKeyStateTests::mapKeyFromEventOffMainThreadDoesNotCrash`

Positives:

- Uses `kVK_ANSI_A`, which requires layout translation (stronger than relay test).
- Pumps `CFRunLoopRunInMode` so `dispatch_sync` to the main queue can execute — essential pattern, well commented.
- Asserts non-zero `KeyButton`.

Gaps:

- Does not inspect `ids` content.
- Timeout failure path uses `detach()` (see Important).
- Wrapped in `APPLE` platform test target — correct.

### `KeyboardRelayMapTests::mapRelayKeyFromCgEventOffMainThreadDoesNotCrash`

Positives:

- Exercises off-main-thread call with run-loop pump.
- Asserts `mapped == true`.

Gaps:

- `kVK_Space` maps via switch before layout-dependent logic; does not assert `id == ' '`.
- No key-up case.
- Same `detach()` timeout issue.

---

## Recommended Actions

1. **Before merge (soft):** Add relay key-up test; remove or fix `worker.detach()` timeout handling.
2. **After merge (follow-up):** Align TIS source API between relay and key state; extract shared layout fetch helper.
3. **Manual verification:** On macOS 14+, reproduce original crash scenario (fleet relay + local key map) and confirm no `_dispatch_assert_queue_fail` in Console.

---

## Checklist (VGV Standards)

- [x] Root cause addressed with established project pattern
- [x] Minimal, focused diff appropriate for hotfix
- [x] RAII for CoreFoundation objects
- [x] Comments explain non-obvious platform constraint
- [x] Unit tests added for new threading paths
- [ ] Tests cover key-up skip and avoid `detach()` UB — **recommended**
- [ ] Manual macOS 14+ crash reproduction verified absent — **required for confidence**
