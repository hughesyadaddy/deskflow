# Test Quality Review — macOS TIS Main-Queue Regression Tests

**Remediation (2026-07-01):** Findings 1–7 resolved — shared harness (no detach), strong assertions, key-up/autorepeat coverage, `ScopedCGEvent`, `QVERIFY2` messages, main-thread comment in harness header. Added `OSXKeyLayoutTests`. Latest run: `OSXKeyStateTests` 7 pass / 3 skip; `KeyboardRelayMapTests` 7/7; `OSXKeyLayoutTests` 4/4.

**Scope:** `OSXKeyStateTests.cpp/h` (`mapKeyFromEventOffMainThreadDoesNotCrash`), `KeyboardRelayMapTests.cpp/h` (`mapRelayKeyFromCgEventOffMainThreadDoesNotCrash`), and the production code they exercise.

**Diff range:** `7f18e1d20..HEAD` (commits `d4dec8aa9`, `facee934a`)

**Date:** 2026-07-01

---

## Test Quality Review

### Coverage Summary

| Metric | Result |
|--------|--------|
| Test run | **Pass** — `OSXKeyStateTests`: 5 functions, 7 checkpoints, ~1.8s; `KeyboardRelayMapTests`: 1 function, 3 checkpoints, ~136ms |
| Coverage | Not measured (project supports `ENABLE_COVERAGE` per-target; not run for this review) |
| Files with tests | 2/2 testable units in scope |
| Missing test files | None for scoped production entry points |

**Production code under test**

| Unit | File | What the fix does |
|------|------|-------------------|
| `OSXKeyState::mapKeyFromEvent` | `src/lib/platform/OSXKeyState.cpp:265–374` | Wraps TIS layout fetch in `runOnMainQueue` before `UCKeyTranslate` |
| `mapRelayKeyFromCgEvent` | `src/lib/coordination/KeyboardRelayMap.mm:98–143` | Same pattern for relay tap thread (`TISCopyCurrentKeyboardInputSource`) |

**CMake registration**

- `OSXKeyStateTests` — `src/unittests/platform/CMakeLists.txt` (`elseif(APPLE)`)
- `KeyboardRelayMapTests` — `src/unittests/coordination/CMakeLists.txt` (`if(APPLE)`)

Both use the project-standard `create_test()` macro (Qt Test, `QTEST_MAIN`, platform working directory).

---

### State Management Test Quality

N/A — scope is platform/coordination mapping functions, not state management.

---

### Platform / Integration Test Quality

#### `OSXKeyStateTests.cpp` — **Pass with issues**

**Strengths**

- Correctly models the production threading model: worker thread invokes mapping while the test thread pumps `kCFRunLoopDefaultMode` so `dispatch_sync(main_queue)` can complete (matches `OSXMainQueue.h` contract and CGEventTap thread behavior).
- Uses real `CGEventCreateKeyboardEvent` + `updateKeyMap()` — not a stub of the code under test.
- 5-second deadline prevents indefinite hang if main-queue pump regresses.
- Follows existing suite conventions (`initTestCase`, `QVERIFY`/`QFAIL`, ordered slots comment in header).
- Asserts non-zero `KeyButton` — proves the TIS/UCKeyTranslate path returned a mapped button, not just that the call returned.

**Gaps**

- Does not assert expected `KeyID` in `ids`, modifier mask, or specific button for `kVK_ANSI_A`.
- Does not cover `kCGEventKeyUp` off main thread (up-path skips TIS but still exercises `mapVirtualKeyToKeyButton`).
- Does not cover autorepeat events.
- Timeout failure path calls `worker.detach()` then `QFAIL` — detached thread may still run TIS work while the suite continues (flaky follow-on failures).

#### `KeyboardRelayMapTests.cpp` — **Pass with issues**

**Strengths**

- Same valid worker + CFRunLoop pump pattern as `OSXKeyStateTests`.
- Exercises the relay monitor code path (`mapRelayKeyFromCgEvent` used from `OSXKeyboardRelayMonitor.mm:83`).
- `mapped == true` confirms the function completed the down-path successfully (space is a known mappable key).

**Gaps**

- Does not assert `id == ' '`, `button == kVK_Space`, or `phase == Message::KeyPhase::Down`.
- No coverage of `kCGEventKeyUp` (no TIS, but different branch) or autorepeat.
- No coverage of invalid event type → `return false`.
- Same `worker.detach()` anti-pattern on timeout.
- `CGEventRef` not released on early `QFAIL` after detach (minor leak on failure path only).

---

### Pattern Compliance

| Pattern | Status |
|---------|--------|
| Qt Test (`QTest`, `QTEST_MAIN`, `Q_OBJECT`, `private Q_SLOTS`) | Compliant |
| `create_test()` CMake macro | Compliant |
| APPLE-gated registration | Compliant |
| `setUp`/`tearDown` | N/A — per-test local setup is appropriate |
| Group organization | Single focused test per new file/slot; acceptable for regression |
| Project mocking library | N/A — integration-style platform tests correctly avoid mocks |

