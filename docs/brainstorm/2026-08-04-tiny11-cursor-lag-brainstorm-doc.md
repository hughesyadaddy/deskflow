# tiny11 cursor lag with WinStream — evidence and conclusion

## Symptom
Cursor lag on tiny11 (Windows 11, Proxmox guest) whenever WinStream runs.
Killing WinStream removes it; Mouser is innocent; a Release rebuild of
WinStream did not help.

## Instrumentation
Deskflow's desk thread logs once a second:
`move latency: N moves/s, worst queue age Xms, worst coalesced Y`
where queue age = time from the core POSTING a move to its desk thread
until `SendInput` actually runs. Both ends are inside Windows, after the
network hop — so the network cannot appear in this number.

## Measured (2026-08-04, WinStream running, lag present)

| moves/s | worst queue age | worst coalesced |
|---|---|---|
| 40 | 32 ms | 4 |
| 37 | 47 ms | 4 |
| 19 | 218 ms | 22 |
| 11 | **250 ms** | 22 |
| 14 | 110 ms | 12 |

**The relationship is inverse.** High throughput ⇒ low latency. Low
throughput ⇒ huge latency AND deep coalescing. A queue that is merely
overwhelmed shows the opposite (throughput pinned at max, latency rising).
This signature — few injections per second, each waiting 100–250 ms, with
20+ moves collapsed per injection — means **the desk thread is frozen for
100–250 ms at a stretch** and drains a backlog when it wakes.

## Ruled out, with data
- **Network** — the metric is measured entirely within Windows, post-network.
- **Duplicate processes** — exactly one core/daemon/GUI/Mouser.
- **Kernel driver latency** — DPC 0.5 %, interrupt 1.2 % (rules out the
  WinStream virtual audio driver).
- **Guest CPU exhaustion** — WinStream ≈ 24 % of one core on a 10-vCPU guest;
  privileged time 20.6 %.
- **A WinStream mouse hook** — no `SetWindowsHookEx` / `WH_MOUSE_LL` /
  `SetWinEventHook` / raw-input registration anywhere in its source.
- **Mouser** — restored alone, lag did not return.
- **Host vCPU starvation (mostly)** — host `schedstat` for tiny11's vCPU
  threads shows average run-queue wait of ~3 µs/slice; P-core-pinned vCPUs
  wait 0.05–0.5 % of runtime. Nowhere near 250 ms. *Caveat: averages hide
  bursts, and the four E-core-pinned vCPUs wait 1.3–2.9 %, ~20× the P-core
  ones — worth revisiting if H1 fails.*

## Competing hypotheses

**H1 — `SendInput` is blocking (recommended).**
Windows serializes injected input against the system input queue, and gives
every low-level hook up to `LowLevelHooksTimeout` (default **300 ms**) before
timing it out. Our stalls top out at ~250 ms — suspiciously just under that
ceiling. Some hook or input-queue owner on the machine stalls injection while
WinStream is active. WinStream installs no hook itself, but it is a
WindowsAppSDK app with 45 threads and a virtual audio driver; a dependency or
the compositor could own the block.
*Fits:* the 250 ms ceiling, the burst pattern, why guest CPU looks idle.

**H2 — bursty host descheduling.**
The host is oversubscribed (macOS VM at 645–1400 %, load 7–10). Average
scheduling latency is microseconds, but a tail event could freeze a vCPU.
*Fits:* why killing WinStream helps (less demand). *Against:* magnitude —
nothing in schedstat approaches 250 ms.

## Discriminating measurement (next step)
Time the `SendInput` call **itself** inside the desk thread, and log it
alongside the existing queue age:

- `SendInput` duration ≈ 200 ms ⇒ **H1**: injection is blocking. Then
  enumerate what else hooks input, and consider lowering
  `HKLM\...\Desktop\LowLevelHooksTimeout`.
- `SendInput` fast (µs) but queue age still 250 ms ⇒ **H2**: the thread was
  not scheduled. Then measure per-slice scheduling tail on the host
  (`perf sched latency`) and reduce host oversubscription.

This is a ~10-line probe in the same place as the existing one and settles it
definitively rather than by elimination.

## CONCLUSION (2026-08-04, proven)

`SendInput` duration tracks queue age one-for-one:

| worst queue age | worst SendInput |
|---|---|
| 171 ms | 187 ms |
| 78 ms | 76 ms |
| 110 ms | 109 ms |
| 109 ms | 110 ms |

**H1 confirmed, H2 eliminated.** The desk thread is not starved — it blocks
INSIDE `SendInput` for 76–187 ms per injected move. The "queue age" of the
next move is simply how long the previous call blocked; the thread is a
serial loop, so one slow injection ages everything behind it.

Mechanism: `SendInput` waits on every low-level hook in the system, bounded
by `LowLevelHooksTimeout` (default 300 ms — our worst case sits just under
it). Something installs a hook that stalls while WinStream is active.
WinStream's own source has no hook, so the owner is a dependency, a driver
it loads, or another process whose behaviour changes when WinStream runs.

### Next actions (in order)
1. **Identify the hook owner.** Enumerate processes/DLLs that install
   `WH_MOUSE_LL` / `WH_KEYBOARD_LL` while WinStream runs (a kernel debugger
   or `!hooks`-style inspection; the API does not expose owners directly).
   Compare the set with WinStream stopped.
2. **Bound the damage regardless of owner.** `HKCU\Control Panel\Desktop`
   → `LowLevelHooksTimeout` (DWORD, ms). Lowering it to ~20 ms makes Windows
   evict a stalling hook quickly instead of letting it hold injection for
   300 ms. Well-established mitigation, reversible, no code change.
3. Only then revisit WinStream itself — the fix may be to stop it installing
   or provoking that hook rather than to tune around it.

## Decisions
- Keep the move-latency probe until the mechanism is proven.
- Do not tune affinity/priority further until the discriminator runs — the
  earlier experiments were confounded (scheduled-task launches silently
  demoted the apps to BelowNormal).
- Built-in priority (core + Mouser at ABOVE_NORMAL) stays: correct
  regardless of which hypothesis wins.
