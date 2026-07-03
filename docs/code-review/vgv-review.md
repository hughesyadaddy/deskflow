# VGV Code Review — Windows Input Injection Path

**Date:** 2026-07-03
**Repo:** `/Users/alexhughes/Desktop/deskflow` (C++/Qt, deskflow fork)
**Scope:** Windows injected mouse/keyboard delivery: `MSWindowsDesks`, `MSWindowsScreen`, `MSWindowsVhidPipeClient`, `Server::relayForwardedKey` / `PrimaryClient::injectForwardedKey`, `KeyboardRouter`, `RelayKeyEvent`, `KeyboardRescue`.
**Context:** Reported cursor lag only on the Windows client (Proxmox VM, GPU passthrough, 4K single display). Physical mouse on the same machine is smooth; network RTT sub-ms; CPU ~10%. That symptom profile points at the injection pipeline itself, not the network or the compositor.

---

## Executive summary

The single biggest latency contributor is architectural: **every injected mouse move is a synchronous cross-thread round trip**. `MSWindowsDesks::sendMessage` posts a thread message to the desk worker thread and then **blocks the client's event-loop thread on a condition variable** until the desk thread has executed `SendInput` and signaled back (`MSWindowsDesks.cpp:351-357`, `891-900`, `785-788`). Each move therefore costs two context switches plus scheduler wake-up latency — cheap on bare metal, but on a VM with vCPU scheduling this is routinely 1–10 ms of jitter per event. While blocked, the client cannot drain the socket, so `ServerProxy` mouse compression kicks in and drops intermediate positions, which the user perceives as a chunky, laggy cursor. The physical mouse bypasses all of this, which matches the reported symptoms exactly.

Compounding it: the process priority is raised to time-critical for the **main** thread (`AppUtilWindows.cpp:118`) while the desk thread that actually calls `SendInput` runs at default priority — exactly backwards for a latency-critical injection thread.

Three genuine correctness bugs were also found (an `||`/`&&` bug that has disabled half of the mouse-speed override for years, an uninitialized member read that is UB on every secondary-screen enter, and an unchecked `PostThreadMessage` that can deadlock the input thread forever). The new VHID pipe client is WIP quality and — importantly — **is not wired into the injection path at all**, so it cannot be the cause of the lag.

---

## Answers to the specific questions

### Q1. Is the absolute mouse-move mapping sound? Is the per-move `GetSystemMetrics` a problem?

**Per-move `GetSystemMetrics` is NOT a latency problem.** `GetSystemMetrics(SM_CXSCREEN)` reads from a per-process cached connection block in user32; it is not a kernel transition per call. Calling it twice per move (`MSWindowsDesks.cpp:458-459`) is measured in nanoseconds. It is untidy — `setShape` already caches `m_w`/`m_h` (`MSWindowsDesks.cpp:207-218`) and could be used — but fixing it will not change perceived lag.

**Correctness on a 4K single-display VM: OK, with two caveats.**

1. **Primary-monitor-only mapping** (`MSWindowsDesks.cpp:453-464`). Without `MOUSEEVENTF_VIRTUALDESK`, `MOUSEEVENTF_ABSOLUTE` coordinates are normalized over the *primary* monitor only. On a single display this is self-consistent and correct. On any multi-monitor Windows client, injected absolute moves are clamped to the primary monitor — the cursor can never be injected onto a secondary monitor. `m_multimon` is tracked (`MSWindowsDesks.h:259`) but never consulted here. Fix: when `m_multimon` is true, use `MOUSEEVENTF_VIRTUALDESK` with `SM_XVIRTUALSCREEN`/`SM_CXVIRTUALSCREEN` normalization. Latent bug for this user (single display), real bug for the codebase.

2. **DPI awareness.** `deskflow-core.exe.manifest` declares no `<dpiAware>` element and no code calls `SetProcessDpiAwareness*`. A DPI-unaware process on a scaled 4K display (e.g. 150%) sees virtualized metrics (2560 instead of 3840). The normalization is a ratio, so positions land in the right place — but the injected cursor can only occupy 2560 distinct horizontal positions across 3840 physical pixels, i.e. it moves in ~1.5-px steps. On 4K that reads as subtle choppiness. If the VM runs at 100% scaling this is moot; otherwise add a Per-Monitor V2 DPI manifest entry.

