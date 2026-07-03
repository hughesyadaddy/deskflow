# Architecture Review — Windows Input Injection Path & Fleet Keyboard Relay

**Date**: 2026-07-03
**Reviewer**: Architecture Review Agent (VGV)
**Scope**: Windows input injection (`MSWindowsDesks`, `MSWindowsScreen`, `MSWindowsVhidPipeClient`), server relay entry (`Server::relayForwardedKey`, `PrimaryClient::injectForwardedKey`), coordination keyboard relay (`KeyboardRouter`, `RelayKeyEvent`, `KeyboardRescue`, `Coordinator` sendKeyForward / relay gating).
**Concern under review**: correctness and latency of injected input on the Windows client.

---

## Verdict

The **static layering is clean** — coordination does not link or include platform, platform does not include coordination, and the relay-injection path crosses boundaries in the right direction (coordination → app layer event → server → deskflow screen → platform). The problems are **dynamic**: thread ownership of the injection pipeline is broken in two places, both of which sit directly on the keystroke hot path for the Windows client. One dead subsystem (`MSWindowsVhidPipeClient`) is checked in but not compiled anywhere.

**Critical: 2 · Important: 5 · Suggestions: 4**

---

## 1. Layer / dependency direction

### 1.1 Static layering is sound

- The `coordination` library links only `common` and `base` (`src/lib/coordination/CMakeLists.txt:47`). Its Windows-specific sources (`MSWindowsKeyboardRelayMonitor.cpp`, `MSWindowsLocalInputMonitor.cpp`) call Win32 directly rather than reaching into the `platform` library — self-contained, no upward dependency.
- The `server` library declares an explicit downward dependency on coordination (`src/lib/server/CMakeLists.txt:48`) and consumes only two deliberately neutral headers: `coordination/RelayKeyEvent.h` (`src/lib/server/Server.h:14`, `src/lib/server/PrimaryClient.h:10`) and `coordination/KeyboardRescue.h` (`src/lib/server/Server.cpp:23`). `RelayKeyEvent` is a plain struct with no mesh/protocol types (`src/lib/coordination/RelayKeyEvent.h:24-32`) — this is exactly the right seam.
- The `platform` library has zero coordination includes (only a comment mention in `src/lib/platform/MSWindowsWatchdog.cpp:189`). No bleed in either direction.

### 1.2 The relay hand-off crosses boundaries cleanly (in shape)

The intended flow is textbook layered:

1. Coordination receives a mesh `Key` message and posts a `CoordinationKeyForward` event carrying a neutral `RelayKeyEvent` (`src/lib/coordination/Coordinator.cpp:614-617`).
2. The app layer subscribes and forwards: `ServerApp::handleCoordinationKeyForward` → `Server::relayForwardedKey` (`src/lib/deskflow/ServerApp.cpp:787-797`); `ClientApp::handleCoordinationKeyForward` → `injectRelayedKey` (`src/lib/deskflow/ClientApp.cpp:376-403`).
3. Server decides destination: local injection via `PrimaryClient::injectForwardedKey` when the active screen is primary, otherwise re-relay through the normal client-proxy key path (`src/lib/server/Server.cpp:1853-1871`).
4. Platform injects: `deskflow::Screen::keyDown` → `MSWindowsScreen`/`MSWindowsKeyState` → `MSWindowsDesks::fakeKeyEvent` → desk thread → `SendInput` (`src/lib/platform/MSWindowsDesks.cpp:246-249`, `726-729`, `84-94`).

The event queue is the correct decoupling mechanism between coordination's network threads and the server/platform world. The problem is that the decoupling is defeated at runtime — see Finding C1.

### 1.3 Minor layering leak: coordination includes `deskflow/` headers

`RelayKeyEvent.h` and `KeyboardRescue.h` include `deskflow/KeyTypes.h` (`src/lib/coordination/RelayKeyEvent.h:9`, `src/lib/coordination/KeyboardRescue.h:9`), which lives in the higher-level `deskflow` library (`src/lib/deskflow/KeyTypes.h`), yet `coordination` declares no dependency on it (`src/lib/coordination/CMakeLists.txt:47` links only `common base`). It compiles because it's a header-only type include and the global include path exposes `src/lib`, but it inverts the declared dependency direction: server → coordination → (undeclared) deskflow, while deskflow's `ServerApp`/`ClientApp` also depend on coordination. Moving `KeyID`/`KeyModifierMask`/`KeyButton` type aliases into `common` (or forward-declaring them) would make the DAG honest. **[Important — see I5]**

