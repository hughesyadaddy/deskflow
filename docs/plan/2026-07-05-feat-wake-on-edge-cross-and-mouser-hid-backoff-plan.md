---
title: "feat: wake Windows on edge-cross + fix Mouser HID++ reconnect storm"
type: feat
date: 2026-07-05
vgv_next:
  skill: build
  artifact: docs/plan/2026-07-05-feat-wake-on-edge-cross-and-mouser-hid-backoff-plan.md
---

## Wake Windows on Edge-Cross + Mouser HID++ Backoff - Standard

## Overview

Two independent workstreams, one per repo:

1. **Deskflow — wake on edge-cross.** When the server tries to switch the
   cursor to a fleet peer that is configured but not connected (asleep), fire
   a per-peer wake action: a Wake-on-LAN magic packet if the peer has a `mac`
   configured, and/or a shell command (`wakeCommand`) if configured. For
   tiny11 (Proxmox VM) the wake command is `ssh proxmox qm wakeup <vmid>`
   since emulated NICs often drop WoL packets.

2. **Mouser — kill the HID++ reconnect storm.** `core/hid_gesture.py`
   forces a reconnect after 3 consecutive request timeouts and immediately
   retries with zero delay, producing a tight open/timeout/reconnect loop
   that pegs a core (observed at 160% CPU on tiny11 while `deskflow-core`
   sat at 0.0%). A pegged process that owns a `WH_MOUSE_LL` hook starves the
   hook chain and lags every Deskflow-injected cursor move. Add exponential
   backoff, stop re-probing device indexes that already failed `REPROG_V4`
   discovery, and re-enumerate on device-arrival instead of polling.

Brainstorm: `docs/brainstorm/2026-07-05-windows-wake-and-mouser-lag-brainstorm-doc.md`

## Problem Statement / Motivation

- Crossing the screen edge toward a sleeping Windows machine does nothing;
  the machine's network stack is down so no Deskflow traffic can reach it.
  The user wants sleep to keep working *and* a seamless wake on edge-cross.
- Cursor lag on tiny11 returns whenever Mouser's HID++ layer loses the
  Logitech receiver (device asleep / power-cycled) and enters its retry
  storm. Comparison against upstream `deskflow/deskflow` master confirmed
  the only Windows-platform divergence in our fork is the fire-and-forget
  injection commit (`a4a47534a`) — there is no Deskflow-side feedback loop;
  the lag source is Mouser.

## Proposed Solution

### Workstream A: Deskflow wake-on-edge-cross

**A1. Peer wake config** — `src/lib/coordination/Peer.h`

Add optional fields to `Peer`:

```cpp
struct Peer
{
  std::string name;
  std::string ip;
  std::string lan;
  std::string mac;         // optional: WoL target, e.g. "bc:24:11:xx:yy:zz"
  std::string wakeCommand; // optional: e.g. "ssh proxmox qm wakeup 100"
  ...
};
```

Parse the new fields wherever peers are parsed today (follow the existing
`ip`/`lan` parsing in `Settings`/coordination config; keep both optional and
backward compatible — peers without them behave exactly as before).

**A2. WakeOnLan sender** — new `src/lib/coordination/WakeOnLan.h/.cpp`

Small free function `sendWakeOnLan(const std::string &mac)`: build the
102-byte magic packet (6×0xFF + 16×MAC) and send it as a UDP broadcast to
port 9. Pure, testable MAC-parsing helper
(`parseMacAddress(std::string) -> std::optional<std::array<uint8_t,6>>`)
split out for unit tests.

**A3. Wake trigger hook** — `src/lib/server/Server.cpp`

`Server::getNeighbor` (fleet-topology path, ~line 835) already detects the
exact condition: `peekFleetNeighbor` returned a configured screen name that
is not in `m_clients`. When that happens for the *first* skipped screen
(the direct neighbor, not screens skipped further along the walk), request a
wake for that screen name.

Layering: `Server` must not depend on `coordination`. Post a new event
(`ServerWakePeerRequested` with the screen name, mirroring the existing
`ServerScreenSwitched` pattern in `src/lib/base/EventTypes.h`), and have the
coordination-aware app layer (`AutoModeRunner` / where the Coordinator
lives) subscribe, resolve the name against `m_config.peers` with
`namesEqual()`, and execute the wake action.