3. Rounding nit: `(65535.0f * x) / (w - 1) + 0.5f` (`MSWindowsDesks.cpp:461-462`) is the classic pixel-center formula and works; Microsoft's documented mapping is `x * 65536 / w`. Worst-case error is sub-pixel. Cosmetic.

### Q2. Does the message-queue hop to the desk thread add latency or backlog?

**Yes — this is the headline finding.** The flow per injected move is:

- Client event-loop thread: `MSWindowsScreen::fakeMouseMove` (`MSWindowsScreen.cpp:699-702`) → `MSWindowsDesks::fakeMouseMove` (`MSWindowsDesks.cpp:303-306`) → `sendMessage` → `PostThreadMessage` + **`waitForDesk()` blocks** (`MSWindowsDesks.cpp:351-357`, `891-900`).
- Desk worker thread: `GetMessage` loop wakes (`MSWindowsDesks.cpp:679`), dispatches `DESKFLOW_MSG_FAKE_MOVE` → `deskMouseMove` → `SendInput`, then takes the mutex and broadcasts `m_deskReady` (`MSWindowsDesks.cpp:785-788`).
- Client thread wakes, resets the flag, returns.

Consequences:

- **Two context switches per move, serialized.** There is never more than one injected event in flight, so there is no queue backlog — but max throughput is `1 / round-trip`, and on a contended VM the round trip has millisecond-scale tail latency. A server sending 200–500 moves/sec will exceed what this handshake can sustain under VM scheduling jitter.
- **The block starves the network.** While parked in `waitForDesk`, the event-loop thread cannot read the socket. Arriving moves buffer in TCP; on the next read pass `m_stream->isReady()` is true, so `ServerProxy::mouseMove` engages compression (`ServerProxy.cpp:690-702`) and only the last position survives (`flushCompressedMouse`, `ServerProxy.cpp:383-395`, called after the drain loop at `:134`). Position stays correct; intermediate motion is discarded → visible steppiness whose magnitude scales with the round-trip cost. This is the coupling that turns per-event jitter into perceived lag.
- **Thread priority is inverted.** `AppUtilWindows.cpp:118` calls `Thread::getCurrentThread().setPriority(-14)`, which via `ArchMultithreadWindows::setPriorityOfThread` (`ArchMultithreadWindows.cpp:333-389`) clamps to the top of the table: `REALTIME_PRIORITY_CLASS` + `THREAD_PRIORITY_TIME_CRITICAL` (falls back to HIGH class without the privilege). Note `SetPriorityClass(GetCurrentProcess(), ...)` there changes the class for the whole process as a side effect. Meanwhile the desk thread — the one that actually performs `SendInput` — is created with default priority (`MSWindowsDesks.cpp:807`, no `setPriority` call). The latency-critical thread is the *lowest*-priority participant in the handshake.

**Fix (concrete):**

