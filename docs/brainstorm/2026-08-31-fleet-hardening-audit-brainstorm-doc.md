# Fleet hardening audit — signing, capitalization, hotkey leakage

Ten parallel read-only audits, zero assumptions, every claim carrying file:line
or live-machine evidence. Three reported symptoms; the audits found one
systemic defect underneath most of them.

## THE SYSTEMIC DEFECT

**Blocking work inside an OS input callback.** Found in three independent
places, each producing "input misbehaves under load":

| Where | What blocks | Consequence |
|---|---|---|
| Mouser, Windows | Python/GIL in `WH_MOUSE_LL` | 76-187ms per event; froze all system input (measured) |
| Deskflow, macOS | 700ms mesh connect inside `CGEventTap` | tap disabled by timeout -> every key leaks locally |
| Deskflow, Windows | 700ms mesh connect inside `WH_KEYBOARD_LL` | exceeds the 300ms `LowLevelHooksTimeout`: Windows delivers the key anyway AND silently removes the hook |

The OS gives an input callback a few hundred milliseconds. Any network I/O,
lock, or interpreter inside one is a latent system-wide input stall. **Fixing
this class is the single highest-value change in this document.**

---

## SYMPTOM 1 — Deskflow does not auto-start; permissions break (macOS)

**Root cause: macbookpro builds ad-hoc signed, and macOS keys privacy grants to
the signature.**

- `build/CMakeCache.txt:18` on macbookpro holds `APPLE_CODESIGN_DEV=-`.
  `-` is `codesign`'s ad-hoc identity **and is CMake-truthy**, so every
  `if(APPLE_CODESIGN_DEV)` guard reads it as "identity configured".
- `scripts/fleet-deploy-macos.sh:61` never reconfigures when a cache exists, so
  the correct identity is resolved every deploy and **discarded**.
- arm64 linkers ad-hoc sign their own output, so `codesign --verify --deep
  --strict` returns **exit 0** and the deploy prints "verify OK".
- `scripts/install-macos.sh:149` greps for `Authority=|TeamIdentifier=` and ends
  in **`|| true`** - the one check that would have caught it.
- Two `else() set(... "-")` branches (`CMakeLists.txt:210`,
  `deploy/mac/deploy.cmake:14`) silently downgrade; the `execute_process`
  codesign calls have no error check.

**Proven consequence** (TCC databases decoded on both machines):

| Machine | Stored requirement | Survives rebuild? |
|---|---|---|
| hackintosh | `identifier ... and certificate leaf[subject.CN] = "Apple Development: Duff Hughes"` | yes |
| macbookpro | `cdhash H"5563a6cc..."` | **no - dies every build** |

macbookpro's grant was created *today* - manually re-granted. macOS keeps the
stale row, **System Settings shows the toggle ON**, and the app is denied. TCC
will not re-prompt while a row exists, so Deskflow's own remediation is a no-op.

Secondary: **neither Mac has an Input Monitoring grant for the current bundle
id** - only for the retired `org.deskflow.deskflow`. No code calls
`IOHIDCheckAccess`/`IOHIDRequestAccess`, so nothing detects it.

Also: on macbookpro the helper binaries carry linker-generated identifiers
(`deskflow-core-555549444cb8...`) derived from `LC_UUID`, so their signing
identity itself changes every build.

**Invariant to enforce:** every Mach-O in the bundle, on every machine, every
build, signed `Apple Development: Duff Hughes (MVDT65NPA4)` / team
`J5KPG8ZR5C`, with a stable identifier. Signing failure must be a hard error.

Note SSH cannot use the login keychain at all (`User interaction is not
allowed`). Mouser already solves this with `scripts/build_macos_gui_session.py`
- whose header literally warns against the ad-hoc workaround - and
`fleet-deploy-macos.sh:91` bypasses it. **Mouser is the control group: same
fleet, same machines, correctly signed on both, permissions stable.**

### Auto-start specifically: the repair is unreachable

`deskflow-gui.cpp:152-154` exits the app (`return 1`) when Accessibility is not
granted - **before `MainWindow` is constructed**, and the SMAppService repair
lives inside that constructor. On an ad-hoc machine the lost AX grant therefore
closes the gate in front of its own fix. Registration failure is silent:
`NSLog` only (never reaches the app logger or a dialog), return value
discarded, and `LoginItemConfigured` saved `true` regardless.