---

### Anti-Patterns Found

#### **[OSXKeyStateTests.cpp:147] / [KeyboardRelayMapTests.cpp:46]** — Detach thread on timeout

- **Issue:** On 5s timeout, the worker is `detach()`ed before `QFAIL`. A detached thread may still block on or complete `dispatch_sync(main_queue)`, causing nondeterministic CI noise or use-after-scope if it touches captured references after the test function returns.
- **Fix:** Prefer `worker.join()` even after timeout (with a documented hang risk), or use `std::jthread` with stop token where supported, or mark the process aborting. At minimum, release `CGEventRef` in a scope guard and avoid `detach()`.

#### **[OSXKeyStateTests.cpp:154] / [KeyboardRelayMapTests.cpp:53]** — Weak behavioral assertions

- **Issue:** `button != 0` and `mapped == true` prove completion but would not catch incorrect key mapping (wrong glyph, wrong phase, wrong button).
- **Fix:** Add stable assertions, e.g. `QCOMPARE(id, static_cast<KeyID>(' '))` and `QCOMPARE(button, static_cast<KeyButton>(kVK_Space))` for relay; for `OSXKeyState`, assert `ids` contains expected key id for `kVK_ANSI_A` on en layout.

#### **[Both files]** — Duplicated CFRunLoop pump loop

- **Issue:** Identical 15-line worker/pump/join scaffold copied between files (not an anti-pattern per se, but drift risk).
- **Fix:** Optional shared helper in a test utility header (e.g. `runOffMainThreadWithMainQueuePump(Fn)`) if more macOS 14+ queue tests are added.

---

### Regression Coverage Assessment

| Regression / requirement | Covered? | Evidence |
|--------------------------|----------|----------|
| TIS API called off main thread does not assert/crash (`mapKeyFromEvent`) | **Yes** | Worker thread + main CFRunLoop pump; test completes |
| TIS API called off relay thread does not assert/crash (`mapRelayKeyFromCgEvent`) | **Yes** | Same pattern; `mapped == true` |
| Main-queue pump required for `runOnMainQueue` to complete | **Implicit** | Test would timeout without pump (not explicitly tested) |
| Correct key translation after queue hop | **Partial** | Non-zero button / `mapped` only |
| Key-up path without TIS | **No** | Untested off main thread |
| Autorepeat path | **No** | Untested |
| Invalid CGEvent type | **No** | Untested (`mapRelayKeyFromCgEvent` only) |

The tests directly guard the two commits in `7f18e1d20..HEAD` and align with the macOS 14+ TIS main-queue assertion called out in production comments.

---

### Recommendations

1. **Remove `worker.detach()`** — use join or fail hard; highest-impact reliability fix.
2. **Strengthen assertions** — assert expected `KeyID`, `KeyButton`, and `KeyPhase` for the chosen VK codes (`kVK_ANSI_A`, `kVK_Space`).
3. **Add KeyUp off-main-thread test** — cheap extra branch coverage; KeyUp does not hit TIS but validates full relay/key-state lifecycle on background thread.
4. **RAII for `CGEventRef`** — `AutoCF` or scope guard so failure paths do not leak.
5. **Optional:** Extract shared `pumpMainQueueUntil(atomic<bool>& done, deadline)` helper if a third macOS queue test appears.

---

### Verdict

**Fix 3 important issues before treating this as fully hardened; safe to merge as a crash regression guard.**

The new tests are **appropriate and effective** for the stated goal: prevent reintroduction of macOS 14+ TIS main-queue assertion crashes on background threads. They pass locally, follow Deskflow Qt Test conventions, and are correctly gated to `APPLE` builds. Gaps are in **assertion strength**, **timeout failure handling**, and **branch coverage** (key-up/repeat/invalid event) — not in missing test files for the scoped units.

| Severity | Count |
|----------|-------|
| Critical | 0 |
| Important | 4 |
| Suggestions | 3 |

---

## One-Line Findings (Summary Index)

| # | Severity | Finding |
|---|----------|---------|
| 1 | Important | `worker.detach()` on timeout can leave a live background thread after test failure |
| 2 | Important | Assertions only check success/completion, not correct key id/button/phase |
| 3 | Important | Key-up and autorepeat branches of both mappers untested off main thread |
| 4 | Important | `CGEventRef` not released on timeout failure path before `QFAIL` |
| 5 | Suggestion | Duplicate CFRunLoop pump scaffold could be a shared test helper |
| 6 | Suggestion | Add `QVERIFY2` messages on timeout/deadline failures for CI logs |
| 7 | Suggestion | Document in test comment that Qt Test must run on the process main thread |
