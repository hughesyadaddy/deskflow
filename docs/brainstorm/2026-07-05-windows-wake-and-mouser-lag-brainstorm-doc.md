---
date: 2026-07-05
topic: windows-wake-and-mouser-lag
vgv_next:
  skill: plan
  artifact: docs/brainstorm/2026-07-05-windows-wake-and-mouser-lag-brainstorm-doc.md
---

# Windows Wake-on-Edge-Cross and Mouser HID++ Lag Fix

## What We're Building

Two fixes for the Windows (tiny11) experience, both root-cause driven:

1. **Wake on edge-cross.** When the server switches the cursor toward a peer
   whose connection is down (asleep), it fires a per-peer configurable wake
   action: a Wake-on-LAN magic packet and/or a shell command. For tiny11
   (a Proxmox VM whose emulated NIC may not honor WoL), the reliable wake
   command is `ssh proxmox qm wakeup <vmid>`. The peer entry in the existing
   fleet peers config gains optional `mac` and `wakeCommand` fields.

2. **Kill the Mouser HID++ reconnect storm.** Live evidence from tiny11
   (2026-07-05): `deskflow-core` at 0.0% CPU while `Mouser.exe` burns 160%
   CPU in a tight open/timeout/retry loop against the Logitech receiver
   (`PID=0xC537`, continuous `request timeout` + re-open every second in
   `mouser.log`). A process pegging a core while owning a `WH_MOUSE_LL` hook
   starves the hook chain, and every Deskflow-injected cursor move traverses
   that chain — this is the residual "atrocious" lag. The fix lives in the
   Mouser repo (`~/Desktop/Mouser`): exponential backoff on failed receiver
   opens, stop re-probing device indexes that already failed `REPROG_V4`
   discovery, and re-enumerate only on a `WM_DEVICECHANGE` device-arrival
   event instead of polling.

## Why This Approach

**Lag:** We considered a Deskflow-side kill-switch that supervises/restarts a
wedged Mouser, and a Raw Input rearchitecture of Mouser's gesture capture.
Both treat symptoms. The diagnostics point at one concrete bug in Mouser's
reconnect loop; fixing it removes the lag *and* the CPU burn. Deskflow's own
Windows injection path was already made fire-and-forget
(`sendInputMessage`, commit `a4a47534a`) and measures idle-cheap, so no
further Deskflow changes are justified by the evidence. A comparison against
upstream `deskflow/deskflow` master confirmed our only Windows platform
divergence is that injection commit — there is no feedback loop in our fork's
Windows path.

**Wake:** We considered preventing sleep entirely and a manual
`kvmctl wake` command. The user explicitly wants sleep to keep working with
a seamless wake when the cursor crosses the edge, so the automatic per-peer
wake action is the only approach that meets the requirement. Plain WoL alone
is insufficient because QEMU-emulated NICs frequently drop magic packets for
suspended guests; the per-peer command covers the VM case.

## Key Decisions

- **Fix Mouser at the root, no Deskflow safety net:** evidence is conclusive
  (live 160% CPU + log storm); a supervision layer would be YAGNI.
- **Wake trigger point is the server's screen-switch path:** the server
  already knows when it is switching to a peer and whether that peer's
  connection is alive; that is the single hook point.
- **Per-peer wake config (`mac`, `wakeCommand`), both optional:** WoL packet
  sent if `mac` present; `wakeCommand` run if present; nothing happens for
  peers with neither. tiny11 will use `wakeCommand` via Proxmox.
- **Graceful "waking" window:** wake takes seconds; cursor behavior during
  that window (hold at edge vs. cross into a dead screen) must be defined in
  the plan phase.
- **Mouser deploy follows the established cycle:** build from a clean clone
  and install on tiny11 (and macbookpro) exactly as done for the
  move-passthrough fix.

## Open Questions

- Cursor UX while the peer is waking: block the edge until the client
  reconnects, or cross immediately and let input queue?
- Should wake fire on *every* switch attempt to a down peer, or be
  rate-limited (e.g. once per 30 s) to avoid hammering the Proxmox API?
- Where does the `wakeCommand` run from — the elected server only, or any
  machine the cursor leaves? (Server-only is simpler and recommended.)
- Does the Mouser backoff fix also resolve the gesture passthrough
  flakiness observed earlier, or is that a separate follow-up?