---

## 2. Threading architecture of injection

### 2.1 The baseline desk-thread design is sound for a single caller

`MSWindowsDesks` exists because a Windows thread can only inject on the desktop it is attached to. The design — one worker thread per desk, `PostThreadMessage` hand-off, `SendInput` executed on the desk thread (`src/lib/platform/MSWindowsDesks.cpp:649-799`) — is a legitimate, necessary pattern. Each request is a synchronous rendezvous: `sendMessage` posts and then blocks on the `m_deskReady` condvar until the desk thread finishes (`src/lib/platform/MSWindowsDesks.cpp:351-357`, `891-900`).

That rendezvous carries a hard structural assumption: **exactly one caller thread at a time**. `m_deskReady` is a single shared bool with no association between a waiter and its message; two concurrent callers can consume each other's ready signal and return before their own `SendInput` executed. Historically the assumption held — everything funnelled through the main event loop. The coordination relay breaks it (C1).

### 2.2 [C1] `DeliverImmediately` executes injection on the mesh network thread — data race into server and platform state

`Coordinator::handleKeyForwardMessage` posts the relay event with `Event::EventFlags::DeliverImmediately` (`src/lib/coordination/Coordinator.cpp:614-617`). `EventQueue::addEvent` treats that flag as "dispatch synchronously on the calling thread" (`src/lib/base/EventQueue.cpp:194-196`). The calling thread here is a **detached per-connection mesh handler thread** — `CoordinationMesh::serveLoop` spawns one per inbound connection (`src/lib/coordination/CoordinationMesh.cpp:315-324`) and invokes `m_receiver` from it (`src/lib/coordination/CoordinationMesh.cpp:355-359`).

Consequently the entire chain — `ServerApp::handleCoordinationKeyForward` → `Server::relayForwardedKey` → `PrimaryClient::injectForwardedKey` → `Screen::keyDown` → `MSWindowsKeyState::fakeKeyDown` → `MSWindowsDesks::fakeKeyEvent`/`sendMessage`/`waitForDesk` — runs on a network thread, concurrently with the main event loop, which touches the same objects (key-state tables, `m_active`, desk handshake, client proxies). None of these are thread-safe:

- `KeyState`/`MSWindowsKeyState` mutate shared key maps with no locking.
- `MSWindowsDesks::sendMessage`/`waitForDesk` single-caller handshake (2.1) is violated (`src/lib/platform/MSWindowsDesks.cpp:351-357`, `891-900`).
- The non-primary branch of `relayForwardedKey` (`src/lib/server/Server.cpp:1860-1870`) writes to `ClientProxy` TCP streams that are otherwise owned by the event-loop thread.
- Worse, **each key event arrives on a different detached thread** (the sender opens one connection per keystroke — see C2), so consecutive Down/Up events can be dispatched concurrently and even **reordered** (Up injected before Down → stuck keys), with no ordering guarantee at all.

This is the single most important structural defect in the relay path. The fix is small and restores the intended architecture: drop `DeliverImmediately` so the event is queued and dispatched on the event-loop thread like every other cross-thread event in the codebase (that is what the event queue is for). Latency cost is one event-loop wakeup, microseconds against the network hop.

### 2.3 [C2] Blocking TCP connect per keystroke inside the WH_KEYBOARD_LL hook

On the forwarding side (Windows client whose keyboard is being relayed away), `hookProc` runs inside the low-level keyboard hook and calls `m_send` synchronously (`src/lib/coordination/MSWindowsKeyboardRelayMonitor.cpp:88-91`). That lands in `Coordinator::sendKeyForward`, which takes the coordinator mutex (contended by the worker loop and mesh threads) and then calls `m_mesh->sendTo(destination, line)` (`src/lib/coordination/Coordinator.cpp:620-689`). `sendTo` opens a **fresh TCP connection per call with a 700 ms connect timeout**, sends one line, and closes (`src/lib/coordination/CoordinationMesh.cpp:218-228`, timeout at `:35`).

Structural consequences:

