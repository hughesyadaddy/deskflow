---
date: 2026-07-07
type: feat
status: shipped
topic: windows-uac-input-without-breaking-powertoys
branch: refactor/fleet-state-hub
supersedes_partial:
  - docs/plan/2026-06-30-windows-vhid-uac-injection.md
related_brainstorms:
  - docs/brainstorm/2026-06-30-windows-vhid-dual-input-path-brainstorm-doc.md
research_agent: 33fce551-5cb9-4488-97f5-ec11d3922bc5
---

# Drive UAC prompts from Deskflow without breaking PowerToys remaps

> **Outcome (2026-07-07): shipped and verified on tiny11.**
> - **PR 1** `ba97b520a` — removed the secure-desktop integrity flip; core runs medium.
> - Login-screen fix `777cf461f` — core elevates to SYSTEM only while `LogonUI`
>   is active, so remote PIN entry works; verified live.
> - **PR 3** `1ff9d027e` — core launched with a UIAccess token on the normal
>   desktop (signed via a self-created fleet cert). Verified core =
>   `MEDIUM + UIAccess=1`; **user confirmed the mouse works under elevated
>   PowerToys and PowerToys re-grabbed remaps after a reset.**
> Remaining: PR 2 (VHID bridge for in-session UAC consent clicks) is still open
> but no longer urgent — the reported problems are resolved.

## Problem statement

On tiny11 the fleet needs both of these true at the same time:

1. **Remote can click through a UAC consent prompt** (and the lock/login
   screen) — input driven from hackintosh/macbookpro must reach `consent.exe`
   on the Windows **secure desktop**.
2. **PowerToys Keyboard Manager keeps remapping keys** that Deskflow injects on
   the **normal desktop**.

Today's mechanism is the auto-elevate watchdog: when `consent.exe`/`LogonUI.exe`
appears, `MSWindowsWatchdog` kills `deskflow-core` and relaunches it as SYSTEM
with a UIAccess token; when the secure desktop clears it kills and relaunches it
again at medium integrity. The user's report:

> Once we elevate the privilege of DeskFlow, PowerToys stops taking hold of any
> of the keyboard shortcuts I'd like to have. We're trying to elevate and
> de-elevate, but **PowerToys never re-grabs it**.

So the elevate/de-elevate flip is both disruptive (mesh churn, cursor/keyboard
lag — see the consent.exe watchdog-churn incident on 2026-07-07) and leaves
PowerToys permanently broken until something restarts.

## Root cause (corrected model)

Deep research against Microsoft and PowerToys official docs
([research agent](33fce551-5cb9-4488-97f5-ec11d3922bc5)) overturns the
assumption baked into the current code.

The comment in `MSWindowsWatchdog::startProcess()` claims:

```
// a UIAccess token also raises the core's *injected* input above normal
// user-level hooks -- so PowerToys/Keyboard Manager (UIAccess=0) can't see
// deskflow's input.
```

That premise is **incorrect** per the documentation:

- A `WH_KEYBOARD_LL` hook (what PowerToys uses) **is called for injected input**
  (`SendInput`/`keybd_event`). Injected events set `LLKHF_INJECTED`; events from
  a _lower_-IL process also set `LLKHF_LOWER_IL_INJECTED`. Injection from a
  _higher_ IL process does **not** hide the event from a medium-IL hook.
  ([LowLevelKeyboardProc](https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelkeyboardproc),
  [KBDLLHOOKSTRUCT](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-kbdllhookstruct))