Measured on hackintosh: login 08:37, GUI launch **08:39:27** (2m17s later),
core 08:39:28. "Late" reads as "never" if you check early. The BTM record shows
`Generation: 1` - registered once and surviving every reinstall, which
**disproves the comment claiming bundle replacement invalidates registration**;
signature churn is the real invalidator.

Architecturally: **no daemon is built on macOS** (`deskflow-daemon/CMakeLists.txt:7`
is `if(WIN32)`), so the chain is BTM item -> GUI -> AX gate -> core, with a
modal dialog inside a hidden tray app. Mouser already does the right thing on
the same machine: a per-user LaunchAgent with `RunAtLoad`+`KeepAlive`, which
needs no BTM approval, survives bundle replacement, and is restartable via
`launchctl kickstart -k`. The core should be owned that way; the GUI login item
should be convenience, not load-bearing.

Also: macOS 26.2 can corrupt per-bundle-id status-item layout, and `autoHide=true`
is set - so a fully working instance can be invisible and read as "didn't start".

## SECURITY — act on this

`scripts/fleet.env` holds `FLEET_KEYCHAIN_PASSWORD_hackintosh` and
`_macbookpro` (login-keychain passwords) in **cleartext**, with a stale
`fleet.env.bak-20260814` beside it. Correctly gitignored and untracked, but two
audit agents read it, so treat both as exposed: **rotate them**, delete the
backup, and move the secret into the login keychain rather than a dotfile.
Note the unlock they enable does not even work over SSH (`User interaction is
not allowed`), so the risk currently buys nothing.

## SYMPTOM 2 — Capitalization fails, needs a keyboard replug (macOS)

Five independent defects. Any one alone breaks capitalization.

1. **Self-certifying stuck state.** `OSXKeyState.cpp:625` posts modifiers with
   `kIOHIDSetGlobalEventFlags` - Deskflow's caps belief is written into
   IOHIDSystem's system-wide flags. `m_capsPressed` is never seeded from the OS
   and has no reset path; `m_mask` reseeds from `GetCurrentKeyModifiers()`,
   which returns 0 without a WindowServer session (i.e. at the login window).
   Once they disagree, `KeyMap.cpp:825` computes `flipMask == 0` and emits **no
   correction, forever**. Only HID re-enumeration clears the kernel side -
   which is exactly what replugging does.
2. **A macOS target never turns caps OFF.** The injector is level-triggered:
   a relayed caps Down sets `m_capsPressed = true`; a second Down (meant to turn
   it off) sets it `true` again. The relay sends **one Down per state change and
   no Up** - correct for Windows (toggles on key-down) and permanently broken
   Mac-to-Mac. Two source platforms, two wire contracts, one receiver.
3. **The designed resync point is a no-op.** The server computes and transmits
   the authoritative toggle mask on **every** screen entry
   (`Server.cpp:758/1064/2751`); `Screen::enterSecondary` is `// do nothing`,
   the vhid bridge's `on_enter` reads only x/y, and **no `setToggleState`
   exists anywhere**. The one mechanism that would self-heal caps drift is
   plumbed end-to-end and unimplemented in the last inch.
4. **Login window: the caps read fails silently.** `target_caps_lock_on()` opens
   IOHIDSystem, which returns `kIOReturnNotOpen` at the login window (27 of 28
   runs in the live log). It falls through to `return false` with no log, so
   the caps inversion is **dead code exactly where it is needed**. The relayed
   caps keypress meanwhile latches the virtual keyboard ON, and
   `IOHIDSetModifierLockState` is **called nowhere in the tree** - the bridge
   can read caps but never write it.
5. **Nothing resyncs on wake.** `stopServer()` keeps the screen alive, so
   `Screen::enable()` - the only caller of `updateKeyState()` - never re-runs.
   The relay's caps tracker is seeded only in `start()`, and since the tap
   survives sleep, `running()` stays true so the reconciler never restarts it.
   A stale seed makes the next caps press look like a no-op and be **swallowed
   entirely**. Windows resyncs at its boundaries; macOS has no equivalent.

Plus: **commit-before-confirm.** The caps tracker commits before the send. A
dropped packet does not skip a toggle - it leaves the two machines permanently
**anti-phase**, and nothing can re-align them. And **Shift+Caps resolves
differently per platform** (`ToUnicodeEx` vs `UCKeyTranslate`), so the same
physical keypress relays as `a` from Windows and `A` from a Mac.