- **System-wide keyboard latency.** WH_KEYBOARD_LL callbacks serialize all keyboard input for the whole session. Every relayed keystroke costs a TCP handshake; a slow/sleeping peer costs up to 700 ms with the entire machine's keyboard frozen behind it.
- **Silent unhook.** Windows removes low-level hooks that exceed `LowLevelHooksTimeout` (a few hundred ms). One stalled connect and the relay hook is silently destroyed — keyboard relay stops working with no error path.
- **Reordering at the receiver.** One connection per keystroke means the receiver handles each key event on a separate detached thread (C1), so ordering is not preserved end-to-end.
- The mutex acquisition inside the hook also couples keystroke latency to whatever the coordinator worker is doing (e.g. `broadcastClaim` blocking connects, guarded by the same `m_mutex`).

The architecture needs a hand-off inside the monitor: hook enqueues, a dedicated sender thread owning a **persistent** mesh connection drains the queue. The comment at `src/lib/coordination/Coordinator.cpp:764-767` shows the codebase already learned this exact lesson for claim broadcasts on the macOS event-tap thread; the same medicine applies here.

### 2.4 Receiving-side hand-off latency (baseline path)

For ordinary server→client injection the hand-off is: network → event loop → `PostThreadMessage` → desk thread → `SendInput` → condvar broadcast → event loop resumes (`src/lib/platform/MSWindowsDesks.cpp:351-357`, `726-729`). Two context switches per event plus full event-loop blockage during injection is acceptable, but two details deserve attention:

- `deskLeave` sleeps 30 ms on the desk thread while the event loop is blocked in `waitForDesk` (`src/lib/platform/MSWindowsDesks.cpp:644`) — a real stall on every screen leave. **[S3]**
- `waitForDesk` blocks even for fire-and-forget fakes (key, wheel, move) where no result is returned; a one-way post for those messages would halve the per-event context-switch cost. **[S4]**

---

## 3. MSWindowsVhidPipeClient: dead scaffolding

### 3.1 [I1] Not wired into any build target

- `MSWindowsVhidPipeClient.cpp/.h` are **absent from `PLATFORM_SOURCES`** in the WIN32 branch of the platform build (`src/lib/platform/CMakeLists.txt:24-61` — the list runs `MSWindowsClipboard…` to `MSWindowsWatchdog` and never mentions the VHID files).
- Its unit test exists (`src/unittests/platform/MSWindowsVhidPipeClientTests.cpp`) but is **not registered** in `src/unittests/platform/CMakeLists.txt:4-11`, which only creates `MSWindowsClipboardTests`.
- `MSWindowsVhidRouting::instance()` has **zero call sites** outside its own translation unit (grep of `src/` finds references only in `MSWindowsVhidPipeClient.{h,cpp}` and the unregistered test).

So the class is never compiled, never linked, never invoked. The separate bridge executables (`src/apps/deskflow-vhid-bridge`, `deskflow-vhid-bridge-win`) implement their own pipe handling and do not use this class.

### 3.2 [I1 cont.] The routing hook it presupposes does not exist

`MSWindowsVhidRouting` is designed as an interception layer ("Routes desk injection to the VHID pipe when secure desktop + bridge are active", `src/lib/platform/MSWindowsVhidPipeClient.h:71-73`), but the actual injection points do not consult it: `send_keyboard_input` and `send_mouse_input` in `MSWindowsDesks.cpp:84-107` call `SendInput` unconditionally, and the desk-thread message handlers (`src/lib/platform/MSWindowsDesks.cpp:726-782`) go straight to those helpers. If the file were added to the build today it would still be inert. It also duplicates concept-level responsibilities of the SendInput path (per-axis clamping, modifier tracking, its own VK→HID table at `src/lib/platform/MSWindowsVhidPipeClient.cpp:143-204` that covers only letters, digits, and eight special keys — no function keys, punctuation, or numpad).

**Recommendation**: either wire it in behind the two `send_*_input` helpers in `MSWindowsDesks.cpp` (the single choke point that makes routing tractable, see §4) with the CMake entries and test registration added, or delete it from the tree and keep it on a branch. Checked-in-but-unbuilt code rots and misleads reviewers — the header's own comment claims routing behavior that the product does not have.

---

## 4. Injection entry points: one main path, two side doors, one ghost

Enumerated injection paths that can synthesize input on a Windows client:

| # | Path | Mechanism | Status |
|---|------|-----------|--------|
| 1 | Desk-thread SendInput | `MSWindowsDesks` `send_keyboard_input`/`send_mouse_input` (`MSWindowsDesks.cpp:84-107`) | **Primary path.** All `fakeKey*`/`fakeMouse*` funnel here via desk messages. |
| 2 | Direct SendInput on caller thread | `MSWindowsScreen::fakeLocalKey` (`MSWindowsScreen.cpp:1678-1688`), called from `onKey` stuck-key release (`MSWindowsScreen.cpp:1043-1052`) | Bypasses the desk thread; would fail on a switched desktop. Narrow (primary-screen stuck-key cleanup) but it is a second, unrouted `SendInput` call site. |
| 3 | Mouser HID loopback | Raw HID frames relayed via `kMsgDMouserData`, delivered to a local Mouser process (`src/lib/client/MouserClient.h:5-8`, own worker thread + queue) which injects independently | Parallel path by design; deskflow never `SendInput`s these. Gated by server-side focus/virtual-host tracking (`Server::syncMouserVirtualHostForFleetCursor`, `src/lib/deskflow/ServerApp.cpp:763`). |
| 4 | VHID pipe | `MSWindowsVhidPipeClient`/`MSWindowsVhidRouting` | **Ghost** — not built, not called (§3). |

Assessment: for keyboard there is effectively a **single injection entry point** (path 1) plus a small leak (path 2); that is a good position to be in, and it is exactly where a future VHID router should sit. Mouser HID (path 3) is architecturally parallel rather than competing — it carries device-level reports deskflow cannot synthesize — but two paths can be live simultaneously for the same physical pointer, with mutual exclusion enforced only by server-side focus bookkeeping, not by any client-side arbitration. **[I3]**

### 4.1 [I2] Relay hook does not filter injected input — feedback-loop exposure

The coordination relay hook inspects `LLKHF_UP`/`LLKHF_EXTENDED` but never `LLKHF_INJECTED` (`src/lib/coordination/MSWindowsKeyboardRelayMonitor.cpp:69-91`), and the injectors provide no marker: `send_keyboard_input` sets `dwExtraInfo = 0` (`src/lib/platform/MSWindowsDesks.cpp:92`), as does `fakeLocalKey` (`src/lib/platform/MSWindowsScreen.cpp:1686`). So keys injected by deskflow itself re-enter the relay hook and are subject to routing. Correctness currently depends entirely on fleet-state freshness: if this client is the cursor host, `routeKeyboard` returns `Local` and the injected key passes through. But fleet snapshots converge via 3-second heartbeats (`src/lib/coordination/Coordinator.cpp:30`, `895-921`); in the window where the local snapshot is stale (cursor host recorded as another machine), server-injected keys get **swallowed and forwarded** to the stale host — the user's remote keystroke types on a third machine, or ping-pongs. A structural fix costs three lines: tag injected input with a magic `dwExtraInfo` (the hook infrastructure already has this concept — `DESKFLOW_HOOK_FAKE_INPUT_VIRTUAL_KEY` at `MSWindowsDesks.cpp:778-781`) and pass anything with `LLKHF_INJECTED` or the magic marker in `hookProc`.

### 4.2 [I4] Asymmetric fake-input bracketing between the two relay injection sites

`PrimaryClient::injectForwardedKey` wraps injection in `fakeInputBegin()`/`fakeInputEnd()` (`src/lib/server/PrimaryClient.cpp:155-167`), which on Windows tells the hook layer to ignore self-generated events (`src/lib/platform/MSWindowsScreen.cpp:643-661`, `MSWindowsDesks.cpp:228-236`). The equivalent client-epoch site, `ClientApp::injectRelayedKey`, performs the same `keyDown`/`keyUp` calls with **no bracketing** (`src/lib/deskflow/ClientApp.cpp:385-403`). The two sites implement the same operation ("inject a `RelayKeyEvent` into the local screen") with different protection and duplicated switch logic. They should share one helper on `deskflow::Screen`, bracketed consistently — this would also give I2's marker a single place to live.

---

## Findings summary

### Critical

