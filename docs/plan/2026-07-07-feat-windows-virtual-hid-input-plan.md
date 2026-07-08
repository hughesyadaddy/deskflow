---
date: 2026-07-07
type: feat
status: draft
topic: windows-virtual-hid-input
branch: refactor/fleet-state-hub
supersedes_partial:
  - docs/plan/2026-06-30-windows-vhid-uac-injection.md
related_plans:
  - docs/plan/2026-07-07-feat-windows-uac-input-without-breaking-powertoys-plan.md
research_agents:
  - 33fce551-5cb9-4488-97f5-ec11d3922bc5
  - 85e245f2-8b1f-4d6b-9679-51e13414ebc2
---

# Windows virtual-HID input: one hardware-class path for remap + elevated + UAC

## Problem statement

Deskflow injects remote input on tiny11 via `SendInput` (`MSWindowsDesks::send_keyboard_input` / `send_mouse_input`). Software injection cannot satisfy all three fleet requirements at once, because they impose **contradictory integrity orderings**:

| Requirement | Needs the core to be… |
|---|---|
| Mouse/keys reach an **elevated foreground window** (e.g. elevated PowerToys) | **≥** that window (UIAccess/High) |
| PowerToys Keyboard Manager can **remap** the core's keys | **≤** KBM (medium, below KBM's High hook) |
| Reach the **UAC secure desktop / login** | SYSTEM on the Winlogon desktop |

We proved this empirically on tiny11 (2026-07-07):
- Core at **MEDIUM** → elevated PowerToys blocks the mouse (UIPI). 
- Core at **MEDIUM + UIAccess** (shipped fix `1ff9d027e`) → mouse reaches elevated windows, **but** KBM can no longer cleanly remap the core's keys — the user observes a "native vs PowerToys override" double-fire, because the UIAccess-injected keystroke and KBM's remap both land.

There is no single integrity level that wins all three. **The escape is to stop using `SendInput` and inject through a virtual HID device instead.** Hardware-class HID input enters through the kernel raw-input path *below* the UIPI/integrity model:
- delivered to `WH_KEYBOARD_LL`/`WH_MOUSE_LL` hooks (PowerToys) with `LLKHF_INJECTED` **clear** → KBM remaps it like a real keyboard, once, cleanly;
- not a user-mode `SendInput` call → **not UIPI-gated** → reaches elevated windows;
- same class as a physical USB device → reaches UAC consent and (hardware permitting) the login screen.

(Research: [driver landscape](85e245f2-8b1f-4d6b-9679-51e13414ebc2), [UIPI/hook model](33fce551-5cb9-4488-97f5-ec11d3922bc5).)

## Goal

Replace the Windows remote-input injection path with a **signed virtual HID keyboard + mouse**, so a single mechanism delivers: clean PowerToys remapping, input into elevated windows, and UAC-consent clicks — without the integrity flip or UIAccess conflict.

Out of scope / unchanged: the login/lock **PIN** path (already works via the shipped SYSTEM-on-LogonUI relaunch `777cf461f`) stays as-is; a virtual keyboard may be blocked from credential entry by the Jan-2026 hardening anyway, so we do **not** depend on it there.

## Decision: which driver to build on

### Option 1 — Adopt FakerInput (open source)
`github.com/Ryochan7/FakerInput` — a KMDF virtual **keyboard + relative + absolute mouse** HID driver (used by DS4Windows) built for exactly this: "send clean input to any active window — full-screen game, system dialog, or **admin app**," working through elevated processes and UAC because it's hardware-class HID. Ships a client lib for submitting reports.

- **Pros:** purpose-built for the elevated/UAC/clean-input case; keyboard+mouse already modeled; a client API exists; fastest path to a working prototype.
- **Cons:** not guaranteed production-signed for Secure-Boot-on loading (likely still need test-sign or self-attestation); **open HID-stack-stability bug reports**; lightly maintained; adds a third-party driver to the fleet. (ViGEmBus — the well-known production-signed one — is **gamepad-only** and cannot emit keyboard/mouse, so it is not an option here.)

### Option 2 — Finish the in-repo VHF driver — **recommended**
`src/driver/deskflow-vhid/` already implements Microsoft's Virtual HID Framework pattern: `VhfCreate` for a mouse (`g_DeskflowVhidMouseReportDescriptor`) and keyboard, an IOCTL surface (`IOCTL_DESKFLOW_VHID_MOUSE_REPORT` / `_KEYBOARD_REPORT` with 4-byte / 8-byte report structs in `public/deskflow_vhid_ioctl.h`), and a working user-mode client (`deskflow-vhid-bridge-win.exe`, `VhidClient`). It just needs finishing, signing, and wiring into the core.

- **Pros:** our code, maintainable, no third-party stability risk; matches MS docs exactly; report structs + IOCTL + device-discovery already written; same signing effort as adopting FakerInput.
- **Cons:** we write/verify the last mile of the driver and own it.

**Recommendation:** finish the in-repo VHF driver (Option 2). The signing wall is identical either way, and we're already ~80% there with a cleaner, owned codebase. Keep FakerInput as a fallback only if the VHF driver hits an intractable snag.

## Signing (the real gate — same for either option)

From the [driver research](85e245f2-8b1f-4d6b-9679-51e13414ebc2):

- **Test-signing** (`bcdedit /set testsigning on`): free, any cert, HVCI can stay on if signed — but **requires Secure Boot OFF** and shows a desktop watermark. User wants Secure Boot **on**, so this is dev-only.
- **Attestation signing** (Partner Center, EV cert): the only way to load a KMDF driver with **Secure Boot on** and no test mode. ~$250–560/yr EV cert on a hardware token (1-yr renewals as of Feb 2026), free Partner Center submission, **no HLK required**, supports KMDF/VHF. Microsoft re-signs the CAB; the result loads on any retail Win11.
- **WHQL/HLK:** overkill for a 3-box fleet.

