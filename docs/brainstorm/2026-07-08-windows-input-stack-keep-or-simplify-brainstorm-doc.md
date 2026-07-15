---
date: 2026-07-08
topic: windows-input-stack-keep-or-simplify
type: brainstorm
branch: refactor/fleet-state-hub
vgv_next:
  skill: none
  artifact: docs/brainstorm/2026-07-08-windows-input-stack-keep-or-simplify-brainstorm-doc.md
---

# Windows input stack: what to keep after the server-side chord remap

## Question

After the server-side chord remap (`899de7910`) solved PowerToys-style keyboard
remapping over remote, do we still need the earlier workarounds — the
login-screen core restart and the UIAccess core — or can we simplify / "just run
as admin"?

## Decision

**Keep both.** The chord remap replaced exactly one hack (the PowerToys keyboard
nudge/UIAccess-for-keyboard tension); it does not touch the other two concerns.
No code change.

## The three mechanisms are independent

| Mechanism | Commit | Solves | Replaced by chord remap? |
|---|---|---|---|
| Login-screen SYSTEM restart (core → SYSTEM while `LogonUI` active, back to medium on unlock) | `777cf461f` | Remote **PIN/login** control on the secure desktop | No — unrelated to keyboard |
| UIAccess core (medium + UIAccess on the normal desktop) | `1ff9d027e` | Remote **mouse into elevated windows** (elevated PowerToys/admin apps), bypassing UIPI | No — mouse, not keyboard |
| Removed integrity flip | `ba97b520a` | Eliminated churn/lag/duplicate cores | Already removed; stays removed |
| Server-side chord remap | `899de7910` | Keyboard shortcut remap over remote (your PowerToys table) | This is the replacement |

## Why keep the login-screen restart

- It is the **only** thing that gives remote control of the PIN/login screen.
- Reaching the secure desktop requires **SYSTEM on the Winlogon desktop** — the
  watchdog restart does exactly this. This is the same approach RustDesk and
  TeamViewer use (SYSTEM service attaching to the secure desktop), confirmed by
  reading RustDesk's source.
- **"Run as admin" does not help here** — even full-admin/High integrity cannot
  touch the secure desktop. So admin is not a substitute.
- Cost is bounded: LogonUI is stable, so it's at most one relaunch on lock and
  one on unlock — no churn (that was the old `consent.exe` flip, which is gone).

## Why keep UIAccess (and why it's better than "run as admin")

- Still needed so the remote **mouse reaches elevated windows** (its absence was
  the "mouse dies when PowerToys is elevated" symptom).
- Its only previous downside — the keyboard **double-fire** — is **gone**, because
  keyboard remapping is now server-side and PowerToys is out of the remote loop.
  So UIAccess is now pure upside with no keyboard conflict.
- **medium + UIAccess is the better version of "run as admin":** it gets
  injection into elevated windows *without* making the whole core elevated, which
  is what reintroduces the hook/churn problems we spent the day removing. Running
  the core as plain admin would be a regression.
- Cost: a self-signed fleet code-signing cert, already automated in
  `install-windows.ps1`.

## The one simplification we chose NOT to take

Dropping UIAccess (+ its signing) is viable **only** if remote mouse clicks into
elevated/admin windows are never needed — the tradeoff being "mouse stalls only
while an elevated window has focus." User needs elevated-window mouse (runs
PowerToys elevated), so UIAccess stays.

## Outcome

No changes. Current shipped state on `refactor/fleet-state-hub` is the intended
end state:
- keyboard remap: server-side chord table (`Server.cpp` `kChordRemaps`)
- remote login: SYSTEM-on-LogonUI restart
- elevated-window mouse: medium + UIAccess core (self-signed)
- no integrity flip / no churn
