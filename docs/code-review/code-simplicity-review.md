# Code Simplicity Review — macOS TIS Main-Queue Hotfix

**Remediation (2026-07-01):** Important items 1–2 resolved via `OSXKeyLayout.h/mm` and `platform_osx_keys`. Suggestion 3 (TIS API comment) in header. Suggestion 5 (short comment) applied. Shared test harness added (suggestion 4 deferred threshold met).

**Scope:** `OSXAutoTypes.h`, `OSXKeyState.cpp`, `KeyboardRelayMap.mm`, coordination/platform unit tests and CMakeLists (diff `7f18e1d20..HEAD`).

**Date:** 2026-07-01

---

## Simplification Analysis

### Core Purpose

On macOS 14+, Text Input Source (TIS) APIs assert the main dispatch queue. Deskflow reads keyboard layout data from background threads (CGEventTap, relay CFRunLoop). The hotfix must:

1. Hop TIS calls to the main queue via `runOnMainQueue`.
2. Retain layout `CFData` before returning it across threads.
3. Prove off-main-thread callers no longer crash (regression tests).

Everything added should serve thread-safe layout lookup and nothing more.

### Overall Assessment

The fix is **appropriately minimal** for a hotfix: no new classes, no config surface, one RAII alias (`AutoCFData`), and reuse of existing `runOnMainQueue` / `g_tisMutex` infrastructure. The relay path also **correctly moved the KeyUp early return before the TIS fetch**, avoiding a pointless main-queue hop on release events.

The main simplification gap is **copy-pasted TIS fetch lambdas** in two production call sites plus **mirrored off-main-thread test harnesses**. Neither blocks merge; both are worth consolidating in a small follow-up.

**Complexity score:** Low  
**Recommended action:** Minor tweaks only (optional helper extraction)  
**Estimated removable LOC if all suggestions applied:** ~35–45 lines (~25–30% of scoped diff)

---

### Unnecessary Complexity Found

#### 1. Duplicated TIS layout-fetch blocks (Important)

**Where:** `OSXKeyState.cpp:300–313`, `KeyboardRelayMap.mm:125–136`

**Issue:** Both sites implement the same pattern inline:

```cpp
CFDataRef layoutRef = deskflow::platform::osx::runOnMainQueue([]() -> CFDataRef {
  std::lock_guard<std::mutex> lock(g_tisMutex);
  AutoTISInputSourceRef source(TISCopyCurrent*(), CFRelease);
  if (!source) return nullptr;
  CFDataRef ref = (CFDataRef)TISGetInputSourceProperty(source.get(), kTISPropertyUnicodeKeyLayoutData);
  if (ref) CFRetain(ref);
  return ref;
});
AutoCFData layoutData(layoutRef, CFRelease);
```

The only difference is `TISCopyCurrentKeyboardLayoutInputSource` (KeyState) vs `TISCopyCurrentKeyboardInputSource` (relay map).

**Why unnecessary:** Identical threading, locking, retain, and RAII wrapping duplicated across modules. A third caller will likely copy this block again (the codebase already has ~6 other raw TIS call sites in `OSXKeyState.cpp`).

**Suggested simplification:** Add one small platform helper, e.g. in `OSXMainQueue.h` or a tiny `OSXKeyboardLayout.h`:

```cpp
CFDataRef copyRetainedUnicodeKeyLayoutOnMainQueue(bool useLayoutInputSource);
```

Both call sites become two-liners (`AutoCFData layout(copyRetained...(true/false), CFRelease);`). Keeps `g_tisMutex` usage inside platform layer and removes coordination's direct dependency on the mutex header.

---

#### 2. Coordination → platform header coupling for `g_tisMutex` (Important)

**Where:** `KeyboardRelayMap.mm:10`, `OSXAutoTypes.h:18`

**Issue:** The coordination library now `#include`s `platform/OSXAutoTypes.h` solely to access `g_tisMutex` inside an inline lambda. Coordination links `common` and `base`, not `platform`.

**Why unnecessary:** Exposes an implementation detail (global mutex) across layer boundaries. The hotfix works, but every future TIS caller in coordination will repeat the include + lock pattern.

**Suggested simplification:** Encapsulate mutex + TIS fetch inside the platform helper above. `KeyboardRelayMap.mm` only needs `OSXMainQueue.h` (or the new layout helper header).

---

#### 3. Divergent TIS source APIs without comment (Suggestion)

**Where:** `OSXKeyState.cpp:302` vs `KeyboardRelayMap.mm:127`

**Issue:** Key mapping uses `TISCopyCurrentKeyboardLayoutInputSource`; relay mapping uses `TISCopyCurrentKeyboardInputSource`. This difference **pre-dates** the hotfix (only threading changed), but the two hotfixed paths now sit side-by-side with nearly identical comments and no explanation of why the sources differ.

**Why it matters:** If the distinction is accidental, relay and local key mapping could disagree for IME/composed-input layouts. If intentional, a one-line comment at the helper or call site prevents future "consolidation" that breaks relay.

**Suggested simplification:** Document the intent, or unify to one API if behavior testing shows they are equivalent for this use case.

---

#### 4. Duplicated off-main-thread test harness (Suggestion)

**Where:** `OSXKeyStateTests.cpp:122–155`, `KeyboardRelayMapTests.cpp:23–54`

**Issue:** Both tests share the same structure: spawn worker thread → call hotfixed API → pump `CFRunLoopRunInMode` on main → 5s deadline → detach-on-timeout → join → assert success.

**Why unnecessary:** ~30 lines duplicated. Only two call sites today — extracting a helper is optional.

