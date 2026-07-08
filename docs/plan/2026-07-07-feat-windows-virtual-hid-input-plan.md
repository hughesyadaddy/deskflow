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
- The `SendInput` path is **retained**, not replaced: it's the fallback when the driver is absent, and the shipped SYSTEM-on-LogonUI PIN path still uses it. The new work is one dispatch branch inside the existing desk-thread handler — do **not** build a parallel abstraction layer.

Target: when virtual-HID mode is enabled and the device is openable, translate the same fake-input events into **HID reports**. The hard parts (from technical review [engineering](62790777-dff8-47ac-9dbf-e86f5e48b089)):

### Stateful HID keyboard component (the biggest missing piece)
`SendInput` is stateless per event; a HID boot-keyboard report is **stateful** — every report must carry the full set of currently-pressed usages (`keys[6]`) plus the live modifier bitmask. Build a named `HidKeyboardState` that:
- owns the pressed-key set + modifier byte; adds/removes on each fake key event; emits the full 8-byte report each time;
- maps **from scan code, not VK** — the message already carries the scancode (`LOWORD(msg.lParam)`); use the PS/2 set-1 scancode → HID Usage Page 0x07 table, handling the `0xE0` extended prefix (arrows/nav/right-modifiers) explicitly;
- defines overflow behavior at a 7th simultaneous key (drop, or emit rollover-error `0x01`×6);
- releases cleanly on disconnect/leave (no stuck keys/modifiers — the FakerInput cautionary bug class).

### Mouse: absolute is REQUIRED (was open question 3 — now decided)
`deskMouseMove` is the primary path and uses `MOUSEEVENTF_ABSOLUTE` (streams absolute positions on screen-enter). A relative-only virtual mouse would drift on every crossing and break entry positioning/UAC targeting — a functional regression, not a self-correcting nuisance. Therefore:
- add an **absolute-mouse collection** to the HID descriptor + a matching report/IOCTL (keep the relative collection for the `fakeMouseRelativeMove` game path);
- the report deltas are `char` (−127..127); `fakeMouseMove`/`RelativeMove` produce larger values, so the submit layer must **clamp-and-split** large deltas across multiple reports (absolute avoids this for positioning; relative still needs it).

### Device access boundary (was open question 2 — now decided)
Write an explicit **SDDL on the device interface** granting write to the core's user/session account (INF security or `WdfDeviceCreateDeviceInterface` + `SDDL`). That decision *determines* whether the medium/UIAccess session-1 core can `CreateFileW` the interface directly (preferred — drop the named-pipe hop) vs needing a helper. Do not leave the pipe's default `PIPE_ACCESS_INBOUND` security in place (it just relocates the "who can write" question).