**Correct design: level-triggered, absolute state.** Every relayed key already
carries the sender's true caps bit; the receiver discards it for the caps key.
Treating a caps event as "set caps to this state" rather than "press caps"
makes the protocol self-healing, costs one keystroke per dropped packet instead
of forever, and deletes the tracker plus eight of its divergence paths.
On macOS, `IOHIDSetModifierLockState` is the idempotent write that makes this real.

## SYMPTOM 3 — Hotkeys fire on both machines (Ctrl-Alt / Wispr Flow)

**The swallow code is correct.** Modifiers are not exempt on either platform.
Three real causes, in order:

1. **Hook/tap eviction from blocking I/O** (the systemic defect above). On
   Windows, exceeding `LowLevelHooksTimeout` makes the OS deliver the key
   anyway and remove the hook. `running()` reports **thread** liveness, not
   **hook** liveness - so the reconciler never notices and `start()`
   early-returns. **The relay is then permanently dead, silently.** macOS
   self-heals from tap disable; Windows does not.
2. **A policy bug, enshrined by my own test.** `forwarded` is a fact about the
   past but is evaluated last, so `{unmapped, forwarded}` passes the key
   locally *after* it already left the machine. Three of eight truth-table rows
   are wrong, and `KeyboardRelayHookPolicyTests.cpp:45-49` asserts one of them
   as intended. Correct rule: `if (forwarded) return false;` first.
3. **An unfixable residual.** Both OSes call the most-recently-installed
   hook/tap first, and Deskflow installs once at epoch start. Any app that
   reinstalls later permanently jumps ahead. `IOHIDManager`-class consumers sit
   upstream of the entire tap chain. **Reliable suppression is not achievable at
   this layer on either platform** - only a filter driver / HID seizure /
   DriverKit shim is deterministic.

Also found: the Windows repeat heuristic (`GetAsyncKeyState` in the hook) is
unsound and both possible readings are bugs - either holding Esc restarts the
fleet, or every keystroke leaks locally. And `GetKeyState(VK_CAPITAL)` is read
on a thread that never pumps input, so it is always 0.

## SIMPLIFICATION — 10 fork-added mechanisms to 6

`KeyState::m_syntheticKeys` already **is** "keys this side injected and has not
released". Two more copies were built on top of it. Three latent bugs follow
from that duplication:

- `m_injectedModifierVks` diverges deterministically: `updateKeyState()` memsets
  `m_syntheticKeys` without touching it, and sanitize calls `updateKeyState()`
  right after - so a VK stays marked injected **forever** and the audit skips it
  permanently. Deriving the bits from `m_syntheticKeys` makes this
  unrepresentable.
- `enter` passes `injectedModifierBits()` where its own comment says the
  invariant requires `0` (`leave` gets it right).
- `~Server` releases chord mods but not the button ledger.

Tier 0 (safe now): derive the bits; pass 0 at enter; release in `~Server`;
single-source caps. Tier 1 (after soak): merge the chord held-mods into the
modifier ledger; fold the pending-clears queue into it; collapse the two
duplicate Esc-rescue counters. **Do not delete:** the desk-thread sanitize (only
thing that works on the secure desktop), the relay ledger, `m_deferredSuper`.

## ORDER OF WORK

1. **Signing** - one identity everywhere, failure is fatal, route through the
   GUI-session helper. Unblocks permissions and auto-start. Cheapest, highest
   certainty.
2. **Blocking I/O out of every input callback** - all three instances. Fixes
   the hotkey leak, the Windows relay death, and the macOS tap disable.
3. **Caps as absolute state** - implement `enterSecondary`, use
   `IOHIDSetModifierLockState`, pair or replace the lone Down, resync on wake.
4. **Tier 0 simplification** - three latent bugs, no behaviour change.
5. **Then** decide whether the residual hotkey leak justifies a driver.

## OPEN QUESTIONS (not answered by source reading)

- Does returning `nullptr` from the session tap actually suppress the **local**
  caps toggle, or has HID already committed it upstream? Needs a runtime test.
- Is Wispr Flow using a tap or an `IOHIDManager` monitor? Determines whether
  item 2 above helps at all for that specific app.