**Suggested simplification:** If a third macOS off-main-thread regression test appears, add a shared `runOffMainThreadWithMainRunLoopPump(Fn)` in test utilities. Not worth it for two tests (YAGNI for now).

---

#### 5. Verbose inline comment (Suggestion)

**Where:** `OSXKeyState.cpp:297–299`

**Issue:** Three-line comment references `getKeyMap`, `getGroups`, and `pollActiveGroup` async cache variant. Accurate but heavy for a localized fix.

**Suggested simplification:** Shorten to one line once a named helper exists: `// TIS requires main queue (macOS 14+); see copyRetainedUnicodeKeyLayoutOnMainQueue`.

---

### Code to Remove / Consolidate

| Location | Action | LOC saved (est.) |
|----------|--------|------------------|
| `OSXKeyState.cpp:300–314` + `KeyboardRelayMap.mm:125–137` | Extract shared TIS layout helper | ~12 production |
| `KeyboardRelayMap.mm` | Drop `OSXAutoTypes.h` include after helper extraction | ~1 |
| `OSXKeyStateTests.cpp` + `KeyboardRelayMapTests.cpp` | Shared test harness (optional, ≥3 tests) | ~25 test |
| `OSXKeyState.cpp:297–299` | Shorten comment after helper | ~2 |

---

### Simplification Recommendations (prioritized)

1. **Extract `copyRetainedUnicodeKeyLayoutOnMainQueue` (or equivalent)**
   - Current: Two identical 12-line lambdas in platform + coordination.
   - Proposed: Single platform function; callers pass layout-vs-input flag or use two thin wrappers.
   - Impact: ~12 LOC saved, mutex stays in platform layer, future TIS callers have one obvious entry point.

2. **Keep KeyUp-before-TIS ordering in relay map**
   - Current: KeyUp returns before TIS fetch (new in this diff).
   - Proposed: No change — this is a correctness + performance win.
   - Impact: Avoids useless main-queue sync on every key release.

3. **Document or unify TIS source API choice**
   - Current: Layout source in KeyState, Input source in relay map, no comment.
   - Proposed: One-line rationale or behavioral test proving equivalence.
   - Impact: Prevents silent divergence; no LOC change.

4. **Defer shared test harness until a third consumer**
   - Current: Two copy-pasted regression tests.
   - Proposed: Leave as-is for now; extract when a third off-main-thread TIS test is added.
   - Impact: YAGNI-compliant; avoids premature test abstraction.

---

### YAGNI Violations

| Item | Violation? | Notes |
|------|------------|-------|
| `AutoCFData` | No | Minimal RAII for cross-thread `CFRetain`/`CFRelease`; directly required. |
| `runOnMainQueue` inline lambdas | No | Matches existing `getKeyMap` / `getGroups` pattern; no new abstraction layer. |
| Duplicate TIS fetch lambdas | **Latent** | Not over-engineered, but violates DRY; helper is justified at 2+ sites. |
| Separate `KeyboardRelayMapTests` target | No | Focused regression for coordination-only code path; one test is enough. |
| Explicit `-framework Carbon` link | No | `LMGetKbdType()` was already used; explicit link fixes latent ODR/link issue. |
| Shared test utility | **Would be YAGNI now** | Only two tests; wait for a third. |

**Not YAGNI (justified):**

- **`CFRetain` before returning from main queue** — Required; `TISGetInputSourceProperty` returns non-owned reference tied to source lifetime.
- **Off-main-thread crash tests** — Minimal smoke tests for the exact failure mode; assertions are loose (`!= 0` / `mapped == true`) which is appropriate for crash-regression tests.
- **`g_tisMutex`** — Pre-existing; serializes TIS calls. Hotfix correctly reuses it rather than introducing a second mutex.

---

### Positive Simplicity Wins

1. **KeyUp short-circuit before TIS** in `mapRelayKeyFromCgEvent` — removes work from the hot path and simplifies control flow.
2. **`AutoCFData`** — one-line type alias; no custom deleter boilerplate at call sites.
3. **No new threading primitives** — reuses `runOnMainQueue` template already proven in `getKeyMap` / `getGroups`.
4. **Tests are smoke-level, not over-specified** — they verify completion without crashing, not full key-mapping semantics off-thread.
5. **CMake changes are minimal** — one framework link, one conditional test target.

---

## Findings Summary

| Level | Count |
|-------|-------|
| Critical | 0 |
| Important | 2 |
| Suggestions | 3 |

### Critical

None. The hotfix is the smallest correct change: main-queue dispatch, retain layout data, RAII cleanup, regression tests. No speculative features or premature abstractions.

### Important

1. **Duplicated TIS layout-fetch lambdas** in `mapKeyFromEvent` and `mapRelayKeyFromCgEvent` — extract a single platform helper (~12 lines saved, clearer ownership).
2. **Coordination depends on `g_tisMutex` via platform header** — helper extraction keeps mutex inside platform layer.

### Suggestions

1. **Document or verify** `TISCopyCurrentKeyboardLayoutInputSource` vs `TISCopyCurrentKeyboardInputSource` divergence.
2. **Shorten** the three-line comment in `mapKeyFromEvent` once a named helper exists.
3. **Defer** shared off-main-thread test harness until a third macOS regression test needs it.

---

## Verdict

**Ready to merge.** Complexity is low and every addition serves the macOS 14+ main-queue requirement. The duplicated TIS fetch blocks are the only meaningful simplification opportunity; extracting one platform helper is optional before merge and reasonable as an immediate follow-up.