- **C1 — Cross-thread injection race**: `CoordinationKeyForward` is dispatched with `DeliverImmediately` on detached mesh network threads, running `Server::relayForwardedKey` → `MSWindowsKeyState`/`MSWindowsDesks` concurrently with the event loop; unsynchronized state, broken single-caller desk handshake, and possible Down/Up reordering across per-keystroke threads. (`Coordinator.cpp:614-617`, `EventQueue.cpp:194-196`, `CoordinationMesh.cpp:315-324`, `MSWindowsDesks.cpp:351-357`, `891-900`, `Server.cpp:1853-1871`)
- **C2 — Blocking per-keystroke TCP connect inside WH_KEYBOARD_LL**: relay hook synchronously performs mutex acquisition + connect(700 ms timeout)/send/close per key; adds system-wide keyboard latency, risks silent OS unhook, and defeats event ordering. (`MSWindowsKeyboardRelayMonitor.cpp:88-91`, `Coordinator.cpp:620-689`, `CoordinationMesh.cpp:35`, `218-228`)

### Important

- **I1 — VHID pipe client is dead scaffolding**: not in `PLATFORM_SOURCES`, tests unregistered, `MSWindowsVhidRouting::instance()` never called, and the SendInput choke points never consult it; header comment claims routing behavior that does not exist. Wire it in at `send_*_input` or remove it. (`platform/CMakeLists.txt:24-61`, `unittests/platform/CMakeLists.txt:4-11`, `MSWindowsVhidPipeClient.h:71-73`, `MSWindowsDesks.cpp:84-107`)
- **I2 — No injected-input filtering in the relay hook**: missing `LLKHF_INJECTED`/`dwExtraInfo` check means self-injected keys re-enter routing; correctness rides on fleet-state freshness (3 s heartbeat), leaving a mis-forwarding window. (`MSWindowsKeyboardRelayMonitor.cpp:69-91`, `MSWindowsDesks.cpp:92`)
- **I3 — Parallel live injection paths without client-side arbitration**: desk-thread SendInput and Mouser HID loopback can both be active for one pointer; exclusion depends solely on server-side focus tracking, plus `fakeLocalKey` is a second unrouted `SendInput` call site bypassing the desk thread. (`MSWindowsScreen.cpp:1043-1052`, `1678-1688`, `MouserClient.h:5-8`, `ServerApp.cpp:763`)
- **I4 — Duplicated, inconsistently bracketed relay injection**: `PrimaryClient::injectForwardedKey` uses `fakeInputBegin/End`; `ClientApp::injectRelayedKey` does not; same switch logic duplicated in two layers. (`PrimaryClient.cpp:152-168`, `ClientApp.cpp:385-403`)
- **I5 — Undeclared upward header dependency**: coordination headers include `deskflow/KeyTypes.h` while the library links only `common base`, inverting the declared dependency DAG. (`RelayKeyEvent.h:9`, `KeyboardRescue.h:9`, `coordination/CMakeLists.txt:47`)

### Suggestions

- **S1 — Field initializer typo**: `m_y = 9` (should be 0) in `MSWindowsDesks.h:252`; harmless today because `setShape` overwrites, but it is a landmine.
- **S2 — Duplicate CMake entries**: `CoordinationProtocol.cpp/.h` listed twice. (`coordination/CMakeLists.txt:7-10`)
- **S3 — 30 ms sleep in `deskLeave` stalls the blocked event loop** on every screen leave; consider a timer-based re-center. (`MSWindowsDesks.cpp:635-645`)
- **S4 — Dead routing field**: `KeyboardRouteInput.secondsSinceRelayStart` is populated at both call sites but `routeKeyboard` never reads it — the v1 boot-grace concept (`passKeyToLocalOs`, `KeyboardRelayDecision.h:22-32`) was not carried into the v2 router; either use it or drop it. (`KeyboardRouter.h:36`, `KeyboardRouter.cpp:16-29`, `Coordinator.cpp:658`, `731`)

---

## Recommended sequencing

1. **C1**: remove `DeliverImmediately` from the key-forward event post — one-line change that restores event-loop ownership of injection and fixes ordering on the receive side.
2. **C2**: move mesh send off the hook thread (queue + dedicated sender with a persistent connection).
3. **I2 + I4**: unify the two relay injection sites into one bracketed helper and tag injected input with a `dwExtraInfo` marker checked by the relay hook.
4. **I1**: decide the VHID pipe client's fate — wire in behind `send_*_input` or delete.
5. Remaining Important/Suggestion items opportunistically.