**A4. Wake executor + rate limit** — `src/lib/coordination/Coordinator.cpp`

`wakePeer(const std::string &name)`:

- Rate-limit per peer: at most one wake per 30 s (steady-clock timestamp map).
- If `mac` set → `sendWakeOnLan(mac)`.
- If `wakeCommand` set → spawn detached (non-blocking, log the command at
  INFO). Reap the child and log a WARNING with the exit code on non-zero
  exit so a broken SSH key or wrong vmid is visible in the log instead of
  failing silently. Never run on the worker thread synchronously.
- If both `mac` and `wakeCommand` are set, fire both (WoL is harmless).
- Only the elected server executes wakes (clients never fire them).

**A5. Waking-window UX** — no new code

While the peer is asleep, `getNeighbor` keeps returning nullptr/skipping, so
the cursor stays on the current screen — existing behavior. The wake fires
in the background; the user re-crosses the edge once the peer is up (the
cursor does not auto-jump when the client reconnects — that is the
documented, intended behavior). No edge-blocking state machine (YAGNI).

**A5b. Prevent immediate re-sleep on the woken client** — Windows only

Windows treats a machine woken without user input as "unattended" and may
re-sleep it within ~2 minutes. On tiny11 this would undo the wake before
the user's first real input arrives. Mitigate at rollout with
`powercfg /setacvalueindex ... UNATTENDSLEEP` (config-only), or if
insufficient, call `SetThreadExecutionState(ES_SYSTEM_REQUIRED)` once in
the Windows client on server connect. Prefer the config route first.

**A6. Fleet config rollout**

Add `mac`/`wakeCommand` for tiny11 to the peers config on hackintosh and
macbookpro. Determine tiny11's VM id on proxmox and verify
`ssh proxmox qm wakeup <vmid>` resumes a suspended guest (also confirm what
"sleep" maps to for the VM: guest S3 → `qm wakeup`; if the guest powers off
instead, the command is `qm start <vmid>` — pick during rollout, config-only).

### Workstream B: Mouser HID++ reconnect backoff

All in `/Users/alexhughes/Desktop/Mouser`, `core/hid_gesture.py`:

**B1. Exponential backoff in `_main_loop`**

Today: `_try_connect()` failure waits ~5 s, but a *successful* open that
later hits `_CONSECUTIVE_TIMEOUT_RECONNECT` (3) raises `IOError` and loops
straight back into `_try_connect()` with no delay — the storm. Add a
reconnect delay that starts at 1 s and doubles to a 60 s cap whenever a
connection ends due to consecutive request timeouts (device asleep), reset
to 0 on a healthy session (one that survives > 30 s or receives real data).

**B2. Negative-result cache in `_try_connect` (with mandatory TTL)**

Candidates that opened but failed `REPROG_V4` discovery
(`"REPROG_V4 was not found"` path, ~line 2910) are re-probed on every
attempt, each probe costing multiple HID++ request timeouts. Cache the
candidate signature as known-bad and skip it. **The cache must expire on a
TTL (5 min), not only on device-change events**: when a Logitech mouse
sleeps, the receiver stays enumerated and its wake fires *no*
`WM_DEVICECHANGE`, so an event-only cache would blacklist the device
forever.

**B3. Device arrival clears cache and interrupts backoff**