1. Make the fire-and-forget messages actually fire-and-forget. `DESKFLOW_MSG_FAKE_MOVE`, `FAKE_REL_MOVE`, `FAKE_WHEEL`, `FAKE_BUTTON`, and `FAKE_KEY` carry all state by value in `wParam`/`lParam` and need no reply. Add an async variant of `sendMessage` that skips `waitForDesk`, and stop broadcasting `m_deskReady` for those message types (otherwise a stale broadcast would release a *later* caller's `waitForDesk` prematurely — that matters because `DESKFLOW_MSG_CURSOR_POS` passes a **stack pointer** cross-thread, `MSWindowsDesks.cpp:238-244`, `755-762`, and must keep a strict handshake). The message queue preserves ordering, so no reordering risk.
2. Raise the desk thread priority (e.g. `desk->m_thread->setPriority(-1)` after creation in `addDesk`).
3. Optionally coalesce at the source: before posting a new FAKE_MOVE, `PeekMessage(..., DESKFLOW_MSG_FAKE_MOVE, DESKFLOW_MSG_FAKE_MOVE, PM_REMOVE)` on the desk thread, or track a pending-move flag. With fix 1 in place the client drains the socket faster, so `ServerProxy` compression naturally handles this.

### Q3. Absolute vs relative; `MOUSEEVENTF_MOVE_NOCOALESCE`; coalescing inventory

- **`MOUSEEVENTF_ABSOLUTE` is the right choice** for a secondary screen: it is idempotent and immune to pointer acceleration. Relative mode (`deskMouseRelativeMove`) is dramatically worse as implemented — see the Critical finding below.
- **`MOUSEEVENTF_MOVE_NOCOALESCE` is worth adding** to injected moves (`MSWindowsDesks.cpp:460-463`, `490`). Windows coalesces consecutive `WM_MOUSEMOVE` in a target app's queue by default; the flag exists precisely for injection scenarios so slow-pumping apps see every injected point. Caveat: coalescing drops points, it does not delay the *cursor* — the on-screen pointer position is updated by the input stack regardless. So NOCOALESCE improves in-app stroke fidelity (drawing, games) but will not by itself fix pointer lag. Cheap, low risk, recommended.
- **Coalescing inventory:**
  - `ServerProxy::mouseMove` / `mouseRelativeMove` compression (`ServerProxy.cpp:690-702`, `723-732`): drops all but the latest move whenever the stream has more data ready; flushed at the end of each read batch and before every key/button/wheel event (`ServerProxy.cpp:134`, `537`, `587`, `603`, `631`, `653`, `667`, `744`). Correct design; its aggressiveness is amplified by the Q2 blocking (the longer the injection round trip, the more it drops).
  - Desk thread message queue: `PostThreadMessage` messages are never coalesced by Windows, and because `sendMessage` blocks, at most one is queued. After the Q2 fix, add source-side coalescing (above) as the replacement backpressure.
  - `SendInput` itself does not coalesce; downstream `WM_MOUSEMOVE` queue coalescing is what NOCOALESCE addresses.

### Q4. Correctness bugs, leaks, unhandled errors, thread safety

See the findings below; the material ones are F1–F5 and the VHID cluster F8.

---

## Findings

### Critical

**F1. `||` where `&&` was intended — mouse speed override half-disabled and system settings churned per event.**
`MSWindowsDesks.cpp:485-486`:

```483:487:src/lib/platform/MSWindowsDesks.cpp
  if (accelChanged) {
    int newSpeed[4] = {0, 0, 0, 1};
    accelChanged = SystemParametersInfo(SPI_SETMOUSE, 0, newSpeed, 0) ||
                   SystemParametersInfo(SPI_SETMOUSESPEED, 0, newSpeed + 3, 0);
  }
```

The read path two lines up uses `&&`. With `||`, short-circuit evaluation means `SPI_SETMOUSESPEED` is **never called** when `SPI_SETMOUSE` succeeds, so relative moves are injected with the user's pointer speed still applied — the delta lands in the wrong place. Beyond the logic bug, this function mutates *system-wide* pointer settings four times per relative move, which is slow and races with any concurrent local input. Fix: change to `&&`; better, set the speed once on `deskLeave` and restore on `deskEnter` instead of per event. (Only bites when `kOptionRelativeMouseMoves` is on; the default absolute path is unaffected.)

**F2. Uninitialized `Desk::m_lowLevel` read — undefined behavior on every enter when hooks are unused.**
`MSWindowsDesks.h:189` declares `bool m_lowLevel;` with no initializer and `Desk` is allocated with `new Desk` (`MSWindowsDesks.cpp:803`). It is only ever assigned inside `DESKFLOW_MSG_SWITCH` *and only when `m_useHooks` is true* (`MSWindowsDesks.cpp:687-706`). On a client (secondary screen, hooks unused), `deskEnter` reads it uninitialized at `MSWindowsDesks.cpp:558` (`EnableWindow(desk->m_window, desk->m_lowLevel ? FALSE : TRUE)`), and `deskLeave` branches on it at `:575`. Fix: `bool m_lowLevel = false;` and initialize the other raw members (`m_thread`, `m_threadID`, `m_targetID`, `m_desk`, `m_window`, `m_foregroundWindow`) too.

**F3. Unchecked `PostThreadMessage` + unbounded wait = permanent input-thread deadlock; stack pointer crosses threads.**
`MSWindowsDesks.cpp:351-357`: `sendMessage` ignores `PostThreadMessage`'s return value, then `waitForDesk` (`:891-900`) waits on `m_deskReady` with no timeout. If the post fails (queue full at 10,000 messages, desk thread exited, desktop torn down mid-switch) the calling thread — the client's only event-loop thread — blocks forever. Worse, `getCursorPos` (`:238-244`) passes a pointer to a stack `POINT` through the message; any handshake break makes that a cross-thread dangling-pointer write. `removeDesks` (`:817`) has the same unchecked post. Fix: check the return, log and bail on failure; add a bounded wait (e.g. 1 s) with a loud error; keep the strict handshake only for pointer-carrying messages (see Q2 fix).

### Important

**F4. Synchronous per-move desk-thread round trip is the primary lag mechanism.**
Detailed under Q2. `MSWindowsDesks.cpp:351-357`, `679-789`; interaction with `ServerProxy.cpp:690-702`. Fix: async posting for value-carrying fake-input messages + desk thread priority boost + optional source-side move coalescing.

**F5. `SendInput` return value ignored — injection failures are silent.**
`send_keyboard_input` and `send_mouse_input` (`MSWindowsDesks.cpp:84-107`) discard the return. `SendInput` returns 0 when input is blocked (UIPI, secure desktop, another thread calling `BlockInput`). On a login/UAC desktop every event silently vanishes and the code has no signal to fall back (e.g. to the VHID bridge this fork is building). Fix: check the return; on 0, `LOG_WARN` with `GetLastError()` (rate-limited) and expose a failure counter so routing decisions can react.

**F6. Process runs at (near-)realtime while the injection thread runs at normal priority.**
`AppUtilWindows.cpp:118` (`setPriority(-14)`) → `ArchMultithreadWindows.cpp:341-388` clamps to `REALTIME_PRIORITY_CLASS`/`TIME_CRITICAL` and, note, `SetPriorityClass` at `:387` re-classes the whole process on every `setPriority` call from any thread. The desk thread (`MSWindowsDesks.cpp:807`) never gets a boost. On a VM this inversion amplifies handshake tail latency. Fix: boost the desk thread; reconsider whether the main thread genuinely needs `-14` (that magic number maps far past HIGH class).

**F7. Fleet key relay: no echo guard on `event.from`.**
`Server::relayForwardedKey` (`Server.cpp:1853-1871`) injects into `m_active` without checking `RelayKeyEvent::from` (`RelayKeyEvent.h:31`). If fleet cursor-host state is stale (the exact race `KeyboardRouter.cpp:18-26` worries about), a key forwarded by client X while `m_active` is X gets relayed straight back to X — double-typing or a ping-pong until the snapshot converges. `KeyboardRescue.h` prevents lockout but not duplication. Fix: drop (or inject locally) when `getName(m_active) == event.from`. Also: `injectForwardedKey` (`PrimaryClient.cpp:152-168`) wraps *every* key in `fakeInputBegin()`/`fakeInputEnd()`; on Windows each of those is itself a blocking desk round trip plus a synthesized marker keystroke (`MSWindowsDesks.cpp:778-782`) — three round trips and three `SendInput` calls per relayed key. Batch begin/end around bursts.

**F8. `MSWindowsVhidPipeClient` (WIP) — not integrated, and several landmines before it is.**
`MSWindowsVhidRouting` is referenced only by its own files and unit tests — nothing in `MSWindowsDesks`/`MSWindowsScreen` calls it, so it is dead code in the production path today (and therefore *not* the cause of the current lag). Before wiring it in:
- **Blocking I/O on a latency path.** The pipe is opened without `FILE_FLAG_OVERLAPPED` and `writeCommand` uses blocking `WriteFile` with no timeout (`MSWindowsVhidPipeClient.cpp:45-46`, `93`). A stalled bridge process wedges whichever thread injects. `connect()` can sleep 10×100 ms synchronously (`:44-60`).
- **Move splitting is O(distance).** `sendMove` clamps to ±127 per step (`:31-34`, `:102-116`), so a 3840-px absolute jump becomes ~30 sequential blocking pipe writes, each a text `snprintf` line. Use a 16-bit HID report or batch steps into one write.
- **First-move warp contradicts its own comment.** The comment at `:247` says last position is kept so the first UAC move is a delta, but `refreshState` clears `m_hasLastPosition` (`:242`, `:253`) and `routeAbsoluteMove` then seeds `(0,0)` (`:278-282`), producing a delta from the origin — the cursor jumps by the full absolute position. Seed from `GetCursorPos` instead.
- **Thread safety:** `MSWindowsVhidRouting` is a mutable singleton (`m_modifiers`, `m_lastX/Y`, connect/disconnect) with no synchronization; decide the owning thread (desk thread) and enforce it, or add a mutex.
- `sendKeyboardState` ignores `keys[1..5]` (`:125-134`) and `virtualKeyToHidUsage` covers a tiny VK subset — fine for WIP, but the API shape promises more than it delivers.
Credit where due: unit tests exist (`src/unittests/platform/MSWindowsVhidPipeClientTests.cpp`), which is more than `MSWindowsDesks` has.

**F9. Absolute mapping ignores multi-monitor and DPI awareness.**
Detailed under Q1. `MSWindowsDesks.cpp:453-464` (no `MOUSEEVENTF_VIRTUALDESK`; `m_multimon` unused), missing DPI manifest (`deskflow-core.exe.manifest`). Correct on this user's single display at 100% scaling; positional quantization at >100% scaling on 4K; hard-broken on multi-monitor clients.

### Suggestions

**S1.** `MSWindowsDesks.h:252` — `int32_t m_y = 9;` is a typo for `0`. Benign today only because `setShape` runs before use; fix it before it isn't.

**S2.** `getDesktopName` (`MSWindowsDesks.cpp:928-940`) calls `GetUserObjectInformation` with an unchecked result into an uninitialized `DWORD size`, then `alloca(size + ...)`. A failed first call makes the `alloca` size garbage (potential stack overflow). Check the call; use `std::wstring`/`std::vector` instead of `alloca`.

**S3.** `sendMessage` is a misnomer — it posts (`PostThreadMessage`) and then waits; rename to `postAndWait` (and the new async variant `post`) so the blocking behavior is visible at call sites like `fakeMouseMove`.

**S4.** `createBlankCursor` (`MSWindowsDesks.cpp:363-368`) uses stride `(cw + 31) >> 2`, roughly 2× the required 1-bpp mask size. Harmless over-allocation; use `((cw + 15) >> 4) << 1` (word-aligned bytes per row) or just document it.

**S5.** `deskLeave`'s hard `ARCH->sleep(0.03)` (`MSWindowsDesks.cpp:644`) is transition-only (screen leave), not per-move — correctly excluded from lag suspects — but the comment itself proposes the better fix (poll cursor visibility) and it blocks the desk thread from processing input messages for 30 ms at every leave.

**S6.** Test coverage: the desk message pump, the `waitForDesk` handshake, and coordinate normalization have no tests. The normalization math and the handshake protocol are both pure enough to extract and unit-test (the VHID client shows the pattern).

---

## What to do first (ordered for the reported symptom)

1. **F4/F6**: async-post FAKE_MOVE/REL_MOVE/WHEEL/BUTTON/KEY and boost the desk thread priority — this attacks the round-trip-per-move mechanism directly.
2. **F5**: check `SendInput` returns so injection failures become visible while testing on the VM.
3. **Q3**: add `MOUSEEVENTF_MOVE_NOCOALESCE` (one-line, improves in-app fidelity).
4. **F1, F2, F3**: correctness fixes; small, contained, zero-risk to ship together.
5. **F9**: DPI manifest + virtual-desktop mapping when convenient; verify the VM's scaling factor first.
