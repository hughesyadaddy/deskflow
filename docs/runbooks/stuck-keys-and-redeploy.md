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

### 1b. A Mac has a key held / types uppercase

```bash
ssh <mac> 'launchctl kickstart -k gui/$(id -u)/io.github.hughesyadaddy.deskflow-core'   # new build
```

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

`fleet-deploy-macos.sh` builds strict-signed, `deskflow-ctl stop` (launchd
bootout), swaps the bundle, `deskflow-ctl start` (bootstrap agents), then
deploys Mouser via its own installer (`mouser-ctl` stop → install → start).
After the identifier fix, expect **one** Accessibility/Input Monitoring
re-grant per Mac for `deskflow-core` and the login bridge.

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
# signatures + TCC + bridge
tools/fleet-health --check all --host all
# no key held anywhere (Windows) — rerun 1a and expect all down=False
```

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
| launchd agents | `~/Library/LaunchAgents/io.github.hughesyadaddy.{deskflow,deskflow-core,mouser}.plist`, `/Library/LaunchAgents/org.deskflow.vhid-bridge.plist`, `/Library/LaunchDaemons/io.github.hughesyadaddy.deskflow-prio.plist` |
| Windows service | `Deskflow` (`deskflow-daemon.exe`, LocalSystem); GUI + Mouser via HKCU Run |
| Mouser⇄Deskflow bridge | Mouser listens `127.0.0.1:19795`; token `~/Library/Application Support/Mouser/bridge.token` / `%APPDATA%\Mouser\bridge.token` |
| Logs | macOS `~/Library/Deskflow/deskflow-core.log`, `~/Library/Logs/Deskflow/`, `/var/log/deskflow-vhid-bridge.log`; Windows `C:\ProgramData\Deskflow\deskflow-daemon.log`; Mouser `~/Library/Logs/Mouser/` |