Decision needed (open question 1): pay for EV + attestation to keep Secure Boot on, or run these dedicated boxes with Secure Boot off + test-signing.

## Core injection rewrite (the actual integration work — shared by both options)

Today (`src/lib/platform/MSWindowsDesks.cpp`):
- `send_keyboard_input()` / `send_mouse_input()` call `SendInput`.
- `fakeKeyEvent`, `fakeMouseButton`, `fakeMouseMove`, `fakeMouseRelativeMove`, `fakeMouseWheel` post `DESKFLOW_MSG_FAKE_*` to the desk thread, which calls those `SendInput` wrappers.

Target: when a virtual-HID mode is enabled and the device is present, translate those same fake-input events into **HID reports** submitted to the driver (the `VhidClient` IOCTL path already exists in the bridge; lift it into the core or keep the bridge as the submitter). Key sub-tasks:
- Map Deskflow `KeyButton`/VK + modifier state → **HID usage codes** + modifier byte (8-byte keyboard report, up to 6 keys — need roll-over handling).
- Map mouse move/button/wheel → the 4-byte mouse report (relative). Absolute positioning needs an absolute-mouse collection (FakerInput has one; our descriptor is relative-only today — may need an absolute collection for cursor placement).
- Decide the boundary: does `deskflow-core` open the vhid device directly (it's medium/UIAccess in session 1) or keep routing through a helper? The IOCTL needs `FILE_WRITE_ACCESS`; confirm a medium/UIAccess core can open the device interface, else keep a small submitter.
- Keep `SendInput` as a fallback when the driver isn't installed/loaded (gated by a setting), so non-driver machines still work.

## Phases

1. **Driver finish + load.** Complete `deskflow-vhid.sys` (keyboard + mouse VHF children; change the Logitech placeholder VID/PID `0x046D/0xFFFF` to our own). Build with WDK; test-sign; load on tiny11 (Secure Boot off for now). Prove with `deskflow-vhid-bridge-win.exe` that the virtual devices enumerate and inject.
2. **Prove the three payoffs** on tiny11 before deep integration: (a) KBM remaps virtual-HID keystrokes cleanly (no double-fire); (b) input reaches an elevated PowerToys window; (c) UAC consent Yes/No click lands. This is the go/no-go gate.
3. **Core integration.** Route `MSWindowsDesks` fake-input through HID reports behind a setting (`daemon/useVhidInput`, default off); `SendInput` fallback when the device is absent. Add absolute-mouse collection if needed for cursor placement.
4. **Signing for production.** If Secure Boot must stay on: EV cert + Partner Center attestation; wire signtool/CAB submission into the Windows build. Else document the test-signing setup.
5. **Retire the UIAccess workaround.** Once HID input is the path, the core no longer needs UIAccess for the mouse (HID isn't UIPI-gated) — set `daemon/uiAccessCore=false` and confirm the keyboard double-fire is gone and the mouse still reaches elevated windows.

## Success criteria

1. A PowerToys remap fires **exactly once** on remote input (no native-vs-override double-fire).
2. Remote mouse + keyboard reach an elevated foreground window without UIAccess on the core.
3. UAC consent Yes/No is clickable remotely via the virtual device.
4. Login/lock PIN path unchanged and still working.
5. Driver loads on tiny11 with the chosen signing method; `SendInput` fallback still works where the driver is absent.

## Risks

- **Signing / Secure Boot** is the biggest gate (open question 1).
- **Jan-2026 credential hardening (CVE-2026-20824)** may block virtual-keyboard password entry at LogonUI regardless — we don't depend on it (SYSTEM path handles PIN today); validate early, don't scope on it.
- **Absolute vs relative mouse**: current descriptor is relative-only; cursor placement for edge-cross may need an absolute collection.
- **HID keyboard roll-over / modifier state**: the 8-byte report holds 6 keys + modifiers; fast typing / stuck-modifier edge cases need care (FakerInput's open bugs are cautionary).

## Open questions

1. **Secure Boot on (pay for EV + attestation) vs off (free test-signing)** on the fleet?
2. Does a medium/UIAccess `deskflow-core` in session 1 have rights to open the vhid device interface directly, or keep a session-1 submitter helper?
3. Absolute-mouse collection needed for cursor positioning, or is relative motion enough (operator self-corrects, as on the macOS bridge)?
4. Finish our VHF driver (recommended) vs adopt FakerInput — final call before Phase 1.

## References

- Driver landscape + signing research: [agent](85e245f2-8b1f-4d6b-9679-51e13414ebc2)
- UIPI / hook / injected-input model: [agent](33fce551-5cb9-4488-97f5-ec11d3922bc5)
- In-repo driver: `src/driver/deskflow-vhid/` (`device.c`, `hid_descriptors.c`, `public/deskflow_vhid_ioctl.h`), bridge `src/apps/deskflow-vhid-bridge-win/deskflow-vhid-bridge-win.cpp`
- Injection path to rewrite: `src/lib/platform/MSWindowsDesks.cpp` (`send_keyboard_input`, `send_mouse_input`, `fake*`)
- FakerInput: https://github.com/Ryochan7/FakerInput · ViGEmBus (gamepad-only): https://github.com/ViGEm/ViGEmBus
- VHF: https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/virtual-hid-framework--vhf-
- Attestation signing: https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation
- Prior UAC/PowerToys plan (shipped): `docs/plan/2026-07-07-feat-windows-uac-input-without-breaking-powertoys-plan.md`
