# Runbook: stuck modifier keys, wrong capitalization, and redeploying a seat

Seats: `macbookpro` (this Mac), `hackintosh` (macOS, usually the Deskflow
server), `tiny11` (Windows 11 VM, user `alexh`). SSH aliases match. All SSH
sessions to `tiny11` are already High-Integrity admin. Do not use inline
`powershell -Command` with quotes over SSH — it mangles them; `scp` a `.ps1`
and run it with `-File`.

## 0. Symptoms → cause

| Symptom | Cause (fixed on `fleet/memory-program`) |
|---|---|
| A machine types as if Shift/Cmd/Ctrl is held; capitals inverted | A modifier Down was relayed while the cursor was remote and its Up was delivered locally (relay checked "pass local" before its ledger); or the server tore down an epoch without releasing held keys; on Windows a lock/UAC desk switch forgot injected keys without releasing them |
| Everything types UPPERCASE on a Mac until you replug the keyboard | Deskflow's macOS shadow modifier flags desynced from the OS and were re-posted as global flags on every fake key; no sanitize existed |
| Login screen: Caps toggles wrong, letters land in the wrong case | The login bridge's caps-aware Shift logic used a legacy IOKit call that fails on macOS 26; a relayed Caps latched the Shift bit (PR3, vhid-bridge -- not this process) |
| A Mac's own keyboard capitalizes wrong right after lock/unlock/wake | Fixed (K5): OSXScreenImpl's session observers now trigger a ledger-only `releaseInjectedKeys()` + `logSecureInputState()` on resync, so a modifier held across the lock never gets swept as "stale" |
| A Mac's own keyboard capitalizes wrong *while already unlocked and typing* -- e.g. a System Settings admin prompt, a pkg installer, or a `sudo`-backed GUI dialog pops up mid-session | K6 (this fix): closes the architectural gap -- the boundary sweep (`Screen::enable()` and its delayed timer) only ever runs while NOT entered, so a mid-session secure-input transition previously got no protection and no log line at all. `OSXScreen::secureInputPollTick()` (armed in `enable()`, ~1 Hz, cancelled in `disable()`) now logs every transition unconditionally and runs the same ledger-only release K5 uses at boundaries on dialog-dismiss. **Wrong-case capitalization actually happening inside a live SecurityAgent dialog remains UNCONFIRMED** -- no repro exists (would need a real admin password) -- only the gap and its observability are fixed; grep `[keys] secure-input=on/off` in the log if this is ever reported live |
| Win key sticks on a Windows seat while PowerToys Keyboard Manager / AutoHotkey Win hooks are active (Start menu flaps, `released stuck modifier vk=0x5b` every second, `injected bits 0x03` with nothing held) | Those hooks re-process Deskflow's injected Win events (proven by PowerToys' own logs 2026-09-25) -- outside our code. Deskflow's server-side `chordRemap` already provides the same Win-chord remaps, so those layers are redundant on a Deskflow-driven seat: **disable them** (PowerToys KBM toggle; the AHK DisableWinStart script). K7 also fixes what made it worse: D1 the client ledger now forgets every VK a release was attempted for (phantom entries used to vouch forever and hide a physically stuck Win from the audit); D2 the ledger is mutex-guarded (it was raced between the desk thread and the main thread); D3 the entered audit releases a non-ledgered Win only after two consecutive sightings and 2 s without a key-down, instead of fighting another injector every second; D4 a dropped injected UP is logged (`[keys] injected UP dropped`); D5 the server strips Super from keys sent during a chord-consumed Super hold and no longer relays a Super UP whose DOWN was never sent; D6 every audit release logs `[keys] audit release vk=... os_after=... desk=...` so an ineffective release (wrong desktop) is distinguishable from a re-press |
| Login screen mouse far too fast | Bridge multiplied by 8 assuming pointer acceleration was off; the accel-disable call was failing; now self-calibrates |
| Two `deskflow-core` / two `Mouser` on one machine | Per-user shm lock, unconditional `removeServer()` race, installers `pkill` + `open`, watchdog respawn 8 ms after terminate; now kernel locks + launchd/service ownership + `deskflow-ctl`/`mouser-ctl` |

