---
date: 2026-06-30
topic: windows-vhid-dual-input-path
vgv_next:
  skill: plan
  artifact: docs/brainstorm/2026-06-30-windows-vhid-dual-input-path-brainstorm-doc.md
---

# Windows dual input path: VHID at login, SendInput + PowerToys on desktop

## What We're Building

Remote KVM on tiny11 where:

- **Logged-in desktop:** `deskflow-core` stays medium integrity, injects via `SendInput`,
  PowerToys/Mouser hooks work.
- **Login screen / UAC:** same core stays running (mesh stable); a SYSTEM
  `deskflow-vhid-bridge.exe` feeds `deskflow-vhid.sys` so input reaches the secure
  desktop like a physical USB keyboard/mouse.

**Not building:** one input path for both worlds, or PowerToys remaps on the secure
desktop.

### Current state (tiny11, 2026-06-30)

| Setting | Value |
|---------|-------|
| `daemon/elevate` | `true` |
| `daemon/vhidBridgeEnabled` | unset → `false` |
| VHID driver | unknown — must verify install |
| Phase 2 core→pipe | **not built** |

Login fails today because auto-elevate relaunch crashes (`process immediately stopped`
in `%ProgramData%\Deskflow\deskflow-daemon.log`), not because VHID is active.

## Why This Approach

### Two input planes (do not merge)

| Path | When | PowerToys? |
|------|------|------------|
| Core → `SendInput` | Normal desktop | **Yes** |
| Bridge → VHF driver | `LogonUI.exe` / `consent.exe` | **No** — hardware-class HID |

PowerToys and VHID are not incompatible; they operate on different paths. The product
decision is **which path is active when**.

### macOS precedent

| macOS | Windows (target) |
|-------|-------------------|
| User-session core | Medium core, always |
| LoginWindow bridge agent | Watchdog starts bridge on secure desktop |
| Karabiner virtual HID | `deskflow-vhid.sys` |
| Bridge = TCP client to mesh | Bridge = **pipe client of core** (simpler on Windows) |

## Key Decisions

1. **Do not elevate `deskflow-core` for login/UAC** in the target architecture.
2. **Dual path by desktop type** — SendInput on normal desktop, VHID on secure desktop only.
3. **Phase 2 (core → named pipe) is the single blocking engineering task** for login KVM.
4. **Secure-desktop signal:** watchdog sets a **named global event** when bridge
   start/stop transitions (same pattern as existing `kSendSasEventName` in
   `Constants.h.in`). Core reads `daemon/vhidBridgeEnabled` from settings and, when
   the event is signaled, routes input to `\\.\pipe\deskflow-vhid-bridge`.
5. **Relative mouse only on secure desktop** (match macOS login bridge). Operator
   steers visually; no absolute positioning on login.
6. **Production settings after Phase 2:**
   ```ini
   [daemon]
   elevate=false
   vhidBridgeEnabled=true
   ```
7. **Mouser / HID++ on secure desktop:** out of scope v1.

## Approaches Considered

### A. Named pipe: core forwards to bridge — **Chosen**

Core mirrors mouse/key to existing bridge `pipe` protocol (`move`, `click`, `key`).
Watchdog already starts bridge as SYSTEM.

- Pros: Reuses Phase 1; core keeps mesh; smallest delta
- Cons: Requires secure-desktop event + pipe client in platform layer

### B. Bridge as standalone mesh client — **Rejected**

Duplicate macOS TCP bridge on Windows.

- Cons: Second mesh participant, coordination/TLS duplication

### C. Fix auto-elevate only — **Fallback only**

Debug elevated-core crash and keep relaunch model.

- Cons: Mesh churn; PowerToys dead on secure desktop; not fleet target

## Success Criteria

1. Normal desktop: remote KVM + PowerToys remaps work.
2. Lock/login: remote pointer moves and clicks password field; **one** `deskflow-core`
   process throughout (no relaunch).
3. UAC: remote Yes/No click via VHID.
4. Mesh epoch stable across UAC open/close.
5. Driver loaded before VHID testing.

## What To Do Now (interim playbook)

Until Phase 2 ships, these are **separate** steps — combining settings without Phase 2
gives login KVM with no input.

| Step | Action | Why |
|------|--------|-----|
| 1 | Set `elevate=false` in Deskflow.conf | Stops elevated-core crash loop killing daemon |
| 2 | Rebuild/reinstall; Stop → Start Deskflow | Reload daemon config |
| 3 | Verify normal-desktop KVM + PowerToys | Confirms SendInput path still good |
| 4 | Install driver (`build-vhid-driver.ps1`, `install-vhid-driver.ps1`) | Bridge needs `deskflow-vhid.sys` |
| 5 | Manual Phase 1 test: trigger UAC, run `deskflow-vhid-bridge.exe demo-uac` | Proves driver reaches secure desktop |
| 6 | **After Phase 2:** set `vhidBridgeEnabled=true` | Bridge lifecycle + forwarding together |

Do **not** enable `vhidBridgeEnabled=true` expecting full remote login until Phase 2
lands — bridge starts but receives no KVM from core.

## Implementation Phases

| Phase | Status | Deliverable |
|-------|--------|-------------|
| 0 | Done | Watchdog debounce |
| 1 | Partial | Driver + bridge `test`/`pipe`/`demo-uac` |
| 3 | Done | `vhidBridgeEnabled` + watchdog bridge lifecycle |
| **2** | **Not started** | Secure-desktop event + core pipe client |
| 4 | Not started | Installer driver, bridge log, GUI driver status |

### Phase 2 scope (for `/plan`)

1. Watchdog: signal `Global\\DeskflowSecureDesktop` (name TBD) on bridge start/stop.
2. `MSWindowsScreen` (or thin helper): pipe client to `\\.\pipe\deskflow-vhid-bridge`.
3. When `vhidBridgeEnabled` + secure desktop active: duplicate
   `mouseRelativeMove`, button, key events to pipe; skip `SendInput` on secure desktop.
4. Bridge failure: log error; **do not** auto-fallback to elevate in v1 (explicit log +
   user toggles `elevate=true` manually if needed).

## Non-Goals

- PowerToys remaps on login/UAC screen
- Mouser gestures on secure desktop
- VHID on normal logged-in desktop
- Disabling UAC policy globally

## Open Questions

1. **Does VHF reach `consent.exe` / `LogonUI.exe` on tiny11?** Must pass manual
   `demo-uac` test before investing in Phase 2.
2. **Elevated-core crash root cause** (fallback path only): capture stderr from elevated
   launch, check SYSTEM profile loading wrong config
   (`C:/WINDOWS/system32/config/systemprofile/...` appeared in daemon log), verify
   coordination port conflict when two cores start in same second.
3. **Driver signing** for production fleet vs test-signing on dev box.
4. **GUI warning** when `vhidBridgeEnabled=true` but driver device not present.
