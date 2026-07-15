# Code Simplicity Review — `hotfix/windows-keyboard-shortcuts-uac`

**Reviewer:** code-simplicity-review-agent  
**Date:** 2026-07-07  
**Base:** `origin/master`  
**Branch tip:** `hotfix/windows-keyboard-shortcuts-uac` (commit `1b54ae699`)  
**Scope:** 17 files in coordination keyboard relay path (see review request)

## Executive summary

The branch delivers real keyboard-relay fixes (fleet cursor routing, media keys, modifier
`kCGEventFlagsChanged` handling, swallow-only-when-forwarded, UAC/injected-key pass-through).
Most additions are justified by platform quirks and regression tests.

The main simplicity concerns are: (1) **incomplete integration** of the UAC hook-policy
work-in-progress (untracked files + uncommitted monitor changes), (2) **duplicate modifier
VK→KeyID tables** on macOS, and (3) **large Coordinator/fleet-hub coupling** bundled into
a keyboard hotfix. `KeyboardRelayHookPolicy` itself is a reasonable extraction for
testability, but the Windows hook currently makes two of its four inputs dead on the
production path.

## Review context

### Committed vs working tree

| State | What it contains |
| --- | --- |
| **Committed** (`origin/master..HEAD`) | Fleet v2 Coordinator refactor, media-key relay, `running()` self-heal, macOS pid guard fix, Windows media VK mapping, `CoordinatorFleetPublishTests` |
| **Uncommitted (scoped files)** | `KeyboardRelayHookPolicy.h`, hook-policy tests, `sendKeyForward` → `bool`, swallow-only-when-forwarded in monitors, macOS modifier relay (`mapRelayModifierFromCgEvent`), neutral `KeyModifier*` mask fix |

The UAC regression fix (injected `SendInput` keys must not be swallowed) lives in the
**uncommitted** layer. The committed branch tip still swallows after every mapped key on
Windows regardless of forward success.

---

## Finding 1 — `KeyboardRelayHookPolicy` (justified abstraction, slim it)

### What it does

`KeyboardRelayHookPolicy.h` extracts the swallow decision into a pure function:

```cpp
inline bool keyboardRelayHookShouldPassThrough(const KeyboardRelayHookContext &ctx)
```

Decision order: pass if local routing → pass if injected → pass if unmapped → swallow only
when `mapped && forwarded`.

### Verdict: **justified**

- The UAC/stale-routing regression is non-obvious; unit tests document intent clearly.
- Separating policy from `hookProc` keeps the Windows callback readable.
- `KeyForwardSend` returning `bool` (uncommitted) gives a single signal for “mesh accepted
  the key,” which is simpler than inferring swallow from side effects.

### Simplicity issue: redundant inputs at call site

`MSWindowsKeyboardRelayMonitor::hookProc` (uncommitted) early-returns for `passLocal` and
`isInjected` **before** building `KeyboardRelayHookContext`. When the policy runs,
`passLocal` and `isInjected` are always `false`. Those branches exist only for tests.

**Recommendation:** Either (a) remove the early returns and let the policy own the full
decision tree, or (b) slim the policy to `!mapped || !forwarded` at the call site and
keep early returns for the cheap cases. Option (b) is smaller diff; option (a) is DRYer.

### Platform asymmetry

macOS (`OSXKeyboardRelayMonitor.mm`) inlines `forwarded ? nullptr : event` and does **not**
use `KeyboardRelayHookPolicy`. Injected keys are filtered via `sourcePid == getpid()` instead
of `LLKHF_INJECTED`. This asymmetry is **acceptable** — different OS event models — but
document it so future contributors do not “unify” the paths blindly.

---

## Finding 2 — Duplicate modifier mapping (`translateVirtualKey` vs `modifierKeyIdFromVirtualKey`)

### What changed (uncommitted `KeyboardRelayMap.mm`)

Three parallel structures now exist for modifiers:

1. `modifierKeyIdFromVirtualKey` — VK → `KeyID` (10 cases)
2. `modifierIsDown` — VK + flags → pressed/released (parallel switch)
3. `translateVirtualKey` — repeats the same 10 VK → `KeyID` cases in its `switch`

`mapRelayModifierFromCgEvent` uses (1) and (2) for `kCGEventFlagsChanged` events.
`mapRelayKeyFromCgEvent` uses (3) for ordinary key down/up.

### Verdict: **unnecessary duplication**

The modifier cases in `translateVirtualKey` are not wrong (modifiers can appear as key
down/up on some paths), but maintaining two identical VK tables will drift.

**Recommendation (minimal):** At the top of `translateVirtualKey`'s `default` path:

```cpp
if (const KeyID mod = modifierKeyIdFromVirtualKey(vk); mod != kKeyNone) {
  return mod;
}
```

Then delete the 10 modifier cases from `translateVirtualKey`. Keep `modifierIsDown` — it
answers a different question (flags-changed phase), though it could later become a small
lookup table if desired.

`modifierIsDown` duplicates VK groupings from `modifierKeyIdFromVirtualKey` (e.g. both
shifts share one flag). That is inherent to CGEventFlags, not over-engineering.

### Windows side

`KeyboardRelayMap.cpp` maps modifier VKs inside `mapVirtualKey` only. Windows delivers
modifiers through the normal hook path (no separate flags-changed handler). **No duplicate
table on Windows** — appropriate platform split.

---

## Finding 3 — `mediaKeyIdFromNxType` duplication