### Injected-input sentinel under HID
HID input has `LLKHF_INJECTED` **clear** by design, so `fakeInputBegin/End` (which today mark Deskflow's own injection via `DESKFLOW_HOOK_FAKE_INPUT_VIRTUAL_KEY`) can't distinguish our HID input. State explicitly what they become under HID (no-op on the HID path; still used by the retained `SendInput` fallback). On a pure client this is expected to be harmless — confirm.

### VID/PID
Change the placeholder `VendorID = 0x046D` (Logitech's registered USB VID — must not ship on a signed device) to a clearly-synthetic, non-impersonating VID/PID.

### Runtime fallback decision tree
Define precisely: setting off → `SendInput`; setting on + device opens (`findDevicePath` **and** `CreateFileW` succeed) → HID; setting on + present-but-open-fails (ACL) → log + `SendInput`. A half-provisioned box must never silently do nothing.

## Testing strategy

The riskiest pieces are deterministic, host-testable logic — do not leave them to manual observation:
- Unit tests for scancode→HID-usage mapping and `HidKeyboardState` (chords, held modifiers, key-repeat, modifier-only reports, 7-key overflow, extended-key `0xE0`, release-all-on-disconnect).
- Delta clamp/split test (e.g. 200 → 127 + 73).
- Soak/fuzz test replaying a captured input stream; assert the report stream ends with all keys/modifiers released (no stuck keys — the FakerInput failure mode).
- Non-functional criterion: no added input latency vs `SendInput`, zero stuck-key events over an N-minute soak (input lag is a prior hard-won concern).

## Phases / PR breakdown

Split into 5 PRs along phase seams ([splitting review](b797eb4c-32c1-4c51-aef0-f8b7e4a8a45a)). Everything defaults off until Phase 5, so all PRs merge safely under test-signing.

- **PR 1 — Driver finish + load + descriptor.** Complete `deskflow-vhid.sys`: keyboard + relative mouse + **absolute mouse** VHF collections; non-impersonating VID/PID; explicit device-interface **SDDL**. Build with WDK; test-sign; load on tiny11 (Secure Boot off). Prove enumeration/injection with `deskflow-vhid-bridge-win.exe`. **Also in PR 1 (de-risk early): submit a trivial signed root-enumerated VHF sample through Partner Center attestation** to confirm the signing path is viable for a root-enumerated software device *before* betting the fleet on Secure-Boot-on (open question 1).
- **PR 2 — HID mapping + submitter (pure logic, unit-tested).** `HidKeyboardState` (scancode→usage, modifier byte, rollover, release-all), mouse clamp/split, absolute mapping. No `MSWindowsDesks` wiring yet. Full unit tests (see Testing).
- **Phase 2 gate (manual, not a PR) — prove the three payoffs** on tiny11: (c) **UAC consent Yes/No click first** (most likely to surprise), then (a) KBM remaps virtual-HID keystrokes cleanly (no double-fire), (b) input reaches an elevated PowerToys window. Go/no-go before PR 3.
- **PR 3 — Core wiring.** One dispatch branch in the desk-thread handler routing fake-input → HID behind `daemon/useVhidInput` (default off); the full fallback decision tree; retained `SendInput`. Resolve `fakeInputBegin/End` semantics under HID.
- **PR 4 — Signing + deploy pipeline (parallelizable with PR 3; blocked on open question 1).** EV cert + Partner Center attestation and signtool/CAB submission, **or** documented test-signing. Driver install/upgrade/uninstall routed through `scripts/install-windows.ps1` (`pnputil`), including a **"disable/uninstall → fall back to SendInput" recovery path** (a kernel fault on a headless fleet box must be recoverable).
- **PR 5 — Retire the UIAccess workaround.** Once HID is proven the path, set `daemon/uiAccessCore=false`; confirm the keyboard double-fire is gone and the mouse still reaches elevated windows. Net deletion.

## Success criteria

1. A PowerToys remap fires **exactly once** on remote input (no native-vs-override double-fire).
2. Remote mouse + keyboard reach an elevated foreground window without UIAccess on the core.
3. UAC consent Yes/No is clickable remotely via the virtual device.
4. Login/lock PIN path unchanged and still working.
5. Driver loads on tiny11 with the chosen signing method; `SendInput` fallback still works where the driver is absent.
6. Cursor lands at the correct absolute position on screen-enter (no drift across crossings) — absolute-mouse collection working.
7. Unit tests pass for scancode→usage mapping, `HidKeyboardState` (chords/modifiers/rollover/release-all), and delta clamp/split.
8. No added input latency vs `SendInput`; zero stuck-key/modifier events over an N-minute soak replay.

## Risks

- **Signing / Secure Boot** is the biggest gate (open question 1).
- **Jan-2026 credential hardening (CVE-2026-20824)** may block virtual-keyboard password entry at LogonUI regardless — we don't depend on it (SYSTEM path handles PIN today); validate early, don't scope on it.
- **Absolute vs relative mouse**: current descriptor is relative-only; cursor placement for edge-cross may need an absolute collection.
- **HID keyboard roll-over / modifier state**: the 8-byte report holds 6 keys + modifiers; fast typing / stuck-modifier edge cases need care (FakerInput's open bugs are cautionary).

## Open questions

1. **Secure Boot on (pay ~$250–560/yr EV + Partner Center attestation) vs off (free test-signing + watermark)** on the fleet? PR 1 validates attestation feasibility for a root-enumerated device early so this can be answered before PR 4.
2. Finish our VHF driver (recommended) vs adopt FakerInput — final call before PR 1.

**Resolved by technical review (were open questions):**
- *Absolute mouse* — **required** (the primary move path is absolute; relative-only regresses cursor sync). Added to the descriptor in PR 1.
- *Device access boundary* — decided by writing an explicit **SDDL** on the device interface; the core opens it directly if the SDDL allows (preferred), dropping the pipe hop.

## Escape hatch (if signing answer is "Secure Boot must stay on and attestation is rejected")

A **hardware HID gadget** (e.g. Raspberry Pi Zero in USB-gadget mode, driven over the network/serial) delivers identical hardware-class keyboard+mouse input with **zero driver signing** and Secure Boot untouched — the price is a ~$15 dongle per box and a small firmware/transport bridge. For a fixed 3-box fleet this is a viable fallback and is why we don't over-invest before PR 1's attestation check.

## References

- Driver landscape + signing research: [agent](85e245f2-8b1f-4d6b-9679-51e13414ebc2)
- UIPI / hook / injected-input model: [agent](33fce551-5cb9-4488-97f5-ec11d3922bc5)
- In-repo driver: `src/driver/deskflow-vhid/` (`device.c`, `hid_descriptors.c`, `public/deskflow_vhid_ioctl.h`), bridge `src/apps/deskflow-vhid-bridge-win/deskflow-vhid-bridge-win.cpp`
- Injection path to rewrite: `src/lib/platform/MSWindowsDesks.cpp` (`send_keyboard_input`, `send_mouse_input`, `fake*`)
- FakerInput: https://github.com/Ryochan7/FakerInput · ViGEmBus (gamepad-only): https://github.com/ViGEm/ViGEmBus
- VHF: https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/virtual-hid-framework--vhf-
- Attestation signing: https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation
- Prior UAC/PowerToys plan (shipped): `docs/plan/2026-07-07-feat-windows-uac-input-without-breaking-powertoys-plan.md`
