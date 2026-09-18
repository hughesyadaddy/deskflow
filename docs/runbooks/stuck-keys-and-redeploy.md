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
| Login screen: Caps toggles wrong, letters land in the wrong case | The login bridge's caps-aware Shift logic used a legacy IOKit call that fails on macOS 26; a relayed Caps latched the Shift bit |
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

## 2. Redeploy one seat from the branch

Prerequisites on the target seat (once):

- `deskflow/.env`: macOS `DESKFLOW_CODESIGN_ID=<sha1 of "Apple Development: Duff Hughes">`
  (`security find-identity -v -p codesigning`); Windows
  `DESKFLOW_SIGN_THUMBPRINT=FBB49069A6C594E83714724217C7A5F54885FAEC`
  (the "Deskflow Fleet Code Signing" cert must stay in `LocalMachine\Root`).
- `Mouser/.env.local`: `MOUSER_SIGN_IDENTITY=<same sha1>` (macOS).
- Both branches pushed: deskflow `origin/fleet/memory-program`,
  Mouser `fork/fleet/memory-program`.
- macOS over SSH: the console user must be logged in and Terminal +
  System Events Automation granted (the build routes through the GUI session
  because the login keychain cannot be unlocked over SSH).

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
A rejected build leaves the old bundle running. Mouser follows via its own
installer. After the identifier fix, expect **one** Accessibility/Input
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

### One-time Login Items (BTM) cleanup per Mac — human step

Older builds registered Deskflow (and Mouser) as Login Items via SMAppService
on every launch, so login runs both the Login Item and the LaunchAgent and one
loser exits. The new GUI does not register itself when `DESKFLOW_LAUNCHD=1`
(set by the rendered plists), but the existing BTM records must be removed
once by hand. Check first:

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