`KeyboardRelayMap.mm` duplicates `convertNXKeyTypeToKeyID` from
`platform/OSXMediaKeySupport.m`. The comment acknowledges this trade-off: coordination stays
testable without linking the platform layer.

**Verdict: acceptable YAGNI trade-off** for ~25 lines. Extract a shared neutral header only
if a third copy appears or mappings diverge (e.g. `NX_KEYTYPE_FAST` aliasing).

---

## Finding 4 — Coordinator scope in a keyboard hotfix

### Committed changes (`Coordinator.cpp` / `.h`)

+578 / +69 lines vs master in scoped files alone. Most is **fleet state hub** work
(`mergeAndBroadcastFleetFragment`, `publishFleetTopology`, `wakePeer`, version probes,
worker relay reconciler) — not keyboard shortcuts per se.

Within scope, keyboard-relevant pieces are lean:

- `relayPassThroughLocal` → `KeyboardRouter` + rescue override
- `sendKeyForward` → direct-to-cursor-host via `peerMeshAddress`
- `handleKeyForwardMessage` → client-as-cursor-host injection path
- Worker self-heal for `m_keyboardRelay->running()`

### Verdict: **scope coupling (Important, not YAGNI violation)**

The fleet refactor appears intentional (mesh v2 replaces `KeyboardRelayDecision`). The
keyboard hotfix **depends** on fleet cursor host for routing. Bundling is coherent but
makes review and rollback harder.

`peerMeshAddress` is a reasonable inline helper; extracting a `FleetPeerResolver` class
would be over-engineering at current size.

---

## Finding 5 — `CoordinatorFleetPublishTests` (506 lines)

### Positives

- Ephemeral mesh port (`meshPort = 0`) avoids test collisions — good pattern.
- Keyboard tests (`rescueChord_forcesRelayLocalUntilCursorMoves`,
  `keyForward_gatingMatrix`, `sendKeyForward_returns*`) directly exercise relay gating.

### Simplicity issues

1. **`friend class CoordinatorFleetPublishTests`** — test coupling to private
   `m_mutex`, `m_election`, `m_fleetState`, `m_lastWakeAt`. Necessary for white-box tests
   but increases fragility. Prefer package-visible test hooks or small package functions
   if this file keeps growing.

2. **Mixed concerns** — ~70% fleet publish/wake/version tests, ~30% keyboard. File name
   suggests fleet-only. Keyboard relay tests fit better beside `KeyboardRouterTests` or a
   dedicated `CoordinatorKeyboardRelayTests`.

3. **`sendKeyForward_returnsTrueWhenDestinationReachable`** — loops back to own mesh
   listener; clever but opaque. Comment helps; still integration-heavy for a unit test.

**Verdict: tests are valuable; file organization is the simplification opportunity.**

---

## Finding 6 — Monitor self-heal (`running()` / `m_active`)

Added on both platforms: `start()` reaps dead threads; Coordinator worker restarts relay
if client epoch lacks a live hook/tap.

**Verdict: justified.** Without it, permission loss leaves a silent no-relay state. Small
`atomic<bool> m_active` flag is the minimal fix. Not over-engineered.

---

## Finding 7 — Incomplete UAC fix integration (Critical)

| Artifact | Git status |
| --- | --- |
| `KeyboardRelayHookPolicy.h` | **Untracked** |
| `KeyboardRelayHookPolicyTests.{cpp,h}` | **Untracked** |
| `MSWindowsKeyboardRelayMonitor.cpp` (policy + injected guard) | **Modified, uncommitted** |
| `Coordinator.cpp` (`sendKeyForward` → `bool`) | **Modified, uncommitted** |
| `CMakeLists.txt` (HookPolicyTests target) | **Modified, uncommitted** |

**Committed branch tip does not include the UAC hook-policy fix.** CI builds the old
“always swallow after map” Windows behavior. The working tree references a header that is
not in git — **build breaks on a clean checkout** once monitor changes are committed without
the policy header.

**Action:** Commit policy header + tests + monitor/Coordinator/CMake changes atomically.

---

## Finding 8 — `CoordinationMesh` scoped changes

Small, focused additions: `FD_CLOEXEC`, ephemeral `port()` after bind, legacy v1 message
drop. **No simplicity concerns** — each supports tests or hygiene.

---

## Summary table

| Item | Verdict |
| --- | --- |
| `KeyboardRelayHookPolicy` | Justified; slim dead branches at Windows call site |
| `KeyForwardSend` → `bool` | Justified |
| Modifier duplicate tables (macOS) | Unnecessary; delegate to `modifierKeyIdFromVirtualKey` |
| `mediaKeyIdFromNxType` copy | Acceptable documented trade-off |
| Fleet hub in Coordinator | Coherent but large; scope coupling |
| `CoordinatorFleetPublishTests` | Valuable; split keyboard vs fleet tests |
| Monitor `running()` self-heal | Justified |
| Untracked policy files | **Critical** — incomplete integration |

## Recommended actions (priority order)

1. **Commit atomically:** `KeyboardRelayHookPolicy.h`, tests, CMake entry, monitor,
   Coordinator `bool` return, macOS modifier relay.
2. **Deduplicate modifiers:** `translateVirtualKey` → call `modifierKeyIdFromVirtualKey`.
3. **Slim hook policy call site:** pass only `{mapped, forwarded}` or remove redundant
   early returns.
4. **Split tests:** move keyboard relay cases out of `CoordinatorFleetPublishTests` when
   touching that file next.
5. **Defer:** shared NX media-key table extraction until a second drift incident.