## 1. Immediate relief (no rebuild)

### 1a. Windows seat (`tiny11`) has a key held

Run `scripts/release-modifiers.ps1` **inside the console session** (session 1),
not in the SSH session (session 0) — key state is per session.

```bash
scp scripts/release-modifiers.ps1 scripts/run-in-session.ps1 tiny11:'C:/Users/alexh/'
ssh tiny11 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\alexh\run-in-session.ps1 -Script C:\Users\alexh\release-modifiers.ps1'
ssh tiny11 'type C:\Users\alexh\AppData\Local\Temp\release-modifiers.log'
```

Expected: one line per modifier with `down=False`; any `down=True` is followed
by `released -> down=False`. `CAPSLOCK toggled=` is reported, not changed.

Then restart the core so its ledger starts clean:

```bash
ssh tiny11 'powershell -NoProfile -Command "Restart-Service Deskflow -Force"'
ssh tiny11 'powershell -NoProfile -Command "Get-Process deskflow* | Select Name,Id,SessionId,StartTime"'
```

Expected: exactly one `deskflow-daemon` (session 0) and one `deskflow-core`
(session 1) with a fresh StartTime. **On the old build the core survives the
service restart** — if `deskflow-core` still shows the old StartTime,
`Stop-Process -Id <pid> -Force`; the watchdog respawns it within 10 s. The new
build's Job object makes this automatic.

### 1b. A Mac has a key held / types uppercase / menubar icon missing

```bash
ssh <mac> '~/Desktop/deskflow/scripts/deskflow-ctl restart'      # kickstart -k core + GUI, clears quit-intent
ssh <mac> '~/Desktop/deskflow/scripts/deskflow-ctl converge'     # report only: what the self-heal tick would do
ssh <mac> '~/Desktop/deskflow/scripts/deskflow-ctl converge --apply'
```

`converge` is what `gui/$UID/io.github.hughesyadaddy.deskflow-converge`
runs every 60 s: it re-renders stale plists, `launchctl enable`s and
bootstraps unloaded agents, kickstarts loaded-but-dead ones, and never kills
anything. It stays quiet while any of these hold:

- `~/Library/Application Support/Deskflow/quit-intent` is newer than the last
  boot (written by tray Quit and `deskflow-ctl stop`; removed by
  `start`/`restart`; a pre-boot sentinel is discarded). A deliberate quit
  therefore sticks until re-login or `deskflow-ctl start`.
- `~/Library/Deskflow/deploy.lock` is younger than 30 min (`install-macos.sh`).
- 3 start actions already happened in the last hour (report-only after that;
  `health.json` says `action budget exhausted`, and `converge --apply` exits 1).

State: `~/Library/Application Support/Deskflow/health.json` (counts, plan,
actions, assert-single) and `~/Library/Logs/Deskflow/converge.log`. A toast
"Deskflow: converge repaired ..." means it acted.

Old build (core owned by the GUI): quit Deskflow from its menu, then reopen
it; if uppercase persists, the shadow flags are wedged in the HID system —
unplug/replug the keyboard (or sleep/wake). The new build clears this on
lock/unlock/wake and at core start.

### 1c. Login window cursor/caps wrong

```bash
ssh <mac> 'sudo launchctl kickstart -k loginwindow/org.deskflow.vhid-bridge'
```

### 1e. Keyboard rescue from any keyboard: 5×Esc restarts, 10×Esc stops everything

Tap plain **Esc** (no Shift/Ctrl/Alt/Cmd; Caps/Num state is ignored) in a
burst, each tap within 700 ms of the previous. The burst is decided **only
once it has ended** (700 ms without another Esc; the same 700 ms is the join
gap) -- nothing fires on the 5th press, so heading for 10 never restarts you
on the way:

| Taps in the burst | Effect on **every seat** (mesh broadcast; the seat that saw the taps included) |
|---|---|
| 1–4 | nothing |
| **5–9** | `fleet keyboard rescue`: every seat restarts its local core (today's behaviour; GUI `restartCore` / Windows daemon relaunch / launchd) |
| **10+** | **stop-all**: every seat logs `WARNING: [rescue] 10x Esc: stopping ALL Deskflow instances and services on <seat>` and then stops every Deskflow instance and service it owns (Mouser untouched) |

From the 5th tap on, the Escs stay off the wire (a rescue in progress is not
typed into the remote app). A non-Esc key abandons the burst.

The taps are counted **off the core event loop** in every role (macOS:
the coordinator's listen-only event tap; Windows: its Raw Input sink) --
a seat whose event loop is wedged is exactly the case this exists for. On a
10×Esc the stop-all executor never touches that loop; on a 5×Esc the
restart request goes out at once and the core hard-exits non-zero
(`[rescue] ... exiting with 1`) if its loop has not acknowledged within 3 s,
so launchd KeepAlive / the Windows service relaunch it. Plain
`deskflow-core --server` (no coordinator) keeps only the on-loop counter.

Trust: without a `[coordination] token`, `rescue`/`stopall` are accepted
only from an address a configured peer resolves to (its LAN name and
Tailscale FQDN entries, IPv4+IPv6, re-resolved every 5 min or after a
miss); anything else is dropped with one `coordination: dropping fleet
stop-all from <addr>` WARN. A shared token is stronger and, when set, is
still required.

What stop-all does per seat:

- **macOS**: writes `~/Library/Application Support/Deskflow/quit-intent`
  (the same sentinel `deskflow-ctl stop` writes, so converge/KeepAlive leave
  everything down), then runs the launchd-safe `~/Library/Deskflow/bin/deskflow-ctl stop`
  when present, else in-process: `launchctl bootout gui/$UID/` converge →
  GUI → SIGTERM/SIGKILL any stray bundle `Deskflow`/`deskflow-core` (this
  user, by executable path) → core (itself) last. The login-window bridge
  (`loginwindow/org.deskflow.vhid-bridge`, root) keeps running -- it is out
  of reach without root, and it is what still gives you a cursor there.
- **Windows** (`tiny11`): the core sends the `Deskflow` service `stopAll`;
  the daemon terminates every `deskflow-core.exe` and `deskflow.exe` under
  the install root in **every session** (the SYSTEM login-screen core too),
  then stops itself cleanly. It reports `SERVICE_STOPPED` with
  `dwWin32ExitCode = NO_ERROR`, and the SCM recovery actions
  `deskflow-ctl.ps1 start` configures (`sc failure … restart/1000/5000/30000`
  + `failureflag 1`) fire only on a crash or a non-zero exit code -- so the
  service stays stopped. If the core cannot reach the daemon it kills the
  GUI under the install root and exits itself.
- **Mouser** is a separate app and is never touched. (A future
  `[coordination] rescueStopsMouser=false` may opt it in; not implemented.)
- A seat that is already stopping ignores repeats; a peer that predates the
  `stopall` mesh message ignores it (no mesh version bump).

Bring it back:

```bash
ssh <mac> '~/Desktop/deskflow/scripts/deskflow-ctl start'          # clears quit-intent, bootstraps core → GUI → converge
ssh tiny11 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\alexh\Desktop\deskflow\scripts\deskflow-ctl.ps1 start'
```

Log lines to grep: `keyboard rescue: 5x Esc burst ended` /
`10x Esc burst ended` on the seat that saw the taps,
`coordination: fleet keyboard rescue received` / `fleet stop-all received`
on the others, `[rescue]` for every stop-all step.

## 2. Redeploy one seat from the branch

Prerequisites on the target seat (once):

- `deskflow/.env`: macOS `DESKFLOW_CODESIGN_ID=<sha1 of "Apple Development: Duff Hughes">`
  (`security find-identity -v -p codesigning`); Windows
  `DESKFLOW_SIGN_THUMBPRINT=FBB49069A6C594E83714724217C7A5F54885FAEC`
  (the "Deskflow Fleet Code Signing" cert must stay in `LocalMachine\Root`).
- `Mouser/.env.local`: `MOUSER_SIGN_IDENTITY=<same sha1>` (macOS).
- Both branches pushed: deskflow `origin/fleet/memory-program`,
  Mouser `fork/fleet/memory-program`.
- macOS over SSH, one of (see `docs/building-signed.md` → "Signing over SSH"):
  - **preferred:** `deskflow/.env` also holds `DESKFLOW_KEYCHAIN_PASSWORD=<login
    password>` and the file is `chmod 600` — signing then runs unattended in
    the SSH session and nothing is routed through the GUI; or
  - **fallback:** the console user is logged in and Terminal + System Events
    Automation are granted, so `tools/fleet-gui-exec.py` can relay the
    keychain steps into the console session.

### Windows (`tiny11`)

```bash
scp scripts/deploy-seat-windows.ps1 tiny11:'C:/Users/alexh/deploy-tiny11.ps1'
ssh tiny11 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\alexh\deploy-tiny11.ps1'
ssh tiny11 'type C:\Users\alexh\fleet-deploy-tiny11.log'
```

The script: fills the thumbprint if empty → `git fetch/checkout/pull` both
repos → `scripts/fleet-deploy-windows.ps1` (build → sign every
`.exe/.dll/.pyd` → stop service and all Deskflow processes → install → start
service → GUI + Mouser into the console session via a scheduled task →
`deskflow-ctl.ps1 assert-single`). Build failure aborts **before** anything is
uninstalled. Last line must be `== DEPLOY OK`.

### macOS (`hackintosh` or this Mac)

```bash
ssh hackintosh 'cd ~/Desktop/deskflow && git fetch origin && git checkout fleet/memory-program && git pull --ff-only origin fleet/memory-program && FLEET_SKIP_GIT_PULL=1 bash scripts/fleet-deploy-macos.sh'
```

`fleet-deploy-macos.sh` builds strict-signed, then `install-macos.sh`
stages the bundle and **verifies its signature first**; only then
`deskflow-ctl stop` (launchd bootout, writes quit-intent), swap, and
`deskflow-ctl start` (clears quit-intent, bootstraps core → GUI → converge).
A rejected build leaves the old bundle running. If the install dies *after*
the stop (copy failure, killed mid-swap) the seat is left stopped with
`quit-intent` set and the old bundle restored from `.bak` where possible:
run `deskflow-ctl start` by hand — converge will not restart a stopped seat
on its own. Mouser follows via its own installer. After the identifier fix, expect **one** Accessibility/Input
Monitoring re-grant per Mac for `deskflow-core` and the login bridge.

The deploy ends with two lines that may need a human with root:

```text
== deskflow-ctl: prio: system/io.github.hughesyadaddy.deskflow-prio needs root; run once as admin: ==
== [<seat>] bridge plist stale — run root step: sudo env DESKFLOW_INSTALL_APP=/Applications/Deskflow.app bash ~/Desktop/deskflow/scripts/install-login-bridge-macos.sh ==
```

Root steps (over ssh, once per seat; nothing in the deploy escalates):

```bash
# prio LaunchDaemon + /private/var/db/deskflow (1777, machine-scope lock dir) — copy the printed sudo lines, or:
ssh <mac> 'sudo ~/Desktop/deskflow/scripts/deskflow-ctl prio'
# LoginWindow bridge plist (hosts from Deskflow.conf peers: ip/lan fields only), log 0600, legacy launchers retired
ssh <mac> 'sudo env DESKFLOW_INSTALL_APP=/Applications/Deskflow.app bash ~/Desktop/deskflow/scripts/install-login-bridge-macos.sh'
# preview without installing
ssh <mac> 'bash ~/Desktop/deskflow/scripts/install-login-bridge-macos.sh --dry-run | plutil -p -'
```

The bridge agent only loads at the next login window (log out or reboot).

### One-time Login Items (BTM) cleanup per Mac — human step, FIRST

Older builds registered Deskflow (and Mouser) as Login Items via SMAppService
on every launch, so login runs both the Login Item and the LaunchAgent and one
loser exits. The new GUI build (A1: honours `DESKFLOW_LAUNCHD=1` from the
rendered plists, never registers a Login Item, and when launchd-owned takes
over from an unmanaged instance instead of exiting 5; tray Quit writes
`quit-intent`) and the GUI plist change (`KeepAlive` on crash) **must ship
together**: the plist alone would relaunch an old GUI that exits on the
single-instance lock every 30 s. Order per seat: (1) this BTM cleanup,
(2) deploy the branch with the new GUI, (3) `deskflow-ctl start`. Check first:

```bash
ssh <mac> 'sfltool dumpbtm | grep -ci deskflow; sfltool dumpbtm | grep -ci mouser'   # want 0 and 0
```

Either, per Mac:

1. **System Settings → General → Login Items & Extensions**: remove every
   Deskflow / Mouser entry under "Open at Login" and toggle off any under
   "Allow in the Background" that points at an old bundle id
   (`org.deskflow.deskflow`, `io.github.tombadash.mouser`). Precise, no reboot.
2. **`sudo sfltool resetbtm`** then reboot: wipes *all* Login Items on the
   Mac (every app, not just ours) and re-prompts them. Use only when the pane
   is wedged or over ssh with no console access.

Then `deskflow-ctl start` (or wait for converge) and re-check the count.

### From any seat, whole fleet (once every seat has `scripts/fleet.env`)

```bash
scripts/fleet-deploy.sh --dry-run --json -      # plan: 3 hosts, server last
scripts/fleet-deploy.sh                          # deploy
scripts/fleet-deploy.sh --self-test --json out.json && jq .ok out.json
tools/fleet-health --check all --host all
```

### What the deploy gates on (2026-09-22 build)

Per macOS seat, in this order, all fatal unless noted: strict-signed build →
`install-macos.sh` verifies **every Mach-O** (no ad-hoc, `TeamIdentifier`
J5KPG8ZR5C, hardened runtime on `Contents/MacOS/*`, PlugIns/Resources
included) → swap → `deskflow-ctl start` → Mouser: three sha256 checkpoints of
`~/Library/Application Support/Mouser/{config.json,last_device.json}`
(before install / after install / 30 s after restart; a change is allowed only
when `version` increased and the old keys are a subset) with
`config.json.pre-deploy-<ts>` kept ×5 → the Mouser log must show
`CGEventTap created (native tap:` within 60 s and never `enabled on its own run
loop` (Python-tap fallback = the leaking path; deploy fails) → **last**:
`deskflow-ctl retire` + `deskflow-ctl assert-single`; a failure here prints a
`#### HUMAN STEP REQUIRED on <seat>` block (Login Items to remove, root-owned
leftovers, bridge reinstall) and exits non-zero **after** everything is
installed. The controller's report has `apple|adhoc|hard|settings` columns
and `ALL_OK=0` on any ad-hoc, non-hardened, or settings mismatch.

## 3. Verify after deploy

```bash
# exactly one of each, right session
ssh tiny11 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\alexh\Desktop\deskflow\scripts\deskflow-ctl.ps1 assert-single'
ssh <mac> '~/Desktop/deskflow/scripts/deskflow-ctl assert-single'
ssh <mac> '~/Desktop/deskflow/scripts/deskflow-ctl converge'        # plan must be empty, assert-single OK
ssh <mac> 'launchctl print gui/$(id -u)/io.github.hughesyadaddy.deskflow-converge | grep -E "state|last exit"'
# signatures + TCC + Mouser bridge + LoginWindow bridge
tools/fleet-health --check all --host all
tools/fleet-health --check loginbridge --host <mac>   # plist lints, program = installed bundle, log 600, 0 keystrokes;
                                                      # agent pid needs passwordless sudo, else SKIP with reason
# memory: no growth over 24 h (Mouser ≤200 MB with the window closed, ≤350 open,
# ≤0.1 MB/h; leak-class counts flat). Install the sampler once per Mac seat
# (user agent; recipe in tools/launchd/com.fleet.soak.plist; --heap-classes
# needs the sudoers line from docs/runbooks/mouser-heap-classes-soak.md):
ssh <mac> 'cd ~/Desktop/deskflow && sed -e "s#__REPO__#$PWD#g" -e "s#__HOME__#$HOME#g" tools/launchd/com.fleet.soak.plist > ~/Library/LaunchAgents/com.fleet.soak.plist && launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.fleet.soak.plist; launchctl kickstart gui/$(id -u)/com.fleet.soak'
# after ≥24 h (72 h for the full contract), on each Mac:
ssh <mac> 'cd ~/Desktop/deskflow && tools/fleet-soak report --in harness/soak/latest/$(hostname -s | tr A-Z a-z)-mouser.jsonl --proc mouser --window 24 --min-hours 24 --slope-max 0.1 --cap 200 --class-slope-max 10'
ssh <mac> 'grep "\[mem\]" ~/Library/Logs/Mouser/mouser.log | tail -3'   # growth_mb_h and passthrough_guard_skipped
# capitalization + stuck keys
ssh <mac> 'grep -c "stuck-release" ~/Library/Deskflow/deskflow-core.log'           # want 0
ssh <mac> 'sudo -n grep "\[keys\] session" /var/log/deskflow-vhid-bridge.log | tail -3'  # after one login-window use
# Windows service recovery
ssh tiny11 'sc.exe qfailure Deskflow'                 # RESTART actions 1000/5000/30000 ms
ssh tiny11 'powershell -NoProfile -Command "Test-Path C:\ProgramData\Deskflow"'
# no key held anywhere (Windows) — rerun 1a and expect all down=False
```

Kill test on a Mac: `kill -9 $(pgrep -x Deskflow)` → tray back within ~35 s
(launchd KeepAlive, throttle 30 s); tray Quit → stays quit (quit-intent) until
`deskflow-ctl start` or re-login.

Type `aBcD` on each seat and across a screen switch; hold Shift while crossing
and release on the other side; toggle Caps on one seat and type on another.
Any wrong case → capture `~/Library/Deskflow/deskflow-core.log` (macOS) /
`C:\ProgramData\Deskflow\deskflow-daemon.log` (Windows): key lines now log at
DEBUG as `id=65 ('A')`.

## 4. Rollback

```bash
scripts/fleet-deploy.sh --rollback --host <seat> --app deskflow   # uses tools/state/last-good.json
```

or manually: `git checkout <last good sha>` in the seat's checkout and rerun
the seat deploy above.

## 5. Where things are

| Thing | Path |
|---|---|
| Core/GUI/bridge locks (macOS) | `~/Library/Application Support/Deskflow/*.lock`, `/private/var/db/deskflow/*.machine.lock` |
| Mouser lock | `~/Library/Application Support/Mouser/mouser.lock`; Windows mutex `Local\MouserSingleInstance` |
| launchd agents | `~/Library/LaunchAgents/io.github.hughesyadaddy.{deskflow,deskflow-core,deskflow-converge,mouser}.plist`, `/Library/LaunchAgents/org.deskflow.vhid-bridge.plist`, `/Library/LaunchDaemons/io.github.hughesyadaddy.deskflow-prio.plist` |
| converge state | `~/Library/Application Support/Deskflow/{health.json,quit-intent,converge-actions}`, `~/Library/Deskflow/deploy.lock` (install in progress), `~/Library/Logs/Deskflow/converge.log` |
| Bridge plist generator | `scripts/install-login-bridge-macos.sh` (`--dry-run` prints the plist; run under `sudo` over ssh; the GUI calls the same script) |
| Windows service | `Deskflow` (`deskflow-daemon.exe`, LocalSystem); GUI + Mouser via HKCU Run |
| Mouser⇄Deskflow bridge | Mouser listens `127.0.0.1:19795`; token `~/Library/Application Support/Mouser/bridge.token` / `%APPDATA%\Mouser\bridge.token` |
| Logs | macOS `~/Library/Deskflow/deskflow-core.log`, `~/Library/Logs/Deskflow/`, `/var/log/deskflow-vhid-bridge.log` (root, 0600); Windows `C:\ProgramData\Deskflow\deskflow-daemon.log` (dir created by `deskflow-ctl.ps1 start`); Mouser `~/Library/Logs/Mouser/` |