- What actually gates a low-level hook is **UIPI**: a non-elevated hook stops
  receiving events only while a **focused window of higher integrity** is in the
  foreground — a function of the _focused window_, not of who injected the input.
  ([PowerToys #15241](https://github.com/microsoft/PowerToys/issues/15241),
  [SO: LL hook and elevated apps](https://stackoverflow.com/questions/52696285/lowlevelkeyboardproc-being-called-for-elevated-applications-when-run-as-non-elev))

Therefore the real reasons PowerToys "never re-grabs" are process/hook-lifecycle
effects of the kill/relaunch flip, not the injector's integrity:

1. **Hook ordering.** "The last hook to be registered receives input first."
   When `deskflow-core` is killed and relaunched, its `WH_KEYBOARD_LL` hook
   re-registers **last** and therefore runs **before** PowerToys. If the core
   consumes/re-emits the event (relay/injection path), PowerToys can be starved
   or see already-transformed input.
   ([About Hooks](https://learn.microsoft.com/en-us/windows/win32/winmsg/about-hooks#hook-procedures),
   [PowerToys KBM devdoc](https://github.com/microsoft/PowerToys/blob/86115a54/doc/devdocs/modules/keyboardmanager/keyboardmanager.md))
2. **Focus/desktop transition residue.** The elevate flip happens around secure
   desktop switches; after returning to the normal desktop the foreground can be
   an elevated window (the thing the user just UAC'd into), so PowerToys — not
   elevated — is correctly UIPI-blocked and stays blocked as long as that
   elevated window has focus.
3. **The secure desktop cannot be reached this way anyway.** UAC prompts render
   on the `Winlogon` secure desktop, which "only Windows processes can access";
   there is **no supported API** to inject there, and a UIAccess token does
   **not** grant secure-desktop access. So the SYSTEM/UIAccess relaunch pays the
   full cost (mesh churn + PowerToys breakage) without reliably solving goal 1.
   ([How UAC works](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/user-account-control/how-it-works),
   [Switch to the secure desktop](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/security/threat-protection/security-policy-settings/user-account-control-switch-to-the-secure-desktop-when-prompting-for-elevation))

## Goal

Split the two concerns onto the two desktops they belong to, and **stop flipping
the core's integrity**:

- **Normal desktop:** `deskflow-core` runs at a single, stable integrity for its
  entire lifetime. PowerToys keeps working because nothing kills/relaunches the
  core and nothing changes hook order mid-session.
- **Secure desktop (UAC/login):** input is delivered by the **VHID bridge** (the
  Phase-1/Phase-2 path already scoped in
  `docs/plan/2026-06-30-windows-vhid-uac-injection.md`), a dedicated SYSTEM
  helper that feeds a signed virtual-HID driver — hardware-class input that the
  secure desktop accepts — while the core stays put.

The auto-elevate kill/relaunch flip is **removed** from the normal-desktop path.

## Chosen approach

Two independent, separately-shippable tracks. They can land in either order; the
fleet gets value from Track A immediately.

### Track A — Stop breaking PowerToys (remove the integrity flip)

Make the core run at **medium integrity for its whole life**, so PowerToys never
loses its hook and the mesh never churns. Track A is a **pure removal**: delete
the secure-desktop-driven elevate/de-elevate machinery from `MSWindowsWatchdog`.
It adds **no new setting** and no manifest change.

> **Critical correctness note.** `daemon/elevate` **defaults to `true`** on
> non-portable Windows (`Settings.cpp`: `return !Settings::isPortableMode();`).
> Track A must **not** "launch the core once at whatever `daemon/elevate` says" —
> that would launch the core _permanently SYSTEM_ on the whole fleet and break
> PowerToys forever, the opposite of the intent. Track A launches the core at
> **medium integrity unconditionally** and stops consulting `daemon/elevate` for
> the core's integrity. (`daemon/elevate`'s only current consumer is the flip
> being deleted — see the Correction below — so Track A retires it; whether the
> settings key is deleted or reserved for a future A2 fallback is open question 4.)

Track A work (all deletion / de-wiring, no new abstractions):

1. Remove the secure-desktop → `StartPending` transition in
   `MSWindowsWatchdog::mainLoop()` (the `wantsElevatedCore()` compare + debounce
   block: `m_pendingElevated`, `m_pendingElevatedSince`,
   `kSecureDesktopDebounceSeconds`).
2. In `startProcess()`: launch the core at medium integrity unconditionally;
   delete the `elevate` computation, the `SetTokenInformation(TokenUIAccess)`
   call, and `m_lastElevated`. Note `getUserToken(elevate=false)` calls
   `m_session.getUserToken()` — see the pre-login regression below.
3. Delete now-unused flip machinery end to end so nothing ships as dead
   plumbing:
   - `wantsElevatedCore()`, `refreshWantsElevatedCore()`, `m_cachedWantsElevated`.
   - `setElevationContext()` + `m_selfName` / `m_coordPort` and the
     `pollLocalFleetStatus` fleet-cursor poll that fed only the elevation
     decision.
   - `m_elevateProcess` and the `elevate` parameter of
     `MSWindowsWatchdog::setProcessConfig()` (it only fed the flip).
   - `DaemonApp.cpp`: the `daemon/elevate` read (`applyWatchdogCommand`, line
     ~80), the `setElevationContext(...)` call, and the `elevate` argument to
     `setProcessConfig`. The now-inert **GUI elevate checkbox** (Advanced tab)
     should be removed or disabled in the same PR so it doesn't imply a
     no-op toggle.
4. Keep `secureDesktopActive()` **only if Track B lands in the same PR**;
   otherwise it becomes a zero-caller function and should be removed with Track A
   and reintroduced by Track B. (See PR split.)
5. Replace the incorrect UIAccess/hook comment with the corrected model so the
   flip is not reintroduced on the false premise.
6. Add the startup log marker `core integrity: medium (secure-desktop flip
   removed)` (this is the one non-deletion line in PR 1 — see success criterion 2).

There is intentionally **no setting to restore the flip** — it is removed, not
gated. Reverting is a code revert, not a config toggle.

> **Correction:** `daemon/elevate`'s only runtime consumer is the flip being
> deleted (`DaemonApp::applyWatchdogCommand` → `setProcessConfig(elevate)` →
> `MSWindowsWatchdog` flip). It does **not** configure the daemon service itself
> (the service is installed as SYSTEM by `install-windows.ps1`/`sc create`,
> independent of this setting). So Track A can retire `daemon/elevate` entirely;
> keep the settings key defined only if a later phase (A2 fallback) reuses it,
> otherwise remove it too.

> **Pre-login regression (must disclose + handle).** Today the `elevate=true`
> path deliberately pulls a token from `winlogon.exe` via
> `getUserToken(elevate=true)`. At the **pre-login / lock screen** there is no
> interactive user, so `m_session.getUserToken()` (the medium path) has no token
> to duplicate and `startProcess()` will fail — today's code reaches the secure
> desktop there precisely *because* it elevates. Removing the flip therefore
> **loses remote reachability of the login screen** until Track B ships, and
> risks a **failed-launch retry loop** at the lock screen (`handleStartError`
> backoff). PR 1 must:
> - Handle "no active user token" as a benign wait (log once, back off, retry
>   when a session appears) — **not** a crit-log spin.
> - Explicitly document the login-screen regression window (normal desktop and
>   in-session UAC dismissal via physical input are unaffected; only remote
>   *pre-login* control regresses until Track B).

### Track B — Reach the secure desktop via VHID bridge (no core relaunch)

This is the already-planned VHID path; this plan **re-commits to it as the only
secure-desktop mechanism** and wires it so the core never dies:

1. Watchdog signals a global event on secure-desktop edges (bridge start/stop)
   and starts `deskflow-vhid-bridge.exe` as SYSTEM in the user session — it does
   **not** touch `deskflow-core`.
2. Core, when `daemon/vhidBridgeEnabled` and the secure-desktop event is
   signalled, **duplicates** mouse/key events to the bridge pipe and skips
   `SendInput` for the secure-desktop window; on the normal desktop it keeps
   using `SendInput`.
3. Requires the signed `deskflow-vhid.sys` driver (Phase 1 exit criterion from
   the 2026-06-30 plan — still unproven on tiny11; must pass `demo-uac`).

**Pipe security (must-fix before shipping Track B).** The bridge runs as SYSTEM
and executes input commands from `\\.\pipe\deskflow-vhid-bridge`. An unrestricted
inbound pipe is a local privilege-escalation vector (any local process could
drive SYSTEM-level input). Requirements:

- Create the pipe with an explicit SDDL/ACL restricting the client to the
  expected medium-IL core account (or use `PIPE_REJECT_REMOTE_CLIENTS` +
  a mandatory-integrity-label ACE), not the default DACL.
- Validate/whitelist the command grammar server-side (`move`/`click`/`key`
  only; reject anything else) — already partially true, make it explicit.
- Single-instance server; reject additional connections while one is active.

### Rejected / fallback

- **Keep auto-elevate flip** — rejected: root cause of both the PowerToys
  breakage and the mesh churn; doesn't even reliably reach the secure desktop.
- **Disable secure desktop policy** (`PromptOnSecureDesktop=0`) — fallback only.
  Moves the UAC prompt to the interactive desktop where A2 UIAccess can click it
  with no driver, but weakens a security boundary fleet-wide. Documented as an
  option, not the default.
- **SYSTEM UI-Automation worker on `Winlogon` desktop** — unsupported/fragile;
  keep as research spike only if the VHID driver can't be signed.

## Architecture

```mermaid
flowchart TB
  subgraph remote [Remote operator]
    R[hackintosh / macbookpro cursor]
  end

  subgraph normal [tiny11 — normal desktop]
    CORE["deskflow-core (STABLE integrity, never relaunched)"]
    PT[PowerToys Keyboard Manager LL hook]
    CORE -->|SendInput| WIN[focused app]
    CORE -.injected input still visible.-> PT
  end

  subgraph secure [tiny11 — secure desktop active]
    BR["deskflow-vhid-bridge.exe (SYSTEM)"]
    DRV["deskflow-vhid.sys (signed KMDF)"]
    SEC["consent.exe / LogonUI.exe"]
    BR --> DRV --> SEC
  end

  subgraph svc [Deskflow service]
    WD[MSWindowsWatchdog]
  end

  R -->|Deskflow protocol| CORE
  CORE -->|pipe: mouse/key when secure desktop up| BR
  WD -->|start/stop bridge on secure-desktop edge| BR
  WD -.->|no integrity flip; core relaunched only on crash/session-change.-> CORE
```

## Key decisions

1. **Core integrity is fixed at medium for the process lifetime.** No
   secure-desktop relaunch, no `daemon/elevate`-driven core integrity. This is
   the single change that fixes PowerToys.
2. **Track A is pure removal** — no new setting, no manifest change. Reverting is
   a code revert, not a config toggle.
3. **Secure desktop is Track B's job only** (VHID bridge). The core never tries
   to reach it and never elevates for it.
4. **A2 (signed UIAccess core) is deferred** behind open question 1 — only pursue
   it if remote clicking of _elevated interactive_ windows proves needed. It
   carries the whole Windows code-signing burden, so it does not gate the
   PowerToys fix.
5. **Correct the misleading `MSWindowsWatchdog` comment** so future work doesn't
   re-introduce the flip on the false premise that UIAccess hides input from
   hooks.
6. **Track B is settings-gated** (`daemon/vhidBridgeEnabled`, default false); its
   SYSTEM pipe must be ACL-restricted.

## User / input-flow analysis (edge cases)

| Scenario                                        | Current (flip)                                          | After this plan                                                                                                                                    |
| ----------------------------------------------- | ------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| Type on normal desktop, PowerToys remap         | Works until first UAC, then dead                        | Always works (core IL never changes)                                                                                                               |
| UAC prompt appears, cursor remote               | Core relaunches SYSTEM; mesh drops; click _maybe_ works | Core untouched; bridge (Track B) clicks; mesh stable                                                                                               |
| UAC prompt, Track B not yet shipped             | as above                                                | Core stays medium; UAC **not** remotely clickable — operator uses physical input or the disable-secure-desktop fallback; **no** PowerToys breakage |
| Return to normal desktop after elevating an app | PowerToys often still blocked (elevated window focused) | Same UIPI limit, but not _caused_ by us; A2 or "run PowerToys as admin" resolves                                                                   |
| Stuck `consent.exe` (2026-07-07 incident)       | Core restart storm                                      | No core restarts at all from secure-desktop detection                                                                                              |
| Rapid UAC open/close flicker                    | Debounced relaunch churn                                | Bridge start/stop only; core stable                                                                                                                |
| Lock/login screen (with Track B)                | Core relaunch SYSTEM                                    | Bridge handles; core stable                                                                                                                        |
| Lock/login screen (**PR 1 only**, before Track B) | Core relaunch SYSTEM reaches it                       | **Regression:** no remote pre-login control; core launch waits for a user token (benign backoff, not a crit spin). Disclosed; recovered by Track B |

Edge cases to cover in implementation:

- PowerToys started **after** the core: hook order still deterministic because we
  no longer re-register the core hook mid-session; document that PowerToys
  re-registers on its own enable/disable.
- Bridge pipe not connected when secure desktop appears: log + no-op, never fall
  back to relaunching the core (explicit non-goal).
- `daemon/uiAccessCore=true` but binary unsigned or not in Program Files:
  UIAccess is silently denied by Windows — detect and warn in GUI/log.

## Implementation phases

### Phase 1 — Track A: remove the integrity flip (fixes PowerToys) — **highest priority, ships alone as PR 1**

Deletion-dominant: removes the flip and all its plumbing. The only additions are
one startup log marker and a no-user-token backoff guard. No new setting, no
manifest change.

- [ ] `src/lib/platform/MSWindowsWatchdog.cpp`
  - [ ] Remove the secure-desktop → `StartPending` transition in `mainLoop()`
        including debounce state (`m_pendingElevated`, `m_pendingElevatedSince`,
        `kSecureDesktopDebounceSeconds`).
  - [ ] `startProcess()`: launch the core at **medium integrity
        unconditionally**; delete the `elevate` computation, `m_lastElevated`,
        and the `SetTokenInformation(TokenUIAccess)` call. **Do not** substitute
        `daemon/elevate` here — it defaults to `true` and would pin the core to
        SYSTEM fleet-wide.
  - [ ] Handle the no-active-user-token case (pre-login/lock screen) as a benign
        wait + backoff, not a crit-log spin (see the pre-login regression note).
  - [ ] Delete now-dead machinery: `wantsElevatedCore()`,
        `refreshWantsElevatedCore()`, `m_cachedWantsElevated`,
        `setElevationContext()` + `m_selfName`/`m_coordPort` and the
        fleet-cursor-host poll that fed only the elevation decision;
        `m_elevateProcess` and the `elevate` parameter of `setProcessConfig()`.
        Audit callers and remove them.
  - [ ] Remove `secureDesktopActive()` and the stale-consent guard here **iff**
        Track B is not in this PR (they become zero-caller); Track B reintroduces
        `secureDesktopActive()` when it needs it.
  - [ ] Replace the incorrect UIAccess/hook comment with the corrected model.
  - [ ] Add the startup log marker
        `core integrity: medium (secure-desktop flip removed)`.
- [ ] `src/apps/deskflow-daemon/DaemonApp.cpp`: drop the `daemon/elevate` read,
      the `setElevationContext(...)` call, and the `elevate` arg to
      `setProcessConfig`.
- [ ] GUI Advanced-tab: remove or disable the now-inert elevate checkbox so it
      doesn't imply a working toggle.
- [ ] `src/lib/common/Settings.h`/`.cpp`: retire `daemon/elevate` (or reserve
      the key per open question 4). **Do not** add `daemon/uiAccessCore` here —
      that belongs to Phase 3.
- [ ] Manifest: **no change** — `deskflow-core.exe.manifest` stays
      `asInvoker uiAccess=false`.
- [ ] Verify on tiny11 (manual, see Testing): type through PowerToys remaps,
      trigger a UAC prompt, dismiss with physical input, confirm PowerToys
      **still** remaps afterward and `deskflow-core`'s PID never changed.

### Phase 2 — Track B: VHID bridge secure-desktop path (no relaunch)

Follows `docs/plan/2026-06-30-windows-vhid-uac-injection.md` Phases 2–3, with the
hard constraint that the watchdog never relaunches the core:

- [ ] Watchdog signals `Global\DeskflowSecureDesktop` on bridge start/stop.
- [ ] Core pipe client to `\\.\pipe\deskflow-vhid-bridge`; duplicate
      mouse/key to the bridge and skip `SendInput` while secure desktop active.
- [ ] `daemon/vhidBridgeEnabled` gates the whole path (default false).
- [ ] Prereq: signed `deskflow-vhid.sys` passes the `demo-uac` proof on tiny11.

### Phase 3 — UIAccess core (self-signed fleet cert) — **THE PowerToys fix, now active (PR 3)**

**Promoted from deferred to the primary fix**, based on live evidence from tiny11
(2026-07-07, logged-in desktop):

| Process | Integrity |
|---|---|
| `deskflow-core` | **MEDIUM** |
| `PowerToys.KeyboardManagerEngine` | **HIGH (elevated)** |
| `PowerToys` (all modules) | **HIGH (elevated)** |

PowerToys runs elevated (High IL); the core injects at Medium. When an elevated
window is focused, UIPI blocks the Medium core's `SendInput` (mouse dies) and the
core cannot inject into PowerToys' own High-IL surfaces. Raising the core to
**UIAccess** bypasses UIPI and runs it effectively at High, making the two
symmetric: the core's input reaches elevated windows, and PowerToys' High-IL
low-level hook still sees and remaps it. No kernel driver, no Secure Boot change.

This coexists with the shipped login-screen SYSTEM path (commit `777cf461f`):
that path already elevates to SYSTEM while `LogonUI` is up; UIAccess only governs
the normal logged-in desktop.

Signing approach — **self-signed, fleet-trusted (zero cost, Secure Boot stays on)**.
UIAccess is user-mode Authenticode; it is unrelated to kernel/Secure Boot signing.
`deskflow.exe`/`deskflow-core.exe` are currently `NotSigned` (verified on tiny11),
which is the only thing blocking UIAccess.

- [ ] Create a self-signed code-signing cert once (kept off the repo); document a
      script to trust it (LocalMachine\Root + TrustedPublisher) on each fleet box.
- [ ] `scripts/build-windows.ps1` / install: `signtool sign` `deskflow-core.exe`
      and `deskflow.exe` with that cert. (Currently no signtool step exists.)
- [ ] `deskflow-core.exe.manifest`: set `uiAccess="true"` (keep
      `level="asInvoker"`); requires signed binary in a secure location
      (Program Files — already satisfied by the install dir).
- [ ] `MSWindowsWatchdog::startProcess()`: when NOT on the login screen, launch
      the core so UIAccess actually sticks. **Caveat to prove first:** a UIAccess
      process launched via the SYSTEM daemon's `CreateProcessAsUser` needs correct
      token handling (the old `SetTokenInformation(TokenUIAccess)` path, or rely
      on the manifest) — Windows silently denies UIAccess if the binary isn't
      signed+in Program Files, or if the token isn't set up right.
- [ ] `daemon/uiAccessCore` (bool, default true on Windows once proven) to gate
      it; log a clear warning when UIAccess was requested but Windows denied it
      (signature/location/token) so a misconfigured box is diagnosable.
- [ ] Verify on tiny11: with elevated PowerToys focused, the remote mouse moves
      AND PowerToys still remaps keys; core PID stable; login-screen PIN still
      works (SYSTEM path unaffected).

**Spike first (low risk):** before wiring the build/signing pipeline, manually
self-sign the current `deskflow-core.exe` on tiny11, flip the manifest, and
confirm the mouse survives elevated PowerToys. That de-risks the
`CreateProcessAsUser`/UIAccess-token caveat before investing in the pipeline.

### Phase 4 — polish

- [ ] Update `.cursor/rules/deskflow-kill-all-restart.mdc` note that the core is
      no longer relaunched on secure-desktop edges. (depends on PR 1)
- [ ] Settings matrix doc. (depends on PR 1)
- [ ] GUI Advanced-tab toggles mirroring macOS Login Bridge — the
      `vhidBridgeEnabled` / `uiAccessCore` toggles only make sense once those
      settings exist, so this half **depends on PR 2 / PR 3**, not PR 1.

## PR breakdown

The technical review recommended splitting (~800–1,200 LOC across the platform
library, settings, build tooling, and GUI). Map to the phases:

| PR       | Scope                                                    | Depends on                                     | Ships value                                      |
| -------- | -------------------------------------------------------- | ---------------------------------------------- | ------------------------------------------------ |
| **PR 1** | Phase 1 — remove the integrity flip (deletion + 1 log marker + no-user-token guard) | nothing                     | **Fixes PowerToys immediately**                  |
| **PR 2** | Phase 2 — VHID bridge secure-desktop path (+ ACL'd pipe) | PR 1 merged **and** signed `deskflow-vhid.sys` | Remote UAC click                                 |
| **PR 3** | Phase 3 — self-signed UIAccess core (**the PowerToys fix**) | PR 1 (shipped); independent of PR 2      | **Mouse survives elevated PowerToys; remaps keep working** |
| **PR 4** | Phase 4 — docs/GUI polish                                | PR 1 (min)                                     | Operability                                      |

PR 1 is the priority and is independently mergeable. PR 3 is deferred behind open
question 1.

## Files touched (anticipated)

- `src/lib/platform/MSWindowsWatchdog.cpp` (remove flip + machinery, fix comment,
  no-user-token guard, log marker) — Phase 1 / PR 1
- `src/apps/deskflow-daemon/DaemonApp.cpp` (drop `daemon/elevate` read,
  `setElevationContext`, `setProcessConfig` elevate arg) — Phase 1 / PR 1
- GUI Advanced-tab elevate checkbox (remove/disable inert toggle) — Phase 1 / PR 1
- `src/lib/common/Settings.h`/`.cpp` (`daemon/uiAccessCore`; retire
  `daemon/elevate`) — Phase 3 (setting add) / PR 1 (retire)
- `src/apps/deskflow-core/deskflow-core.exe.manifest` (+ UIAccess variant) — Phase 3
- `src/lib/platform/MSWindowsScreen.*` / new pipe client — Phase 2
- `scripts/build-windows.ps1` (signtool) — Phase 3
- `scripts/install-windows.ps1` (driver register, unchanged kill-all) — Phase 2/4

## Testing strategy

- **Unit-testable (Track B / A2 only):** the settings layer (`daemon/uiAccessCore`
  default, `daemon/vhidBridgeEnabled` gating) follows the existing `Settings`
  test pattern — add coverage there when those settings are introduced. Pipe
  command-grammar validation in the bridge is unit-testable and should have tests.
- **Track A is deletion of Win32-token/desktop-transition code** that cannot be
  unit-tested off-Windows; it is validated by the manual tiny11 procedure below
  plus the daemon-log assertion (success criterion 2). No new unit tests are
  expected for PR 1 — call this out explicitly in the PR description.
- **Manual tiny11 acceptance (PR 1):** (1) remap a key via PowerToys, confirm it
  works; (2) trigger a UAC prompt, dismiss with physical mouse/keyboard;
  (3) confirm the PowerToys remap still works and `deskflow-core`'s PID is
  unchanged; (4) grep the daemon log for zero secure-desktop relaunches over the
  cycle.
- **Regression guard:** add a one-line startup log marker (below) so success
  criterion 2 is assertable from logs, not just by absence.

## Success criteria

1. PowerToys Keyboard Manager remaps work continuously across a full UAC
   open/dismiss cycle, with `deskflow-core`'s PID unchanged throughout.
2. Over a full UAC open/dismiss cycle the daemon log shows **exactly one**
   `running command` for the core (its original launch) and **zero**
   `forcefully terminated` of the core — asserted positively, not just by
   absence of the old relaunch lines. Add a startup marker such as
   `core integrity: medium (secure-desktop flip removed)` so the mode is
   greppable.
3. Mesh stable across UAC flicker — measured concretely: over a UAC
   open/dismiss cycle, tiny11's log shows **no** `starting client epoch` /
   `following` re-entries and the hackintosh server logs **no** tiny11
   disconnect/reconnect (`client "tiny11" has connected` count unchanged).
4. (Track B) UAC "Yes" is clickable from the remote via the VHID bridge while the
   core stays medium — the recorded `demo-uac` proof; bridge pipe is
   ACL-restricted (LPE check).
5. PR 1 requires no signing and no new setting; A2 (if pursued) documented and
   gated behind `daemon/uiAccessCore`.

## Non-goals

- Injecting into `consent.exe` via `SendInput`/UIAccess (impossible on the
  secure desktop — that's Track B's driver's job).
- Disabling UAC globally.
- PowerToys remaps on the secure desktop.
- Replacing normal-desktop `SendInput` with VHID.
- Making injected input invisible to hooks (not possible, and not wanted).

## Open questions

1. **A1 vs A2 default per fleet:** is remote clicking of _elevated interactive_
   windows (not UAC) needed often enough to justify Windows signing now, or is
   A1 + Track B enough?
2. **Driver signing** for `deskflow-vhid.sys` on the production fleet vs
   test-signing on tiny11 (blocks Track B).
3. **Hook-order guarantee:** confirm empirically that with the core no longer
   relaunched, PowerToys remaps survive a UAC cycle without any PowerToys
   restart (validates the root-cause model).
4. Should `daemon/elevate=true` be **deprecated** once Track B is proven, or kept
   as an explicit "disable secure desktop + UIAccess" fallback? (PR 1 removes its
   only current consumer; decide whether to delete the key or reserve it.)
5. **Pre-login regression acceptance:** is losing remote *pre-login* screen
   control between PR 1 and Track B acceptable for the fleet? If not, PR 1 and
   Track B (or the disable-secure-desktop fallback) must ship together rather
   than PR 1 alone.

## References

- Corrected root-cause research: [research agent](33fce551-5cb9-4488-97f5-ec11d3922bc5)
- [LowLevelKeyboardProc](https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelkeyboardproc) ·
  [KBDLLHOOKSTRUCT](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-kbdllhookstruct)
- [How UAC works](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/user-account-control/how-it-works) ·
  [Switch to the secure desktop](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/security/threat-protection/security-policy-settings/user-account-control-switch-to-the-secure-desktop-when-prompting-for-elevation)
- [UAC: elevate UIAccess apps in secure locations](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/security/threat-protection/security-policy-settings/user-account-control-only-elevate-uiaccess-applications-that-are-installed-in-secure-locations) ·
  [Assistive-tech security overview](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-securityoverview)
- [PowerToys & running as Administrator](https://learn.microsoft.com/en-us/windows/powertoys/administrator) ·
  [PowerToys KBM devdoc (UIAccess note)](https://github.com/microsoft/PowerToys/blob/86115a54/doc/devdocs/modules/keyboardmanager/keyboardmanager.md) ·
  [PowerToys #3255](https://github.com/microsoft/PowerToys/issues/3255) · [#15241](https://github.com/microsoft/PowerToys/issues/15241)
- Prior plan: `docs/plan/2026-06-30-windows-vhid-uac-injection.md`
- Brainstorm: `docs/brainstorm/2026-06-30-windows-vhid-dual-input-path-brainstorm-doc.md`
- Current code: `src/lib/platform/MSWindowsWatchdog.cpp`
  (`secureDesktopActive()`, `wantsElevatedCore()`, `startProcess()`),
  `src/apps/deskflow-core/deskflow-core.exe.manifest`