On a `WM_DEVICECHANGE` device-arrival (Mouser's existing device watcher),
clear the negative cache and set a flag that the backoff sleep checks each
0.1 s slice so reconnect happens immediately — a device plugged in mid-way
through a 60 s backoff must not wait out the timer.

**B4. Deploy**

Build from a clean clone and install on tiny11 (and macbookpro if its
Mouser is affected) using the established build/install/sign cycle
(`scripts/build_and_install.py`), same as the move-passthrough fix.

## Technical Considerations

- **Layering:** `Server` (server lib) communicates the wake request via the
  event queue only; coordination code owns peer config and execution. Same
  decoupling pattern as `ServerScreenSwitched`.
- **Security:** `wakeCommand` is an arbitrary shell command from the local
  config file — same trust level as the rest of `Deskflow.conf`; log the
  command at INFO when executed. WoL packets are unauthenticated broadcast
  by design.
- **Performance:** wake path only runs on edge-cross to a *disconnected*
  peer, rate-limited; zero cost on the hot cursor path.
- **Anti-cheat:** no changes to input injection in either workstream;
  nothing new for Ricochet to see.
- **Mouser threading:** backoff must remain interruptible by
  `self._running` and `_deskflow_attach` (mirror the existing 0.1 s-slice
  sleep loop at line 2935).

## Acceptance Criteria

Workstream A (Deskflow):

- [ ] `Peer` parses optional `mac` and `wakeCommand`; peers without them are unaffected
- [ ] `parseMacAddress` unit tests: valid forms (`:`/`-` separators), invalid strings rejected
- [ ] Magic-packet builder unit test: 102 bytes, correct layout
- [ ] Edge-cross to a disconnected fleet neighbor posts `ServerWakePeerRequested` exactly once per crossing attempt (ServerTests)
- [ ] Coordinator wake is rate-limited to one per peer per 30 s (unit test with injected clock or timestamp override)
- [ ] Clients (non-server role) never execute wake actions
- [ ] Live: crossing toward sleeping tiny11 wakes it and the client reconnects without restarting anything

Workstream B (Mouser):

- [ ] Reconnect after consecutive-timeout disconnect backs off 1→2→4…→60 s (pytest with mocked connect)
- [ ] Candidates that failed `REPROG_V4` discovery are skipped while cached and re-probed after the 5 min TTL expires (pytest)
- [ ] Device-arrival event clears the cache and interrupts an in-progress backoff sleep within ~0.1 s (pytest)
- [ ] Healthy session resets backoff
- [ ] A sleeping mouse that wakes (no device-change event) reconnects within one TTL window
- [ ] Live on tiny11: put the Logitech device to sleep / unplug receiver → Mouser CPU stays < 5%, no log storm; cursor stays smooth while Mouser is degraded
- [ ] Gestures still work after device wakes (reconnect completes)

## Success Metrics

- Mouser CPU on tiny11 stays under 5% with the receiver absent/asleep
  (previously 160%).
- Injected cursor movement on tiny11 remains smooth during a Mouser
  degraded state (subjective check + no hook-timeout events in system logs).
- A sleeping tiny11 wakes within a few seconds of an edge-cross with no
  manual action.

## Dependencies & Risks

- **Proxmox wake semantics:** if tiny11's "sleep" suspends the VM at the
  hypervisor level rather than guest S3, `qm wakeup` may not apply —
  rollout step A6 verifies and picks `qm wakeup` vs `qm resume`. Do **not**
  use `qm start` as the wake command: it would boot the VM even when it was
  intentionally shut down, which is not a "wake".
- **Windows unattended re-sleep:** a machine woken without user input may
  re-sleep within ~2 min (see A5b) — must be handled or the wake feels
  broken.
- **SSH from server to proxmox:** `wakeCommand` assumes non-interactive key
  auth from the elected server host to proxmox; must be verified on both
  hackintosh and macbookpro since either can be elected.
- **Mouser frozen build:** the fix only takes effect after a full
  rebuild/reinstall on tiny11 (the installed app is a frozen exe; patching
  py files in place does nothing).
- **Event-loop coupling:** the wake event fires from inside the server's
  locked `getNeighbor`; posting to the event queue is safe (existing
  pattern), but do not execute the wake synchronously there.

## References & Research

- Skip-disconnected-neighbor hook point: `src/lib/server/Server.cpp:826-843`
- Peer struct: `src/lib/coordination/Peer.h:19-27`
- Event decoupling pattern: `ServerScreenSwitched` in `src/lib/base/EventTypes.h`, consumed in `src/lib/deskflow/ServerApp.cpp`
- Mouser storm loop: `Mouser/core/hid_gesture.py:2927-2968` (`_main_loop`, `_CONSECUTIVE_TIMEOUT_RECONNECT`)
- Mouser REPROG_V4 negative path: `Mouser/core/hid_gesture.py:2910-2925`
- Live evidence (2026-07-05): `Mouser.exe` 160.6% CPU, `deskflow-core` 0.0%; `mouser.log` continuous `request timeout` + re-open of `PID=0xC537`
- Upstream diff check: only Windows-platform divergence from `deskflow/deskflow` master is commit `a4a47534a` (fire-and-forget injection)
- Prior related fix: Mouser `WM_MOUSEMOVE` passthrough in `core/mouse_hook_windows.py`
